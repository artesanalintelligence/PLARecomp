// SPDX-License-Identifier: MIT
#include "hostmem.h"

#include <algorithm>
#include <chrono>

#include <windows.h>

namespace nvnvk {

HostMemory g_hostmem;
thread_local uint32_t t_find_kind = 0;

void HostMemory::Init() {
    if (!g_cfg.import_memory || !g_vk.has_external_memory_host || !g_guest.host_ptr) {
        NVK_INFO("imported memory: disabled (config=%d, extension=%d, host_pointer=%d)", (int)g_cfg.import_memory,
                 (int)g_vk.has_external_memory_host, (int)g_guest.host_ptr);
        return;
    }
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    align_ = std::max<VkDeviceSize>(hp.minImportedHostPointerAlignment, 1);
    get_props_ = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    enabled_ = get_props_ != nullptr;
    NVK_INFO("imported memory: %s (alignment %llu)", enabled_ ? "active" : "no function", (unsigned long long)align_);
}

void HostMemory::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [k, e] : pools_) {
        if (e.buf) vkDestroyBuffer(g_vk.dev, e.buf, nullptr);
        if (e.mem) vkFreeMemory(g_vk.dev, e.mem, nullptr);
    }
    pools_.clear();
    sorted_.clear();
}

void HostMemory::Resort() {
    sorted_.clear();
    for (auto& [k, e] : pools_)
        for (uint64_t g : e.gpu)
            if (g && (e.buf || !e.runs.empty())) sorted_.push_back({g, e.size, e.buf, e.cpu, e.obj});
    std::sort(sorted_.begin(), sorted_.end(), [](const Range& a, const Range& b) { return a.gpu < b.gpu; });
    gen_.fetch_add(1, std::memory_order_release);
}

namespace {
struct ResMemo {
    uint64_t gpu = 0, size = 0, gen = 0;
    int res = 0;
};
struct FindMemo {
    uint64_t gpu = 0, size = 0, align = 0, gen = 0;
    bool clamp = false, ok = false;
    HostRef ref;
};
thread_local ResMemo t_res_memo[512];
thread_local FindMemo t_find_memo[512];
inline size_t MemoSlot(uint64_t gpu, uint64_t size) { return (size_t)(((gpu >> 4) ^ (gpu >> 13) ^ (size * 0x9E3779B1ull)) & 511); }
}  // namespace

int HostMemory::Residency(uint64_t gpu, uint64_t size) {
    if (!enabled_) return 0;
    const uint64_t g = gen_.load(std::memory_order_acquire);
    ResMemo& m = t_res_memo[MemoSlot(gpu, size)];
    if (m.gen == g && m.gpu == gpu && m.size == size) return m.res;
    const int r = ResidencySlow(gpu, size);
    m.gpu = gpu;
    m.size = size;
    m.gen = g;  // read before computing: a change made meanwhile invalidates the entry
    m.res = r;
    return r;
}

bool HostMemory::Find(uint64_t gpu, uint64_t size, VkDeviceSize align, HostRef* out, bool clamp) {
    if (!enabled_) return false;
    const uint64_t g = gen_.load(std::memory_order_acquire);
    FindMemo& m = t_find_memo[MemoSlot(gpu, size ^ (align << 48) ^ (clamp ? 1ull << 63 : 0))];
    if (m.gen == g && m.gpu == gpu && m.size == size && m.align == align && m.clamp == clamp) {
        if (m.ok) *out = m.ref;
        return m.ok;
    }
    HostRef r;
    const bool ok = FindSlow(gpu, size, align, &r, clamp);
    m.gpu = gpu;
    m.size = size;
    m.align = align;
    m.clamp = clamp;
    m.gen = g;
    m.ok = ok;
    m.ref = r;
    if (ok) *out = r;
    return ok;
}

void HostMemory::AddPool(const PoolInfo& p) {
    if (!enabled_ || !p.cpu || !p.size) return;
    if (g_cfg.import_backing) {
        // Contiguous runs in backing memory, page by page (cheap: no Vulkan calls).
        Entry e;
        e.obj = p.obj;
        e.cpu = p.cpu;
        e.size = p.size;
        e.gpu[0] = p.gpu;
        e.gpu[1] = p.alias[0];
        e.gpu[2] = p.alias[1];
        e.flags = p.flags;
        if (!PLA_API_HAS(g_guest.api, host_backing_pointer) || !g_guest.api->host_backing_pointer) return;
        const uint8_t* whole = static_cast<const uint8_t*>(g_guest.api->host_backing_pointer(p.cpu, p.size));
        if (whole) {
            e.runs.push_back({0, p.size, whole});
        } else {
            for (uint64_t o = 0; o < p.size; o += 4096) {
                const uint8_t* b = static_cast<const uint8_t*>(g_guest.api->host_backing_pointer(p.cpu + o, 4096));
                if (!b) {
                    e.runs.clear();
                    break;
                }
                if (!e.runs.empty() && e.runs.back().host + e.runs.back().len == b)
                    e.runs.back().len += 4096;
                else
                    e.runs.push_back({o, std::min<uint64_t>(4096, p.size - o), b});
            }
        }
        if (e.runs.empty()) {
            ++failed_;
            return;
        }
        e.host = e.runs[0].host;
        // CPU_NO_ACCESS (1) with GPU access (no GPU_NO_ACCESS 8): VRAM candidate (InitVram).
        e.vram = g_cfg.vram_pools && g_cfg.unified_memory && (p.flags & 1) && !(p.flags & 8);
        std::lock_guard<std::mutex> lk(mu_);
        auto old = pools_.find(p.obj);
        if (old != pools_.end() && old->second.vbuf) {
            vgrave_.push_back({old->second.vbuf, old->second.vsub});
            vram_bytes_ -= old->second.size;
        }
        pools_[p.obj] = std::move(e);
        imported_bytes_ += p.size;
        Resort();
        return;
    }
    // The import is done on the submit thread (ProcessPending): on the game thread, with the GPU busy, the driver
    // serialized it with rendering (~16 ms + ~16 ms per pool; >1,400 pools while loading = 30-48 s of loading screen).
    std::lock_guard<std::mutex> lk(pmu_);
    for (auto& q : pending_)
        if (q.obj == p.obj) {
            q = p;
            return;
        }
    pending_.push_back(p);
}

bool HostMemory::ImportRange(const uint8_t* ptr, uint64_t size, VkDeviceMemory* mem, VkBuffer* buf) {
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = get_props_(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void*)ptr, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) {
        NVK_WARN("backing: vkGetMemoryHostPointerPropertiesEXT(%p) = %d", ptr, (int)r);
        return false;
    }
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &eb;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | 
                VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (vkCreateBuffer(g_vk.dev, &bci, nullptr, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev, *buf, &req);
    const uint32_t bits = hp.memoryTypeBits & req.memoryTypeBits;
    uint32_t type = g_vk.MemType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) type = g_vk.MemType(bits, 0);
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = (void*)ptr;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;
    r = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g_vk.dev, &ai, nullptr, mem);
    if (r != VK_SUCCESS || vkBindBufferMemory(g_vk.dev, *buf, *mem, 0) != VK_SUCCESS) {
        NVK_WARN("backing: importing %p (%llu MiB) failed: %d", ptr, (unsigned long long)(size >> 20), (int)r);
        vkDestroyBuffer(g_vk.dev, *buf, nullptr);
        *buf = VK_NULL_HANDLE;
        if (*mem) vkFreeMemory(g_vk.dev, *mem, nullptr);
        *mem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void HostMemory::InitBacking(const uint8_t* sample) {
    bk_tried_ = true;
    // The whole backing view: contiguous regions with the same AllocationBase as the sample pointer.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(sample, &mbi, sizeof mbi)) return;
    const uint8_t* base = static_cast<const uint8_t*>(mbi.AllocationBase);
    uint64_t size = 0;
    for (const uint8_t* q = base;;) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(q, &m, sizeof m) || m.AllocationBase != mbi.AllocationBase || m.State != MEM_COMMIT) break;
        size += m.RegionSize;
        q += m.RegionSize;
    }
    NVK_INFO("backing: view %p, %llu MiB (type %lx)", base, (unsigned long long)(size >> 20), mbi.Type);
    if (!size) return;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t kChunk = (uint64_t)g_cfg.import_chunk_mb << 20;
    for (uint64_t o = 0; o < size; o += kChunk) {
        Chunk c;
        c.base = base + o;
        c.size = std::min<uint64_t>(kChunk, size - o);
        if (!ImportRange(c.base, c.size, &c.mem, &c.buf)) {
            chunks_.clear();  // (imported ones stay until shutdown; without full backing it is not used)
            return;
        }
        chunks_.push_back(c);
    }
    bk_base_ = base;
    bk_size_ = size;
    NVK_INFO("backing: %zu chunks of %u MiB imported in %.1f ms", chunks_.size(), g_cfg.import_chunk_mb,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void HostMemory::ProcessPending() {
    if (g_cfg.import_backing) {
        if (!bk_tried_) {
            const uint8_t* sample = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& [k, e] : pools_)
                    if (e.host) {
                        sample = e.host;
                        break;
                    }
            }
            if (sample) InitBacking(sample);
        }
        static uint64_t tick = 0;
        if ((++tick % 6000) == 0)
            NVK_INFO("backing: bindings clamped at a run boundary %llu, outside %llu", (unsigned long long)bk_clamped_.load(),
                     (unsigned long long)bk_miss_.load());
        return;
    }
    std::vector<PoolInfo> todo;
    {
        std::lock_guard<std::mutex> lk(pmu_);
        todo.swap(pending_);
    }
    if (todo.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    for (const PoolInfo& p : todo) ImportNow(p);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    pending_ms_ += ms;
    if (ms > 5 || (++pending_batches_ % 100) == 0)
        NVK_INFO("imported memory: batch of %zu pools in %.1f ms (%.0f ms total, %llu MB imported)", todo.size(), ms, pending_ms_,
                 (unsigned long long)(imported_bytes_ >> 20));
}

void HostMemory::ImportNow(const PoolInfo& p) {
    // The physical backing memory is imported (host_backing_pointer: a single view that suyu never
    // reprotects or remaps), not the fastmem arena (host_pointer), which suyu protects and splits into
    // placeholders: importing the arena crashed the game after ~4 s (later reads hit AVs).
    if (g_cfg.import_verify && PLA_API_HAS(g_guest.api, host_backing_pointer) && g_guest.api->host_backing_pointer &&
        p.size >= (1u << 20)) {
        // Physically contiguous runs of the pool in backing memory and their alignment (would a 64 KiB sparse buffer work?).
        uint64_t runs = 0, mis64 = 0, max_run = 0, run = 0;
        const uint8_t* prev = nullptr;
        for (uint64_t o = 0; o < p.size; o += 4096) {
            const uint8_t* b = static_cast<const uint8_t*>(g_guest.api->host_backing_pointer(p.cpu + o, 4096));
            if (!prev || b != prev + 4096) {
                if (run > max_run) max_run = run;
                run = 0;
                ++runs;
                if (((uintptr_t)b & 0xFFFF) != ((p.cpu + o) & 0xFFFF) || (o & 0xFFFF)) ++mis64;
            }
            run += 4096;
            prev = b;
        }
        if (run > max_run) max_run = run;
        NVK_INFO("backing of pool %llx (%llu KiB): %llu runs, %llu not 64 KiB aligned, largest %llu KiB", (unsigned long long)p.obj,
                 (unsigned long long)(p.size >> 10), (unsigned long long)runs, (unsigned long long)mis64,
                 (unsigned long long)(max_run >> 10));
    }
    auto tq = std::chrono::steady_clock::now();
    auto lap = [&tq]() {
        const auto n = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(n - tq).count();
        tq = n;
        return ms;
    };
    double ms_props = 0, ms_buf = 0, ms_alloc = 0;
    void* ptr = nullptr;
    if (PLA_API_HAS(g_guest.api, host_backing_pointer) && g_guest.api->host_backing_pointer)
        ptr = g_guest.api->host_backing_pointer(p.cpu, p.size);
    if (!ptr && g_cfg.import_arena) ptr = g_guest.api->host_pointer(p.cpu, p.size);
    if (!ptr || ((uintptr_t)ptr % align_) || (p.size % align_)) {
        ++failed_;
        NVK_DBG("pool %llx not importable (pointer %p, size %llx)", (unsigned long long)p.obj, ptr, (unsigned long long)p.size);
        return;
    }
    auto read_us = [&](const void* q, size_t n) {
        static volatile uint64_t sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t acc = 0;
        const uint64_t* w = static_cast<const uint64_t*>(q);
        for (size_t k = 0; k < n / 8; k += 8) acc += w[k];
        sink = sink + acc;
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    };
    static int timed = 0;
    const bool time_it = g_cfg.import_verify && p.size >= (1u << 20) && timed < 20;
    const size_t tn = (size_t)std::min<uint64_t>(p.size, 1u << 20);
    double before = 0;
    if (time_it) { read_us(ptr, tn); before = read_us(ptr, tn); }
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    lap();
    VkResult r = get_props_(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp);
    ms_props = lap();
    if (r != VK_SUCCESS || !hp.memoryTypeBits) {
        if (failed_++ < 8) NVK_WARN("vkGetMemoryHostPointerPropertiesEXT(%p) = %d, types %x", ptr, (int)r, hp.memoryTypeBits);
        return;
    }
    Entry e;
    e.obj = p.obj;
    e.cpu = p.cpu;
    e.size = p.size;
    e.gpu[0] = p.gpu;
    e.gpu[1] = p.alias[0];
    e.gpu[2] = p.alias[1];
    e.host = static_cast<const uint8_t*>(ptr);
    e.flags = p.flags;
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &eb;
    bci.size = p.size;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | 
                VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (vkCreateBuffer(g_vk.dev, &bci, nullptr, &e.buf) != VK_SUCCESS) {
        ++failed_;
        return;
    }
    ms_buf = lap();
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.dev, e.buf, &req);
    const uint32_t bits = hp.memoryTypeBits & req.memoryTypeBits;
    uint32_t type = g_vk.MemType(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) type = g_vk.MemType(bits, 0);
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = ptr;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = p.size;
    ai.memoryTypeIndex = type;
    r = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g_vk.dev, &ai, nullptr, &e.mem);
    ms_alloc = lap();
    {
        static int nlog = 0;
        if (g_cfg.import_verify && nlog < 60 && ms_props + ms_buf + ms_alloc > 5) {
            ++nlog;
            NVK_INFO("import pool %llx (%llu KiB): properties %.1f ms, buffer %.1f ms, vkAllocateMemory %.1f ms",
                     (unsigned long long)p.obj, (unsigned long long)(p.size >> 10), ms_props, ms_buf, ms_alloc);
        }
    }
    if (r != VK_SUCCESS || vkBindBufferMemory(g_vk.dev, e.buf, e.mem, 0) != VK_SUCCESS) {
        if (failed_++ < 8) NVK_WARN("import pool %llx (%llu KiB) failed: %d", (unsigned long long)p.obj,
                                    (unsigned long long)(p.size >> 10), (int)r);
        vkDestroyBuffer(g_vk.dev, e.buf, nullptr);
        if (e.mem) vkFreeMemory(g_vk.dev, e.mem, nullptr);
        return;
    }
    if (time_it) {
        ++timed;
        const double after = read_us(ptr, tn);
        NVK_INFO("CPU read of 1 MiB of pool %llx: %.0f us before import, %.0f us after (memory type %u)",
                 (unsigned long long)p.obj, before, after, type);
    }
    // No submit_mu: taking it here (game thread) waited for the render thread to finish a whole frame for each
    // pool (~30 ms; >1,000 pools while loading = 36 s of loading, the "black screen with the area banner"). Whatever is
    // retired goes to graveyard_ and the submit thread hands it to its frame (CollectDead).
    std::lock_guard<std::mutex> lk(mu_);
    auto old = pools_.find(p.obj);
    if (old != pools_.end()) graveyard_.push_back(Buffer{old->second.buf, old->second.mem, old->second.size, nullptr});
    imported_bytes_ += p.size;
    pools_[p.obj] = e;
    Resort();
    NVK_DBG("pool %llx imported: %llu KiB, type %u", (unsigned long long)p.obj, (unsigned long long)(p.size >> 10), type);
}

void HostMemory::RemovePool(uint64_t obj) {
    {
        std::lock_guard<std::mutex> lk(pmu_);
        for (size_t i = 0; i < pending_.size(); ++i)
            if (pending_[i].obj == obj) {
                pending_.erase(pending_.begin() + i);
                break;
            }
    }
    // The GPU may be using it: it is destroyed when the current submit finishes (CollectDead).
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pools_.find(obj);
    if (it == pools_.end()) return;
    if (it->second.buf) graveyard_.push_back(Buffer{it->second.buf, it->second.mem, it->second.size, nullptr});
    if (it->second.vbuf) {
        vgrave_.push_back({it->second.vbuf, it->second.vsub});
        vram_bytes_ -= it->second.size;
    }
    imported_bytes_ -= it->second.size;
    pools_.erase(it);
    Resort();
}

int HostMemory::ResidencySlow(uint64_t gpu, uint64_t size) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = std::upper_bound(sorted_.begin(), sorted_.end(), gpu, [](uint64_t g, const Range& r) { return g < r.gpu; });
        if (it == sorted_.begin()) return 0;
        --it;
        if (gpu - it->gpu >= it->size) return 0;
        auto pe = pools_.find(it->obj);
        if (pe == pools_.end()) return 0;
        if (pe->second.vready) return 1;
    }
    std::lock_guard<std::mutex> lk(wmu_);
    const uint64_t end = gpu + std::max<uint64_t>(size, 1);
    auto w = written_.lower_bound(end);  // first range starting at or after the end
    if (w != written_.begin()) {
        --w;
        if (w->second > gpu) return 2;  // starts before the end and ends after the start
    }
    return 3;
}

void HostMemory::MarkGpuWritten(uint64_t gpu, uint64_t size) {
    if (!enabled_ || !gpu || !size) return;
    std::lock_guard<std::mutex> lk(wmu_);
    uint64_t lo = gpu, hi = gpu + size;
    auto it = written_.upper_bound(lo);
    if (it != written_.begin()) {
        auto p = std::prev(it);
        if (p->second >= lo) {
            if (p->second >= hi) return;  // already covered (no generation change)
            lo = p->first;
            it = p;
        }
    }
    while (it != written_.end() && it->first <= hi) {
        hi = std::max(hi, it->second);
        it = written_.erase(it);
    }
    written_[lo] = hi;
    gen_.fetch_add(1, std::memory_order_release);
}

void HostMemory::CollectDeadSub(std::vector<std::pair<VkBuffer, MemSub>>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (vgrave_.empty()) return;
    out.insert(out.end(), vgrave_.begin(), vgrave_.end());
    vgrave_.clear();
}

void HostMemory::InitVram(VkCommandBuffer cmd) {
    if (!enabled_ || !g_cfg.import_backing || !g_cfg.vram_pools || chunks_.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t kChunk = (uint64_t)g_cfg.import_chunk_mb << 20;
    bool any = false;
    uint64_t made = 0, bytes = 0;
    for (auto& [k, e] : pools_) {
        if (!e.vram || e.vbuf) continue;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = e.size;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (vkCreateBuffer(g_vk.dev, &bci, nullptr, &e.vbuf) != VK_SUCCESS) {
            e.vbuf = VK_NULL_HANDLE;
            e.vram = false;
            continue;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_vk.dev, e.vbuf, &req);
        e.vsub = g_vk.AllocBufferMem(req);
        if (!e.vsub.mem || vkBindBufferMemory(g_vk.dev, e.vbuf, e.vsub.mem, e.vsub.off) != VK_SUCCESS) {
            vkDestroyBuffer(g_vk.dev, e.vbuf, nullptr);
            if (e.vsub.mem) g_vk.FreeImage(e.vsub);
            e.vbuf = VK_NULL_HANDLE;
            e.vram = false;
            continue;
        }
        // Initial contents: what is in game memory (data may be loaded before the pool is created).
        bool ok = true;
        std::vector<VkBufferCopy> cps;
        std::vector<VkBuffer> srcs;
        for (const Entry::Run& r : e.runs) {
            for (uint64_t o = 0; o < r.len;) {
                const uint8_t* h = r.host + o;
                if (h < bk_base_ || h >= bk_base_ + bk_size_) {
                    ok = false;
                    break;
                }
                const Chunk& c = chunks_[(size_t)((h - bk_base_) / kChunk)];
                const uint64_t coff = (uint64_t)(h - c.base);
                const uint64_t n = std::min<uint64_t>(r.len - o, c.size - coff);
                cps.push_back(VkBufferCopy{coff, r.off + o, n});
                srcs.push_back(c.buf);
                o += n;
            }
            if (!ok) break;
        }
        if (!ok) {
            vgrave_.push_back({e.vbuf, e.vsub});
            e.vbuf = VK_NULL_HANDLE;
            e.vram = false;
            continue;
        }
        if (!any) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            any = true;
        }
        for (size_t i = 0; i < cps.size(); ++i) vkCmdCopyBuffer(cmd, srcs[i], e.vbuf, 1, &cps[i]);
        e.vready = true;
        gen_.fetch_add(1, std::memory_order_release);
        vram_bytes_ += e.size;
        ++vram_pools_n_;
        ++made;
        bytes += e.size;
    }
    if (any) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        static uint64_t logs = 0;
        if (logs++ < 20 || (logs % 200) == 0)
            NVK_INFO("vram_pools: %llu new pools in VRAM (%llu KiB); total %llu pools, %llu MiB", (unsigned long long)made,
                     (unsigned long long)(bytes >> 10), (unsigned long long)vram_pools_n_, (unsigned long long)(vram_bytes_ >> 20));
    }
}

void HostMemory::CollectDead(std::vector<Buffer>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (graveyard_.empty()) return;
    out.insert(out.end(), graveyard_.begin(), graveyard_.end());
    graveyard_.clear();
}

void HostMemory::Verify(uint32_t max_pools) {
    if (!enabled_ || g_cfg.import_backing) return;
    std::vector<Entry> list;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [k, e] : pools_) list.push_back(e);
    }
    if (list.empty()) return;
    std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.cpu < b.cpu; });
    const uint32_t n = std::min<uint32_t>(max_pools, (uint32_t)list.size());
    constexpr VkDeviceSize kChunk = 64u << 10;
    Buffer st = g_vk.CreateBuffer(kChunk * n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
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
    std::vector<size_t> idx(n);
    for (uint32_t i = 0; i < n; ++i) {
        idx[i] = (verify_next_ + i) % list.size();
        const Entry& e = list[idx[i]];
        // Look at the end of the pool (the start is usually zero on both sides).
        const VkDeviceSize len = std::min<VkDeviceSize>(kChunk, e.size), off = e.size - len;
        const VkBufferCopy c{off, kChunk * i, len};
        vkCmdCopyBuffer(cb, e.buf, st.buf, 1, &c);
    }
    verify_next_ = (verify_next_ + n) % list.size();
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.queue);
    for (uint32_t i = 0; i < n; ++i) {
        const Entry& e = list[idx[i]];
        const VkDeviceSize len = std::min<VkDeviceSize>(kChunk, e.size), off = e.size - len;
        const uint8_t* g = st.map + kChunk * i;
        const uint8_t* h = e.host + off;
        size_t diff = 0, first = SIZE_MAX, nzh = 0, nzg = 0;
        for (size_t b = 0; b < len; ++b) {
            nzh += h[b] != 0;
            nzg += g[b] != 0;
            if (g[b] != h[b]) {
                ++diff;
                if (first == SIZE_MAX) first = b;
            }
        }
        if (diff) {
            if (verify_bad_++ < 60)
                NVK_WARN("verify: pool %llx (cpu %llx, %llu KiB, flags %x, gpu %llx) DIFFERENT: %zu bytes of %llu (first +%zu; "
                         "non-zero CPU %zu, GPU %zu)", (unsigned long long)e.obj, (unsigned long long)e.cpu,
                         (unsigned long long)(e.size >> 10), e.flags, (unsigned long long)e.gpu[0], diff,
                         (unsigned long long)len, first == SIZE_MAX ? 0 : (size_t)(first + off), nzh, nzg);
        } else {
            ++verify_ok_;
        }
    }
    // Arena (what is imported) vs. physical backing memory (what the old path copies), page by page.
    uint64_t pages = 0, bad_pages = 0, no_backing = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const Entry& e = list[idx[i]];
        uint64_t pool_bad = 0, first_bad = ~0ull;
        for (uint64_t o = 0; o < e.size; o += 4096) {
            const uint8_t* b = PLA_API_HAS(g_guest.api, host_backing_pointer) && g_guest.api->host_backing_pointer
                                   ? static_cast<const uint8_t*>(g_guest.api->host_backing_pointer(e.cpu + o, 4096))
                                   : nullptr;
            ++pages;
            if (!b) {
                ++no_backing;
                continue;
            }
            if (b != e.host + o && std::memcmp(b, e.host + o, 4096)) {
                ++bad_pages;
                ++pool_bad;
                if (first_bad == ~0ull) first_bad = o;
            }
        }
        if (pool_bad && verify_bad_++ < 120)
            NVK_WARN("verify: pool %llx (cpu %llx, %llu KiB, flags %x): %llu arena pages DIFFERENT from backing "
                     "(first +%llx)", (unsigned long long)e.obj, (unsigned long long)e.cpu, (unsigned long long)(e.size >> 10),
                     e.flags, (unsigned long long)pool_bad, (unsigned long long)first_bad);
    }
    NVK_INFO("verify arena/backing: %llu pages, %llu different, %llu without backing", (unsigned long long)pages,
             (unsigned long long)bad_pages, (unsigned long long)no_backing);
    NVK_INFO("imported memory verify: %u pools checked, %llu equal and %llu different in total", n,
             (unsigned long long)verify_ok_, (unsigned long long)verify_bad_);
    vkDestroyCommandPool(g_vk.dev, pool, nullptr);
    g_vk.DestroyBuffer(st);
}

bool HostMemory::FindSlow(uint64_t gpu, uint64_t size, VkDeviceSize align, HostRef* out, bool clamp) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::upper_bound(sorted_.begin(), sorted_.end(), gpu, [](uint64_t g, const Range& r) { return g < r.gpu; });
    if (it == sorted_.begin()) return false;
    --it;
    const uint64_t off = gpu - it->gpu;
    if (off >= it->size || (!clamp && off + size > it->size)) return false;
    if (g_cfg.import_backing) {
        if (chunks_.empty()) return false;
        auto pe = pools_.find(it->obj);
        if (pe == pools_.end()) return false;
        if (pe->second.vready) {
            // Pool in VRAM (vram_pools): contiguous, no runs.
            if (align > 1 && (off % align)) return false;
            out->buf = pe->second.vbuf;
            out->off = off;
            out->avail = it->size - off;
            out->cpu = it->cpu + off;
            out->want = std::min<uint64_t>(size, it->size - off);
            out->vram = true;
            return true;
        }
        const auto& runs = pe->second.runs;
        auto rr = std::upper_bound(runs.begin(), runs.end(), off, [](uint64_t o, const Entry::Run& r) { return o < r.off; });
        if (rr == runs.begin()) return false;
        --rr;
        if (off >= rr->off + rr->len) return false;
        const uint8_t* h = rr->host + (off - rr->off);
        if (h < bk_base_ || h >= bk_base_ + bk_size_) {
            ++bk_miss_;
            return false;
        }
        const uint64_t kChunk = (uint64_t)g_cfg.import_chunk_mb << 20;
        const Chunk& c = chunks_[(size_t)((h - bk_base_) / kChunk)];
        const uint64_t coff = (uint64_t)(h - c.base);
        const uint64_t avail = std::min<uint64_t>(rr->off + rr->len - off, c.size - coff);
        const uint64_t want = std::min<uint64_t>(size, it->size - off);
        if (want > avail) {
            if (!clamp) return false;
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 20)
                NVK_INFO("backing: binding (usage %u) %llx+%llx crosses a run boundary: %llu B left (run %llu KiB of %zu)", t_find_kind,
                         (unsigned long long)gpu, (unsigned long long)size, (unsigned long long)avail,
                         (unsigned long long)(rr->len >> 10), runs.size());
            ++bk_clamped_;
        }
        if (align > 1 && (coff % align)) return false;
        out->buf = c.buf;
        out->off = coff;
        out->avail = avail;
        out->cpu = it->cpu + off;
        out->want = want;
        return true;
    }
    if (align > 1 && (off % align)) return false;
    out->buf = it->buf;
    out->off = off;
    out->avail = it->size - off;
    out->cpu = it->cpu + off;
    out->want = std::min<uint64_t>(size, it->size - off);
    return true;
}

}  // namespace nvnvk
