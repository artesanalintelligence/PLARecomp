// Texture cache (VkImage per GPU address), views, samplers and block-linear deswizzle.
// All images live in VK_IMAGE_LAYOUT_GENERAL: hazards are covered with memory barriers
// (NVN Barrier and render target changes), not with layout transitions.
// SPDX-License-Identifier: MIT
#pragma once

#include <unordered_map>

#include "nvnvk.h"
#include "vkctx.h"

namespace nvnvk {

struct ImageKeyInfo {
    uint64_t gpu = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t width = 1, height = 1, depth = 1, layers = 1, levels = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool cube = false;
};

struct Image {
    ImageKeyInfo info;
    VkImage img = VK_NULL_HANDLE;
    MemSub mem;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    bool gpu_written = false;    // render target / copy destination: do not reload from the CPU
    bool uploaded = false;
    uint64_t last_use = 0;
    // Detection of CPU changes made without reinitializing the texture (video, dynamic atlases):
    // hash of 64 samples of 64 bytes spread over the guest range, checked once per submit.
    uint64_t guest_cpu = 0, guest_size = 0, sample_hash = 0, checked_submit = 0, diag_tick = ~0ull;
    bool cpu_writable = false;  // pool without NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS: full fingerprint (<= 4 MiB)
    std::unordered_map<uint64_t, VkImageView> views;
};

// Guest memory layout parameters (TIC)
struct GuestLayout {
    bool block_linear = true;
    uint32_t block_height_log2 = 4, block_depth_log2 = 0, tile_width_spacing = 0;
    uint32_t pitch = 0;  // linear
};

class TextureCache {
public:
    // FPS-1: bumped whenever ViewFromTic's answer for a TIC can change (image created/destroyed, became GPU-written,
    // marked for re-upload). BuildWrites memoizes ViewFromTic per submission while it stays the same.
    std::atomic<uint64_t> epoch{1};
    // Image for an NVN texture described by its object (render target, copies, window).
    Image* FromDesc(const TexDesc& d, bool for_attachment);
    // Image + view for a TIC descriptor (8 words) read from the texture pool. Uploads the data
    // from guest memory the first time. Returns the view (or null).
    VkImageView ViewFromTic(pla_ctx* ctx, VkCommandBuffer cmd, const uint32_t tic[8], VkImageViewType want, bool* is_depth,
                            bool storage = false);
    VkImageView View(Image* im, VkFormat fmt, VkImageViewType type, uint32_t base_level, uint32_t levels,
                     uint32_t base_layer, uint32_t layers, VkComponentMapping sw, VkImageAspectFlags aspect = 0);
    // nearest: force NEAREST filtering (integer-format textures, which do not support LINEAR)
    VkSampler Sampler(const uint32_t tsc[8], bool nearest = false);
    // Marks images whose GPU range overlaps [gpu, gpu+size) as dirty (new CPU data).
    // overlap: any overlap with CPU-uploaded data (does not touch render targets).
    void Invalidate(uint64_t gpu, uint64_t size, bool overlap = false);    bool Overlaps(uint64_t gpu, uint64_t size);  // some known texture overlaps [gpu, gpu+size)
    void InitImageLayout(VkCommandBuffer cmd, Image* im);
    // Transitions images created since the last flush to GENERAL (call before using them).
    void FlushPending(VkCommandBuffer cmd);
    VkImageView NullView(VkImageViewType type);
    VkSampler NullSampler();
    void Shutdown();
    size_t Count() const { return images_.size(); }
    // Iterates over the images (with the cache lock held).
    template <class F>
    void ForEach(F&& f) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [k, v] : images_) f(v.get());
    }

private:
    Image* Create(const ImageKeyInfo& k);
    void Destroy(Image* im);
    // false: the address is in no live pool (nothing uploaded; retried on the next use)
    bool Upload(pla_ctx* ctx, VkCommandBuffer cmd, Image* im, const GuestLayout& lay, uint32_t bpb, uint32_t bw,
                uint32_t bh);
    std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<Image>> images_;  // by GPU address
    std::unordered_map<uint64_t, VkSampler> samplers_;
    std::vector<std::unique_ptr<Image>> null_images_;
    std::unordered_map<int, VkImageView> null_views_;
    VkSampler null_sampler_ = VK_NULL_HANDLE;
    std::vector<Image*> pending_init_;
};
extern TextureCache g_tex;

// Deswizzle of a block-linear level: (w_bytes x rows x depth) -> linear (row = w_bytes).
void UnswizzleLevel(const uint8_t* src, size_t src_size, uint8_t* dst, uint32_t w_bytes, uint32_t rows,
                    uint32_t depth, uint32_t bh_log2, uint32_t bd_log2);
// Size of a block-linear level in bytes.
uint64_t BlockLinearLevelSize(uint32_t w_bytes, uint32_t rows, uint32_t depth, uint32_t bh_log2, uint32_t bd_log2);
uint32_t AdjustBlockLog2(uint32_t log2, uint32_t extent, uint32_t gob_extent);

}  // namespace nvnvk
