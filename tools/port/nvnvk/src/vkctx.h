// Backend Vulkan context: device, memory, upload ring, window and swapchain.
// SPDX-License-Identifier: MIT
#pragma once

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <map>
#include <windows.h>
#endif
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nvnvk {

#define VKCHECK(x)                                                                            \
    do {                                                                                      \
        VkResult _r = (x);                                                                    \
        if (_r != VK_SUCCESS) ::nvnvk::VkFail(#x, _r, __FILE__, __LINE__);                    \
    } while (0)
void VkFail(const char* what, VkResult r, const char* file, int line);

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint8_t* map = nullptr;
};

// Image memory chunk: suballocated from a large block (block >= 0) or dedicated (block < 0).
struct MemSub {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0, size = 0;
    int block = -1;
};

// Per frame-in-flight block: command buffer, fence and upload ring.
struct FrameSlot {
    uint64_t arena_head_at_begin = 0;  // VkCtx::arena_head when this slot's current use began (snap_cache)
    uint64_t dev_head_at_begin = 0;    // same for the device-local arena (snap_vram)
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t timeline_value = 0;
    bool timed = false;  // timestamps were written in this slot
    Buffer upload;
    VkDeviceSize upload_used = 0;
    bool upload_grow = false;  // the ring filled up: double it (up to 256 MiB) when the slot is reused
    std::vector<VkSampler> dead_samplers;
    std::vector<std::pair<VkImage, MemSub>> dead_images;
    std::vector<VkImageView> dead_views;
    std::vector<VkBufferView> dead_bviews;  // texel buffer views of this frame
    std::vector<Buffer> dead_buffers;
    std::vector<std::pair<VkBuffer, MemSub>> dead_subbufs;  // buffers with suballocated memory
    // Occlusion queries (sprint 6): one segment per run of draws between two sample counter reports
    // (ReportCounter type 1) or up to the end of the render pass. Resolved when the slot is reused.
    struct OcclEvent {
        uint64_t addr;  // report address (0 = ResetCounter)
        uint32_t seg;   // segments finished before the event
    };
    // Chunked submission (sprint 6, mount): extra command buffers from the same pool; cur_cmd is the one being recorded.
    VkCommandBuffer cur_cmd = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> extra;
    uint32_t extra_used = 0;
    // Write-back to guest memory of what the GPU writes to retained SSBOs (sprint 6, unified memory): copied to a
    // CPU-visible buffer at the end of the submit; written to the guest when the slot is reused.
    struct WriteBack {
        uint64_t gpu, size, off;
    };
    Buffer readback;
    std::vector<WriteBack> wb;
    bool readback_grow = false;
    VkQueryPool occl = VK_NULL_HANDLE;
    uint32_t occl_used = 0;
    std::vector<OcclEvent> occl_events;
};

class VkCtx {
public:
    bool Init();
    void Shutdown();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfamily = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memprops{};
    bool has_push_descriptor = false, has_custom_border = false, has_depth_clip_control = false,
         has_external_memory_host = false, has_eds3 = false, has_xfb = false, has_aniso = false;
    // Chosen GPU class (FPS-1): integrated, or a discrete GPU with little VRAM -> low_end (auto Performance preset).
    bool integrated = false, low_end = false;
    uint64_t vram_bytes = 0;
    VkSemaphore timeline = VK_NULL_HANDLE;  // value = number of the completed submit
    std::atomic<uint64_t> timeline_next{1};

    PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR_ = nullptr;
    PFN_vkCmdBindTransformFeedbackBuffersEXT vkCmdBindTransformFeedbackBuffersEXT_ = nullptr;
    PFN_vkCmdBeginTransformFeedbackEXT vkCmdBeginTransformFeedbackEXT_ = nullptr;
    PFN_vkCmdEndTransformFeedbackEXT vkCmdEndTransformFeedbackEXT_ = nullptr;
    PFN_vkCmdDrawIndirectByteCountEXT vkCmdDrawIndirectByteCountEXT_ = nullptr;

    // Buffers written by the GPU (transform feedback): guest GPU address -> own buffer.
    // Draws reading those addresses use this buffer instead of copying from guest memory.
    // Suballocated memory (AllocBufferMem) and recycling: those neither written nor read for
    // kGpuBufIdleFrames frames are freed (EvictGpuBuffers), and a new one fully covering older
    // ones replaces them (the guest address was reused).
    struct GpuBuf {
        uint64_t gpu = 0, size = 0;
        Buffer b;  // b.mem stays null: the memory is `sub`
        MemSub sub;
        mutable uint64_t last_use = 0;  // frame (g_stats.frames) of the last use
        // Hash of the guest memory under the buffer when XFB wrote it: if the CPU rewrites it
        // (address reused for other data), the buffer becomes invalid and the guest's is read.
        mutable uint64_t guest_hash = 0, checked_frame = UINT64_MAX;
        // Copy of guest memory as seen at the last sync (sprint 6, Q4): at the start of each submit, blocks the CPU
        // changed since then are copied over the buffer (memory is shared on the Switch: the last write wins, CPU or
        // GPU). Empty = not synced.
        std::vector<uint8_t> shadow;
        // Usage diagnostics (bits): 1 written by compute, 2 written by graphics (SSBO), 4 XFB, 8 read as VB, 16 as
        // indices, 32 as UBO, 64 as read SSBO, 128 the CPU changed its memory (sync).
        mutable uint32_t flags = 0;
    };
    // Creates/grows. If new (*fresh), `moved` receives the buffers it replaces (the same one when growing, or those
    // inside the new range): they stay alive until this submit ends, so their contents can be copied.
    struct Moved {
        VkBuffer buf;
        uint64_t gpu, size;
        std::vector<uint8_t> shadow;  // guest memory at its last sync (to see what the CPU changed afterwards)
    };
    GpuBuf* GpuBuffer(uint64_t gpu, uint64_t size, bool* fresh = nullptr, std::vector<Moved>* moved = nullptr);
    const GpuBuf* FindGpuBuffer(uint64_t gpu, uint64_t size) const;
    void EvictGpuBuffers();  // call with submit_mu held (every ~120 frames)
    void KillGpuBuf(GpuBuf& g);
    std::map<uint32_t, uint64_t> flag_hist;  // usage of retired own buffers (and live ones at report time)
    void DropGpuBuf(uint64_t gpu);  // discards the buffer starting at gpu
    uint64_t gpubuf_created = 0, gpubuf_evicted = 0, gpubuf_replaced = 0, gpubuf_stale = 0;
    uint64_t gpubuf_synced_bufs = 0, gpubuf_synced_bytes = 0;
    std::map<uint64_t, GpuBuf> gpu_bufs;  // by start GPU address (O(log n) lookup: there were >1,500 and they were scanned on every upload)
    // XFB byte counters: a 16 B slot (4 buffers) per NVN control address (XfbSlot), so that
    // DrawTransformFeedback reads its own session's (sprint 7). Slot 0 is for the null control.
    static constexpr uint32_t kXfbSlots = 4096;
    void* rdoc = nullptr;  // RENDERDOC_API_1_6_0*, with renderdoc_dir (vkctx.cpp)
    Buffer xfb_counters;

    uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;
    VkDeviceMemory Alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want);
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void DestroyBuffer(Buffer& b);
    // Image memory (DEVICE_LOCAL) suballocated from 256 MiB blocks: avoids one vkAllocateMemory
    // per texture (hundreds in bursts while loading; in shadow, while suyu presents).
    MemSub AllocImage(const VkMemoryRequirements& req);
    MemSub AllocBufferMem(const VkMemoryRequirements& req);  // freed with FreeImage
    MemSub AllocSub(const VkMemoryRequirements& req, int kind);
    void FreeImage(const MemSub& s);

private:
    struct MemBlock {
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t type = 0;
        int kind = 0;  // 0 images, 1 buffers
        VkDeviceSize size = 0;
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free;  // (offset, size), sorted
    };
    std::vector<MemBlock> blocks_;
    std::mutex blocks_mu_;

public:

    // Frames in flight
    static constexpr int kFrames = 3;
    FrameSlot frames[kFrames];
    int cur = 0;
    FrameSlot& Begin();  // waits for the slot, resets it, begins the command buffer
    // Bumped whenever a command buffer EmitDraw records into is begun (Begin, SubmitChunk): its cached dynamic state is void.
    uint64_t cmd_epoch = 0;
    // snap_cache arena: bytes ever allocated (monotonic). A slot's work may reference arena bytes allocated from
    // (its arena_head_at_begin - arena/2) on (pipeline.cpp, SnapCached).
    uint64_t arena_head = 0, dev_head = 0;
    // Ends and submits; signals the timeline with the returned value.
    uint64_t Submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    // Submits what the slot has recorded so far (no fence or semaphores) and begins another command buffer, which it
    // returns: the GPU starts while the CPU keeps recording the rest of the frame.
    VkCommandBuffer SubmitChunk();
    std::atomic<uint64_t> chunks{0};
    // Reserves space in the current frame's upload ring (aligned). Returns the offset.
    VkDeviceSize UploadAlloc(VkDeviceSize size, VkDeviceSize align, uint8_t** ptr);
    uint64_t CompletedValue();
    bool WaitValue(uint64_t v, uint64_t timeout_ns);

    std::mutex submit_mu;  // one submit at a time

    // GPU time (timestamps at the start and end of each command buffer)
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double ts_period_ns = 1.0;
    std::atomic<uint64_t> gpu_ns{0}, gpu_cmdbufs{0};
    std::atomic<uint64_t> fence_wait_ns{0};
    // Occlusion: value of the last resolved report per address (ReportCounter writes it to the guest).
    static constexpr uint32_t kOcclQueries = 16384;
    bool has_precise_occl = false;
    std::unordered_map<uint64_t, uint64_t> occl_values;
    uint64_t occl_total = 0, occl_resolved = 0, occl_overflow = 0;
    void ResolveOcclusion(FrameSlot& f);
    void (*on_slot_reuse)(FrameSlot& f) = nullptr;  // when a slot is reused (fence already waited), before clearing it  // CPU waiting for the GPU in Begin (slot busy)
};
extern VkCtx g_vk;

// Own window + swapchain
class Presenter {
public:
    bool Start(int w, int h);
    // Uses a foreign window (the game's, pla_api window_handle) instead of creating one.
    bool StartOnWindow(HWND hwnd);
    void Stop();
    // Copies `img` (in GENERAL layout) to the swapchain and presents. Records into the current cmd.
    void Present(VkCommandBuffer cmd, VkImage img, int w, int h, VkFormat fmt);
    // Captures: dumps the final pass output image (from the last present) to a PPM, with the same sRGB conversion the
    // blit would do if the game's frame was sRGB. False if the previous frame didn't go through the final pass.
    // Call with the queue stopped.
    bool DumpPost(const std::string& path, bool srgb);
    // aa_hud: copies the already composited scene (before the UI) from the window texture. Records into `cmd` outside
    // a render pass; the image is in GENERAL and was just written by a draw.
    void SnapScene(VkCommandBuffer cmd, VkImage img, VkFormat fmt, int w, int h);
    void SetSceneValid(bool v) { scene_valid_ = v; }
    // After Submit: vkQueuePresent
    void FinishPresent();
    bool alive() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_.load(); }
    // In-game menu: recreate the swapchain on the next present (e.g. when vsync changes).
    void RequestRecreate() { resized_ = true; }
    // In-game menu: dumps the final image with the menu (from the last present) to a PPM. With the queue stopped.
    bool DumpOverlay(const std::string& path);
    VkSemaphore acquire_sem() const { return pending_ ? acquire_[sem_idx_] : VK_NULL_HANDLE; }
    VkSemaphore render_sem() const { return pending_ ? render_[sem_idx_] : VK_NULL_HANDLE; }

private:
    void ThreadMain(int w, int h);
    bool Recreate();
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> resized_{false}, quit_{false};
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    VkSemaphore acquire_[VkCtx::kFrames] = {}, render_[VkCtx::kFrames] = {};
    int sem_idx_ = 0;
    uint32_t image_idx_ = 0;
    bool pending_ = false;
    bool external_ = false;
    uint32_t presents_ = 0;
    // Final pass (graphics options): compute chain over the game's frame before copying it to the swapchain:
    // antialiasing (`aa`), color/sharpening/FPS counter and scaling to the window (`upscale`, or downscaling if the
    // game renders at a higher resolution than the window). RGBA16F images, always in GENERAL.
    enum PostKind { kPostFxaa, kPostFinal, kPostScale, kPostOvDown, kPostOvBlur, kPostOvCompose, kPostCount };
    struct PostImage {
        VkImage img = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        MemSub mem;
        int w = 0, h = 0;
        bool fresh = true;
    };
    bool EnsurePostPipes();
    bool EnsurePostImage(int slot, int w, int h, VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT);
    // src2: second input (binding 2; unfiltered unless linear2); null = src.
    void RunPost(VkCommandBuffer cmd, int kind, VkImageView src, bool linear, int dst, const void* pc, uint32_t pc_size,
                 VkImageView src2 = VK_NULL_HANDLE, bool linear2 = false);
    // In-game menu: blurred background + UI into post_[5] (window size). False on failure.
    bool OverlayPass(VkCommandBuffer cmd, VkImage img, VkImageView view, VkFormat fmt, int w, int h);
    void DestroyPost();
    static void FreePostImage(PostImage& pi);
    VkDescriptorSetLayout post_dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout post_pl_ = VK_NULL_HANDLE;
    VkPipeline post_pipe_[kPostCount] = {};
    VkSampler post_smp_ = VK_NULL_HANDLE, post_lin_ = VK_NULL_HANDLE;
    // 0 and 1: game resolution (ping-pong); 2: window resolution; 3 and 4: menu blur (1/4 of the window);
    // 5: final image with the menu (RGBA8, window size; ImGui target).
    PostImage post_[8];  // 6, 7: presented area and scene copy cut to the game's screen (render_res live resolution)
    VkFormat post_fmt_[8] = {};
    bool ov_shown_ = false;  // the last present included the menu (DumpOverlay)
    int post_last_ = -1;  // image presented in the last frame (DumpPost)
    PostImage scene_;  // aa_hud: scene copy (same format as the window texture)
    VkFormat scene_fmt_ = VK_FORMAT_UNDEFINED;
    bool scene_valid_ = false;
    bool post_failed_ = false;
    double fps_ema_ = 0;  // ms per frame (FPS counter)
    LARGE_INTEGER fps_t_{};
};
extern Presenter g_present;

}  // namespace nvnvk
