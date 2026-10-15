// SPDX-License-Identifier: MIT
#include "formats.h"

#include <unordered_map>

#include "nvnvk.h"

namespace nvnvk {

namespace {

struct RawFormat {
    uint32_t nvn;
    VkFormat vk;
    uint32_t bytes;
    uint32_t tic_w0;
    uint32_t rt;
    uint32_t vertex;
};

const RawFormat kRaw[] = {
#define NVN_FORMAT(n, vk, bytes, tic, rt, vtx) {n, vk, bytes, tic, rt, vtx},
#include "nvn_formats.inc"
#undef NVN_FORMAT
};

void BlockDims(VkFormat f, uint32_t* bw, uint32_t* bh) {
    *bw = *bh = 1;
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) *bw = *bh = 4;
    if (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) *bw = *bh = 4;
    switch (f) {
#define A(P, Q) case VK_FORMAT_ASTC_##P##x##Q##_UNORM_BLOCK: case VK_FORMAT_ASTC_##P##x##Q##_SRGB_BLOCK: *bw = P; *bh = Q; break;
        A(4, 4) A(5, 4) A(5, 5) A(6, 5) A(6, 6) A(8, 5) A(8, 6) A(8, 8) A(10, 5) A(10, 6) A(10, 8) A(10, 10) A(12, 10) A(12, 12)
#undef A
        default: break;
    }
}

uint32_t BytesOf(VkFormat f, uint32_t fallback) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_A4B4G4R4_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
        case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return 16;
        default: break;
    }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return 16;
    return fallback ? fallback : 4;
}

bool IsSrgb(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return true;
        default: return f >= VK_FORMAT_ASTC_4x4_SRGB_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK &&
                        ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1);
    }
}

struct Tables {
    std::vector<FormatInfo> nvn;
    std::unordered_map<uint32_t, VkFormat> tic;  // (w0 & 0x7ffff) | srgb << 31
    Tables() {
        uint32_t maxn = 0;
        for (auto& r : kRaw) maxn = r.nvn > maxn ? r.nvn : maxn;
        nvn.resize(maxn + 1);
        for (auto& r : kRaw) {
            FormatInfo f;
            f.vk = r.vk;
            BlockDims(r.vk, &f.bw, &f.bh);
            f.bpb = BytesOf(r.vk, r.bytes);
            f.tic_w0 = r.tic_w0;
            f.rt = r.rt;
            f.vertex = r.vertex;
            nvn[r.nvn] = f;
            if (r.tic_w0 && r.vk != VK_FORMAT_UNDEFINED) {
                const bool srgb = IsSrgb(r.vk);
                const uint32_t key = (r.tic_w0 & 0x7FFFF) | (srgb ? 0x80000000u : 0);
                // First one wins (BGRA formats share the TIC with RGBA: the swizzle decides there)
                if (!tic.count(key)) tic[key] = r.vk;
            }
        }
    }
};

const Tables& T() {
    static Tables t;
    return t;
}

VkFormat VertexFromWord(uint32_t w) {
    const uint32_t size = (w >> 21) & 0x3F, type = (w >> 27) & 7;
    // type: 1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled, 7 float
#define SEL(sn, un, si, ui, us, ss, fl) \
    switch (type) { case 1: return sn; case 2: return un; case 3: return si; case 4: return ui; case 5: return us; case 6: return ss; case 7: return fl; default: return VK_FORMAT_UNDEFINED; }
    switch (size) {
        case 0x01: SEL(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32A32_SFLOAT)
        case 0x02: SEL(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32B32_SFLOAT)
        case 0x03: SEL(VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SFLOAT)
        case 0x04: SEL(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32G32_SFLOAT)
        case 0x05: SEL(VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16_SFLOAT)
        case 0x0A: SEL(VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_UNDEFINED)
        case 0x0F: SEL(VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_SFLOAT)
        case 0x12: SEL(VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SINT, VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT)
        case 0x13: SEL(VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_UNDEFINED)
        case 0x18: SEL(VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_UNDEFINED)
        case 0x1B: SEL(VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED, VK_FORMAT_R16_SFLOAT)
        case 0x1D: SEL(VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SINT, VK_FORMAT_R8_UINT, VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_UNDEFINED)
        case 0x30: SEL(VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32, VK_FORMAT_UNDEFINED)
        case 0x31: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
#undef SEL
}

}  // namespace

const FormatInfo& NvnFormat(uint32_t nvn) {
    static const FormatInfo none{};
    const auto& t = T();
    return nvn < t.nvn.size() ? t.nvn[nvn] : none;
}

VkFormat TicFormat(uint32_t w0, bool srgb, uint32_t* bpb, uint32_t* bw, uint32_t* bh) {
    const auto& t = T();
    auto it = t.tic.find((w0 & 0x7FFFF) | (srgb ? 0x80000000u : 0));
    if (it == t.tic.end()) it = t.tic.find(w0 & 0x7FFFF);
    if (it == t.tic.end()) {
        // Per-component types differ from the table's (e.g. FLOAT in one channel): retry
        // with every channel using the R channel's type.
        const uint32_t r = (w0 >> 7) & 7;
        const uint32_t key = (w0 & 0x7F) | (r << 7) | (r << 10) | (r << 13) | (r << 16);
        it = t.tic.find(key);
        if (it == t.tic.end()) return VK_FORMAT_UNDEFINED;
    }
    VkFormat f = it->second;
    if (bpb) *bpb = BytesOf(f, 4);
    uint32_t a, b;
    BlockDims(f, &a, &b);
    if (bw) *bw = a;
    if (bh) *bh = b;
    return f;
}

VkComponentMapping TicSwizzle(uint32_t w0) {
    auto one = [](uint32_t s) {
        switch (s) {
            case 0: return VK_COMPONENT_SWIZZLE_ZERO;
            case 2: return VK_COMPONENT_SWIZZLE_R;
            case 3: return VK_COMPONENT_SWIZZLE_G;
            case 4: return VK_COMPONENT_SWIZZLE_B;
            case 5: return VK_COMPONENT_SWIZZLE_A;
            case 6: case 7: return VK_COMPONENT_SWIZZLE_ONE;
            default: return VK_COMPONENT_SWIZZLE_IDENTITY;
        }
    };
    return {one((w0 >> 19) & 7), one((w0 >> 22) & 7), one((w0 >> 25) & 7), one((w0 >> 28) & 7)};
}

VkFormat VertexFormat(uint32_t nvn_format) { return VertexFromWord(NvnFormat(nvn_format).vertex); }

bool IsDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}
bool IsStencilFormat(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
VkImageAspectFlags AspectOf(VkFormat f) {
    if (!IsDepthFormat(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (IsStencilFormat(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}
bool IsCompressed(VkFormat f) {
    uint32_t a, b;
    BlockDims(f, &a, &b);
    return a > 1;
}

VkCompareOp CompareOp(uint32_t v) {
    static const VkCompareOp k[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                    VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                    VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return v < 9 ? k[v] : VK_COMPARE_OP_ALWAYS;
}
VkStencilOp StencilOp(uint32_t v) {
    static const VkStencilOp k[] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                    VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                    VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return v < 9 ? k[v] : VK_STENCIL_OP_KEEP;
}
VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}
VkBlendFactor BlendFactor(uint32_t v) {
    switch (v) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkPrimitiveTopology Topology(uint32_t v, bool* restart_ok) {
    *restart_ok = true;
    switch (v) {
        case 0: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 4: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 0xa: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case 0xb: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case 0xc: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case 0xd: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case 0xe: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: *restart_ok = false; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // QUADS: unsupported
    }
}
VkIndexType IndexType(uint32_t v, uint32_t* bytes) {
    switch (v) {
        case 0: *bytes = 1; return VK_INDEX_TYPE_UINT8_EXT;
        case 2: *bytes = 4; return VK_INDEX_TYPE_UINT32;
        default: *bytes = 2; return VK_INDEX_TYPE_UINT16;
    }
}

}  // namespace nvnvk
