// Draws: programs (nvnshader translator), variants per bound textures (TranslateBound),
// cached pipelines (prewarmed VkPipelineCache), descriptors and buffer uploads.
// SPDX-License-Identifier: MIT
#include <set>
#include <cmath>
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <thread>
#include <deque>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "capture.h"
#include "exec.h"
#include "formats.h"
#include "hostmem.h"
#include "objects.h"
#include "pipeline.h"

#ifdef NVNVK_WITH_NVNSHADER
#include "nvnshader/nvnshader.h"
#include "nvnshader/vk_host_caps.h"
#include "nvnshader/vk_program_layout.h"
#endif
#include "nvnvk/shader_iface.h"

namespace nvnvk {
// aa_hud: the current draw renders into the window texture; the first texture it samples (the already composited image, scene
// + UI) is stored in g_win_src so the executor knows where the frame is composited (exec.cpp, Executor::Draw).
thread_local bool t_win_draw = false;
std::atomic<uint64_t> g_win_src{0};

DeviceModes g_modes;
thread_local uint64_t t_xfb_draw_ctrl = ~0ull;

#ifndef NVNVK_WITH_NVNSHADER
void PipelineInit() { NVK_WARN("nvnvk built without nvnshader: nothing is drawn"); }

void PipelineShutdown() {}
void PipelineBeginSubmit() {}
void PipelineSnapPromote(VkCommandBuffer) {}
void PipelineReport() {}
void PipelineCounters(uint64_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
void PipelineInvalidateUploads() {}
bool PipelineUnified() { return false; }
void PipelineXfbCheckBegin() {}
void PipelineXfbCheckEnd(pla_ctx*, uint64_t) {}
bool PipelineHasScatters() { return false; }
void PipelineFlushScatters(VkCommandBuffer) {}
void PipelineInvalidateGathers() {}
void SyncGpuBuffers(VkCommandBuffer) {}
void QueueWriteback(VkCommandBuffer) {}
void ResolveWriteback(FrameSlot&) {}
void PipelineCpuCounters(uint64_t out[6]) { for (int i = 0; i < 6; ++i) out[i] = 0; }
void PipelineBeginFrame() {}
bool EmitDraw(Executor*, pla_ctx*, VkCommandBuffer, const NvnState&, const PDrawArrays*, const PDrawElements*, VkFormat*, uint32_t,
              VkFormat, VkSampleCountFlagBits) {
    return false;
}
bool EmitDispatch(pla_ctx*, VkCommandBuffer, const NvnState&, uint32_t, uint32_t, uint32_t) { return false; }
#else

using shader::AttribType;
using shader::Descriptor;
using shader::DescriptorType;
using shader::HostCaps;
using shader::ITranslator;
using shader::ShaderBinary;
using shader::ShaderHeader;
using shader::ShaderModule;
using shader::Stage;
using shader::TextureHint;
using shader::TextureResolver;
using shader::TextureType;
using shader::TranslationState;
namespace sh = nvnvk::shader;

namespace {

std::unique_ptr<ITranslator> g_tr;
bool g_use_import = false;
// Unified memory (sprint 7, nvnvk.h): direct bindings on the imported pools, no buffers of our own.
bool Unified() { return g_use_import && g_cfg.unified_memory; }
std::atomic<uint64_t> g_unified_miss{0};
// XFB counter slot per control address (BeginTransformFeedback / DrawTransformFeedback).
std::unordered_map<uint64_t, uint32_t> g_xfb_slots;
uint32_t XfbSlot(uint64_t ctrl) {
    if (!ctrl) return 0;
    auto it = g_xfb_slots.find(ctrl);
    if (it != g_xfb_slots.end()) return it->second;
    const uint32_t s = 1 + (uint32_t)(g_xfb_slots.size() % (VkCtx::kXfbSlots - 1));
    g_xfb_slots[ctrl] = s;
    return s;
}
uint64_t g_xfb_draws = 0, g_xfb_draws_bad = 0;
// xfb_check diagnostics (unified memory): XFB draws of the marked frame, to later inspect in game memory
// the positions they wrote (NaN/inf/huge = exploded vertices) and that vertex's input.
struct XfbRec {
    uint64_t program, out_addr, out_size, in_addr, in_stride, first, count, ctrl;
    bool resume;
    uint32_t seq;  // draw order within the frame
    uint32_t mask;              // xfb_resume_mask
    VertexBinding ubo[18];      // vertex shader UBOs (xfb_trace: bone palette fingerprint)
    bool gathered;              // the output crossed a span boundary: written to a gathered copy
    uint32_t vattrib[16];       // attribute formats (xfb_trace)
    uint64_t vstream[4];
    VertexBinding vbuf[4];
};
std::vector<XfbRec> g_xfb_recs;
bool g_xfb_rec_on = false;
struct VbUse {
    uint64_t addr, size, program;
    uint32_t slot, stride, count, seq;
};
std::vector<VbUse> g_vb_uses;  // xfb_trace: vertex buffers of the non-XFB draws of the marked frame
// Unified memory over the backing store: a binding that crosses a boundary between physical spans is gathered into the ring
// with GPU copies (before opening the render pass) and, if the draw/dispatch writes it (SSBO, XFB), scattered back afterwards.
struct Gather {
    uint64_t gpu, size;
    VkBuffer buf;
    VkDeviceSize off;
    bool write;
};
thread_local VkCommandBuffer t_cmd = VK_NULL_HANDLE;  // command buffer of the current draw/dispatch (for gathering)
std::vector<Gather> g_gathers;   // valid until the next barrier/write (PipelineInvalidateGathers)
std::vector<Gather> g_scatters;  // to scatter back after the current draw/dispatch
uint64_t g_gather_n = 0, g_gather_bytes = 0, g_scatter_n = 0;

void CopyPieces(VkCommandBuffer cmd, uint64_t gpu, uint64_t size, VkBuffer tmp, VkDeviceSize toff, bool to_tmp) {
    uint64_t o = 0;
    while (o < size) {
        HostRef h;
        if (!g_hostmem.Find(gpu + o, size - o, 1, &h, true)) break;
        const uint64_t n = std::min<uint64_t>(h.avail, size - o);
        if (!n) break;
        const VkBufferCopy c{to_tmp ? h.off : toff + o, to_tmp ? toff + o : h.off, n};
        if (to_tmp)
            vkCmdCopyBuffer(cmd, h.buf, tmp, 1, &c);
        else
            vkCmdCopyBuffer(cmd, tmp, h.buf, 1, &c);
        o += n;
    }
}

void FullBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

void AllocUpload(uint64_t size, VkDeviceSize align, VkBuffer* buf, VkDeviceSize* off, uint8_t** ptr);
// Returns the gathered binding for [gpu, gpu+size) if it crosses spans (creating it, which closes the render pass), or false otherwise.
bool GatherIfCrossing(VkCommandBuffer cmd, uint64_t gpu, uint64_t size, bool write, VkBuffer* buf, VkDeviceSize* off) {
    HostRef h;
    if (write) g_hostmem.MarkGpuWritten(gpu, size);
    if (!g_hostmem.Find(gpu, size, 1, &h, true) || h.want <= h.avail) return false;
    const uint64_t want = h.want;
    for (const Gather& g : g_gathers)
        if (g.gpu == gpu && g.size >= want) {
            if (write && !g.write) break;  // gather again as writable
            *buf = g.buf;
            *off = g.off;
            if (write) g_scatters.push_back(g);
            return true;
        }
    OnTransferBegin();
    uint8_t* p = nullptr;
    Gather g{gpu, want, VK_NULL_HANDLE, 0, write};
    AllocUpload(want, 256, &g.buf, &g.off, &p);
    FullBarrier(cmd);
    CopyPieces(cmd, gpu, want, g.buf, g.off, true);
    FullBarrier(cmd);
    g_gathers.push_back(g);
    if (write) g_scatters.push_back(g);
    ++g_gather_n;
    g_gather_bytes += want;
    *buf = g.buf;
    *off = g.off;
    return true;
}
HostCaps g_caps;
VkPipelineCache g_pcache = VK_NULL_HANDLE;
std::mutex g_mu;  // caches (submission is already serialized; this guards against the shutdown thread)

struct PipeStats {
    uint64_t variants = 0, variant_fail = 0, pipelines = 0, pipe_fail = 0, imported_binds = 0;
    double translate_ms = 0, pipeline_ms = 0;
} g_ps;
std::mutex g_ps_mu;
uint64_t g_xfb_inits = 0;  // new XFB buffers initialized from the guest (and replaced ones)  // BuildVariant may run on background threads

double Ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------- program sources
struct StageSrc {
    std::vector<uint8_t> control, data;
    ShaderHeader hdr;
    bool ok = false;
};
struct ProgramSrc {
    uint64_t version = 0;
    std::vector<StageSrc> stages;  // in order VS, (TCS, TES, GS), FS / CS
    bool ok = false;
};
std::unordered_map<uint64_t, std::shared_ptr<ProgramSrc>> g_srcs;

std::shared_ptr<ProgramSrc> LoadProgram(pla_ctx* ctx, uint64_t prog) {
    auto info = g_obj.Program(prog);
    if (!info) return nullptr;
    auto it = g_srcs.find(prog);
    if (it != g_srcs.end() && it->second->version == info->version) return it->second;
    auto src = std::make_shared<ProgramSrc>();
    src->version = info->version;
    src->ok = true;
    for (const ProgramStage& s : info->stages) {
        StageSrc st;
        st.control.resize(0x800);
        g_guest.Read(ctx, s.control_cpu, st.control.data(), st.control.size());
        uint32_t csize = 0;
        std::memcpy(&csize, st.control.data() + 0x6F0, 4);
        if (csize > 0x800 && csize < (1u << 20)) {
            st.control.resize(csize);
            g_guest.Read(ctx, s.control_cpu, st.control.data(), csize);
        }
        uint32_t dsize = 0;
        std::memcpy(&dsize, st.control.data() + 0x704, 4);
        uint64_t avail = 0;
        const uint64_t cpu = g_reg.GpuToCpu(s.data_gpu, &avail);
        if (cpu && dsize && dsize <= avail) {
            st.data.resize(dsize);
            g_guest.Read(ctx, cpu, st.data.data(), dsize);
        }
        ShaderBinary b{st.control.data(), st.control.size(), st.data.data(), st.data.size()};
        std::string err;
        st.ok = !st.data.empty() && nvnshader::ParseControl(b, &st.hdr, &err);
        if (!st.ok) {
            NVK_WARN("program %llx: invalid stage (%s)", (unsigned long long)prog, err.c_str());
            src->ok = false;
        }
        src->stages.push_back(std::move(st));
    }
    std::stable_sort(src->stages.begin(), src->stages.end(), [](const StageSrc& a, const StageSrc& b) {
        auto rank = [](Stage s) {
            switch (s) {
                case Stage::Vertex: return 0;
                case Stage::TessControl: return 1;
                case Stage::TessEval: return 2;
                case Stage::Geometry: return 3;
                case Stage::Fragment: return 4;
                default: return 5;
            }
        };
        return rank(a.hdr.stage) < rank(b.hdr.stage);
    });
    g_srcs[prog] = src;
    return src;
}

// ---------------------------------------------------------------- translated variants
struct DescSlot {
    Descriptor d;
    Stage stage;
    uint32_t binding;
    VkDescriptorType type;
};
struct Variant {
    std::vector<std::shared_ptr<const ShaderModule>> mods;  // from TranslateBound (not relocated)
    std::vector<Stage> stages;
    std::vector<VkShaderModule> vk;
    std::vector<DescSlot> descs;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t push_size = 0;
    uint32_t input_mask = 0;  // attributes read by the VS
    // c1 (compiler constants) per stage: the translator folds fixed-offset reads,
    // but indexed ones (tables by gl_VertexIndex, e.g. text quad corners) really read the
    // UBO, so it must be bound with these bytes (data + const_offset).
    std::vector<uint8_t> c1[6];
    bool uses_xfb = false;
    bool ok = false;
};
// (program, version, state without hints) -> variants per bound textures
std::unordered_map<uint64_t, std::vector<std::unique_ptr<Variant>>> g_variants;
struct PipeEntry;
std::unordered_map<uint64_t, std::shared_ptr<PipeEntry>> g_pipelines;

VkShaderStageFlagBits VkStage(Stage s) { return nvnshader::VkStageOf(s); }

uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))); }

AttribType AttribTypeOf(uint32_t nvn_format) {
    const uint32_t w = NvnFormat(nvn_format).vertex;
    switch ((w >> 27) & 7) {
        case 3: return AttribType::SInt;
        case 4: return AttribType::UInt;
        case 5: return AttribType::UScaled;
        case 6: return AttribType::SScaled;
        default: return AttribType::Float;
    }
}

// Reads a TIC/TSC descriptor from a pool.
bool ReadDesc(pla_ctx* ctx, uint64_t pool_cpu, uint32_t count, uint32_t id, uint32_t out[8]) {
    if (!pool_cpu || id >= count) return false;
    return g_guest.Read(ctx, pool_cpu + (uint64_t)id * 32, out, 32);
}

// Texture/image handle bound at (cbuf, offset) for stage `s`.
bool BoundHandle(pla_ctx* ctx, const NvnState& st, uint32_t s, uint32_t cbuf, uint32_t off, uint32_t* handle) {
    if (cbuf == 2) {
        if (off >= 0x120 && (off - 0x120) / 8 < 8) {
            *handle = (uint32_t)st.img[s][(off - 0x120) / 8];
            return true;
        }
        if (off >= 0x20 && (off - 0x20) / 8 < 32) {
            *handle = (uint32_t)st.tex[s][(off - 0x20) / 8];
            return true;
        }
        return false;
    }
    if (cbuf >= 3 && cbuf - 3 < 18) {
        const VertexBinding& b = st.ubo[s][cbuf - 3];
        if (!b.addr || off + 4 > std::max<uint64_t>(b.size, 4)) return false;
        const uint64_t cpu = g_reg.GpuToCpu(b.addr + off);
        if (!cpu) return false;
        *handle = g_guest.Get<uint32_t>(ctx, cpu);
        return true;
    }
    return false;
}

TextureType TicTextureType(const uint32_t t[8]) {
    const bool normalized = (t[5] >> 31) & 1;
    switch ((t[4] >> 23) & 0xF) {
        case 0: return TextureType::Color1D;
        case 1: case 7: return normalized ? TextureType::Color2D : TextureType::Color2DRect;
        case 2: return TextureType::Color3D;
        case 3: return TextureType::ColorCube;
        case 4: return TextureType::ColorArray1D;
        case 5: return TextureType::ColorArray2D;
        case 6: return TextureType::Buffer;
        case 8: return TextureType::ColorArrayCube;
        default: return TextureType::Color2D;
    }
}

bool TicIsInteger(const uint32_t t[8]) {
    const uint32_t r = (t[0] >> 7) & 7;  // ComponentType: 3 SINT, 4 UINT
    return r == 3 || r == 4;
}

Variant* BuildVariant(pla_ctx* ctx, const NvnState& st, const ProgramSrc& src, const TranslationState& base,
                      const TextureResolver& res_vs, const TextureResolver& res_fs) {
    auto v = std::make_unique<Variant>();
    TranslationState ts = base;
    for (const StageSrc& ss : src.stages) {
        {
            const uint32_t si = std::min<uint32_t>((uint32_t)ss.hdr.stage, 5);
            if (ss.hdr.const_size && (uint64_t)ss.hdr.const_offset + ss.hdr.const_size <= ss.data.size())
                v->c1[si].assign(ss.data.begin() + ss.hdr.const_offset,
                                 ss.data.begin() + ss.hdr.const_offset + ss.hdr.const_size);
        }
        ShaderBinary b{ss.control.data(), ss.control.size(), ss.data.data(), ss.data.size()};
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const TextureResolver& res = ss.hdr.stage == Stage::Fragment ? res_fs : res_vs;
        auto m = g_tr->TranslateBound(b, ts, res, &err);
        {
            std::lock_guard<std::mutex> lk(g_ps_mu);
            g_ps.translate_ms += Ms(t0);
        }
        if (!m) {
            NVK_WARN("translation failed (program %llx, stage %d): %s", (unsigned long long)st.program,
                     (int)ss.hdr.stage, err.c_str());
            v->ok = false;
            std::lock_guard<std::mutex> lk(g_ps_mu);
            ++g_ps.variant_fail;
            return nullptr;
        }
        if (!m->warnings.empty()) NVK_DBG("translator warnings (%llx): %s", (unsigned long long)m->code_hash, m->warnings.c_str());
        ts.previous_stage_stores.assign(m->stores.begin(), m->stores.end());
        ts.texture_hints.clear();
        v->mods.push_back(m);
        v->stages.push_back(m->stage);
    }
    // Convention shared with prewarming (vk_program_layout.h): one set, stacked stages.
    std::vector<ShaderModule> copies;
    copies.reserve(v->mods.size());
    for (auto& m : v->mods) copies.push_back(*m);
    std::vector<ShaderModule*> ptrs;
    for (auto& c : copies) ptrs.push_back(&c);
    const nvnshader::ProgramLayout pl = nvnshader::LinkProgramStages(ptrs);
    uint32_t base_binding = 0;
    for (const ShaderModule& m : copies) {
        uint32_t max_b = 0;
        for (const Descriptor& d : m.descriptors) {
            v->descs.push_back({d, m.stage, d.binding + base_binding, nvnshader::VkDescriptorTypeOf(d.type)});
            max_b = std::max(max_b, d.binding + 1);
        }
        base_binding += max_b;
        if (m.stage == Stage::Vertex) {
            for (int a = 0; a < 32; ++a)
                if (((m.input_mask[a / 16] >> ((a % 16) * 4)) & 0xF) != 0) v->input_mask |= 1u << a;
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = m.spirv.size() * 4;
        ci.pCode = m.spirv.data();
        VkShaderModule smod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(g_vk.dev, &ci, nullptr, &smod) != VK_SUCCESS) {
            std::lock_guard<std::mutex> lk(g_ps_mu);
            ++g_ps.variant_fail;
            return nullptr;
        }
        v->vk.push_back(smod);
        // Diagnostics: NVNVK_DUMP_SPV=<folder> writes each module as <VkShaderModule handle>-<hash>.spv
        // (the handle is the one quoted by validation / GPU-AV messages).
        static const char* dump = std::getenv("NVNVK_DUMP_SPV");
        if (dump && *dump) {
            char name[512];
            std::snprintf(name, sizeof name, "%s/p%llx-%llx-%016llx-e%d.spv", dump, (unsigned long long)st.program,
                          (unsigned long long)(uint64_t)smod, (unsigned long long)m.code_hash, (int)m.stage);
            if (FILE* f = std::fopen(name, "wb")) {
                std::fwrite(m.spirv.data(), 4, m.spirv.size(), f);
                std::fclose(f);
            }
        }
    }
    v->push_size = pl.push_constant_size;
    v->uses_xfb = base.xfb_active;
    if (nvnshader::CreateProgramLayout(g_vk.dev, pl, &v->dsl, &v->layout) != VK_SUCCESS) {
        std::lock_guard<std::mutex> lk(g_ps_mu);
        ++g_ps.variant_fail;
        return nullptr;
    }
    v->ok = true;
    {
        std::lock_guard<std::mutex> lk(g_ps_mu);
        ++g_ps.variants;
    }
    (void)ctx;
    Variant* raw = v.release();
    return raw;
}

// ---------------------------------------------------------------- buffer uploads (per submission)
struct UploadRef {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint64_t avail = ~0ull;  // valid bytes from off (unified memory: what is left of the pool)
};
std::unordered_map<uint64_t, UploadRef> g_uploads;  // (addr ^ size<<40) -> copy in the ring
Buffer g_zero;                                        // 64 KiB of zeros (attributes without a buffer)
VkBufferView g_null_bview = VK_NULL_HANDLE;            // texel buffer without an address: zeros

// Allocates in the frame ring; if full, a single-use buffer (freed with the slot).
void AllocUpload(uint64_t size, VkDeviceSize align, VkBuffer* buf, VkDeviceSize* off, uint8_t** ptr) {
    const VkDeviceSize o = g_vk.UploadAlloc((size + 3) & ~3ull, align, ptr);
    if (o != VK_WHOLE_SIZE) {
        *buf = g_vk.frames[g_vk.cur].upload.buf;
        *off = o;
        return;
    }
    Buffer b = g_vk.CreateBuffer(std::max<uint64_t>((size + 3) & ~3ull, 16),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                                 true);
    g_vk.frames[g_vk.cur].dead_buffers.push_back(b);
    *buf = b.buf;
    *off = 0;
    *ptr = b.map;
}

void DedupReport();  // state_dedup statistics (defined next to EmitDraw)
// Breakdown of the submission CPU cost (accumulated ns; reported in PipelineReport).
struct CpuNs {
    std::atomic<uint64_t> upload{0}, variant{0}, pipeline{0}, writes{0}, draws{0}, n_draws{0};
    std::atomic<uint64_t> w_desc{0}, w_view{0}, w_smp{0};
} g_cpu;
// Only with cpu_stats (FPS-1): two clock reads per scope and texture were a measurable part of the submit cost.
struct ScopedNs {
    std::atomic<uint64_t>* a;
    std::chrono::steady_clock::time_point t0{};
    explicit ScopedNs(std::atomic<uint64_t>& x) : a(g_cfg.cpu_stats ? &x : nullptr) {
        if (a) t0 = std::chrono::steady_clock::now();
    }
    ~ScopedNs() {
        if (a)
            a->fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                         std::memory_order_relaxed);
    }
};

// Fingerprint of [gpu, gpu+size) in guest memory (to validate transform feedback buffers).
uint64_t GuestRangeHash(pla_ctx* ctx, uint64_t gpu, uint64_t size) {
    uint64_t h = 0xcbf29ce484222325ull ^ size;
    auto mix = [&h](const uint8_t* p, uint64_t n) {
        uint64_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            h = (h ^ v) * 0x100000001b3ull;
        }
        for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    };
    uint64_t avail = 0;
    if (!g_capture.active()) {
        if (const uint8_t* p = g_reg.GpuToHost(gpu, &avail)) {
            const uint64_t n = std::min(size, avail);
            if (n <= 2048) {
                mix(p, n);
            } else {
                // X2 (sprint 6): sample 256 chunks of 8 bytes (previously the whole buffer every frame: +0.3 ms per
                // frame in the field). Reusing the address for other data changes almost the whole range.
                const uint64_t step = ((n / 256) + 7) & ~7ull;
                for (uint64_t o = 0; o + 8 <= n; o += step) mix(p + o, 8);
                mix(p + n - 8, 8);
            }
            return h;
        }
    }
    const uint64_t cpu = g_reg.GpuToCpu(gpu, &avail);
    if (!cpu) return h;
    std::vector<uint8_t> tmp((size_t)std::min(size, avail));
    g_guest.Read(ctx, cpu, tmp.data(), tmp.size());
    mix(tmp.data(), tmp.size());
    return h;
}

// Is the XFB buffer still valid? (the CPU has not rewritten its memory since the last XFB write)
std::atomic<uint64_t> g_xfb_hash_ns{0};
bool XfbStillValid(pla_ctx* ctx, const VkCtx::GpuBuf* gb) {
    if (!g_cfg.xfb_validate) return true;
    ScopedPhase _ph(g_xfb_hash_ns);
    const uint64_t f = g_stats.frames.load(std::memory_order_relaxed);
    if (gb->checked_frame == f) return true;
    if (GuestRangeHash(ctx, gb->gpu, gb->size) != gb->guest_hash) return false;
    gb->checked_frame = f;
    return true;
}

// XFB buffer containing the start of [gpu, gpu+size) but not the end (e.g. the particle vertex buffer
// bound with the size of the whole pool, and the XFB writing only part of it).
uint64_t g_xfb_partial = 0;
const VkCtx::GpuBuf* PartialGpuBuf(uint64_t gpu, uint64_t size) {
    auto it = g_vk.gpu_bufs.upper_bound(gpu);
    if (it == g_vk.gpu_bufs.begin()) return nullptr;
    --it;
    const VkCtx::GpuBuf& g = it->second;
    // Only if it overruns the end slightly (Q4: 16 B; a VB bound a bit longer than what the compute writes). A large overrun
    // is another mesh starting there (TRIANGLES, legs in Q5): read from guest memory.
    if (gpu >= g.gpu && gpu < g.gpu + g.size && gpu + size > g.gpu + g.size) {
        if (gpu + size - (g.gpu + g.size) <= g_cfg.xfb_partial_max) return &g;
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 30)
            NVK_INFO("VB %llx+%llx starts in own buffer %llx+%llx and overruns it by %llu B: reading from the guest", (unsigned long long)gpu,
                     (unsigned long long)size, (unsigned long long)g.gpu, (unsigned long long)g.size,
                     (unsigned long long)(gpu + size - g.gpu - g.size));
    }
    return nullptr;
}

std::vector<uint64_t> g_wb_pending;  // own buffers written via SSBO in the current submission
uint64_t g_wb_bytes = 0, g_wb_skipped = 0;
std::set<uint64_t> g_no_persist;  // SSBOs the CPU also writes: not persisted (ssbo_cpu_blacklist)
std::atomic<uint64_t> g_vbib_res[4];
// vbib_trim: max index of each index buffer copied at record time (per submission), to copy only the vertices the draw uses.
std::unordered_map<uint64_t, uint32_t> g_max_index;
// Record-time VB/IB copies (vbib_snapshot) per base address in this submission: the largest one made. Draws sharing a large VB
// request different sizes (vbib_trim), and with the (address, size) cache it was copied once per draw.
std::unordered_map<uint64_t, std::pair<UploadRef, uint64_t>> g_snap;
thread_local bool t_nosnap = false;
uint64_t g_trim_saved = 0, g_trim_copied = 0;
bool MaxIndex(pla_ctx* ctx, uint64_t addr, uint32_t count, uint32_t ib, uint32_t* out) {
    if (!addr || !count || (ib != 1 && ib != 2 && ib != 4)) return false;
    const uint64_t key = addr ^ ((uint64_t)count << 36) ^ ((uint64_t)ib << 62);
    auto it = g_max_index.find(key);
    if (it != g_max_index.end()) {
        *out = it->second;
        return true;
    }
    const uint64_t bytes = (uint64_t)count * ib;
    uint64_t avail = 0;
    const uint8_t* p = g_reg.GpuToHost(addr, &avail);
    std::vector<uint8_t> tmp;
    if (!p || avail < bytes) {
        const uint64_t cpu = g_reg.GpuToCpu(addr, &avail);
        if (!cpu || avail < bytes) return false;
        tmp.resize(bytes);
        if (!g_guest.Read(ctx, cpu, tmp.data(), bytes)) return false;
        p = tmp.data();
    }
    uint32_t mx = 0;
    if (ib == 1) {
        for (uint32_t i = 0; i < count; ++i)
            if (p[i] != 0xFF && p[i] > mx) mx = p[i];
    } else if (ib == 2) {
        const uint16_t* q = reinterpret_cast<const uint16_t*>(p);
        for (uint32_t i = 0; i < count; ++i)
            if (q[i] != 0xFFFF && q[i] > mx) mx = q[i];
    } else {
        const uint32_t* q = reinterpret_cast<const uint32_t*>(p);
        for (uint32_t i = 0; i < count; ++i)
            if (q[i] != 0xFFFFFFFFu && q[i] > mx) mx = q[i];
    }
    g_max_index[key] = mx;
    *out = mx;
    return true;
}  // vbib_snapshot diagnostics: VB/IB bindings by residency (0 ?, 1 VRAM, 2 GPU, 3 CPU)
uint32_t g_read_kind = 8;  // usage of the next Upload for flag diagnostics (8 VB, 16 indices, 32 UBO, 64 SSBO)

// ---- snap_cache (FPS-1) ---------------------------------------------------------------------------------------------
// vbib_snapshot used to copy every CPU-only VB/IB into the frame's upload ring on every submission (~12 MB/frame in the
// town; on 4 E-cores the copies were ~18% of the submit thread). The copies now live in a persistent host-visible arena,
// keyed by GPU address, and are reused while the source bytes hash the same: a full XXH64 of the range (reading is
// cheaper than copying into write-combined memory, and nothing is assumed about when the game writes).
// Arena lifetime: positions grow monotonically (VkCtx::arena_head); an entry is reused only while it is within half the
// arena of the head (older entries are copied again), and a new allocation is refused when the bytes allocated since the
// oldest frame slot in flight began would exceed half the arena. So an overwritten region was last referenced by a frame
// whose fence has already been waited (FrameSlot::arena_head_at_begin).
inline uint64_t Rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
uint64_t Xxh64(const uint8_t* p, uint64_t len, uint64_t seed) {
    constexpr uint64_t P1 = 11400714785074694791ull, P2 = 14029467366897019727ull, P3 = 1609587929392839161ull,
                       P4 = 9650029242287828579ull, P5 = 2870177450012600261ull;
    auto rd64 = [](const uint8_t* q) { uint64_t v; std::memcpy(&v, q, 8); return v; };
    auto rd32 = [](const uint8_t* q) { uint32_t v; std::memcpy(&v, q, 4); return (uint64_t)v; };
    auto round = [](uint64_t acc, uint64_t in) { return Rotl(acc + in * P2, 31) * P1; };
    auto merge = [&](uint64_t acc, uint64_t v) { return (acc ^ round(0, v)) * P1 + P4; };
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* lim = end - 32;
        do {
            v1 = round(v1, rd64(p));
            v2 = round(v2, rd64(p + 8));
            v3 = round(v3, rd64(p + 16));
            v4 = round(v4, rd64(p + 24));
            p += 32;
        } while (p <= lim);
        h = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
        h = merge(merge(merge(merge(h, v1), v2), v3), v4);
    } else {
        h = seed + P5;
    }
    h += len;
    for (; p + 8 <= end; p += 8) h = Rotl(h ^ round(0, rd64(p)), 27) * P1 + P4;
    if (p + 4 <= end) {
        h = Rotl(h ^ (rd32(p) * P1), 23) * P2 + P3;
        p += 4;
    }
    for (; p < end; ++p) h = Rotl(h ^ (*p * P5), 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}
struct SnapEntry {
    uint64_t size = 0, hash = 0, pos = 0, last_frame = 0;
    uint64_t dev_pos = 0;
    bool dev = false, promote = false;  // valid copy in the device arena / copy requested for the next submission
};
Buffer g_arena_dev;
bool g_snap_vram = false;
std::vector<uint64_t> g_promote;  // gpu keys of g_snapc
uint64_t g_snapc_dev_hits = 0, g_snapc_promoted = 0, g_snapc_dev_bytes = 0;
// Host-read accounting (host_read_stats): bytes bound per kind (0 VB, 1 IB, 2 UBO, 3 SSBO) and place (0 unknown,
// 1 frame ring, 2 snapshot arena (host), 3 imported game memory, 4 VRAM). Bound ranges: a range bound by N draws counts N times.
uint64_t g_hr[4][5] = {};
thread_local int t_place = 0;
std::unordered_map<uint64_t, SnapEntry> g_snapc;
Buffer g_arena;
uint64_t g_snapc_hits = 0, g_snapc_miss = 0, g_snapc_full = 0, g_snapc_hit_bytes = 0, g_snapc_copy_bytes = 0;
bool SnapCached(uint64_t gpu, uint64_t size, struct UploadRef* out);
UploadRef UploadImpl(pla_ctx* ctx, uint64_t gpu, uint64_t size, VkDeviceSize align);
UploadRef Upload(pla_ctx* ctx, uint64_t gpu, uint64_t size, VkDeviceSize align) {
    const bool outer = !t_nosnap;
    t_place = 0;
    const UploadRef r = UploadImpl(ctx, gpu, size, align);
    if (outer && g_cfg.host_read_stats) {
        const uint32_t k = g_read_kind & ~0x1000u;
        const int kind = k == 8 ? 0 : k == 16 ? 1 : k == 32 ? 2 : 3;
        g_hr[kind][t_place] += std::min<uint64_t>(size ? size : 16, r.avail);
    }
    return r;
}
UploadRef UploadImpl(pla_ctx* ctx, uint64_t gpu, uint64_t size, VkDeviceSize align) {
    ScopedNs _t(g_cpu.upload);
    if (!size) size = 16;
    size = std::min<uint64_t>(size, 64ull << 20);
    // unified_mask (diagnostics): bit 1 vertices, 2 indices, 4 UBO, 8 SSBO read, 16 SSBO written, 32 XFB.
    const uint32_t kbit = (g_read_kind & 0x1000) ? 0 : g_read_kind == 8 ? 1 : g_read_kind == 16 ? 2 : g_read_kind == 32 ? 4 : g_read_kind == 64 ? 8 : 16;
    // vbib_snapshot: vertices/indices from a pool the GPU never wrote -> copy at record time (since the CPU is the only
    // writer and can only rewrite after a fence that requires this to be recorded and executed, the copy is correct).
    if (Unified() && (g_read_kind & ~0x1000u) == 128) g_hostmem.MarkGpuWritten(gpu, size);  // SSBO / buffer image written
    const int res = (kbit == 1 || kbit == 2) && Unified() ? g_hostmem.Residency(gpu, size) : -1;
    if (res >= 0) g_vbib_res[res].fetch_add(1, std::memory_order_relaxed);
    const bool snap = res == 3 && g_cfg.vbib_snapshot;
    if (snap && !t_nosnap) {
        auto sit = g_snap.find(gpu);
        if (sit != g_snap.end() && sit->second.second >= size) {
            const VkBuffer b = sit->second.first.buf;
            t_place = b == g_arena_dev.buf ? 4 : b == g_arena.buf ? 2 : 1;
            return sit->second.first;
        }
        UploadRef r;
        if (!(g_cfg.snap_cache_mb && !g_capture.active() && SnapCached(gpu, size, &r))) {
            t_nosnap = true;
            r = Upload(ctx, gpu, size, align);
            t_nosnap = false;
        }
        g_snap[gpu] = {r, size};
        return r;
    }
    if (Unified() && (g_cfg.unified_mask & kbit) && !snap) {
        HostRef h;
        t_find_kind = g_read_kind;
        if (g_hostmem.Find(gpu, size, align, &h, true)) {
            if (h.want > h.avail) {
                // Crosses a span boundary: gathered copy (VB/IB already prepared before the render pass; UBO/SSBO are gathered here,
                // while it is not open yet).
                VkBuffer gb = VK_NULL_HANDLE;
                VkDeviceSize go = 0;
                if (t_cmd && GatherIfCrossing(t_cmd, gpu, size, g_read_kind == 128, &gb, &go)) {
                    t_place = 1;
                    UploadRef r{gb, go, h.want};
                    return r;
                }
            }
            if (g_cfg.import_verify) {
                // Is the chosen imported pool the same one the registry resolves (GpuToCpu)?
                uint64_t av = 0;
                const uint64_t rc = g_reg.GpuToCpu(gpu, &av);
                static std::atomic<int> logs{0};
                if (rc != h.cpu && logs.fetch_add(1) < 60)
                    NVK_WARN("unified memory: %llx (usage %u) -> imported pool cpu %llx, but the registry says %llx", (unsigned long long)gpu,
                             g_read_kind, (unsigned long long)h.cpu, (unsigned long long)rc);
            }
            ++g_ps.imported_binds;
            t_place = h.vram ? 4 : 3;
            UploadRef r{h.buf, h.off, h.avail};
            return r;
        }
        // Outside the imported pools (or misaligned): single-use copy, as before.
        if (g_unified_miss.fetch_add(1) < 40)
            NVK_INFO("unified memory: %llx+%llx (alignment %llu, usage %u) outside the imported pools: copying",
                     (unsigned long long)gpu, (unsigned long long)size, (unsigned long long)align, g_read_kind);
    } else if (Unified()) {
        // type outside unified_mask: copy at record time (no own buffers or imported memory)
    } else if (const VkCtx::GpuBuf* gb = g_vk.FindGpuBuffer(gpu, size)) {
        gb->flags |= g_read_kind;
        if (XfbStillValid(ctx, gb)) return {gb->b.buf, gpu - gb->gpu};  // written by the GPU (transform feedback)
        // The CPU rewrote that memory (address reused): the XFB buffer is no longer valid.
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 20)
            NVK_INFO("XFB buffer %llx (%llu KiB) dropped: the CPU rewrote its memory", (unsigned long long)gb->gpu,
                     (unsigned long long)(gb->size >> 10));
        g_vk.DropGpuBuf(gb->gpu);
    }
    if (g_use_import && !Unified()) {
        HostRef h;
        if (g_hostmem.Find(gpu, size, align, &h)) {
            ++g_ps.imported_binds;
            return {h.buf, h.off};
        }
    }
    t_place = 1;  // below: a copy in the frame ring
    const uint64_t key = gpu ^ (size << 40);
    auto it = g_uploads.find(key);
    if (it != g_uploads.end()) return it->second;
    uint64_t avail = 0;
    uint8_t* p = nullptr;
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    UploadRef r;
    if (!g_capture.active()) {
        // Fast path: contiguous pool backing, no API calls or SEH per read (U3).
        if (const uint8_t* h = g_reg.GpuToHost(gpu, &avail)) {
            AllocUpload(size, align, &dst, &off, &p);
            const uint64_t n = std::min(size, avail);
            std::memcpy(p, h, n);
            if (n < size) std::memset(p + n, 0, size - n);
            g_stats.uploads_bytes.fetch_add(size, std::memory_order_relaxed);
            r.buf = dst;
            r.off = off;
            g_uploads[key] = r;
            return r;
        }
    }
    const uint64_t cpu = g_reg.GpuToCpu(gpu, &avail);
    AllocUpload(size, align, &dst, &off, &p);
    if (cpu) {
        const uint64_t n = std::min(size, avail);
        g_guest.Read(ctx, cpu, p, n);
        if (n < size) std::memset(p + n, 0, size - n);
    } else {
        std::memset(p, 0, size);
    }
    g_stats.uploads_bytes.fetch_add(size, std::memory_order_relaxed);
    r.buf = dst;
    r.off = off;
    g_uploads[key] = r;
    return r;
}

bool SnapCached(uint64_t gpu, uint64_t size, UploadRef* out) {
    const uint64_t cap = (uint64_t)g_cfg.snap_cache_mb << 20;
    if (size > cap / 8) return false;
    uint64_t avail = 0;
    const uint8_t* h = g_reg.GpuToHost(gpu, &avail);
    if (!h || avail < size) return false;  // not contiguous in the backing: the ring path handles it
    if (!g_arena.buf) {
        g_arena = g_vk.CreateBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    true);
        if (!g_arena.buf || !g_arena.map) {
            NVK_WARN("snap_cache: could not create the %u MB arena; off", g_cfg.snap_cache_mb);
            g_cfg.snap_cache_mb = 0;
            return false;
        }
        NVK_INFO("snap_cache: %u MB host-visible arena", g_cfg.snap_cache_mb);
        g_snap_vram = g_cfg.snap_vram == "true" || (g_cfg.snap_vram == "auto" && !g_vk.integrated);
        if (g_snap_vram) {
            g_arena_dev = g_vk.CreateBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
            if (!g_arena_dev.buf) {
                NVK_WARN("snap_vram: no %u MB device-local arena; snapshots stay in host memory", g_cfg.snap_cache_mb);
                g_snap_vram = false;
            } else {
                NVK_HOST("snap_vram: unchanged vertex/index snapshots are kept in a %u MB VRAM arena", g_cfg.snap_cache_mb);
            }
        }
    }
    const uint64_t hs = Xxh64(h, size, gpu);
    const uint64_t frame = g_stats.frames.load(std::memory_order_relaxed);
    SnapEntry& e = g_snapc[gpu];
    const uint64_t head = g_vk.arena_head;
    if (e.size == size && e.hash == hs && head - e.pos <= cap / 2) {
        e.last_frame = frame;
        ++g_snapc_hits;
        g_snapc_hit_bytes += size;
        if (e.dev && g_vk.dev_head - e.dev_pos <= cap / 2) {
            out->buf = g_arena_dev.buf;
            out->off = e.dev_pos % cap;
            ++g_snapc_dev_hits;
            g_snapc_dev_bytes += size;
            t_place = 4;
            return true;
        }
        e.dev = false;
        if (g_snap_vram && !e.promote) {
            e.promote = true;
            g_promote.push_back(gpu);
        }
        out->buf = g_arena.buf;
        out->off = e.pos % cap;
        t_place = 2;
        return true;
    }
    // New copy at the head (16-aligned; never split across the end of the arena).
    uint64_t pos = (head + 15) & ~15ull;
    if (pos % cap + size > cap) pos += cap - pos % cap;
    uint64_t oldest = head;
    for (const FrameSlot& f : g_vk.frames) oldest = std::min(oldest, f.arena_head_at_begin);
    if (pos + size - oldest > cap / 2) {
        ++g_snapc_full;  // too much churn in the frames in flight: this submission copies into the ring
        return false;
    }
    std::memcpy(g_arena.map + pos % cap, h, size);
    g_vk.arena_head = pos + size;
    e.size = size;
    e.hash = hs;
    e.pos = pos;
    e.dev = false;  // content changed: the device copy (if any) is stale; promoted again on the next unchanged use
    t_place = 2;
    e.last_frame = frame;
    out->buf = g_arena.buf;
    out->off = pos % cap;
    ++g_snapc_miss;
    g_snapc_copy_bytes += size;
    g_stats.uploads_bytes.fetch_add(size, std::memory_order_relaxed);
    return true;
}

}  // namespace

void PipelineSnapPromote(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_snap_vram || g_promote.empty()) return;
    const uint64_t cap = (uint64_t)g_cfg.snap_cache_mb << 20;
    uint64_t oldest = g_vk.dev_head;
    for (const FrameSlot& f : g_vk.frames) oldest = std::min(oldest, f.dev_head_at_begin);
    std::vector<VkBufferCopy> cps;
    cps.reserve(g_promote.size());
    for (uint64_t key : g_promote) {
        auto it = g_snapc.find(key);
        if (it == g_snapc.end()) continue;
        SnapEntry& e = it->second;
        e.promote = false;
        if (e.dev || g_vk.arena_head - e.pos > cap / 2) continue;  // source too old: may already be overwritten
        uint64_t pos = (g_vk.dev_head + 15) & ~15ull;
        if (pos % cap + e.size > cap) pos += cap - pos % cap;
        if (pos + e.size - oldest > cap / 2) continue;  // device arena busy with frames in flight
        cps.push_back({e.pos % cap, pos % cap, e.size});
        g_vk.dev_head = pos + e.size;
        e.dev_pos = pos;
        e.dev = true;
        ++g_snapc_promoted;
    }
    g_promote.clear();
    if (cps.empty()) return;
    vkCmdCopyBuffer(cmd, g_arena.buf, g_arena_dev.buf, (uint32_t)cps.size(), cps.data());
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

namespace {

UploadRef UploadBytes(const void* data, size_t size, VkDeviceSize align) {
    uint8_t* p = nullptr;
    UploadRef r;
    AllocUpload(size, align, &r.buf, &r.off, &p);
    std::memcpy(p, data, size);
    return r;
}

// ---------------------------------------------------------------- pipeline
// Everything needed to create a graphics VkPipeline, copied from the draw state, so it can be
// created on another thread (sprint 5: background pipelines) while submission continues.
struct PipeDesc {
    const Variant* v = nullptr;
    uint32_t vattrib[16] = {};
    uint64_t vstream[16] = {};
    uint32_t polygon = 0, depth_clamp = 0, multisample[1] = {}, color = 0, channel_mask = 0;
    uint32_t blend[8][2] = {};
    VkFormat cf[8] = {};
    uint32_t ncolor = 0;
    VkFormat df = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t depth_mode = 0;
};

struct PipeEntry {
    std::atomic<int> state{0};  // 0 queued/being created, 1 ready, 2 failed
    VkPipeline p = VK_NULL_HANDLE;
    PipeDesc d;
};

// Background creation counters (touched by the threads: atomic).
struct AsyncStats {
    std::atomic<uint64_t> created{0}, create_ns{0}, waited{0}, wait_ns{0}, skipped{0}, max_ns{0};
} g_as;
std::atomic<int64_t> g_budget_us{4000};  // wait budget left in this frame (PipelineBeginFrame refills it)

// A draw's wait: at most pipeline_wait_ms and whatever is left of the frame budget.
double WaitMs() {
    const double left = g_budget_us.load(std::memory_order_relaxed) / 1000.0;
    // With the budget exhausted we still wait pipeline_min_wait_ms: with warm caches a pipeline
    // takes <0.5 ms, so the draw is not skipped (s5c field run: 600 draws skipped during loading without this minimum).
    return std::min(g_cfg.pipeline_wait_ms, std::max(g_cfg.pipeline_min_wait_ms, left));
}
void Spend(std::chrono::steady_clock::time_point t0) {
    g_budget_us.fetch_sub(
        (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
}
std::atomic<uint64_t> g_var_async{0}, g_var_skipped{0};  // background variants

VkPipeline CreateFromDesc(const PipeDesc& d) {
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (size_t i = 0; i < d.v->vk.size(); ++i) {
        VkPipelineShaderStageCreateInfo s{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        s.stage = VkStage(d.v->stages[i]);
        s.module = d.v->vk[i];
        s.pName = "main";
        stages.push_back(s);
    }
    // Vertex input: attribute i -> location i; stream s -> binding s. Attributes the shader reads
    // but that are disabled -> binding 16 (zero buffer, stride 0).
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> divs;
    uint32_t used_streams = 0;
    bool zero_binding = false;
    for (uint32_t i = 0; i < 32; ++i) {
        if (!(d.v->input_mask >> i & 1)) continue;
        const uint32_t a = i < 16 ? d.vattrib[i] : 0;
        VkVertexInputAttributeDescription ad{};
        ad.location = i;
        const VkFormat vf = (a & 0x40) ? VertexFormat((a >> 21) & 0x3FF) : VK_FORMAT_UNDEFINED;
        if ((a & 0x40) && vf != VK_FORMAT_UNDEFINED) {
            ad.binding = a & 0x1F;
            ad.format = vf;
            ad.offset = (a >> 7) & 0x3FFF;
            used_streams |= 1u << ad.binding;
        } else {
            ad.binding = 16;
            ad.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            ad.offset = 0;
            zero_binding = true;
        }
        attrs.push_back(ad);
    }
    for (uint32_t s = 0; s < 16; ++s) {
        if (!(used_streams >> s & 1)) continue;
        const uint32_t divisor = (uint32_t)(d.vstream[s] >> 32);
        VkVertexInputBindingDescription b{s, (uint32_t)(d.vstream[s] & 0xFFFFFFFF),
                                          divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        binds.push_back(b);
        if (divisor > 1) divs.push_back({s, divisor});
    }
    if (zero_binding) binds.push_back({16, 0, VK_VERTEX_INPUT_RATE_VERTEX});
    VkPipelineVertexInputDivisorStateCreateInfoEXT div{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT};
    div.vertexBindingDivisorCount = (uint32_t)divs.size();
    div.pVertexBindingDivisors = divs.data();
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)binds.size();
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();
    if (!divs.empty()) vi.pNext = &div;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    if (g_caps.native_ndc && g_vk.has_depth_clip_control && d.depth_mode == 0) {
        dcc.negativeOneToOne = VK_TRUE;
        vp.pNext = &dcc;
    }
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    const uint32_t pm = (d.polygon >> 3) & 3;  // NVN: 0 POINT, 1 LINE, 2 FILL
    rs.polygonMode = pm == 0 ? VK_POLYGON_MODE_POINT : pm == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.depthClampEnable = d.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    ms.alphaToCoverageEnable = (d.multisample[0] >> 1) & 1;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[8]{};
    const uint32_t enables = (d.color >> 8) & 0xFF;
    for (uint32_t i = 0; i < d.ncolor; ++i) {
        VkPipelineColorBlendAttachmentState& a = cb[i];
        const uint32_t w0 = d.blend[i][0], w1 = d.blend[i][1];
        a.blendEnable = (enables >> i) & 1;
        a.colorBlendOp = BlendOp((w0 >> 24) & 7);
        a.alphaBlendOp = BlendOp((w0 >> 28) & 7);
        a.srcColorBlendFactor = BlendFactor(w1 & 0x7F);
        a.dstColorBlendFactor = BlendFactor((w1 >> 8) & 0x7F);
        a.srcAlphaBlendFactor = BlendFactor((w1 >> 16) & 0x7F);
        a.dstAlphaBlendFactor = BlendFactor((w1 >> 24) & 0x7F);
        a.colorWriteMask = (d.channel_mask >> (i * 4)) & 0xF;
        if (d.cf[i] == VK_FORMAT_UNDEFINED) {
            a.colorWriteMask = 0;
            a.blendEnable = VK_FALSE;
            continue;
        }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, d.cf[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) a.blendEnable = VK_FALSE;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    const uint32_t logic = (d.color >> 16) & 0xFF;
    cbs.logicOpEnable = logic != 3 && logic < 16;
    cbs.logicOp = (VkLogicOp)(logic & 0xF);
    cbs.attachmentCount = d.ncolor;
    cbs.pAttachments = cb;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = d.ncolor;
    rci.pColorAttachmentFormats = d.cf;
    rci.depthAttachmentFormat = d.df;
    rci.stencilAttachmentFormat = IsStencilFormat(d.df) ? d.df : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rci;
    pci.stageCount = (uint32_t)stages.size();
    pci.pStages = stages.data();
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dy;
    pci.layout = d.v->layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_pcache, 1, &pci, nullptr, &p);
    if (r != VK_SUCCESS) {
        NVK_WARN("vkCreateGraphicsPipelines = %d", (int)r);
        return VK_NULL_HANDLE;
    }
    return p;
}

// Pipeline creation threads.
class PipeWorkers {
public:
    void Start(int n) {
        for (int i = 0; i < n; ++i) th_.emplace_back([this] {
        SetThreadDescription(GetCurrentThread(), L"nvnvk pipelines");
        Loop();
    });
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        for (auto& t : th_)
            if (t.joinable()) t.join();
        th_.clear();
        quit_ = false;
    }
    bool running() const { return !th_.empty(); }
    void Push(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.push_back(std::move(task));
        }
        cv_.notify_one();
    }
    // Marks a job as finished (state != 0) and wakes up whoever waits for it.
    void Finish(std::atomic<int>& state, int value) {
        {
            std::lock_guard<std::mutex> lk(done_mu_);
            state.store(value, std::memory_order_release);
        }
        done_cv_.notify_all();
    }
    // Waits for state != 0 for at most `ms` (ms < 0: no limit). true if it finished.
    bool Wait(std::atomic<int>& state, double ms) {
        if (state.load(std::memory_order_acquire)) return true;
        std::unique_lock<std::mutex> lk(done_mu_);
        auto pred = [&] { return state.load(std::memory_order_acquire) != 0; };
        if (ms < 0) {
            done_cv_.wait(lk, pred);
            return true;
        }
        return done_cv_.wait_for(lk, std::chrono::duration<double, std::milli>(ms), pred);
    }

private:
    void Loop() {
        // Normal priority: with low priority and a saturated CPU (game threads), a thread holding a driver
        // lock while creating a pipeline can be starved and block submission (priority inversion).
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return quit_ || !q_.empty(); });
                if (q_.empty()) return;  // quit_ with an empty queue
                task = std::move(q_.front());
                q_.pop_front();
            }
            task();
        }
    }
    std::vector<std::thread> th_;
    std::mutex mu_, done_mu_;
    std::condition_variable cv_, done_cv_;
    std::deque<std::function<void()>> q_;
    bool quit_ = false;
} g_pw;

void CompilePipeJob(const std::shared_ptr<PipeEntry>& e) {
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline p = CreateFromDesc(e->d);
    const uint64_t ns =
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    g_as.create_ns.fetch_add(ns, std::memory_order_relaxed);
    g_as.created.fetch_add(1, std::memory_order_relaxed);
    uint64_t m = g_as.max_ns.load();
    while (ns > m && !g_as.max_ns.compare_exchange_weak(m, ns)) {
    }
    e->p = p;
    g_pw.Finish(e->state, p ? 1 : 2);
}

VkPipeline GetPipeline(const Variant* v, const NvnState& st, const VkFormat* cf, uint32_t ncolor, VkFormat df,
                       VkSampleCountFlagBits samples, VkPrimitiveTopology topo) {
    uint64_t k = Mix(0, (uint64_t)(uintptr_t)v);
    for (uint32_t i = 0; i < 16; ++i) k = Mix(k, (v->input_mask >> i & 1) ? st.vattrib[i] : 0);
    for (uint32_t i = 0; i < 16; ++i) k = Mix(k, st.vstream[i] >> 32);  // divisor
    k = Mix(k, topo);
    k = Mix(k, st.color);
    k = Mix(k, st.channel_mask);
    for (uint32_t i = 0; i < ncolor; ++i) {
        k = Mix(k, cf[i]);
        k = Mix(k, ((uint64_t)st.blend[i][0] << 32) | st.blend[i][1]);
    }
    k = Mix(k, df);
    k = Mix(k, samples);
    k = Mix(k, st.polygon & ~0x7u);  // polygon mode/offset enables (cull and front face are dynamic)
    k = Mix(k, st.multisample[0] & 0x6);
    k = Mix(k, st.depth_clamp);
    k = Mix(k, g_modes.depth_mode);
    std::shared_ptr<PipeEntry>& slot = g_pipelines[k];
    if (slot) {
        const int s = slot->state.load(std::memory_order_acquire);
        if (s == 1) return slot->p;
        if (s == 2) return VK_NULL_HANDLE;
    } else {
        slot = std::make_shared<PipeEntry>();
        PipeDesc& d = slot->d;
        d.v = v;
        std::memcpy(d.vattrib, st.vattrib, sizeof d.vattrib);
        std::memcpy(d.vstream, st.vstream, sizeof d.vstream);
        d.polygon = st.polygon;
        d.depth_clamp = st.depth_clamp;
        d.multisample[0] = st.multisample[0];
        d.color = st.color;
        d.channel_mask = st.channel_mask;
        std::memcpy(d.blend, st.blend, sizeof d.blend);
        d.ncolor = std::min<uint32_t>(ncolor, 8);
        for (uint32_t i = 0; i < d.ncolor; ++i) d.cf[i] = cf[i];
        d.df = df;
        d.samples = samples;
        d.topo = topo;
        d.depth_mode = g_modes.depth_mode;
        ++g_ps.pipelines;
        g_stats.pipelines.fetch_add(1, std::memory_order_relaxed);
        if (!g_pw.running()) {
            const auto t0 = std::chrono::steady_clock::now();
            slot->p = CreateFromDesc(d);
            g_ps.pipeline_ms += Ms(t0);
            if (!slot->p) ++g_ps.pipe_fail;
            slot->state.store(slot->p ? 1 : 2, std::memory_order_release);
            return slot->p;
        }
        g_pw.Push([e = slot] { CompilePipeJob(e); });
    }
    // Queued or being created: transform feedback draws always wait (they write data others
    // read later); the rest wait at most pipeline_wait_ms, otherwise they are skipped this frame.
    const auto t0 = std::chrono::steady_clock::now();
    const bool done = g_pw.Wait(slot->state, v->uses_xfb ? -1.0 : WaitMs());
    Spend(t0);
    g_as.wait_ns.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
        std::memory_order_relaxed);
    g_as.waited.fetch_add(1, std::memory_order_relaxed);
    if (!done) {
        g_as.skipped.fetch_add(1, std::memory_order_relaxed);
        return VK_NULL_HANDLE;
    }
    if (slot->state.load(std::memory_order_acquire) == 2) {
        ++g_ps.pipe_fail;
        return VK_NULL_HANDLE;
    }
    return slot->p;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> v;
    std::ifstream f(path, std::ios::binary);
    if (!f) return v;
    f.seekg(0, std::ios::end);
    v.resize((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), (std::streamsize)v.size());
    return v;
}

// ---------------------------------------------------------------- descriptors (push)
struct Writes {
    std::vector<VkWriteDescriptorSet> w;
    std::vector<VkDescriptorBufferInfo> b;
    std::vector<VkDescriptorImageInfo> i;
    std::vector<VkBufferView> tv;  // texel buffers
    std::vector<size_t> first;  // per write: index into b, i or tv
    // Reused between draws (FPS-1: four vectors allocated and freed per draw showed up as ~12% of the submit thread).
    void Clear() {
        w.clear();
        b.clear();
        i.clear();
        tv.clear();
        first.clear();
    }
    void Finish() {
        for (size_t k = 0; k < w.size(); ++k) {
            if (w[k].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w[k].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                w[k].pBufferInfo = &b[first[k]];
            else if (w[k].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                     w[k].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                w[k].pTexelBufferView = &tv[first[k]];
            else
                w[k].pImageInfo = &i[first[k]];
        }
    }
};

const VkImageViewType kViewTypes[] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D,
                                      VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE,
                                      VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D};
// c0 bases for SSBOs per stage (VS, FS, GS, TCS, TES, CS). In compute, c0[0x210 + 0x10*i] are
// the UBOs (NVN does not use hardware cbufs in compute) and c0[0x310 + 0x10*i] the SSBOs.
const uint32_t kSsboBase[6] = {0x110, 0x510, 0x410, 0x210, 0x310, 0x310};
constexpr uint32_t kComputeUboBase = 0x210;

// Game buffer the shader sees at c0[off] (SSBO; in compute also UBO).
const VertexBinding* C0Buffer(const NvnState& st, uint32_t s, uint32_t off) {
    if (s == 5 && off >= kComputeUboBase && off < kSsboBase[5]) {
        const uint32_t i = (off - kComputeUboBase) / 0x10;
        return i < 18 ? &st.ubo[5][i] : nullptr;
    }
    if (off < kSsboBase[s]) return nullptr;
    const uint32_t i = (off - kSsboBase[s]) / 0x10;
    return i < 16 ? &st.ssbo[s][i] : nullptr;
}

// "trace_rt" diagnostics: logs the draws/dispatches that write the given GPU address (as a
// render target or storage image) and those that sample it, with all their bound textures.
thread_local bool t_trace = false;
std::atomic<int> g_trace_lines{0};
bool TraceOn() {
    if (g_cfg.trace_gpus.empty() || g_trace_lines.load(std::memory_order_relaxed) >= g_cfg.trace_lines) return false;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cfg.t0).count() >= g_cfg.trace_sec;
}
void TraceLine(const char* fmt, ...) {
    if (g_trace_lines.fetch_add(1) >= g_cfg.trace_lines) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    NVK_INFO("trace f%llu: %s", (unsigned long long)g_stats.frames.load(std::memory_order_relaxed), buf);
}

// Own (persistent) buffer for [addr, addr+size) written by the GPU (XFB or writable SSBO). If new, it is
// initialized from guest memory and, on top, with the contents of the buffers it replaces (sprint 6). The
// copies go outside the render pass (OnTransferBegin closes it).
VkCtx::GpuBuf* EnsureGpuBuf(pla_ctx* ctx, VkCommandBuffer cmd, uint64_t addr, uint64_t size) {
    // If an own buffer already contains it (e.g. a gpubuf_cover union), write into it at an offset: a smaller
    // new buffer inside would leave the union with stale data (undrawn legs, sprint 6).
    if (const VkCtx::GpuBuf* c = g_vk.FindGpuBuffer(addr, size)) return const_cast<VkCtx::GpuBuf*>(c);
    bool fresh = false;
    std::vector<VkCtx::Moved> moved;
    VkCtx::GpuBuf* gb = g_vk.GpuBuffer(addr, size, &fresh, &moved);
    if (!gb || !fresh || !g_cfg.xfb_init) return gb;
    OnTransferBegin();
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    {
        VkBuffer ub = VK_NULL_HANDLE;
        VkDeviceSize uo = 0;
        uint8_t* p = nullptr;
        AllocUpload(gb->size, 16, &ub, &uo, &p);
        uint64_t avail = 0;
        const uint8_t* h = g_capture.active() ? nullptr : g_reg.GpuToHost(gb->gpu, &avail);
        if (h) {
            std::memcpy(p, h, (size_t)std::min(gb->size, avail));
            if (avail < gb->size) std::memset(p + avail, 0, (size_t)(gb->size - avail));
        } else {
            std::memset(p, 0, (size_t)gb->size);
            if (const uint64_t cpu = g_reg.GpuToCpu(gb->gpu, &avail)) g_guest.Read(ctx, cpu, p, (size_t)std::min(gb->size, avail));
        }
        const VkBufferCopy c{uo, 0, gb->size};
        vkCmdCopyBuffer(cmd, ub, gb->b.buf, 1, &c);
        if (g_cfg.gpubuf_sync) gb->shadow.assign(p, p + gb->size);  // also without contiguous backing (read with Read)
    }
    for (const VkCtx::Moved& m : moved) {
        if (m.gpu < gb->gpu || m.gpu >= gb->gpu + gb->size) continue;
        const VkBufferCopy c{0, m.gpu - gb->gpu, std::min<uint64_t>(m.size, gb->gpu + gb->size - m.gpu)};
        vkCmdCopyBuffer(cmd, m.buf, gb->b.buf, 1, &c);
    }
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    ++g_xfb_inits;
    if (gb->size >= (64u << 10)) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 30) NVK_INFO("large own buffer (XFB/SSBO) %llx: %llu KiB", (unsigned long long)addr,
                                             (unsigned long long)(gb->size >> 10));
    }
    gb->guest_hash = GuestRangeHash(ctx, gb->gpu, gb->size);
    gb->checked_frame = g_stats.frames.load(std::memory_order_relaxed);
    return gb;
}

uint64_t g_ssbo_gpu = 0, g_ssbo_partial = 0;  // writable SSBO bindings served with an own buffer
uint64_t g_cover_unions = 0;  // ranges served with the union of own buffers

// Unified memory (sprint 6, Q4): a range that PARTIALLY overlaps own buffers (written by XFB/compute) is served
// with an own buffer covering the union (initialized from the guest and, on top, with the contents of the ones it replaces; then
// gpubuf_sync carries over what the CPU writes). This way both the GPU-written and CPU-written parts read correctly (Q4: with
// the GPU part lost, giant wall; TRIANGLES: reading past the XFB, giant triangles). nullptr if it touches
// no own buffer (and !create) or if the union exceeds gpubuf_cover_max.
const VkCtx::GpuBuf* CoverGpuBuf(pla_ctx* ctx, VkCommandBuffer cmd, uint64_t base, uint64_t size, uint64_t align, bool create) {
    if (const VkCtx::GpuBuf* g = g_vk.FindGpuBuffer(base, size); g && ((base - g->gpu) % align) == 0) return g;
    uint64_t us = base, ue = base + size;
    bool any = false;
    for (int round = 0; round < 4; ++round) {
        bool grew = false;
        auto it = g_vk.gpu_bufs.lower_bound(us);
        if (it != g_vk.gpu_bufs.begin()) {
            auto pv = std::prev(it);
            if (pv->second.gpu + pv->second.size > us) {
                us = pv->second.gpu;
                ue = std::max(ue, pv->second.gpu + pv->second.size);
                any = grew = true;
            }
        }
        for (it = g_vk.gpu_bufs.lower_bound(us); it != g_vk.gpu_bufs.end() && it->first < ue; ++it) {
            any = true;
            if (it->second.gpu + it->second.size > ue) {
                ue = it->second.gpu + it->second.size;
                grew = true;
            }
        }
        const uint64_t al = us & ~255ull;  // like GpuBuffer
        if (al != us) {
            us = al;
            grew = true;
        }
        if (!grew) break;
    }
    if (!any && !create) return nullptr;
    if (ue - us > g_cfg.gpubuf_cover_max) {
        static std::atomic<int> logs{0};
        if (any && logs.fetch_add(1) < 30)
            NVK_INFO("union too large: range %llx+%llx -> union %llx+%llx (%llu KiB)", (unsigned long long)base,
                     (unsigned long long)size, (unsigned long long)us, (unsigned long long)(ue - us),
                     (unsigned long long)((ue - us) >> 10));
        return create ? EnsureGpuBuf(ctx, cmd, base, size) : nullptr;
    }
    if (any) {
        static std::atomic<int> logs2{0};
        if (logs2.fetch_add(1) < 40)
            NVK_INFO("union: range %llx+%llx -> %llx+%llx", (unsigned long long)base, (unsigned long long)size,
                     (unsigned long long)us, (unsigned long long)(ue - us));
    }
    if (any) ++g_cover_unions;
    const VkCtx::GpuBuf* r = EnsureGpuBuf(ctx, cmd, us, ue - us);
    if (r && ((base - r->gpu) % align) != 0) return nullptr;  // invalid offset: prefer the single-use copy
    return r;
}


struct DrvCbufCache {
    uint64_t serial = 0;
    VkDeviceSize align = 0;
    std::vector<uint8_t> bytes;
    UploadRef ref;
};
DrvCbufCache g_drv_cbuf[6][3];
uint64_t g_submit_serial = 1;  // PipelineBeginSubmit: ring copies and texture memos of earlier submissions are void

// FPS-1: ViewFromTic per (TIC words, view type, storage) memoized for the current submission while the texture cache
// epoch is unchanged (the per-frame CPU-change check and the uploads happen on the first lookup of each submission).
struct TexMemo {
    uint32_t tic[8];
    uint64_t serial = 0, epoch = 0;
    VkImageView view = VK_NULL_HANDLE;
    int want = 0;
    bool storage = false, is_depth = false;
};
TexMemo g_tex_memo[1024];
uint64_t g_tex_memo_hits = 0, g_tex_memo_calls = 0;
VkImageView TexViewMemo(pla_ctx* ctx, VkCommandBuffer cmd, const uint32_t tic[8], VkImageViewType want, bool* is_depth, bool storage) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t)want << 1) ^ (storage ? 1 : 0);
    for (int i = 0; i < 8; ++i) h = (h ^ tic[i]) * 0x100000001B3ull;
    TexMemo& m = g_tex_memo[(h >> 20) & 1023];
    ++g_tex_memo_calls;
    const uint64_t ep = g_tex.epoch.load(std::memory_order_relaxed);
    if (m.serial == g_submit_serial && m.epoch == ep && m.want == (int)want && m.storage == storage &&
        !std::memcmp(m.tic, tic, sizeof m.tic)) {
        ++g_tex_memo_hits;
        *is_depth = m.is_depth;
        return m.view;
    }
    const VkImageView v = g_tex.ViewFromTic(ctx, cmd, tic, want, is_depth, storage);
    // Epoch read after the call: whatever the call itself changed (upload done, image created) is part of this answer.
    std::memcpy(m.tic, tic, sizeof m.tic);
    m.serial = g_submit_serial;
    m.epoch = g_tex.epoch.load(std::memory_order_relaxed);
    m.want = (int)want;
    m.storage = storage;
    m.is_depth = *is_depth;
    m.view = v;
    return v;
}

// ---- Frame-locked uniform finder (diagnostics, NVNVK_UBO_SCAN=<file>) -----------------------------
// For the high FPS work: per (program, stage, cbuf, draw ordinal) it samples the UBO once per frame and, per float,
// keeps the per-frame delta statistics against the real frame time. Compare two runs at different FPS caps: a value
// whose mean delta per frame does not change is a per-frame accumulator (time += 1/30, frame += 1), i.e. a shader
// animation that speeds up with the FPS; a time-based one scales with the frame time. Report rewritten every 600 frames.
namespace {
struct ScanSlot {
    double n = 0, sd = 0, sd2 = 0, sdt = 0, sdt2 = 0, sddt = 0;
    uint32_t nz = 0;
};
constexpr uint32_t kScanFloats = 256;
struct ScanEntry {
    uint64_t prog = 0, seen = ~0ull;
    uint32_t stage = 0, cb = 0, ord = 0, size = 0;
    float prev[kScanFloats] = {}, first[kScanFloats] = {};
    bool have = false, have_first = false;
    ScanSlot slot[kScanFloats];
};
struct UboScan {
    std::string path;
    bool on = false;
    uint64_t frame = 0;
    double last_t = 0, dt = 0, dt_sum = 0, dt2_sum = 0, dt_n = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, std::unique_ptr<ScanEntry>> map;
    std::unordered_map<uint64_t, uint32_t> ord;  // (program, stage, cb) -> draws seen this frame
};
UboScan& Scan() {
    static UboScan s = [] {
        UboScan u;
        if (const char* e = std::getenv("NVNVK_UBO_SCAN")) { u.path = e; u.on = !u.path.empty(); }
        return u;
    }();
    return s;
}
void UboScanReport(UboScan& u) {
    FILE* f = std::fopen(u.path.c_str(), "w");
    if (!f) return;
    const double mdt = u.dt_n ? u.dt_sum / u.dt_n : 0, vdt = u.dt_n ? u.dt2_sum / u.dt_n - mdt * mdt : 0;
    std::fprintf(f, "# frames %llu, mean dt %.3f ms, cv dt %.3f\n# prog stage cb ord off n nz mean_d cv_d corr(d,dt) first last\n",
                 (unsigned long long)u.frame, mdt * 1e3, mdt > 0 ? std::sqrt(std::max(0.0, vdt)) / mdt : 0);
    for (auto& [k, e] : u.map) {
        for (uint32_t j = 0; j < kScanFloats && j * 4 < e->size; ++j) {
            const ScanSlot& sl = e->slot[j];
            if (sl.n < 60 || sl.nz < sl.n * 0.9) continue;
            const double md = sl.sd / sl.n, vd = std::max(0.0, sl.sd2 / sl.n - md * md);
            const double mt = sl.sdt / sl.n, vt = std::max(0.0, sl.sdt2 / sl.n - mt * mt);
            const double cv = md != 0 ? std::sqrt(vd) / std::fabs(md) : 1e9;
            const double corr = (vd > 0 && vt > 0) ? (sl.sddt / sl.n - md * mt) / std::sqrt(vd * vt) : 0;
            std::fprintf(f, "%llx %u %u %u 0x%x %.0f %u %.6g %.4f %.3f %g %g\n", (unsigned long long)e->prog, e->stage, e->cb,
                         e->ord, j * 4, sl.n, sl.nz, md, cv, corr, e->first[j], e->prev[j]);
        }
    }
    std::fclose(f);
}
void UboScanFrame() {
    UboScan& u = Scan();
    if (!u.on) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - u.t0).count();
    u.dt = u.last_t > 0 ? t - u.last_t : 0;
    u.last_t = t;
    if (u.dt > 0) { u.dt_sum += u.dt; u.dt2_sum += u.dt * u.dt; ++u.dt_n; }
    ++u.frame;
    u.ord.clear();
    if (u.frame % 600 == 0) UboScanReport(u);
}
void UboScanSample(pla_ctx* ctx, uint64_t prog, uint32_t stage, uint32_t cb, uint64_t gpu, uint64_t size) {
    UboScan& u = Scan();
    if (!u.on || !gpu || !size) return;
    const uint64_t pk = prog * 0x9E3779B97F4A7C15ull ^ ((uint64_t)stage << 8 | cb);
    const uint32_t ord = u.ord[pk]++;
    if (ord >= 4) return;
    std::unique_ptr<ScanEntry>& ep = u.map[pk * 31 + ord];
    if (!ep) { ep = std::make_unique<ScanEntry>(); ep->prog = prog; ep->stage = stage; ep->cb = cb; ep->ord = ord; }
    ScanEntry& e = *ep;
    if (e.seen == u.frame) return;
    const uint64_t cpu = g_reg.GpuToCpu(gpu);
    if (!cpu) return;
    float cur[kScanFloats] = {};
    const uint32_t n = (uint32_t)std::min<uint64_t>(size, sizeof cur);
    if (!g_guest.Read(ctx, cpu, cur, n)) return;
    if (e.have && e.seen + 1 == u.frame && u.dt > 0 && n == e.size) {
        for (uint32_t j = 0; j < n / 4; ++j) {
            const double d = (double)cur[j] - e.prev[j];
            if (!std::isfinite(d) || std::fabs(d) > 1e4) continue;
            ScanSlot& sl = e.slot[j];
            sl.n += 1; sl.sd += d; sl.sd2 += d * d; sl.sdt += u.dt; sl.sdt2 += u.dt * u.dt; sl.sddt += d * u.dt;
            if (d != 0) ++sl.nz;
        }
    }
    if (!e.have_first) { std::memcpy(e.first, cur, sizeof cur); e.have_first = true; }
    std::memcpy(e.prev, cur, sizeof cur);
    e.size = n;
    e.have = true;
    e.seen = u.frame;
}
}  // namespace

void BuildWrites(pla_ctx* ctx, VkCommandBuffer cmd, const NvnState& st, const Variant* v, Writes* out) {
    const bool uni = Unified();
    const VkDeviceSize ubo_align = std::max<VkDeviceSize>(uni ? 1 : 256, g_vk.props.limits.minUniformBufferOffsetAlignment);
    const VkDeviceSize ssbo_align = std::max<VkDeviceSize>(uni ? 1 : 64, g_vk.props.limits.minStorageBufferOffsetAlignment);
    for (const DescSlot& d : v->descs) {
        const uint32_t s = std::min<uint32_t>((uint32_t)d.stage, 5);
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = d.binding;
        w.descriptorType = d.type;
        w.descriptorCount = std::max(1u, d.d.count);
        if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            UploadRef r;
            uint64_t range = d.d.size ? d.d.size : 0x10000;
            range = std::min<uint64_t>(range, g_vk.props.limits.maxUniformBufferRange);
            if (d.d.cbuf_index >= 3 && d.d.cbuf_index - 3 < 18) {
                const VertexBinding& b = st.ubo[s][d.d.cbuf_index - 3];
                // Only what the game bound (like the hardware: reads past the bound size return 0).
                // Previously up to 64 KiB were copied per UBO and draw: the bulk of the submission CPU cost.
                if (b.size && !(uni && g_cfg.ubo_full_range)) range = std::min<uint64_t>(range, (b.size + 15) & ~15ull);
                range = std::max<uint64_t>(range, 16);
                UboScanSample(ctx, st.program, s, d.d.cbuf_index, b.addr, b.size);
                g_read_kind = 32;
                r = b.addr ? Upload(ctx, b.addr, range, ubo_align) : UploadRef{g_zero.buf, 0};
                g_read_kind = 8;
                range = std::max<uint64_t>(16, std::min<uint64_t>(range, r.avail));
                if (t_trace) {
                    float q[24] = {};
                    if (const uint64_t cpu = b.addr ? g_reg.GpuToCpu(b.addr) : 0) g_guest.Read(ctx, cpu, q, sizeof q);
                    TraceLine("  ubo stage=%u c%u binding=%u addr=%llx size=%llx range=%llx [16..23]=%g %g %g %g %g %g %g %g", s,
                              d.d.cbuf_index, w.dstBinding, (unsigned long long)b.addr, (unsigned long long)b.size,
                              (unsigned long long)range, q[16], q[17], q[18], q[19], q[20], q[21], q[22], q[23]);
                }
            } else {
                // driver cbufs: c0 = SSBO addresses and sizes (and UBOs in compute);
                // c2 = texture (0x20 + 8*i) and image (0x120 + 8*i) handles.
                uint32_t tmp[0x1000 / 4] = {};
                if (d.d.cbuf_index == 1) {
                    const std::vector<uint8_t>& c1 = v->c1[s];
                    std::memcpy(tmp, c1.data(), std::min<size_t>(c1.size(), sizeof tmp));
                    if (c1.size() > sizeof tmp) range = std::min<uint64_t>(range, sizeof tmp);
                } else if (d.d.cbuf_index == 2) {
                    for (uint32_t i = 0; i < 32; ++i) tmp[(0x20 + 8 * i) / 4] = (uint32_t)st.tex[s][i];
                    for (uint32_t i = 0; i < 8; ++i) tmp[(0x120 + 8 * i) / 4] = (uint32_t)st.img[s][i];
                } else if (d.d.cbuf_index == 0) {
                    auto put = [&](uint32_t off, const VertexBinding& b) {
                        tmp[off / 4] = (uint32_t)b.addr;
                        tmp[off / 4 + 1] = (uint32_t)(b.addr >> 32);
                        tmp[off / 4 + 2] = (uint32_t)b.size;
                    };
                    for (uint32_t i = 0; i < 16; ++i) put(kSsboBase[s] + 0x10 * i, st.ssbo[s][i]);
                    if (s == 5)
                        for (uint32_t i = 0; i < 16; ++i) put(kComputeUboBase + 0x10 * i, st.ubo[5][i]);
                }
                range = std::min<uint64_t>(range, sizeof tmp);
                // FPS-1: consecutive draws usually build the same driver buffer (same textures, same SSBOs): reuse the
                // previous copy of this (stage, cbuf) in this submission's ring instead of a new one (also lets
                // state_dedup skip the descriptor push).
                DrvCbufCache& dc = g_drv_cbuf[s][std::min<uint32_t>(d.d.cbuf_index, 2)];
                if (dc.serial == g_submit_serial && dc.bytes.size() == range && dc.align == ubo_align &&
                    !std::memcmp(dc.bytes.data(), tmp, (size_t)range)) {
                    r = dc.ref;
                } else {
                    r = UploadBytes(tmp, (size_t)range, ubo_align);
                    dc.serial = g_submit_serial;
                    dc.align = ubo_align;
                    dc.bytes.assign(reinterpret_cast<const uint8_t*>(tmp), reinterpret_cast<const uint8_t*>(tmp) + range);
                    dc.ref = r;
                }
            }
            out->first.push_back(out->b.size());
            out->b.push_back({r.buf, r.off, range});
        } else if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            UploadRef r{g_zero.buf, 0};
            uint64_t range = 16;
            const VertexBinding* b = d.d.cbuf_index == 0 ? C0Buffer(st, s, d.d.cbuf_offset) : nullptr;
            // Sprint 7: the SSBO pointer (global memory, LDG/STG) may live in a game UBO (c3..c20), not in the
            // driver table (c0). Previously a zero buffer was bound: zeroed bone matrices -> unanimated characters
            // and exploded vertices. Read {u64 address, u32 size} from that UBO, like the hardware does.
            VertexBinding ub_ptr;
            if (!b && g_cfg.ssbo_from_ubo && d.d.cbuf_index >= 3 && d.d.cbuf_index - 3 < 18) {
                const VertexBinding& u = st.ubo[s][d.d.cbuf_index - 3];
                uint64_t av = 0;
                const uint64_t ucpu = u.addr ? g_reg.GpuToCpu(u.addr + d.d.cbuf_offset, &av) : 0;
                if (ucpu && av >= 12) {
                    uint32_t q[3] = {};
                    g_guest.Read(ctx, ucpu, q, sizeof q);
                    ub_ptr.addr = (uint64_t)q[0] | ((uint64_t)q[1] << 32);
                    ub_ptr.size = q[2];
                    b = &ub_ptr;
                }
                static std::mutex lmu;
                static std::set<std::tuple<uint64_t, uint32_t, uint32_t>> seen;
                std::lock_guard<std::mutex> lk(lmu);
                if (seen.insert({st.program, d.d.cbuf_index, d.d.cbuf_offset}).second && seen.size() < 200)
                    NVK_INFO("SSBO via pointer in UBO: prog %llx stage %u c%u+%x -> %llx+%llx", (unsigned long long)st.program, s,
                             d.d.cbuf_index, d.d.cbuf_offset, (unsigned long long)ub_ptr.addr, (unsigned long long)ub_ptr.size);
            } else if (!b && d.d.cbuf_index != 0) {
                static std::atomic<int> logs{0};
                if (logs.fetch_add(1) < 40)
                    NVK_INFO("SSBO without address: prog %llx stage %u c%u+%x (binding zeros)", (unsigned long long)st.program, s,
                             d.d.cbuf_index, d.d.cbuf_offset);
            }
            if (b && b->addr) {
                // The shader indexes relative to (address & ~15) (it subtracts c0[base] & ~0xF): the buffer must
                // start at the 16-aligned address, not the bound one.
                const uint64_t base = b->addr & ~15ull, lead = b->addr - base;
                range = std::min<uint64_t>(std::max<uint64_t>(16, b->size + lead), 64ull << 20);
                // Sprint 6 (X1, frozen rain): an SSBO the shader writes (particles simulated in compute) went to a
                // single-use ring copy and the writes were lost; now it has a persistent own buffer, which later
                // draws (vertices/SSBO/UBO) also read through FindGpuBuffer.
                const VkCtx::GpuBuf* wb = nullptr;
                // Q5 (sprint 6): persistence only in the ssbo_persist_stages stages (default compute = bit 5: the
                // rain particles); in other stages it broke the character's legs.
                const bool persist = d.d.is_written && g_cfg.ssbo_persist && ((g_cfg.ssbo_persist_stages >> s) & 1) &&
                                     !g_no_persist.count(base) && (!g_cfg.ssbo_persist_rw_only || d.d.is_read);
                if (!uni && (persist || g_cfg.gpubuf_cover)) wb = CoverGpuBuf(ctx, cmd, base, range, ssbo_align, persist);
                if (wb) {
                    wb->last_use = g_stats.frames.load(std::memory_order_relaxed);
                    wb->guest_hash = GuestRangeHash(ctx, wb->gpu, wb->size);
                    wb->checked_frame = wb->last_use;
                    r = {wb->b.buf, base - wb->gpu};
                    ++g_ssbo_gpu;
                    wb->flags |= d.d.is_written ? (s == 5 ? 1u : 2u) : 64u;
                    if (d.d.is_written) g_wb_pending.push_back(wb->gpu);
                } else {
                    g_read_kind = d.d.is_written ? 128 : 64;
                    if (uni && d.d.is_written) {
                        static std::mutex lmu;
                        static std::set<std::pair<uint64_t, uint32_t>> seen;
                        std::lock_guard<std::mutex> lk(lmu);
                        if (seen.insert({st.program, d.binding}).second && seen.size() < 400)
                            NVK_INFO("SSBO written: prog %llx stage %u binding %u reads=%d %llx+%llx", (unsigned long long)st.program, s,
                                     d.binding, (int)d.d.is_read, (unsigned long long)base, (unsigned long long)range);
                    }
                    // Diagnostics: writes of small SSBOs (<= unified_ssbo_drop_max) go to a single-use copy (they are lost).
                    if (uni && d.d.is_written && range <= g_cfg.unified_ssbo_drop_max) g_read_kind = 64 | 0x1000;
                    r = Upload(ctx, base, range, ssbo_align);
                    g_read_kind = 8;
                    range = std::max<uint64_t>(16, std::min<uint64_t>(range, r.avail));
                }
                if (g_cfg.log_tex_updates) {
                    // Diagnostics (U2): does an SSBO land on a texture's memory? (GPU writes that nvnvk does not
                    // carry to the image: the texture keeps what was in guest memory).
                    const uint64_t cpu = g_reg.GpuToCpu(base);
                    static std::mutex mu;
                    static std::set<std::pair<uint64_t, uint64_t>> seen;
                    if (cpu) g_tex.ForEach([&](Image* im) {
                        if (!im->guest_cpu || cpu >= im->guest_cpu + im->guest_size || im->guest_cpu >= cpu + range) return;
                        std::lock_guard<std::mutex> lk(mu);
                        if (seen.insert({st.program, im->info.gpu}).second)
                            NVK_INFO("SSBO over texture: prog %llx stage %u ssbo %llx+%llx (cpu %llx) overlaps tex %llx %ux%u fmt %d (cpu %llx+%llx)",
                                     (unsigned long long)st.program, s, (unsigned long long)b->addr, (unsigned long long)range,
                                     (unsigned long long)cpu, (unsigned long long)im->info.gpu, im->info.width, im->info.height,
                                     (int)im->info.format, (unsigned long long)im->guest_cpu, (unsigned long long)im->guest_size);
                    });
                }
            }
            if (t_trace)
                TraceLine("  ssbo prog=%llx stage=%u binding=%u c0+%x addr=%llx(cpu %llx) size=%llx", (unsigned long long)st.program, s,
                          w.dstBinding, d.d.cbuf_offset, b ? (unsigned long long)b->addr : 0ull,
                          b ? (unsigned long long)g_reg.GpuToCpu(b->addr) : 0ull, b ? (unsigned long long)b->size : 0ull);
            if (t_trace && b && b->addr) {
                const uint64_t cpu = g_reg.GpuToCpu(b->addr);
                const uint64_t n = std::min<uint64_t>(std::max<uint64_t>(b->size, 64), 0x1000);
                for (uint64_t o = 0; cpu && o < n; o += 64) {
                    uint32_t q[16] = {};
                    g_guest.Read(ctx, cpu + o, q, sizeof q);
                    TraceLine("    ssbo[%llx]: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                              (unsigned long long)o, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11],
                              q[12], q[13], q[14], q[15]);
                }
            }
            out->first.push_back(out->b.size());
            out->b.push_back({r.buf, r.off, range});
        } else if (d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            out->first.push_back(out->i.size());
            const VkImageViewType want = kViewTypes[std::min<uint32_t>((uint32_t)d.d.texture_type, 8)];
            const uint32_t stride = d.d.size_shift ? (1u << d.d.size_shift) : 8;
            for (uint32_t e = 0; e < w.descriptorCount; ++e) {
                VkDescriptorImageInfo ii{};
                ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                uint32_t handle = 0;
                BoundHandle(ctx, st, s, d.d.cbuf_index, d.d.cbuf_offset + e * stride, &handle);
                if (d.d.has_secondary) {
                    uint32_t h2 = 0;
                    BoundHandle(ctx, st, s, d.d.secondary_cbuf_index, d.d.secondary_cbuf_offset, &h2);
                    handle = (handle << d.d.shift_left) | (h2 << d.d.secondary_shift_left);
                }
                uint32_t tic[8], tsc[8];
                bool is_depth = false;
                VkImageView view = VK_NULL_HANDLE;
                VkSampler smp = VK_NULL_HANDLE;
                bool have_tic;
                {
                    ScopedNs _a(g_cpu.w_desc);
                    have_tic = handle && ReadDesc(ctx, st.tex_pool_cpu, st.tex_pool_count, handle & 0xFFFFF, tic);
                }
                if (have_tic) {
                    ScopedNs _b(g_cpu.w_view);
                    view = TexViewMemo(ctx, cmd, tic, want, &is_depth, storage);
                }
                if (!storage && handle) {
                    ScopedNs _c(g_cpu.w_smp);
                    if (ReadDesc(ctx, st.smp_pool_cpu, st.smp_pool_count, handle >> 20, tsc))
                        smp = g_tex.Sampler(tsc, TicIsInteger(tic));
                }
                if (t_win_draw && have_tic && !storage) {
                    g_win_src.store(tic[1] | ((uint64_t)(tic[2] & 0xFFFF) << 32), std::memory_order_relaxed);
                    t_win_draw = false;
                }
                if (TraceOn() && handle) {
                    const uint64_t tg = tic[1] | ((uint64_t)(tic[2] & 0xFFFF) << 32);
                    if (t_trace || (g_cfg.Traced(tg) && (storage || !g_cfg.trace_writes_only)))
                        TraceLine("%s prog=%llx stage=%u binding=%u[%u] %s handle=%x tic.gpu=%llx(cpu %llx) tic0=%08x tic2=%08x tic4=%08x "
                                  "tic5=%08x view=%s depth=%d tsc0=%08x tsc1=%08x want=%d",
                                  t_trace ? "  texture" : "SAMPLES", (unsigned long long)st.program, s, w.dstBinding, e,
                                  storage ? "storage" : "sampled", handle, (unsigned long long)tg,
                                  (unsigned long long)g_reg.GpuToCpu(tg), tic[0], tic[2], tic[4],
                                  tic[5], view ? "ok" : "NULL", (int)is_depth, storage ? 0 : tsc[0], storage ? 0 : tsc[1],
                                  (int)want);
                }
                ii.imageView = view ? view : g_tex.NullView(want);
                if (!storage) ii.sampler = smp ? smp : g_tex.NullSampler();
                out->i.push_back(ii);
            }
        } else if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            // Sprint 7: buffer textures (previously skipped: the descriptor stayed unwritten and the shader read garbage;
            // e.g. bone palettes -> unanimated characters and exploded vertices). TIC with header 0 (1D buffer):
            // address, element count and format.
            if (!g_cfg.texel_buffers) continue;
            if (!g_null_bview) {
                VkBufferViewCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                bi.buffer = g_zero.buf;
                bi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                bi.range = 256;
                vkCreateBufferView(g_vk.dev, &bi, nullptr, &g_null_bview);
            }
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            out->first.push_back(out->tv.size());
            const uint32_t stride = d.d.size_shift ? (1u << d.d.size_shift) : 8;
            for (uint32_t e = 0; e < w.descriptorCount; ++e) {
                VkBufferView view = VK_NULL_HANDLE;
                uint32_t handle = 0, tic[8];
                BoundHandle(ctx, st, s, d.d.cbuf_index, d.d.cbuf_offset + e * stride, &handle);
                if (d.d.has_secondary) {
                    uint32_t h2 = 0;
                    BoundHandle(ctx, st, s, d.d.secondary_cbuf_index, d.d.secondary_cbuf_offset, &h2);
                    handle = (handle << d.d.shift_left) | (h2 << d.d.secondary_shift_left);
                }
                if (handle && ReadDesc(ctx, st.tex_pool_cpu, st.tex_pool_count, handle & 0xFFFFF, tic)) {
                    const uint64_t addr = tic[1] | ((uint64_t)(tic[2] & 0xFFFF) << 32);
                    const uint32_t header = (tic[2] >> 21) & 7;
                    const uint64_t texels = header == 0 ? ((((uint64_t)(tic[3] & 0xFFFF)) << 16) | (tic[4] & 0xFFFF)) + 1 : 0;
                    uint32_t bpb = 4, bw = 1, bh = 1;
                    const VkFormat fmt = TicFormat(tic[0], false, &bpb, &bw, &bh);
                    const uint64_t maxel = g_vk.props.limits.maxTexelBufferElements;
                    const uint64_t n = std::min<uint64_t>(texels, maxel);
                    if (addr && n && fmt != VK_FORMAT_UNDEFINED && bpb) {
                        const VkDeviceSize talign = std::max<VkDeviceSize>(16, g_vk.props.limits.minTexelBufferOffsetAlignment);
                        g_read_kind = storage ? 128 : 64;
                        UploadRef r = Upload(ctx, addr, n * bpb, talign);
                        g_read_kind = 8;
                        const uint64_t range = std::min<uint64_t>(n * bpb, r.avail);
                        VkBufferViewCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
                        bi.buffer = r.buf;
                        bi.format = fmt;
                        bi.offset = r.off;
                        bi.range = range - range % bpb;
                        if (bi.range && (r.off % talign) == 0 && vkCreateBufferView(g_vk.dev, &bi, nullptr, &view) == VK_SUCCESS)
                            g_vk.frames[g_vk.cur].dead_bviews.push_back(view);
                        else
                            view = VK_NULL_HANDLE;
                    }
                    static std::atomic<int> logs{0};
                    if (logs.fetch_add(1) < 30)
                        NVK_INFO("texel buffer: prog %llx stage %u %s handle %x addr %llx elements %llu format %d bpb %u -> %s",
                                 (unsigned long long)st.program, s, storage ? "image" : "texture", handle, (unsigned long long)addr,
                                 (unsigned long long)texels, (int)fmt, bpb, view ? "ok" : "NULL");
                }
                out->tv.push_back(view ? view : g_null_bview);
            }
        } else {
            continue;
        }
        out->w.push_back(w);
    }
    out->Finish();
}

}  // namespace

void PipelineInit() {
    g_vk.on_slot_reuse = &ResolveWriteback;
    g_caps = nvnshader::HostCapsFromVulkan(g_vk.phys);
    if (g_caps.native_ndc && !g_vk.has_depth_clip_control) g_caps.native_ndc = false;
    g_tr = nvnshader::CreateTranslator(g_caps, g_cfg.shader_cache_dir);
    g_zero = g_vk.CreateBuffer(64 * 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
                               true);
    std::memset(g_zero.map, 0, 64 * 1024);
    // VkPipelineCache: the one saved by nvnvk if present; otherwise the one prewarmed by nvnshader_vkcheck.
    std::vector<uint8_t> data;
    std::string from;
    for (const std::string& p : {g_cfg.pipeline_cache_out, g_cfg.pipeline_cache}) {
        if (p.empty()) continue;
        data = ReadFile(p);
        if (!data.empty()) {
            from = p;
            break;
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(g_vk.dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(g_vk.dev, &ci, nullptr, &g_pcache);
        from = "(rejected)";
    }
    NVK_INFO("shader translator ready (nvnshader, cache '%s', native_ndc=%d); VkPipelineCache %.1f MB from %s",
             g_cfg.shader_cache_dir.c_str(), (int)g_caps.native_ndc, data.size() / 1048576.0,
             from.empty() ? "(none)" : from.c_str());
    if (g_cfg.pipeline_threads > 0) {
        g_pw.Start(std::min(g_cfg.pipeline_threads, 8));
        NVK_INFO("background pipelines: %d threads, max wait %.1f ms per draw", std::min(g_cfg.pipeline_threads, 8),
                 g_cfg.pipeline_wait_ms);
    }
}

void PipelineCounters(uint64_t out[4]) {
    out[0] = g_ps.variants;
    out[1] = (uint64_t)(g_ps.translate_ms * 1000.0);
    out[2] = g_as.skipped.load(std::memory_order_relaxed) + g_var_skipped.load(std::memory_order_relaxed);
    out[3] = g_as.wait_ns.load(std::memory_order_relaxed) / 1000;
}

void PipelineReport() {
    NVK_INFO("pipelines: variants=%llu (failed %llu, %.1f ms translating), VkPipeline=%llu (failed %llu, %.1f ms, %.2f ms/ea); "
             "imported memory bindings=%llu (%.1f MB imported)",
             (unsigned long long)g_ps.variants, (unsigned long long)g_ps.variant_fail, g_ps.translate_ms,
             (unsigned long long)g_ps.pipelines, (unsigned long long)g_ps.pipe_fail, g_ps.pipeline_ms,
             g_ps.pipelines ? g_ps.pipeline_ms / g_ps.pipelines : 0.0, (unsigned long long)g_ps.imported_binds,
             g_hostmem.imported_bytes() / 1048576.0);
    if (g_pw.running()) {
        const uint64_t c = g_as.created.load();
        NVK_INFO("  background: created %llu (%.1f ms, max %.1f ms), waits %llu (%.1f ms total), draws skipped %llu",
                 (unsigned long long)c, g_as.create_ns.load() / 1e6, g_as.max_ns.load() / 1e6,
                 (unsigned long long)g_as.waited.load(), g_as.wait_ns.load() / 1e6, (unsigned long long)g_as.skipped.load());
        NVK_INFO("  background variants: %llu translated, draws skipped waiting for them %llu",
                 (unsigned long long)g_var_async.load(), (unsigned long long)g_var_skipped.load());
    }
    NVK_INFO("  transform feedback buffers: %zu live, %llu created, %llu evicted by age, %llu replaced, "
             "%llu dropped (CPU)",
             g_vk.gpu_bufs.size(), (unsigned long long)g_vk.gpubuf_created, (unsigned long long)g_vk.gpubuf_evicted,
             (unsigned long long)g_vk.gpubuf_replaced, (unsigned long long)g_vk.gpubuf_stale);
    NVK_INFO("  CPU -> own buffer sync: %llu buffers, %.1f MB", (unsigned long long)g_vk.gpubuf_synced_bufs,
             g_vk.gpubuf_synced_bytes / 1048576.0);
    NVK_INFO("  GPU -> guest writeback: %.1f MB, blocks already changed by the CPU: %llu", g_wb_bytes / 1048576.0,
             (unsigned long long)g_wb_skipped);
    {
        std::map<uint32_t, uint64_t> h = g_vk.flag_hist;
        for (auto& [k, g] : g_vk.gpu_bufs) ++h[g.flags];
        char b[1024];
        int n = 0;
        for (auto& [f, c] : h) n += std::snprintf(b + n, sizeof b - n > 0 ? sizeof b - n : 0, " %u:%llu", f, (unsigned long long)c);
        NVK_INFO("  own buffer usage (flags:n):%s", b);
    }
    NVK_INFO("  occlusion: mode %d, reports resolved %llu, addresses %zu, queries exhausted %llu", g_cfg.occlusion,
             (unsigned long long)g_vk.occl_resolved, g_vk.occl_values.size(), (unsigned long long)g_vk.occl_overflow);
    NVK_INFO("  XFB/SSBO buffers initialized from the guest: %llu; vertex buffers partly read from an XFB: %llu; "
             "writable SSBO bindings with own buffer: %llu; own buffer unions: %llu",
             (unsigned long long)g_xfb_inits, (unsigned long long)g_xfb_partial, (unsigned long long)g_ssbo_gpu,
             (unsigned long long)g_cover_unions);
    DedupReport();
    if (g_cfg.host_read_stats) {
        static uint64_t last_frames = 0, last[4][5] = {};
        const uint64_t fr = g_stats.frames.load(std::memory_order_relaxed), df = fr - last_frames;
        if (df) {
            static const char* kKind[4] = {"VB", "IB", "UBO", "SSBO"};
            char b[600];
            int n = 0;
            double host = 0, vram = 0;
            for (int k = 0; k < 4; ++k) {
                double v[5];
                for (int p = 0; p < 5; ++p) v[p] = (double)(g_hr[k][p] - last[k][p]) / df / 1048576.0;
                host += v[1] + v[2] + v[3];
                vram += v[4];
                n += std::snprintf(b + n, sizeof b - n, " %s ring %.1f snap %.1f imported %.1f vram %.1f;", kKind[k], v[1], v[2], v[3], v[4]);
            }
            NVK_HOST("GPU reads per frame (MB bound, host memory %.1f / VRAM %.1f):%s snapshots reused %llu (from VRAM %llu), promoted %llu",
                     host, vram, b, (unsigned long long)g_snapc_hits, (unsigned long long)g_snapc_dev_hits,
                     (unsigned long long)g_snapc_promoted);
            std::memcpy(last, g_hr, sizeof last);
            last_frames = fr;
        }
    }
    static uint64_t last[6] = {};
    const uint64_t now[6] = {g_cpu.upload.load(), g_cpu.variant.load(), g_cpu.pipeline.load(), g_cpu.writes.load(),
                             g_cpu.draws.load(), g_cpu.n_draws.load()};
    const double nd = (double)(now[5] - last[5]);
    if (nd > 0 && g_cfg.cpu_stats)
        NVK_INFO("CPU per draw (us): total %.2f = variant %.2f + VkPipeline %.2f + descriptors %.2f + uploads %.2f + rest",
                 (now[4] - last[4]) / nd / 1e3, (now[1] - last[1]) / nd / 1e3, (now[2] - last[2]) / nd / 1e3,
                 (now[3] - last[3]) / nd / 1e3, (now[0] - last[0]) / nd / 1e3);
    for (int i = 0; i < 6; ++i) last[i] = now[i];
    static uint64_t lw[3] = {};
    const uint64_t nw[3] = {g_cpu.w_desc.load(), g_cpu.w_view.load(), g_cpu.w_smp.load()};
    if (nd > 0 && g_cfg.cpu_stats)
        NVK_INFO("  descriptors per draw (us): TIC %.2f, texture view/upload %.2f, sampler %.2f",
                 (nw[0] - lw[0]) / nd / 1e3, (nw[1] - lw[1]) / nd / 1e3, (nw[2] - lw[2]) / nd / 1e3);
    for (int i = 0; i < 3; ++i) lw[i] = nw[i];
}

void DestroyComputePipelines();

void PipelineShutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    PipelineReport();
    if (g_pcache && !g_cfg.pipeline_cache_out.empty()) {
        size_t n = 0;
        vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, nullptr);
        std::vector<uint8_t> d(n);
        if (n && vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, d.data()) == VK_SUCCESS) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(g_cfg.pipeline_cache_out).parent_path(), ec);
            if (std::FILE* f = std::fopen(g_cfg.pipeline_cache_out.c_str(), "wb")) {
                std::fwrite(d.data(), 1, n, f);
                std::fclose(f);
                NVK_INFO("VkPipelineCache saved: %s (%.1f MB)", g_cfg.pipeline_cache_out.c_str(), n / 1048576.0);
            }
        }
    }
    g_pw.Stop();
    for (auto& [k, e] : g_pipelines)
        if (e && e->p) vkDestroyPipeline(g_vk.dev, e->p, nullptr);
    g_pipelines.clear();
    DestroyComputePipelines();
    for (auto& [k, list] : g_variants)
        for (auto& v : list) {
            for (auto m : v->vk) vkDestroyShaderModule(g_vk.dev, m, nullptr);
            if (v->layout) vkDestroyPipelineLayout(g_vk.dev, v->layout, nullptr);
            if (v->dsl) vkDestroyDescriptorSetLayout(g_vk.dev, v->dsl, nullptr);
        }
    g_variants.clear();
    if (g_pcache) vkDestroyPipelineCache(g_vk.dev, g_pcache, nullptr);
    g_pcache = VK_NULL_HANDLE;
    g_vk.DestroyBuffer(g_zero);
    if (g_arena.buf) g_vk.DestroyBuffer(g_arena);
    if (g_arena_dev.buf) g_vk.DestroyBuffer(g_arena_dev);
    g_snapc.clear();
    g_tr.reset();
}

void PipelineCpuCounters(uint64_t out[6]) {
    out[0] = g_cpu.upload.load();
    out[1] = g_cpu.writes.load();
    out[2] = g_cpu.variant.load();
    out[3] = g_cpu.pipeline.load();
    out[4] = g_cpu.draws.load();
    out[5] = g_xfb_hash_ns.load();
}

void PipelineBeginFrame() {
    UboScanFrame();
    g_budget_us.store((int64_t)(g_cfg.pipeline_frame_budget_ms * 1000.0), std::memory_order_relaxed);
}

// At the start of each submission (outside a render pass): carries into own buffers (XFB/SSBO) what the CPU wrote to their
// memory since last time, in 256 B blocks compared against the `shadow` copy. Only recently used ones.
// ---- GPU -> guest writeback of persisted SSBOs (gpubuf_writeback)

void QueueWriteback(VkCommandBuffer cmd) {
    if (!g_cfg.gpubuf_writeback || g_wb_pending.empty()) return;
    std::sort(g_wb_pending.begin(), g_wb_pending.end());
    g_wb_pending.erase(std::unique(g_wb_pending.begin(), g_wb_pending.end()), g_wb_pending.end());
    FrameSlot& f = g_vk.frames[g_vk.cur];
    uint64_t need = 0;
    for (uint64_t a : g_wb_pending)
        if (auto it = g_vk.gpu_bufs.find(a); it != g_vk.gpu_bufs.end()) need += (it->second.size + 15) & ~15ull;
    if (!f.readback.buf || f.readback.size < need) {
        // Grows on the slot's next use (could it be in use by the GPU now? no: it is recorded in this submission; it can be
        // recreated because the slot's previous buffer already finished when its fence was waited on in Begin).
        if (f.readback.buf) f.dead_buffers.push_back(f.readback);
        f.readback = g_vk.CreateBuffer(std::max<uint64_t>(need, 1ull << 20) * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    uint64_t off = 0;
    for (uint64_t a : g_wb_pending) {
        auto it = g_vk.gpu_bufs.find(a);
        if (it == g_vk.gpu_bufs.end() || it->second.shadow.empty()) continue;
        const VkCtx::GpuBuf& g = it->second;
        const VkBufferCopy c{0, off, g.size};
        vkCmdCopyBuffer(cmd, g.b.buf, f.readback.buf, 1, &c);
        f.wb.push_back({g.gpu, g.size, off});
        off += (g.size + 15) & ~15ull;
    }
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    g_wb_pending.clear();
}

// On slot reuse (its submission finished): what the GPU wrote goes to guest memory, in 256 B blocks, only
// where the CPU has not written since the last sync (guest memory == shadow). The shadow becomes the GPU data.
void ResolveWriteback(FrameSlot& f) {
    if (f.wb.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (const FrameSlot::WriteBack& w : f.wb) {
        auto it = g_vk.gpu_bufs.find(w.gpu);
        if (it == g_vk.gpu_bufs.end()) continue;
        VkCtx::GpuBuf& g = it->second;
        uint64_t avail = 0;
        uint8_t* h = const_cast<uint8_t*>(g_reg.GpuToHost(w.gpu, &avail));
        uint64_t cpu = 0;
        static std::vector<uint8_t> tmp;
        if (!h || avail < std::min<uint64_t>(w.size, g.shadow.size())) {
            // Non-contiguous backing: read and write through the guest API.
            cpu = g_reg.GpuToCpu(w.gpu, &avail);
            if (!cpu) continue;
            tmp.resize((size_t)std::min<uint64_t>(std::min<uint64_t>(avail, w.size), g.shadow.size()));
            g_guest.Read(nullptr, cpu, tmp.data(), tmp.size());
            h = tmp.data();
            avail = tmp.size();
        }
        const uint64_t n = std::min<uint64_t>(std::min<uint64_t>(avail, w.size), std::min<uint64_t>(g.size, g.shadow.size()));
        const uint8_t* src = f.readback.map + w.off;
        for (uint64_t o = 0; o < n; o += 256) {
            const uint64_t len = std::min<uint64_t>(256, n - o);
            if (!std::memcmp(src + o, g.shadow.data() + o, (size_t)len)) continue;  // the GPU did not change it
            if (std::memcmp(h + o, g.shadow.data() + o, (size_t)len)) {
                ++g_wb_skipped;  // the CPU wrote there: the CPU wins (SyncGpuBuffers will carry it)
                continue;
            }
            if (cpu) g_guest.Write(nullptr, cpu + o, src + o, (size_t)len);
            else std::memcpy(h + o, src + o, (size_t)len);
            std::memcpy(g.shadow.data() + o, src + o, (size_t)len);
            g_wb_bytes += len;
        }
    }
    f.wb.clear();
}

void SyncGpuBuffers(VkCommandBuffer cmd) {
    if (!g_cfg.gpubuf_sync) return;
    std::lock_guard<std::mutex> lk(g_mu);
    const uint64_t now = g_stats.frames.load(std::memory_order_relaxed);
    std::vector<VkBufferCopy> regions;
    std::vector<uint64_t> drop;
    bool any = false;
    for (auto& [k, g] : g_vk.gpu_bufs) {
        if (g.shadow.empty() || g.last_use + 600 < now) continue;
        uint64_t avail = 0;
        const uint8_t* h = g_reg.GpuToHost(g.gpu, &avail);
        static std::vector<uint8_t> tmp;
        if (!h || avail < std::min<uint64_t>(g.size, g.shadow.size())) {
            // Non-contiguous backing: copy through the guest API.
            uint64_t av2 = 0;
            const uint64_t cpu = g_reg.GpuToCpu(g.gpu, &av2);
            if (!cpu) continue;
            tmp.resize((size_t)std::min<uint64_t>(std::min<uint64_t>(av2, g.size), g.shadow.size()));
            g_guest.Read(nullptr, cpu, tmp.data(), tmp.size());
            h = tmp.data();
            avail = tmp.size();
        }
        const uint64_t n = std::min<uint64_t>(std::min<uint64_t>(avail, g.size), g.shadow.size());
        if (!std::memcmp(h, g.shadow.data(), (size_t)n)) continue;
        regions.clear();
        constexpr uint64_t kBlk = 256;
        for (uint64_t o = 0; o < n;) {
            const uint64_t len = std::min(kBlk, n - o);
            if (std::memcmp(h + o, g.shadow.data() + o, (size_t)len)) {
                uint64_t e = o + len;
                while (e < n) {
                    const uint64_t l2 = std::min(kBlk, n - e);
                    if (!std::memcmp(h + e, g.shadow.data() + e, (size_t)l2)) break;
                    e += l2;
                }
                // GPU-written SSBO (flags 1|2): if the CPU touches anything in a page, the CPU wins for the WHOLE page. On the
                // Switch the CPU usually rewrites the whole block every frame (sometimes with the same bytes, invisible here) and
                // the GPU modifies it in place (Kira's cloth): without this, the own buffer accumulates and explodes.
                if ((g.flags & 3) && g_cfg.ssbo_cpu_page) {
                    const uint64_t a0 = std::max<uint64_t>(g.gpu, (g.gpu + o) & ~4095ull);
                    const uint64_t a1 = (g.gpu + e + 4095) & ~4095ull;
                    o = a0 - g.gpu;
                    e = std::min<uint64_t>(n, a1 - g.gpu);
                }
                VkBuffer ub = VK_NULL_HANDLE;
                VkDeviceSize uo = 0;
                uint8_t* p = nullptr;
                AllocUpload(e - o, 16, &ub, &uo, &p);
                std::memcpy(p, h + o, (size_t)(e - o));
                std::memcpy(g.shadow.data() + o, h + o, (size_t)(e - o));
                if (!any) {
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                         nullptr, 0, nullptr);
                    any = true;
                }
                const VkBufferCopy c{uo, o, e - o};
                vkCmdCopyBuffer(cmd, ub, g.b.buf, 1, &c);
                g_vk.gpubuf_synced_bytes += e - o;
                o = e;
            } else {
                o += len;
            }
        }
        ++g_vk.gpubuf_synced_bufs;
        g.flags |= 128;
        // Q5/Q6 (sprint 6): if the CPU writes to the memory of an SSBO we persist (GPU-written), on the Switch the
        // CPU resets it every frame (sometimes with the same bytes, invisible here): stop persisting that address.
        if ((g.flags & 3) && g_cfg.ssbo_cpu_blacklist) {
            g_no_persist.insert(g.gpu);
            drop.push_back(g.gpu);
        }
        g.guest_hash = GuestRangeHash(nullptr, g.gpu, g.size);  // already includes the CPU data: no need to drop it
        g.checked_frame = now;
    }
    for (uint64_t a : drop) g_vk.DropGpuBuf(a);  // the rest of the frame reads guest memory (already with the CPU data)
    if (any) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }
}

bool PipelineUnified() { return Unified(); }

void PipelineXfbCheckBegin() {
    g_xfb_recs.clear();
    g_vb_uses.clear();
    g_xfb_rec_on = true;
}

// FNV-1a fingerprint of [gpu, gpu+size) in game memory (up to 1 MiB).
uint64_t ExactHash(pla_ctx* ctx, uint64_t gpu, uint64_t size) {
    const uint64_t cpu = g_reg.GpuToCpu(gpu);
    if (!cpu || !size) return 0;
    std::vector<uint8_t> b((size_t)std::min<uint64_t>(size, 1u << 20));
    g_guest.Read(ctx, cpu, b.data(), b.size());
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    return h;
}

// xfb_trace: one line per XFB draw of the marked frame, with fingerprints of the output, the input and the vertex shader UBOs.
// Between two consecutive frames: an animated mesh changes output; if the output does not change but the palette does, the XFB
// does not write where it is read; if the palette does not change either, the game (or the compute computing it) is not animating.
void XfbTrace(pla_ctx* ctx, uint64_t frame) {
    for (const XfbRec& r : g_xfb_recs) {
        std::string u;
        for (int i = 0; i < 18; ++i)
            if (r.ubo[i].addr) {
                char b[80];
                std::snprintf(b, sizeof b, " u%d=%llx+%llx:%08llx", i, (unsigned long long)r.ubo[i].addr, (unsigned long long)r.ubo[i].size,
                              (unsigned long long)(ExactHash(ctx, r.ubo[i].addr, r.ubo[i].size) & 0xFFFFFFFF));
                u += b;
            }
        float p0[3] = {};
        if (const uint64_t oc = g_reg.GpuToCpu(r.out_addr)) g_guest.Read(ctx, oc, p0, sizeof p0);
        char gs[96];
        std::snprintf(gs, sizeof gs, "%s v0(%g %g %g)", r.gathered ? " GATHERED" : "", p0[0], p0[1], p0[2]);
        u += gs;
        {
            // Attributes 12 (bone indices) and 13 (weights): format word and bytes of vertex 0 and vertex count/2.
            char ab[400];
            int o = std::snprintf(ab, sizeof ab, " attr12=%08x attr13=%08x attr0=%08x streams", r.vattrib[12], r.vattrib[13], r.vattrib[0]);
            for (int s = 0; s < 4 && o < 300; ++s)
                if (r.vbuf[s].addr)
                    o += std::snprintf(ab + o, sizeof ab - o, " %d:%llx/%llx", s, (unsigned long long)r.vbuf[s].addr,
                                       (unsigned long long)r.vstream[s]);
            u += ab;
            for (uint64_t vi : {r.first, r.first + r.count / 2}) {
                uint8_t raw[48] = {};
                if (const uint64_t ic = r.in_addr ? g_reg.GpuToCpu(r.in_addr + vi * r.in_stride) : 0) g_guest.Read(ctx, ic, raw, 48);
                std::string hx = " v" + std::to_string(vi) + "=";
                for (int i = 0; i < 48; ++i) {
                    char b[4];
                    std::snprintf(b, sizeof b, "%02x", raw[i]);
                    hx += b;
                    if ((i & 3) == 3) hx += ' ';
                }
                u += hx;
            }
        }
        int consumers = 0;
        for (const VbUse& v : g_vb_uses)
            if (v.addr < r.out_addr + r.out_size && r.out_addr < v.addr + std::max<uint64_t>(v.size, 1)) {
                if (consumers++ < 4) {
                    char cb[120];
                    std::snprintf(cb, sizeof cb, " [reads: prog %llx vb%u %+lld stride %u n %u]", (unsigned long long)v.program, v.slot,
                                  (long long)(v.addr - r.out_addr), v.stride, v.count);
                    u += cb;
                }
            }
        {
            char cb[32];
            std::snprintf(cb, sizeof cb, " readers %d", consumers);
            u += cb;
        }
        NVK_INFO("xfb_trace f%llu #%u prog %llx ctrl %llx %s mask %x first %llu count %llu out %llx+%llx:%08llx in %llx/%llu:%08llx%s",
                 (unsigned long long)frame, r.seq, (unsigned long long)r.program, (unsigned long long)r.ctrl, r.resume ? "CONT" : "new",
                 r.mask, (unsigned long long)r.first, (unsigned long long)r.count, (unsigned long long)r.out_addr,
                 (unsigned long long)r.out_size, (unsigned long long)(ExactHash(ctx, r.out_addr, r.out_size) & 0xFFFFFFFF),
                 (unsigned long long)r.in_addr, (unsigned long long)r.in_stride,
                 (unsigned long long)(ExactHash(ctx, r.in_addr + r.first * r.in_stride, r.count * r.in_stride) & 0xFFFFFFFF), u.c_str());
    }
}

// Call with the GPU idle (vkQueueWaitIdle) after the marked frame.
void PipelineXfbCheckEnd(pla_ctx* ctx, uint64_t frame) {
    g_xfb_rec_on = false;
    if (g_cfg.xfb_trace) XfbTrace(ctx, frame);
    uint64_t bad_draws = 0;
    for (const XfbRec& r : g_xfb_recs) {
        const uint64_t cpu = g_reg.GpuToCpu(r.out_addr);
        if (!cpu || !r.out_size) continue;
        const uint64_t n = std::min<uint64_t>(r.out_size, 1u << 20) / 4;
        std::vector<float> f(n);
        g_guest.Read(ctx, cpu, f.data(), n * 4);
        uint64_t bad = 0, first_bad = ~0ull;
        // Only position outputs (12 B per vertex and size = vertices x 12): non-finite vertices or more than 200
        // units from the mesh median (a vertex at the world origin stretches triangles up to the sky).
        const uint64_t nv = r.count;
        if (!nv || r.out_size < nv * 12 || r.out_size > nv * 12 + 64) continue;
        auto med = [&](int c) {
            std::vector<float> v;
            for (uint64_t i = 0; i < nv && i < n / 3; i += std::max<uint64_t>(1, nv / 256))
                if (std::isfinite(f[i * 3 + c])) v.push_back(f[i * 3 + c]);
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        const float mx = med(0), my = med(1), mz = med(2);
        for (uint64_t i = 0; i < nv && i * 3 + 2 < n; ++i) {
            const float x = f[i * 3], y = f[i * 3 + 1], z = f[i * 3 + 2];
            const bool fin = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
            if (!fin || std::fabs(x - mx) > 200 || std::fabs(y - my) > 200 || std::fabs(z - mz) > 200) {
                ++bad;
                if (first_bad == ~0ull) first_bad = i * 3;
            }
        }
        if (!bad) continue;
        ++bad_draws;
        static int logs = 0;
        if (logs++ > 60) continue;
        // Is that vertex's input already bad? (position of the first bad vertex: 12 B per vertex in the output)
        const uint64_t vtx = first_bad * 4 / 12;
        uint8_t in[48] = {};
        const uint64_t icpu = r.in_addr ? g_reg.GpuToCpu(r.in_addr + (r.first + vtx) * r.in_stride) : 0;
        if (icpu) g_guest.Read(ctx, icpu, in, std::min<uint64_t>(sizeof in, r.in_stride ? r.in_stride : 48));
        float pos[3];
        std::memcpy(pos, in, 12);
        std::string idx;
        for (uint64_t i = 0, k = 0; i < nv && i * 3 + 2 < n && k < 12; ++i) {
            const float x = f[i * 3], y = f[i * 3 + 1], z = f[i * 3 + 2];
            if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) || std::fabs(x - mx) > 200 || std::fabs(y - my) > 200 ||
                std::fabs(z - mz) > 200) {
                char b[96];
                std::snprintf(b, sizeof b, "v%llu(%g %g %g) ", (unsigned long long)i, x, y, z);
                idx += b;
                ++k;
            }
        }
        {
            char b[96];
            std::snprintf(b, sizeof b, " median (%g %g %g)", mx, my, mz);
            idx += b;
        }
        // Does another XFB draw of the frame write this same range? (overlapping outputs)
        std::string ov;
        for (const XfbRec& o : g_xfb_recs)
            if (&o != &r && o.out_addr < r.out_addr + r.out_size && r.out_addr < o.out_addr + o.out_size)
                ov += " [#" + std::to_string(o.seq) + " " + std::to_string(o.out_addr - r.out_addr) + "+" + std::to_string(o.out_size) +
                      (o.resume ? " cont" : "") + "]";
        NVK_WARN("xfb_check: #%u count %llu first %llu ctrl %llx %s; bad floats: %s; overlaps:%s", r.seq, (unsigned long long)r.count,
                 (unsigned long long)r.first, (unsigned long long)r.ctrl, r.resume ? "CONTINUES" : "new", idx.c_str(), ov.c_str());
        NVK_WARN("xfb_check: prog %llx output %llx+%llx: %llu bad values (first vertex %llu); input %llx stride %llu: "
                 "pos %g %g %g, bytes 36..47 %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                 (unsigned long long)r.program, (unsigned long long)r.out_addr, (unsigned long long)r.out_size,
                 (unsigned long long)bad, (unsigned long long)vtx, (unsigned long long)r.in_addr, (unsigned long long)r.in_stride,
                 pos[0], pos[1], pos[2], in[36], in[37], in[38], in[39], in[40], in[41], in[42], in[43], in[44], in[45], in[46],
                 in[47]);
    }
    NVK_INFO("xfb_check: %zu XFB draws checked, %llu with bad output", g_xfb_recs.size(), (unsigned long long)bad_draws);
    g_xfb_recs.clear();
}

bool PipelineHasScatters() { return !g_scatters.empty(); }

// Scatters back to their spans what the draw/dispatch wrote to gathered bindings (outside the render pass).
void PipelineFlushScatters(VkCommandBuffer cmd) {
    if (g_scatters.empty()) return;
    FullBarrier(cmd);
    for (const Gather& g : g_scatters) CopyPieces(cmd, g.gpu, g.size, g.buf, g.off, false);
    FullBarrier(cmd);
    g_scatter_n += g_scatters.size();
    g_scatters.clear();
    g_gathers.clear();  // gathered copies of those ranges are no longer valid
}

void PipelineInvalidateGathers() { g_gathers.clear(); }

void PipelineInvalidateUploads() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_uploads.clear();
}

void PipelineBeginSubmit() {
    ++g_submit_serial;
    {
        static uint64_t tick = 0;
        if (g_cfg.snap_cache_mb && (++tick % 3000) == 0) {
            const uint64_t frame = g_stats.frames.load(std::memory_order_relaxed), cap = (uint64_t)g_cfg.snap_cache_mb << 20;
            size_t pruned = 0;
            for (auto it = g_snapc.begin(); it != g_snapc.end();) {
                if (frame - it->second.last_frame > 600 || g_vk.arena_head - it->second.pos > cap / 2) {
                    it = g_snapc.erase(it);
                    ++pruned;
                } else {
                    ++it;
                }
            }
            NVK_INFO("snap_cache: %llu reused (%llu MB), %llu copied (%llu MB), %llu arena full; %zu entries (%zu pruned)",
                     (unsigned long long)g_snapc_hits, (unsigned long long)(g_snapc_hit_bytes >> 20), (unsigned long long)g_snapc_miss,
                     (unsigned long long)(g_snapc_copy_bytes >> 20), (unsigned long long)g_snapc_full, g_snapc.size(), pruned);
        }
    }
    g_uploads.clear();
    g_max_index.clear();
    g_snap.clear();
    g_gathers.clear();
    g_scatters.clear();
    static uint64_t tick = 0;
    if (g_gather_n && (++tick % 3000) == 0)
        NVK_INFO("VB/IB by residency: unknown %llu, VRAM %llu, GPU-written %llu, CPU only (copied at record) %llu; "
                 "vbib_trim: %llu MB copied, %llu MB saved",
                 (unsigned long long)g_vbib_res[0].load(), (unsigned long long)g_vbib_res[1].load(), (unsigned long long)g_vbib_res[2].load(),
                 (unsigned long long)g_vbib_res[3].load(), (unsigned long long)(g_trim_copied >> 20), (unsigned long long)(g_trim_saved >> 20));
        NVK_INFO("backing: %llu gathered bindings (%llu MB), %llu scattered back", (unsigned long long)g_gather_n,
                 (unsigned long long)(g_gather_bytes >> 20), (unsigned long long)g_scatter_n);
    g_use_import = g_hostmem.enabled() && (g_cfg.exclusive || g_cfg.import_in_shadow) && g_cfg.import_bind;
}

// ---------------------------------------------------------------- background variants
// Translating a new program (0.5-5 ms each; ~1 s in total when a battle starts) is done on
// the pipeline threads. The translator asks about bound textures while translating: the job
// carries a copy of the draw state and reads guest memory through host_pointer (no ctx). If the
// memory changed in between, the variant does not match on lookup and another is requested (a
// variant whose texture queries do not match the real draw state is never used).
struct VarJob {
    std::atomic<int> state{0};  // 0 running, 1 done, 2 failed
    Variant* result = nullptr;
    NvnState st;
    std::shared_ptr<ProgramSrc> src;
    TranslationState ts;
};
std::unordered_map<uint64_t, std::shared_ptr<VarJob>> g_varjobs;  // vkey -> running job

void RunVarJob(const std::shared_ptr<VarJob>& j) {
    auto make_resolver = [&j](uint32_t stage) -> TextureResolver {
        return [&j, stage](uint32_t cbuf, uint32_t off, TextureHint* out) -> bool {
            uint32_t handle = 0, tic[8];
            if (!BoundHandle(nullptr, j->st, stage, cbuf, off, &handle) || !handle) return false;
            if (!ReadDesc(nullptr, j->st.tex_pool_cpu, j->st.tex_pool_count, handle & 0xFFFFF, tic)) return false;
            out->type = TicTextureType(tic);
            out->is_integer = TicIsInteger(tic);
            return true;
        };
    };
    const TextureResolver res_vs = make_resolver(0), res_fs = make_resolver(1);
    j->result = BuildVariant(nullptr, j->st, *j->src, j->ts, res_vs, res_fs);
    g_var_async.fetch_add(1, std::memory_order_relaxed);
    g_pw.Finish(j->state, j->result ? 1 : 2);
}

// Returns the variant if ready (or ready within pipeline_wait_ms); otherwise nullptr and the draw is skipped.
// *failed = true if translation failed (as in the synchronous path: remembered and not retried).
Variant* AsyncVariant(uint64_t vkey, const NvnState& st, const std::shared_ptr<ProgramSrc>& src, const TranslationState& ts,
                      std::vector<std::unique_ptr<Variant>>& list, const TextureResolver& res_vs,
                      const TextureResolver& res_fs, bool* failed) {
    *failed = false;
    std::shared_ptr<VarJob>& j = g_varjobs[vkey];
    if (!j) {
        j = std::make_shared<VarJob>();
        j->st = st;
        j->src = src;
        j->ts = ts;
        g_pw.Push([job = j] { RunVarJob(job); });
    }
    const auto t0 = std::chrono::steady_clock::now();
    const bool ready = g_pw.Wait(j->state, WaitMs());
    Spend(t0);
    if (!ready) {
        g_var_skipped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    std::shared_ptr<VarJob> done = std::move(j);
    g_varjobs.erase(vkey);
    if (!done->result) {
        *failed = true;
        return nullptr;
    }
    Variant* v = done->result;
    list.emplace_back(v);
    bool match = true;
    for (size_t i = 0; match && i < v->mods.size(); ++i)
        match = sh::TextureQueriesMatch(*v->mods[i], v->stages[i] == Stage::Fragment ? res_fs : res_vs);
    if (!match) {
        g_var_skipped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;  // state changed during translation: the next draw will request another
    }
    return v;
}

// state_dedup (FPS-1): dynamic state, pipeline and vertex/index bindings last recorded into the current command buffer.
// Every graphics pipeline EmitDraw binds has the same dynamic state list (GetPipeline), so values persist across
// pipeline binds within a command buffer; a new command buffer (VkCtx::cmd_epoch) forgets everything.
// ~45 vkCmd* calls per draw went down to the ones whose value changed.
struct DynCache {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t epoch = ~0ull;
    uint64_t known = 0;  // bit per group below
    VkPipeline pipe = VK_NULL_HANDLE;
    VkViewport vp{};
    VkRect2D sc{};
    float bias[3] = {}, blend[4] = {}, bounds[2] = {};
    uint32_t bias_en = 0, bounds_en = 0, cmp_mask[2] = {}, wr_mask[2] = {}, ref[2] = {}, cull = 0, front = 0, dtest = 0, dwrite = 0,
             dcmp = 0, stest = 0, sop[2][4] = {}, discard = 0, restart = 0;
    VkBuffer vb[17] = {};
    VkDeviceSize vo[17] = {}, vs[17] = {};
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize io = 0;
    VkIndexType it = VK_INDEX_TYPE_UINT16;
    uint64_t dedup_hits = 0, dedup_calls = 0;
    // Last graphics push descriptor set (layout + contents) in this command buffer.
    VkPipelineLayout push_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorBufferInfo> push_b;
    std::vector<VkDescriptorImageInfo> push_i;
    std::vector<VkBufferView> push_tv;
    std::vector<uint32_t> push_w;  // binding, type, count per write
    uint64_t push_hits = 0, push_calls = 0;
} g_dyn;
enum : uint64_t {
    kDynPipe = 1ull << 0, kDynVp = 1ull << 1, kDynSc = 1ull << 2, kDynLw = 1ull << 3, kDynBiasEn = 1ull << 4, kDynBias = 1ull << 5,
    kDynBlend = 1ull << 6, kDynBounds = 1ull << 7, kDynBoundsEn = 1ull << 8, kDynCmpF = 1ull << 9, kDynCmpB = 1ull << 10,
    kDynWrF = 1ull << 11, kDynWrB = 1ull << 12, kDynRefF = 1ull << 13, kDynRefB = 1ull << 14, kDynCull = 1ull << 15,
    kDynFront = 1ull << 16, kDynDTest = 1ull << 17, kDynDWrite = 1ull << 18, kDynDCmp = 1ull << 19, kDynSTest = 1ull << 20,
    kDynSopF = 1ull << 21, kDynSopB = 1ull << 22, kDynDiscard = 1ull << 23, kDynRestart = 1ull << 24, kDynIb = 1ull << 25,
    kDynPush = 1ull << 26,
    kDynVb0 = 1ull << 32  // + slot (17 slots)
};
namespace {
void DedupReport() {
    static uint64_t lc = 0, lh = 0;
    const uint64_t c = g_dyn.dedup_calls - lc, h = g_dyn.dedup_hits - lh;
    lc = g_dyn.dedup_calls;
    lh = g_dyn.dedup_hits;
    if (c) NVK_INFO("  state_dedup: %.1f%% of %llu state/binding commands skipped", 100.0 * h / c, (unsigned long long)c);
    static uint64_t lpc = 0, lph = 0;
    const uint64_t pc = g_dyn.push_calls - lpc, ph = g_dyn.push_hits - lph;
    lpc = g_dyn.push_calls;
    lph = g_dyn.push_hits;
    if (pc) NVK_INFO("  state_dedup: %.1f%% of %llu descriptor pushes skipped (same layout and contents)", 100.0 * ph / pc, (unsigned long long)pc);
    static uint64_t ltc = 0, lth = 0;
    if (g_tex_memo_calls - ltc)
        NVK_INFO("  texture view memo: %.1f%% of %llu lookups", 100.0 * (g_tex_memo_hits - lth) / (g_tex_memo_calls - ltc),
                 (unsigned long long)(g_tex_memo_calls - ltc));
    ltc = g_tex_memo_calls;
    lth = g_tex_memo_hits;
}
}  // namespace
// True if the group must be (re)recorded; then it is marked as known and `cur` is stored in `cache`.
template <class T>
inline bool DynChanged(uint64_t bit, T& cache, const T& cur) {
    ++g_dyn.dedup_calls;
    if ((g_dyn.known & bit) && std::memcmp(&cache, &cur, sizeof(T)) == 0) {
        ++g_dyn.dedup_hits;
        return false;
    }
    std::memcpy(&cache, &cur, sizeof(T));
    g_dyn.known |= bit;
    return true;
}

bool EmitDraw(Executor* ex, pla_ctx* ctx, VkCommandBuffer cmd, const NvnState& st, const PDrawArrays* da, const PDrawElements* de,
              VkFormat* cf, uint32_t ncolor, VkFormat df, VkSampleCountFlagBits samples) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_tr || !st.program) return false;
    if (st.raster_discard && !st.xfb_active) return true;
    ScopedNs _tdraw(g_cpu.draws);
    if (g_cfg.cpu_stats) g_cpu.n_draws.fetch_add(1, std::memory_order_relaxed);
    t_trace = false;
    if (TraceOn() && g_cfg.trace_rtsets) {
        // Each change of render target set (address and size of the 8 + depth).
        static std::unordered_map<uint64_t, int> seen;  // each distinct set only once
        uint64_t hsh = Mix(st.rt_depth.gpu, st.program);
        for (uint32_t i = 0; i < ncolor; ++i) hsh = Mix(Mix(hsh, st.rt_color[i].gpu), st.rt_color[i].base_layer);
        if (seen.emplace(hsh, 1).second) {
            char b[400];
            int n = 0;
            for (uint32_t i = 0; i < ncolor && i < 8; ++i)
                n += std::snprintf(b + n, sizeof b - n, " %llx[l%u t%u](cpu %llx)/%ux%u/f%d", (unsigned long long)st.rt_color[i].gpu,
                                   st.rt_color[i].base_layer, st.rt_color[i].target, (unsigned long long)g_reg.GpuToCpu(st.rt_color[i].gpu),
                                   st.rt_color[i].width, st.rt_color[i].height, (int)cf[i]);
            TraceLine("RTS prog=%llx depth=%llx:%s", (unsigned long long)st.program, (unsigned long long)st.rt_depth.gpu, b);
        }
    }
    if (TraceOn()) {
        for (uint32_t i = 0; i < ncolor; ++i)
            if (g_cfg.Traced(st.rt_color[i].gpu)) t_trace = true;
        if (g_cfg.Traced(st.rt_depth.gpu)) t_trace = true;
        if (t_trace)
            TraceLine("DRAWS to the RT: prog=%llx ncolor=%u rt0=%llx fmt0=%d depth=%llx count=%u inst=%u base_inst=%u",
                      (unsigned long long)st.program, ncolor, (unsigned long long)st.rt_color[0].gpu, (int)cf[0],
                      (unsigned long long)st.rt_depth.gpu, da ? da->count : de->count,
                      da ? da->instance_count : de->instance_count, da ? da->base_instance : de->base_instance);
        if (t_trace) {
            const uint32_t a0 = st.vattrib[0], sidx = a0 & 0x1F;
            TraceLine("  color=%08x blend0=%08x/%08x depth_state=%08x/%08x df=%d", st.color, st.blend[0][0], st.blend[0][1],
                      st.depth_stencil[0], st.depth_stencil[1], (int)df);
            TraceLine("  vattrib0=%08x vattrib1=%08x vbuf[%u]=%llx/%llx stride=%llx first=%u", a0, st.vattrib[1], sidx,
                      (unsigned long long)st.vbuf[sidx & 15].addr, (unsigned long long)st.vbuf[sidx & 15].size,
                      (unsigned long long)st.vstream[sidx & 15], da ? da->first : 0u);
            if (const uint64_t cpu = g_reg.GpuToCpu(st.vbuf[sidx & 15].addr)) {
                float q[8] = {};
                g_guest.Read(ctx, cpu, q, sizeof q);
                TraceLine("    vb[0..7]: %g %g %g %g %g %g %g %g", q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
            }
        }
    }
    // ---- translation state (no texture hints: TranslateBound adds them)
    TranslationState ts{};
    for (uint32_t i = 0; i < 32; ++i) {
        const uint32_t a = i < 16 ? st.vattrib[i] : 0;
        ts.attrib_types[i] = (a & 0x40) ? AttribTypeOf((a >> 21) & 0x3FF) : AttribType::Float;
    }
    const uint32_t mode = da ? da->mode : de->mode;
    bool restart_ok = false;
    const VkPrimitiveTopology topo = nvnvk::Topology(mode, &restart_ok);
    ts.topology = topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? sh::Topology::Points
                  : (topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) ? sh::Topology::Lines
                                                                                                          : sh::Topology::Triangles;
    ts.point_size = st.point_size;
    ts.depth_minus_one_to_one = g_modes.depth_mode == 0;
    ts.y_negate = false;
    ts.xfb_active = st.xfb_active;
    const uint32_t at = (st.color >> 24) & 0xF;
    ts.alpha_test = at >= 1 && at <= 8 ? (sh::CompareOp)(at - 1) : sh::CompareOp::Always;
    ts.alpha_ref = st.alpha_ref;
    // Dual source: some RT 0 blend factor is SRC1_* (16-19) with blending enabled.
    if (ncolor && ((st.color >> 8) & 1)) {
        const uint32_t w1 = st.blend[0][1];
        for (int f = 0; f < 4; ++f) {
            const uint32_t b = (w1 >> (8 * f)) & 0x7F;
            if (b >= 16 && b <= 19) ts.dual_source_blend = true;
        }
    }
    for (uint32_t i = 0; i < 8; ++i) {
        AttribType t = AttribType::Float;
        if (i < ncolor) {
            switch (cf[i]) {
                case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8_UINT:
                case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R8G8B8A8_UINT:
                case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
                    t = AttribType::UInt; break;
                case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_SINT:
                case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R8G8B8A8_SINT:
                case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT:
                    t = AttribType::SInt; break;
                default: break;
            }
        }
        ts.color_output_types[i] = t;
    }
    const auto t_var = g_cfg.cpu_stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto info = g_obj.Program(st.program);
    auto src = LoadProgram(ctx, st.program);
    if (!info || !src || !src->ok || src->stages.empty()) return false;

    // ---- variant for the bound textures
    auto make_resolver = [&](uint32_t stage) -> TextureResolver {
        return [&, stage](uint32_t cbuf, uint32_t off, TextureHint* out) -> bool {
            uint32_t handle = 0, tic[8];
            if (!BoundHandle(ctx, st, stage, cbuf, off, &handle) || !handle) return false;
            if (!ReadDesc(ctx, st.tex_pool_cpu, st.tex_pool_count, handle & 0xFFFFF, tic)) return false;
            out->type = TicTextureType(tic);
            out->is_integer = TicIsInteger(tic);
            return true;
        };
    };
    const TextureResolver res_vs = make_resolver(0), res_fs = make_resolver(1);
    const uint64_t vkey = Mix(Mix(st.program, info->version), ts.Hash());
    auto& list = g_variants[vkey];
    Variant* v = nullptr;
    for (auto& cand : list) {
        bool match = true;
        for (size_t i = 0; match && i < cand->mods.size(); ++i)
            match = sh::TextureQueriesMatch(*cand->mods[i], cand->stages[i] == Stage::Fragment ? res_fs : res_vs);
        if (match) {
            v = cand.get();
            break;
        }
    }
    if (!v) {
        const bool async = g_pw.running() && g_cfg.async_variants && !ts.xfb_active && g_guest.host_ptr && !g_cfg.replay &&
                           g_cfg.capture_path.empty();
        if (async) {
            bool failed = false;
            v = AsyncVariant(vkey, st, src, ts, list, res_vs, res_fs, &failed);
            if (!v) {
                if (failed) list.push_back(std::make_unique<Variant>());
                return false;
            }
        } else {
            v = BuildVariant(ctx, st, *src, ts, res_vs, res_fs);
            if (!v) {
                // Remember the failure as an empty variant so it is not retried on every draw.
                auto dead = std::make_unique<Variant>();
                list.push_back(std::move(dead));
                return false;
            }
            list.emplace_back(v);
        }
    }
    if (!v->ok) return false;
    if (g_cfg.cpu_stats)
        g_cpu.variant.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_var).count());
    VkPipeline pipe;
    {
        ScopedNs _tp(g_cpu.pipeline);
        pipe = GetPipeline(v, st, cf, ncolor, df, samples, topo);
    }
    if (!pipe) return false;

    // ---- descriptores (push)
    t_cmd = Unified() && g_cfg.import_backing ? cmd : VK_NULL_HANDLE;
    static Writes s_wr;  // EmitDraw runs under g_mu
    Writes& wr = s_wr;
    wr.Clear();
    {
        ScopedNs _tw(g_cpu.writes);
        BuildWrites(ctx, cmd, st, v, &wr);
    }
    // Unified memory over the backing store: vertices, indices and XFB crossing a span boundary are gathered BEFORE the render pass.
    VkBuffer gvb[16] = {}, gxb[4] = {}, gib = VK_NULL_HANDLE;
    VkDeviceSize gvo[16] = {}, gxo[4] = {}, gio = 0;
    if (t_cmd) {
        const bool snapshot = g_cfg.vbib_snapshot;
        for (uint32_t s2 = 0; s2 < 16; ++s2)
            if (st.vbuf[s2].addr && !(snapshot && g_hostmem.Residency(st.vbuf[s2].addr, std::max<uint64_t>(st.vbuf[s2].size, 16)) == 3))
                GatherIfCrossing(cmd, st.vbuf[s2].addr, std::max<uint64_t>(st.vbuf[s2].size, 16), false, &gvb[s2], &gvo[s2]);
        uint32_t ibs = 2;
        if (de) IndexType(de->type, &ibs);
        if (de && de->index_addr && !(snapshot && g_hostmem.Residency(de->index_addr, std::max<uint64_t>((uint64_t)de->count * ibs, 4)) == 3)) {
            uint32_t ib = 2;
            IndexType(de->type, &ib);
            GatherIfCrossing(cmd, de->index_addr, std::max<uint64_t>((uint64_t)de->count * ib, 4), false, &gib, &gio);
        }
        if (st.xfb_active && g_vk.has_xfb && v->uses_xfb)
            for (uint32_t n = 0; n < 4 && st.xfb_buf[n].addr; ++n)
                GatherIfCrossing(cmd, st.xfb_buf[n].addr, std::max<uint64_t>(st.xfb_buf[n].size, 16), true, &gxb[n], &gxo[n]);
    }
    t_cmd = VK_NULL_HANDLE;
    // New transform feedback buffers (sprint 6): initialized with what is in guest memory and,
    // on top, with what the buffers they replace held. On the Switch the XFB writes to that memory, so
    // what the draw does not write keeps its previous contents; previously it was left uninitialized (reused
    // suballocated memory: garbage -> giant polygons) or, on replacement, the XFB-written data was lost.
    if (!Unified() && st.xfb_active && g_vk.has_xfb && v->uses_xfb && g_cfg.xfb_init) {
        for (uint32_t n = 0; n < 4 && st.xfb_buf[n].addr; ++n)
            EnsureGpuBuf(ctx, cmd, st.xfb_buf[n].addr, std::max<uint64_t>(st.xfb_buf[n].size, 16));
    }
    // Vertices and indices partially touching an own buffer: the union, before opening the render pass. All of this goes BEFORE
    // BuildWrites: a union replaces buffers and already built descriptors would point to the old ones.
    if (!Unified() && g_cfg.gpubuf_cover && !g_vk.gpu_bufs.empty()) {
        for (uint32_t s2 = 0; s2 < 16; ++s2)
            if (st.vbuf[s2].addr) CoverGpuBuf(ctx, cmd, st.vbuf[s2].addr, std::max<uint64_t>(st.vbuf[s2].size, 16), 1, false);
        if (de && de->index_addr) {
            uint32_t ib = 2;
            IndexType(de->type, &ib);
            CoverGpuBuf(ctx, cmd, de->index_addr, std::max<uint64_t>((uint64_t)de->count * ib, 4), 1, false);
        }
    }
    // Texture uploads (ViewFromTic) close the render pass; it is opened here, with no copies left.
    ex->BeginRendering();
    if (!ex->rendering()) return false;
    ex->OcclBegin();
    if (!g_cfg.state_dedup || g_dyn.cmd != cmd || g_dyn.epoch != g_vk.cmd_epoch) {
        g_dyn.cmd = cmd;
        g_dyn.epoch = g_vk.cmd_epoch;
        g_dyn.known = 0;
    }
    if (DynChanged(kDynPipe, g_dyn.pipe, pipe)) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (!wr.w.empty()) {
        // state_dedup: the same set (layout and every descriptor) as the previous draw's push stays bound.
        bool same = g_cfg.state_dedup && (g_dyn.known & kDynPush) && g_dyn.push_layout == v->layout && g_dyn.push_b.size() == wr.b.size() &&
                    g_dyn.push_i.size() == wr.i.size() && g_dyn.push_tv.size() == wr.tv.size() && g_dyn.push_w.size() == wr.w.size() * 3;
        if (same) {
            same = (wr.b.empty() || !std::memcmp(g_dyn.push_b.data(), wr.b.data(), wr.b.size() * sizeof(wr.b[0]))) &&
                   (wr.i.empty() || !std::memcmp(g_dyn.push_i.data(), wr.i.data(), wr.i.size() * sizeof(wr.i[0]))) &&
                   (wr.tv.empty() || !std::memcmp(g_dyn.push_tv.data(), wr.tv.data(), wr.tv.size() * sizeof(wr.tv[0])));
            for (size_t k = 0; same && k < wr.w.size(); ++k)
                same = g_dyn.push_w[3 * k] == wr.w[k].dstBinding && g_dyn.push_w[3 * k + 1] == (uint32_t)wr.w[k].descriptorType &&
                       g_dyn.push_w[3 * k + 2] == wr.w[k].descriptorCount;
        }
        ++g_dyn.push_calls;
        if (same) {
            ++g_dyn.push_hits;
        } else {
            g_vk.vkCmdPushDescriptorSetKHR_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout, 0, (uint32_t)wr.w.size(), wr.w.data());
            g_dyn.push_layout = v->layout;
            g_dyn.push_b = wr.b;
            g_dyn.push_i = wr.i;
            g_dyn.push_tv = wr.tv;
            g_dyn.push_w.clear();
            for (const VkWriteDescriptorSet& w : wr.w) {
                g_dyn.push_w.push_back(w.dstBinding);
                g_dyn.push_w.push_back((uint32_t)w.descriptorType);
                g_dyn.push_w.push_back(w.descriptorCount);
            }
            g_dyn.known |= kDynPush;
        }
    }

    // ---- dynamic state
    VkViewport vps[1];
    VkRect2D scs[1];
    const uint32_t nvp = 1;
    for (uint32_t i = 0; i < nvp; ++i) {
        const float* p = st.viewport[i];
        VkViewport& vv = vps[i];
        vv.x = p[0];
        vv.width = p[2] ? p[2] : 1;
        // UPPER_LEFT (D3D style): NDC +y up and row 0 at the top; in Vulkan NDC +y maps to row h,
        // so flip with a negative height. LOWER_LEFT (GL): row 0 is the bottom one, same as
        // Vulkan with a positive height (same contents in memory).
        if (g_modes.origin_mode == 1 && !g_cfg.flip_viewport) {
            vv.y = p[1] + p[3];
            vv.height = -p[3];
        } else {
            vv.y = p[1];
            vv.height = p[3] ? p[3] : 1;
        }
        vv.minDepth = st.depth_range[i][0];
        vv.maxDepth = st.depth_range[i][1];
        if (vv.minDepth == 0 && vv.maxDepth == 0) vv.maxDepth = 1;
        const int32_t* sc = st.scissor[i];
        scs[i].offset = {std::max(0, sc[0]), std::max(0, sc[1])};
        scs[i].extent = {(uint32_t)std::max(0, sc[2]), (uint32_t)std::max(0, sc[3])};
        if (sc[2] == 0 && sc[3] == 0) scs[i] = {{0, 0}, {16384, 16384}};
    }
    if (DynChanged(kDynVp, g_dyn.vp, vps[0])) vkCmdSetViewportWithCount(cmd, nvp, vps);
    if (DynChanged(kDynSc, g_dyn.sc, scs[0])) vkCmdSetScissorWithCount(cmd, nvp, scs);
    if (!(g_dyn.known & kDynLw)) {
        vkCmdSetLineWidth(cmd, 1.0f);
        g_dyn.known |= kDynLw;
    }
    const uint32_t poff = (st.polygon >> 15) & 0xF;
    if (DynChanged(kDynBiasEn, g_dyn.bias_en, (uint32_t)(poff != 0))) vkCmdSetDepthBiasEnable(cmd, poff != 0);
    {
        const float b3[3] = {st.poly_offset[1], st.poly_offset[2], st.poly_offset[0]};
        if (DynChanged(kDynBias, g_dyn.bias, b3)) vkCmdSetDepthBias(cmd, b3[0], b3[1], b3[2]);
    }
    if (DynChanged(kDynBlend, g_dyn.blend, st.blend_color)) vkCmdSetBlendConstants(cmd, st.blend_color);
    if (DynChanged(kDynBounds, g_dyn.bounds, st.depth_bounds)) vkCmdSetDepthBounds(cmd, st.depth_bounds[0], st.depth_bounds[1]);
    {
        const uint32_t be = df != VK_FORMAT_UNDEFINED && st.depth_bounds_enable;
        if (DynChanged(kDynBoundsEn, g_dyn.bounds_en, be)) vkCmdSetDepthBoundsTestEnable(cmd, be);
    }
    if (DynChanged(kDynCmpF, g_dyn.cmp_mask[0], st.stencil_vmask[0])) vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, st.stencil_vmask[0]);
    if (DynChanged(kDynCmpB, g_dyn.cmp_mask[1], st.stencil_vmask[1])) vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, st.stencil_vmask[1]);
    if (DynChanged(kDynWrF, g_dyn.wr_mask[0], st.stencil_mask[0])) vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, st.stencil_mask[0]);
    if (DynChanged(kDynWrB, g_dyn.wr_mask[1], st.stencil_mask[1])) vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, st.stencil_mask[1]);
    if (DynChanged(kDynRefF, g_dyn.ref[0], st.stencil_ref[0])) vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, st.stencil_ref[0]);
    if (DynChanged(kDynRefB, g_dyn.ref[1], st.stencil_ref[1])) vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, st.stencil_ref[1]);
    if (DynChanged(kDynCull, g_dyn.cull, st.polygon & 3)) vkCmdSetCullMode(cmd, (VkCullModeFlags)(st.polygon & 3));
    // NVN: bit 2 = 1 -> CCW (default, like GL).
    if (DynChanged(kDynFront, g_dyn.front, (st.polygon >> 2) & 1))
        vkCmdSetFrontFace(cmd, (st.polygon >> 2) & 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    const bool has_depth = df != VK_FORMAT_UNDEFINED;
    const bool has_stencil = IsStencilFormat(df);
    {
        const uint32_t dt = has_depth && (st.depth_stencil[0] & 1), dw = has_depth && (st.depth_stencil[0] & 2),
                       dc = (st.depth_stencil[0] >> 4) & 0xF, se = has_stencil && (st.depth_stencil[0] & 4);
        if (DynChanged(kDynDTest, g_dyn.dtest, dt)) vkCmdSetDepthTestEnable(cmd, dt);
        if (DynChanged(kDynDWrite, g_dyn.dwrite, dw)) vkCmdSetDepthWriteEnable(cmd, dw);
        if (DynChanged(kDynDCmp, g_dyn.dcmp, dc)) vkCmdSetDepthCompareOp(cmd, nvnvk::CompareOp(dc));
        if (DynChanged(kDynSTest, g_dyn.stest, se)) vkCmdSetStencilTestEnable(cmd, se);
    }
    {
        const uint32_t f = st.depth_stencil[1];
        const uint32_t sf[4] = {(f >> 4) & 0xF, (f >> 8) & 0xF, (f >> 12) & 0xF, f & 0xF};
        const uint32_t sb[4] = {(f >> 20) & 0xF, (f >> 24) & 0xF, (f >> 28) & 0xF, (f >> 16) & 0xF};
        if (DynChanged(kDynSopF, g_dyn.sop[0], sf))
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, StencilOp(sf[0]), StencilOp(sf[1]), StencilOp(sf[2]), nvnvk::CompareOp(sf[3]));
        if (DynChanged(kDynSopB, g_dyn.sop[1], sb))
            vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, StencilOp(sb[0]), StencilOp(sb[1]), StencilOp(sb[2]), nvnvk::CompareOp(sb[3]));
    }
    if (DynChanged(kDynDiscard, g_dyn.discard, st.raster_discard ? 1u : 0u))
        vkCmdSetRasterizerDiscardEnable(cmd, st.raster_discard ? VK_TRUE : VK_FALSE);
    if (DynChanged(kDynRestart, g_dyn.restart, (uint32_t)(de && restart_ok)))
        vkCmdSetPrimitiveRestartEnable(cmd, de && restart_ok ? VK_TRUE : VK_FALSE);
    if (v->push_size) {
        const float ra[4] = {(float)st.rt_color[0].width, (float)st.rt_color[0].height, 0, 0};
        vkCmdPushConstants(cmd, v->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16, ra);
    }

    // ---- vertices and indices
    for (uint32_t s = 0; s < 17; ++s) {
        VkBuffer b = g_zero.buf;
        VkDeviceSize off = 0, stride = 0;
        if (s < 16) {
            if (!st.vbuf[s].addr) continue;
            UploadRef r;
            // Sprint 6 (X1, frozen rain/particles): if the vertex buffer starts inside an XFB buffer but
            // was bound longer, it used to be read entirely from guest memory (never written by the XFB in exclusive mode).
            const VkCtx::GpuBuf* pg = !Unified() && g_cfg.xfb_partial_vertex ? PartialGpuBuf(st.vbuf[s].addr, std::max<uint64_t>(st.vbuf[s].size, 16)) : nullptr;
            if (pg && XfbStillValid(ctx, pg)) {
                if (g_xfb_partial++ < 10)
                    NVK_INFO("vertex buffer %llx (%llu B) read from XFB buffer %llx (%llu B)", (unsigned long long)st.vbuf[s].addr,
                             (unsigned long long)st.vbuf[s].size, (unsigned long long)pg->gpu, (unsigned long long)pg->size);
                pg->last_use = g_stats.frames.load(std::memory_order_relaxed);
                r = {pg->b.buf, st.vbuf[s].addr - pg->gpu};
            } else if (gvb[s]) {
                r = {gvb[s], gvo[s]};
            } else {
                uint64_t vsize = st.vbuf[s].size;
                // vbib_trim: the game often binds the VB with the size of the whole block; if it will be copied at record time
                // (vbib_snapshot), the vertices (or instances) the draw uses are enough (+1 for attribute offsets).
                if (g_cfg.vbib_trim && Unified() && vsize > 4096 && g_hostmem.Residency(st.vbuf[s].addr, vsize) == 3) {
                    const uint64_t stride = st.vstream[s] & 0xFFFFFFFF;
                    const uint32_t div = (uint32_t)(st.vstream[s] >> 32);
                    uint64_t n = 0;
                    bool known = false;
                    if (div) {
                        const uint32_t inst = std::max(1u, da ? da->instance_count : de ? de->instance_count : 1);
                        const uint32_t base = da ? da->base_instance : de ? de->base_instance : 0;
                        n = base + (inst + div - 1) / div;
                        known = true;
                    } else if (da) {
                        n = (uint64_t)da->first + da->count;
                        known = true;
                    } else if (de && de->base_vertex >= 0) {
                        uint32_t ibb = 2, mx = 0;
                        IndexType(de->type, &ibb);
                        if (MaxIndex(ctx, de->index_addr, de->count, ibb, &mx)) {
                            n = (uint64_t)de->base_vertex + mx + 1;
                            known = true;
                        }
                    }
                    if (known && stride) {
                        const uint64_t need = std::min<uint64_t>(vsize, (n + 1) * stride);
                        g_trim_saved += vsize - need;
                        vsize = need;
                    }
                    g_trim_copied += vsize;
                }
                r = Upload(ctx, st.vbuf[s].addr, vsize, Unified() ? 1 : 16);
            }
            b = r.buf;
            off = r.off;
            stride = (uint32_t)(st.vstream[s] & 0xFFFFFFFF);
        }
        ++g_dyn.dedup_calls;
        if ((g_dyn.known & (kDynVb0 << s)) && g_dyn.vb[s] == b && g_dyn.vo[s] == off && g_dyn.vs[s] == stride) {
            ++g_dyn.dedup_hits;
            continue;
        }
        g_dyn.vb[s] = b;
        g_dyn.vo[s] = off;
        g_dyn.vs[s] = stride;
        g_dyn.known |= kDynVb0 << s;
        vkCmdBindVertexBuffers2(cmd, s, 1, &b, &off, nullptr, &stride);
    }
    // ---- transform feedback (around this draw; the counters chain the draws of a session)
    const bool xfb = st.xfb_active && g_vk.has_xfb && v->uses_xfb;
    if (xfb) {
        VkBuffer bufs[4];
        VkDeviceSize offs[4], sizes[4];
        uint32_t n = 0;
        for (; n < 4 && st.xfb_buf[n].addr; ++n) {
            const uint64_t xa = st.xfb_buf[n].addr, xs = std::max<uint64_t>(st.xfb_buf[n].size, 16);
            if (Unified()) g_hostmem.MarkGpuWritten(xa, xs);
            if (gxb[n]) {
                bufs[n] = gxb[n];
                offs[n] = gxo[n];
                sizes[n] = xs;
                continue;
            }
            if (Unified() && (g_cfg.unified_mask & 32)) {
                // The XFB writes directly to game memory (as on the Switch).
                HostRef h;
                t_find_kind = 256;
                if (g_hostmem.Find(xa, xs, 4, &h, true)) {
                    bufs[n] = h.buf;
                    offs[n] = h.off;
                    sizes[n] = std::min<uint64_t>(xs, h.avail);
                    continue;
                }
                if (g_unified_miss.fetch_add(1) < 40)
                    NVK_INFO("unified memory: XFB %llx+%llx outside the imported pools: own buffer", (unsigned long long)xa,
                             (unsigned long long)xs);
            }
            const VkCtx::GpuBuf* gb = g_vk.FindGpuBuffer(xa, xs);  // also inside a union, at an offset
            if (!gb) gb = g_vk.GpuBuffer(xa, xs);
            if (!gb) break;
            gb->guest_hash = GuestRangeHash(ctx, gb->gpu, gb->size);
            gb->checked_frame = g_stats.frames.load(std::memory_order_relaxed);
            gb->last_use = gb->checked_frame;
            gb->flags |= 4;
            bufs[n] = gb->b.buf;
            offs[n] = xa - gb->gpu;
            sizes[n] = std::max<uint64_t>(st.xfb_buf[n].size, 16);
        }
        if (n) {
            g_vk.vkCmdBindTransformFeedbackBuffersEXT_(cmd, 0, n, bufs, offs, sizes);
            VkBuffer cb[4] = {g_vk.xfb_counters.buf, g_vk.xfb_counters.buf, g_vk.xfb_counters.buf, g_vk.xfb_counters.buf};
            // Slot whose buffer changed since the last End: no counter (starts at its beginning).
            for (uint32_t k = 0; k < 4; ++k)
                if (!((st.xfb_resume_mask >> k) & 1)) cb[k] = VK_NULL_HANDLE;
            const VkDeviceSize sb = (VkDeviceSize)XfbSlot(st.xfb_ctrl) * 16;
            const VkDeviceSize co[4] = {sb, sb + 4, sb + 8, sb + 12};
            g_vk.vkCmdBeginTransformFeedbackEXT_(cmd, 0, st.xfb_resume ? n : 0, st.xfb_resume ? cb : nullptr,
                                                 st.xfb_resume ? co : nullptr);
        }
    }
    if (de) {
        uint32_t ib = 2;
        VkIndexType it = IndexType(de->type, &ib);
        const uint64_t bytes = (uint64_t)de->count * ib;
        UploadRef r;
        if (it == VK_INDEX_TYPE_UINT8_EXT) {
            // 8-bit indices: widened to 16 (VK_EXT_index_type_uint8 is not enabled).
            std::vector<uint8_t> idx8(de->count);
            std::vector<uint16_t> dst(de->count + 1);
            const uint64_t cpu = g_reg.GpuToCpu(de->index_addr);
            if (cpu) g_guest.Read(ctx, cpu, idx8.data(), idx8.size());
            for (uint32_t i = 0; i < de->count; ++i) dst[i] = idx8[i] == 0xFF ? 0xFFFF : idx8[i];
            r = UploadBytes(dst.data(), dst.size() * 2, 4);
            it = VK_INDEX_TYPE_UINT16;
        } else {
            g_read_kind = 16;
            r = gib ? UploadRef{gib, gio} : Upload(ctx, de->index_addr, std::max<uint64_t>(bytes, 4), Unified() ? ib : 4);
            g_read_kind = 8;
        }
        ++g_dyn.dedup_calls;
        if (!(g_dyn.known & kDynIb) || g_dyn.ib != r.buf || g_dyn.io != r.off || g_dyn.it != it) {
            vkCmdBindIndexBuffer(cmd, r.buf, r.off, it);
            g_dyn.ib = r.buf;
            g_dyn.io = r.off;
            g_dyn.it = it;
            g_dyn.known |= kDynIb;
        } else {
            ++g_dyn.dedup_hits;
        }
        vkCmdDrawIndexed(cmd, de->count, std::max(1u, de->instance_count), 0, de->base_vertex, de->base_instance);
    } else if (t_xfb_draw_ctrl != ~0ull) {
        // nvnCommandBufferDrawTransformFeedback: as many vertices as bytes written by that session's XFB (buffer 0)
        // divided by vertex stream 0's stride. Previously not executed: those models were not drawn.
        const uint32_t stride = (uint32_t)(st.vstream[0] & 0xFFFFFFFF);
        ++g_xfb_draws;
        if (g_vk.vkCmdDrawIndirectByteCountEXT_ && stride) {
            g_vk.vkCmdDrawIndirectByteCountEXT_(cmd, 1, 0, g_vk.xfb_counters.buf, (VkDeviceSize)XfbSlot(t_xfb_draw_ctrl) * 16, 0,
                                                stride);
        } else {
            ++g_xfb_draws_bad;
        }
        if ((g_xfb_draws & (g_xfb_draws - 1)) == 0)
            NVK_INFO("DrawTransformFeedback: %llu (no stride %llu), control %llx, stride %u", (unsigned long long)g_xfb_draws,
                     (unsigned long long)g_xfb_draws_bad, (unsigned long long)t_xfb_draw_ctrl, stride);
    } else {
        vkCmdDraw(cmd, da->count, std::max(1u, da->instance_count), da->first, da->base_instance);
    }
    if (!xfb && g_xfb_rec_on && g_cfg.xfb_trace && g_vb_uses.size() < 200000)
        for (uint32_t s2 = 0; s2 < 16; ++s2)
            if (st.vbuf[s2].addr)
                g_vb_uses.push_back({st.vbuf[s2].addr, st.vbuf[s2].size, st.program, s2, (uint32_t)(st.vstream[s2] & 0xFFFFFFFF),
                                     da ? da->count : de ? de->count : 0, (uint32_t)g_vb_uses.size()});
    if (xfb && g_xfb_rec_on && st.xfb_buf[0].addr && da && g_xfb_recs.size() < 4096)
    {
        XfbRec r{st.program, st.xfb_buf[0].addr, st.xfb_buf[0].size, st.vbuf[0].addr,
                 (uint64_t)(st.vstream[0] & 0xFFFFFFFF), da->first, da->count, st.xfb_ctrl, st.xfb_resume,
                 (uint32_t)g_xfb_recs.size(), st.xfb_resume_mask, {}, gxb[0] != VK_NULL_HANDLE};
        std::memcpy(r.ubo, st.ubo[0], sizeof r.ubo);
        std::memcpy(r.vattrib, st.vattrib, sizeof r.vattrib);
        std::memcpy(r.vstream, st.vstream, sizeof r.vstream);
        std::memcpy(r.vbuf, st.vbuf, sizeof r.vbuf);
        g_xfb_recs.push_back(r);
    }
    if (xfb && st.xfb_buf[0].addr) {
        uint32_t n = 0;
        while (n < 4 && st.xfb_buf[n].addr) ++n;
        VkBuffer cb[4] = {g_vk.xfb_counters.buf, g_vk.xfb_counters.buf, g_vk.xfb_counters.buf, g_vk.xfb_counters.buf};
        const VkDeviceSize sb = (VkDeviceSize)XfbSlot(st.xfb_ctrl) * 16;
        const VkDeviceSize co[4] = {sb, sb + 4, sb + 8, sb + 12};
        g_vk.vkCmdEndTransformFeedbackEXT_(cmd, 0, n, cb, co);
        st.xfb_resume = true;
        st.xfb_resume_mask = (1u << n) - 1;
    }
    return true;
}


namespace {
std::unordered_map<const Variant*, VkPipeline> g_cpipelines;
}

bool EmitDispatch(pla_ctx* ctx, VkCommandBuffer cmd, const NvnState& st, uint32_t x, uint32_t y, uint32_t z) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_tr || !st.program || !x || !y || !z) return false;
    t_trace = g_cfg.trace_compute && TraceOn();
    if (t_trace) TraceLine("DISPATCH prog=%llx %ux%ux%u", (unsigned long long)st.program, x, y, z);
    auto info = g_obj.Program(st.program);
    auto src = LoadProgram(ctx, st.program);
    if (!info || !src || !src->ok || src->stages.size() != 1 || src->stages[0].hdr.stage != Stage::Compute) return false;
    TranslationState ts{};
    const TextureResolver res = [&](uint32_t cbuf, uint32_t off, TextureHint* out) -> bool {
        uint32_t handle = 0, tic[8];
        if (!BoundHandle(ctx, st, 5, cbuf, off, &handle) || !handle) return false;
        if (!ReadDesc(ctx, st.tex_pool_cpu, st.tex_pool_count, handle & 0xFFFFF, tic)) return false;
        out->type = TicTextureType(tic);
        out->is_integer = TicIsInteger(tic);
        return true;
    };
    const uint64_t vkey = Mix(Mix(st.program, info->version), ts.Hash() ^ 0xC0C0C0C0ull);
    auto& list = g_variants[vkey];
    Variant* v = nullptr;
    for (auto& cand : list)
        if (cand->ok && sh::TextureQueriesMatch(*cand->mods[0], res)) {
            v = cand.get();
            break;
        }
    if (!v) {
        if (!list.empty() && !list.back()->ok) return false;  // already failed
        v = BuildVariant(ctx, st, *src, ts, res, res);
        if (!v) {
            list.push_back(std::make_unique<Variant>());
            return false;
        }
        list.emplace_back(v);
    }
    VkPipeline& pipe = g_cpipelines[v];
    if (!pipe) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = v->vk[0];
        ci.stage.pName = "main";
        ci.layout = v->layout;
        const auto t0 = std::chrono::steady_clock::now();
        if (vkCreateComputePipelines(g_vk.dev, g_pcache, 1, &ci, nullptr, &pipe) != VK_SUCCESS) {
            ++g_ps.pipe_fail;
            pipe = VK_NULL_HANDLE;
            return false;
        }
        g_ps.pipeline_ms += Ms(t0);
        ++g_ps.pipelines;
        g_stats.pipelines.fetch_add(1, std::memory_order_relaxed);
    }
    static Writes s_wr;  // EmitDispatch runs under g_mu
    Writes& wr = s_wr;
    wr.Clear();
    t_cmd = Unified() && g_cfg.import_backing ? cmd : VK_NULL_HANDLE;
    BuildWrites(ctx, cmd, st, v, &wr);
    t_cmd = VK_NULL_HANDLE;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    if (!wr.w.empty())
        g_vk.vkCmdPushDescriptorSetKHR_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->layout, 0, (uint32_t)wr.w.size(), wr.w.data());
    vkCmdDispatch(cmd, x, y, z);
    PipelineFlushScatters(cmd);
    return true;
}

void DestroyComputePipelines() {
    for (auto& [k, p] : g_cpipelines)
        if (p) vkDestroyPipeline(g_vk.dev, p, nullptr);
    g_cpipelines.clear();
}

#endif  // NVNVK_WITH_NVNSHADER

}  // namespace nvnvk
