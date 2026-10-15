// SPDX-License-Identifier: MIT
#include "textures.h"

#include <algorithm>
#include <cmath>

#include "exec.h"
#include "formats.h"

namespace nvnvk {

TextureCache g_tex;

// ---------------------------------------------------------------- cache
namespace {
uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

VkImageUsageFlags UsageFor(VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &fp);
    const auto feat = fp.optimalTilingFeatures;
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    return u;
}
std::atomic<int> g_replace_logs{0};

// Cache key: the guest (CPU) address if the VA is in a known pool. A MemoryPool has up to 3 GPU
// mappings and the game uses one for the RT and another in the TIC for the same memory (e.g. the
// shadow mask 550488000 is sampled as 540a88000): with the VA as key they were two images and the
// lighting read a stale copy (the dark rock in the field).
uint64_t CacheKey(uint64_t gpu) {
    const uint64_t cpu = g_reg.GpuToCpu(gpu);
    return cpu ? (cpu | (1ull << 63)) : gpu;
}

// Fingerprint of a texture's contents in guest memory, to detect CPU writes (glyph atlas of the
// dialog text, filled character by character). Textures in CPU-writable pools (<= 16 MiB) are
// hashed in full from physical backing memory; the rest, by samples.
constexpr uint64_t kFullHashMax = 16ull << 20;  // 2048x2048 R8 glyph atlas (4 MiB + alignment)
// Full fingerprint in runs (same result as a single pass over the contiguous block as long as the
// cuts fall on multiples of 16, like page boundaries).
struct FullHasher {
    uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x9E3779B97F4A7C15ull;
    void Add(const uint8_t* p, uint64_t size) {
        size_t i = 0;
        for (; i + 16 <= size; i += 16) {
            uint64_t a, b;
            std::memcpy(&a, p + i, 8);
            std::memcpy(&b, p + i + 8, 8);
            h1 = (h1 ^ a) * 0x100000001b3ull;
            h2 = (h2 ^ b) * 0xff51afd7ed558ccdull;
        }
        for (; i < size; ++i) h1 = (h1 ^ p[i]) * 0x100000001b3ull;
    }
    uint64_t Done() const { return h1 ^ (h2 + 0x9E3779B97F4A7C15ull + (h1 << 6) + (h1 >> 2)); }
};
uint64_t HashFull(const uint8_t* p, uint64_t size) {
    FullHasher h;
    h.Add(p, size);
    return h.Done();
}

bool HasBackingApi() {
    return PLA_API_HAS(g_guest.api, host_backing_pointer) && g_guest.api->host_backing_pointer && !g_cfg.replay;
}

// Full fingerprint from physical backing memory, in page runs if not contiguous (a 4 MiB atlas almost
// never is: previously it fell back to 1,024 samples and new glyphs went unseen -> missing letters).
bool BackingHash(uint64_t cpu, uint64_t size, uint64_t* out) {
    if (!HasBackingApi()) return false;
    FullHasher h;
    if (const void* p = g_guest.api->host_backing_pointer(cpu, size)) {
        h.Add(static_cast<const uint8_t*>(p), size);
        *out = h.Done();
        return true;
    }
    uint64_t off = 0;
    while (off < size) {
        const uint64_t chunk = std::min<uint64_t>(size - off, 4096 - ((cpu + off) & 4095));
        const void* p = g_guest.api->host_backing_pointer(cpu + off, chunk);
        if (!p) return false;
        h.Add(static_cast<const uint8_t*>(p), chunk);
        off += chunk;
    }
    *out = h.Done();
    return true;
}

bool FullHashable(uint64_t cpu, uint64_t size, bool full) {
    uint64_t dummy;
    return full && size <= kFullHashMax && BackingHash(cpu, std::min<uint64_t>(size, 4096 - (cpu & 4095)), &dummy);
}

uint64_t SampleHash(pla_ctx* ctx, uint64_t cpu, uint64_t size, bool full) {
    uint64_t h = 0xcbf29ce484222325ull;
    if (full && size <= kFullHashMax) {
        // Bypasses capture (g_guest.Ptr would record 1 MiB into the .nvkc on every check);
        // the replayer has no backing and uses the samples below, which are recorded.
        uint64_t fh;
        if (BackingHash(cpu, size, &fh)) return fh;
    }
    uint8_t buf[64];
    const uint64_t n = size <= (4ull << 20) ? 1024 : 64, step = size > n * 64 ? size / n : 64;
    for (uint64_t off = 0; off < size; off += step) {
        const size_t len = (size_t)std::min<uint64_t>(64, size - off);
        g_guest.Read(ctx, cpu + off, buf, len);
        for (size_t i = 0; i < len; i += 8) {
            uint64_t v = 0;
            std::memcpy(&v, buf + i, std::min<size_t>(8, len - i));
            h = (h ^ v) * 0x100000001b3ull;
        }
    }
    return h;
}
void LogReplace(const char* why, const ImageKeyInfo& o, const ImageKeyInfo& k) {
    if (g_cfg.Traced(k.gpu))
        NVK_INFO("trace: texture %llx replaced (%s): %ux%u l%u ly%u f%d -> %ux%u l%u ly%u f%d cube=%d", (unsigned long long)k.gpu,
                 why, o.width, o.height, o.levels, o.layers, (int)o.format, k.width, k.height, k.levels, k.layers, (int)k.format,
                 (int)k.cube);
    if (g_replace_logs.fetch_add(1) >= 60) return;
    NVK_DBG("texture %llx replaced (%s): %ux%u l%u ly%u f%d s%d -> %ux%u l%u ly%u f%d s%d", (unsigned long long)k.gpu, why,
            o.width, o.height, o.levels, o.layers, (int)o.format, (int)o.samples, k.width, k.height, k.levels, k.layers,
            (int)k.format, (int)k.samples);
}
}  // namespace

Image* TextureCache::Create(const ImageKeyInfo& k) {
    auto im = std::make_unique<Image>();
    im->info = k;
    im->aspect = AspectOf(k.format);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (!IsCompressed(k.format) && !IsDepthFormat(k.format)) ci.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (k.cube && k.layers % 6 == 0 && k.width == k.height) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (k.type == VK_IMAGE_TYPE_3D) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    ci.imageType = k.type;
    ci.format = k.format;
    ci.extent = {std::max(1u, k.width), std::max(1u, k.height), k.type == VK_IMAGE_TYPE_3D ? std::max(1u, k.depth) : 1};
    ci.mipLevels = std::max(1u, k.levels);
    ci.arrayLayers = k.type == VK_IMAGE_TYPE_3D ? 1 : std::max(1u, k.layers);
    ci.samples = k.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = UsageFor(k.format);
    if (k.samples != VK_SAMPLE_COUNT_1_BIT) ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
    im->usage = ci.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (k.format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && k.format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        // Sprint 5 review: desktop GPUs (NVIDIA, AMD) lack ASTC. So far the game has used none in the title,
        // intro, field, battle or mount; if one shows up, it is logged here.
        static std::atomic<int> astc_logs{0};
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, k.format, &fp);
        if (astc_logs.fetch_add(1) < 20)
            NVK_WARN("ASTC texture %llx %ux%u fmt=%d: %s", (unsigned long long)k.gpu, k.width, k.height, (int)k.format,
                     fp.optimalTilingFeatures ? "supported by the GPU" : "NOT supported by the GPU (would need CPU decoding)");
    }
    VkResult r = vkCreateImage(g_vk.dev, &ci, nullptr, &im->img);
    if (r != VK_SUCCESS) {
        NVK_WARN("vkCreateImage %ux%ux%u fmt=%d lv=%u ly=%u = %d", ci.extent.width, ci.extent.height, ci.extent.depth,
                 (int)k.format, ci.mipLevels, ci.arrayLayers, (int)r);
        return nullptr;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, im->img, &req);
    im->mem = g_vk.AllocImage(req);
    if (!im->mem.mem) {
        NVK_WARN("out of memory for image %ux%u fmt=%d (%llu bytes)", ci.extent.width, ci.extent.height,
                 (int)k.format, (unsigned long long)req.size);
        vkDestroyImage(g_vk.dev, im->img, nullptr);
        return nullptr;
    }
    VKCHECK(vkBindImageMemory(g_vk.dev, im->img, im->mem.mem, im->mem.off));
    g_stats.textures.fetch_add(1);
    Image* raw = im.get();
    pending_init_.push_back(raw);
    images_[CacheKey(k.gpu)] = std::move(im);
    epoch.fetch_add(1, std::memory_order_relaxed);
    return raw;
}

void TextureCache::InitImageLayout(VkCommandBuffer cmd, Image* im) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

void TextureCache::FlushPending(VkCommandBuffer cmd) {
    std::vector<Image*> v;
    {
        std::lock_guard<std::mutex> lk(mu_);
        v.swap(pending_init_);
    }
    if (v.empty()) return;
    OnTransferBegin();  // image barriers cannot go inside a render pass
    for (Image* p : v) InitImageLayout(cmd, p);
}

void TextureCache::Destroy(Image* im) {
    epoch.fetch_add(1, std::memory_order_relaxed);
    FrameSlot& f = g_vk.frames[g_vk.cur];
    for (auto& [k, v] : im->views) f.dead_views.push_back(v);
    f.dead_images.emplace_back(im->img, im->mem);
}

Image* TextureCache::FromDesc(const TexDesc& d, bool for_attachment) {
    std::lock_guard<std::mutex> lk(mu_);
    const FormatInfo& fi = NvnFormat(d.format);
    if (fi.vk == VK_FORMAT_UNDEFINED || !d.gpu) return nullptr;
    ImageKeyInfo k;
    k.gpu = d.gpu;
    k.format = fi.vk;
    k.width = d.width;
    k.height = std::max(1u, d.height);
    // targets NVN: 0 1D, 1 2D, 2 3D, 3 1D array, 4 2D array, 5 2DMS, 6 2DMS array, 7 rectangle,
    // 8 cubemap, 9 cubemap array, 10 buffer (order deduced from usage; see NVN-SEMANTICS)
    switch (d.target) {
        case 2: k.type = VK_IMAGE_TYPE_3D; k.depth = std::max(1u, d.depth); break;
        case 0: case 3: k.type = VK_IMAGE_TYPE_1D; k.layers = d.target == 3 ? std::max(1u, d.height) : 1; k.height = 1; break;
        case 4: case 6: k.layers = std::max(1u, d.depth); break;
        case 8: k.layers = 6; k.cube = true; break;
        case 9: k.layers = std::max(1u, d.depth); k.cube = true; break;
        default: break;
    }
    k.levels = std::max(1u, d.levels);
    if (d.samples > 1) k.samples = (VkSampleCountFlagBits)d.samples;
    // View of one layer of an array (shadow cascades): the image needs at least base_layer+1 layers.
    if (k.type == VK_IMAGE_TYPE_2D && !k.cube) k.layers = std::max(k.layers, d.base_layer + std::max(1u, d.num_layers));
    auto it = images_.find(CacheKey(d.gpu));
    if (it != images_.end()) {
        Image* im = it->second.get();
        const auto& o = im->info;
        if (o.width == k.width && o.height == k.height && o.layers >= k.layers && o.samples == k.samples &&
            o.levels >= k.levels && (o.format == k.format || NvnFormat(d.format).bpb == 0 ||
                                     (!IsDepthFormat(o.format) && !IsDepthFormat(k.format) && !IsCompressed(o.format)))) {
            if (for_attachment) {
                if (!im->gpu_written && im->uploaded && im->cpu_writable)
                    NVK_INFO("texture %llx %ux%u fmt %d (uploaded from guest, writable pool) becomes GPU-written (f%llu)",
                             (unsigned long long)d.gpu, o.width, o.height, (int)o.format,
                             (unsigned long long)g_stats.frames.load(std::memory_order_relaxed));
                if (!im->gpu_written) epoch.fetch_add(1, std::memory_order_relaxed);
                im->gpu_written = true;
                im->last_use = g_stats.frames.load(std::memory_order_relaxed);
            }
            im->info.gpu = d.gpu;  // same guest memory, maybe through a newer pool's VA (see ViewFromTic)
            return im;
        }
        LogReplace("RT/copy", o, k);
        Destroy(im);
        images_.erase(it);
    }
    Image* im = Create(k);
    if (im && for_attachment) {
        im->gpu_written = true;
        im->last_use = g_stats.frames.load(std::memory_order_relaxed);
    }
    return im;
}

VkImageView TextureCache::View(Image* im, VkFormat fmt, VkImageViewType type, uint32_t base_level, uint32_t levels,
                               uint32_t base_layer, uint32_t layers, VkComponentMapping sw, VkImageAspectFlags aspect) {
    if (!aspect) aspect = im->aspect;
    uint64_t key = Mix(fmt, type);
    key = Mix(key, base_level | (uint64_t)levels << 8 | (uint64_t)base_layer << 16 | (uint64_t)layers << 40);
    key = Mix(key, sw.r | sw.g << 4 | sw.b << 8 | sw.a << 12 | (uint64_t)aspect << 16);
    auto it = im->views.find(key);
    if (it != im->views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    uci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) uci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) uci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        uci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && im->info.samples == VK_SAMPLE_COUNT_1_BIT)
        uci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    uci.usage &= im->usage;
    vi.pNext = &uci;
    vi.image = im->img;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = sw;
    vi.subresourceRange = {aspect, base_level, std::max(1u, levels), base_layer, std::max(1u, layers)};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &vi, nullptr, &v) != VK_SUCCESS) {
        NVK_WARN("vkCreateImageView fmt=%d type=%d (image fmt=%d) failed", (int)fmt, (int)type, (int)im->info.format);
        return VK_NULL_HANDLE;
    }
    im->views[key] = v;
    return v;
}

VkImageView TextureCache::ViewFromTic(pla_ctx* ctx, VkCommandBuffer cmd, const uint32_t t[8], VkImageViewType want, bool* is_depth,
                                      bool storage) {
    const uint32_t w0 = t[0];
    const uint64_t gpu = t[1] | ((uint64_t)(t[2] & 0xFFFF) << 32);
    const uint32_t header = (t[2] >> 21) & 7;
    const bool srgb = (t[4] >> 22) & 1;
    const uint32_t ttype = (t[4] >> 23) & 0xF;
    uint32_t bpb = 4, bw = 1, bh = 1;
    const VkFormat fmt = TicFormat(w0, srgb, &bpb, &bw, &bh);
    if (fmt == VK_FORMAT_UNDEFINED || !gpu) return VK_NULL_HANDLE;
    *is_depth = IsDepthFormat(fmt);
    uint32_t width, height = (t[5] & 0xFFFF) + 1, depth = ((t[5] >> 16) & 0x3FFF) + 1;
    if (header == 0) {
        width = ((t[3] & 0xFFFF) << 16 | (t[4] & 0xFFFF)) + 1;  // 1D buffer (not supported as an image)
        return VK_NULL_HANDLE;
    }
    width = (t[4] & 0xFFFF) + 1;
    const uint32_t levels = (header == 3 || header == 4) ? ((t[3] >> 28) & 0xF) + 1 : 1;
    const uint32_t min_level = t[7] & 0xF, max_level = (t[7] >> 4) & 0xF;
    ImageKeyInfo k;
    k.gpu = gpu;
    k.format = fmt;
    k.width = width;
    k.height = height;
    k.levels = levels;
    VkImageViewType vt = VK_IMAGE_VIEW_TYPE_2D;
    switch (ttype) {
        case 0: k.type = VK_IMAGE_TYPE_1D; k.height = 1; vt = VK_IMAGE_VIEW_TYPE_1D; break;
        case 1: case 7: vt = VK_IMAGE_VIEW_TYPE_2D; k.layers = 1; break;
        case 2: k.type = VK_IMAGE_TYPE_3D; k.depth = depth; vt = VK_IMAGE_VIEW_TYPE_3D; break;
        case 3: k.layers = 6; k.cube = true; vt = VK_IMAGE_VIEW_TYPE_CUBE; break;
        case 4: k.type = VK_IMAGE_TYPE_1D; k.layers = depth; k.height = 1; vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case 5: k.layers = depth; vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case 8: k.layers = depth * 6; k.cube = true; vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
        default: return VK_NULL_HANDLE;
    }
    const uint32_t msaa = (t[7] >> 8) & 0xF;
    if (msaa) k.samples = msaa >= 6 ? VK_SAMPLE_COUNT_16_BIT : msaa >= 3 ? VK_SAMPLE_COUNT_8_BIT : msaa == 2 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
    Image* im = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = images_.find(CacheKey(gpu));
        if (it != images_.end()) {
            Image* o = it->second.get();
            const bool same_shape = o->info.width == k.width && o->info.height == k.height &&
                                    o->info.layers >= k.layers && o->info.depth == k.depth;
            if (same_shape && (o->gpu_written || (o->info.format == k.format && o->info.levels == k.levels))) {
                im = o;
                // Same guest memory, maybe through another pool's VA: the next area's pools reuse the memory (and the
                // VAs) of the previous ones. Upload translates info.gpu; with the creation VA it read whatever pool owns
                // that VA now (another texture's data in the interiors after re-entering a house).
                o->info.gpu = gpu;
            } else if (!o->gpu_written || !same_shape) {
                LogReplace("TIC", o->info, k);
                Destroy(o);
                images_.erase(it);
            }
        }
        if (!im) im = Create(k);
    }
    if (!im) return VK_NULL_HANDLE;
    FlushPending(cmd);
    // In exclusive mode only data in CPU-writable pools changes from the CPU (checked once per frame);
    // what nvnvk writes to guest memory (CopyBufferToBuffer) is flagged by Invalidate. In shadow mode
    // suyu's emulated GPU also writes: samples once per submit, as before.
    const bool check = !g_cfg.exclusive || im->cpu_writable;
    if (g_cfg.log_tex_updates && (im->cpu_writable || im->info.format == VK_FORMAT_R8_UNORM) && im->info.width >= 64 &&
        im->info.height >= 64) {
        // Text diagnostics (sprint 3): every 300 frames, state of large writable textures.
        const uint64_t f = g_stats.frames.load(std::memory_order_relaxed);
        const uint64_t period = g_cfg.Traced(gpu) ? 20 : 300;
        if (f / period != im->diag_tick) {
            im->diag_tick = f / period;
            if (g_cfg.Traced(gpu) && im->guest_size >= (64u << 10)) {
                // Atlas tail (last 64 KiB: last block row, high GOB columns) and count of bytes != 0
                std::vector<uint8_t> tail(64u << 10);
                g_guest.Read(ctx, im->guest_cpu + im->guest_size - tail.size(), tail.data(), tail.size());
                size_t nz = 0;
                for (uint8_t c : tail) nz += c != 0;
                const uint8_t* bp = PLA_API_HAS(g_guest.api, host_backing_pointer) && g_guest.api->host_backing_pointer
                                        ? static_cast<const uint8_t*>(g_guest.api->host_backing_pointer(im->guest_cpu + im->guest_size - tail.size(), tail.size()))
                                        : nullptr;
                size_t nzb = 0;
                if (bp) for (size_t i = 0; i < tail.size(); ++i) nzb += bp[i] != 0;
                const uint8_t* hp = PLA_API_HAS(g_guest.api, host_pointer) && g_guest.api->host_pointer
                                        ? static_cast<const uint8_t*>(g_guest.api->host_pointer(im->guest_cpu + im->guest_size - tail.size(), tail.size()))
                                        : nullptr;
                size_t nzh = 0;
                if (hp) for (size_t i = 0; i < tail.size(); ++i) nzh += hp[i] != 0;
                NVK_INFO("diag f%llu: tail of %llx: non-zero read=%zu backing=%zu host=%zu", (unsigned long long)f,
                         (unsigned long long)gpu, nz, nzb, nzh);
            }
            const uint64_t fresh = im->guest_size ? SampleHash(ctx, im->guest_cpu, im->guest_size, true) : 0;
            NVK_INFO("diag f%llu: tex %llx %ux%u fmt %d uploaded=%d gpu=%d fingerprint %s (cpu %llx, %llu B)",
                     (unsigned long long)f, (unsigned long long)gpu, im->info.width, im->info.height,
                     (int)im->info.format, (int)im->uploaded, (int)im->gpu_written,
                     fresh == im->sample_hash ? "same" : "DIFFERENT", (unsigned long long)im->guest_cpu,
                     (unsigned long long)im->guest_size);
        }
    }
    if (check && im->uploaded && !im->gpu_written && im->guest_size) {
        const uint64_t sub = g_cfg.exclusive ? g_stats.frames.load(std::memory_order_relaxed) + 1
                                             : g_stats.submits.load(std::memory_order_relaxed) + 1;
        // Large textures (> 1 MiB, the 4 MiB glyph atlas): full fingerprint every 4 frames (CPU cost, U3).
        const bool big = g_cfg.exclusive && im->guest_size > (1u << 20);
        if (im->checked_submit != sub && (!big || (sub & 3) == 0 || im->checked_submit + 4 <= sub)) {
            im->checked_submit = sub;
            if (SampleHash(ctx, im->guest_cpu, im->guest_size, im->cpu_writable) != im->sample_hash) {
                im->uploaded = false;
                epoch.fetch_add(1, std::memory_order_relaxed);
                g_stats.reuploads.fetch_add(1, std::memory_order_relaxed);
                if (g_cfg.Traced(gpu) || g_cfg.log_tex_updates)
                    NVK_INFO("trace f%llu: texture %llx changed by the CPU: re-uploading (cpu %llx+%llx %ux%u fmt %d writable %d, now cpu %llx)",
                             (unsigned long long)g_stats.frames.load(), (unsigned long long)gpu, (unsigned long long)im->guest_cpu,
                             (unsigned long long)im->guest_size, im->info.width, im->info.height, (int)im->info.format,
                             (int)im->cpu_writable, (unsigned long long)g_reg.GpuToCpu(gpu));
            }
        }
    }
    if (!im->uploaded && !im->gpu_written) {
        GuestLayout lay;
        lay.block_linear = header == 3 || header == 4;
        lay.block_height_log2 = (t[3] >> 3) & 7;
        lay.block_depth_log2 = (t[3] >> 6) & 7;
        lay.tile_width_spacing = (t[3] >> 10) & 7;
        lay.pitch = (t[3] & 0xFFFF) << 5;
        const bool ok = Upload(ctx, cmd, im, lay, bpb, bw, bh);
        if ((g_cfg.log_tex_updates && im->info.format == VK_FORMAT_R8_UNORM) || g_cfg.Traced(gpu))
            NVK_INFO("trace f%llu: R8 upload %llx %ux%u writable=%d", (unsigned long long)g_stats.frames.load(),
                     (unsigned long long)gpu, im->info.width, im->info.height, (int)im->cpu_writable);
        im->uploaded = ok;
    }
    VkImageAspectFlags aspect = im->aspect;
    if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    const uint32_t vlevels = std::min(max_level, im->info.levels - 1) >= min_level
                                 ? std::min(max_level, im->info.levels - 1) - min_level + 1
                                 : 1;
    VkFormat vfmt = fmt;
    if (im->info.format != fmt) {
        // Image created as an RT with another format of the same size: viewed with the TIC format
        // if compatible; otherwise with the image's format.
        const bool ok = !IsDepthFormat(fmt) && !IsDepthFormat(im->info.format) && !IsCompressed(fmt);
        if (!ok) vfmt = im->info.format;
    }
    // The view type is dictated by the shader (OpTypeImage); accepted if the image is compatible.
    auto dim = [](VkImageViewType v) {
        switch (v) {
            case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return 1;
            case VK_IMAGE_VIEW_TYPE_3D: return 3;
            default: return 2;
        }
    };
    if (dim(want) == dim(vt)) {
        const bool want_cube = want == VK_IMAGE_VIEW_TYPE_CUBE || want == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        if (!want_cube || k.cube) vt = want;
    }
    uint32_t vlayers = k.layers;
    if (vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_3D) vlayers = 1;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE) vlayers = 6;
    if (storage) {
        // Storage image: a single level and no swizzle.
        if (!im->gpu_written) epoch.fetch_add(1, std::memory_order_relaxed);
        im->gpu_written = true;
        return View(im, vfmt, vt, std::min(min_level, im->info.levels - 1), 1, 0, vlayers, {}, aspect);
    }
    VkComponentMapping sw = TicSwizzle(w0);
    if (vfmt == VK_FORMAT_D24_UNORM_S8_UINT || vfmt == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        // On Maxwell, Z24S8/S8Z24/Z32S8 put depth in G (the TIC asks for .G); in Vulkan a depth view
        // returns it in R (and G is 0). Like suyu (ConvertGreenRed): G -> R. Without this the pass that
        // linearizes field depth read 0 (= far with reversed Z) and everything was fogged.
        auto fix = [](VkComponentSwizzle& c) {
            if (c == VK_COMPONENT_SWIZZLE_G) c = VK_COMPONENT_SWIZZLE_R;
        };
        fix(sw.r);
        fix(sw.g);
        fix(sw.b);
        fix(sw.a);
    }
    return View(im, vfmt, vt, std::min(min_level, im->info.levels - 1), vlevels, 0, vlayers, sw, aspect);
}

bool TextureCache::Upload(pla_ctx* ctx, VkCommandBuffer cmd, Image* im, const GuestLayout& lay, uint32_t bpb,
                          uint32_t bw, uint32_t bh) {
    const auto& k = im->info;
    const uint32_t layers = k.type == VK_IMAGE_TYPE_3D ? 1 : k.layers;
    // Per-level sizes (in format blocks)
    struct Lv { uint32_t wb, hb, d; uint64_t guest_off, guest_size, lin_off, lin_size; uint32_t bh, bd; };
    std::vector<Lv> lv(k.levels);
    uint64_t goff = 0, loff = 0;
    for (uint32_t l = 0; l < k.levels; ++l) {
        Lv& v = lv[l];
        const uint32_t w = std::max(1u, k.width >> l), h = std::max(1u, k.height >> l);
        v.d = k.type == VK_IMAGE_TYPE_3D ? std::max(1u, k.depth >> l) : 1;
        v.wb = (w + bw - 1) / bw;
        v.hb = (h + bh - 1) / bh;
        if (lay.block_linear) {
            const bool single = k.levels == 1;
            v.bh = single ? lay.block_height_log2 : AdjustBlockLog2(lay.block_height_log2, v.hb, 8);
            v.bd = single ? lay.block_depth_log2 : AdjustBlockLog2(lay.block_depth_log2, v.d, 1);
            v.guest_size = BlockLinearLevelSize(v.wb * bpb, v.hb, v.d, v.bh, v.bd);
        } else {
            v.bh = v.bd = 0;
            v.guest_size = (uint64_t)std::max(lay.pitch, v.wb * bpb) * v.hb;
        }
        v.guest_off = goff;
        goff += v.guest_size;
        v.lin_size = (uint64_t)v.wb * bpb * v.hb * v.d;
        v.lin_off = loff;
        loff += (v.lin_size + 15) & ~15ull;
    }
    uint64_t layer_stride = goff;
    if (layers > 1 && lay.block_linear) {
        // Layer alignment to the level-0 block (adjusted to the image height)
        uint32_t bhl = AdjustBlockLog2(lay.block_height_log2, lv[0].hb, 8);
        uint32_t bdl = AdjustBlockLog2(lay.block_depth_log2, lv[0].d, 1);
        const uint64_t a = 512ull << (bhl + bdl);
        layer_stride = (layer_stride + a - 1) / a * a;
    }
    const uint64_t guest_total = layer_stride * layers;
    uint64_t avail = 0;
    uint32_t pflags = 0;
    const uint64_t cpu = g_reg.GpuToCpu(k.gpu, &avail, &pflags);
    im->cpu_writable = !(pflags & 1);  // 1 = CPU_NO_ACCESS
    if (!cpu) {
        static std::atomic<uint64_t> n{0};
        const uint64_t c = n.fetch_add(1);
        if (c < 40 || (c % 5000) == 0)
            NVK_WARN("texture %llx with no known pool (%llu times):%s", (unsigned long long)k.gpu, (unsigned long long)c + 1,
                     g_reg.Explain(k.gpu).c_str());
        return false;
    }
    std::vector<uint8_t> guest(std::min<uint64_t>(guest_total, avail));
    g_guest.Read(ctx, cpu, guest.data(), guest.size());
    im->guest_cpu = cpu;
    im->guest_size = guest.size();
    // The fingerprint must be of the SAME bytes that are uploaded: if the CPU writes between the read and a
    // fingerprint taken afterwards (a glyph atlas another thread fills in meanwhile), those glyphs were
    // never re-uploaded (text with scattered missing letters in battle).
    im->sample_hash = FullHashable(cpu, guest.size(), im->cpu_writable) ? HashFull(guest.data(), guest.size())
                                                                    : SampleHash(ctx, cpu, guest.size(), im->cpu_writable);
    const uint64_t lin_total = loff * layers;
    // Staging in the frame ring (no vkAllocateMemory/vkFreeMemory per texture: in shadow mode that
    // happens right while suyu presents). A dedicated buffer only if large or the ring is full.
    const uint64_t ssize = std::max<uint64_t>(lin_total, 16);
    Buffer staging{};
    uint8_t* sptr = nullptr;
    VkDeviceSize sbase = ssize <= (16ull << 20) ? g_vk.UploadAlloc(ssize, 16, &sptr) : VK_WHOLE_SIZE;
    VkBuffer sbuf = VK_NULL_HANDLE;
    if (sbase != VK_WHOLE_SIZE) {
        sbuf = g_vk.frames[g_vk.cur].upload.buf;
    } else {
        staging = g_vk.CreateBuffer(ssize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        sbuf = staging.buf;
        sptr = staging.map;
        sbase = 0;
    }
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        for (uint32_t l = 0; l < k.levels; ++l) {
            const Lv& v = lv[l];
            const uint64_t src_off = layer * layer_stride + v.guest_off;
            if (src_off >= guest.size()) continue;
            uint8_t* dst = sptr + layer * loff + v.lin_off;
            if (lay.block_linear) {
                UnswizzleLevel(guest.data() + src_off, guest.size() - src_off, dst, v.wb * bpb, v.hb, v.d, v.bh, v.bd);
            } else {
                const uint32_t pitch = std::max(lay.pitch, v.wb * bpb);
                for (uint32_t y = 0; y < v.hb; ++y) {
                    const uint64_t s = src_off + (uint64_t)y * pitch;
                    if (s + v.wb * bpb <= guest.size()) std::memcpy(dst + (uint64_t)y * v.wb * bpb, guest.data() + s, v.wb * bpb);
                }
            }
            VkBufferImageCopy r{};
            r.bufferOffset = sbase + layer * loff + v.lin_off;
            r.bufferRowLength = v.wb * bw;
            r.bufferImageHeight = v.hb * bh;
            r.imageSubresource = {im->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                      ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect,
                                  l, layer, 1};
            r.imageExtent = {std::max(1u, k.width >> l), std::max(1u, k.height >> l), v.d};
            regions.push_back(r);
        }
    }
    g_stats.uploads_bytes.fetch_add(lin_total);
    OnTransferBegin();
    if (!regions.empty())
        vkCmdCopyBufferToImage(cmd, sbuf, im->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    if (staging.buf) g_vk.frames[g_vk.cur].dead_buffers.push_back(staging);
    return true;
}

VkSampler TextureCache::Sampler(const uint32_t t[8], bool nearest) {
    uint64_t key = nearest ? 0x5EA4E57ull : 0;
    for (int i = 0; i < 8; ++i) key = Mix(key, t[i]);
    // Live graphics options (menu): forced anisotropy and the extra LOD bias are part of the key, so a change makes the
    // next draws build new samplers. The old ones stay in the cache (descriptors in flight may still use them; a few
    // hundred small objects per distinct setting).
    uint32_t bias_bits;
    std::memcpy(&bias_bits, &g_cfg.tex_lod_bias, 4);
    key = Mix(Mix(key, g_cfg.anisotropy), bias_bits);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    auto wrap = [](uint32_t w) {
        switch (w) {
            case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            default: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        }
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.addressModeU = wrap(t[0] & 7);
    ci.addressModeV = wrap((t[0] >> 3) & 7);
    ci.addressModeW = wrap((t[0] >> 6) & 7);
    ci.compareEnable = (t[0] >> 9) & 1;
    static const VkCompareOp cmp[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                      VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                      VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    ci.compareOp = cmp[(t[0] >> 10) & 7];
    const uint32_t aniso = (t[0] >> 20) & 7;
    ci.magFilter = (t[1] & 3) == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = ((t[1] >> 4) & 3) == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (t[1] >> 6) & 3;
    ci.mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const int32_t bias13 = (int32_t)(((t[1] >> 12) & 0x1FFF) << 19) >> 19;
    ci.mipLodBias = bias13 / 256.0f;
    ci.minLod = (t[2] & 0xFFF) / 256.0f;
    ci.maxLod = mip == 1 ? 0.25f : ((t[2] >> 12) & 0xFFF) / 256.0f;
    if (aniso > 0 && mip != 1) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min<float>((float)(1u << aniso), g_vk.props.limits.maxSamplerAnisotropy);
    }
    // Graphics option `anisotropy` (2..16): forced minimum on linear mipmapped samplers (ground textures at an angle).
    if (g_cfg.anisotropy > 1 && mip >= 2 && ci.minFilter == VK_FILTER_LINEAR && g_vk.has_aniso) {
        const float want = std::min<float>((float)g_cfg.anisotropy, g_vk.props.limits.maxSamplerAnisotropy);
        if (!ci.anisotropyEnable || ci.maxAnisotropy < want) {
            ci.anisotropyEnable = VK_TRUE;
            ci.maxAnisotropy = want;
        }
    }
    // Extra LOD bias (menu "Distant texture sharpness"): only samplers with mipmaps.
    if (g_cfg.tex_lod_bias != 0.0f && mip >= 2)
        ci.mipLodBias = std::clamp(ci.mipLodBias + g_cfg.tex_lod_bias, -15.0f, 15.0f);
    if (nearest) {
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.anisotropyEnable = VK_FALSE;
    }
    VkSamplerReductionModeCreateInfo red{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const uint32_t rm = (t[1] >> 10) & 3;
    const void** tail = &ci.pNext;
    if (rm) {
        red.reductionMode = rm == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        *tail = &red;
        tail = &red.pNext;
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    float border[4];
    std::memcpy(border, &t[4], 16);
    if (ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
        if (g_vk.has_custom_border) {
            std::memcpy(cb.customBorderColor.float32, border, 16);
            cb.format = VK_FORMAT_UNDEFINED;
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            *tail = &cb;
        } else {
            ci.borderColor = border[3] > 0.5f ? (border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK)
                                              : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        }
    }
    VkSampler s = VK_NULL_HANDLE;
    VKCHECK(vkCreateSampler(g_vk.dev, &ci, nullptr, &s));
    samplers_[key] = s;
    return s;
}

void TextureCache::Invalidate(uint64_t gpu, uint64_t size, bool overlap) {
    gpu = CacheKey(gpu);  // same space as the keys (guest address)
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [addr, im] : images_)
        if (overlap ? (!im->gpu_written && addr < gpu + size && gpu < addr + std::max<uint64_t>(im->guest_size, 1))
                    : (addr >= gpu && addr < gpu + size)) {
            im->uploaded = false;
            im->gpu_written = false;
            epoch.fetch_add(1, std::memory_order_relaxed);
        }
}

bool TextureCache::Overlaps(uint64_t gpu, uint64_t size) {
    gpu = CacheKey(gpu);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [addr, im] : images_)
        if (addr < gpu + size && gpu < addr + std::max<uint64_t>(im->guest_size, 1)) return true;
    return false;
}

VkImageView TextureCache::NullView(VkImageViewType type) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = null_views_.find((int)type);
    if (it != null_views_.end()) return it->second;
    ImageKeyInfo k;
    k.format = VK_FORMAT_R8G8B8A8_UNORM;
    k.width = k.height = 1;
    k.layers = (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ? 6 : 1;
    k.cube = k.layers == 6;
    k.type = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
             : (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D
                                                                                      : VK_IMAGE_TYPE_2D;
    k.gpu = 0xFFFF000000000000ull | (uint64_t)type;
    Image* im = Create(k);
    if (!im) return VK_NULL_HANDLE;
    VkImageView v = View(im, k.format, type, 0, 1, 0, k.layers, {});
    null_views_[(int)type] = v;
    return v;
}

VkSampler TextureCache::NullSampler() {
    if (null_sampler_) return null_sampler_;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    VKCHECK(vkCreateSampler(g_vk.dev, &ci, nullptr, &null_sampler_));
    return null_sampler_;
}

void TextureCache::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [a, im] : images_) {
        for (auto& [k, v] : im->views) vkDestroyImageView(g_vk.dev, v, nullptr);
        vkDestroyImage(g_vk.dev, im->img, nullptr);
        g_vk.FreeImage(im->mem);
    }
    images_.clear();
    epoch.fetch_add(1, std::memory_order_relaxed);
    for (auto& [k, s] : samplers_) vkDestroySampler(g_vk.dev, s, nullptr);
    samplers_.clear();
    if (null_sampler_) vkDestroySampler(g_vk.dev, null_sampler_, nullptr);
}

}  // namespace nvnvk
