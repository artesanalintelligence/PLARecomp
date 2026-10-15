// Formats: NVN -> Vulkan, Maxwell TIC -> Vulkan, vertex attributes and state enums.
// Tables derived from the driver's own format table (generated/nvn_formats.inc) and the public
// Maxwell formats (NVN-SEMANTICS.md section 4).
// SPDX-License-Identifier: MIT
#pragma once

#include "vkctx.h"

namespace nvnvk {

struct FormatInfo {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    uint32_t bpb = 0;          // bytes per block (per texel when uncompressed)
    uint32_t bw = 1, bh = 1;   // block dimensions in texels
    uint32_t tic_w0 = 0;       // TIC word 0 used by the driver
    uint32_t rt = 0;           // Maxwell render target format
    uint32_t vertex = 0;       // Maxwell vertex attribute word
};

const FormatInfo& NvnFormat(uint32_t nvn);
// Image format from TIC word 0 (+ sRGB bit of word 4).
VkFormat TicFormat(uint32_t w0, bool srgb, uint32_t* bpb, uint32_t* bw, uint32_t* bh);
VkComponentMapping TicSwizzle(uint32_t w0);
// NVN vertex attribute (NVN format) -> VkFormat
VkFormat VertexFormat(uint32_t nvn_format);
bool IsDepthFormat(VkFormat f);
bool IsStencilFormat(VkFormat f);
VkImageAspectFlags AspectOf(VkFormat f);
bool IsCompressed(VkFormat f);

VkCompareOp CompareOp(uint32_t nvn);     // 1..8
VkStencilOp StencilOp(uint32_t nvn);     // 1..8
VkBlendOp BlendOp(uint32_t nvn);         // 1..5
VkBlendFactor BlendFactor(uint32_t nvn); // D3D + "OGL" (0x61-0x64)
VkPrimitiveTopology Topology(uint32_t nvn, bool* restart_ok);
VkIndexType IndexType(uint32_t nvn, uint32_t* bytes);

}  // namespace nvnvk
