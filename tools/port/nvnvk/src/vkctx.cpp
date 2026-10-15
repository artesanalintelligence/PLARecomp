// SPDX-License-Identifier: MIT
#include "vkctx.h"
#include "renderdoc_app.h"
#include <dxgi.h>

#include <algorithm>

#include "nvnvk.h"
#include "overlay.h"

namespace nvnvk {

VkCtx g_vk;
Presenter g_present;

void VkFail(const char* what, VkResult r, const char* file, int line) {
    NVK_ERR("Vulkan: %s = %d (%s:%d)", what, (int)r, file, line);
}

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL DebugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        // Each message type (messageIdNumber) is logged at most 3 times; synchronization hazards
        // (VK_LAYER_VALIDATE_SYNC) repeat every frame.
        static std::mutex mu;
        static std::map<int32_t, int> seen;
        static int total = 0;
        std::lock_guard<std::mutex> lk(mu);
        const int n = ++seen[data->messageIdNumber];
        if (n <= 3 && total < 400) {
            ++total;
            NVK_WARN("[vk] (%d) %s", n, data->pMessage);
        }
    }
    return VK_FALSE;
}
VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

bool HasExt(const std::vector<VkExtensionProperties>& v, const char* n) {
    for (auto& e : v)
        if (!std::strcmp(e.extensionName, n)) return true;
    return false;
}

// Keep third-party implicit layers (RTSS, Overwolf, OBS, Medal, Steam, EOS...) out of OUR instance.
// In shadow mode there are two VkDevices in the process (suyu's and ours) and those overlay hooks
// mix up their objects: intro3 crashed in nvoglv64.dll on suyu's GPU thread with RTSSHooks64 on the
// stack. They're disabled only while our instance is created (the loader reads the variables in
// vkCreateInstance) and restored right after, so suyu's instance is left untouched.
class ScopedEnv {
public:
    void Set(const char* name, const char* value) {
        char buf[1024];
        const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
        saved_.push_back({name, n > 0 && n < sizeof buf, n > 0 && n < sizeof buf ? std::string(buf, n) : std::string()});
        SetEnvironmentVariableA(name, value);
    }
    ~ScopedEnv() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it)
            SetEnvironmentVariableA(it->name, it->had ? it->value.c_str() : nullptr);
    }

private:
    struct Saved {
        const char* name;
        bool had;
        std::string value;
    };
    std::vector<Saved> saved_;
};

void DisableImplicitLayers(ScopedEnv& env) {
    env.Set("VK_LOADER_LAYERS_DISABLE", "~implicit~");  // loader >= 1.3.234
    env.Set("VK_LOADER_LAYERS_ALLOW", "*NV_*");          // the driver's own layers (optimus, present) stay
    // Old loaders: the disable variable of each known manifest.
    for (const char* v : {"DISABLE_RTSS_LAYER", "DISABLE_VULKAN_OW_OBS_CAPTURE", "DISABLE_VULKAN_OW_OVERLAY_LAYER",
                          "DISABLE_VULKAN_OBS_CAPTURE", "DISABLE_VK_LAYER_VALVE_steam_overlay_1",
                          "DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "DISABLE_MEDAL_LAYER", "DISABLE_EOS_OVERLAY_LAYER"})
        env.Set(v, "1");
}
}  // namespace

bool VkCtx::Init() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "PLA nvnvk";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    // RenderDoc debugging (renderdoc_dir): its layer, only in our instance (VK_ADD_LAYER_PATH); the capture API comes
    // from renderdoc.dll, which the layer loads.
    if (!g_cfg.renderdoc_dir.empty()) {
        LoadLibraryA((g_cfg.renderdoc_dir + "\renderdoc.dll").c_str());
    }
    if (g_cfg.validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    VkResult r;
    {
        ScopedEnv env;
        if (g_cfg.isolate_layers) DisableImplicitLayers(env);
        if (!g_cfg.renderdoc_dir.empty()) {
            // The RenderDoc layer can only be enabled as implicit (it refuses to load by name).
            env.Set("VK_ADD_IMPLICIT_LAYER_PATH", g_cfg.renderdoc_dir.c_str());
            env.Set("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1");
            if (g_cfg.isolate_layers) env.Set("VK_LOADER_LAYERS_ALLOW", "*NV_*,VK_LAYER_RENDERDOC_Capture");
        }
        r = vkCreateInstance(&ici, nullptr, &instance);
        if (!g_cfg.renderdoc_dir.empty()) {
            if (HMODULE m = GetModuleHandleA("renderdoc.dll")) {
                auto get = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI");
                RENDERDOC_API_1_6_0* api = nullptr;
                if (get && get(eRENDERDOC_API_Version_1_6_0, (void**)&api) == 1 && api) {
                    api->SetCaptureFilePathTemplate((g_cfg.capture_dir + "/rdoc").c_str());
                    api->MaskOverlayBits(0, 0);
                    rdoc = api;
                }
            }
            NVK_INFO("RenderDoc: %s (instance %d)", rdoc ? "API ready" : "unavailable", (int)r);
        }
        if (r != VK_SUCCESS && g_cfg.validation) {
            NVK_WARN("no validation layer (%d); continuing without it", (int)r);
            ici.enabledLayerCount = 0;
            exts.pop_back();
            ici.enabledExtensionCount = (uint32_t)exts.size();
            g_cfg.validation = false;
            r = vkCreateInstance(&ici, nullptr, &instance);
        }
    }
    NVK_INFO("third-party implicit layers in our instance: %s", g_cfg.isolate_layers ? "disabled" : "allowed");
    if (r != VK_SUCCESS) {
        NVK_ERR("vkCreateInstance = %d", (int)r);
        return false;
    }
    if (g_cfg.validation) {
        auto f = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mi.pfnUserCallback = DebugCb;
        if (f) f(instance, &mi, nullptr, &g_messenger);
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    // GPU choice: the one given in "gpu" (index or part of the name) or else the best by type
    // (discrete > integrated > virtual > CPU), ties broken by most VRAM: on laptops the iGPU
    // is often listed first.
    auto rank = [](VkPhysicalDeviceType t) {
        switch (t) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
            default: return 0;
        }
    };
    auto vram = [](VkPhysicalDevice d) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(d, &mp);
        VkDeviceSize v = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) v = std::max(v, mp.memoryHeaps[i].size);
        return v;
    };
    int want = -1;
    if (!g_cfg.gpu.empty() && g_cfg.gpu.find_first_not_of("0123456789") == std::string::npos) want = std::stoi(g_cfg.gpu);
    int best = -1, best_rank = -1;
    VkDeviceSize best_vram = 0;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        const VkDeviceSize v = vram(devs[i]);
        NVK_HOST("GPU %u: %s (type %d, %llu MiB)%s", i, p.deviceName, (int)p.deviceType, (unsigned long long)(v >> 20),
                 p.apiVersion < VK_API_VERSION_1_3 ? " [no Vulkan 1.3]" : "");
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        if (want >= 0 || !g_cfg.gpu.empty()) {
            const bool match = want >= 0 ? (int)i == want : std::string(p.deviceName).find(g_cfg.gpu) != std::string::npos;
            if (match && best_rank < 100) {
                best = (int)i;
                best_rank = 100;
            }
        }
        const int rk = rank(p.deviceType);
        if (best_rank < 100 && (rk > best_rank || (rk == best_rank && v > best_vram))) {
            best = (int)i;
            best_rank = rk;
            best_vram = v;
        }
    }
    if (!g_cfg.gpu.empty() && best_rank < 100) NVK_WARN("gpu=\"%s\" not found; using the best type", g_cfg.gpu.c_str());
    if (best >= 0) {
        phys = devs[best];
        vkGetPhysicalDeviceProperties(phys, &props);
    }
    if (!phys) {
        NVK_ERR("no Vulkan GPU");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    vram_bytes = vram(phys);
    // Integrated GPUs report shared system memory as their device-local heap: the type decides for them. A discrete GPU
    // with under 3 GiB of VRAM is treated the same way.
    low_end = integrated || vram_bytes < (3ull << 30) || g_cfg.lowend_force;
    NVK_HOST("chosen GPU: %s (%s, %llu MiB device-local)%s", props.deviceName, integrated ? "integrated" : "discrete", (unsigned long long)(vram_bytes >> 20),
             low_end ? " -> low-end (auto preset = Performance)" : "");
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            qfamily = i;
            break;
        }

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> avail(en);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &en, avail.data());
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    auto opt = [&](const char* e, bool* flag) {
        if (HasExt(avail, e)) {
            dexts.push_back(e);
            if (flag) *flag = true;
        }
    };
    opt(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, &has_push_descriptor);
    opt(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, &has_custom_border);
    opt(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &has_depth_clip_control);
    opt(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, &has_external_memory_host);
    opt(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, &has_eds3);
    opt(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, &has_xfb);
    bool has_robust2 = false;
    if (g_cfg.robust) opt(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, &has_robust2);

    // Enable every feature the device supports (1.0-1.3 + the extensions used).
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfbf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    void** tail = &f13.pNext;
    if (has_custom_border) { *tail = &cbf; tail = &cbf.pNext; }
    if (has_depth_clip_control) { *tail = &dcc; tail = &dcc.pNext; }
    if (has_eds3) { *tail = &eds3; tail = &eds3.pNext; }
    if (has_xfb) { *tail = &xfbf; tail = &xfbf.pNext; }
    VkPhysicalDeviceRobustness2FeaturesEXT rb2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    if (has_robust2) { *tail = &rb2; tail = &rb2.pNext; }
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    if (!g_cfg.robust) f2.features.robustBufferAccess = VK_FALSE;  // unneeded cost
    has_precise_occl = f2.features.occlusionQueryPrecise;
    has_aniso = f2.features.samplerAnisotropy;
    NVK_INFO("robustness: buffer=%d image=%d robustness2=%d (buffer2=%d image2=%d nullDescriptor=%d)",
             (int)f2.features.robustBufferAccess, (int)f13.robustImageAccess, (int)has_robust2,
             (int)rb2.robustBufferAccess2, (int)rb2.robustImageAccess2, (int)rb2.nullDescriptor);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    r = vkCreateDevice(phys, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) {
        NVK_ERR("vkCreateDevice = %d", (int)r);
        return false;
    }
    vkGetDeviceQueue(dev, qfamily, 0, &queue);
    vkCmdPushDescriptorSetKHR_ = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (has_xfb) {
        vkCmdBindTransformFeedbackBuffersEXT_ = (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(dev, "vkCmdBindTransformFeedbackBuffersEXT");
        vkCmdBeginTransformFeedbackEXT_ = (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginTransformFeedbackEXT");
        vkCmdEndTransformFeedbackEXT_ = (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdEndTransformFeedbackEXT");
        vkCmdDrawIndirectByteCountEXT_ = (PFN_vkCmdDrawIndirectByteCountEXT)vkGetDeviceProcAddr(dev, "vkCmdDrawIndirectByteCountEXT");
        has_xfb = vkCmdBindTransformFeedbackBuffersEXT_ && vkCmdBeginTransformFeedbackEXT_ && vkCmdEndTransformFeedbackEXT_;
    }

    VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &st;
    VKCHECK(vkCreateSemaphore(dev, &sci, nullptr, &timeline));

    {
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = 2 * kFrames;
        VKCHECK(vkCreateQueryPool(dev, &qp, nullptr, &timestamps));
        ts_period_ns = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo oq{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        oq.queryType = VK_QUERY_TYPE_OCCLUSION;
        oq.queryCount = kOcclQueries;
        for (auto& f : frames) VKCHECK(vkCreateQueryPool(dev, &oq, nullptr, &f.occl));
    }
    for (auto& f : frames) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = qfamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VKCHECK(vkCreateCommandPool(dev, &pci, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VKCHECK(vkAllocateCommandBuffers(dev, &ai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(dev, &fci, nullptr, &f.fence));
        f.upload = CreateBuffer(64ull << 20,
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | 
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    (has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0),
                                true);
    }
    if (has_xfb)
        xfb_counters = CreateBuffer(kXfbSlots * 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
                                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
    NVK_INFO("Vulkan: %s (API %u.%u.%u), xfb=%d push_descriptor=%d custom_border=%d depth_clip_control=%d "
             "external_memory_host=%d eds3=%d validation=%d",
             props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), has_xfb, has_push_descriptor, has_custom_border,
             has_depth_clip_control, has_external_memory_host, has_eds3, g_cfg.validation);
    return true;
}

void VkCtx::Shutdown() {
    if (!dev) return;
    vkDeviceWaitIdle(dev);
    for (auto& f : frames) {
        for (auto smp : f.dead_samplers) vkDestroySampler(dev, smp, nullptr);
        for (auto v : f.dead_views) vkDestroyImageView(dev, v, nullptr);
    for (auto v : f.dead_bviews) vkDestroyBufferView(dev, v, nullptr);
    f.dead_bviews.clear();
        for (auto& [img, mem] : f.dead_images) {
            vkDestroyImage(dev, img, nullptr);
            FreeImage(mem);
        }
        for (auto& b : f.dead_buffers) DestroyBuffer(b);
        for (auto& [b, m] : f.dead_subbufs) {
            vkDestroyBuffer(dev, b, nullptr);
            FreeImage(m);
        }
        f.dead_subbufs.clear();
        f.dead_samplers.clear();
        f.dead_views.clear();
        f.dead_images.clear();
        f.dead_buffers.clear();
        DestroyBuffer(f.upload);
        if (f.readback.buf) DestroyBuffer(f.readback);
        vkDestroyFence(dev, f.fence, nullptr);
        vkDestroyCommandPool(dev, f.pool, nullptr);
    }
    for (auto& [k, g] : gpu_bufs) {
        vkDestroyBuffer(dev, g.b.buf, nullptr);
        FreeImage(g.sub);
    }
    gpu_bufs.clear();
    if (xfb_counters.buf) DestroyBuffer(xfb_counters);
    vkDestroySemaphore(dev, timeline, nullptr);
    if (timestamps) vkDestroyQueryPool(dev, timestamps, nullptr);
    for (auto& f : frames)
        if (f.occl) vkDestroyQueryPool(dev, f.occl, nullptr);
    for (auto& b : blocks_) vkFreeMemory(dev, b.mem, nullptr);
    blocks_.clear();
    vkDestroyDevice(dev, nullptr);
    dev = VK_NULL_HANDLE;
}

uint32_t VkCtx::MemType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want &&
            !(memprops.memoryTypes[i].propertyFlags & avoid))
            return i;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

VkDeviceMemory VkCtx::Alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(req.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX) return VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    VKCHECK(vkAllocateMemory(dev, &ai, nullptr, &m));
    return m;
}

Buffer VkCtx::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VKCHECK(vkCreateBuffer(dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    b.mem = Alloc(req, host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    if (host_visible) VKCHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, (void**)&b.map));
    return b;
}

MemSub VkCtx::AllocImage(const VkMemoryRequirements& req) { return AllocSub(req, 0); }

// Small, numerous DEVICE_LOCAL buffers (transform feedback: >1,500 in battle, previously one
// vkAllocateMemory each with bursts of ~180 per second). Own blocks (kind 1), separate from the
// image blocks so linear and optimal resources don't mix (bufferImageGranularity).
MemSub VkCtx::AllocBufferMem(const VkMemoryRequirements& req) { return AllocSub(req, 1); }

MemSub VkCtx::AllocSub(const VkMemoryRequirements& req, int kind) {
    MemSub s;
    const uint32_t type = MemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return s;
    const VkDeviceSize kBlock = kind ? (32ull << 20) : (256ull << 20), kDedicated = kind ? (8ull << 20) : (64ull << 20);
    if (req.size > kDedicated) {
        s.mem = Alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        s.size = req.size;
        return s;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 256);
    std::lock_guard<std::mutex> lk(blocks_mu_);
    auto try_block = [&](int bi) -> bool {
        MemBlock& b = blocks_[bi];
        if (b.type != type || b.kind != kind) return false;
        for (size_t i = 0; i < b.free.size(); ++i) {
            auto [fo, fs] = b.free[i];
            const VkDeviceSize o = (fo + align - 1) & ~(align - 1);
            if (o + req.size > fo + fs) continue;
            // Split the hole: [fo, o) before and [o+size, fo+fs) after.
            const VkDeviceSize tail_o = o + req.size, tail_s = fo + fs - tail_o;
            b.free.erase(b.free.begin() + i);
            if (tail_s) b.free.insert(b.free.begin() + i, {tail_o, tail_s});
            if (o > fo) b.free.insert(b.free.begin() + i, {fo, o - fo});
            s.mem = b.mem;
            s.off = o;
            s.size = req.size;
            s.block = bi;
            return true;
        }
        return false;
    };
    for (int bi = 0; bi < (int)blocks_.size(); ++bi)
        if (try_block(bi)) return s;
    MemBlock nb;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = kBlock;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev, &ai, nullptr, &nb.mem) != VK_SUCCESS) {
        s.mem = Alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);  // no block: dedicated
        s.size = req.size;
        return s;
    }
    nb.type = type;
    nb.kind = kind;
    nb.size = kBlock;
    nb.free.push_back({0, kBlock});
    blocks_.push_back(std::move(nb));
    NVK_INFO("%s memory: block %zu of %llu MiB", kind ? "GPU buffer" : "image", blocks_.size(),
             (unsigned long long)(kBlock >> 20));
    try_block((int)blocks_.size() - 1);
    return s;
}

void VkCtx::FreeImage(const MemSub& s) {
    if (!s.mem) return;
    if (s.block < 0) {
        vkFreeMemory(dev, s.mem, nullptr);
        return;
    }
    std::lock_guard<std::mutex> lk(blocks_mu_);
    MemBlock& b = blocks_[s.block];
    auto it = std::lower_bound(b.free.begin(), b.free.end(), std::make_pair(s.off, VkDeviceSize(0)));
    it = b.free.insert(it, {s.off, s.size});
    // Merge with the next and previous holes if contiguous.
    if (it + 1 != b.free.end() && it->first + it->second == (it + 1)->first) {
        it->second += (it + 1)->second;
        b.free.erase(it + 1);
    }
    if (it != b.free.begin() && (it - 1)->first + (it - 1)->second == it->first) {
        (it - 1)->second += it->second;
        b.free.erase(it);
    }
}

void VkCtx::DestroyBuffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    b = Buffer{};
}

FrameSlot& VkCtx::Begin() {
    cur = (cur + 1) % kFrames;
    FrameSlot& f = frames[cur];
    const auto tw = std::chrono::steady_clock::now();
    VKCHECK(vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX));
    fence_wait_ns.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tw).count(),
        std::memory_order_relaxed);
    VKCHECK(vkResetFences(dev, 1, &f.fence));
    if (f.timed) {
        uint64_t t[2] = {};
        if (vkGetQueryPoolResults(dev, timestamps, 2 * cur, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            t[1] > t[0]) {
            gpu_ns.fetch_add((uint64_t)((t[1] - t[0]) * ts_period_ns), std::memory_order_relaxed);
            gpu_cmdbufs.fetch_add(1, std::memory_order_relaxed);
        }
        f.timed = false;
    }
    if (on_slot_reuse) on_slot_reuse(f);
    f.arena_head_at_begin = arena_head;
    f.dev_head_at_begin = dev_head;
    for (auto s : f.dead_samplers) vkDestroySampler(dev, s, nullptr);
    for (auto v : f.dead_views) vkDestroyImageView(dev, v, nullptr);
    for (auto v : f.dead_bviews) vkDestroyBufferView(dev, v, nullptr);
    f.dead_bviews.clear();
    for (auto& [img, mem] : f.dead_images) {
        vkDestroyImage(dev, img, nullptr);
        FreeImage(mem);
    }
    for (auto& b : f.dead_buffers) DestroyBuffer(b);
    for (auto& [b, m] : f.dead_subbufs) {
        vkDestroyBuffer(dev, b, nullptr);
        FreeImage(m);
    }
    f.dead_samplers.clear();
    f.dead_views.clear();
    f.dead_images.clear();
    f.dead_buffers.clear();
    f.dead_subbufs.clear();
    VKCHECK(vkResetCommandPool(dev, f.pool, 0));
    if (f.upload_grow && f.upload.size < (256ull << 20)) {
        // Large uploads (sprint 5 review): instead of a one-shot buffer for each upload that doesn't fit,
        // this slot's ring grows (its fence was already waited on: nobody is using it).
        const VkDeviceSize ns = f.upload.size * 2;
        DestroyBuffer(f.upload);
        f.upload = CreateBuffer(ns,
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | 
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    (has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0),
                                true);
        NVK_INFO("upload ring of slot %d: %llu MiB", cur, (unsigned long long)(ns >> 20));
    }
    f.upload_grow = false;
    f.upload_used = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(f.cmd, &bi));
    ++cmd_epoch;
    f.cur_cmd = f.cmd;
    f.extra_used = 0;
    vkCmdResetQueryPool(f.cmd, timestamps, 2 * cur, 2);
    ResolveOcclusion(f);
    if (f.occl) vkCmdResetQueryPool(f.cmd, f.occl, 0, kOcclQueries);
    vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 2 * cur);
    return f;
}

VkCommandBuffer VkCtx::SubmitChunk() {
    FrameSlot& f = frames[cur];
    VKCHECK(vkEndCommandBuffer(f.cur_cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cur_cmd;
    VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    if (f.extra_used == f.extra.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer nb = VK_NULL_HANDLE;
        VKCHECK(vkAllocateCommandBuffers(dev, &ai, &nb));
        f.extra.push_back(nb);
    }
    f.cur_cmd = f.extra[f.extra_used++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(f.cur_cmd, &bi));
    ++cmd_epoch;
    chunks.fetch_add(1, std::memory_order_relaxed);
    return f.cur_cmd;
}

uint64_t VkCtx::Submit(VkSemaphore wait, VkSemaphore signal) {
    FrameSlot& f = frames[cur];
    if (!f.cur_cmd) f.cur_cmd = f.cmd;
    vkCmdWriteTimestamp(f.cur_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 2 * cur + 1);
    f.timed = true;
    VKCHECK(vkEndCommandBuffer(f.cur_cmd));
    const uint64_t value = timeline_next.fetch_add(1);
    f.timeline_value = value;
    VkSemaphore sigs[2] = {timeline, signal};
    uint64_t sigv[2] = {value, 0};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.signalSemaphoreValueCount = signal ? 2 : 1;
    ts.pSignalSemaphoreValues = sigv;
    uint64_t waitv = 0;
    ts.waitSemaphoreValueCount = wait ? 1 : 0;
    ts.pWaitSemaphoreValues = &waitv;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cur_cmd;
    si.signalSemaphoreCount = signal ? 2 : 1;
    si.pSignalSemaphores = sigs;
    si.waitSemaphoreCount = wait ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &ws;
    VKCHECK(vkQueueSubmit(queue, 1, &si, f.fence));
    if (g_cfg.sync_check) {
        // Diagnostic: wait for the GPU after every submit to see which submit loses the device.
        static std::atomic<uint64_t> n{0};
        const uint64_t i = n.fetch_add(1);
        const VkResult w = vkWaitForFences(dev, 1, &f.fence, VK_TRUE, 5'000'000'000ull);
        if (w != VK_SUCCESS) NVK_ERR("sync_check: Vulkan submit %llu = %d (device lost or hung)", (unsigned long long)i, (int)w);
    }
    return value;
}

VkDeviceSize VkCtx::UploadAlloc(VkDeviceSize size, VkDeviceSize align, uint8_t** ptr) {
    FrameSlot& f = frames[cur];
    VkDeviceSize off = (f.upload_used + align - 1) & ~(align - 1);
    if (off + size > f.upload.size) {
        // ring full: extra one-shot buffer (and the ring will grow next time)
        if (size <= (16ull << 20)) f.upload_grow = true;
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 5) NVK_WARN("upload ring full (%llu + %llu)", (unsigned long long)off,
                                              (unsigned long long)size);
        *ptr = nullptr;
        return VK_WHOLE_SIZE;
    }
    f.upload_used = off + size;
    *ptr = f.upload.map + off;
    return off;
}

namespace {
bool MakeGpuBuf(VkCtx& vk, VkDeviceSize size, VkCtx::GpuBuf* g) {
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    g->b = Buffer{};
    g->b.size = size;
    VKCHECK(vkCreateBuffer(vk.dev, &bci, nullptr, &g->b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, g->b.buf, &req);
    g->sub = vk.AllocBufferMem(req);
    if (!g->sub.mem) {
        vkDestroyBuffer(vk.dev, g->b.buf, nullptr);
        g->b.buf = VK_NULL_HANDLE;
        return false;
    }
    VKCHECK(vkBindBufferMemory(vk.dev, g->b.buf, g->sub.mem, g->sub.off));
    g->size = size;
    return true;
}
}  // namespace

void VkCtx::KillGpuBuf(GpuBuf& g) {
    ++flag_hist[g.flags];
    if (g.b.buf) frames[cur].dead_subbufs.push_back({g.b.buf, g.sub});
    g.b = Buffer{};
    g.sub = MemSub{};
}

VkCtx::GpuBuf* VkCtx::GpuBuffer(uint64_t gpu, uint64_t size, bool* fresh, std::vector<Moved>* moved) {
    // (Version from b06fda67, the one validated with Q4/TRIANGLES/RAIN. The "no overlaps" variant (6473531a) broke the Q4 mesh.)
    const uint64_t now = g_stats.frames.load(std::memory_order_relaxed);
    if (fresh) *fresh = false;
    // Start aligned to 256 (sprint 6): XFB buffers start at 4 B addresses; an SSBO bound inside one of them
    // (or inside a union starting there) ended up with an offset invalid for Vulkan (shifted writes).
    {
        const uint64_t a = gpu & ~255ull;
        size += gpu - a;
        gpu = a;
    }
    auto it = gpu_bufs.find(gpu);
    if (it != gpu_bufs.end()) {
        GpuBuf& g = it->second;
        g.last_use = now;
        if (g.size >= size) return &g;
        if (moved && g.b.buf) moved->push_back({g.b.buf, g.gpu, g.size, std::move(g.shadow)});
        if (fresh) *fresh = true;
        KillGpuBuf(g);
        if (!MakeGpuBuf(*this, size, &g)) {
            gpu_bufs.erase(it);
            return nullptr;
        }
        return &g;
    }
    // Those starting inside the new range and fitting in it: address reused by the guest.
    for (auto o = gpu_bufs.lower_bound(gpu); o != gpu_bufs.end() && o->first < gpu + size;) {
        if (o->second.gpu + o->second.size <= gpu + size) {
            if (moved && o->second.b.buf) moved->push_back({o->second.b.buf, o->second.gpu, o->second.size, std::move(o->second.shadow)});
            KillGpuBuf(o->second);
            o = gpu_bufs.erase(o);
            ++gpubuf_replaced;
        } else {
            ++o;
        }
    }
    GpuBuf g;
    g.gpu = gpu;
    g.last_use = now;
    if (!MakeGpuBuf(*this, size, &g)) return nullptr;
    if (fresh) *fresh = true;
    ++gpubuf_created;
    if (g_cfg.log_gpubufs) NVK_INFO("GPU buffer for %llx (%llu KiB)", (unsigned long long)gpu, (unsigned long long)(size >> 10));
    return &gpu_bufs.emplace(gpu, g).first->second;
}

void VkCtx::DropGpuBuf(uint64_t gpu) {
    auto it = gpu_bufs.find(gpu);
    if (it == gpu_bufs.end()) return;
    KillGpuBuf(it->second);
    gpu_bufs.erase(it);
    ++gpubuf_stale;
}

void VkCtx::EvictGpuBuffers() {
    const uint64_t now = g_stats.frames.load(std::memory_order_relaxed), idle = g_cfg.xfb_idle_frames;
    if (!idle || now < idle) return;
    for (auto it = gpu_bufs.begin(); it != gpu_bufs.end();) {
        if (it->second.last_use + idle < now) {
            KillGpuBuf(it->second);
            it = gpu_bufs.erase(it);
            ++gpubuf_evicted;
        } else {
            ++it;
        }
    }
}

const VkCtx::GpuBuf* VkCtx::FindGpuBuffer(uint64_t gpu, uint64_t size) const {
    // The one starting at gpu or before that contains it (a few earlier ones are checked in case of overlaps).
    auto it = gpu_bufs.upper_bound(gpu);
    for (int n = 0; n < 8 && it != gpu_bufs.begin(); ++n) {
        --it;
        const GpuBuf& g = it->second;
        if (gpu + size <= g.gpu + g.size) {
            g.last_use = g_stats.frames.load(std::memory_order_relaxed);
            return &g;
        }
    }
    return nullptr;
}

// Slot results (its fence was already waited on): running sample total at each report, like the Switch counter
// (ResetCounter zeroes it). The game subtracts two consecutive reports; with the previous frame's value for each
// address, the difference gives that test's samples one frame late (the console also reads with latency).
void VkCtx::ResolveOcclusion(FrameSlot& f) {
    if (!f.occl || (f.occl_events.empty() && !f.occl_used)) return;
    std::vector<uint64_t> r(f.occl_used);
    if (f.occl_used &&
        vkGetQueryPoolResults(dev, f.occl, 0, f.occl_used, r.size() * 8, r.data(), 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        std::fill(r.begin(), r.end(), 0);
    uint32_t cur_seg = 0;
    for (const FrameSlot::OcclEvent& e : f.occl_events) {
        for (; cur_seg < e.seg && cur_seg < r.size(); ++cur_seg) occl_total += r[cur_seg];
        if (!e.addr) {
            occl_total = 0;
        } else {
            occl_values[e.addr] = occl_total;
            ++occl_resolved;
        }
    }
    for (; cur_seg < r.size(); ++cur_seg) occl_total += r[cur_seg];
    f.occl_events.clear();
    f.occl_used = 0;
}

uint64_t VkCtx::CompletedValue() {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev, timeline, &v);
    return v;
}

bool VkCtx::WaitValue(uint64_t v, uint64_t timeout_ns) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &v;
    return vkWaitSemaphores(dev, &wi, timeout_ns) == VK_SUCCESS;
}

// ---------------------------------------------------------------- Presenter
namespace {
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;  // the game closes it, not the user
    if (m == WM_SIZE) {
        auto* p = reinterpret_cast<std::atomic<bool>*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (p) p->store(true);
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

bool Presenter::StartOnWindow(HWND hwnd) {
    external_ = true;
    hwnd_ = hwnd;
    if (Start(0, 0)) return true;
    // No swapchain on the game's window (someone else using it?): release it and fall back to our own.
    if (surface_) vkDestroySurfaceKHR(g_vk.instance, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    for (int i = 0; i < VkCtx::kFrames; ++i) {
        if (acquire_[i]) vkDestroySemaphore(g_vk.dev, acquire_[i], nullptr);
        if (render_[i]) vkDestroySemaphore(g_vk.dev, render_[i], nullptr);
        acquire_[i] = render_[i] = VK_NULL_HANDLE;
    }
    external_ = false;
    hwnd_ = nullptr;
    return false;
}

// FPS-1 (laptops): which adapter drives the monitor the window is on. On a hybrid laptop without a MUX the display is
// wired to the iGPU: frames rendered on the dGPU are copied across adapters by the driver on every present.
static void LogPresentPath(HWND hwnd) {
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    HMODULE dx = LoadLibraryW(L"dxgi.dll");
    using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dx ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(dx, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory1* fac = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&fac))) || !fac) {
        NVK_HOST("present path: unknown (no DXGI)");
        return;
    }
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    std::string disp;
    LUID disp_luid{};
    bool found = false;
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; !found && fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad_desc{};
        ad->GetDesc1(&ad_desc);
        IDXGIOutput* out = nullptr;
        for (UINT o = 0; !found && ad->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (od.Monitor == mon) {
                found = true;
                disp_luid = ad_desc.AdapterLuid;
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, ad_desc.Description, -1, name, sizeof name - 1, nullptr, nullptr);
                disp = name;
            }
            out->Release();
        }
        ad->Release();
    }
    fac->Release();
    if (!found) {
        NVK_HOST("present path: display adapter not found (window on a monitor DXGI does not list)");
        return;
    }
    const bool same = idp.deviceLUIDValid && std::memcmp(&disp_luid, idp.deviceLUID, sizeof disp_luid) == 0;
    if (same)
        NVK_HOST("present path: direct (the monitor is driven by the rendering GPU, %s)", g_vk.props.deviceName);
    else
        NVK_HOST("present path: CROSS-ADAPTER: rendering on %s, monitor driven by %s; the driver copies every frame between "
                 "adapters (hybrid laptop without MUX: a dGPU-direct display mode, if the laptop has one, avoids it)",
                 g_vk.props.deviceName, disp.c_str());
}

bool Presenter::Start(int w, int h) {
    if (!external_) {
        thread_ = std::thread([this, w, h] { ThreadMain(w, h); });
        for (int i = 0; i < 200 && !hwnd_.load(); ++i) Sleep(10);
    }
    if (!hwnd_.load()) return false;
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = hwnd_.load();
    VKCHECK(vkCreateWin32SurfaceKHR(g_vk.instance, &sci, nullptr, &surface_));
    LogPresentPath(hwnd_.load());
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, g_vk.qfamily, surface_, &ok);
    if (!ok) {
        NVK_ERR("the queue cannot present to the window");
        return false;
    }
    VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < VkCtx::kFrames; ++i) {
        VKCHECK(vkCreateSemaphore(g_vk.dev, &s, nullptr, &acquire_[i]));
        VKCHECK(vkCreateSemaphore(g_vk.dev, &s, nullptr, &render_[i]));
    }
    return Recreate();
}

void Presenter::ThreadMain(int w, int h) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"nvnvk";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowExW(0, L"nvnvk", g_cfg.exclusive ? L"PLA - nvnvk (own Vulkan)" : L"PLA - nvnvk (shadow mode)",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, r.right - r.left, r.bottom - r.top, nullptr,
                              nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)&resized_);
    hwnd_ = hw;
    MSG msg;
    while (!quit_.load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    DestroyWindow(hw);
}

bool Presenter::Recreate() {
    vkDeviceWaitIdle(g_vk.dev);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, surface_, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, surface_, &n, fmts.data());
    VkSurfaceFormatKHR sf = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) sf = f;
    format_ = sf.format;
    uint32_t pmn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, surface_, &pmn, nullptr);
    std::vector<VkPresentModeKHR> pms(pmn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, surface_, &pmn, pms.data());
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_cfg.vsync) {
        for (auto m : pms)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) pm = m;
        for (auto m : pms)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) pm = m;
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pm;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult cr = vkCreateSwapchainKHR(g_vk.dev, &ci, nullptr, &sc);
    if (cr != VK_SUCCESS) {
        NVK_WARN("vkCreateSwapchainKHR = %d", (int)cr);
        return false;
    }
    if (swapchain_) vkDestroySwapchainKHR(g_vk.dev, swapchain_, nullptr);
    swapchain_ = sc;
    extent_ = caps.currentExtent;
    vkGetSwapchainImagesKHR(g_vk.dev, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(g_vk.dev, swapchain_, &n, images_.data());
    NVK_INFO("swapchain %ux%u, %u images, mode %d", extent_.width, extent_.height, n, (int)pm);
    return true;
}

namespace {
#include "shaders/final.comp.inc"
#include "shaders/fxaa.comp.inc"
#include "shaders/scale.comp.inc"
#include "shaders/ovblur.comp.inc"
#include "shaders/ovcompose.comp.inc"
#include "shaders/ovdown.comp.inc"
}

void Presenter::FreePostImage(PostImage& pi) {
    if (pi.view) vkDestroyImageView(g_vk.dev, pi.view, nullptr);
    if (pi.img) vkDestroyImage(g_vk.dev, pi.img, nullptr);
    if (pi.mem.mem) g_vk.FreeImage(pi.mem);
    pi = PostImage{};
}

void Presenter::DestroyPost() {
    for (PostImage& pi : post_) FreePostImage(pi);
    FreePostImage(scene_);
    post_last_ = -1;
    scene_valid_ = false;
}

void Presenter::SnapScene(VkCommandBuffer cmd, VkImage img, VkFormat fmt, int w, int h) {
    if (post_failed_ || w <= 0 || h <= 0) return;
    // The game's composite is sRGB and the copy to the window (UNORM) samples it decoded: read with its sRGB format, the
    // copy yields the same values as the final frame (measured with dump_rts in the village: mean error 0.3/255).
    if (!scene_.img || scene_.w != w || scene_.h != h || scene_fmt_ != fmt) {
        if (scene_.img) {
            vkDeviceWaitIdle(g_vk.dev);
            FreePostImage(scene_);
        }
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = fmt;
        ic.extent = {(uint32_t)w, (uint32_t)h, 1};
        ic.mipLevels = ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(g_vk.dev, &ic, nullptr, &scene_.img) != VK_SUCCESS) {
            scene_.img = VK_NULL_HANDLE;
            return;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_vk.dev, scene_.img, &req);
        scene_.mem = g_vk.AllocImage(req);
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = scene_.img;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = fmt;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!scene_.mem.mem || vkBindImageMemory(g_vk.dev, scene_.img, scene_.mem.mem, scene_.mem.off) != VK_SUCCESS ||
            vkCreateImageView(g_vk.dev, &vc, nullptr, &scene_.view) != VK_SUCCESS) {
            FreePostImage(scene_);
            return;
        }
        scene_.w = w;
        scene_.h = h;
        scene_.fresh = true;
        scene_fmt_ = fmt;
        NVK_INFO("aa_hud: scene copy %dx%d (format %d)", w, h, (int)fmt);
    }
    VkImageMemoryBarrier ib[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    ib[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ib[0].oldLayout = ib[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib[0].image = img;
    ib[1].srcAccessMask = scene_.fresh ? 0 : VK_ACCESS_SHADER_READ_BIT;
    ib[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib[1].oldLayout = scene_.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    ib[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib[1].image = scene_.img;
    for (auto& b : ib) {
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    scene_.fresh = false;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, ib);
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, scene_.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    // The UI draws into the window texture again (write after the copy's read).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

bool Presenter::EnsurePostPipes() {
    if (post_failed_ || !g_vk.vkCmdPushDescriptorSetKHR_) return false;
    if (post_pipe_[0]) return true;
    VkDescriptorSetLayoutBinding bd[3] = {};
    bd[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bd[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bd[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 3;
    dl.pBindings = bd;
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &post_dsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerCreateInfo lc = sc;
    lc.magFilter = lc.minFilter = VK_FILTER_LINEAR;
    if (vkCreateDescriptorSetLayout(g_vk.dev, &dl, nullptr, &post_dsl_) != VK_SUCCESS ||
        vkCreatePipelineLayout(g_vk.dev, &pl, nullptr, &post_pl_) != VK_SUCCESS ||
        vkCreateSampler(g_vk.dev, &sc, nullptr, &post_smp_) != VK_SUCCESS ||
        vkCreateSampler(g_vk.dev, &lc, nullptr, &post_lin_) != VK_SUCCESS) {
        post_failed_ = true;
        NVK_WARN("final pass: could not create the layout");
        return false;
    }
    const struct {
        const uint32_t* code;
        size_t size;
    } spv[kPostCount] = {{kFxaaSpv, sizeof(kFxaaSpv)},   {kFinalSpv, sizeof(kFinalSpv)},   {kScaleSpv, sizeof(kScaleSpv)},
                         {kOvDownSpv, sizeof(kOvDownSpv)}, {kOvBlurSpv, sizeof(kOvBlurSpv)}, {kOvComposeSpv, sizeof(kOvComposeSpv)}};
    for (int k = 0; k < kPostCount; ++k) {
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = spv[k].size;
        sm.pCode = spv[k].code;
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(g_vk.dev, &sm, nullptr, &mod) != VK_SUCCESS) {
            post_failed_ = true;
            return false;
        }
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = mod;
        cp.stage.pName = "main";
        cp.layout = post_pl_;
        const VkResult r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &cp, nullptr, &post_pipe_[k]);
        vkDestroyShaderModule(g_vk.dev, mod, nullptr);
        if (r != VK_SUCCESS) {
            post_failed_ = true;
            NVK_WARN("final pass: vkCreateComputePipelines(%d) = %d", k, (int)r);
            return false;
        }
    }
    static const char* kAa[] = {"off", "FXAA", "SMAA (FXAA for now)"};
    NVK_INFO("final pass active: antialiasing %s, sharpen %.2f, scaling %s, brightness %.2f, contrast %.2f, saturation %.2f, FPS %s",
             kAa[g_cfg.aa], g_cfg.sharpen, g_cfg.upscale_sharp ? "sharp" : "bilinear", g_cfg.brightness, g_cfg.contrast,
             g_cfg.saturation, g_cfg.show_fps ? "on" : "off");
    return true;
}

bool Presenter::EnsurePostImage(int slot, int w, int h, VkFormat fmt) {
    PostImage& pi = post_[slot];
    if (pi.img && pi.w == w && pi.h == h && post_fmt_[slot] == fmt) return true;
    if (pi.img) {
        vkDeviceWaitIdle(g_vk.dev);  // only on resize
        if (pi.view) vkDestroyImageView(g_vk.dev, pi.view, nullptr);
        vkDestroyImage(g_vk.dev, pi.img, nullptr);
        if (pi.mem.mem) g_vk.FreeImage(pi.mem);
        pi = PostImage{};
        if (post_last_ == slot) post_last_ = -1;
    }
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = fmt;
    ic.extent = {(uint32_t)w, (uint32_t)h, 1};
    ic.mipLevels = ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM) ic.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;  // menu: ImGui target
    post_fmt_[slot] = fmt;
    if (vkCreateImage(g_vk.dev, &ic, nullptr, &pi.img) != VK_SUCCESS) {
        pi.img = VK_NULL_HANDLE;
        post_failed_ = true;
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, pi.img, &req);
    pi.mem = g_vk.AllocImage(req);
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = pi.img;
    vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vc.format = ic.format;
    vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!pi.mem.mem || vkBindImageMemory(g_vk.dev, pi.img, pi.mem.mem, pi.mem.off) != VK_SUCCESS ||
        vkCreateImageView(g_vk.dev, &vc, nullptr, &pi.view) != VK_SUCCESS) {
        DestroyPost();
        post_failed_ = true;
        return false;
    }
    pi.w = w;
    pi.h = h;
    pi.fresh = true;
    return true;
}

void Presenter::RunPost(VkCommandBuffer cmd, int kind, VkImageView src, bool linear, int dst, const void* pc, uint32_t pc_size,
                        VkImageView src2, bool linear2) {
    PostImage& d = post_[dst];
    // The destination image may have been read in the previous frame (blit) or by the previous pass; and the source was
    // just written by the previous pass (or the game).
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = d.fresh ? 0 : (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ib.oldLayout = d.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = d.img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    d.fresh = false;
    VkMemoryBarrier rd{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    rd.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    rd.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &rd, 0, nullptr, 1, &ib);
    VkDescriptorImageInfo si{linear ? post_lin_ : post_smp_, src, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo di{VK_NULL_HANDLE, d.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo si2{linear2 ? post_lin_ : post_smp_, src2 ? src2 : src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &si;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[1].pImageInfo = &di;
    wr[2].dstBinding = 2;
    wr[2].descriptorCount = 1;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[2].pImageInfo = &si2;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, post_pipe_[kind]);
    g_vk.vkCmdPushDescriptorSetKHR_(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, post_pl_, 0, 3, wr);
    vkCmdPushConstants(cmd, post_pl_, VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size, pc);
    vkCmdDispatch(cmd, (uint32_t)(d.w + 7) / 8, (uint32_t)(d.h + 7) / 8, 1);
}

static float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else v = std::ldexp((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}

bool Presenter::DumpPost(const std::string& path, bool srgb) {
    if (post_last_ < 0 || !post_[post_last_].img) return false;
    const uint32_t w = (uint32_t)post_[post_last_].w, h = (uint32_t)post_[post_last_].h;
    Buffer b = g_vk.CreateBuffer((uint64_t)w * h * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
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
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, post_[post_last_].img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.queue);
    bool ok = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(w * 3);
        const uint16_t* px = reinterpret_cast<const uint16_t*>(b.map);
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c) {
                    float v = std::min(1.0f, std::max(0.0f, HalfToFloat(px[((uint64_t)y * w + x) * 4 + c])));
                    if (srgb) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                    row[x * 3 + c] = (uint8_t)(v * 255.0f + 0.5f);
                }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        ok = true;
    }
    vkDestroyCommandPool(g_vk.dev, pool, nullptr);
    g_vk.DestroyBuffer(b);
    return ok;
}

void Presenter::Present(VkCommandBuffer cmd, VkImage img, int w, int h, VkFormat fmt) {
    pending_ = false;
    if (!swapchain_) return;
    if (external_ && (++presents_ % 30) == 0) {
        // Foreign window: we don't get WM_SIZE; check the size every so often.
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, surface_, &caps) == VK_SUCCESS &&
            (caps.currentExtent.width != extent_.width || caps.currentExtent.height != extent_.height))
            resized_ = true;
    }
    if (resized_.exchange(false)) Recreate();
    sem_idx_ = (sem_idx_ + 1) % VkCtx::kFrames;
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, swapchain_, 0, acquire_[sem_idx_], VK_NULL_HANDLE, &image_idx_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            Recreate();
            return;
        }
    } else if (r != VK_SUCCESS) {
        return;  // no free image (timeout 0): skip this present
    }
    VkImage dst = images_[image_idx_];
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = dst;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    // The presented image was written by an earlier submit (attachment, copy or compute): without this
    // dependency the blit may read it half-written (barrier review, sprint 5).
    VkMemoryBarrier src_mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    src_mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    src_mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &src_mb, 0,
                         nullptr, 1, &b);
    // Final pass (graphics options): antialiasing, color/sharpening/FPS and scaling, chained through RGBA16F images.
    {
        LARGE_INTEGER now, fq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&fq);
        if (fps_t_.QuadPart) {
            const double ms = 1000.0 * (double)(now.QuadPart - fps_t_.QuadPart) / (double)fq.QuadPart;
            if (ms < 1000) fps_ema_ = fps_ema_ > 0 ? fps_ema_ * 0.95 + ms * 0.05 : ms;
        }
        fps_t_ = now;
    }
    // Live internal resolution (render_res): the game draws a smaller screen in the top left of a presented texture
    // reserved at a larger size. That area (and the aa_hud scene copy) is cut into post_[6] / post_[7] first, and the
    // rest of the chain works on it as if it were the whole texture.
    VkImageView scene_view = scene_.view;
    bool scene_ok = scene_valid_ && scene_.img && scene_.w == w && scene_.h == h;
    {
        const uint32_t crop = g_present_crop.load(std::memory_order_relaxed);
        const int cw = (int)(crop >> 16), ch = (int)(crop & 0xFFFF);
        if (img && fmt != VK_FORMAT_UNDEFINED && cw > 0 && ch > 0 && cw <= w && ch <= h && (cw < w || ch < h) &&
            EnsurePostImage(6, cw, ch) && (!scene_ok || EnsurePostImage(7, cw, ch))) {
            VkImageMemoryBarrier tb[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
            const uint32_t nb = scene_ok ? 2u : 1u;
            for (uint32_t i = 0; i < nb; ++i) {
                PostImage& d = post_[6 + i];
                tb[i].srcAccessMask = d.fresh ? 0 : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
                tb[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                tb[i].oldLayout = d.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
                tb[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                tb[i].image = d.img;
                tb[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                tb[i].srcQueueFamilyIndex = tb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                d.fresh = false;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, nb, tb);
            VkImageBlit cb{};
            cb.srcSubresource = cb.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cb.srcOffsets[1] = cb.dstOffsets[1] = {cw, ch, 1};
            vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, post_[6].img, VK_IMAGE_LAYOUT_GENERAL, 1, &cb, VK_FILTER_NEAREST);
            if (scene_ok) {
                vkCmdBlitImage(cmd, scene_.img, VK_IMAGE_LAYOUT_GENERAL, post_[7].img, VK_IMAGE_LAYOUT_GENERAL, 1, &cb,
                               VK_FILTER_NEAREST);
                scene_view = post_[7].view;
            }
            VkMemoryBarrier cm{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            cm.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            cm.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &cm, 0, nullptr, 0, nullptr);
            img = post_[6].img;
            fmt = post_fmt_[6];
            w = cw;
            h = ch;
        }
    }
    const bool want_aa = g_cfg.aa != 0;
    const bool want_final = g_cfg.sharpen > 0 || g_cfg.brightness != 0 || g_cfg.contrast != 1 || g_cfg.saturation != 1 || g_cfg.show_fps;
    const bool other_size = w != (int)extent_.width || h != (int)extent_.height;
    const bool want_scale = other_size && (g_cfg.upscale_sharp || w > (int)extent_.width || h > (int)extent_.height);
    post_last_ = -1;
    VkImageView fin_view = VK_NULL_HANDLE;  // view of the image to be presented (if it already exists)
    if (img && fmt != VK_FORMAT_UNDEFINED && w > 0 && h > 0 && (want_aa || want_final || want_scale) && EnsurePostPipes() &&
        (!want_aa || EnsurePostImage(0, w, h)) && (!want_final || EnsurePostImage(want_aa ? 1 : 0, w, h)) &&
        (!want_scale || EnsurePostImage(2, (int)extent_.width, (int)extent_.height))) {
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = img;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = fmt;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView sv = VK_NULL_HANDLE;
        if (vkCreateImageView(g_vk.dev, &vc, nullptr, &sv) == VK_SUCCESS) {
            g_vk.frames[g_vk.cur].dead_views.push_back(sv);
            VkImageView cur = sv;
            int slot = -1;
            if (want_aa) {
                // With the scene copied before the UI: the scene is smoothed and pixels the UI changed come straight
                // from the final frame (sharp text and icons).
                const bool hud = scene_ok;
                const struct {
                    int32_t w, h;
                    float subpix, threshold;
                    int32_t hud;
                } pc{w, h, 0.5f, 0.125f, hud ? (g_cfg.aa_debug ? 2 : 1) : 0};  // subpixel 0.5: less blurry than the classic 0.75
                RunPost(cmd, kPostFxaa, hud ? scene_view : cur, true, 0, &pc, sizeof pc, cur);
                slot = 0;
                cur = post_[0].view;
            }
            if (want_final) {
                const int dsti = slot == 0 ? 1 : 0;
                const struct {
                    int32_t w, h;
                    float sharp, bright, contrast, sat;
                    int32_t fps, pad;
                } pc{w, h, g_cfg.sharpen, g_cfg.brightness, g_cfg.contrast, g_cfg.saturation,
                     g_cfg.show_fps ? (int32_t)(fps_ema_ > 0 ? std::min(999.0, 1000.0 / fps_ema_ + 0.5) : 0) : -1, 0};
                RunPost(cmd, kPostFinal, cur, false, dsti, &pc, sizeof pc);
                slot = dsti;
                cur = post_[dsti].view;
            }
            if (want_scale) {
                const int32_t pc[4] = {w, h, (int32_t)extent_.width, (int32_t)extent_.height};
                RunPost(cmd, kPostScale, cur, true, 2, pc, sizeof pc);
                slot = 2;
            }
            VkMemoryBarrier cw{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            cw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            cw.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cw, 0, nullptr, 0,
                                 nullptr);
            img = post_[slot].img;
            w = post_[slot].w;
            h = post_[slot].h;
            fmt = post_fmt_[slot];
            fin_view = post_[slot].view;
            post_last_ = slot;
        }
    }
    scene_valid_ = false;
    // In-game overlay menu: dimmed/blurred background (compute) + UI (ImGui) into post_[5], window-sized; that is the
    // image copied to the swapchain.
    ov_shown_ = false;
    if (img && fmt != VK_FORMAT_UNDEFINED && w > 0 && h > 0 && g_overlay.WantsDraw(hwnd_.load()) &&
        OverlayPass(cmd, img, fin_view, fmt, w, h)) {
        img = post_[5].img;
        w = post_[5].w;
        h = post_[5].h;
        ov_shown_ = true;
    }
    if (img) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {(int)extent_.width, (int)extent_.height, 1};
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
    } else {
        VkClearColorValue c{{0.5f, 0.0f, 0.5f, 1.0f}};
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    pending_ = true;
}

bool Presenter::OverlayPass(VkCommandBuffer cmd, VkImage img, VkImageView view, VkFormat fmt, int w, int h) {
    if (!EnsurePostPipes()) return false;
    const int ew = (int)extent_.width, eh = (int)extent_.height;
    const int qw = std::max(1, ew / 4), qh = std::max(1, eh / 4);
    if (!EnsurePostImage(3, qw, qh) || !EnsurePostImage(4, qw, qh) || !EnsurePostImage(5, ew, eh, VK_FORMAT_R8G8B8A8_UNORM))
        return false;
    if (!view) {
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = img;
        vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vc.format = fmt;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(g_vk.dev, &vc, nullptr, &view) != VK_SUCCESS) return false;
        g_vk.frames[g_vk.cur].dead_views.push_back(view);
    }
    const float t = g_overlay.Blend(), e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const bool blur = g_overlay.Blur();
    if (blur) {
        const int32_t dn[4] = {qw, qh, 0, 0}, hx[4] = {qw, qh, 1, 0}, vy[4] = {qw, qh, 0, 1};
        RunPost(cmd, kPostOvDown, view, true, 3, dn, sizeof dn);
        for (int i = 0; i < 2; ++i) {
            RunPost(cmd, kPostOvBlur, post_[3].view, true, 4, hx, sizeof hx);
            RunPost(cmd, kPostOvBlur, post_[4].view, true, 3, vy, sizeof vy);
        }
    }
    const struct {
        int32_t w, h;
        float t, blur, dim, pad;
    } pc{ew, eh, e, blur ? 1.0f : 0.0f, 0.52f, 0};
    RunPost(cmd, kPostOvCompose, view, true, 5, &pc, sizeof pc, blur ? post_[3].view : view, true);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    g_overlay.Record(cmd, post_[5].img, post_[5].view, ew, eh);
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    (void)w;
    (void)h;
    return true;
}

bool Presenter::DumpOverlay(const std::string& path) {
    PostImage& pi = post_[5];
    if (!ov_shown_ || !pi.img) return false;
    const uint32_t w = (uint32_t)pi.w, h = (uint32_t)pi.h;
    Buffer b = g_vk.CreateBuffer((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
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
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, pi.img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.queue);
    bool ok = false;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(w * 3);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* px = b.map + (uint64_t)y * w * 4;
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c) row[x * 3 + c] = px[x * 4 + c];
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        ok = true;
    }
    vkDestroyCommandPool(g_vk.dev, pool, nullptr);
    g_vk.DestroyBuffer(b);
    return ok;
}

void Presenter::FinishPresent() {
    if (!pending_) return;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_[sem_idx_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_idx_;
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resized_ = true;
    pending_ = false;
    // Menu dump requested by the test script (overlay_script dump=<name>): capture_dir/menu-<name>.ppm.
    if (g_overlay.Enabled()) {
        std::string req = g_overlay.TakeDumpRequest();
        if (!req.empty() && !ov_shown_) {
            NVK_INFO("menu: dump %s skipped (menu closed)", req.c_str());
        } else if (!req.empty()) {
            const std::string path = req.find_first_of("/\\") != std::string::npos ? req : g_cfg.capture_dir + "/menu-" + req + ".ppm";
            vkQueueWaitIdle(g_vk.queue);
            NVK_INFO("menu: dump %s: %s", path.c_str(), DumpOverlay(path) ? "ok" : "failed");
        }
    }
}

void Presenter::Stop() {
    g_overlay.Shutdown();
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (external_) hwnd_ = nullptr;
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        for (int i = 0; i < VkCtx::kFrames; ++i) {
            if (acquire_[i]) vkDestroySemaphore(g_vk.dev, acquire_[i], nullptr);
            if (render_[i]) vkDestroySemaphore(g_vk.dev, render_[i], nullptr);
        }
        if (swapchain_) vkDestroySwapchainKHR(g_vk.dev, swapchain_, nullptr);
        DestroyPost();
        for (VkPipeline& pp : post_pipe_) {
            if (pp) vkDestroyPipeline(g_vk.dev, pp, nullptr);
            pp = VK_NULL_HANDLE;
        }
        if (post_pl_) vkDestroyPipelineLayout(g_vk.dev, post_pl_, nullptr);
        if (post_dsl_) vkDestroyDescriptorSetLayout(g_vk.dev, post_dsl_, nullptr);
        if (post_smp_) vkDestroySampler(g_vk.dev, post_smp_, nullptr);
        if (post_lin_) vkDestroySampler(g_vk.dev, post_lin_, nullptr);
        post_pl_ = VK_NULL_HANDLE;
        post_dsl_ = VK_NULL_HANDLE;
        post_smp_ = post_lin_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(g_vk.instance, surface_, nullptr);
    hwnd_ = nullptr;
}

}  // namespace nvnvk
