// PLARecompSetup.exe: Win32 window + Direct3D 11 + Dear ImGui (official win32 and dx11 backends).
// A single .exe: Lato fonts and the icon are resources; the components are an appended payload (components.h).
//
//   PLARecompSetup.exe [--dump <path>] [--dest <folder>] [--work <folder>] [--keys <prod.keys>] [--title-keys <file>]
//                      [--simulate] [--sim-fail <phase>] [--sim-scale <x>] [--capture <folder>] [--only <phases>]
//                      [--size <w>x<h>] [--data-dir <folder>] [--dev-repo <checkout>] [--url-base <url>] [--strip-path]
//
// --capture walks through the wizard unattended and saves a PNG of each step, read back from this window's own
// swap chain (never from the screen). The window is placed off-screen.
// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "engine.h"
#include "resources.h"
#include "ui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_sc;
ComPtr<ID3D11RenderTargetView> g_rtv;
UINT g_rw = 0, g_rh = 0;
bool g_occluded = false;
setup::Options g_op;

void CreateRtv() {
    ComPtr<ID3D11Texture2D> bb;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &sd, &g_sc,
                                               &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no usable GPU: WARP (software)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev,
                                           &got, &g_ctx);
    if (FAILED(hr)) return false;
    // Alt+Enter makes no sense here.
    ComPtr<IDXGIFactory> fac;
    if (SUCCEEDED(g_sc->GetParent(IID_PPV_ARGS(&fac)))) fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    CreateRtv();
    return true;
}

// Saves the back buffer (what was just drawn) as a PNG with WIC.
bool SavePng(const std::wstring& path) {
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return false;
    g_ctx->CopyResource(st.Get(), bb.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::string px((size_t)d.Width * d.Height * 4, '\0');
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        uint8_t* dst = (uint8_t*)px.data() + (size_t)y * d.Width * 4;
        memcpy(dst, src, (size_t)d.Width * 4);
        for (UINT x = 0; x < d.Width; ++x) dst[x * 4 + 3] = 255;
    }
    g_ctx->Unmap(st.Get(), 0);
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> s;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(s.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(d.Width, d.Height)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fr->SetPixelFormat(&fmt))) return false;
    if (FAILED(fr->WritePixels(d.Height, d.Width * 4, (UINT)px.size(), (BYTE*)px.data()))) return false;
    return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}

const void* Resource(int id, DWORD& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(nullptr, r);
    size = SizeofResource(nullptr, r);
    return g ? LockResource(g) : nullptr;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const int ids[3] = {kResLatoRegular, kResLatoBold, kResLatoBlack};
    for (int i = 0; i < 3; ++i) {
        DWORD size = 0;
        const void* d = Resource(ids[i], size);
        ImFontConfig fc;
        fc.FontDataOwnedByAtlas = false;  // the data belongs to the .exe resource
        std::snprintf(fc.Name, sizeof fc.Name, "Lato-%d", i);
        if (d && size) setup::ui::g_fonts[i] = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(d), (int)size, 18.0f, &fc);
        if (!setup::ui::g_fonts[i]) setup::ui::g_fonts[i] = i ? setup::ui::g_fonts[0] : io.Fonts->AddFontDefault();
    }
    io.FontDefault = setup::ui::g_fonts[0];
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) return 0;
        g_rw = LOWORD(l);
        g_rh = HIWORD(l);
        return 0;
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(h);
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = MulDiv(960, dpi ? dpi : 96, 96);
        mm->ptMinTrackSize.y = MulDiv(620, dpi ? dpi : 96, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;  // Alt does not open the window menu
        break;
    case WM_CLOSE:
        if (setup::AppRequestClose()) DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void ParseArgs() {
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; ++i) {
        const std::wstring k = a[i];
        auto next = [&]() -> std::wstring { return i + 1 < n ? a[++i] : L""; };
        if (k == L"--simulate") g_op.simulate = true;
        else if (k == L"--sim-fail") { g_op.simulate = true; g_op.sim_fail = setup::U8(next()); }
        else if (k == L"--sim-scale") g_op.sim_scale = _wtof(next().c_str());
        else if (k == L"--capture") g_op.capture = next();
        else if (k == L"--dump") g_op.dump = next();
        else if (k == L"--dest") g_op.dest = next();
        else if (k == L"--work") g_op.work = next();
        else if (k == L"--keys") g_op.keys = next();
        else if (k == L"--title-keys") g_op.title_keys = next();
        else if (k == L"--only") g_op.only = setup::U8(next());
        else if (k == L"--data-dir") g_op.data_dir = next();
        else if (k == L"--dev-repo") g_op.dev_repo = next();
        else if (k == L"--url-base") g_op.url_base = setup::U8(next());
        else if (k == L"--strip-path") setup::StripToolPaths();
    }
    LocalFree(a);
    if (g_op.sim_scale <= 0) g_op.sim_scale = 1.0;
}

std::wstring SizeArg() {
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    std::wstring r;
    for (int i = 1; i + 1 < n; ++i)
        if (std::wstring(a[i]) == L"--size") r = a[i + 1];
    LocalFree(a);
    return r;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ParseArgs();
    const bool capture = !g_op.capture.empty();
    if (capture) setup::MakeDirs(g_op.capture);

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(kResIcon));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PLARecompSetup";
    RegisterClassExW(&wc);

    // Size: 1280x800 (design) at the monitor scale, within the work area.
    const POINT pt{0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    const UINT dpi = capture ? 96 : GetDpiForSystem();
    int cw = MulDiv(1280, dpi, 96), ch = MulDiv(800, dpi, 96);
    const std::wstring sz = SizeArg();
    if (!sz.empty()) swscanf_s(sz.c_str(), L"%dx%d", &cw, &ch);
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    const int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    int x, y;
    if (capture) {
        // Off-screen: nothing on the desktop shows up in the screenshots or gets covered.
        x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    } else {
        ww = std::min(ww, aw);
        wh = std::min(wh, ah);
        x = mi.rcWork.left + (aw - ww) / 2;
        y = mi.rcWork.top + (ah - wh) / 2;
    }
    HWND hwnd = CreateWindowExW(capture ? WS_EX_TOOLWINDOW : 0, wc.lpszClassName, L"PLARecomp Setup",
                                style, x, y, ww, wh, nullptr, nullptr, inst, nullptr);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    const COLORREF caption = RGB(18, 14, 28);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);

    if (!CreateDevice(hwnd)) {
        MessageBoxW(hwnd, L"Direct3D 11 could not be started.", L"PLARecomp Setup", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, capture ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev.Get(), g_ctx.Get());
    LoadFonts();
    setup::AppInit(hwnd, g_op);

    float style_scale = -1;
    bool quit = false;
    while (!quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        if (quit) break;
        if (setup::AppWantsExit()) {
            DestroyWindow(hwnd);
            continue;
        }
        if (g_occluded && !capture && g_sc->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(30);
            continue;
        }
        g_occluded = false;
        if (g_rw && g_rh) {
            g_rtv.Reset();
            g_sc->ResizeBuffers(0, g_rw, g_rh, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
            g_rw = g_rh = 0;
            CreateRtv();
        }
        RECT cr;
        GetClientRect(hwnd, &cr);
        const int w = std::max<int>(1, cr.right), h = std::max<int>(1, cr.bottom);
        setup::ui::g_s = std::clamp(std::min(w / 1280.0f, h / 800.0f), 0.6f, 3.0f);
        if (std::fabs(style_scale - setup::ui::g_s) > 0.001f) {
            style_scale = setup::ui::g_s;
            setup::ui::ApplyStyle();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        setup::AppFrame(w, h);
        ImGui::Render();
        const float clear[4] = {0.043f, 0.035f, 0.07f, 1.0f};
        g_ctx->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_ctx->ClearRenderTargetView(g_rtv.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const std::wstring cap = setup::AppCaptureRequest();
        if (!cap.empty() && capture) {
            SavePng(setup::Join(g_op.capture, cap + L".png"));
        }
        const HRESULT hr = g_sc->Present(capture ? 0 : 1, 0);
        g_occluded = hr == DXGI_STATUS_OCCLUDED;
        if (capture) Sleep(8);
    }
    setup::AppShutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_rtv.Reset();
    g_sc.Reset();
    g_ctx.Reset();
    g_dev.Reset();
    CoUninitialize();
    return 0;
}
