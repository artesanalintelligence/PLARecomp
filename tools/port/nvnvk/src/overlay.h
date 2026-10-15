// In-game overlay menu (docs/port/IN-GAME-MENU.md): Dear ImGui (Win32 + Vulkan backends) drawn over the final
// frame, with the game dimmed and blurred behind it (compute in the final pass chain, vkctx.cpp).
// Esc (or holding Minus + Plus on the controller) opens and closes it; while open, the mouse is released and the
// nnshim passes no input to the game (export nvnvk_overlay_active). It can show a start screen at boot.
// SPDX-License-Identifier: MIT
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <string>

namespace nvnvk {

// Custom messages to the game window (handled on its thread, in the menu's subclass).
constexpr UINT kOvMsgFullscreen = WM_APP + 0x3A1;  // wParam: 1 = borderless fullscreen, 0 = windowed
constexpr UINT kOvMsgClientSize = WM_APP + 0x3A2;  // wParam x lParam: client area size

class Overlay {
public:
    // Reads the overlay* keys of the nvnvk config (pla_mod_init).
    void Configure(const char* config_json);
    bool Enabled() const { return enabled_; }
    // Render thread (Presenter::Present): draw this frame? Advances the animations and the test script.
    bool WantsDraw(HWND hwnd);
    // Layer opacity (0..1, with the open/close animation) and whether to blur the background.
    float Blend() const { return blend_; }
    bool Blur() const { return blur_; }
    // Builds the UI and records its drawing into `cmd` onto `view` (R8G8B8A8_UNORM in GENERAL, w x h).
    void Record(VkCommandBuffer cmd, VkImage img, VkImageView view, int w, int h);
    // Dump requested by the test script (.ppm path) or empty.
    std::string TakeDumpRequest();
    void Shutdown();

    // State shared with the WndProc and the nnshim.
    int ActiveBits() const;  // bit 0 = block game input; bit 1 = start screen
    void OnEscape();          // Esc key (window thread or script)
    void ToggleFromPad();     // Minus + Plus held
    bool CapturingKey() const { return capturing_.load(); }
    void OnCapturedKey(int vk, bool extended);  // window thread

    // Internals (overlay.cpp / overlay_ui.cpp).
    enum Mode { kClosed = 0, kStart = 1, kOptions = 2 };
    std::atomic<int> mode_{kClosed};
    std::atomic<bool> from_start_{false};
    std::atomic<int> esc_pending_{0};
    std::atomic<bool> capturing_{false};
    std::atomic<int> captured_vk_{0};
    std::atomic<bool> captured_ext_{false};
    std::atomic<int> last_input_{0};  // 0 keyboard and mouse, 1 controller

public:  // internals (overlay.cpp, overlay_ui.cpp)
    bool InitImGui(HWND hwnd);
    bool InitVulkan();
    bool enabled_ = false, start_ = true, blur_ = true, save_ = true;
    std::string script_, pla_ini_, version_;
    bool inited_ = false, failed_ = false;
    float blend_ = 0;
    double t_open_ = 0;
    HWND hwnd_ = nullptr;
    VkRenderPass rp_ = VK_NULL_HANDLE;
    VkFramebuffer fb_ = VK_NULL_HANDLE;
    VkImageView fb_view_ = VK_NULL_HANDLE;
    int fb_w_ = 0, fb_h_ = 0;
    std::string dump_req_;
};
extern Overlay g_overlay;

}  // namespace nvnvk

extern "C" __declspec(dllexport) int nvnvk_overlay_active(void);
