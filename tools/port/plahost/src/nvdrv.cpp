// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// nvdrv without a GPU. With the nvnvk renderer the game still runs the console's NVN memory
// code, which only needs the driver's bookkeeping: nvmap handles, the GPU address space
// (allocations and mappings: nvnvk reads the GPU addresses back from the NVN objects), the
// channel setup, and the fence numbers of the few submissions made at startup (all reported
// as already reached). Structure layouts and the answers follow the public nvgpu interface
// (as documented on switchbrew and implemented by every Switch emulator).

#include <map>
#include <mutex>
#include <unordered_map>

#include "ipc.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "services.h"

namespace plahost::services {
namespace {

enum NvResult : u32 {
    NvSuccess = 0,
    NvNotImplemented = 1,
    NvNotSupported = 2,
    NvNotInitialized = 3,
    NvBadParameter = 4,
    NvInsufficientMemory = 6,
    NvInvalidState = 8,
    NvBadValue = 0xB,
    NvAlreadyAllocated = 0xD,
    NvFileOperationFailed = 0x30003,
    NvAccessDenied = 0x30010,
};

std::recursive_mutex g_lock;

// ---- page allocator over [start, limit) (first fit) ----
class PageAllocator {
public:
    void Init(u32 start, u32 limit) {
        free_.clear();
        start_ = start;
        limit_ = limit;
        if (limit > start) free_[start] = limit - start;
    }
    u32 Allocate(u32 n) {
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second >= n) {
                const u32 at = it->first, len = it->second;
                free_.erase(it);
                if (len > n) free_[at + n] = len - n;
                return at;
            }
        }
        return 0;
    }
    void AllocateFixed(u32 at, u32 n) {
        auto it = free_.upper_bound(at);
        if (it == free_.begin()) return;
        --it;
        const u32 s = it->first, len = it->second;
        if (at + n > s + len) return;
        free_.erase(it);
        if (at > s) free_[s] = at - s;
        if (s + len > at + n) free_[at + n] = s + len - (at + n);
    }
    void Free(u32 at, u32 n) {
        auto next = free_.lower_bound(at);
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) {
                prev->second += n;
                if (next != free_.end() && prev->first + prev->second == next->first) {
                    prev->second += next->second;
                    free_.erase(next);
                }
                return;
            }
        }
        if (next != free_.end() && at + n == next->first) {
            const u32 len = next->second;
            free_.erase(next);
            free_[at] = n + len;
            return;
        }
        free_[at] = n;
    }
    u32 Start() const { return start_; }
    u32 Limit() const { return limit_; }

private:
    std::map<u32, u32> free_;
    u32 start_ = 0, limit_ = 0;
};

// ---- nvmap ----
struct NvHandle {
    u32 id = 0;
    u64 size = 0, orig_size = 0;
    u32 align = 0x1000;
    u8 kind = 0;
    u32 flags = 0;
    u64 address = 0;
    bool allocated = false;
    s32 dupes = 1;
};
std::unordered_map<u32, NvHandle> g_handles;
u32 g_next_handle = 4;

// ---- devices ----
enum class Dev { Nvmap, AsGpu, CtrlGpu, Gpu, Ctrl, Other };
struct Fd {
    Dev dev;
    std::string path;
};
std::unordered_map<u32, Fd> g_fds;
u32 g_next_fd = 1;

// GPU address space (one per process here).
struct AsGpu {
    bool init = false;
    u32 big_page_size = 0x20000;
    u32 big_bits = 17;
    u64 va_start = 0x20000ULL << 10, va_split = 1ULL << 34, va_end = 1ULL << 37;
    PageAllocator small, big;
    struct Alloc { u64 size; u32 page_size; bool sparse; bool big; std::vector<u64> maps; };
    struct Map { u64 size; u32 handle; bool fixed; bool big; };
    std::map<u64, Alloc> allocs;
    std::map<u64, Map> maps;
} g_as;

u32 g_syncpt_value = 0;
constexpr u32 kChannelSyncpt = 20;
u64 g_client_data = 0;

std::map<std::pair<u32, u32>, std::shared_ptr<kernel::Event>> g_dev_events;

std::shared_ptr<kernel::Event> DevEvent(u32 fd, u32 id) {
    auto& e = g_dev_events[{fd, id}];
    if (!e) e = kernel::NewEvent("nvdrv");
    return e;
}

template <typename T>
T In(const std::vector<u8>& in) {
    T v{};
    std::memcpy(&v, in.data(), std::min(sizeof(T), in.size()));
    return v;
}
template <typename T>
void Out(std::vector<u8>& out, const T& v) {
    if (out.size() < sizeof(T)) out.resize(sizeof(T));
    std::memcpy(out.data(), &v, sizeof(T));
}

u32 NvmapIoctl(u32 cmd, const std::vector<u8>& in, std::vector<u8>& out) {
    out = in;
    switch (cmd) {
    case 0x01: { // Create
        struct P { u32 size, handle; } p = In<P>(in);
        NvHandle h;
        h.id = g_next_handle;
        g_next_handle += 4;
        h.orig_size = p.size;
        h.size = AlignUp(p.size, kPageSize);
        g_handles[h.id] = h;
        p.handle = h.id;
        Out(out, p);
        return NvSuccess;
    }
    case 0x03: { // FromId
        struct P { u32 id, handle; } p = In<P>(in);
        auto it = g_handles.find(p.id);
        if (!p.id || it == g_handles.end() || !it->second.allocated) return NvBadValue;
        ++it->second.dupes;
        p.handle = p.id;
        Out(out, p);
        return NvSuccess;
    }
    case 0x04: { // Alloc
        struct P { u32 handle, heap_mask, flags, align; u8 kind; u8 pad[7]; u64 address; } p = In<P>(in);
        if (!p.handle || ((p.align - 1) & p.align)) return NvBadValue;
        auto it = g_handles.find(p.handle);
        if (it == g_handles.end()) return NvBadValue;
        if (it->second.allocated) return NvInsufficientMemory;
        auto& h = it->second;
        h.flags = p.flags;
        h.kind = p.kind;
        h.align = p.align < kPageSize ? static_cast<u32>(kPageSize) : p.align;
        h.size = AlignUp(h.size, h.align);
        h.address = p.address;
        h.allocated = true;
        Out(out, p);
        return NvSuccess;
    }
    case 0x05: { // Free
        struct P { u32 handle, pad; u64 address; u32 size, flags; } p = In<P>(in);
        if (!p.handle) return NvSuccess;
        auto it = g_handles.find(p.handle);
        if (it != g_handles.end() && --it->second.dupes <= 0) {
            p.address = it->second.address;
            p.size = static_cast<u32>(it->second.size);
            p.flags = 0;
            g_handles.erase(it);
        }
        Out(out, p);
        return NvSuccess;
    }
    case 0x09: { // Param
        struct P { u32 handle, param, result; } p = In<P>(in);
        auto it = g_handles.find(p.handle);
        if (!p.handle || it == g_handles.end()) return NvBadValue;
        const auto& h = it->second;
        switch (p.param) {
        case 1: p.result = static_cast<u32>(h.orig_size); break;
        case 2: p.result = h.align; break;
        case 3: p.result = static_cast<u32>(-22); break;
        case 4: p.result = h.allocated ? 0x40000000 : 0; break;
        case 5: p.result = h.kind; break;
        case 6: p.result = 0; break;
        default: return NvBadValue;
        }
        Out(out, p);
        return NvSuccess;
    }
    case 0x0E: { // GetId
        struct P { u32 id, handle; } p = In<P>(in);
        if (!p.handle) return NvBadValue;
        if (!g_handles.count(p.handle)) return NvAccessDenied;
        p.id = p.handle;
        Out(out, p);
        return NvSuccess;
    }
    }
    return NvNotImplemented;
}

u32 AsGpuIoctl(u32 nr, const std::vector<u8>& in, std::vector<u8>& out, std::vector<u8>* inline_out) {
    out = in;
    auto& as = g_as;
    switch (nr) {
    case 0x01: // BindChannel
        return NvSuccess;
    case 0x09: { // AllocAsEx
        struct P { u32 flags; s32 as_fd; u32 big_page_size, reserved; u64 start, end, split; } p = In<P>(in);
        if (as.init) return NvInvalidState;
        if (p.big_page_size) {
            if ((p.big_page_size & (p.big_page_size - 1)) || !(p.big_page_size & 0x30000)) return NvBadValue;
            as.big_page_size = p.big_page_size;
            as.big_bits = static_cast<u32>(_tzcnt_u32(p.big_page_size));
            as.va_start = u64{p.big_page_size} << 10;
        }
        if (p.start) {
            as.va_start = p.start;
            as.va_split = p.split;
            as.va_end = p.end;
        }
        as.small.Init(static_cast<u32>(as.va_start >> 12), static_cast<u32>(as.va_split >> 12));
        as.big.Init(static_cast<u32>(as.va_split >> as.big_bits),
                    static_cast<u32>((as.va_end - as.va_split) >> as.big_bits));
        as.init = true;
        return NvSuccess;
    }
    case 0x02: { // AllocSpace
        struct P { u32 pages, page_size, flags, pad; u64 offset; } p = In<P>(in);
        if (!as.init) return NvBadValue;
        if (p.page_size != 0x1000 && p.page_size != as.big_page_size) return NvBadValue;
        const bool big = p.page_size != 0x1000;
        if (!big && (p.flags & 2)) return NvNotImplemented;
        const u32 bits = big ? as.big_bits : 12;
        auto& a = big ? as.big : as.small;
        if (p.flags & 1) {
            a.AllocateFixed(static_cast<u32>(p.offset >> bits), p.pages);
        } else {
            p.offset = u64{a.Allocate(p.pages)} << bits;
            if (!p.offset) return NvInsufficientMemory;
        }
        as.allocs[p.offset] = AsGpu::Alloc{u64{p.pages} * p.page_size, p.page_size, (p.flags & 2) != 0, big, {}};
        Out(out, p);
        return NvSuccess;
    }
    case 0x03: { // FreeSpace
        struct P { u64 offset; u32 pages, page_size; } p = In<P>(in);
        auto it = as.allocs.find(p.offset);
        if (!as.init || it == as.allocs.end()) return NvBadValue;
        for (u64 m : it->second.maps) as.maps.erase(m);
        const u32 bits = it->second.big ? as.big_bits : 12;
        (it->second.big ? as.big : as.small).Free(static_cast<u32>(p.offset >> bits), static_cast<u32>(it->second.size >> bits));
        as.allocs.erase(it);
        return NvSuccess;
    }
    case 0x05: { // UnmapBuffer
        struct P { s64 offset; } p = In<P>(in);
        auto it = as.maps.find(static_cast<u64>(p.offset));
        if (it != as.maps.end()) {
            if (!it->second.fixed) {
                const u32 bits = it->second.big ? as.big_bits : 12;
                const u64 ps = it->second.big ? as.big_page_size : 0x1000;
                (it->second.big ? as.big : as.small).Free(static_cast<u32>(it->first >> bits),
                                                           static_cast<u32>(AlignUp(it->second.size, ps) >> bits));
            }
            as.maps.erase(it);
        }
        return NvSuccess;
    }
    case 0x06: { // MapBufferEx
        struct P { u32 flags, kind, handle, page_size; s64 buffer_offset; u64 mapping_size; s64 offset; } p = In<P>(in);
        if (!as.init) return NvBadValue;
        if (p.flags & 0x100) { // Remap: a subrange of an existing mapping
            return as.maps.count(static_cast<u64>(p.offset)) ? NvSuccess : NvBadValue;
        }
        auto hit = g_handles.find(p.handle);
        if (hit == g_handles.end()) return NvBadValue;
        const u64 size = p.mapping_size ? p.mapping_size : hit->second.orig_size;
        const bool big_ok = (hit->second.align % as.big_page_size) == 0;
        if (p.flags & 1) { // fixed
            auto a = as.allocs.upper_bound(static_cast<u64>(p.offset));
            if (a == as.allocs.begin()) return NvBadValue;
            --a;
            if (static_cast<u64>(p.offset) - a->first + size > a->second.size) return NvBadValue;
            a->second.maps.push_back(static_cast<u64>(p.offset));
            as.maps[static_cast<u64>(p.offset)] = AsGpu::Map{size, p.handle, true, a->second.big && big_ok};
        } else {
            const u64 ps = big_ok ? as.big_page_size : 0x1000;
            const u32 bits = big_ok ? as.big_bits : 12;
            const u32 at = (big_ok ? as.big : as.small).Allocate(static_cast<u32>(AlignUp(size, ps) >> bits));
            if (!at) return NvInsufficientMemory;
            p.offset = static_cast<s64>(u64{at} << bits);
            as.maps[static_cast<u64>(p.offset)] = AsGpu::Map{size, p.handle, false, big_ok};
        }
        Out(out, p);
        return NvSuccess;
    }
    case 0x08: { // GetVaRegions
        struct Region { u64 offset; u32 page_size, pad; u64 pages; };
        struct P { u64 buf_addr; u32 buf_size, reserved; Region r[2]; } p = In<P>(in);
        if (!as.init) return NvBadValue;
        p.buf_size = 2 * sizeof(Region);
        p.r[0] = Region{u64{as.small.Start()} << 12, 0x1000, 0, u64{as.small.Limit()} - as.small.Start()};
        p.r[1] = Region{u64{as.big.Start()} << as.big_bits, as.big_page_size, 0, u64{as.big.Limit()} - as.big.Start()};
        Out(out, p);
        if (inline_out) {
            std::memcpy(inline_out->data(), p.r, std::min(inline_out->size(), sizeof p.r));
        }
        return NvSuccess;
    }
    case 0x14: // Remap
        return NvSuccess;
    }
    return NvNotImplemented;
}

u32 CtrlGpuIoctl(u32 nr, const std::vector<u8>& in, std::vector<u8>& out, std::vector<u8>* inline_out) {
    out = in;
    switch (nr) {
    case 0x01: Out<u32>(out, 1); return NvSuccess; // ZcullGetCtxSize
    case 0x02: { // ZcullGetInfo
        const u32 v[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
        Out(out, v);
        return NvSuccess;
    }
    case 0x03: return NvSuccess; // ZbcSetTable
    case 0x04: return NvSuccess; // ZbcQueryTable
    case 0x05: { // GetCharacteristics
        struct Gc {
            u32 arch, impl, rev, num_gpc; u64 l2, vram; u32 tpc_per_gpc, bus, big_page, comp_page, pde_bits,
                big_sizes, gpc_mask, sm_ver, spa_ver, warps, va_bits, rsv; u64 flags;
            u32 twod, threed, compute, gpfifo, i2m, dma, max_fbps, fbp_mask, ltc_per_fbp, lts_per_ltc, tex_per_tpc,
                max_gpc, rop0, rop1; u64 chip, compbit;
        } gc{0x120, 0xB, 0xA1, 1, 0x40000, 0, 2, 0x20, 0x20000, 0x20000, 0x1B, 0x30000, 1, 0x503, 0x503, 0x80, 0x28, 0, 0x55,
             0x902D, 0xB197, 0xB1C0, 0xB06F, 0xA140, 0xB0B5, 1, 0, 2, 1, 0, 1, 0x21D70, 0, 0x6230326D67ULL, 0};
        static_assert(sizeof(Gc) == 160);
        struct P { u64 size, addr; Gc gc; } p{0xA0, 0xdeadbeef, gc};
        Out(out, p);
        if (inline_out && !inline_out->empty()) {
            std::memcpy(inline_out->data(), &gc, std::min(inline_out->size(), sizeof gc));
        }
        return NvSuccess;
    }
    case 0x06: { // GetTpcMasks
        struct P { u32 size, pad; u64 addr; u32 mask, pad2; } p = In<P>(in);
        if (p.size) p.mask = 3;
        Out(out, p);
        if (inline_out && inline_out->size() >= 4) std::memcpy(inline_out->data(), &p.mask, 4);
        return NvSuccess;
    }
    case 0x07: return NvSuccess; // FlushL2
    case 0x13: Out<u32>(out, 100); return NvSuccess; // PmuGetGpuLoad
    case 0x14: { // GetActiveSlotMask
        const u32 v[2] = {7, 1};
        Out(out, v);
        return NvSuccess;
    }
    case 0x1C: { // GetGpuTime
        const u64 ns = kernel::GuestTicks() * 625 / 12;
        Out(out, ns);
        return NvSuccess;
    }
    }
    return NvNotImplemented;
}

u32 GpuIoctl(u32 group, u32 nr, const std::vector<u8>& in, const std::vector<u8>* inline_in, std::vector<u8>& out) {
    out = in;
    if (group == 'G') {
        if (nr == 0x14) { g_client_data = In<u64>(in); return NvSuccess; }
        if (nr == 0x15) { Out(out, g_client_data); return NvSuccess; }
        return NvNotImplemented;
    }
    if (group == 0 && nr == 0x03) { // GetWaitbase
        const u32 v[2] = {In<u32>(in), 0};
        Out(out, v);
        return NvSuccess;
    }
    if (group != 'H') return NvNotImplemented;
    switch (nr) {
    case 0x01: case 0x03: case 0x0B: case 0x0C: case 0x0D: case 0x1D:
        return NvSuccess;
    case 0x09: { // AllocObjCtx
        struct P { u32 class_num, flags; u64 obj_id; } p = In<P>(in);
        p.obj_id = 0;
        Out(out, p);
        return NvSuccess;
    }
    case 0x18:   // AllocGpfifoEx
    case 0x1A: { // AllocGpfifoEx2
        struct P { u32 num_entries, num_jobs, flags, fence_id, fence_value, rsv[3]; } p = In<P>(in);
        p.fence_id = kChannelSyncpt;
        p.fence_value = g_syncpt_value;
        Out(out, p);
        return NvSuccess;
    }
    case 0x08:
    case 0x1B: { // SubmitGpfifo: nothing executes; the fence is already reached.
        struct P { u64 address; u32 num_entries, flags; u32 fence_id, fence_value; } p = In<P>(in);
        const bool fence_increment = p.flags & 2, increment_value = p.flags & 0x100;
        const u32 inc = (fence_increment ? 2 : 0) + (increment_value ? p.fence_value : 0);
        g_syncpt_value += inc;
        p.fence_id = kChannelSyncpt;
        p.fence_value = g_syncpt_value;
        (void)inline_in;
        out.resize(sizeof(P));
        std::memcpy(out.data(), &p, sizeof p);
        return NvSuccess;
    }
    }
    return NvNotImplemented;
}

u32 CtrlIoctl(u32 nr, const std::vector<u8>& in, std::vector<u8>& out) {
    out = in;
    switch (nr) {
    case 0x14: { // SyncptRead
        u32 v[2] = {In<u32>(in), g_syncpt_value};
        Out(out, v);
        return NvSuccess;
    }
    case 0x1B: return NvNotImplemented; // GetConfig
    default:
        return NvSuccess; // waits and event registration: every fence is already reached
    }
}

u32 Ioctl(u32 fd, u32 raw, const std::vector<u8>& in, const std::vector<u8>* inline_in, std::vector<u8>& out,
          std::vector<u8>* inline_out) {
    std::scoped_lock lk{g_lock};
    const auto it = g_fds.find(fd);
    if (it == g_fds.end()) return NvBadValue;
    const u32 nr = raw & 0xFF, group = (raw >> 8) & 0xFF;
    u32 r;
    switch (it->second.dev) {
    case Dev::Nvmap: r = group == 1 ? NvmapIoctl(nr, in, out) : NvNotImplemented; break;
    case Dev::AsGpu: r = group == 'A' ? AsGpuIoctl(nr, in, out, inline_out) : NvNotImplemented; break;
    case Dev::CtrlGpu: r = group == 'G' ? CtrlGpuIoctl(nr, in, out, inline_out) : NvNotImplemented; break;
    case Dev::Gpu: r = GpuIoctl(group, nr, in, inline_in, out); break;
    case Dev::Ctrl: r = CtrlIoctl(nr, in, out); break;
    default: out = in; r = NvSuccess; break;
    }
    if (r == NvNotImplemented) {
        static std::map<std::pair<std::string, u32>, bool> logged;
        if (!logged[{it->second.path, raw}]) {
            logged[{it->second.path, raw}] = true;
            LOGW("nvdrv", "%s ioctl %08x not implemented", it->second.path.c_str(), raw);
        }
    }
    return r;
}

class Nvdrv final : public ipc::Service {
public:
    using Service::Service;
    void Handle(ipc::Request& rq, ipc::Response& rs) override {
        switch (rq.cmd) {
        case 0: { // Open
            const auto buf = rq.ReadBuffer(0);
            std::string path(buf.begin(), buf.end());
            path = path.c_str();
            if (!initialized_) {
                rs.Push<u32>(0);
                rs.Push<u32>(NvNotInitialized);
                return;
            }
            Dev dev = Dev::Other;
            if (path == "/dev/nvmap") dev = Dev::Nvmap;
            else if (path == "/dev/nvhost-as-gpu") dev = Dev::AsGpu;
            else if (path == "/dev/nvhost-ctrl-gpu") dev = Dev::CtrlGpu;
            else if (path == "/dev/nvhost-gpu") dev = Dev::Gpu;
            else if (path == "/dev/nvhost-ctrl") dev = Dev::Ctrl;
            else if (path == "/dev/nvhost-prof-gpu") {
                rs.Push<u32>(0);
                rs.Push<u32>(NvNotSupported);
                return;
            }
            std::scoped_lock lk{g_lock};
            const u32 fd = g_next_fd++;
            g_fds[fd] = Fd{dev, path};
            LOGI("nvdrv", "open %s -> fd %u", path.c_str(), fd);
            rs.Push<u32>(fd);
            rs.Push<u32>(NvSuccess);
            return;
        }
        case 1:   // Ioctl
        case 11:  // Ioctl2
        case 12: { // Ioctl3
            const u32 fd = rq.Pop<u32>();
            const u32 raw = rq.Pop<u32>();
            if (!initialized_) {
                rs.Push<u32>(NvNotInitialized);
                return;
            }
            const auto in = rq.ReadBuffer(0);
            std::vector<u8> inline_in;
            if (rq.cmd == 11) inline_in = rq.ReadBuffer(1);
            std::vector<u8> out;
            std::vector<u8> inline_out(rq.cmd == 12 ? rq.OutBuffer(1).size : 0);
            const u32 r = Ioctl(fd, raw, in, rq.cmd == 11 ? &inline_in : nullptr, out,
                                rq.cmd == 12 ? &inline_out : nullptr);
            static const bool trace = std::getenv("PLAHOST_NVDRV_TRACE") != nullptr;
            if (trace) {
                std::string hex;
                char b[4];
                for (size_t k = 0; k < std::min<size_t>(out.size(), 48); ++k) {
                    std::snprintf(b, sizeof b, "%02x", out[k]);
                    hex += b;
                }
                LOGI("nvdrv", "ioctl%u fd %u %08x in %zu out %zu (buf %llu) -> %u: %s", rq.cmd == 1 ? 1 : rq.cmd - 9, fd,
                     raw, in.size(), out.size(), static_cast<unsigned long long>(rq.OutBuffer(0).size), r, hex.c_str());
            }
            if (raw & 0x80000000u) {
                rq.WriteBuffer(out.data(), out.size(), 0);
                if (rq.cmd == 12) rq.WriteBuffer(inline_out.data(), inline_out.size(), 1);
            }
            rs.Push<u32>(r);
            return;
        }
        case 2: { // Close
            const u32 fd = rq.Pop<u32>();
            std::scoped_lock lk{g_lock};
            g_fds.erase(fd);
            rs.Push<u32>(NvSuccess);
            return;
        }
        case 3: // Initialize
            initialized_ = true;
            rs.Push<u32>(NvSuccess);
            return;
        case 4: { // QueryEvent
            const u32 fd = rq.Pop<u32>();
            const u32 id = rq.Pop<u32>();
            std::scoped_lock lk{g_lock};
            rs.PushCopy(kernel::ReadableHandle(DevEvent(fd, id)));
            rs.Push<u32>(NvSuccess);
            return;
        }
        case 6: // GetStatus
        case 8: // SetAruid
            rs.Push<u32>(NvSuccess);
            return;
        case 9:  // DumpGraphicsMemoryInfo
        case 13: // SetGraphicsFirmwareMemoryMarginEnabled
            return;
        }
        Unknown(rq, rs);
    }

private:
    bool initialized_ = false;
};

} // namespace

std::shared_ptr<ipc::Service> MakeNvdrv(const std::string& name) {
    return std::make_shared<Nvdrv>(name);
}

} // namespace plahost::services
