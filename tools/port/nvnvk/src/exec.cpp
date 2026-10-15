// SPDX-License-Identifier: MIT
#include "exec.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "formats.h"
#include "hostmem.h"
#include "renderdoc_app.h"
#include "objects.h"

namespace nvnvk {
extern thread_local bool t_win_draw;  // pipeline.cpp
extern std::atomic<uint64_t> g_win_src;


Executor g_exec;

namespace {
template <class T>
const T& As(const uint32_t* p) {
    return *reinterpret_cast<const T*>(p);
}
}  // namespace

void Executor::Submit(pla_ctx* ctx, const uint64_t* handles, uint32_t n) {
    if (!g_cfg.execute) return;  // diagnostics: record (and capture) but execute nothing on Vulkan
    if (!g_cfg.exclusive && g_cfg.start_after && g_stats.frames.load(std::memory_order_relaxed) < g_cfg.start_after)
        return;  // shadow: do not execute until suyu has presented N frames
    if (!g_vk.dev) return;  // the device does not exist yet (created on the next Present)
    std::vector<RecordingPtr> recs;
    recs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) recs.push_back(FindRecording(handles[i]));
    if (async_) {
        Job j;
        j.recs = std::move(recs);
        Enqueue(std::move(j));
        return;
    }
    SubmitNow(ctx, recs);
}

std::atomic<uint64_t> g_counter_types[32];
std::atomic<uint64_t> g_phase_copy_ns{0}, g_phase_flush_ns{0};  // submit phases (hitch diagnostics)
std::atomic<uint64_t> g_fake_counter[32];  // value of the simulated counters (counter_value_mask)

uint64_t Executor::PlannedValue() const { return async_ ? planned_.load() : g_vk.timeline_next.load() - 1; }

void Executor::Enqueue(Job&& j) {
    std::unique_lock<std::mutex> lk(qmu_);
    // Backpressure: at most 3 pending jobs (bounded latency).
    const auto t0 = std::chrono::steady_clock::now();
    qspace_.wait(lk, [&] { return jobs_.size() < 3 || quit_; });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_game_wait_ns[3].fetch_add((uint64_t)(ms * 1e6), std::memory_order_relaxed);
    if (ms > 30) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 200) NVK_INFO("game wait: submit queue full %.1f ms", ms);
    }
    planned_.fetch_add(1);  // each job does exactly one g_vk.Submit
    jobs_.push_back(std::move(j));
    qcv_.notify_one();
}

void Executor::Worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(qmu_);
            qcv_.wait(lk, [&] { return !jobs_.empty() || quit_; });
            if (jobs_.empty()) return;
            j = std::move(jobs_.front());
            jobs_.pop_front();
        }
        qspace_.notify_one();
        if (j.present)
            PresentNow(nullptr, j.win);
        else
            SubmitNow(nullptr, j.recs);
    }
}

void Executor::StartAsync() {
    planned_ = g_vk.timeline_next.load() - 1;
    async_ = true;
    worker_ = std::thread([this] {
        SetThreadDescription(GetCurrentThread(), L"nvnvk submit");
        Worker();
    });
    NVK_INFO("async submit: dedicated render thread");
}

void Executor::StopAsync() {
    if (!async_) return;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        quit_ = true;
    }
    qcv_.notify_all();
    qspace_.notify_all();
    if (worker_.joinable()) worker_.join();
    async_ = false;
}

void Executor::SubmitNow(pla_ctx* ctx, const std::vector<RecordingPtr>& recs) {
    const auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_vk.submit_mu);
    ctx_ = ctx;
    FrameSlot& f = g_vk.Begin();
    g_hostmem.CollectDead(f.dead_buffers);
    g_hostmem.CollectDeadSub(f.dead_subbufs);
    g_hostmem.ProcessPending();
    cmd_ = f.cmd;
    g_hostmem.InitVram(cmd_);
    chunk_draws_ = 0;
    // Dependency on previous submits (a render target from the previous submit that this one samples, a
    // transform feedback buffer this one reads...): there are no implicit barriers between command buffers.
    Barrier(0);
    if ((++evict_tick_ & 127) == 0) g_vk.EvictGpuBuffers();
    SyncGpuBuffers(cmd_);
    {
        ScopedPhase _ph(g_phase_flush_ns);
        g_tex.FlushPending(cmd_);
    }
    PipelineBeginSubmit();
    PipelineSnapPromote(cmd_);
    st_.rt_dirty = true;
    for (const RecordingPtr& r : recs)
        if (r) Run(*r, 0);
    EndRendering();
    QueueWriteback(cmd_);
    g_vk.Submit();
    g_stats.submits.fetch_add(1, std::memory_order_relaxed);
    ctx_ = nullptr;
    g_stats.submit_ns.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
}

void Executor::Run(const Recording& r, int depth) {
    const uint32_t* w = r.words.data();
    const uint32_t* end = w + r.words.size();
    while (w < end) {
        PacketHeader h;
        std::memcpy(&h, w, 4);
        const uint32_t* p = w + 1;
        w = p + h.words;
        if (w > end) break;
        switch (h.op) {
            case Op::BindBlend: {
                const uint32_t* s = p;
                const uint32_t target = s[0] & 7;
                st_.blend[target][0] = s[0];
                st_.blend[target][1] = s[1];
                break;
            }
            case Op::BindColor: st_.color = p[0]; break;
            case Op::BindChannelMask: st_.channel_mask = p[0]; break;
            case Op::BindDepthStencil: st_.depth_stencil[0] = p[0]; st_.depth_stencil[1] = p[1]; break;
            case Op::BindMultisample: std::memcpy(st_.multisample, p, 24); break;
            case Op::BindPolygon: st_.polygon = p[0]; break;
            case Op::BindVertexAttrib: {
                const auto& v = As<PVertexAttrib>(p);
                st_.num_vattrib = v.count;
                std::memset(st_.vattrib, 0, sizeof st_.vattrib);
                std::memcpy(st_.vattrib, v.attr, v.count * 4);
                break;
            }
            case Op::BindVertexStream: {
                const auto& v = As<PVertexStream>(p);
                st_.num_vstream = v.count;
                std::memset(st_.vstream, 0, sizeof st_.vstream);
                std::memcpy(st_.vstream, v.stream, v.count * 8);
                break;
            }
            case Op::BindVertexBuffer: {
                const auto& v = As<PVertexBuffer>(p);
                if (v.index < 16) st_.vbuf[v.index] = {v.addr, v.size};
                break;
            }
            case Op::BindProgram: {
                const auto& v = As<PProgram>(p);
                st_.program = v.program;
                st_.program_stages = v.stages;
                break;
            }
            case Op::BindUniformBuffer: {
                const auto& v = As<PBuffer>(p);
                if (v.stage < 6 && v.index < 18) st_.ubo[v.stage][v.index] = {v.addr, v.size};
                break;
            }
            case Op::BindStorageBuffer: {
                const auto& v = As<PBuffer>(p);
                if (v.stage < 6 && v.index < 16) st_.ssbo[v.stage][v.index] = {v.addr, v.size};
                break;
            }
            case Op::BindTexture: {
                const auto& v = As<PTexture>(p);
                if (v.stage < 6 && v.index < 32) st_.tex[v.stage][v.index] = v.handle;
                break;
            }
            case Op::BindImage: {
                const auto& v = As<PTexture>(p);
                if (v.stage < 6 && v.index < 8) st_.img[v.stage][v.index] = v.handle;
                break;
            }
            case Op::SetRenderTargets: {
                const auto& v = As<PRenderTargets>(p);
                bool same = v.count == st_.num_rt && !std::memcmp(&v.depth, &st_.rt_depth, sizeof(TexDesc));
                for (uint32_t i = 0; same && i < v.count; ++i)
                    same = !std::memcmp(&v.color[i], &st_.rt_color[i], sizeof(TexDesc));
                if (same) break;  // same targets: the render pass is not closed
                EndRendering();
                Barrier(0xFFFFFFFF);
                st_.num_rt = v.count;
                for (uint32_t i = 0; i < 8; ++i) st_.rt_color[i] = i < v.count ? v.color[i] : TexDesc{};
                st_.rt_depth = v.depth;
                st_.rt_dirty = true;
                break;
            }
            case Op::SetViewports: {
                const auto& v = As<PRects>(p);
                for (uint32_t i = 0; i < v.count && v.first + i < 16; ++i)
                    std::memcpy(st_.viewport[v.first + i], &v.v[i * 4], 16);
                break;
            }
            case Op::SetScissors: {
                const auto& v = As<PRects>(p);
                for (uint32_t i = 0; i < v.count && v.first + i < 16; ++i)
                    std::memcpy(st_.scissor[v.first + i], &v.v[i * 4], 16);
                break;
            }
            case Op::SetDepthRanges: {
                const auto& v = As<PRects>(p);
                for (uint32_t i = 0; i < v.count && v.first + i < 16; ++i)
                    std::memcpy(st_.depth_range[v.first + i], &v.v[i * 2], 8);
                break;
            }
            case Op::SetStencilRef:
            case Op::SetStencilMask:
            case Op::SetStencilValueMask: {
                const auto& v = As<PFaceValue>(p);
                uint32_t* dst = h.op == Op::SetStencilRef ? st_.stencil_ref
                                : h.op == Op::SetStencilMask ? st_.stencil_mask : st_.stencil_vmask;
                if (v.faces & 1) dst[0] = v.value;
                if (v.faces & 2) dst[1] = v.value;
                break;
            }
            case Op::SetBlendColor: std::memcpy(st_.blend_color, p, 16); break;
            case Op::SetAlphaRef: std::memcpy(&st_.alpha_ref, p, 4); break;
            case Op::SetDepthBounds: {
                const auto& v = As<PDepthBounds>(p);
                st_.depth_bounds_enable = v.enable;
                st_.depth_bounds[0] = v.mn;
                st_.depth_bounds[1] = v.mx;
                break;
            }
            case Op::SetPolygonOffsetClamp: std::memcpy(st_.poly_offset, p, 12); break;
            case Op::SetLineWidth: std::memcpy(&st_.line_width, p, 4); break;
            case Op::SetPointSize: std::memcpy(&st_.point_size, p, 4); break;
            case Op::SetDepthClamp: st_.depth_clamp = p[0]; break;
            case Op::SetRasterizerDiscard: st_.raster_discard = p[0]; break;
            case Op::SetSampleMask: st_.sample_mask = p[0]; break;
            case Op::SetTexturePool: {
                const auto& v = As<PPool>(p);
                st_.tex_pool_cpu = v.cpu;
                st_.tex_pool_count = v.count;
                break;
            }
            case Op::SetSamplerPool: {
                const auto& v = As<PPool>(p);
                st_.smp_pool_cpu = v.cpu;
                st_.smp_pool_count = v.count;
                break;
            }
            case Op::DrawArrays: Draw(&As<PDrawArrays>(p), nullptr); break;
            case Op::DrawElements: Draw(nullptr, &As<PDrawElements>(p)); break;
            case Op::DrawTransformFeedback: {
                const auto& c = As<PCounter>(p);
                const PDrawArrays da{c.type, 0, 0, 0, 1};
                t_xfb_draw_ctrl = c.addr;
                Draw(&da, nullptr);
                t_xfb_draw_ctrl = ~0ull;
                break;
            }
            case Op::DispatchCompute: {
                const auto& d = As<PDispatch>(p);
                EndRendering();
                g_tex.FlushPending(cmd_);
                if (EmitDispatch(ctx_, cmd_, st_, d.x, d.y, d.z))
                    g_stats.dispatches.fetch_add(1, std::memory_order_relaxed);
                else
                    g_stats.draws_skipped.fetch_add(1, std::memory_order_relaxed);
                if (PipelineUnified() && g_cfg.unified_barriers) Barrier(0);  // compute SSBOs -> subsequent reads
                break;
            }
            case Op::ClearColor: ClearColor(As<PClearColor>(p)); break;
            case Op::ClearDepthStencil: ClearDepthStencil(As<PClearDepthStencil>(p)); break;
            case Op::ClearTexture:
            case Op::CopyBufferToBuffer:
            case Op::CopyBufferToTexture:
            case Op::CopyTextureToBuffer:
            case Op::CopyTextureToTexture:
            case Op::Downsample: Copy(h.op, p); break;
            case Op::Barrier: EndRendering(); Barrier(p[0]); break;
            case Op::ReportCounter: ReportCounter(As<PCounter>(p)); break;
            case Op::ResetCounter: {
                const auto& rc = As<PCounter>(p);
                g_fake_counter[std::min<uint32_t>(rc.type, 31)].store(0, std::memory_order_relaxed);
                if (rc.type == 1 && g_cfg.occlusion == 2 && g_cfg.exclusive) {
                    OcclEnd();
                    FrameSlot& f = g_vk.frames[g_vk.cur];
                    f.occl_events.push_back({0, f.occl_used});
                }
                break;
            }
            case Op::BindTransformFeedbackBuffer: {
                const auto& v = As<PBuffer>(p);
                if (v.index < 4) {
                    if (g_cfg.xfb_rebind_reset && (st_.xfb_buf[v.index].addr != v.addr || st_.xfb_buf[v.index].size != v.size))
                        st_.xfb_resume_mask &= ~(1u << v.index);
                    st_.xfb_buf[v.index] = {v.addr, v.size};
                }
                break;
            }
            case Op::BeginTransformFeedback: {
                // In the game's NVN driver (sdk+0x4845a0) Begin only calls a macro with the control address:
                // it starts from 0. Resuming is nvnCommandBufferResumeTransformFeedback (separate; the game does not use
                // it); End/Pause write the byte counters to ctrl+4..+0x13. Previously we resumed if ctrl+4.. was not 0 in
                // game memory, which nvnvk never writes: with reused memory (a new model, e.g. the mount on landing)
                // skinning was written right after, outside the mesh -> frozen pose or garbage.
                st_.xfb_active = true;
                st_.xfb_ctrl = As<PCounter>(p).addr;
                uint32_t ctr[4] = {};
                if (const uint64_t cpu = st_.xfb_ctrl ? g_reg.GpuToCpu(st_.xfb_ctrl + 4) : 0) g_guest.Read(ctx_, cpu, ctr, 16);
                const bool nonzero = (ctr[0] | ctr[1] | ctr[2] | ctr[3]) != 0;
                if (nonzero) {
                    static std::atomic<uint64_t> n{0};
                    const uint64_t k = n.fetch_add(1);
                    if (k < 20 || (k % 10000) == 0)
                        NVK_INFO("BeginTransformFeedback with non-zero counters at control %llx (%u %u %u %u; %llu times)%s",
                                 (unsigned long long)st_.xfb_ctrl, ctr[0], ctr[1], ctr[2], ctr[3], (unsigned long long)k + 1,
                                 g_cfg.xfb_begin_resume ? ": resuming" : ": starting from 0");
                }
                st_.xfb_resume = g_cfg.xfb_begin_resume && nonzero;
                st_.xfb_resume_mask = st_.xfb_resume ? 0xF : 0;
                break;
            }
            case Op::EndTransformFeedback:
                st_.xfb_active = false;
                // Unified memory: what XFB wrote is later read by draws as vertices/UBO/SSBO from the same memory.
                // Conservative barrier in case the game does not issue one (on Maxwell the ordering may be implicit).
                if (PipelineUnified() && g_cfg.unified_barriers) {
                    EndRendering();
                    Barrier(0);
                }
                break;
            case Op::CallCommands: {
                const uint32_t count = p[0];
                const uint64_t* hs = reinterpret_cast<const uint64_t*>(p + 2);
                if (depth < 8)
                    for (uint32_t i = 0; i < count; ++i) {
                        RecordingPtr sub = FindRecording(hs[i]);
                        if (sub) {
                            Run(*sub, depth + 1);
                        } else {
                            static std::atomic<uint64_t> miss{0};
                            const uint64_t n = miss.fetch_add(1);
                            if (n < 20 || (n % 10000) == 0)
                                NVK_WARN("CallCommands: handle %llx has no recording (%llu times)", (unsigned long long)hs[i],
                                         (unsigned long long)n + 1);
                        }
                    }
                break;
            }
            default: break;
        }
    }
}

void Executor::ReportCounter(const PCounter& c) {
    // In shadow mode the emulated GPU writes them. In exclusive mode: {u64 value, u64 timestamp} with the
    // Tegra GPU clock (guest ticks at 19.2 MHz x 32 = 614.4 MHz), consistent with
    // nvnDeviceGetTimestampInNanoseconds. Statistics counter values stay at 0.
    if (!g_cfg.exclusive || !c.addr) return;
    g_counter_types[std::min<uint32_t>(c.type, 31)].fetch_add(1, std::memory_order_relaxed);
    const uint64_t cpu = g_reg.GpuToCpu(c.addr);
    if (!cpu) return;
    uint64_t ticks = 0;
    if (PLA_API_HAS(g_guest.api, guest_ticks) && g_guest.api->guest_ticks) ticks = g_guest.api->guest_ticks() * 32;
    // Sprint 6 (X1): with 0 in every counter, the game may think nothing is visible (occlusion queries,
    // SAMPLES_PASSED type) and stop animating characters. counter_value_types: list of types that get
    // counter_value written instead of 0 (type 1 by default).
    // Increasing value (not constant): if the game subtracts two reports (before/after drawing the occlusion
    // box), the difference must be > 0. ResetCounter of that type sets it back to 0.
    uint64_t val = 0;
    // occlusion: 2 = real (Vulkan occlusion query, resolved value of the previous report at that address),
    // 1 = simulated increasing, 0 = always 0 (as before sprint 6; the game culls too much).
    const bool occl_type = c.type == 1;
    if (occl_type && g_cfg.occlusion == 2) {
        OcclEnd();
        FrameSlot& f = g_vk.frames[g_vk.cur];
        f.occl_events.push_back({c.addr, f.occl_used});
        occl_armed_ = g_cfg.occlusion_window;
        auto it = g_vk.occl_values.find(c.addr);
        if (it != g_vk.occl_values.end())
            val = it->second;
        else  // first time: increasing, so the difference reads "visible"
            val = g_fake_counter[1].fetch_add(g_cfg.counter_value, std::memory_order_relaxed) + g_cfg.counter_value;
    } else if (occl_type ? g_cfg.occlusion == 1 : ((g_cfg.counter_value_mask >> std::min<uint32_t>(c.type, 63)) & 1)) {
        val = g_fake_counter[std::min<uint32_t>(c.type, 31)].fetch_add(g_cfg.counter_value, std::memory_order_relaxed) +
              g_cfg.counter_value;
    }
    const uint64_t data[2] = {val, ticks};
    g_guest.Write(ctx_, cpu, data, sizeof data);
}

void Executor::Barrier(uint32_t) {
    PipelineInvalidateGathers();
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void Executor::BeginRendering() {
    if (rendering_) return;
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    uint32_t n = 0;
    for (uint32_t i = 0; i < st_.num_rt; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const TexDesc& d = st_.rt_color[i];
        n = i + 1;
        if (!d.valid()) continue;
        Image* im = g_tex.FromDesc(d, true);
        if (!im) continue;
        const uint32_t fmt = d.view_format ? d.view_format : d.format;
        VkFormat vf = NvnFormat(fmt).vk;
        if (vf == VK_FORMAT_UNDEFINED) vf = im->info.format;
        colors[i].imageView = g_tex.View(im, vf, VK_IMAGE_VIEW_TYPE_2D, d.base_level, 1, d.base_layer, 1, {},
                                         VK_IMAGE_ASPECT_COLOR_BIT);
        w = std::min(w, std::max(1u, d.width >> d.base_level));
        h = std::min(h, std::max(1u, d.height >> d.base_level));
    }
    bool has_depth = false, has_stencil = false;
    if (st_.rt_depth.valid()) {
        Image* im = g_tex.FromDesc(st_.rt_depth, true);
        if (im) {
            const VkFormat df = im->info.format;
            VkImageView v = g_tex.View(im, df, VK_IMAGE_VIEW_TYPE_2D, st_.rt_depth.base_level, 1,
                                       st_.rt_depth.base_layer, 1, {}, AspectOf(df));
            depth.imageView = v;
            depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            has_depth = true;
            if (IsStencilFormat(df)) {
                stencil = depth;
                has_stencil = true;
            }
            w = std::min(w, std::max(1u, st_.rt_depth.width >> st_.rt_depth.base_level));
            h = std::min(h, std::max(1u, st_.rt_depth.height >> st_.rt_depth.base_level));
        }
    }
    if (w == UINT32_MAX) return;  // no render targets
    g_tex.FlushPending(cmd_);
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = n;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = has_depth ? &depth : nullptr;
    ri.pStencilAttachment = has_stencil ? &stencil : nullptr;
    vkCmdBeginRendering(cmd_, &ri);
    rendering_ = true;
    rt_extent_ = {w, h};
}

void Executor::OcclBegin() {
    if (occl_active_) {
        if (occl_armed_ && --occl_armed_ == 0) OcclEnd();  // end of the window: this draw no longer counts
        return;
    }
    if (g_cfg.occlusion != 2 || !g_cfg.exclusive || !rendering_ || !occl_armed_) return;
    --occl_armed_;
    FrameSlot& f = g_vk.frames[g_vk.cur];
    if (!f.occl) return;
    if (f.occl_used >= VkCtx::kOcclQueries) {
        ++g_vk.occl_overflow;
        return;
    }
    vkCmdBeginQuery(cmd_, f.occl, f.occl_used, g_vk.has_precise_occl && g_cfg.occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    occl_active_ = true;
}

void Executor::OcclEnd() {
    if (!occl_active_) return;
    FrameSlot& f = g_vk.frames[g_vk.cur];
    vkCmdEndQuery(cmd_, f.occl, f.occl_used);
    ++f.occl_used;
    occl_active_ = false;
}

void Executor::EndRendering() {
    if (!rendering_) return;
    OcclEnd();
    vkCmdEndRendering(cmd_);
    rendering_ = false;
}

void Executor::ClearColor(const PClearColor& p) {
    if (!g_cfg.trace_gpus.empty() && p.index < 8 && g_cfg.Traced(st_.rt_color[p.index].gpu)) {
        float c[4];
        std::memcpy(c, p.color, 16);
        NVK_INFO("trace f%llu: ClearColor rt%u=%llx mask=%x num_rt=%u color=%g %g %g %g", (unsigned long long)g_stats.frames.load(),
                 p.index, (unsigned long long)st_.rt_color[p.index].gpu, p.mask, st_.num_rt, c[0], c[1], c[2], c[3]);
    }
    if (p.index >= st_.num_rt || !st_.rt_color[p.index].valid() || !p.mask) return;
    BeginRendering();
    if (!rendering_) return;
    VkClearAttachment a{};
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = p.index;
    std::memcpy(a.clearValue.color.float32, p.color, 16);
    VkClearRect r{{{0, 0}, rt_extent_}, 0, 1};
    vkCmdClearAttachments(cmd_, 1, &a, 1, &r);
    g_stats.clears.fetch_add(1, std::memory_order_relaxed);
}

void Executor::ClearDepthStencil(const PClearDepthStencil& p) {
    if (!st_.rt_depth.valid()) return;
    BeginRendering();
    if (!rendering_) return;
    Image* im = g_tex.FromDesc(st_.rt_depth, true);
    if (!im) return;
    VkClearAttachment a{};
    if (p.depth_mask) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p.stencil_mask && IsStencilFormat(im->info.format)) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a.aspectMask) return;
    a.clearValue.depthStencil = {p.depth, p.stencil};
    VkClearRect r{{{0, 0}, rt_extent_}, 0, 1};
    vkCmdClearAttachments(cmd_, 1, &a, 1, &r);
    g_stats.clears.fetch_add(1, std::memory_order_relaxed);
}

void OnTransferBegin() { g_exec.EndRendering(); }

void Executor::Draw(const PDrawArrays* da, const PDrawElements* de) {
    if (g_cfg.skip_draws || (!st_.num_rt && !st_.rt_depth.valid())) {  // skip_draws: diagnostics
        g_stats.draws_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    VkFormat cf[8] = {};
    for (uint32_t i = 0; i < st_.num_rt; ++i) {
        const TexDesc& d = st_.rt_color[i];
        cf[i] = d.valid() ? NvnFormat(d.view_format ? d.view_format : d.format).vk : VK_FORMAT_UNDEFINED;
    }
    VkFormat df = st_.rt_depth.valid() ? NvnFormat(st_.rt_depth.format).vk : VK_FORMAT_UNDEFINED;
    const uint32_t ms = (st_.multisample[0] >> 8) & 0x1F;
    VkSampleCountFlagBits samples = ms > 1 ? (VkSampleCountFlagBits)ms : VK_SAMPLE_COUNT_1_BIT;
    if (st_.num_rt && st_.rt_color[0].samples > 1) samples = (VkSampleCountFlagBits)st_.rt_color[0].samples;
    const bool hud_track = g_cfg.aa && g_cfg.aa_hud && st_.num_rt;
    t_win_draw = hud_track && IsWindowTex(st_.rt_color[0].gpu);
    if (EmitDraw(this, ctx_, cmd_, st_, da, de, cf, st_.num_rt, df, samples))
        g_stats.draws.fetch_add(1, std::memory_order_relaxed);
    else
        g_stats.draws_skipped.fetch_add(1, std::memory_order_relaxed);
    t_win_draw = false;
    // The game composes scene + UI into its own texture and copies it to the window texture with a single draw: the scene
    // is complete after draw number aa_scene_draw on that texture (the one sampled by the window draw).
    const uint64_t comp = g_win_src.load(std::memory_order_relaxed);
    if (hud_track && comp && st_.rt_color[0].gpu == comp && ++win_draws_ == g_cfg.aa_scene_draw && !scene_snapped_) {
        if (Image* im = g_tex.FromDesc(st_.rt_color[0], true)) {
            EndRendering();
            g_present.SnapScene(cmd_, im->img, im->info.format, (int)st_.rt_color[0].width, (int)st_.rt_color[0].height);
            scene_snapped_ = true;
        }
    }
    if (PipelineHasScatters()) {
        EndRendering();
        PipelineFlushScatters(cmd_);
    }
    if (g_cfg.unified_barriers_all && PipelineUnified()) {  // diagnostics: serialize all draws
        EndRendering();
        Barrier(0);
    }
    // Chunked submit: every chunk_draws draws, what has been recorded is submitted so the GPU does not wait for the end
    // of the frame (on the mount, submit CPU ~10 ms + GPU ~5.5 ms in series: the game waits for the previous frame).
    if (g_cfg.chunk_draws && ++chunk_draws_ >= g_cfg.chunk_draws) {
        chunk_draws_ = 0;
        EndRendering();
        cmd_ = g_vk.SubmitChunk();
        Barrier(0);
        st_.rt_dirty = true;
    }
}

void Executor::Copy(Op op, const uint32_t* p) {
    ScopedPhase _ph(g_phase_copy_ns);
    EndRendering();
    g_stats.copies.fetch_add(1, std::memory_order_relaxed);
    switch (op) {
        case Op::CopyTextureToTexture: {
            const auto& c = As<PCopyTexTex>(p);
            if (g_cfg.Traced(c.dst.gpu) || g_cfg.Traced(c.src.gpu) || g_cfg.log_tex_updates)
                NVK_INFO("trace f%llu: CopyTextureToTexture %llx (%ux%u) -> %llx (%ux%u)", (unsigned long long)g_stats.frames.load(),
                         (unsigned long long)c.src.gpu, c.src.width, c.src.height, (unsigned long long)c.dst.gpu, c.dst.width,
                         c.dst.height);
            Image* src = g_tex.FromDesc(c.src, false);
            Image* dst = g_tex.FromDesc(c.dst, true);
            if (!src || !dst) return;
            g_tex.FlushPending(cmd_);
            Barrier(0);
            VkImageBlit b{};
            b.srcSubresource = {src->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect,
                                c.src.base_level, c.src.base_layer, 1};
            b.dstSubresource = {dst->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : dst->aspect,
                                c.dst.base_level, c.dst.base_layer, 1};
            auto reg = [](const PRegion& r, const TexDesc& d, VkOffset3D o[2]) {
                const int w = r.w ? r.w : (int)d.width, h = r.h ? r.h : (int)d.height, dd = r.d ? r.d : 1;
                o[0] = {r.x, r.y, r.z};
                o[1] = {r.x + w, r.y + h, r.z + dd};
            };
            reg(c.src_region, c.src, b.srcOffsets);
            reg(c.dst_region, c.dst, b.dstOffsets);
            const bool same = (b.srcOffsets[1].x - b.srcOffsets[0].x) == (b.dstOffsets[1].x - b.dstOffsets[0].x) &&
                              (b.srcOffsets[1].y - b.srcOffsets[0].y) == (b.dstOffsets[1].y - b.dstOffsets[0].y);
            if (same && src->info.format == dst->info.format) {
                VkImageCopy ic{};
                ic.srcSubresource = b.srcSubresource;
                ic.dstSubresource = b.dstSubresource;
                ic.srcOffset = b.srcOffsets[0];
                ic.dstOffset = b.dstOffsets[0];
                ic.extent = {(uint32_t)(b.srcOffsets[1].x - b.srcOffsets[0].x), (uint32_t)(b.srcOffsets[1].y - b.srcOffsets[0].y), 1};
                vkCmdCopyImage(cmd_, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
            } else {
                vkCmdBlitImage(cmd_, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                               (c.flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            }
            Barrier(0);
            break;
        }
        case Op::Downsample: {
            const auto& c = As<PDownsample>(p);
            if (g_cfg.log_tex_updates)
                NVK_INFO("trace f%llu: Downsample %llx (%ux%u) -> %llx (%ux%u)", (unsigned long long)g_stats.frames.load(),
                         (unsigned long long)c.src.gpu, c.src.width, c.src.height, (unsigned long long)c.dst.gpu,
                         c.dst.width, c.dst.height);
            Image* src = g_tex.FromDesc(c.src, false);
            Image* dst = g_tex.FromDesc(c.dst, true);
            if (!src || !dst) return;
            g_tex.FlushPending(cmd_);
            Barrier(0);
            if (src->info.samples != VK_SAMPLE_COUNT_1_BIT) {
                VkImageResolve r{};
                r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.extent = {dst->info.width, dst->info.height, 1};
                vkCmdResolveImage(cmd_, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
            } else if (!IsDepthFormat(src->info.format) && !IsDepthFormat(dst->info.format)) {
                // nvnCommandBufferDownsample without MSAA: filtered reduction (e.g. 1920x1080 -> 960x540). Previously
                // nothing was done and the half-resolution buffers read by field lighting stayed at zero.
                VkImageBlit b{};
                b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.srcOffsets[1] = {(int32_t)src->info.width, (int32_t)src->info.height, 1};
                b.dstOffsets[1] = {(int32_t)dst->info.width, (int32_t)dst->info.height, 1};
                vkCmdBlitImage(cmd_, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                               VK_FILTER_LINEAR);
            }
            if (g_cfg.Traced(c.dst.gpu) || g_cfg.Traced(c.src.gpu))
                NVK_INFO("trace f%llu: Downsample %llx (%ux%u) -> %llx (%ux%u)", (unsigned long long)g_stats.frames.load(),
                         (unsigned long long)c.src.gpu, src->info.width, src->info.height, (unsigned long long)c.dst.gpu,
                         dst->info.width, dst->info.height);
            Barrier(0);
            break;
        }
        case Op::ClearTexture: {
            const auto& c = As<PClearTexture>(p);
            if (g_cfg.Traced(c.tex.gpu) || g_cfg.log_tex_updates)
                NVK_INFO("trace f%llu: ClearTexture %llx %ux%u fmt=%u color=%08x %08x %08x %08x", (unsigned long long)g_stats.frames.load(),
                         (unsigned long long)c.tex.gpu, c.tex.width, c.tex.height, c.tex.format, c.color[0], c.color[1],
                         c.color[2], c.color[3]);
            Image* im = g_tex.FromDesc(c.tex, true);
            if (!im || IsDepthFormat(im->info.format)) return;
            g_tex.FlushPending(cmd_);
            VkClearColorValue v;
            std::memcpy(v.float32, c.color, 16);
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, c.tex.base_level, 1, c.tex.base_layer, 1};
            vkCmdClearColorImage(cmd_, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
            Barrier(0);
            break;
        }
        case Op::CopyBufferToTexture: {
            const auto& c = As<PCopyBufTex>(p);
            if (g_cfg.Traced(c.tex.gpu) || g_cfg.log_tex_updates)
                NVK_INFO("trace f%llu: CopyBufferToTexture -> %llx region %d,%d %ux%u", (unsigned long long)g_stats.frames.load(),
                         (unsigned long long)c.tex.gpu, c.region.x, c.region.y, c.region.w, c.region.h);
            Image* im = g_tex.FromDesc(c.tex, true);
            if (!im) return;
            g_tex.FlushPending(cmd_);
            const FormatInfo& fi = NvnFormat(c.tex.format);
            const uint32_t w = c.region.w ? c.region.w : c.tex.width, h = c.region.h ? c.region.h : c.tex.height;
            const uint32_t d = c.region.d ? c.region.d : 1;
            const uint64_t bytes = (uint64_t)((w + fi.bw - 1) / fi.bw) * ((h + fi.bh - 1) / fi.bh) * d * std::max(1u, fi.bpb);
            uint64_t avail = 0;
            const uint64_t cpu = g_reg.GpuToCpu(c.buffer, &avail);
            if (!cpu || !bytes) return;
            // Staging from the frame ring if it fits (no memory allocation per copy).
            Buffer stg{};
            uint8_t* sp = nullptr;
            VkDeviceSize so = bytes <= (16ull << 20) ? g_vk.UploadAlloc(bytes, 16, &sp) : VK_WHOLE_SIZE;
            VkBuffer sb = g_vk.frames[g_vk.cur].upload.buf;
            if (so == VK_WHOLE_SIZE) {
                stg = g_vk.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
                sp = stg.map;
                sb = stg.buf;
                so = 0;
            }
            g_guest.Read(ctx_, cpu, sp, std::min(bytes, avail));
            VkBufferImageCopy r{};
            r.imageSubresource = {im->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect,
                                  c.tex.base_level, c.tex.base_layer, 1};
            r.imageOffset = {c.region.x, c.region.y, c.region.z};
            r.imageExtent = {w, h, d};
            Barrier(0);
            r.bufferOffset = so;
            vkCmdCopyBufferToImage(cmd_, sb, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
            Barrier(0);
            if (stg.buf) g_vk.frames[g_vk.cur].dead_buffers.push_back(stg);
            break;
        }
        case Op::CopyBufferToBuffer: {
            if (!g_cfg.exclusive) break;  // in shadow mode the emulated GPU does it in guest memory
            const auto& c = As<PCopyBuffer>(p);
            uint64_t a1 = 0, a2 = 0;
            const uint64_t s = g_reg.GpuToCpu(c.src, &a1), d = g_reg.GpuToCpu(c.dst, &a2);
            if (!s || !d) break;
            const uint64_t n = std::min({c.size, a1, a2});
            if (PipelineUnified() && g_cfg.unified_copy_gpu) {
                // Unified memory: the copy goes on the GPU timeline (what XFB/compute wrote earlier is not yet in
                // memory when this is recorded; a CPU copy would read stale data).
                // In pieces: source and destination may cross backing run boundaries (previously that fell back to
                // the CPU copy only, outside GPU ordering).
                struct Piece {
                    VkBuffer s, d;
                    VkBufferCopy c;
                };
                std::vector<Piece> pieces;
                bool ok = n != 0;
                for (uint64_t o = 0; ok && o < n;) {
                    HostRef hs, hd;
                    if (!g_hostmem.Find(c.src + o, n - o, 1, &hs, true) || !g_hostmem.Find(c.dst + o, n - o, 1, &hd, true)) {
                        ok = false;
                        break;
                    }
                    const uint64_t m = std::min({hs.avail, hd.avail, n - o});
                    if (!m) {
                        ok = false;
                        break;
                    }
                    pieces.push_back({hs.buf, hd.buf, VkBufferCopy{hs.off, hd.off, m}});
                    o += m;
                }
                if (ok) {
                    // The destination is only "GPU-written" if the source is (or lives in VRAM): otherwise the CPU copy
                    // below leaves game memory equal to the GPU's and vbib_snapshot can keep copying at record time.
                    if (g_cfg.unified_copy_gpu_only || g_hostmem.Residency(c.src, n) != 3) g_hostmem.MarkGpuWritten(c.dst, n);
                    Barrier(0);
                    for (const Piece& pc : pieces) vkCmdCopyBuffer(cmd_, pc.s, pc.d, 1, &pc.c);
                    Barrier(0);
                    // Texture uploads (by CPU, at record time) need the data now, and the texture may be created later
                    // over that memory (streaming): so the CPU copy is done too, except with unified_copy_gpu_only.
                    if (g_cfg.unified_copy_gpu_only && !g_tex.Overlaps(c.dst, n)) break;
                }
            }
            std::vector<uint8_t> tmp(n);
            g_guest.Read(ctx_, s, tmp.data(), n);
            g_guest.Write(ctx_, d, tmp.data(), n);
            g_tex.Invalidate(c.dst, n, true);  // if it lands in a texture, it must be re-uploaded
            // Vertex/index/UBO copies already uploaded in this submit become stale: the draws that
            // follow must read the copied data (sprint 5, sky artifacts).
            PipelineInvalidateUploads();
            break;
        }
        default: break;
    }
}

void Executor::CaptureNext(const std::string& path) { capture_path_ = path; }

void Executor::DumpImage(Image* im, const std::string& path) {
    // Synchronous copy to a host buffer and writes a PPM (RGB8). Only for debugging/comparison.
    const uint32_t w = im->info.width, h = im->info.height;
    Buffer b = g_vk.CreateBuffer((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = g_vk.qfamily;
    vkCreateCommandPool(g_vk.dev, &pci, nullptr, &pool);
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(g_vk.dev, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.queue);
    // Live internal resolution (render_res): only the area the game draws to (the size of the capture = the internal
    // resolution in use).
    uint32_t ow = w, oh = h;
    if (const uint32_t crop = g_present_crop.load(); crop && (crop >> 16) <= w && (crop & 0xFFFF) <= h) {
        ow = crop >> 16;
        oh = crop & 0xFFFF;
    }
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", ow, oh);
        const bool bgr = im->info.format == VK_FORMAT_B8G8R8A8_UNORM || im->info.format == VK_FORMAT_B8G8R8A8_SRGB;
        std::vector<uint8_t> row(ow * 3);
        for (uint32_t y = 0; y < oh; ++y) {
            const uint8_t* s = b.map + (uint64_t)y * w * 4;
            for (uint32_t x = 0; x < ow; ++x) {
                row[x * 3 + 0] = s[x * 4 + (bgr ? 2 : 0)];
                row[x * 3 + 1] = s[x * 4 + 1];
                row[x * 3 + 2] = s[x * 4 + (bgr ? 0 : 2)];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
    vkDestroyCommandPool(g_vk.dev, pool, nullptr);
    g_vk.DestroyBuffer(b);
}

namespace {
uint32_t TexelBytes(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SNORM: case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT:
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R5G6B5_UNORM_PACK16: return 2;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 4;  // RGBA8/BGRA8, A2B10G10R10, B10G11R11, R32, RG16, D32, D24S8 (depth aspect)
    }
}
}  // namespace

// Raw dump (level 0, layer 0) of each render target written in frame `frame`:
// <dir>/rt-<sec>s-<gpu>-<w>x<h>-f<VkFormat>.bin. tools/rt_dump.py converts them to PNG.
void Executor::DumpRenderTargets(uint64_t frame, int sec) {
    std::vector<Image*> rts;
    g_tex.ForEach([&](Image* im) {
        if (((im->gpu_written && im->last_use == frame) || g_cfg.Traced(im->info.gpu)) &&
            im->info.samples == VK_SAMPLE_COUNT_1_BIT && im->info.type == VK_IMAGE_TYPE_2D)
            rts.push_back(im);
    });
    for (Image* im : rts) {
        const uint32_t w = im->info.width, h = im->info.height, bpp = TexelBytes(im->info.format);
        Buffer b = g_vk.CreateBuffer((uint64_t)w * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkCommandPool pool;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = g_vk.qfamily;
        vkCreateCommandPool(g_vk.dev, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(g_vk.dev, &ai, &cb);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkBufferImageCopy r{};
        const VkImageAspectFlags asp = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource = {asp, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(g_vk.queue);
        char name[160];
        std::snprintf(name, sizeof name, "/rt-%03ds-%llx-%ux%u-f%d.bin", sec, (unsigned long long)im->info.gpu, w, h,
                      (int)im->info.format);
        if (FILE* f = std::fopen((g_cfg.capture_dir + name).c_str(), "wb")) {
            std::fwrite(b.map, 1, (size_t)w * h * bpp, f);
            std::fclose(f);
        }
        vkDestroyCommandPool(g_vk.dev, pool, nullptr);
        g_vk.DestroyBuffer(b);
    }
    NVK_INFO("dumped %zu render targets of frame %llu (%d s)", rts.size(), (unsigned long long)frame, sec);
}

// `fps_limit` option: waits on the game thread until the next tick of the fixed pace (high-resolution timer + busy wait
// for the last half millisecond). If running late, the pace restarts without accumulating.
static void PaceFrame() {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static LARGE_INTEGER next{};
    LARGE_INTEGER fq, now;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&now);
    const int64_t period = fq.QuadPart / g_cfg.fps_limit;
    if (next.QuadPart == 0 || now.QuadPart - next.QuadPart > period) {
        next.QuadPart = now.QuadPart + period;
        return;
    }
    const int64_t left = next.QuadPart - now.QuadPart;
    const int64_t spin = fq.QuadPart / 2000;
    if (left > spin && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(int64_t)((double)(left - spin) * 1e7 / (double)fq.QuadPart);  // in 100 ns units, relative
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < next.QuadPart);
    next.QuadPart += period;
}

void Executor::Present(pla_ctx* ctx, uint64_t window, int index) {
    if (g_cfg.fps_limit > 0) PaceFrame();
    WindowInfo wi;
    if (!g_cfg.execute ||
        (!g_cfg.exclusive && g_cfg.start_after && g_stats.frames.load(std::memory_order_relaxed) < g_cfg.start_after)) {
        g_stats.frames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_vk.dev && !InitVulkan()) {  // shadow with start_after: the device is created here
        g_stats.frames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_obj.Window(window, &wi) || index < 0 || index >= (int)wi.textures.size()) return;
    TexDesc d;
    if (!ReadTexDesc(ctx, wi.textures[index], 0, &d)) return;
    if (async_) {
        Job j;
        j.present = true;
        j.win = d;
        Enqueue(std::move(j));
        return;
    }
    PresentNow(ctx, d);
}

void Executor::HitchCheck() {
    // Long frames (present to present): how much was nvnvk CPU and what was created in that frame.
    const auto now = std::chrono::steady_clock::now();
    uint64_t pc[4];
    PipelineCounters(pc);
    const uint64_t c[10] = {g_stats.submit_ns.load(), g_stats.textures.load(), g_stats.reuploads.load(), pc[0], pc[1],
                           g_stats.pipelines.load(), pc[3], g_vk.gpubuf_created, pc[2],
                           g_vk.fence_wait_ns.load()};
    if (last_present_.time_since_epoch().count()) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_present_).count();
        if (frame_ema_ms_ > 0 && ms > g_cfg.hitch_ms && ms > 2 * frame_ema_ms_ && hitch_logs_ < 300) {
            ++hitch_logs_;
            NVK_INFO("hitch: frame %.1f ms (avg %.1f): submit CPU %.1f ms (waiting on GPU %.1f); variants +%llu (%.1f ms), pipelines +%llu "
                     "(wait %.1f ms), new textures +%llu, reuploads +%llu, XFB buffers +%llu, skipped draws %llu",
                     ms, frame_ema_ms_, (c[0] - hitch_base_[0]) / 1e6, (c[9] - hitch_base_[9]) / 1e6, (unsigned long long)(c[3] - hitch_base_[3]),
                     (c[4] - hitch_base_[4]) / 1e3, (unsigned long long)(c[5] - hitch_base_[5]), (c[6] - hitch_base_[6]) / 1e3,
                     (unsigned long long)(c[1] - hitch_base_[1]), (unsigned long long)(c[2] - hitch_base_[2]),
                     (unsigned long long)(c[7] - hitch_base_[7]), (unsigned long long)(c[8] - hitch_base_[8]));
        }
        const double sub_ms = (c[0] - hitch_base_[0]) / 1e6;
        if (sub_ms > g_cfg.hitch_phase_ms && hitch_logs_ < 300) {
            uint64_t pc5[6];
            PipelineCpuCounters(pc5);
            NVK_INFO("  submit by phase (ms): uploads %.1f, descriptors %.1f, variants %.1f, pipelines %.1f, draws total %.1f, "
                     "copies %.1f, pending textures %.1f, GPU wait %.1f, XFB fingerprint %.1f",
                     (pc5[0] - phase_base_[0]) / 1e6, (pc5[1] - phase_base_[1]) / 1e6, (pc5[2] - phase_base_[2]) / 1e6,
                     (pc5[3] - phase_base_[3]) / 1e6, (pc5[4] - phase_base_[4]) / 1e6,
                     (g_phase_copy_ns.load() - phase_base_[6]) / 1e6, (g_phase_flush_ns.load() - phase_base_[7]) / 1e6,
                     (c[9] - hitch_base_[9]) / 1e6, (pc5[5] - phase_base_[5]) / 1e6);
        }
        if (ms < 1000) frame_ema_ms_ = frame_ema_ms_ > 0 ? frame_ema_ms_ * 0.95 + ms * 0.05 : ms;
    }
    std::memcpy(hitch_base_, c, sizeof c);
    {
        uint64_t pc5[6];
        PipelineCpuCounters(pc5);
        for (int i = 0; i < 6; ++i) phase_base_[i] = pc5[i];
        phase_base_[6] = g_phase_copy_ns.load();
        phase_base_[7] = g_phase_flush_ns.load();
    }
    last_present_ = now;
}

void Executor::PresentNow(pla_ctx*, const TexDesc& d) {
    std::lock_guard<std::mutex> lk(g_vk.submit_mu);
    if ((g_cfg.xfb_check || g_cfg.xfb_trace) && PipelineUnified()) {
        // The frame just recorded (armed at the previous present) is already queued: wait for it and check it.
        if (xfb_check_armed_) {
            vkQueueWaitIdle(g_vk.queue);
            PipelineXfbCheckEnd(nullptr, frame_);
            xfb_check_armed_ = false;
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cfg.t0).count();
        const bool in_win = !g_cfg.xfb_trace || (t >= g_cfg.xfb_trace_from && t < g_cfg.xfb_trace_to);
        if (in_win && (frame_ % (g_cfg.xfb_trace ? g_cfg.xfb_trace : 60)) == 0) {
            PipelineXfbCheckBegin();
            xfb_check_armed_ = true;
        }
    }
    if (g_cfg.import_verify && (frame_ % 300) == 299) {
        vkQueueWaitIdle(g_vk.queue);
        g_hostmem.Verify(256);
    }
    if (g_cfg.hitch_ms > 0) HitchCheck();
    PipelineBeginFrame();
    if (g_cfg.aa && g_cfg.aa_hud) {
        if (!IsWindowTex(d.gpu)) {
            win_gpu_[win_idx_++ & 3] = d.gpu;
            if (g_cfg.trace_window) g_cfg.trace_gpus.push_back(d.gpu);
            NVK_INFO("aa_hud: window texture %llx", (unsigned long long)d.gpu);
        }
        if ((frame_ % 1800) == 300)
            NVK_INFO("aa_hud: %u draws on composition texture %llx this frame; scene copied: %s", win_draws_,
                     (unsigned long long)g_win_src.load(), scene_snapped_ ? "yes" : "no");
        g_present.SetSceneValid(scene_snapped_);
        win_draws_ = 0;
        scene_snapped_ = false;
    }
    Image* im = g_tex.FromDesc(d, false);
    ++frame_;
    const uint64_t this_frame = g_stats.frames.fetch_add(1, std::memory_order_relaxed);
    if (im && g_cfg.capture_every > 0 && !g_cfg.capture_dir.empty() && frame_ % g_cfg.capture_every == 0 &&
        (g_cfg.capture_max <= 0 || (int)(frame_ / g_cfg.capture_every) <= g_cfg.capture_max)) {
        vkQueueWaitIdle(g_vk.queue);
        char name[64];
        std::snprintf(name, sizeof name, "/nvnvk-f%06llu.ppm", (unsigned long long)frame_);
        DumpImage(im, g_cfg.capture_dir + name);
    }
    if (im && !g_cfg.capture_dir.empty() && next_sec_ < g_cfg.capture_secs.size()) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cfg.t0).count();
        if (t >= g_cfg.capture_secs[next_sec_]) {
            vkQueueWaitIdle(g_vk.queue);
            char name[64];
            {
                const double cs = g_cfg.capture_secs[next_sec_];
                const int ms = (int)((cs - (int)cs) * 1000.0 + 0.5);
                if (ms) std::snprintf(name, sizeof name, "/nvnvk-%03ds%03d.ppm", (int)cs, ms);  // seconds with fraction
                else std::snprintf(name, sizeof name, "/nvnvk-%03ds.ppm", (int)cs);
            }
            DumpImage(im, g_cfg.capture_dir + name);
            {  // with the final pass active, also the image actually shown (from this same frame, after the present)
                post_dump_ = g_cfg.capture_dir + name;
                post_dump_.replace(post_dump_.size() - 4, 4, "-final.ppm");
                const VkFormat ff = im->info.format;
                post_dump_srgb_ = ff == VK_FORMAT_R8G8B8A8_SRGB || ff == VK_FORMAT_B8G8R8A8_SRGB || ff == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
            }
            if (g_vk.rdoc) {
                static_cast<RENDERDOC_API_1_6_0*>(g_vk.rdoc)->TriggerCapture();  // the next vkQueuePresentKHR (this frame is already recorded: the following one)
                NVK_INFO("RenderDoc: capture requested at %.0f s", g_cfg.capture_secs[next_sec_]);
            }
            if (g_cfg.dump_rts) DumpRenderTargets(this_frame, (int)g_cfg.capture_secs[next_sec_]);
            ++next_sec_;
        }
    }
    FrameSlot& f = g_vk.Begin();
    g_hostmem.CollectDead(f.dead_buffers);
    g_hostmem.CollectDeadSub(f.dead_subbufs);
    g_hostmem.ProcessPending();
    g_tex.FlushPending(f.cmd);
    if (!g_present.alive()) {
        g_vk.Submit();  // always one Submit per job (the planned timeline counts on it)
        return;
    }
    g_present.Present(f.cmd, im ? im->img : VK_NULL_HANDLE, (int)d.width, (int)d.height, im ? im->info.format : VK_FORMAT_UNDEFINED);
    g_vk.Submit(g_present.acquire_sem(), g_present.render_sem());
    g_present.FinishPresent();
    g_stats.presents.fetch_add(1, std::memory_order_relaxed);
    if (!post_dump_.empty()) {
        vkQueueWaitIdle(g_vk.queue);
        g_present.DumpPost(post_dump_, post_dump_srgb_);
        post_dump_.clear();
    }
}

}  // namespace nvnvk
