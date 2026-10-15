// SPDX-License-Identifier: GPL-3.0-or-later
//
// Program layout convention (header-only; include <vulkan/vulkan.h> first).
//
// The prewarmed VkPipelineCache (nvnshader_vkcheck --pipeline-cache) only hits if the backend
// creates pipelines with the SAME SPIR-V and the SAME VkPipelineLayout as the prewarm
// (measured on the RTX 4070: changing bindings, the push-descriptor flag or the push constant
// misses; changing render target formats, attribute formats or the dynamic state list does not).
// This function is that convention; nvnshader_vkcheck uses it, and so must nvnvk to get hits:
//   * a single descriptor set with the stages stacked in order (VS, FS...): each stage's
//     bindings start where the previous stage's end (RelocateDescriptors(spirv, 0, base));
//   * stageFlags = the descriptor's stage;
//   * VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
//   * 16-byte push constant (VK_SHADER_STAGE_ALL_GRAPHICS) only if some stage uses
//     render_area.
// It matches what tools/port/nvnvk/src/pipeline.cpp (GetVariant) does today.
#pragma once

#include <algorithm>
#include <vector>

#include "nvnvk/shader_iface.h"

namespace nvnshader {

inline VkShaderStageFlagBits VkStageOf(nvnvk::shader::Stage s) {
    using nvnvk::shader::Stage;
    switch (s) {
    case Stage::Vertex:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case Stage::Fragment:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case Stage::Geometry:
        return VK_SHADER_STAGE_GEOMETRY_BIT;
    case Stage::TessControl:
        return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case Stage::TessEval:
        return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case Stage::Compute:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return VK_SHADER_STAGE_ALL;
}

inline VkDescriptorType VkDescriptorTypeOf(nvnvk::shader::DescriptorType t) {
    using nvnvk::shader::DescriptorType;
    switch (t) {
    case DescriptorType::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorType::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorType::TextureBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case DescriptorType::ImageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case DescriptorType::SampledTexture:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case DescriptorType::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

struct ProgramLayout {
    std::vector<VkDescriptorSetLayoutBinding> bindings; ///< already offset by each stage's base
    uint32_t push_constant_size = 0;                    ///< 0 or 16
    VkShaderStageFlags push_constant_stages = VK_SHADER_STAGE_ALL_GRAPHICS; ///< compute: COMPUTE
};

/// Relocates the stages' SPIR-V (in execution order) and returns the single set's bindings.
inline ProgramLayout LinkProgramStages(std::vector<nvnvk::shader::ShaderModule*>& stages) {
    ProgramLayout out;
    uint32_t base = 0;
    for (nvnvk::shader::ShaderModule* m : stages) {
        if (!m) {
            continue;
        }
        nvnvk::shader::RelocateDescriptors(m->spirv, 0, base);
        uint32_t max_b = 0;
        for (const auto& d : m->descriptors) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = d.binding + base;
            b.descriptorType = VkDescriptorTypeOf(d.type);
            b.descriptorCount = std::max(1u, d.count);
            b.stageFlags = VkStageOf(m->stage);
            out.bindings.push_back(b);
            max_b = std::max(max_b, d.binding + 1);
        }
        base += max_b;
        if (m->uses_render_area) {
            out.push_constant_size = 16;
        }
        if (m->stage == nvnvk::shader::Stage::Compute) {
            out.push_constant_stages = VK_SHADER_STAGE_COMPUTE_BIT;
        }
    }
    return out;
}

/// Creates the convention's set layout (push descriptors) and pipeline layout.
inline VkResult CreateProgramLayout(VkDevice dev, const ProgramLayout& pl, VkDescriptorSetLayout* dsl,
                                    VkPipelineLayout* layout) {
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = static_cast<uint32_t>(pl.bindings.size());
    dci.pBindings = pl.bindings.data();
    VkResult r = vkCreateDescriptorSetLayout(dev, &dci, nullptr, dsl);
    if (r != VK_SUCCESS) {
        return r;
    }
    const VkPushConstantRange pr{pl.push_constant_stages, 0, pl.push_constant_size};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = dsl;
    pli.pushConstantRangeCount = pl.push_constant_size ? 1 : 0;
    pli.pPushConstantRanges = &pr;
    return vkCreatePipelineLayout(dev, &pli, nullptr, layout);
}

} // namespace nvnshader
