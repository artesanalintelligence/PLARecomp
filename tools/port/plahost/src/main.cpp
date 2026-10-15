// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PLA.exe without an emulator: loads the user's exefs and the recompiled images, runs the
// game's threads natively, serves the kernel calls and the few services the mods do not, and
// gives nvnvk a window to present in. See docs/port/HOST.md.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#include <psapi.h>

#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "loader.h"
#include "log.h"
#include "memory.h"
#include "mods.h"
#include "recomp.h"
#include "services.h"
#include "svc.h"
#include "tune.h"
#include "hle.h"

// Laptops with two GPUs (NVIDIA Optimus, AMD PowerXpress): ask the driver for the discrete GPU.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace plahost::host {

std::atomic<u64> g_frames{0};

namespace {

Options g_opts;
std::wstring g_exe_dir;
HWND g_hwnd = nullptr;
std::atomic<bool> g_exit_started{false};
bool g_fullscreen = false;
RECT g_windowed_rect{};
LONG_PTR g_windowed_style = 0;

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

bool ParseBool(const std::string& v, bool fallback) {
    const std::string l = Lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on" || l == "si") return true;
    if (l == "0" || l == "false" || l == "no" || l == "off") return false;
    return fallback;
}

constexpr const char* kDefaultIni =
    "; PLA.ini - options for PLA.exe (PLARecomp).\n"
    "; 1 = yes, 0 = no. Lines starting with ; are ignored.\n"
    "[PLA]\n"
    "fullscreen=0\n"
    "; Optional mods from the mods\\ folder (nnshim and nvnvk always load).\n"
    "mods=1\n"
    "; FPS cap (e.g. 30 or 60; 0 = no cap). Also --fps-cap=N.\n"
    "fps_cap=0\n"
    "; If the game stops drawing for N seconds, writes user\\log\\threads-*.txt (0 = off).\n"
    "hang_report=30\n";

void LoadOptions() {
    const auto ini = std::filesystem::path(g_exe_dir) / L"PLA.ini";
    std::ifstream in(ini, std::ios::binary);
    if (!in) {
        std::ofstream out(ini, std::ios::binary);
        out << kDefaultIni;
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        const std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(t.substr(0, eq)));
        const std::string value = Trim(t.substr(eq + 1));
        if (key == "fullscreen" || key == "pantalla_completa") g_opts.fullscreen = ParseBool(value, g_opts.fullscreen);
        else if (key == "mods") g_opts.mods = ParseBool(value, g_opts.mods);
        else if (key == "fps_cap" || key == "nvnvk_fps_cap" || key == "tope_fps_nvnvk") {
            g_opts.fps_cap = std::clamp(std::atoi(value.c_str()), 0, 1000);
            g_opts.fps_cap_set = true;
        } else if (key == "hang_report" || key == "informe_cuelgue") g_opts.hang_report = std::max(0, std::atoi(value.c_str()));
        else if (key == "name" || key == "nombre") { if (!value.empty()) g_opts.app_name = value; }
        else if (key == "memory_mib") g_opts.memory_mib = std::clamp<u64>(std::strtoull(value.c_str(), nullptr, 10), 2048, 8192);
        else if (key == "window_width") g_opts.window_width = std::clamp(std::atoi(value.c_str()), 320, 16384);
        else if (key == "window_height") g_opts.window_height = std::clamp(std::atoi(value.c_str()), 180, 16384);
        // Keys of the emulator-based PLA.exe (limiter, strict, renderer, native_threads...) mean
        // nothing here and are ignored.
    }
}

void ApplyFlags() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::string a = Utf8(argv[i]);
        if (a.rfind("--fps-cap=", 0) == 0 || a.rfind("--tope-fps=", 0) == 0) {
            g_opts.fps_cap = std::clamp(std::atoi(a.substr(a.find('=') + 1).c_str()), 0, 1000);
            g_opts.fps_cap_set = true;
        } else if (a == "-f" || a == "--fullscreen") {
            g_opts.fullscreen = true;
        } else if (a == "--windowed") {
            g_opts.fullscreen = false;
        } else if (a == "--no-mods" || a == "--sin-mods") {
            g_opts.mods = false;
        }
    }
    LocalFree(argv);
}

void SetFullscreen(bool on) {
    if (!g_hwnd || on == g_fullscreen) return;
    if (on) {
        GetWindowRect(g_hwnd, &g_windowed_rect);
        g_windowed_style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, (g_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_windowed_style);
        SetWindowPos(g_hwnd, nullptr, g_windowed_rect.left, g_windowed_rect.top,
                     g_windowed_rect.right - g_windowed_rect.left, g_windowed_rect.bottom - g_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);
    }
    g_fullscreen = on;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        // Off the window thread: the mods' shutdown may still talk to this window.
        if (!g_exit_started.load()) {
            std::thread([] { Exit(0, "window closed"); }).detach();
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            SetFullscreen(!g_fullscreen);
            return 0;
        }
        break;
    case WM_PAINT:
        if (g_frames.load() == 0) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(200, 200, 200));
            DrawTextW(dc, L"Loading...", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateGameWindow() {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"PLARecompHost";
    // Icon group 1 (the installer embeds the game's icon from the user's dump), else PLA.ico beside the exe.
    wc.hIcon = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    if (!wc.hIcon) {
        const auto ico = std::filesystem::path(g_exe_dir) / L"PLA.ico";
        wc.hIcon = static_cast<HICON>(LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
        wc.hIconSm = wc.hIcon;
    }
    RegisterClassExW(&wc);
    RECT r{0, 0, g_opts.window_width, g_opts.window_height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, Wide(g_opts.app_name).c_str(), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, SW_SHOW);
    if (g_opts.fullscreen) SetFullscreen(true);
    UpdateWindow(g_hwnd);
    return true;
}

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

std::string LogDir() {
    return ExeDirUtf8() + "\\user\\log";
}

std::string ModuleOf(u64 addr) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &m) && m) {
        wchar_t name[MAX_PATH];
        GetModuleFileNameW(m, name, MAX_PATH);
        char b[64];
        std::snprintf(b, sizeof b, "+%#llx", addr - reinterpret_cast<u64>(m));
        return Utf8(std::filesystem::path(name).filename().wstring()) + b;
    }
    return "?";
}

std::string GuestWhere(u64 pc) {
    for (const auto& im : recomp::Images()) {
        if (pc >= im.base && pc < im.hi + 4) {
            char b[64];
            std::snprintf(b, sizeof b, "%s+%#llx", im.name.c_str(), pc - im.base);
            return b;
        }
    }
    return "?";
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    const auto* rec = ep->ExceptionRecord;
    const std::string path = LogDir() + "\\crash-" + Stamp() + ".txt";
    std::string text;
    char b[512];
    std::snprintf(b, sizeof b, "PLARecomp host crash\nexception %#lx at %p (%s)\n", rec->ExceptionCode,
                  rec->ExceptionAddress, ModuleOf(reinterpret_cast<u64>(rec->ExceptionAddress)).c_str());
    text += b;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const u64 a = rec->ExceptionInformation[1];
        const u64 base = reinterpret_cast<u64>(mem::Base());
        std::snprintf(b, sizeof b, "access %s %#llx%s\n", rec->ExceptionInformation[0] ? "write" : "read", a,
                      a >= base && a < base + mem::kAddressSpaceEnd ? " (guest memory)" : "");
        text += b;
        if (a >= base && a < base + mem::kAddressSpaceEnd) {
            std::snprintf(b, sizeof b, "guest address %#llx\n", a - base);
            text += b;
        }
    }
    if (kernel::Thread* t = kernel::Current()) {
        std::snprintf(b, sizeof b, "guest thread %llu pc=%#llx (%s) lr=%#llx (%s) sp=%#llx\n", t->id, t->ctx.pc,
                      GuestWhere(t->ctx.pc).c_str(), t->ctx.x[30], GuestWhere(t->ctx.x[30]).c_str(), t->ctx.x[31]);
        text += b;
    } else {
        text += "not a guest thread\n";
    }
    std::FILE* f = _wfopen(Wide(path).c_str(), L"wb");
    if (f) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
    log::Raw(log::Critical, "Host", text);
    log::Raw(log::Critical, "Host", "crash report: " + path);
    log::Flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

void Watchdog() {
    SetThreadDescription(GetCurrentThread(), L"HostWatchdog");
    u64 last_frames = 0;
    auto last = std::chrono::steady_clock::now();
    s64 still_since = kernel::NowMs();
    bool reported = false;
    u64 ticks = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = std::chrono::steady_clock::now();
        const u64 frames = g_frames.load();
        const double secs = std::chrono::duration<double>(now - last).count();
        const double fps = (frames - last_frames) / secs;
        log::Write(log::Info, "Host", "PERF game_fps=%.3f frametime_ms=%.3f", fps, fps > 0 ? 1000.0 / fps : 0.0);
        tune::Tick();
        if (++ticks % 30 == 0) {
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
                LOGI("Host", "memory: private %llu MB, working set %llu MB, handles %zu", static_cast<unsigned long long>(pmc.PrivateUsage >> 20),
                     static_cast<unsigned long long>(pmc.WorkingSetSize >> 20), kernel::HandleCount());
            }
        }
        if (g_hwnd) {
            const std::string title = frames == 0 ? g_opts.app_name + "  |  Loading..."
                                                  : g_opts.app_name + "  |  " + std::to_string(static_cast<int>(fps + 0.5)) + " FPS";
            SetWindowTextW(g_hwnd, Wide(title).c_str());
        }
        if (frames != last_frames || frames == 0) {
            still_since = kernel::NowMs();
            reported = false;
        } else if (g_opts.hang_report > 0 && !reported && kernel::NowMs() - still_since >= g_opts.hang_report * 1000LL) {
            reported = true;
            const std::string p = WriteThreadReport("no frame presented");
            LOGW("Host", "no frame for %d s: thread report %s", g_opts.hang_report, p.c_str());
        }
        last_frames = frames;
        last = now;
    }
}

[[noreturn]] void Fatal(const std::string& msg) {
    LOGC("Host", "%s", msg.c_str());
    log::Flush();
    MessageBoxW(nullptr, Wide(msg).c_str(), L"PLARecomp", MB_ICONERROR | MB_OK);
    TerminateProcess(GetCurrentProcess(), 2);
    std::abort();
}

} // namespace

const Options& Opts() {
    return g_opts;
}

const std::wstring& ExeDir() {
    return g_exe_dir;
}

std::string ExeDirUtf8() {
    return Utf8(g_exe_dir);
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void* WindowHandle() {
    return g_hwnd;
}

// Guest call chain from the frame records (x29 -> {previous x29, return address}); for the hang report, so a
// waiting thread shows which game function waits and not only the SDK primitive.
std::string GuestChain(u64 fp) {
    std::string s;
    char b[128];
    for (int i = 0; i < 8 && fp && (fp & 7) == 0; ++i) {
        u64 rec[2] = {};
        if (mem::Read(fp, rec, sizeof rec) != sizeof rec || !rec[1]) break;
        std::snprintf(b, sizeof b, "%s%#llx (%s)", i ? " < " : "\n    calls: ", static_cast<unsigned long long>(rec[1]),
                      GuestWhere(rec[1]).c_str());
        s += b;
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    return s;
}

std::string WriteThreadReport(const char* reason) {
    const std::string path = LogDir() + "\\threads-" + Stamp() + ".txt";
    std::string text = std::string{"PLARecomp host thread report: "} + reason + "\n\n";
    char b[512];
    const s64 now = kernel::NowMs();
    for (const auto& t : kernel::Threads()) {
        const auto& c = t->ctx;
        std::snprintf(b, sizeof b,
                      "thread %llu prio %d core %d%s pc=%#llx (%s) lr=%#llx (%s) sp=%#llx svcs %llu %s svc %#x for %lld ms",
                      t->id, t->prio.load(), t->ideal_core, t->exited ? " EXITED" : "", c.pc, GuestWhere(c.pc).c_str(),
                      c.x[30], GuestWhere(c.x[30]).c_str(), c.x[31], static_cast<unsigned long long>(t->svc_count.load()),
                      t->in_svc ? "IN" : "after", t->last_svc.load(), static_cast<long long>(now - t->svc_since_ms.load()));
        text += b;
        if (const u32 nw = t->native_wait.load()) {
            std::snprintf(b, sizeof b, " native-wait %#x", nw);
            text += b;
        }
        if (!t->exited) text += GuestChain(c.x[29]);
        text += "\n";
    }
    text += sync::DescribeWaits();
    text += "\n" + svc::Report() + "\n" + ipc::Report();
    std::FILE* f = _wfopen(Wide(path).c_str(), L"wb");
    if (f) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
    return path;
}

void Exit(int code, const char* why) {
    if (g_exit_started.exchange(true)) {
        while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    LOGI("Host", "exit (%d): %s", code, why);
    kernel::g_exiting = true;
    // The mods' shutdown (nnshim flushes saves and stops audio; nvnvk releases the GPU), bounded.
    std::thread sd([] { mods::Shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!mods::Retired() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (sd.joinable()) {
        // Give shutdown the rest of its 5 s.
        HANDLE h = sd.native_handle();
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        WaitForSingleObject(h, static_cast<DWORD>(std::max<long long>(left, 0)));
        sd.detach();
    }
    log::Raw(log::Info, "Host", svc::Report());
    log::Raw(log::Info, "Host", ipc::Report());
    log::Raw(log::Info, "Host", mods::Report());
    LOGI("Host", "bye");
    log::Flush();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    std::abort();
}

// Per-monitor DPI aware (V2) before any window exists. Without it Windows virtualizes the coordinates on a scaled
// display: a 2560x1440 monitor at 125% looked like 2048x1152, so borderless fullscreen and the swapchain stayed at that
// size (bitmap-stretched by the compositor) and 1440p was only reachable in windowed mode.
void EnableDpiAwareness() {
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    if (const HMODULE u = GetModuleHandleW(L"user32.dll"))
        if (const auto f = reinterpret_cast<SetCtx>(GetProcAddress(u, "SetProcessDpiAwarenessContext")))
            if (f(reinterpret_cast<HANDLE>(-4)) || f(reinterpret_cast<HANDLE>(-3)))  // PER_MONITOR_AWARE_V2, then V1
                return;
    SetProcessDPIAware();  // before Windows 10 1703: system aware
}

int Run() {
    EnableDpiAwareness();
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exe_dir = std::filesystem::path(exe).parent_path().wstring();
    log::Open(LogDir());
    SetUnhandledExceptionFilter(CrashFilter);
    LoadOptions();
    ApplyFlags();
    LOGI("Host", "PLARecomp host starting in %s (fullscreen=%d mods=%d fps_cap=%d memory=%llu MiB)", ExeDirUtf8().c_str(),
         g_opts.fullscreen, g_opts.mods, g_opts.fps_cap, g_opts.memory_mib);
    tune::Init();
    timeBeginPeriod(1);

    std::string err;
    if (!mem::Init((g_opts.memory_mib + 1024) << 20, &err)) Fatal(err);
    const std::wstring exefs = g_exe_dir + L"\\exefs";
    loader::Npdm npdm;
    if (!loader::LoadNpdm(exefs, npdm, &err)) Fatal(err + " (the game's exefs folder must be beside PLA.exe)");
    std::vector<loader::Module> modules;
    if (!loader::LoadModules(exefs, modules, &err)) Fatal(err);
    if (!recomp::LoadImages(g_exe_dir, &err)) Fatal(err);
    if (!recomp::Attach(modules, &err)) Fatal(err);
    loader::RelocateRtld(modules.front());

    if (!CreateGameWindow()) Fatal("cannot create the game window");

    u64 code_size = 0;
    for (const auto& m : modules) code_size += m.size;
    const u64 stack_size = AlignUp(npdm.main_thread_stack_size ? npdm.main_thread_stack_size : 0x100000, kPageSize);
    svc::SetMemoryLimits(g_opts.memory_mib << 20, code_size, stack_size, npdm.title_id, npdm.system_resource_size);
    services::InitApplet();

    std::wstring mods_root = g_exe_dir + L"\\mods";
    if (const wchar_t* d = _wgetenv(L"PLA_RECOMP_MODS_DIR"); d && *d) mods_root = d;
    if (const wchar_t* d = _wgetenv(L"PLA_MODS_DIR"); d && *d) mods_root = d;
    if (!g_opts.mods) {
        _putenv_s("PLAHOST_REQUIRED_MODS_ONLY", "1");
    }
    std::string main_build;
    for (const auto& m : modules) {
        if (m.file == "main") main_build = loader::BuildIdHex(m);
    }
    hle::Install(); // before the mods: a mod hooking the same address chains to the host version
    mods::Activate(npdm.title_id, mods_root, main_build);

    // The main thread: its stack in the stack region, entry at rtld, x0 = 0, x1 = its handle.
    const u64 stack = mem::MapAnywhere(mem::kStackStart, mem::kStackSize, stack_size, mem::Stack, mem::RW);
    if (!stack) Fatal("cannot map the main thread stack");
    Result r;
    auto main_thread = kernel::CreateThread(modules.front().base, 0, stack + stack_size, npdm.main_thread_priority,
                                            npdm.main_thread_core, &r);
    if (!main_thread) Fatal("cannot create the main thread");
    main_thread->ctx.x[0] = 0;
    main_thread->ctx.x[1] = main_thread->handle;
    main_thread->name = "main";
    main_thread->is_main = true;
    LOGI("Host", "main thread %llu: entry %#llx stack %#llx-%#llx", main_thread->id, modules.front().base, stack,
         stack + stack_size);
    kernel::StartThread(main_thread);
    std::thread(Watchdog).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Exit(0, "message loop ended");
}

} // namespace plahost::host

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return plahost::host::Run();
}
