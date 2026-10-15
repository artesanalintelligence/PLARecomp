// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fills nvnvk::shader::HostCaps from a VkPhysicalDevice (header-only; include
// <vulkan/vulkan.h> first). Use it when creating the translator so the SPIR-V only requests
// capabilities the device has (e.g. float controls: the RTX 4070 does not support
// fp32 flush-to-zero and spirv-val/the validation layer reject it if declared).
#pragma once

#include <cstring>
#include <vector>

#include "nvnvk/shader_iface.h"

namespace nvnshader {

inline nvnvk::shader::HostCaps HostCapsFromVulkan(VkPhysicalDevice phys) {
    nvnvk::shader::HostCaps c{};
    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    p12.pNext = &fc;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(phys, &p2);

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(phys, &f2);

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    auto has = [&](const char* e) {
        for (const auto& x : exts) {
            if (std::strcmp(x.extensionName, e) == 0) {
                return true;
            }
        }
        return false;
    };

    const uint32_t api = p2.properties.apiVersion;
    c.spirv_version = VK_API_VERSION_MINOR(api) >= 3 ? 0x00010600 : (VK_API_VERSION_MINOR(api) >= 2 ? 0x00010500 : 0x00010300);
    c.float_controls = true; // core in 1.2
    c.separate_denorm_behavior = fc.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
    c.separate_rounding_mode = fc.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
    c.denorm_preserve_fp16 = fc.shaderDenormPreserveFloat16;
    c.denorm_preserve_fp32 = fc.shaderDenormPreserveFloat32;
    c.denorm_flush_fp16 = fc.shaderDenormFlushToZeroFloat16;
    c.denorm_flush_fp32 = fc.shaderDenormFlushToZeroFloat32;
    c.signed_zero_inf_nan_preserve_fp16 = fc.shaderSignedZeroInfNanPreserveFloat16;
    c.signed_zero_inf_nan_preserve_fp32 = fc.shaderSignedZeroInfNanPreserveFloat32;
    c.signed_zero_inf_nan_preserve_fp64 = fc.shaderSignedZeroInfNanPreserveFloat64;
    c.int8 = f12.shaderInt8;
    c.int16 = f2.features.shaderInt16;
    c.int64 = f2.features.shaderInt64;
    c.float16 = f12.shaderFloat16;
    c.float64 = f2.features.shaderFloat64;
    c.storage_8bit = f12.uniformAndStorageBuffer8BitAccess;
    c.storage_16bit = f11.uniformAndStorageBuffer16BitAccess;
    c.demote_to_helper = f13.shaderDemoteToHelperInvocation;
    c.int64_atomics = f12.shaderBufferInt64Atomics;
    c.subgroup_vote = true;
    c.viewport_index_layer_any_stage = f12.shaderOutputLayer && f12.shaderOutputViewportIndex;
    c.typeless_image_loads = f2.features.shaderStorageImageReadWithoutFormat;
    c.native_ndc = has(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    c.scaled_attributes = false;
    c.geometry_streams = has(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    c.sampled_image_array_nonuniform = f12.shaderSampledImageArrayNonUniformIndexing;
    c.explicit_workgroup_layout = has(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
    c.nv_viewport_array2 = has(VK_NV_VIEWPORT_ARRAY_2_EXTENSION_NAME);
    c.nv_geometry_passthrough = has(VK_NV_GEOMETRY_SHADER_PASSTHROUGH_EXTENSION_NAME);
    c.transform_feedback = has(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    c.is_nvidia = p2.properties.vendorID == 0x10DE;
    c.min_ssbo_alignment = static_cast<uint32_t>(p2.properties.limits.minStorageBufferOffsetAlignment);
    c.max_user_clip_distances = p2.properties.limits.maxClipDistances;
    return c;
}

} // namespace nvnshader
