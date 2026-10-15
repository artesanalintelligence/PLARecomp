// SPDX-License-Identifier: GPL-3.0-or-later
//
// nvnshader_vkcheck: builds pipelines for every RomFS program (VS+FS pairs of each variation
// and the compute shaders) on the real Vulkan driver to detect driver failures with
// nvnshader's SPIR-V.
//
//   nvnshader_vkcheck <extract_dir> [--programs FILE] [--limit N] [--jobs N] [--layers]
//                     [--report FILE]
//
// <extract_dir>/programs.txt is written by scripts/romfs_scan.py ("vs fs cs n", '-' = absent).
// --layers enables VK_LAYER_KHRONOS_validation and counts the error messages.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

#include "nvnshader/nvnshader.h"
#include "nvnshader/vk_host_caps.h"
#include "nvnshader/vk_program_layout.h"
#include "state_line.h"

namespace fs = std::filesystem;
using namespace nvnshader;

namespace {

std::vector<uint8_t> ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

HostCaps g_caps{};
VkFormat g_rt_format = VK_FORMAT_R16G16B16A16_SFLOAT;
std::string g_nonce;
bool g_stacked = false;     // a single set with the stages stacked (like nvnvk)
bool g_push_desc = false;   // VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR (like nvnvk)
bool g_pc_auto = false;     // push constant only if some module uses render_area (like nvnvk)
bool g_dyn_nvnvk = false;   // same dynamic state list as nvnvk (pipeline.cpp)
VkFormat g_attr_format = VK_FORMAT_R32G32B32A32_SFLOAT;

// Inserts "OpModuleProcessed <text>" before the first annotation: changes the module hash
// without changing its semantics, to force misses in the driver's disk cache (cold measurements).
void AddNonce(std::vector<uint32_t>& spirv, const std::string& text) {
    std::vector<uint32_t> str((text.size() + 4) / 4, 0);
    std::memcpy(str.data(), text.data(), text.size());
    size_t i = 5;
    while (i < spirv.size()) {
        const uint32_t op = spirv[i] & 0xFFFF;
        const uint32_t n = spirv[i] >> 16;
        if (n == 0) {
            return;
        }
        if (op == 71 || op == 72 || op == 73 || op == 74 || op == 332 || op == 5632 || op == 5633) {
            break;
        }
        i += n;
    }
    std::vector<uint32_t> ins;
    ins.push_back(static_cast<uint32_t>((1 + str.size()) << 16) | 330u); // OpModuleProcessed
    ins.insert(ins.end(), str.begin(), str.end());
    spirv.insert(spirv.begin() + static_cast<std::ptrdiff_t>(i), ins.begin(), ins.end());
}
std::unique_ptr<ITranslator> g_tr;
VkPipelineCache g_pcache = VK_NULL_HANDLE;
std::atomic<uint64_t> g_ns_translate{0};
std::atomic<uint64_t> g_ns_pipeline{0};
uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
std::atomic<uint32_t> g_layer_errors{0};
std::mutex g_layer_mtx;
std::map<std::string, uint32_t> g_layer_msgs;

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                       VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_layer_errors;
        std::scoped_lock lk{g_layer_mtx};
        std::string id = data->pMessageIdName ? data->pMessageIdName : "?";
        g_layer_msgs[id]++;
        if (g_layer_msgs[id] == 1) {
            std::fprintf(stderr, "[layer] %s\n", data->pMessage ? data->pMessage : "");
        }
    }
    return VK_FALSE;
}

struct Vk {
    VkInstance instance{};
    VkPhysicalDevice phys{};
    VkDevice device{};
    VkDebugUtilsMessengerEXT messenger{};
    std::string name;
    bool xfb = false;

    bool Init(bool layers) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "nvnshader_vkcheck";
        app.apiVersion = VK_API_VERSION_1_3;
        std::vector<const char*> inst_layers;
        std::vector<const char*> inst_exts;
        if (layers) {
            inst_layers.push_back("VK_LAYER_KHRONOS_validation");
            inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledLayerCount = static_cast<uint32_t>(inst_layers.size());
        ici.ppEnabledLayerNames = inst_layers.data();
        ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
        ici.ppEnabledExtensionNames = inst_exts.data();
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            std::fprintf(stderr, "vkCreateInstance failed\n");
            return false;
        }
        if (layers) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            dci.pfnUserCallback = DebugCb;
            if (create) {
                create(instance, &dci, nullptr, &messenger);
            }
        }
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());
        for (VkPhysicalDevice d : devs) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            if (!phys || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                phys = d;
                name = p.deviceName;
                char ver[64];
                std::snprintf(ver, sizeof(ver), " (driver 0x%x)", p.driverVersion);
                name += ver;
            }
        }
        if (!phys) {
            return false;
        }
        // Every feature the device supports (suyu's SPIR-V uses Int8/16/64,
        // Float16/64, demote, typeless image reads, etc.).
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &ne, exts.data());
        auto has = [&](const char* e) {
            return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& x) {
                return std::strcmp(x.extensionName, e) == 0;
            });
        };
        static VkPhysicalDeviceTransformFeedbackFeaturesEXT tf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        static VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
        static VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
        static VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        static VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        static VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        static VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        static VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = nullptr;
        void** tail = &f13.pNext;
        std::vector<const char*> dev_exts;
        auto push = [&](const char* e, void* feat) {
            if (!has(e)) {
                return false;
            }
            dev_exts.push_back(e);
            if (!feat) {
                return true;
            }
            *tail = feat;
            tail = reinterpret_cast<void**>(&reinterpret_cast<VkBaseOutStructure*>(feat)->pNext);
            *tail = nullptr;
            return true;
        };
        xfb = push(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, &tf);
        push(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, &af);
        push(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME, &wg);
        push(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &dcc);
        push(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, nullptr);
        vkGetPhysicalDeviceFeatures2(phys, &f2);
        f2.features.robustBufferAccess = VK_FALSE;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = 0;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
        dci.ppEnabledExtensionNames = dev_exts.data();
        const VkResult r = vkCreateDevice(phys, &dci, nullptr, &device);
        if (r != VK_SUCCESS) {
            std::fprintf(stderr, "vkCreateDevice failed: %d\n", r);
            return false;
        }
        return true;
    }
    ~Vk() {
        if (device) {
            vkDestroyDevice(device, nullptr);
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) {
                destroy(instance, messenger, nullptr);
            }
        }
        if (instance) {
            vkDestroyInstance(instance, nullptr);
        }
    }
};

VkDescriptorType ToVk(DescriptorType t) {
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

struct Translated {
    bool ok = false;
    std::string error;
    ShaderModule module;
};

bool TranslateHash(const fs::path& dir, const std::string& hash, const TranslationState& st,
                   Translated& out) {
    const auto ctrl = ReadFile(dir / (hash + ".ctrl"));
    const auto data = ReadFile(dir / (hash + ".data"));
    ShaderBinary b{ctrl.data(), ctrl.size(), data.data(), data.size()};
    const uint64_t t0 = NowNs();
    const bool ok = g_tr->Translate(b, st, &out.module, &out.error);
    g_ns_translate += NowNs() - t0;
    out.ok = ok;
    return ok;
}

struct Built {
    VkResult result = VK_SUCCESS;
    std::string error;
};

class Builder {
public:
    explicit Builder(Vk& vk_) : vk{vk_} {}

    VkShaderModule Module(const std::vector<uint32_t>& code_in) {
        std::vector<uint32_t> code = code_in;
        if (!g_nonce.empty()) {
            AddNonce(code, g_nonce);
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        VkShaderModule m{};
        if (vkCreateShaderModule(vk.device, &ci, nullptr, &m) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return m;
    }

    VkDescriptorSetLayout SetLayout(const ShaderModule& m, VkShaderStageFlags stage) {
        std::vector<VkDescriptorSetLayoutBinding> b;
        for (const Descriptor& d : m.descriptors) {
            b.push_back({d.binding, ToVk(d.type), d.count, stage, nullptr});
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (g_push_desc) {
            ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
        ci.bindingCount = static_cast<uint32_t>(b.size());
        ci.pBindings = b.data();
        VkDescriptorSetLayout l{};
        vkCreateDescriptorSetLayout(vk.device, &ci, nullptr, &l);
        return l;
    }

    Built Compute(ShaderModule& cs) {
        Built out;
        VkDescriptorSetLayout set{};
        VkPipelineLayout layout{};
        if (g_push_desc) {
            std::vector<ShaderModule*> st{&cs};
            CreateProgramLayout(vk.device, LinkProgramStages(st), &set, &layout);
        } else {
            set = SetLayout(cs, VK_SHADER_STAGE_COMPUTE_BIT);
            VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            lci.setLayoutCount = 1;
            lci.pSetLayouts = &set;
            vkCreatePipelineLayout(vk.device, &lci, nullptr, &layout);
        }
        VkShaderModule mod = Module(cs.spirv);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = layout;
        VkPipeline p{};
        const uint64_t t0 = NowNs();
        out.result = vkCreateComputePipelines(vk.device, g_pcache, 1, &ci, nullptr, &p);
        g_ns_pipeline += NowNs() - t0;
        if (p) {
            vkDestroyPipeline(vk.device, p, nullptr);
        }
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
        vkDestroyDescriptorSetLayout(vk.device, set, nullptr);
        vkDestroyShaderModule(vk.device, mod, nullptr);
        return out;
    }

    Built Graphics(ShaderModule& vs, ShaderModule* fs) {
        Built out;
        // One set per stage: VS = set 0, FS = set 1 (see shader_iface.h); or, with --layout stacked,
        // a single set with the FS bindings after the VS ones (what nvnvk does).
        std::array<VkDescriptorSetLayout, 2> sets{};
        uint32_t nsets = 1;
        VkPipelineLayout conv_layout = VK_NULL_HANDLE;
        if (g_push_desc) {
            std::vector<ShaderModule*> st{&vs};
            if (fs) {
                st.push_back(fs);
            }
            CreateProgramLayout(vk.device, LinkProgramStages(st), &sets[0], &conv_layout);
        } else if (g_stacked) {
            const uint32_t base = static_cast<uint32_t>(vs.descriptors.size());
            ShaderModule merged = vs;
            if (fs) {
                nvnvk::shader::RelocateDescriptors(fs->spirv, 0, base);
                for (Descriptor d : fs->descriptors) {
                    d.binding += base;
                    merged.descriptors.push_back(d);
                }
            }
            sets[0] = SetLayout(merged, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        } else {
            if (fs) {
                nvnvk::shader::RelocateDescriptors(fs->spirv, 1, 0);
            }
            sets = {SetLayout(vs, VK_SHADER_STAGE_VERTEX_BIT),
                    fs ? SetLayout(*fs, VK_SHADER_STAGE_FRAGMENT_BIT) : VK_NULL_HANDLE};
            nsets = fs ? 2 : 1;
        }
        VkShaderModule vmod = Module(vs.spirv);
        VkShaderModule fmod = fs ? Module(fs->spirv) : VK_NULL_HANDLE;
        VkPushConstantRange pc{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = nsets;
        lci.pSetLayouts = sets.data();
        lci.pushConstantRangeCount =
            (!g_pc_auto || vs.uses_render_area || (fs && fs->uses_render_area)) ? 1 : 0;
        lci.pPushConstantRanges = &pc;
        VkPipelineLayout layout = conv_layout;
        if (!layout) {
            vkCreatePipelineLayout(vk.device, &lci, nullptr, &layout);
        }

        std::vector<VkVertexInputAttributeDescription> attrs;
        for (uint32_t i = 0; i < 32; ++i) {
            const uint64_t bits = (vs.input_mask[(i * 4) / 64] >> ((i * 4) % 64)) & 0xF;
            if (bits) {
                attrs.push_back({i, 0, g_attr_format, 0});
            }
        }
        VkVertexInputBindingDescription bind{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = attrs.empty() ? 0 : 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        rs.rasterizerDiscardEnable = fs ? VK_FALSE : VK_TRUE;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        uint32_t ncolor = 0;
        if (fs) {
            for (uint32_t i = 0; i < 8; ++i) {
                if (fs->color_outputs_mask & (1u << i)) {
                    ncolor = i + 1;
                }
            }
        }
        std::vector<VkFormat> formats(ncolor, g_rt_format);
        std::vector<VkPipelineColorBlendAttachmentState> blends(ncolor);
        for (auto& b : blends) {
            b.colorWriteMask = 0xF;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = ncolor;
        cb.pAttachments = blends.data();
        std::array<VkDynamicState, 2> dyn{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        static const VkDynamicState dyn_nvnvk[] = {
            VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH,
            VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = static_cast<uint32_t>(dyn.size());
        dy.pDynamicStates = dyn.data();
        if (g_dyn_nvnvk) {
            dy.dynamicStateCount = static_cast<uint32_t>(sizeof(dyn_nvnvk) / sizeof(dyn_nvnvk[0]));
            dy.pDynamicStates = dyn_nvnvk;
            vp.viewportCount = 0;
            vp.scissorCount = 0;
            rs.rasterizerDiscardEnable = VK_FALSE;
        }
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = ncolor;
        ri.pColorAttachmentFormats = formats.data();
        ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, vmod, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main", nullptr};
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &ri;
        ci.stageCount = fs ? 2 : 1;
        ci.pStages = stages.data();
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        VkPipeline p{};
        if (!vmod || (fs && !fmod)) {
            out.result = VK_ERROR_INVALID_SHADER_NV;
            out.error = "vkCreateShaderModule";
        } else {
            const uint64_t t0 = NowNs();
            out.result = vkCreateGraphicsPipelines(vk.device, g_pcache, 1, &ci, nullptr, &p);
            g_ns_pipeline += NowNs() - t0;
        }
        if (p) {
            vkDestroyPipeline(vk.device, p, nullptr);
        }
        vkDestroyPipelineLayout(vk.device, layout, nullptr);
        for (auto s : sets) {
            if (s) {
                vkDestroyDescriptorSetLayout(vk.device, s, nullptr);
            }
        }
        if (vmod) {
            vkDestroyShaderModule(vk.device, vmod, nullptr);
        }
        if (fmod) {
            vkDestroyShaderModule(vk.device, fmod, nullptr);
        }
        return out;
    }

private:
    Vk& vk;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: nvnshader_vkcheck <extract_dir> [--programs FILE] [--limit N] "
                             "[--jobs N] [--layers] [--report FILE] [--spv-cache DIR] [--pipeline-cache FILE] "
                             "[--opt Os|O] [--rt rgba8|rgba16f] [--no-driver-cache]\n");
        return 1;
    }
    fs::path extract = argv[1];
    fs::path programs = extract / "programs.txt";
    std::string report = "nvnshader_vkcheck.txt";
    size_t limit = 0;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency() / 2);
    bool layers = false;
    std::string spv_cache;
    std::string pipeline_cache;
    OptLevel opt = OptLevel::None;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--programs") {
            programs = next();
        } else if (a == "--limit") {
            limit = std::stoul(next());
        } else if (a == "--jobs") {
            jobs = static_cast<unsigned>(std::stoul(next()));
        } else if (a == "--layers") {
            layers = true;
        } else if (a == "--report") {
            report = next();
        } else if (a == "--spv-cache") {
            spv_cache = next();
        } else if (a == "--pipeline-cache") {
            pipeline_cache = next();
        } else if (a == "--opt") {
            const std::string v = next();
            opt = v == "Os" ? OptLevel::Size : v == "O" ? OptLevel::Performance : OptLevel::None;
        } else if (a == "--rt") {
            g_rt_format = next() == "rgba8" ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
        } else if (a == "--layout") {
            const std::string v = next();
            g_stacked = v == "stacked" || v == "nvnvk";
            g_push_desc = g_pc_auto = v == "nvnvk";
        } else if (a == "--va") {
            g_attr_format = next() == "half" ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
        } else if (a == "--dyn") {
            g_dyn_nvnvk = next() == "nvnvk";
        } else if (a == "--nonce") {
            g_nonce = next();
        } else if (a == "--no-driver-cache") {
            // NVIDIA driver disk cache (also used by Vulkan): disable to measure cold compilation.
            _putenv_s("__GL_SHADER_DISK_CACHE", "0");
        }
    }
    const fs::path shaders = extract / "shaders";
    // Line: "vs fs cs n [; vs_state ; fs_state]" (states in --states format, without hash).
    struct Prog {
        std::string vs, fs, cs;
        TranslationState vs_state{}, fs_state{}, cs_state{};
    };
    std::vector<Prog> progs;
    {
        std::ifstream f(programs);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::string states_part;
            if (const auto semi = line.find(';'); semi != std::string::npos) {
                states_part = line.substr(semi + 1);
                line = line.substr(0, semi);
            }
            std::istringstream is(line);
            Prog p;
            is >> p.vs >> p.fs >> p.cs;
            if (!states_part.empty()) {
                const auto semi2 = states_part.find(';');
                std::string h, tag;
                nvnshader::tools::ParseStateLine("x " + states_part.substr(0, semi2), h, p.vs_state, tag);
                if (semi2 != std::string::npos) {
                    nvnshader::tools::ParseStateLine("x " + states_part.substr(semi2 + 1), h, p.fs_state, tag);
                }
                p.cs_state = p.vs_state;
            }
            auto dash = [](std::string& s) {
                if (s == "-") {
                    s.clear();
                }
            };
            dash(p.vs);
            dash(p.fs);
            dash(p.cs);
            progs.push_back(p);
        }
    }
    if (limit && progs.size() > limit) {
        progs.resize(limit);
    }
    Vk vk;
    if (!vk.Init(layers)) {
        return 1;
    }
    g_caps = HostCapsFromVulkan(vk.phys);
    TranslateOptions topts{};
    topts.optimize = opt;
    g_tr = CreateTranslator(g_caps, spv_cache, topts);
    std::vector<uint8_t> pc_initial;
    if (!pipeline_cache.empty()) {
        pc_initial = ReadFile(pipeline_cache);
        VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pci.initialDataSize = pc_initial.size();
        pci.pInitialData = pc_initial.empty() ? nullptr : pc_initial.data();
        if (vkCreatePipelineCache(vk.device, &pci, nullptr, &g_pcache) != VK_SUCCESS) {
            pci.initialDataSize = 0; // data from another driver/device: start empty
            pci.pInitialData = nullptr;
            vkCreatePipelineCache(vk.device, &pci, nullptr, &g_pcache);
        }
        std::printf("VkPipelineCache: %zu initial bytes\n", pc_initial.size());
    }
    std::printf("device: %s; %zu programs; %u threads; layers %s\n", vk.name.c_str(),
                progs.size(), jobs, layers ? "yes" : "no");
    std::fflush(stdout);
    std::atomic<size_t> next_i{0}, done{0}, ok{0}, fail_tr{0}, fail_vk{0};
    std::mutex mtx;
    std::vector<std::string> failures;
    const auto t0 = std::chrono::steady_clock::now();
    auto worker = [&] {
        Builder b{vk};
        for (;;) {
            const size_t i = next_i.fetch_add(1);
            if (i >= progs.size()) {
                return;
            }
            const Prog& p = progs[i];
            Built built;
            std::string terr;
            if (!p.cs.empty()) {
                Translated cs;
                if (!TranslateHash(shaders, p.cs, p.cs_state, cs)) {
                    terr = "CS " + p.cs + ": " + cs.error;
                } else {
                    built = b.Compute(cs.module);
                }
            } else {
                Translated vs, fs;
                if (!TranslateHash(shaders, p.vs, p.vs_state, vs)) {
                    terr = "VS " + p.vs + ": " + vs.error;
                } else {
                    TranslationState fst = p.fs_state;
                    fst.previous_stage_stores.assign(vs.module.stores.begin(), vs.module.stores.end());
                    if (!p.fs.empty() && !TranslateHash(shaders, p.fs, fst, fs)) {
                        terr = "FS " + p.fs + ": " + fs.error;
                    } else {
                        built = b.Graphics(vs.module, p.fs.empty() ? nullptr : &fs.module);
                    }
                }
            }
            if (!terr.empty()) {
                ++fail_tr;
                std::scoped_lock lk{mtx};
                failures.push_back("translation: " + terr);
            } else if (built.result != VK_SUCCESS) {
                ++fail_vk;
                std::scoped_lock lk{mtx};
                failures.push_back("vk " + std::to_string(built.result) + ": " + p.vs + " " + p.fs +
                                   " " + p.cs + " " + built.error);
            } else {
                ++ok;
            }
            const size_t d = ++done;
            if (d % 500 == 0) {
                std::scoped_lock lk{mtx};
                std::printf("  %zu/%zu\n", d, progs.size());
                std::fflush(stdout);
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned j = 0; j < jobs; ++j) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    size_t pc_size = 0;
    if (g_pcache) {
        vkGetPipelineCacheData(vk.device, g_pcache, &pc_size, nullptr);
        std::vector<uint8_t> blob(pc_size);
        vkGetPipelineCacheData(vk.device, g_pcache, &pc_size, blob.data());
        std::ofstream pf(pipeline_cache, std::ios::binary | std::ios::trunc);
        pf.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(pc_size));
        vkDestroyPipelineCache(vk.device, g_pcache, nullptr);
        g_pcache = VK_NULL_HANDLE;
    }
    std::printf("translation time (sum over threads) %.1f s; pipeline creation (sum over threads) %.1f s; "
                "VkPipelineCache saved: %.1f MB\n",
                g_ns_translate.load() / 1e9, g_ns_pipeline.load() / 1e9, pc_size / 1e6);
    std::printf("\npipelines created %zu / %zu; translation failures %zu; Vulkan failures %zu; "
                "validation layer errors %u; %.1f s\n",
                ok.load(), progs.size(), fail_tr.load(), fail_vk.load(), g_layer_errors.load(), secs);
    std::ofstream rf(report);
    rf << "device " << vk.name << "\nprograms " << progs.size() << "\nok " << ok.load()
       << "\ntranslation_failures " << fail_tr.load() << "\nvulkan_failures " << fail_vk.load()
       << "\nlayer_errors " << g_layer_errors.load() << "\nseconds " << secs
       << "\ntranslate_thread_s " << g_ns_translate.load() / 1e9 << "\npipeline_thread_s "
       << g_ns_pipeline.load() / 1e9 << "\npipeline_cache_bytes " << pc_size << "\n";
    for (const auto& [id, n] : g_layer_msgs) {
        rf << "layer " << n << " " << id << "\n";
    }
    for (const auto& f : failures) {
        rf << f << "\n";
    }
    return (fail_tr == 0 && fail_vk == 0) ? 0 : 2;
}
