// Guest memory imported into Vulkan (VK_EXT_external_memory_host).
//
// Each game MemoryPool lives in the runtime's fastmem arena (pla_api host_pointer): if the driver
// accepts importing that range, a VkBuffer is created over it and UBOs/SSBOs/vertices/indices are bound
// directly (no copies, and whatever the GPU writes -compute, transform feedback- lands in game memory).
// If the import fails, the backend keeps copying per submit (pipeline.cpp, Upload).
// SPDX-License-Identifier: MIT
#pragma once

#include <map>

#include "nvnvk.h"
#include "vkctx.h"

namespace nvnvk {

extern thread_local uint32_t t_find_kind;  // diagnostics: usage of the binding being looked up (8 VB, 16 IB, 32 UBO, 64/128 SSBO, 256 XFB)

struct HostRef {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint64_t avail = 0;  // valid bytes from off
    uint64_t cpu = 0;    // guest address corresponding to off (diagnostics)
    uint64_t want = 0;   // requested bytes that fall inside the pool (if want > avail, the binding crosses a run boundary)
    bool vram = false;   // vram_pools copy (device-local), not imported host memory
};

class HostMemory {
public:
    void Init();  // after VkCtx::Init
    void Shutdown();
    bool enabled() const { return enabled_; }
    // Called when a pool is registered (MemoryPoolInitialize).
    void AddPool(const PoolInfo& p);
    void RemovePool(uint64_t obj);
    // Submit thread (with submit_mu), before recording: imports the pools registered since the last submit.
    void ProcessPending();
    // Imported buffer containing [gpu, gpu+size). false if none (or alignment is not met).
    // clamp: it is enough to start inside (unified memory: a binding longer than the pool is clamped to what is available).
    bool Find(uint64_t gpu, uint64_t size, VkDeviceSize align, HostRef* out, bool clamp = false);
    uint64_t imported_bytes() const { return imported_bytes_; }
    // Diagnostics (import_verify): the GPU copies up to 64 KiB of each pool (up to max_pools, round-robin) and compares
    // it with what the CPU sees through the same pointer. Call with submit_mu held, on the submit thread.
    void Verify(uint32_t max_pools);
    // Submit thread (with submit_mu): hands retired imported buffers to the current frame.
    void CollectDead(std::vector<Buffer>& out);
    // vram_pools (sprint 8): CPU_NO_ACCESS pools (the game CPU never writes them; only the GPU: copies, XFB, compute)
    // live in VRAM. Submit thread, at the start of a submit: creates and fills (GPU copy from game memory) the new
    // pools; from that submit on, Find binds them in VRAM. Reading vertices from RAM over PCIe cost ~6 ms/frame in the village.
    void InitVram(VkCommandBuffer cmd);
    void CollectDeadSub(std::vector<std::pair<VkBuffer, MemSub>>& out);
    // vbib_snapshot (sprint 8): 0 = unknown, 1 = pool in VRAM, 2 = in shared memory and written by the GPU at some point,
    // 3 = in shared memory and never written by the GPU (its vertices/indices are copied at record time: reading them
    // directly from imported memory, 4 KiB pages, cost ~6 ms of GPU per frame in the village).
    // By exact ranges (the game mixes static meshes and XFB outputs in the same large pool).
    int Residency(uint64_t gpu, uint64_t size);
    // FPS-1: Residency/Find are memoized per thread (they cost two mutexes and a map walk, several times per draw). gen_
    // changes whenever their answer can change (Resort, a pool moved to VRAM, a new GPU-written range).
    std::atomic<uint64_t> gen_{1};
    void MarkGpuWritten(uint64_t gpu, uint64_t size);

private:
    int ResidencySlow(uint64_t gpu, uint64_t size);
    bool FindSlow(uint64_t gpu, uint64_t size, VkDeviceSize align, HostRef* out, bool clamp);
    struct Entry {
        uint64_t obj = 0, cpu = 0, size = 0;
        uint64_t gpu[3] = {};
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkBuffer buf = VK_NULL_HANDLE;
        const uint8_t* host = nullptr;
        uint32_t flags = 0;
        // Backing mode (import_backing): physically contiguous runs of the pool in backing memory.
        struct Run {
            uint64_t off, len;
            const uint8_t* host;
        };
        std::vector<Run> runs;
        bool vram = false, vready = false;  // vram_pools: candidate / VRAM buffer ready
        bool gpu_written = false;           // the GPU wrote to it (XFB, SSBO, copy, write merge)
        VkBuffer vbuf = VK_NULL_HANDLE;
        MemSub vsub;
    };
    std::vector<std::pair<VkBuffer, MemSub>> vgrave_;
    std::mutex wmu_;
    std::map<uint64_t, uint64_t> written_;  // ranges written by the GPU: start -> end (merged)
    uint64_t vram_bytes_ = 0, vram_pools_n_ = 0;
    // Backing mode (sprint 7): all of the game's physical memory imported ONCE, in chunks (importing per pool while the
    // GPU is busy costs ~30 ms each in the driver; the game creates pools constantly while loading and moving around).
    struct Chunk {
        const uint8_t* base = nullptr;
        uint64_t size = 0;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkBuffer buf = VK_NULL_HANDLE;
    };
    std::vector<Chunk> chunks_;
    const uint8_t* bk_base_ = nullptr;
    uint64_t bk_size_ = 0;
    bool bk_tried_ = false;
    std::atomic<uint64_t> bk_clamped_{0}, bk_miss_{0};
    void InitBacking(const uint8_t* sample);
    bool ImportRange(const uint8_t* ptr, uint64_t size, VkDeviceMemory* mem, VkBuffer* buf);
    std::vector<Buffer> graveyard_;
    void ImportNow(const PoolInfo& p);
    std::mutex pmu_;
    std::vector<PoolInfo> pending_;
    double pending_ms_ = 0;
    uint64_t pending_batches_ = 0;
    size_t verify_next_ = 0;
    uint64_t verify_bad_ = 0, verify_ok_ = 0;
    std::mutex mu_;
    std::unordered_map<uint64_t, Entry> pools_;
    struct Range {
        uint64_t gpu, size;
        VkBuffer buf;
        uint64_t cpu;
        uint64_t obj;
    };
    std::vector<Range> sorted_;
    void Resort();
    bool enabled_ = false;
    VkDeviceSize align_ = 4096;
    PFN_vkGetMemoryHostPointerPropertiesEXT get_props_ = nullptr;
    uint64_t imported_bytes_ = 0, failed_ = 0;
};
extern HostMemory g_hostmem;

}  // namespace nvnvk
