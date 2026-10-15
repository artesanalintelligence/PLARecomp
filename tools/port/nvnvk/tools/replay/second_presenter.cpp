// Second VkDevice presenting in its own window while the replayer runs nvnvk, to mimic suyu's
// Vulkan renderer in shadow mode (two devices in the same process; this presenter's instance DOES
// load the system's implicit layers, like suyu's).
//   nvnvk_replay <capture> --second-presenter
// SPDX-License-Identifier: MIT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace second_presenter {

namespace {
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_presents{0}, g_errors{0};
std::thread g_thread;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

void Run() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"nvnvk_second_presenter";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"second presenter (like suyu)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                40, 40, 960, 540, nullptr, nullptr, inst, nullptr);
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "suyu-like";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VkInstance vi;
    if (vkCreateInstance(&ici, nullptr, &vi) != VK_SUCCESS) {
        std::fprintf(stderr, "[2nd] vkCreateInstance failed\n");
        return;
    }
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = inst;
    sci.hwnd = hwnd;
    VkSurfaceKHR surf;
    vkCreateWin32SurfaceKHR(vi, &sci, nullptr, &surf);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vi, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(vi, &n, pds.data());
    VkPhysicalDevice pd = pds[0];
    for (auto d : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = d;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) {
        std::fprintf(stderr, "[2nd] vkCreateDevice failed\n");
        return;
    }
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sw.surface = surf;
    sw.minImageCount = std::max(3u, caps.minImageCount);
    sw.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sw.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sw.imageExtent = caps.currentExtent;
    sw.imageArrayLayers = 1;
    sw.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sw.preTransform = caps.currentTransform;
    sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sw.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sw.clipped = VK_TRUE;
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev, &sw, nullptr, &sc) != VK_SUCCESS) {
        std::fprintf(stderr, "[2nd] vkCreateSwapchainKHR failed\n");
        return;
    }
    vkGetSwapchainImagesKHR(dev, sc, &n, nullptr);
    std::vector<VkImage> imgs(n);
    vkGetSwapchainImagesKHR(dev, sc, &n, imgs.data());
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acq, ren;
    vkCreateSemaphore(dev, &semi, nullptr, &acq);
    vkCreateSemaphore(dev, &semi, nullptr, &ren);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence;
    vkCreateFence(dev, &fci, nullptr, &fence);
    std::fprintf(stderr, "[2nd] presenter ready: %u images %ux%u\n", n, sw.imageExtent.width, sw.imageExtent.height);
    uint64_t frame = 0;
    while (!g_stop) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        uint32_t idx = 0xFFFFFFFFu;
        VkResult r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            if (g_errors.fetch_add(1) < 20) std::fprintf(stderr, "[2nd] vkAcquireNextImageKHR = %d (idx %u)\n", (int)r, idx);
            vkQueueSubmit(q, 0, nullptr, fence);
            Sleep(16);
            continue;
        }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[idx];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue c{{(frame % 120) / 120.0f, 0.2f, 0.4f, 1.0f}};
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acq;
        si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &ren;
        r = vkQueueSubmit(q, 1, &si, fence);
        if (r != VK_SUCCESS && g_errors.fetch_add(1) < 20) std::fprintf(stderr, "[2nd] vkQueueSubmit = %d\n", (int)r);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &ren;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(q, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && g_errors.fetch_add(1) < 20)
            std::fprintf(stderr, "[2nd] vkQueuePresentKHR = %d\n", (int)r);
        g_presents.fetch_add(1);
        ++frame;
    }
    vkDeviceWaitIdle(dev);
    vkDestroySwapchainKHR(dev, sc, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroySurfaceKHR(vi, surf, nullptr);
    vkDestroyInstance(vi, nullptr);
    DestroyWindow(hwnd);
}
}  // namespace

void Start() {
    g_thread = std::thread(Run);
    Sleep(800);  // like suyu: its device exists before nvnvk's
}

void Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    std::fprintf(stderr, "[2nd] presents=%llu errors=%llu\n", (unsigned long long)g_presents.load(),
                 (unsigned long long)g_errors.load());
}

}  // namespace second_presenter
