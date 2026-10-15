// Guest access, memory pool registry and object hooks (pools, programs, window).
// SPDX-License-Identifier: MIT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <map>
#include <set>
#include <windows.h>

#include <algorithm>

#include <thread>
#include "objects.h"
#include "capture.h"
#include "formats.h"
#include "hostmem.h"
#include "pipeline.h"
#include "textures.h"
#include "nvnvk.h"

namespace nvnvk {

Guest g_guest;
Registry g_reg;
Objects g_obj;

// ---------------------------------------------------------------- Guest
// The fastmem arena (host_pointer) is NOT safe to read from the host: with use_reactive_flushing=true
// suyu removes read permission (VirtualProtect) from pages its GPU has cached
// (RasterizerMarkRegionCached) and waits for guest code to fault in order to flush them. A memcpy
// of ours over those pages was the c0000005 in VCRUNTIME140!memcpy (shadow3/4, intro1/2).
// Order: physical backing memory (host_backing_pointer, a view suyu never protects) in contiguous
// runs; otherwise the arena with the fault caught (SEH); otherwise read_mem/write_mem.
thread_local int t_guarded = 0;  // the fault handler ignores AVs from a guarded copy
std::atomic<uint64_t> g_guarded_faults{0};

namespace {

// Copy that catches an access fault (no C++ objects in the frame: required by __try).
bool GuardedCopy(void* dst, const void* src, size_t n) {
    bool ok = true;
    ++t_guarded;
    __try {
        std::memcpy(dst, src, n);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        ok = false;
    }
    --t_guarded;
    return ok;
}

bool HasBacking(const pla_api* api) { return PLA_API_HAS(api, host_backing_pointer) && api->host_backing_pointer; }

// Walks [va, va+n) in runs within a 4 KiB page (merging those contiguous in backing memory)
// and calls fn(host, off, len) per run; returns false if any run has no backing.
template <class F>
bool ForBacking(const pla_api* api, uint64_t va, size_t n, F&& fn) {
    if (void* p = api->host_backing_pointer(va, n)) return fn(static_cast<uint8_t*>(p), size_t{0}, n);
    size_t off = 0;
    while (off < n) {
        const size_t chunk = std::min<size_t>(n - off, 4096 - ((va + off) & 4095));
        void* p = api->host_backing_pointer(va + off, chunk);
        if (!p || !fn(static_cast<uint8_t*>(p), off, chunk)) return false;
        off += chunk;
    }
    return true;
}

}  // namespace

bool Guest::Read(pla_ctx* ctx, uint64_t va, void* dst, size_t n) const {
    if (!n) return true;
    uint8_t* d = static_cast<uint8_t*>(dst);
    bool ok = false;
    if (HasBacking(api))
        ok = ForBacking(api, va, n, [&](uint8_t* p, size_t off, size_t len) { return GuardedCopy(d + off, p, len); });
    if (!ok && host_ptr) {
        if (const void* p = api->host_pointer(va, n)) {
            ok = GuardedCopy(dst, p, n);
            if (!ok) g_guarded_faults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!ok && ctx) ok = api->read_mem(ctx, va, dst, n) == n;
    if (!ok) {
        std::memset(dst, 0, n);
        return false;
    }
    if (g_capture.active()) g_capture.Mem(va, dst, n);
    return true;
}

bool Guest::Write(pla_ctx* ctx, uint64_t va, const void* src, size_t n) const {
    if (!n) return true;
    // Arena first (respects suyu's write tracking if it is not protected).
    if (host_ptr) {
        if (void* p = api->host_pointer(va, n)) {
            if (GuardedCopy(p, src, n)) return true;
            g_guarded_faults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (ctx) return api->write_mem(ctx, va, src, n) == n;
    const uint8_t* s = static_cast<const uint8_t*>(src);
    return HasBacking(api) &&
           ForBacking(api, va, n, [&](uint8_t* p, size_t off, size_t len) { return GuardedCopy(p, s + off, len); });
}

const uint8_t* Guest::Ptr(uint64_t va, size_t n) const {
    // Contiguous backing only: the arena may be protected (see above).
    if (!HasBacking(api)) return nullptr;
    const uint8_t* p = static_cast<const uint8_t*>(api->host_backing_pointer(va, n));
    if (p && g_capture.active()) g_capture.Mem(va, p, n);
    return p;
}

// ---------------------------------------------------------------- Registry
// Two live pools cannot share GPU addresses. If the new one overlaps another, the other no longer exists even if we
// have not seen its Finalize (sprint 7): it is removed, or GPU->CPU lookups could land in the dead pool (vertices,
// indices and textures read from unrelated memory after loading many areas: giant polygons, swapped textures).
bool PoolsOverlap(const PoolInfo& a, const PoolInfo& b) {
    for (uint64_t ga : {a.gpu, a.alias[0], a.alias[1]})
        for (uint64_t gb : {b.gpu, b.alias[0], b.alias[1]})
            if (ga && gb && ga < gb + b.size && gb < ga + a.size) return true;
    return false;
}
std::atomic<uint64_t> g_pool_evictions{0};

void Registry::AddPool(const PoolInfo& p) {
    std::lock_guard<std::mutex> lk(mu_);
    if (g_cfg.pool_evict_overlap) {
        for (auto it = by_obj_.begin(); it != by_obj_.end();) {
            if (it->first != p.obj && PoolsOverlap(it->second, p)) {
                const uint64_t n = g_pool_evictions.fetch_add(1) + 1;
                if (n <= 30 || (n & (n - 1)) == 0)
                    NVK_INFO("pool %llx (gpu %llx, %llu KiB) overlaps the new %llx (gpu %llx, %llu KiB): removing the old one (%llu total)",
                             (unsigned long long)it->first, (unsigned long long)it->second.gpu, (unsigned long long)(it->second.size >> 10),
                             (unsigned long long)p.obj, (unsigned long long)p.gpu, (unsigned long long)(p.size >> 10),
                             (unsigned long long)n);
                g_hostmem.RemovePool(it->first);
                it = by_obj_.erase(it);
            } else {
                ++it;
            }
        }
    }
    by_obj_[p.obj] = p;
    Resort();
}

void Registry::RemovePool(uint64_t obj) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_obj_.find(obj);
    if (it != by_obj_.end()) {
        Removed r{it->second, g_stats.frames.load(std::memory_order_relaxed)};
        if (removed_.size() < 4096)
            removed_.push_back(r);
        else
            removed_[removed_next_++ % 4096] = r;
    }
    by_obj_.erase(obj);
    Resort();
}

std::string Registry::Explain(uint64_t gpu) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string s;
    char b[200];
    for (const PoolInfo& p : sorted_)
        if (gpu >= p.gpu && gpu < p.gpu + p.size) {
            std::snprintf(b, sizeof b, " live: pool %llx gpu %llx+%llx", (unsigned long long)p.obj, (unsigned long long)p.gpu,
                          (unsigned long long)p.size);
            s += b;
        }
    const uint64_t f = g_stats.frames.load(std::memory_order_relaxed);
    for (const Removed& r : removed_)
        for (uint64_t g : {r.p.gpu, r.p.alias[0], r.p.alias[1]})
            if (g && gpu >= g && gpu < g + r.p.size) {
                std::snprintf(b, sizeof b, " finalized %llu frames ago: pool %llx gpu %llx+%llx", (unsigned long long)(f - r.frame),
                              (unsigned long long)r.p.obj, (unsigned long long)g, (unsigned long long)r.p.size);
                s += b;
            }
    return s.empty() ? std::string(" in no live or finalized pool") : s;
}

bool Registry::PoolByObj(uint64_t obj, PoolInfo* out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_obj_.find(obj);
    if (it == by_obj_.end()) return false;
    *out = it->second;
    return true;
}

void Registry::Resort() {
    sorted_.clear();
    for (auto& [k, v] : by_obj_)
        for (uint64_t g : {v.gpu, v.alias[0], v.alias[1]})
            if (g) {
                PoolInfo e = v;
                e.gpu = g;
                sorted_.push_back(e);
            }
    std::sort(sorted_.begin(), sorted_.end(), [](const PoolInfo& a, const PoolInfo& b) { return a.gpu < b.gpu; });
}

const uint8_t* Registry::GpuToHost(uint64_t gpu, uint64_t* avail) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::upper_bound(sorted_.begin(), sorted_.end(), gpu,
                               [](uint64_t g, const PoolInfo& p) { return g < p.gpu; });
    if (it == sorted_.begin()) return nullptr;
    --it;
    if (gpu >= it->gpu + it->size || !it->host) return nullptr;
    if (avail) *avail = it->gpu + it->size - gpu;
    return it->host + (gpu - it->gpu);
}

uint64_t Registry::GpuToCpu(uint64_t gpu, uint64_t* avail, uint32_t* flags) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::upper_bound(sorted_.begin(), sorted_.end(), gpu,
                               [](uint64_t g, const PoolInfo& p) { return g < p.gpu; });
    if (it == sorted_.begin()) return 0;
    --it;
    if (gpu >= it->gpu + it->size) return 0;
    if (avail) *avail = it->gpu + it->size - gpu;
    if (flags) *flags = it->flags;
    return it->cpu + (gpu - it->gpu);
}

std::vector<PoolInfo> Registry::Pools() {
    std::lock_guard<std::mutex> lk(mu_);
    return sorted_;
}

// ---------------------------------------------------------------- textures
bool ReadTexDesc(pla_ctx* ctx, uint64_t tex, uint64_t view, TexDesc* out) {
    *out = TexDesc{};
    if (!tex) return false;
    // Texture: copy of the builder (+0x08..+0x50), +0x60 stride. NVN-SEMANTICS §2.
    uint8_t t[0x68];
    if (!g_guest.Read(ctx, tex, t, sizeof t)) return false;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, t + o, 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v; std::memcpy(&v, t + o, 8); return v; };
    const uint64_t pool = u64(0x10), offset = u64(0x18);
    PoolInfo p;
    if (!g_reg.PoolByObj(pool, &p)) return false;
    out->obj = tex;
    // Textures use the pool's texture VA: the last non-null mapping (+0x60, else +0x50,
    // else +0x40). Verified in the trace: pool a1 +0x60 0x544c30000 + 0xf190000 = 0x553dc0000 (what
    // the driver stores in the object and writes to the TIC); pool 21 +0x50 0x5598e0000 + 0.
    const uint64_t tex_base = p.alias[1] ? p.alias[1] : p.alias[0] ? p.alias[0] : p.gpu;
    out->gpu = tex_base + offset;
    out->flags = u32(0x20);
    out->target = u32(0x24);
    out->width = u32(0x28);
    out->height = u32(0x2c);
    out->depth = u32(0x30);
    out->levels = std::max(1u, u32(0x34));
    out->format = u32(0x38);
    out->samples = u32(0x3c);
    out->stride = u32(0x60);
    if (view) {
        uint8_t v[0x24];
        if (g_guest.Read(ctx, view, v, sizeof v)) {
            const uint8_t mask = v[0];
            if (mask & 1) {
                out->base_level = v[1];
                out->num_levels = v[2];
            }
            if (mask & 2) {
                uint16_t bl, nl;
                std::memcpy(&bl, v + 4, 2);
                std::memcpy(&nl, v + 6, 2);
                out->base_layer = bl;
                out->num_layers = nl;
            }
            if (mask & 4) std::memcpy(&out->view_format, v + 8, 4);
        }
    }
    return true;
}

// ---------------------------------------------------------------- object hooks
namespace {

// Diagnostics (sprint 3, text): does nvnMemoryPoolMap return an address other than the builder's?
pla_action PoolMapPre(pla_ctx* ctx) {
    g_guest.api->call_data(ctx)[0] = X(ctx, 0);
    return PLA_CONTINUE;
}
void PoolMapPost(pla_ctx* ctx) {
    static std::mutex mu;
    static std::set<uint64_t> seen;
    const uint64_t pool = g_guest.api->call_data(ctx)[0], ret = X(ctx, 0);
    PoolInfo p;
    if (!g_reg.PoolByObj(pool, &p)) return;
    std::lock_guard<std::mutex> lk(mu);
    if (!seen.insert(pool).second) return;
    if (ret != p.cpu)
        NVK_INFO("MemoryPoolMap %llx returns %llx, different from storage %llx (size %llx, flags %x, gpu %llx)",
                 (unsigned long long)pool, (unsigned long long)ret, (unsigned long long)p.cpu, (unsigned long long)p.size,
                 p.flags, (unsigned long long)p.gpu);
}

void PoolInitPost(pla_ctx* ctx) {
    if ((X(ctx, 0) & 0xFF) == 0) return;  // Initialize returned false
    const uint64_t* cd = g_guest.api->call_data(ctx);
    const uint64_t pool = cd[0], builder = cd[1];
    uint64_t b[3] = {};
    g_guest.Read(ctx, builder + 0x10, b, sizeof b);
    PoolInfo p;
    p.obj = pool;
    p.cpu = b[0];
    p.size = b[1];
    p.flags = (uint32_t)b[2];
    // MemoryPool: +0x40 buffer VA, +0x50 and +0x60 other mappings (textures / compression).
    uint64_t m[5] = {};
    g_guest.Read(ctx, pool + 0x40, m, sizeof m);
    p.gpu = m[0];
    p.alias[0] = m[2];
    p.alias[1] = m[4];
    if (HasBacking(g_guest.api) && !g_cfg.replay && p.cpu && p.size)
        p.host = static_cast<const uint8_t*>(g_guest.api->host_backing_pointer(p.cpu, p.size));
    g_reg.AddPool(p);
    {
        static double total_ms = 0;
        static uint64_t n = 0;
        const auto t0 = std::chrono::steady_clock::now();
        g_hostmem.AddPool(p);
        if (g_cfg.pool_init_delay_us_per_mb)
            std::this_thread::sleep_for(std::chrono::microseconds((p.size >> 20) * g_cfg.pool_init_delay_us_per_mb + 50));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        total_ms += ms;
        {
            // Diagnostics: memory created per pool flag combination (CPU_NO_ACCESS 1, CPU_UNCACHED 2, CPU_CACHED 4,
            // GPU_CACHED 0x20, SHADER_CODE 0x40, COMPRESSIBLE 0x80...): what could live in VRAM.
            static std::map<uint32_t, std::pair<uint64_t, uint64_t>> by_flags;
            auto& e = by_flags[p.flags];
            ++e.first;
            e.second += p.size;
            if (((n + 1) % 400) == 0) {
                std::string s;
                for (auto& [f, v] : by_flags) {
                    char b[64];
                    std::snprintf(b, sizeof b, " %x:%llux%lluMB", f, (unsigned long long)v.first, (unsigned long long)(v.second >> 20));
                    s += b;
                }
                NVK_INFO("pools by flags (created, cumulative):%s", s.c_str());
            }
        }
        if ((++n % 200) == 0 || ms > 20)
            NVK_INFO("pools: %llu created, %.1f ms importing/delaying (this one %.1f ms, %llu KiB)", (unsigned long long)n, total_ms, ms,
                     (unsigned long long)(p.size >> 10));
    }
    NVK_DBG("pool %llx cpu=%llx size=%llx gpu=%llx/%llx/%llx flags=%x", (unsigned long long)pool,
            (unsigned long long)p.cpu, (unsigned long long)p.size, (unsigned long long)p.gpu,
            (unsigned long long)p.alias[0], (unsigned long long)p.alias[1], p.flags);
}

pla_action PoolInitPre(pla_ctx* ctx) {
    uint64_t* cd = g_guest.api->call_data(ctx);
    cd[0] = X(ctx, 0);
    cd[1] = X(ctx, 1);
    return PLA_CONTINUE;
}

pla_action PoolFinalize(pla_ctx* ctx) {
    g_reg.RemovePool(X(ctx, 0));
    g_hostmem.RemovePool(X(ctx, 0));
    return PLA_CONTINUE;
}

pla_action SetShaders(pla_ctx* ctx, unsigned stride) {
    const uint64_t prog = X(ctx, 0);
    const uint32_t n = std::min<uint32_t>((uint32_t)X(ctx, 1), 6);
    std::vector<uint64_t> raw(n * stride, 0);
    if (n) g_guest.Read(ctx, X(ctx, 2), raw.data(), raw.size() * 8);
    ProgramInfo pi;
    pi.obj = prog;
    for (uint32_t i = 0; i < n; ++i) {
        ProgramStage s;
        s.data_gpu = raw[i * stride];
        s.control_cpu = raw[i * stride + 1];
        pi.stages.push_back(s);
    }
    g_obj.SetProgram(pi);
    return PLA_CONTINUE;
}
pla_action SetShadersPre(pla_ctx* ctx) { return SetShaders(ctx, 2); }
pla_action SetShadersExtPre(pla_ctx* ctx) { return SetShaders(ctx, 3); }

pla_action WindowInitPre(pla_ctx* ctx) {
    // WindowBuilder: +0x10 texture count, +0x18 Texture**
    const uint64_t builder = X(ctx, 1);
    const uint32_t n = std::min<uint32_t>(g_guest.Get<uint32_t>(ctx, builder + 0x10), 8);
    const uint64_t arr = g_guest.Get<uint64_t>(ctx, builder + 0x18);
    WindowInfo w;
    w.obj = X(ctx, 0);
    w.textures.resize(n);
    if (n) g_guest.Read(ctx, arr, w.textures.data(), n * 8);
    g_obj.SetWindow(w);
    NVK_INFO("window %llx with %u textures", (unsigned long long)w.obj, n);
    return PLA_CONTINUE;
}

pla_action WindowSetCropPre(pla_ctx* ctx) {
    g_obj.SetCrop(X(ctx, 0), (int)X(ctx, 1), (int)X(ctx, 2), (int)X(ctx, 3), (int)X(ctx, 4));
    return PLA_CONTINUE;
}

pla_action OriginModePre(pla_ctx* ctx) {
    g_modes.origin_mode = (uint32_t)X(ctx, 1);
    NVK_INFO("nvnDeviceSetWindowOriginMode(%u)", g_modes.origin_mode);
    return PLA_CONTINUE;
}
pla_action DepthModePre(pla_ctx* ctx) {
    g_modes.depth_mode = (uint32_t)X(ctx, 1);
    NVK_INFO("nvnDeviceSetDepthMode(%u)", g_modes.depth_mode);
    return PLA_CONTINUE;
}

// A new texture (or one written from the CPU) at an already cached address: the guest data
// changed and must be uploaded again (texture streaming).
void InvalidateTexture(pla_ctx* ctx, uint64_t tex) {
    TexDesc d;
    if (!ReadTexDesc(ctx, tex, 0, &d) || !d.gpu) return;
    const FormatInfo& fi = NvnFormat(d.format);
    uint64_t size = (uint64_t)std::max(1u, d.width) * std::max(1u, d.height) * std::max(1u, d.depth) *
                    std::max(1u, fi.bpb) / std::max(1u, fi.bw * fi.bh);
    size += size / 3;
    g_tex.Invalidate(d.gpu, std::max<uint64_t>(size, 1));
}
pla_action TextureInitPre(pla_ctx* ctx) {
    g_guest.api->call_data(ctx)[0] = X(ctx, 0);
    return PLA_CONTINUE;
}
void TextureInitPost(pla_ctx* ctx) {
    if ((X(ctx, 0) & 0xFF) == 0) return;
    InvalidateTexture(ctx, g_guest.api->call_data(ctx)[0]);
}
pla_action TextureWriteTexelsPre(pla_ctx* ctx) {
    InvalidateTexture(ctx, X(ctx, 0));
    return PLA_CONTINUE;
}

pla_action PresentIntervalPre(pla_ctx* ctx) {
    NVK_INFO("nvnWindowSetPresentInterval(%llu)", (unsigned long long)X(ctx, 1));
    return PLA_CONTINUE;
}

}  // namespace

// ---------------------------------------------------------------- Objects
void Objects::SetProgram(const ProgramInfo& p) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& slot = programs_[p.obj];
    ProgramInfo q = p;
    q.version = slot ? slot->version + 1 : 1;
    slot = std::make_shared<const ProgramInfo>(std::move(q));
}

std::shared_ptr<const ProgramInfo> Objects::Program(uint64_t obj) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = programs_.find(obj);
    return it == programs_.end() ? nullptr : it->second;
}

void Objects::SetWindow(const WindowInfo& w) {
    std::lock_guard<std::mutex> lk(mu_);
    windows_[w.obj] = w;
}

bool Objects::Window(uint64_t obj, WindowInfo* out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = windows_.find(obj);
    if (it == windows_.end()) return false;
    *out = it->second;
    return true;
}

void Objects::SetCrop(uint64_t obj, int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = windows_.find(obj);
    if (it != windows_.end()) it->second.crop = {x, y, w, h};
}

void RegisterObjectHooks(PreFn* pre, PostFn* post) {
    pre[FN_nvnMemoryPoolInitialize] = PoolInitPre;
    post[FN_nvnMemoryPoolInitialize] = PoolInitPost;
    pre[FN_nvnMemoryPoolMap] = PoolMapPre;
    post[FN_nvnMemoryPoolMap] = PoolMapPost;
    pre[FN_nvnMemoryPoolFinalize] = PoolFinalize;
    pre[FN_nvnProgramSetShaders] = SetShadersPre;
    pre[FN_nvnProgramSetShadersExt] = SetShadersExtPre;
    pre[FN_nvnWindowInitialize] = WindowInitPre;
    pre[FN_nvnWindowSetCrop] = WindowSetCropPre;
    pre[FN_nvnWindowSetPresentInterval] = PresentIntervalPre;
    pre[FN_nvnDeviceSetWindowOriginMode] = OriginModePre;
    pre[FN_nvnDeviceSetDepthMode] = DepthModePre;
    pre[FN_nvnTextureInitialize] = TextureInitPre;
    post[FN_nvnTextureInitialize] = TextureInitPost;
    pre[FN_nvnTextureWriteTexels] = TextureWriteTexelsPre;
    pre[FN_nvnTextureWriteTexelsStrided] = TextureWriteTexelsPre;
}

}  // namespace nvnvk
