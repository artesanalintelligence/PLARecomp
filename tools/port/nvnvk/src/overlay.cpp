// In-game overlay menu: window (subclass), controller, ImGui and Vulkan. The UI lives in overlay_ui.cpp.
// SPDX-License-Identifier: MIT
#include "overlay.h"

#include <xinput.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "nvnvk.h"
#include "overlay_res.h"
#include "vkctx.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nvnvk {

namespace {
// Config reading (like CfgStr in mod_main.cpp: searches for "key" anywhere in the text).
std::string CfgStr(const char* json, const char* key, const char* def) {
    const std::string j = json ? json : "";
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return def;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) ++p;
    if (p < j.size() && j[p] == '"') {
        std::string out;
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] == '\\' && p + 1 < j.size()) ++p;
            out += j[p];
        }
        return out;
    }
    const size_t e = j.find_first_of(",}", p);
    std::string v = j.substr(p, e - p);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return v;
}
bool CfgBool(const char* json, const char* key, bool def) {
    const std::string v = CfgStr(json, key, def ? "true" : "false");
    return v == "true" || v == "1";
}
}  // namespace

// overlay_ui.cpp
void OverlayUiInit(Overlay& ov, const std::string& pla_ini, bool save, const std::string& version);
void OverlayUiFrame(Overlay& ov, HWND hwnd, int w, int h, double now);
void OverlayUiScript(Overlay& ov, const std::string& action);
void OverlayUiShutdown();

Overlay g_overlay;

namespace {
std::recursive_mutex g_mu;  // ImGui context: window thread (WndProc, reentrant: ReleaseCapture sends WM_CAPTURECHANGED) and render thread
std::atomic<WNDPROC> g_orig{nullptr};
std::atomic<HWND> g_sub{nullptr};
std::atomic<bool> g_quit{false};
std::thread g_pad_thread;

double Now() {
    static LARGE_INTEGER fq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)fq.QuadPart;
}

// ---- window operations: always on its thread (custom messages), never with g_mu held ----
LONG_PTR g_prev_style = 0;
RECT g_prev_rect{};
bool g_is_fs = false;

void SetFullscreen(HWND h, bool on) {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    if (on) {
        if (!g_is_fs) {
            g_prev_style = GetWindowLongPtrW(h, GWL_STYLE);
            GetWindowRect(h, &g_prev_rect);
        }
        SetWindowLongPtrW(h, GWL_STYLE, (g_prev_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_is_fs = true;
    } else {
        LONG_PTR st = g_is_fs ? g_prev_style : GetWindowLongPtrW(h, GWL_STYLE);
        if (!(st & WS_CAPTION)) st = (st & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW;
        SetWindowLongPtrW(h, GWL_STYLE, st | WS_VISIBLE);
        RECT r = g_prev_rect;
        if (!g_is_fs || r.right - r.left < 320) {
            const int w = 1280, hh = 720;
            RECT c{0, 0, w, hh};
            AdjustWindowRectEx(&c, (DWORD)st, FALSE, (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE));
            const int cx = (mi.rcWork.left + mi.rcWork.right) / 2, cy = (mi.rcWork.top + mi.rcWork.bottom) / 2;
            r = RECT{cx - (c.right - c.left) / 2, cy - (c.bottom - c.top) / 2, 0, 0};
            r.right = r.left + (c.right - c.left);
            r.bottom = r.top + (c.bottom - c.top);
        }
        SetWindowPos(h, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_is_fs = false;
    }
}

void SetClientSize(HWND h, int w, int hh) {
    const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    if (!(st & WS_CAPTION)) return;  // borderless / fullscreen: left alone
    RECT c{0, 0, w, hh};
    AdjustWindowRectEx(&c, (DWORD)st, FALSE, (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE));
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    const int ww = c.right - c.left, wh = c.bottom - c.top;
    const int x = std::max<int>(mi.rcWork.left, (mi.rcWork.left + mi.rcWork.right - ww) / 2);
    const int y = std::max<int>(mi.rcWork.top, (mi.rcWork.top + mi.rcWork.bottom - wh) / 2);
    SetWindowPos(h, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOOWNERZORDER);
}

bool IsKeyboardMsg(UINT m) { return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_CHAR; }
bool IsMouseMsg(UINT m) { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSELEAVE; }

LRESULT CALLBACK OvWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC orig = g_orig.load();
    auto pass = [&] { return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l); };
    if (m == kOvMsgFullscreen) {
        SetFullscreen(h, w != 0);
        return 0;
    }
    if (m == kOvMsgClientSize) {
        SetClientSize(h, (int)w, (int)l);
        return 0;
    }
    if (m == WM_NCDESTROY && g_sub.load() == h) {
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        g_sub = nullptr;
        return pass();
    }
    Overlay& ov = g_overlay;
    const int mode = ov.mode_.load();
    if (mode != Overlay::kClosed && ov.capturing_.load() && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN)) {
        int vk = (int)w;
        const bool ext = ((l >> 24) & 1) != 0;
        if (vk == VK_SHIFT) vk = (int)MapVirtualKeyW((UINT)((l >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
        ov.OnCapturedKey(vk, ext);
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w == VK_ESCAPE) {
        if (!(l & (1 << 30))) ov.OnEscape();  // ignore auto-repeat
        return 0;
    }
    if (mode == Overlay::kClosed) return pass();
    if (IsKeyboardMsg(m) || (IsMouseMsg(m) && m != WM_MOUSELEAVE)) ov.last_input_ = 0;
    LRESULT r = 0;
    try {  // an exception here would kill the game (the WndProc runs on SDL's thread)
        std::lock_guard<std::recursive_mutex> lk(g_mu);
        if (ImGui::GetCurrentContext()) r = ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    } catch (...) {
        r = 0;
    }
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
        if (!r) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));  // IDC_ARROW
        return TRUE;
    }
    switch (m) {
    // With the menu open the game gets no key presses (releases still pass, so nothing stays held).
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return 0;
    default:
        return pass();
    }
}

void Subclass(HWND h) {
    if (!h || g_sub.load() == h) return;
    const LONG_PTR cur = GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!cur || cur == reinterpret_cast<LONG_PTR>(&OvWndProc)) return;
    g_orig = reinterpret_cast<WNDPROC>(cur);
    if (SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&OvWndProc))) g_sub = h;
}

// ---- controller: Minus + Plus (Back + Start in XInput) held 0.4 s open or close the menu ----
void PadThread() {
    SetThreadDescription(GetCurrentThread(), L"nvnvk menu pad");
    HMODULE x = LoadLibraryW(L"xinput1_4.dll");
    if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    auto get = x ? reinterpret_cast<GetStateFn>(GetProcAddress(x, "XInputGetState")) : nullptr;
    if (!get) return;
    int slot = -1;
    double probe = 0, combo_t = 0;
    bool fired = false;
    WORD prev = 0;
    while (!g_quit.load()) {
        const double now = Now();
        XINPUT_STATE st{};
        if (slot < 0) {
            if (now - probe > 2.0) {
                probe = now;
                for (DWORD i = 0; i < XUSER_MAX_COUNT && slot < 0; ++i)
                    if (get(i, &st) == ERROR_SUCCESS) slot = (int)i;
            }
        } else if (get((DWORD)slot, &st) != ERROR_SUCCESS) {
            slot = -1;
        } else {
            const WORD b = st.Gamepad.wButtons;
            const bool combo = (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START);
            if (combo && !(prev & XINPUT_GAMEPAD_BACK && prev & XINPUT_GAMEPAD_START)) combo_t = now;
            if (combo && !fired && now - combo_t >= 0.4) {
                fired = true;
                g_overlay.ToggleFromPad();
            }
            if (!combo) fired = false;
            const bool sticks = std::abs(st.Gamepad.sThumbLX) > 12000 || std::abs(st.Gamepad.sThumbLY) > 12000;
            if ((b && b != prev) || sticks) g_overlay.last_input_ = 1;
            prev = b;
        }
        Sleep(slot < 0 ? 200 : 25);
    }
}

const void* Recurso(int id, DWORD* size) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Recurso), &m);
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(m, r);
    *size = SizeofResource(m, r);
    return g ? LockResource(g) : nullptr;
}

struct ScriptStep {
    double t;
    std::string action;
};
std::vector<ScriptStep> g_script;
size_t g_script_next = 0;
double g_last_frame = 0;
}  // namespace

// Fonts (overlay_ui.cpp uses them by index).
ImFont* g_ov_fonts[3] = {};

void Overlay::Configure(const char* cfg) {
    enabled_ = CfgBool(cfg, "overlay", true);
    start_ = CfgBool(cfg, "overlay_start", true);
    // The test queue (run-pla-exe.ps1) sets NVNVK_OVERLAY_START=0: no start screen covering screenshots and
    // blocking scripted input. A job that wants to test it sets NVNVK_OVERLAY_START=1.
    if (const char* e = std::getenv("NVNVK_OVERLAY_START")) start_ = e[0] == '1';
    blur_ = CfgBool(cfg, "overlay_blur", true);
    save_ = CfgBool(cfg, "overlay_save", true);
    script_ = CfgStr(cfg, "overlay_script", "");
    pla_ini_ = CfgStr(cfg, "overlay_pla_ini", "");
    version_ = CfgStr(cfg, "overlay_version", "");
    // Test script: "sec:action;sec:action;..." (actions in overlay_ui.cpp, OverlayUiScript).
    g_script.clear();
    size_t p = 0;
    while (p < script_.size()) {
        size_t e = script_.find(';', p);
        if (e == std::string::npos) e = script_.size();
        const std::string it = script_.substr(p, e - p);
        const size_t c = it.find(':');
        if (c != std::string::npos) g_script.push_back({std::atof(it.substr(0, c).c_str()), it.substr(c + 1)});
        p = e + 1;
    }
    if (enabled_) NVK_INFO("menu: active (start screen %s, blur %s, script of %zu steps)", start_ ? "yes" : "no", blur_ ? "yes" : "no", g_script.size());
}

bool Overlay::InitImGui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavMoveSetMousePos = false;
    io.ConfigNavCursorVisibleAuto = true;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 2;
    fc.OversampleV = 2;
    const int ids[3] = {kResLatoRegular, kResLatoBold, kResLatoBlack};
    for (int i = 0; i < 3; ++i) {
        DWORD sz = 0;
        const void* d = Recurso(ids[i], &sz);
        if (d && sz) g_ov_fonts[i] = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(d), (int)sz, 20.0f, &fc);
        if (!g_ov_fonts[i]) g_ov_fonts[i] = i ? g_ov_fonts[0] : io.Fonts->AddFontDefault();
    }
    if (!ImGui_ImplWin32_Init(hwnd)) return false;
    return true;
}

bool Overlay::InitVulkan() {
    VkAttachmentDescription at{};
    at.format = VK_FORMAT_R8G8B8A8_UNORM;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    at.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rc.attachmentCount = 1;
    rc.pAttachments = &at;
    rc.subpassCount = 1;
    rc.pSubpasses = &sp;
    if (vkCreateRenderPass(g_vk.dev, &rc, nullptr, &rp_) != VK_SUCCESS) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_vk.instance;
    ii.PhysicalDevice = g_vk.phys;
    ii.Device = g_vk.dev;
    ii.QueueFamily = g_vk.qfamily;
    ii.Queue = g_vk.queue;
    ii.DescriptorPoolSize = 32;
    ii.MinImageCount = 2;
    ii.ImageCount = VkCtx::kFrames + 1;
    ii.PipelineInfoMain.RenderPass = rp_;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    return ImGui_ImplVulkan_Init(&ii);
}

bool Overlay::WantsDraw(HWND hwnd) {
    if (!enabled_ || failed_ || !hwnd) return false;
    const double now = Now();
    if (!inited_) {
        if (!InitImGui(hwnd) || !InitVulkan()) {
            failed_ = true;
            NVK_WARN("menu: could not initialize ImGui (Win32/Vulkan); the menu is disabled");
            return false;
        }
        hwnd_ = hwnd;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            OverlayUiInit(*this, pla_ini_, save_, version_);
        }
        Subclass(hwnd);
        NVK_INFO("menu: window subclass %s", g_sub.load() == hwnd ? "installed" : "NOT installed (Esc will not open the menu)");
        g_quit = false;
        g_pad_thread = std::thread(PadThread);
        if (start_) {
            mode_ = kStart;
            blend_ = 1.0f;  // the start screen appears fully shown
        }
        inited_ = true;
        g_last_frame = now;
        NVK_INFO("menu: ready on window %p%s", (void*)hwnd, start_ ? " (start screen)" : "");
    }
    // Test script (seconds since mod start).
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_cfg.t0).count();
    while (g_script_next < g_script.size() && t >= g_script[g_script_next].t) {
        const std::string a = g_script[g_script_next++].action;
        NVK_INFO("menu: script %.1f s: %s", t, a.c_str());
        if (a == "esc") {
            OnEscape();
        } else if (a == "combo") {
            ToggleFromPad();
        } else if (a.rfind("wm=", 0) == 0) {
            // Real messages to the game window (test the subclass and ImGui's Win32 backend):
            // wm=esc, wm=key:<vk hex>, wm=click:<x>,<y> (0..1 of the client area). Old aliases: tecla:, clic:.
            const std::string v = a.substr(3);
            auto key = [&](UINT vk) {
                const LPARAM sc = (LPARAM)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16;
                PostMessageW(hwnd_, WM_KEYDOWN, vk, sc | 1);
                PostMessageW(hwnd_, WM_KEYUP, vk, sc | 1 | (1u << 30) | (1u << 31));
            };
            if (v == "esc") key(VK_ESCAPE);
            else if (v.rfind("key:", 0) == 0) key((UINT)std::strtoul(v.c_str() + 4, nullptr, 16));
            else if (v.rfind("tecla:", 0) == 0) key((UINT)std::strtoul(v.c_str() + 6, nullptr, 16));
            else if (v.rfind("click:", 0) == 0 || v.rfind("clic:", 0) == 0) {
                RECT rc{};
                GetClientRect(hwnd_, &rc);
                const double fx = std::atof(v.c_str() + v.find(':') + 1);
                const size_t c = v.find(',');
                const double fy = c == std::string::npos ? 0.5 : std::atof(v.c_str() + c + 1);
                const LPARAM xy = MAKELPARAM((int)(fx * rc.right), (int)(fy * rc.bottom));
                PostMessageW(hwnd_, WM_MOUSEMOVE, 0, xy);
                PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, xy);
                PostMessageW(hwnd_, WM_LBUTTONUP, 0, xy);
            }
        } else {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            OverlayUiScript(*this, a);
        }
    }
    const double dt = std::min(0.1, now - g_last_frame);
    g_last_frame = now;
    const float target = mode_.load() != kClosed ? 1.0f : 0.0f;
    if (blend_ < target) blend_ = std::min(target, blend_ + (float)(dt / 0.20));
    else if (blend_ > target) blend_ = std::max(target, blend_ - (float)(dt / 0.15));
    return blend_ > 0.0f;
}

void Overlay::Record(VkCommandBuffer cmd, VkImage, VkImageView view, int w, int h) {
    if (!inited_) return;
    if (view != fb_view_ || w != fb_w_ || h != fb_h_) {
        if (fb_) vkDestroyFramebuffer(g_vk.dev, fb_, nullptr);  // the previous image was already freed after vkDeviceWaitIdle
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp_;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = (uint32_t)w;
        fi.height = (uint32_t)h;
        fi.layers = 1;
        fb_ = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(g_vk.dev, &fi, nullptr, &fb_) != VK_SUCCESS) return;
        fb_view_ = view;
        fb_w_ = w;
        fb_h_ = h;
    }
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    // Mouse: if the window size differs from the image (resize in progress), the difference is ignored.
    ImGui::NewFrame();
    OverlayUiFrame(*this, hwnd_, w, h, Now());
    ImGui::Render();
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp_;
    bi.framebuffer = fb_;
    bi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRenderPass(cmd);
}

std::string Overlay::TakeDumpRequest() {
    std::string s;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    s.swap(dump_req_);
    return s;
}

void Overlay::Shutdown() {
    g_quit = true;
    if (g_pad_thread.joinable()) g_pad_thread.join();
    if (!inited_) return;
    HWND h = g_sub.exchange(nullptr);
    if (h && IsWindow(h) && GetWindowLongPtrW(h, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&OvWndProc))
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig.load()));
    mode_ = kClosed;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    OverlayUiShutdown();
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        ImGui_ImplVulkan_Shutdown();
        if (fb_) vkDestroyFramebuffer(g_vk.dev, fb_, nullptr);
        if (rp_) vkDestroyRenderPass(g_vk.dev, rp_, nullptr);
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    fb_ = VK_NULL_HANDLE;
    rp_ = VK_NULL_HANDLE;
    inited_ = false;
}

int Overlay::ActiveBits() const {
    const int m = mode_.load();
    return m == kClosed ? 0 : (1 | (m == kStart ? 2 : 0));
}

void Overlay::OnEscape() {
    if (!inited_) return;
    int expected = kClosed;
    if (mode_.compare_exchange_strong(expected, kOptions)) {
        from_start_ = false;
        t_open_ = Now();
        return;
    }
    esc_pending_.fetch_add(1);
}

void Overlay::ToggleFromPad() {
    if (!inited_) return;
    if (mode_.load() == kOptions && !from_start_.load() && !capturing_.load()) {
        mode_ = kClosed;
        return;
    }
    OnEscape();
}

void Overlay::OnCapturedKey(int vk, bool ext) {
    captured_ext_ = ext;
    captured_vk_ = vk;
}

}  // namespace nvnvk

extern "C" __declspec(dllexport) int nvnvk_overlay_active(void) { return nvnvk::g_overlay.ActiveBits(); }
