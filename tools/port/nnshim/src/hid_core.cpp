// nnshim::hid: host input backends (XInput, dynamic SDL2, keyboard, remote UDP).
//
// A host thread polls the devices at `poll_hz` and publishes a snapshot; the adapters
// (guest threads) only copy it. That way SDL calls always happen on the same thread and
// the game never waits on XInput (an empty slot takes milliseconds).
#include "nnshim/hid.hpp"
#include "nnshim/pointer.hpp"
#include "nnshim/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <xinput.h>
#endif

namespace nnshim::hid {
namespace {

using Clock = std::chrono::steady_clock;
constexpr float kPi = 3.14159265358979f;

// ---- generic controller elements (XInput and SDL translate to these) ----------------------
enum PadElem : int {
    PeSouth, PeEast, PeWest, PeNorth, PeLB, PeRB, PeLT, PeRT, PeStart, PeBack, PeLS, PeRS,
    PeDUp, PeDDown, PeDLeft, PeDRight, PeCount
};
const char* const kPadElemNames[PeCount] = {"south", "east", "west", "north", "lb", "rb", "lt", "rt",
                                            "start", "back", "ls", "rs", "dpup", "dpdown", "dpleft", "dpright"};

struct PadRaw {
    bool connected = false;
    bool elem[PeCount] = {};
    int lx = 0, ly = 0, rx = 0, ry = 0;   // ±0x7FFF, up = positive
    bool has_motion = false;
    float accel[3] = {0, 0, -1};          // already in Switch convention (G)
    float gyro[3] = {0, 0, 0};            // turns/s
};

// Named Switch buttons (0-15) + stick directions for the keyboard (16-23).
const char* const kSwitchNames[] = {"A", "B", "X", "Y", "StickL", "StickR", "L", "R", "ZL", "ZR",
                                    "Plus", "Minus", "Left", "Up", "Right", "Down",
                                    "LStickLeft", "LStickUp", "LStickRight", "LStickDown",
                                    "RStickLeft", "RStickUp", "RStickRight", "RStickDown"};
constexpr int kNumSwitch = 24;

// ---- SDL2 loaded at runtime (no headers or import lib) ------------------------
struct Sdl {
#ifdef _WIN32
    HMODULE lib = nullptr;
#endif
    int (*InitSubSystem)(uint32_t) = nullptr;
    void (*QuitSubSystem)(uint32_t) = nullptr;
    int (*SetHint)(const char*, const char*) = nullptr;
    int (*NumJoysticks)() = nullptr;
    int (*IsGameController)(int) = nullptr;
    void* (*GameControllerOpen)(int) = nullptr;
    void (*GameControllerClose)(void*) = nullptr;
    int (*GameControllerGetAttached)(void*) = nullptr;
    void (*GameControllerUpdate)() = nullptr;
    uint8_t (*GameControllerGetButton)(void*, int) = nullptr;
    int16_t (*GameControllerGetAxis)(void*, int) = nullptr;
    int (*GameControllerRumble)(void*, uint16_t, uint16_t, uint32_t) = nullptr;
    int (*GameControllerHasSensor)(void*, int) = nullptr;
    int (*GameControllerSetSensorEnabled)(void*, int, int) = nullptr;
    int (*GameControllerGetSensorData)(void*, int, float*, int) = nullptr;
    const char* (*GameControllerName)(void*) = nullptr;
    void (*PumpEvents)() = nullptr;
    bool ok = false;
};
constexpr uint32_t kSdlInitJoystick = 0x200, kSdlInitGameController = 0x2000, kSdlInitSensor = 0x8000;
constexpr int kSdlSensorAccel = 1, kSdlSensorGyro = 2;

struct Step {   // one remote command: state held for `ms`
    uint64_t buttons = 0;
    bool sticks = false;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    int ms = 0;
};

struct Motion {
    bool started = false;
    bool fusion = true;
    float revise_power = 0.03f, revise_range = 0.4f;
    int zero_drift = 1;  // Standard
};

// ---- shared state -------------------------------------------------------------------
std::mutex g_mutex;               // guards everything below
Config g_cfg;
struct Snapshot {
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
} g_snap;
std::atomic<int64_t> g_sampling{0};
SixAxisState g_motion_hist[32] = {};
int g_motion_head = 0, g_motion_count = 0;
float g_quat[4] = {1, 0, 0, 0};   // w, x, y, z
float g_angle[3] = {0, 0, 0};
bool g_has_motion = false;
std::map<uint32_t, Motion> g_motion_cfg;
VibrationValue g_vib[2] = {};     // left, right
Clock::time_point g_vib_time[2] = {};
bool g_vib_dirty = false;
std::deque<Step> g_steps;
Clock::time_point g_step_start{};
Step g_inject;
Clock::time_point g_inject_until{};
Info g_info;
std::atomic<bool> g_pad_connected{false};
std::atomic<bool> g_fake_pad{false};   // UDP tests ("fakepad 1"): as if a controller were connected
std::atomic<bool> g_menu_state{false};
// Virtual mouse for UDP tests ("mouse", "wheel", "mousemove" commands); guarded by g_mutex.
struct VirtualMouse {
    Clock::time_point btn_until[3] = {};      // left, right, middle
    int wheel = 0;                            // pending notches (signed)
    float vx = 0, vy = 0;                     // px per ms
    Clock::time_point move_until{};
} g_vmouse;

// Resolved mapping.
std::vector<int> g_key_map[kNumSwitch];     // virtual keys per button
std::vector<int> g_pad_map[16];             // controller elements per button

ExitSafeThread g_thread;
std::atomic<bool> g_stop{false};
bool g_initialized = false;
Sdl g_sdl;
void* g_pad = nullptr;
int g_xinput_slot = -1;
Clock::time_point g_last_probe{};
#ifdef _WIN32
SOCKET g_sock = INVALID_SOCKET;
#endif

int32_t Clamp16(int v) { return std::clamp(v, -0x7FFF, 0x7FFF); }
int32_t Dead(int v, int dz) { return (v > -dz && v < dz) ? 0 : v; }

uint64_t StickButtons(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    constexpr int32_t t = 0x4000;
    uint64_t b = 0;
    if (lx < -t) b |= BtnStickLLeft;
    if (lx > t) b |= BtnStickLRight;
    if (ly > t) b |= BtnStickLUp;
    if (ly < -t) b |= BtnStickLDown;
    if (rx < -t) b |= BtnStickRLeft;
    if (rx > t) b |= BtnStickRRight;
    if (ry > t) b |= BtnStickRUp;
    if (ry < -t) b |= BtnStickRDown;
    return b;
}

int SwitchIndex(const std::string& name) {
    for (int i = 0; i < kNumSwitch; ++i)
        if (!_stricmp(name.c_str(), kSwitchNames[i])) return i;
    return -1;
}

#ifdef _WIN32
int VkFromName(std::string n) {
    for (auto& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (n.rfind("VK_", 0) == 0) n = n.substr(3);
    if (n.rfind("0X", 0) == 0) return static_cast<int>(std::strtol(n.c_str(), nullptr, 16));
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return n[0];
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        const int k = std::atoi(n.c_str() + 1);
        if (k >= 1 && k <= 24) return VK_F1 + k - 1;
    }
    if (n.rfind("NUMPAD", 0) == 0 && n.size() == 7) return VK_NUMPAD0 + (n[6] - '0');
    static const std::pair<const char*, int> kNames[] = {
        {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN}, {"BACK", VK_BACK}, {"BACKSPACE", VK_BACK},
        {"TAB", VK_TAB}, {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE}, {"SPACE", VK_SPACE},
        {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"SHIFT", VK_SHIFT}, {"LCONTROL", VK_LCONTROL},
        {"RCONTROL", VK_RCONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL}, {"LMENU", VK_LMENU},
        {"LALT", VK_LMENU}, {"RMENU", VK_RMENU}, {"RALT", VK_RMENU}, {"LEFT", VK_LEFT}, {"UP", VK_UP},
        {"RIGHT", VK_RIGHT}, {"DOWN", VK_DOWN}, {"HOME", VK_HOME}, {"END", VK_END}, {"PRIOR", VK_PRIOR},
        {"PAGEUP", VK_PRIOR}, {"NEXT", VK_NEXT}, {"PAGEDOWN", VK_NEXT}, {"INSERT", VK_INSERT},
        {"DELETE", VK_DELETE}, {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD}, {"MINUS", VK_OEM_MINUS},
        {"PLUS", VK_OEM_PLUS}};
    for (const auto& [k, v] : kNames)
        if (n == k) return v;
    return 0;
}
#else
int VkFromName(const std::string&) { return 0; }
#endif

std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == '|' || c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void BuildMaps() {
    for (auto& v : g_key_map) v.clear();
    for (auto& v : g_pad_map) v.clear();
    // Default keyboard (the prototype's).
    const std::pair<const char*, const char*> kKeyDefault[] = {
        {"A", "RETURN"}, {"B", "BACK"}, {"X", "LSHIFT"}, {"Y", "SPACE"}, {"L", "Q"}, {"R", "E"},
        {"ZL", "1"}, {"ZR", "3"}, {"Plus", "TAB"}, {"Minus", "M"}, {"StickL", "Z"}, {"StickR", "C"},
        {"Left", "LEFT"}, {"Up", "UP"}, {"Right", "RIGHT"}, {"Down", "DOWN"},
        {"LStickLeft", "A"}, {"LStickUp", "W"}, {"LStickRight", "D"}, {"LStickDown", "S"},
        {"RStickLeft", "J"}, {"RStickUp", "I"}, {"RStickRight", "L"}, {"RStickDown", "K"}};
    std::map<int, std::string> keys;
    for (const auto& [b, k] : kKeyDefault) keys[SwitchIndex(b)] = k;
    for (const auto& [b, k] : g_cfg.keyboard_map) {
        const int i = SwitchIndex(b);
        if (i >= 0) keys[i] = k;
    }
    for (const auto& [i, list] : keys)
        for (const auto& name : SplitList(list))
            if (const int vk = VkFromName(name)) g_key_map[i].push_back(vk);
    // Controller: positional (south = B) or by label (south = A).
    const bool pos = g_cfg.positional_face_buttons;
    std::map<int, std::string> pad = {
        {0, pos ? "east" : "south"}, {1, pos ? "south" : "east"}, {2, pos ? "north" : "west"}, {3, pos ? "west" : "north"},
        {4, "ls"}, {5, "rs"}, {6, "lb"}, {7, "rb"}, {8, "lt"}, {9, "rt"}, {10, "start"}, {11, "back"},
        {12, "dpleft"}, {13, "dpup"}, {14, "dpright"}, {15, "dpdown"}};
    for (const auto& [b, e] : g_cfg.pad_map) {
        const int i = SwitchIndex(b);
        if (i >= 0 && i < 16) pad[i] = e;
    }
    for (const auto& [i, list] : pad)
        for (const auto& name : SplitList(list))
            for (int e = 0; e < PeCount; ++e)
                if (!_stricmp(name.c_str(), kPadElemNames[e])) g_pad_map[i].push_back(e);
}

// ---- SDL2 ----------------------------------------------------------------------------------
#ifdef _WIN32
template <class F> bool Bind(HMODULE m, F& f, const char* name) {
    f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return f != nullptr;
}

HMODULE SelfModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

bool LoadSdl() {
    std::wstring path;
    if (!g_cfg.sdl_library.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, g_cfg.sdl_library.c_str(), -1, nullptr, 0);
        path.resize(static_cast<size_t>(n));
        MultiByteToWideChar(CP_UTF8, 0, g_cfg.sdl_library.c_str(), -1, path.data(), n);
        path.resize(wcslen(path.c_str()));
    } else {
        wchar_t self[MAX_PATH];
        if (GetModuleFileNameW(SelfModule(), self, MAX_PATH)) {
            path = self;
            path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"SDL2.dll";
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) path = L"SDL2.dll";
        } else path = L"SDL2.dll";
    }
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) return false;
    Sdl& s = g_sdl;
    s.lib = m;
    bool ok = Bind(m, s.InitSubSystem, "SDL_InitSubSystem") && Bind(m, s.QuitSubSystem, "SDL_QuitSubSystem") &&
              Bind(m, s.SetHint, "SDL_SetHint") && Bind(m, s.NumJoysticks, "SDL_NumJoysticks") &&
              Bind(m, s.IsGameController, "SDL_IsGameController") && Bind(m, s.GameControllerOpen, "SDL_GameControllerOpen") &&
              Bind(m, s.GameControllerClose, "SDL_GameControllerClose") &&
              Bind(m, s.GameControllerGetAttached, "SDL_GameControllerGetAttached") &&
              Bind(m, s.GameControllerUpdate, "SDL_GameControllerUpdate") &&
              Bind(m, s.GameControllerGetButton, "SDL_GameControllerGetButton") &&
              Bind(m, s.GameControllerGetAxis, "SDL_GameControllerGetAxis") &&
              Bind(m, s.GameControllerName, "SDL_GameControllerName") && Bind(m, s.PumpEvents, "SDL_PumpEvents");
    // Optional (SDL >= 2.0.9 / 2.0.14).
    Bind(m, s.GameControllerRumble, "SDL_GameControllerRumble");
    Bind(m, s.GameControllerHasSensor, "SDL_GameControllerHasSensor");
    Bind(m, s.GameControllerSetSensorEnabled, "SDL_GameControllerSetSensorEnabled");
    Bind(m, s.GameControllerGetSensorData, "SDL_GameControllerGetSensorData");
    if (!ok) {
        FreeLibrary(m);
        s = Sdl{};
        return false;
    }
    s.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    s.SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    s.SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    s.SetHint("SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0");   // A/B by position (south/east)
    if (s.InitSubSystem(kSdlInitJoystick | kSdlInitGameController | kSdlInitSensor) != 0 &&
        s.InitSubSystem(kSdlInitJoystick | kSdlInitGameController) != 0) {
        FreeLibrary(m);
        s = Sdl{};
        return false;
    }
    s.ok = true;
    return true;
}

void UnloadSdl() {
    if (!g_sdl.ok) return;
    if (g_pad) g_sdl.GameControllerClose(g_pad);
    g_pad = nullptr;
    g_sdl.QuitSubSystem(kSdlInitJoystick | kSdlInitGameController | kSdlInitSensor);
    FreeLibrary(g_sdl.lib);
    g_sdl = Sdl{};
}

// Analog triggers (ZL/ZR) to digital, with hysteresis and debounce: pressed above 50 %,
// released below 30 %, and at most one change every 25 ms. With a single threshold (formerly 37.5 %
// in SDL and 12 % in XInput) trigger noise near the threshold, read at 250 Hz, gave several edges
// per press: the inventory menu switched more than one tab with a single ZL/ZR.
struct TriggerState {
    bool on = false;
    Clock::time_point last{};
};
TriggerState g_trig[2];
bool TriggerDigital(int which, float value01) {
    TriggerState& t = g_trig[which];
    const bool want = t.on ? value01 > 0.30f : value01 > 0.50f;
    if (want != t.on) {
        const auto now = Clock::now();
        if (now - t.last >= std::chrono::milliseconds(25)) {
            t.on = want;
            t.last = now;
        }
    }
    return t.on;
}

bool PollSdl(PadRaw& p, std::string& dev) {
    Sdl& s = g_sdl;
    s.PumpEvents();
    s.GameControllerUpdate();
    if (g_pad && !s.GameControllerGetAttached(g_pad)) {
        s.GameControllerClose(g_pad);
        g_pad = nullptr;
    }
    if (!g_pad) {
        const auto now = Clock::now();
        if (now - g_last_probe < std::chrono::seconds(1)) return false;
        g_last_probe = now;
        for (int i = 0; i < s.NumJoysticks() && !g_pad; ++i)
            if (s.IsGameController(i)) g_pad = s.GameControllerOpen(i);
        if (!g_pad) return false;
        if (g_cfg.motion && s.GameControllerSetSensorEnabled) {
            s.GameControllerSetSensorEnabled(g_pad, kSdlSensorAccel, 1);
            s.GameControllerSetSensorEnabled(g_pad, kSdlSensorGyro, 1);
        }
    }
    const char* name = s.GameControllerName(g_pad);
    dev = std::string("sdl2:") + (name ? name : "?");
    static const int kBtn[PeCount] = {0 /*A*/, 1 /*B*/, 2 /*X*/, 3 /*Y*/, 9, 10, -1, -1, 6 /*START*/, 4 /*BACK*/,
                                      7, 8, 11 /*UP*/, 12 /*DOWN*/, 13 /*LEFT*/, 14 /*RIGHT*/};
    for (int e = 0; e < PeCount; ++e)
        if (kBtn[e] >= 0) p.elem[e] = s.GameControllerGetButton(g_pad, kBtn[e]) != 0;
    p.elem[PeLT] = TriggerDigital(0, s.GameControllerGetAxis(g_pad, 4) / 32767.0f);
    p.elem[PeRT] = TriggerDigital(1, s.GameControllerGetAxis(g_pad, 5) / 32767.0f);
    // SDL: up = negative; nn::hid: up = positive.
    p.lx = s.GameControllerGetAxis(g_pad, 0);
    p.ly = -s.GameControllerGetAxis(g_pad, 1);
    p.rx = s.GameControllerGetAxis(g_pad, 2);
    p.ry = -s.GameControllerGetAxis(g_pad, 3);
    p.connected = true;
    if (g_cfg.motion && s.GameControllerGetSensorData && s.GameControllerHasSensor &&
        s.GameControllerHasSensor(g_pad, kSdlSensorGyro) && s.GameControllerHasSensor(g_pad, kSdlSensorAccel)) {
        float a[3] = {}, w[3] = {};
        if (s.GameControllerGetSensorData(g_pad, kSdlSensorAccel, a, 3) == 0 &&
            s.GameControllerGetSensorData(g_pad, kSdlSensorGyro, w, 3) == 0) {
            // SDL axes (x right, y up, z towards the player) -> Switch: the same
            // conversion suyu uses (input_common/drivers/sdl_driver.cpp).
            constexpr float g = 9.80665f;
            p.accel[0] = -a[0] / g;
            p.accel[1] = a[2] / g;
            p.accel[2] = -a[1] / g;
            p.gyro[0] = w[0] / (2 * kPi);
            p.gyro[1] = -w[2] / (2 * kPi);
            p.gyro[2] = w[1] / (2 * kPi);
            p.has_motion = true;
        }
    }
    return true;
}

bool PollXInput(PadRaw& p, std::string& dev) {
    const auto now = Clock::now();
    if (g_xinput_slot < 0) {
        if (now - g_last_probe < std::chrono::seconds(1)) return false;
        g_last_probe = now;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS) {
                g_xinput_slot = static_cast<int>(i);
                break;
            }
        }
        if (g_xinput_slot < 0) return false;
    }
    XINPUT_STATE st{};
    if (XInputGetState(static_cast<DWORD>(g_xinput_slot), &st) != ERROR_SUCCESS) {
        g_xinput_slot = -1;
        return false;
    }
    dev = "xinput:" + std::to_string(g_xinput_slot);
    const XINPUT_GAMEPAD& x = st.Gamepad;
    const WORD b = x.wButtons;
    p.elem[PeSouth] = b & XINPUT_GAMEPAD_A;
    p.elem[PeEast] = b & XINPUT_GAMEPAD_B;
    p.elem[PeWest] = b & XINPUT_GAMEPAD_X;
    p.elem[PeNorth] = b & XINPUT_GAMEPAD_Y;
    p.elem[PeLB] = b & XINPUT_GAMEPAD_LEFT_SHOULDER;
    p.elem[PeRB] = b & XINPUT_GAMEPAD_RIGHT_SHOULDER;
    p.elem[PeLT] = TriggerDigital(0, x.bLeftTrigger / 255.0f);
    p.elem[PeRT] = TriggerDigital(1, x.bRightTrigger / 255.0f);
    p.elem[PeStart] = b & XINPUT_GAMEPAD_START;
    p.elem[PeBack] = b & XINPUT_GAMEPAD_BACK;
    p.elem[PeLS] = b & XINPUT_GAMEPAD_LEFT_THUMB;
    p.elem[PeRS] = b & XINPUT_GAMEPAD_RIGHT_THUMB;
    p.elem[PeDUp] = b & XINPUT_GAMEPAD_DPAD_UP;
    p.elem[PeDDown] = b & XINPUT_GAMEPAD_DPAD_DOWN;
    p.elem[PeDLeft] = b & XINPUT_GAMEPAD_DPAD_LEFT;
    p.elem[PeDRight] = b & XINPUT_GAMEPAD_DPAD_RIGHT;
    p.lx = x.sThumbLX;   // XInput: up = positive, same as nn::hid
    p.ly = x.sThumbLY;
    p.rx = x.sThumbRX;
    p.ry = x.sThumbRY;
    p.connected = true;
    return true;
}

bool KeyboardAllowed() {
    if (!g_cfg.keyboard_requires_focus) return true;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void PollKeyboard(uint64_t& buttons, int32_t& lx, int32_t& ly, int32_t& rx, int32_t& ry) {
    auto down = [](int i) {
        for (int vk : g_key_map[i])
            if (GetAsyncKeyState(vk) & 0x8000) return true;
        return false;
    };
    for (int i = 0; i < 16; ++i)
        if (down(i)) buttons |= 1ull << i;
    const int klx = (down(18) ? 0x7FFF : 0) - (down(16) ? 0x7FFF : 0);
    const int kly = (down(17) ? 0x7FFF : 0) - (down(19) ? 0x7FFF : 0);
    const int krx = (down(22) ? 0x7FFF : 0) - (down(20) ? 0x7FFF : 0);
    const int kry = (down(21) ? 0x7FFF : 0) - (down(23) ? 0x7FFF : 0);
    if (klx || kly) { lx = klx; ly = kly; }
    if (krx || kry) { rx = krx; ry = kry; }
}

// ---- mouse: cursor captured in the game window -> right stick + buttons -------------------
// The polling thread measures the cursor offset from the window center and moves it back to
// the center (SetCursorPos) on every sample. To hide the cursor and read the wheel, the game
// window is subclassed (WM_SETCURSOR / WM_MOUSEWHEEL); the window belongs to another thread of
// the same process.
struct MouseRt {
    MouseStick stick;
    WheelPulser wheel;
    bool captured = false;
    bool user_released = false;   // released with the key (or without capture_on_focus)
    bool had_focus = false;
    bool toggle_prev = false;
    bool lbutton_prev = false;
    bool swallow = false;         // ignore buttons until released (the click that captured)
    HWND hwnd = nullptr;
    int vk_release = 0, vk_hold = 0;
    uint64_t btn_left = 0, btn_right = 0, btn_middle = 0, btn_x1 = 0, btn_x2 = 0, wheel_up = 0, wheel_down = 0;
    // Mouse in menus (MouseConfig::menu_*).
    MenuMouse menu;
    WheelPulser menu_wheel;
    bool menu_prev = false;
    Clock::time_point virt_tail{};   // virtual mouse: keep processing 1 s after the last command (release, presses)
    uint64_t menu_left = 0, menu_right = 0, menu_middle = 0, menu_wheel_up = 0, menu_wheel_down = 0;
} g_mouse;
std::atomic<WNDPROC> g_mouse_orig_proc{nullptr};
std::atomic<HWND> g_mouse_sub_hwnd{nullptr};
std::atomic<bool> g_mouse_hide{false};
std::atomic<bool> g_ptr_arrow{false};   // mouse pointer in the game UI: show the arrow (PointerMouse)
std::atomic<int> g_mouse_wheel_accum{0};
std::atomic<bool> g_mouse_clipped{false};   // to release the cursor if the process ends abruptly

LRESULT CALLBACK MouseSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC orig = g_mouse_orig_proc.load();
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_ptr_arrow.load()) {   // mouse pointer in the game UI
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_mouse_hide.load()) {
        SetCursor(nullptr);
        return TRUE;
    }
    if (m == WM_MOUSEWHEEL) g_mouse_wheel_accum.fetch_add(GET_WHEEL_DELTA_WPARAM(w));
    if (m == WM_NCDESTROY && g_mouse_sub_hwnd.load() == h) {
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        g_mouse_sub_hwnd = nullptr;
    }
    return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void MouseUnsubclass() {
    HWND h = g_mouse_sub_hwnd.exchange(nullptr);
    if (h && IsWindow(h) &&
        GetWindowLongPtrW(h, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&MouseSubclassProc))
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_mouse_orig_proc.load()));
    // g_mouse_orig_proc is kept: a message may still be in flight inside MouseSubclassProc.
}

void MouseSubclass(HWND h) {
    if (g_mouse_sub_hwnd.load() == h) return;
    MouseUnsubclass();
    const LONG_PTR cur = GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!cur || cur == reinterpret_cast<LONG_PTR>(&MouseSubclassProc)) return;
    g_mouse_orig_proc = reinterpret_cast<WNDPROC>(cur);   // before installing: the window thread may enter right away
    if (SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MouseSubclassProc))) g_mouse_sub_hwnd = h;
}

// Game window in the foreground: of this process, unowned, not a console or dialog box, not minimized.
HWND GameForegroundWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId() || fg == GetConsoleWindow() || GetWindow(fg, GW_OWNER) || IsIconic(fg)) return nullptr;
    char cls[64] = {};
    GetClassNameA(fg, cls, sizeof cls);
    if (!std::strcmp(cls, "#32770") || !std::strcmp(cls, "ConsoleWindowClass")) return nullptr;
    RECT rc{};
    if (!GetClientRect(fg, &rc) || rc.right - rc.left < 64 || rc.bottom - rc.top < 64) return nullptr;
    return fg;
}

bool ClientCenter(HWND h, POINT& center, RECT& screen) {
    RECT rc{};
    if (!GetClientRect(h, &rc)) return false;
    POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
    ClientToScreen(h, &tl);
    ClientToScreen(h, &br);
    screen = RECT{tl.x, tl.y, br.x, br.y};
    center = POINT{(tl.x + br.x) / 2, (tl.y + br.y) / 2};
    return br.x > tl.x && br.y > tl.y;
}

void MouseRelease() {
    MouseRt& m = g_mouse;
    if (!m.captured) return;
    m.captured = false;
    g_mouse_hide = false;
    if (g_mouse_clipped.exchange(false)) ClipCursor(nullptr);
    m.stick.Reset();
    m.wheel.Reset();
}

// ---- nvnvk overlay menu (docs/port/IN-GAME-MENU.md) -------------------------------------------
// nvnvk.dll exports `int nvnvk_overlay_active(void)`: bit 0 = the menu or the start screen is open
// and the game must not receive input. It is looked up by name once per second until found (nvnvk
// may load after the nnshim) and the module is pinned (never unloaded: the pointer stays valid even
// while the process is shutting down). Without nvnvk (suyu renderer) nothing changes.
using OverlayFn = int (*)();
std::atomic<OverlayFn> g_overlay_fn{nullptr};
Clock::time_point g_overlay_probe{};
bool g_overlay_prev = false;
bool g_kb_swallow = false;    // after closing the menu: ignore the keyboard until all keys are released
bool g_pad_swallow = false;   // same for the controller buttons

int OverlayState() {
    OverlayFn f = g_overlay_fn.load(std::memory_order_relaxed);
    if (!f) {
        const auto now = Clock::now();
        if (now - g_overlay_probe < std::chrono::seconds(1)) return 0;
        g_overlay_probe = now;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvnvk.dll", &mod) || !mod) return 0;
        f = reinterpret_cast<OverlayFn>(GetProcAddress(mod, "nvnvk_overlay_active"));
        if (!f) return 0;
        g_overlay_fn = f;
    }
    return f();
}

void MouseCapture(HWND h) {
    MouseRt& m = g_mouse;
    POINT c{};
    RECT r{};
    if (!ClientCenter(h, c, r)) return;
    if (g_cfg.mouse.hide_cursor) MouseSubclass(h);
    else if (g_cfg.mouse.wheel_up.size() + g_cfg.mouse.wheel_down.size() ||
             (g_cfg.mouse.menu_enabled && g_cfg.mouse.menu_wheel_up.size() + g_cfg.mouse.menu_wheel_down.size()))
        MouseSubclass(h);   // wheel
    m.captured = true;
    m.hwnd = h;
    m.swallow = true;
    m.stick.Reset();
    m.wheel.Reset();
    g_mouse_wheel_accum = 0;
    g_mouse_hide = g_cfg.mouse.hide_cursor;
    SetCursorPos(c.x, c.y);
    std::lock_guard lk(g_mutex);
    g_info.mouse_captures++;
}

bool MouseActiveMode(bool pad_connected) {
    const std::string& e = g_cfg.mouse.enabled;
    if (!_stricmp(e.c_str(), "on") || !_stricmp(e.c_str(), "true") || e == "1") return true;
    if (!_stricmp(e.c_str(), "auto")) return !pad_connected;
    return false;
}

// Button and key bindings of the mouse (MouseSetup, and live reloads of controls.json without losing the capture).
void MouseBindings() {
    MouseRt& m = g_mouse;
    const MouseConfig& c = g_cfg.mouse;
    m.vk_release = c.release_key.empty() ? 0 : VkFromName(c.release_key);
    m.vk_hold = c.hold_release_key.empty() ? 0 : VkFromName(c.hold_release_key);
    m.btn_left = ButtonsFromList(c.left);
    m.btn_right = ButtonsFromList(c.right);
    m.btn_middle = ButtonsFromList(c.middle);
    m.btn_x1 = ButtonsFromList(c.x1);
    m.btn_x2 = ButtonsFromList(c.x2);
    m.wheel_up = ButtonsFromList(c.wheel_up);
    m.wheel_down = ButtonsFromList(c.wheel_down);
    m.menu_left = ButtonsFromList(c.menu_left);
    m.menu_right = ButtonsFromList(c.menu_right);
    m.menu_middle = ButtonsFromList(c.menu_middle);
    m.menu_wheel_up = ButtonsFromList(c.menu_wheel_up);
    m.menu_wheel_down = ButtonsFromList(c.menu_wheel_down);
}

void MouseSetup() {
    g_mouse = MouseRt{};
    MouseBindings();
}

bool KeyDown(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000); }

// ---- mouse pointer in the game UI (abi_pointer.cpp, docs/port/POINTER.md) -----------------------------------
// In menu mode the cursor is free and visible: hovering an element of the game's UI highlights it (d-pad presses
// decided by ptr::HostPoll) and the left click accepts it. Right click = B and the wheel work as in the menu mouse;
// a left press that the pointer does not own (it does not start on an element, e.g. on the map) is the legacy
// menu mouse (A on release, drag = left stick).
struct PointerRt {
    bool mode = false;
    POINT last{};
    bool have_last = false;
} g_ptr_rt;

void PointerMouse(HWND fg, bool entered, int64_t dt_ns, const bool vb[3], int vwheel, uint64_t& buttons, int32_t& lx,
                  int32_t& ly) {
    MouseRt& m = g_mouse;
    const MouseConfig& c = g_cfg.mouse;
    const float dt_ms = static_cast<float>(dt_ns) * 1e-6f;
    ptr::HostSample hs;
    hs.dt_ms = dt_ms;
    bool pl = false, pr = false, pm = false;
    float dx = 0, dy = 0;
    if (fg) {
        POINT p{};
        RECT rc{};
        if (GetCursorPos(&p) && GetClientRect(fg, &rc) && rc.right > 0 && rc.bottom > 0) {
            POINT cp = p;
            ScreenToClient(fg, &cp);
            if (cp.x >= 0 && cp.y >= 0 && cp.x < rc.right && cp.y < rc.bottom) {
                hs.has_pos = true;
                hs.x = (static_cast<float>(cp.x) + 0.5f) / static_cast<float>(rc.right);
                hs.y = (static_cast<float>(cp.y) + 0.5f) / static_cast<float>(rc.bottom);
                const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
                pl = KeyDown(swap ? VK_RBUTTON : VK_LBUTTON);
                pr = KeyDown(swap ? VK_LBUTTON : VK_RBUTTON);
                pm = KeyDown(VK_MBUTTON);
            }
            if (g_ptr_rt.have_last) {
                dx = static_cast<float>(p.x - g_ptr_rt.last.x);
                dy = static_cast<float>(p.y - g_ptr_rt.last.y);
            }
            g_ptr_rt.last = p;
            g_ptr_rt.have_last = true;
        }
    } else g_ptr_rt.have_last = false;
    float vx = 0, vy = 0;
    if (ptr::VirtualPos(vx, vy)) {   // test pointer (UDP "ptr x y")
        hs.has_pos = true;
        hs.x = vx;
        hs.y = vy;
    }
    bool bl = pl || vb[0], br = pr || vb[1], bm = pm || vb[2];
    if (entered) {
        m.menu.Reset();
        m.menu_wheel.Reset();
        ptr::HostReset();
        if (bl || br || bm) m.swallow = true;   // what opened the menu does not count until released
    }
    if (m.swallow) {
        if (!bl && !br && !bm) m.swallow = false;
        else bl = br = bm = false;
    }
    m.menu_prev = true;   // leaving the menu goes through the usual mode change (held buttons swallowed)
    hs.left = bl;
    hs.right = br;
    ptr::HostResult hr;
    ptr::HostPoll(hs, hr);
    buttons |= hr.buttons;
    int32_t mlx = 0, mly = 0;
    MouseConfig mc = c;   // a plain click (dialogs, screens without detected elements) = A as long as the pointer's
    mc.menu_click_ms = std::max(c.menu_click_ms, ptr::AcceptMs());
    m.menu.Update(mc, bl && !hr.owns_left, hr.right, bm, dx, dy, dt_ms, m.menu_left, m.menu_right, m.menu_middle, buttons,
                  mlx, mly);
    if (mlx || mly) {
        lx = mlx;
        ly = mly;
    }
    if (hr.stick) {   // stick cursor (map): the pointer drives the left stick towards the click
        lx = hr.lx;
        ly = hr.ly;
    }
    int notches = vwheel;
    const int wheel = g_mouse_wheel_accum.exchange(0);
    const int wn = wheel / WHEEL_DELTA;
    if (wn) g_mouse_wheel_accum.fetch_add(wheel - wn * WHEEL_DELTA);
    else if (wheel) g_mouse_wheel_accum.fetch_add(wheel);
    notches += wn;
    if (notches) m.menu_wheel.AddNotches(notches);
    buttons |= m.menu_wheel.Step(dt_ms, m.menu_wheel_up, m.menu_wheel_down, c.menu_wheel_pulse_ms);
    ptr::TraceButtons(buttons, lx, ly);
}

// One mouse sample. Adds buttons and sets the right stick (if the mouse moves) over the controller's.
void PollMouse(bool pad_connected, int64_t dt_ns, uint64_t& buttons, int32_t& lx, int32_t& ly, int32_t& rx,
               int32_t& ry) {
    MouseRt& m = g_mouse;
    const MouseConfig& c = g_cfg.mouse;
    const bool active = MouseActiveMode(pad_connected);
    HWND fg = active ? GameForegroundWindow() : nullptr;
    const bool lbutton = KeyDown(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON);
    if (fg && !m.had_focus) m.user_released = !c.capture_on_focus;   // focus (re)gained
    m.had_focus = fg != nullptr;
    const bool toggle = KeyDown(m.vk_release);
    if (fg && toggle && !m.toggle_prev) m.user_released = m.captured;   // F1: toggles
    m.toggle_prev = toggle;
    if (fg && m.user_released && !m.captured && c.click_to_capture && lbutton && !m.lbutton_prev) {
        POINT p{}, ctr{};
        RECT r{};
        if (GetCursorPos(&p) && ClientCenter(fg, ctr, r) && PtInRect(&r, p)) m.user_released = false;
    }
    m.lbutton_prev = lbutton;
    // Mouse pointer in the game UI: in menus the cursor is free (see PointerMouse).
    float ptr_vx = 0, ptr_vy = 0;
    const bool ptr_mode = (fg || ptr::VirtualPos(ptr_vx, ptr_vy)) && c.menu_enabled && MenuActive() && ptr::Enabled();
    const bool ptr_entered = ptr_mode && !g_ptr_rt.mode;
    g_ptr_rt.mode = ptr_mode;
    g_ptr_arrow = ptr_mode;
    if (ptr_mode && fg) MouseSubclass(fg);   // arrow cursor and wheel
    const bool want = fg && !m.user_released && !KeyDown(m.vk_hold) && !ptr_mode;
    if (m.captured && (!want || fg != m.hwnd)) MouseRelease();
    if (want && !m.captured) MouseCapture(fg);
    // Virtual mouse (UDP tests): adds to the physical one and works without a captured cursor.
    bool vb[3] = {false, false, false};
    float vdx = 0, vdy = 0;
    int vwheel = 0;
    {
        const auto now = Clock::now();
        std::lock_guard lk(g_mutex);
        for (int i = 0; i < 3; ++i) vb[i] = now < g_vmouse.btn_until[i];
        if (now < g_vmouse.move_until) {
            const float ms = static_cast<float>(dt_ns) * 1e-6f;
            vdx = g_vmouse.vx * ms;
            vdy = g_vmouse.vy * ms;
        }
        vwheel = g_vmouse.wheel;
        g_vmouse.wheel = 0;
    }
    if (ptr_mode) {
        PointerMouse(fg, ptr_entered, dt_ns, vb, vwheel, buttons, lx, ly);
        return;
    }
    const bool virt = vb[0] || vb[1] || vb[2] || vdx != 0 || vdy != 0 || vwheel != 0;
    float dx = vdx, dy = vdy;
    bool bl = vb[0], br = vb[1], bm = vb[2], b4 = false, b5 = false;
    int notches = vwheel;
    if (m.captured) {
        POINT ctr{}, p{};
        RECT r{};
        if (!ClientCenter(m.hwnd, ctr, r) || !GetCursorPos(&p)) {
            MouseRelease();
            return;
        }
        if (c.clip_cursor) {
            ClipCursor(&r);
            g_mouse_clipped = true;
        }   // every sample: the window may move or resize
        dx += static_cast<float>(p.x - ctr.x);
        dy += static_cast<float>(p.y - ctr.y);
        if (p.x != ctr.x || p.y != ctr.y) SetCursorPos(ctr.x, ctr.y);
        const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        const bool pl = KeyDown(swap ? VK_RBUTTON : VK_LBUTTON), pr = KeyDown(swap ? VK_LBUTTON : VK_RBUTTON);
        const bool pm = KeyDown(VK_MBUTTON);
        b4 = KeyDown(VK_XBUTTON1);
        b5 = KeyDown(VK_XBUTTON2);
        if (m.swallow && !pl && !pr && !pm && !b4 && !b5) m.swallow = false;
        bl = bl || pl;
        br = br || pr;
        bm = bm || pm;
        const int wheel = g_mouse_wheel_accum.exchange(0);
        const int wn = wheel / WHEEL_DELTA;
        if (wn) g_mouse_wheel_accum.fetch_add(wheel - wn * WHEEL_DELTA);   // remainder (high-resolution wheels)
        else if (wheel) g_mouse_wheel_accum.fetch_add(wheel);
        notches += wn;
    } else {
        g_mouse_wheel_accum = 0;
        const auto now = Clock::now();
        if (virt) m.virt_tail = now + std::chrono::seconds(1);
        if (!virt && now >= m.virt_tail) {
            m.swallow = false;
            m.menu_prev = c.menu_enabled && MenuActive();   // no samples: the mode change does not "swallow" the next click
            return;
        }
        if (m.swallow && !bl && !br && !bm) m.swallow = false;
    }
    const float dt_ms = static_cast<float>(dt_ns) * 1e-6f;
    const bool menu = c.menu_enabled && MenuActive();
    if (menu != m.menu_prev) {
        // On a mode change, buttons still held do not count until released (the click that closed
        // a menu must not throw a ball, nor the one that opened a menu pick something).
        if (bl || br || bm || b4 || b5) m.swallow = true;
        m.stick.Reset();
        m.wheel.Reset();
        m.menu.Reset();
        m.menu_wheel.Reset();
        m.menu_prev = menu;
    }
    const bool on = !m.swallow;
    uint64_t new_clicks = 0, drag_samples = 0;
    if (menu) {
        int32_t mlx = 0, mly = 0;
        const uint64_t clicks_before = m.menu.Clicks();
        m.menu.Update(c, bl && on, br && on, bm && on, dx, dy, dt_ms, m.menu_left, m.menu_right, m.menu_middle, buttons,
                      mlx, mly);
        new_clicks = m.menu.Clicks() - clicks_before;
        if (mlx || mly) {
            ++drag_samples;
            lx = mlx;
            ly = mly;
        }
        if (notches) m.menu_wheel.AddNotches(notches);
        buttons |= m.menu_wheel.Step(dt_ms, m.menu_wheel_up, m.menu_wheel_down, c.menu_wheel_pulse_ms);
    } else {
        int32_t mx = 0, my = 0;
        m.stick.Update(c, dx, dy, static_cast<float>(dt_ns) * 1e-9f, mx, my);
        if (mx || my) {
            rx = Clamp16(rx + mx);
            ry = Clamp16(ry + my);
        }
        if (on) {
            if (bl) buttons |= m.btn_left;
            if (br) buttons |= m.btn_right;
            if (bm) buttons |= m.btn_middle;
            if (b4) buttons |= m.btn_x1;
            if (b5) buttons |= m.btn_x2;
        }
        if (notches) m.wheel.AddNotches(notches);
        buttons |= m.wheel.Step(dt_ms, m.wheel_up, m.wheel_down, c.wheel_pulse_ms);
    }
    std::lock_guard lk(g_mutex);
    g_info.mouse_samples++;
    if (menu) {
        g_info.menu_mouse_samples++;
        g_info.menu_clicks += new_clicks;
        g_info.menu_drag_samples += drag_samples;
    }
    if (notches) g_info.mouse_wheel_notches += static_cast<uint64_t>(std::abs(notches));
}

void MouseShutdown() {
    MouseRelease();
    MouseUnsubclass();
}

void OpenRemote() {
    if (g_cfg.remote_port <= 0) return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(g_cfg.remote_port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(g_sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return;
    }
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
}

void CloseRemote() {
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
    }
}
#endif

bool ParseRemoteLocked(const std::string& text);

#ifdef _WIN32
void PollRemote() {
    if (g_sock == INVALID_SOCKET) return;
    char buf[4096];
    for (;;) {
        const int n = recv(g_sock, buf, sizeof buf - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        std::lock_guard lk(g_mutex);
        ParseRemoteLocked(buf);
    }
}
#endif

// ---- motion ------------------------------------------------------------------------------
void QuatNormalize(float q[4]) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0) for (int i = 0; i < 4; ++i) q[i] /= n;
}

void IntegrateMotionLocked(const PadRaw& p, float dt, bool fusion, float power) {
    // Angular velocity in rad/s.
    float w[3] = {p.gyro[0] * 2 * kPi, p.gyro[1] * 2 * kPi, p.gyro[2] * 2 * kPi};
    if (fusion && p.has_motion) {
        // Complementary filter: corrects the tilt towards the measured gravity.
        const float a[3] = {p.accel[0], p.accel[1], p.accel[2]};
        const float an = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (an > 0.5f && an < 1.5f) {
            const float* q = g_quat;
            // Expected gravity in controller axes (q* (0,0,-1) q).
            const float gx = -2 * (q[1] * q[3] - q[0] * q[2]);
            const float gy = -2 * (q[0] * q[1] + q[2] * q[3]);
            const float gz = -(q[0] * q[0] - q[1] * q[1] - q[2] * q[2] + q[3] * q[3]);
            const float ex = (a[1] * gz - a[2] * gy) / an, ey = (a[2] * gx - a[0] * gz) / an, ez = (a[0] * gy - a[1] * gx) / an;
            const float k = std::clamp(power, 0.0f, 1.0f) * 10.0f;
            w[0] += k * ex;
            w[1] += k * ey;
            w[2] += k * ez;
        }
    }
    float* q = g_quat;
    const float dq[4] = {0.5f * (-q[1] * w[0] - q[2] * w[1] - q[3] * w[2]), 0.5f * (q[0] * w[0] + q[2] * w[2] - q[3] * w[1]),
                         0.5f * (q[0] * w[1] - q[1] * w[2] + q[3] * w[0]), 0.5f * (q[0] * w[2] + q[1] * w[1] - q[2] * w[0])};
    for (int i = 0; i < 4; ++i) q[i] += dq[i] * dt;
    QuatNormalize(q);
    for (int i = 0; i < 3; ++i) g_angle[i] += p.gyro[i] * dt;
}

void PushMotionLocked(const PadRaw& p, int64_t dt_ns) {
    SixAxisState s{};
    s.delta_time = dt_ns;
    s.sampling_number = g_sampling.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        s.accel[i] = p.accel[i];
        s.gyro[i] = p.gyro[i];
        s.angle[i] = g_angle[i];
    }
    const float w = g_quat[0], x = g_quat[1], y = g_quat[2], z = g_quat[3];
    const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                           {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                           {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    // Rows = controller axes expressed in world space (like nn::hid's direction.x/y/z).
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) s.direction[r][c] = m[c][r];
    s.attributes = 1;   // IsConnected
    g_motion_head = (g_motion_head + 1) % 32;
    g_motion_hist[g_motion_head] = s;
    g_motion_count = std::min(g_motion_count + 1, 32);
}

// ---- rumble ------------------------------------------------------------------------------
void ApplyRumble() {
    float lo = 0, hi = 0;
    const auto now = Clock::now();
    {
        std::lock_guard lk(g_mutex);
        for (int d = 0; d < 2; ++d) {
            if (now - g_vib_time[d] > std::chrono::milliseconds(100)) continue;   // stale value
            lo = std::max(lo, g_vib[d].amp_low);
            hi = std::max(hi, g_vib[d].amp_high);
        }
        if (!g_vib_dirty && lo == 0 && hi == 0) return;
        g_vib_dirty = lo != 0 || hi != 0;
    }
    const float k = std::clamp(g_cfg.rumble_strength, 0.0f, 2.0f);
    const uint16_t ulo = static_cast<uint16_t>(std::clamp(lo * k, 0.0f, 1.0f) * 65535);
    const uint16_t uhi = static_cast<uint16_t>(std::clamp(hi * k, 0.0f, 1.0f) * 65535);
#ifdef _WIN32
    if (g_pad && g_sdl.GameControllerRumble) {
        g_sdl.GameControllerRumble(g_pad, ulo, uhi, 120);
    } else if (g_xinput_slot >= 0) {
        XINPUT_VIBRATION v{ulo, uhi};   // left motor = low frequency, right = high
        XInputSetState(static_cast<DWORD>(g_xinput_slot), &v);
    }
#endif
}

void PollOnce(int64_t dt_ns) {
    PadRaw pad;
    std::string dev = "none";
    const std::string& b = g_cfg.backend;
#ifdef _WIN32
    if (g_sdl.ok && (b == "auto" || b == "sdl2")) PollSdl(pad, dev);
    if (!pad.connected && (b == "auto" || b == "xinput")) PollXInput(pad, dev);
    PollRemote();
#endif
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    g_pad_connected.store(pad.connected, std::memory_order_relaxed);
    if (pad.connected) {
        for (int i = 0; i < 16; ++i)
            for (int e : g_pad_map[i])
                if (pad.elem[e]) buttons |= 1ull << i;
        lx = Clamp16(Dead(pad.lx, g_cfg.deadzone));
        ly = Clamp16(Dead(pad.ly, g_cfg.deadzone));
        rx = Clamp16(Dead(pad.rx, g_cfg.deadzone));
        ry = Clamp16(Dead(pad.ry, g_cfg.deadzone));
    }
#ifdef _WIN32
    // Holding Minus + Plus on the controller opens the nvnvk menu: the game does not see that combo.
    if ((buttons & BtnPlus) && (buttons & BtnMinus)) buttons &= ~(uint64_t(BtnPlus) | uint64_t(BtnMinus));
    const bool overlay = (OverlayState() & 1) != 0;
    if (overlay) {
        // Menu open: the game receives nothing from the devices; the cursor is free and visible.
        buttons = 0;
        lx = ly = rx = ry = 0;
        MouseRelease();
        g_mouse_wheel_accum = 0;
        g_mouse.swallow = true;   // the click that closes the menu does not reach the game
        g_kb_swallow = g_pad_swallow = true;
    } else {
        if (g_overlay_prev) {
            g_kb_swallow = g_pad_swallow = true;
            g_mouse.had_focus = false;   // recaptured as on regaining focus
        }
        if (g_pad_swallow) {
            if (buttons == 0) g_pad_swallow = false;
            else buttons = 0;
        }
        if (g_cfg.keyboard && (b == "auto" || b == "keyboard" || b == "xinput" || b == "sdl2") && KeyboardAllowed()) {
            uint64_t kb = 0;
            int32_t klx = lx, kly = ly, krx = rx, kry = ry;
            PollKeyboard(kb, klx, kly, krx, kry);
            const bool any = kb != 0 || klx != lx || kly != ly || krx != rx || kry != ry;
            if (g_kb_swallow && !any) g_kb_swallow = false;
            if (!g_kb_swallow) {
                buttons |= kb;
                lx = klx;
                ly = kly;
                rx = krx;
                ry = kry;
            }
            if (dev == "none") dev = "keyboard";
        }
        if (b != "none") PollMouse(pad.connected, dt_ns, buttons, lx, ly, rx, ry);
    }
    const bool overlay_opened = overlay && !g_overlay_prev;
    g_overlay_prev = overlay;
#endif
    const auto now = Clock::now();
    std::lock_guard lk(g_mutex);
    // Remote commands (queue) and test injection: added to the device state.
    while (!g_steps.empty() && now - g_step_start >= std::chrono::milliseconds(g_steps.front().ms)) {
        g_steps.pop_front();
        g_step_start = now;
    }
    if (!g_steps.empty()) {
        const Step& s = g_steps.front();
        buttons |= s.buttons;
        if (s.sticks) { lx = s.lx; ly = s.ly; rx = s.rx; ry = s.ry; }
    }
    if (now < g_inject_until) {
        buttons |= g_inject.buttons;
        if (g_inject.sticks) { lx = g_inject.lx; ly = g_inject.ly; rx = g_inject.rx; ry = g_inject.ry; }
    }
    buttons |= StickButtons(lx, ly, rx, ry);
    g_snap = Snapshot{buttons, lx, ly, rx, ry};
    g_sampling.fetch_add(1, std::memory_order_relaxed);
    g_info.device = dev;
    g_info.samples++;
#ifdef _WIN32
    if (g_overlay_prev) g_info.overlay_samples++;
    if (overlay_opened) g_info.overlay_opens++;
#endif
#ifdef _WIN32
    g_info.mouse_captured = g_mouse.captured;
#endif
    g_has_motion = pad.has_motion;
    g_info.has_motion = pad.has_motion;
    // Motion: with sensors, integrate; without them, controller at rest (like suyu without motion).
    bool fusion = true;
    float power = 0.03f;
    if (!g_motion_cfg.empty()) {
        fusion = g_motion_cfg.begin()->second.fusion;
        power = g_motion_cfg.begin()->second.revise_power;
    }
    IntegrateMotionLocked(pad, static_cast<float>(dt_ns) * 1e-9f, fusion, power);
    PushMotionLocked(pad, dt_ns);
}

// ---- live reload of the controls (controls.json changed: in-game menu of nvnvk or launcher) ----------------
// Reconfigure() leaves the new config here; the polling thread swaps it in between two samples, so the thread never
// reads g_cfg while it changes. Settings that need restarting (backend, SDL, motion, UDP port, poll rate) are kept.
std::atomic<bool> g_reload_pending{false};
Config g_reload_cfg;   // under g_mutex

void ApplyPendingReload() {
    if (!g_reload_pending.exchange(false)) return;
    std::lock_guard lk(g_mutex);
    Config c = g_reload_cfg;
    c.backend = g_cfg.backend;
    c.sdl_library = g_cfg.sdl_library;
    c.motion = g_cfg.motion;
    c.remote_port = g_cfg.remote_port;
    c.poll_hz = g_cfg.poll_hz;
    c.mirror_handheld = g_cfg.mirror_handheld;
    g_cfg = std::move(c);
    BuildMaps();
#ifdef _WIN32
    MouseBindings();
    g_kb_swallow = true;   // keys held while the bindings change do not reach the game until released
#endif
}

void ThreadMain() {
#ifdef _WIN32
    if (g_cfg.backend == "auto" || g_cfg.backend == "sdl2") {
        const bool ok = LoadSdl();   // outside the mutex: SDL_Init can be slow (enumerates HID)
        std::lock_guard lk(g_mutex);
        g_info.sdl_loaded = ok;
    }
    OpenRemote();
#endif
    const int hz = std::clamp(g_cfg.poll_hz, 30, 1000);
    const auto period = std::chrono::microseconds(1000000 / hz);
    auto last = Clock::now();
    while (!g_stop.load()) {
        const auto now = Clock::now();
        const int64_t dt = std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count();
        last = now;
        ApplyPendingReload();
        PollOnce(dt);
        if (g_cfg.rumble) ApplyRumble();
        std::this_thread::sleep_until(now + period);
    }
#ifdef _WIN32
    if (g_xinput_slot >= 0) {
        XINPUT_VIBRATION v{0, 0};
        XInputSetState(static_cast<DWORD>(g_xinput_slot), &v);
    }
    MouseShutdown();
    UnloadSdl();
    CloseRemote();
#endif
}

uint64_t ParseButtons(const std::string& s) {
    if (s.empty()) return 0;
    if (s[0] == '0' && s.size() > 1 && (s[1] == 'x' || s[1] == 'X')) return std::strtoull(s.c_str(), nullptr, 16);
    if (std::isdigit(static_cast<unsigned char>(s[0]))) return std::strtoull(s.c_str(), nullptr, 10);
    uint64_t m = 0;
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '+' || s[i] == '|') {
            m |= ButtonFromName(cur);
            cur.clear();
        } else cur.push_back(s[i]);
    }
    return m;
}

// Commands (separated by ';' or newlines; executed in order):
//   press <buttons> <ms>        buttons: "A", "A+ZR", "0x1"
//   stick <L|R> <x> <y> <ms>    x, y in ±32767 (up = positive)
//   state <buttons> <lx> <ly> <rx> <ry> <ms>
//   wait <ms>                    nothing pressed
//   clear                        empties the queue
bool ParseRemoteLocked(const std::string& text) {
    std::string norm = text;
    for (auto& c : norm)
        if (c == ';' || c == '\r') c = '\n';
    std::istringstream lines(norm);
    std::string line;
    bool any = false;
    while (std::getline(lines, line)) {
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        Step st;
        if (cmd == "clear") { g_steps.clear(); any = true; continue; }
        // Virtual mouse (immediate, not queued): mouse <left|right|middle> <ms>, wheel <n>,
        // mousemove <dx> <dy> <ms> (px spread over ms).
        if (cmd == "mouse") {
            std::string which;
            int ms = 0;
            ss >> which >> ms;
            const int i = which == "right" ? 1 : which == "middle" ? 2 : 0;
            g_vmouse.btn_until[i] = Clock::now() + std::chrono::milliseconds(std::max(ms, 1));
            g_info.remote_commands++;
            any = true;
            continue;
        }
        if (cmd == "fakepad") {   // tests of the live icon switch (only affects PadConnected)
            int on = 0;
            ss >> on;
            g_fake_pad = on != 0;
            g_info.remote_commands++;
            any = true;
            continue;
        }
        if (cmd == "wheel") {
            int n = 0;
            ss >> n;
            g_vmouse.wheel += n;
            g_info.remote_commands++;
            any = true;
            continue;
        }
        if (cmd == "mousemove") {
            float dx = 0, dy = 0;
            int ms = 0;
            ss >> dx >> dy >> ms;
            ms = std::max(ms, 1);
            g_vmouse.vx = dx / static_cast<float>(ms);
            g_vmouse.vy = dy / static_cast<float>(ms);
            g_vmouse.move_until = Clock::now() + std::chrono::milliseconds(ms);
            g_info.remote_commands++;
            any = true;
            continue;
        }
        if (cmd == "ptr") {   // mouse pointer in the game UI (tests): ptr <x> <y> (normalized) | ptr off
            std::string rest;
            std::getline(ss, rest);
            ptr::RemoteCommand(cmd, rest);
            g_info.remote_commands++;
            any = true;
            continue;
        }
        if (cmd == "press") {
            std::string btn;
            ss >> btn >> st.ms;
            st.buttons = ParseButtons(btn);
        } else if (cmd == "stick") {
            std::string which;
            int x = 0, y = 0;
            ss >> which >> x >> y >> st.ms;
            st.sticks = true;
            if (which == "R" || which == "r") { st.rx = Clamp16(x); st.ry = Clamp16(y); }
            else { st.lx = Clamp16(x); st.ly = Clamp16(y); }
        } else if (cmd == "state") {
            std::string btn;
            ss >> btn >> st.lx >> st.ly >> st.rx >> st.ry >> st.ms;
            st.buttons = ParseButtons(btn);
            st.sticks = true;
        } else if (cmd == "wait") {
            ss >> st.ms;
        } else {
            continue;
        }
        if (st.ms <= 0) st.ms = 1;
        if (g_steps.empty()) g_step_start = Clock::now();
        g_steps.push_back(st);
        g_info.remote_commands++;
        any = true;
    }
    return any;
}

uint32_t MakeHandle(uint32_t style_index, uint32_t npad, uint32_t dev) { return style_index | (npad << 8) | (dev << 16); }

} // namespace

uint64_t ButtonFromName(const std::string& name) {
    const int i = SwitchIndex(name);
    return i >= 0 ? (1ull << i) : 0;
}

bool PadConnected() {
    return g_pad_connected.load(std::memory_order_relaxed) || g_fake_pad.load(std::memory_order_relaxed);
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void SetMenuState(bool on) { g_menu_state.store(on, std::memory_order_relaxed); }

bool MenuActive() { return g_menu_state.load(std::memory_order_relaxed); }

void Initialize(const Config& cfg) {
    Finalize();
    {
        std::lock_guard lk(g_mutex);
        g_cfg = cfg;
        BuildMaps();
        g_info = Info{};
        g_steps.clear();
        g_quat[0] = 1; g_quat[1] = g_quat[2] = g_quat[3] = 0;
        g_angle[0] = g_angle[1] = g_angle[2] = 0;
        g_motion_count = 0;
        g_motion_cfg.clear();
        g_initialized = true;
    }
#ifdef _WIN32
    MouseSetup();   // before the thread: only the polling thread uses it
#endif
    if (cfg.backend != "none") {
        g_stop = false;
        g_thread = std::thread(ThreadMain);
    }
}

void Reconfigure(const Config& cfg) {
    {
        std::lock_guard lk(g_mutex);
        g_reload_cfg = cfg;
    }
    g_reload_pending = true;
}

void Finalize() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard lk(g_mutex);
    g_initialized = false;
}

uint32_t GetStyleSet(uint32_t npad_id) {
    std::lock_guard lk(g_mutex);
    if (npad_id == NpadIdNo1) return StyleFullKey;
    if (npad_id == NpadIdHandheld && g_cfg.mirror_handheld) return StyleHandheld;
    return 0;
}

void GetState(NpadStyle style, uint32_t npad_id, NpadState& out) {
    std::lock_guard lk(g_mutex);
    out = NpadState{};
    out.sampling_number = g_sampling.load(std::memory_order_relaxed);
    const bool connected =
        (npad_id == NpadIdNo1 && style == StyleFullKey) ||
        (npad_id == NpadIdHandheld && style == StyleHandheld && g_cfg.mirror_handheld);
    if (!connected) return;
    // No polling thread (backend "none" or tests): only the injection is applied.
    if (!g_thread.joinable()) {
        const auto now = Clock::now();
        Snapshot s{};
        while (!g_steps.empty() && now - g_step_start >= std::chrono::milliseconds(g_steps.front().ms)) {
            g_steps.pop_front();
            g_step_start = now;
        }
        if (!g_steps.empty()) {
            s.buttons |= g_steps.front().buttons;
            if (g_steps.front().sticks) { s.lx = g_steps.front().lx; s.ly = g_steps.front().ly; s.rx = g_steps.front().rx; s.ry = g_steps.front().ry; }
        }
        if (now < g_inject_until) {
            s.buttons |= g_inject.buttons;
            if (g_inject.sticks) { s.lx = g_inject.lx; s.ly = g_inject.ly; s.rx = g_inject.rx; s.ry = g_inject.ry; }
        }
        s.buttons |= StickButtons(s.lx, s.ly, s.rx, s.ry);
        g_snap = s;
        g_sampling.fetch_add(1, std::memory_order_relaxed);
        out.sampling_number = g_sampling.load(std::memory_order_relaxed);
    }
    out.buttons = g_snap.buttons;
    out.lx = g_snap.lx;
    out.ly = g_snap.ly;
    out.rx = g_snap.rx;
    out.ry = g_snap.ry;
    out.attributes = 1u /*IsConnected*/ | (style == StyleHandheld ? 0u : 2u /*IsWired*/);
}

int GetSixAxisHandles(uint32_t* out, int max, uint32_t npad_id, uint32_t style_set) {
    // NpadStyleIndex style indices (hid_types): FullKey=3, Handheld=4, JoyDual=5, Left=6, Right=7.
    int n = 0;
    auto put = [&](uint32_t idx, uint32_t dev) { if (n < max) out[n++] = MakeHandle(idx, npad_id, dev); };
    if (style_set & StyleFullKey) put(3, 0);
    else if (style_set & StyleHandheld) { put(4, 0); put(4, 1); }
    else if (style_set & StyleJoyDual) { put(5, 0); put(5, 1); }
    else if (style_set & StyleJoyLeft) put(6, 0);
    else if (style_set & StyleJoyRight) put(7, 1);
    return n;
}

void StartSixAxis(uint32_t h) { std::lock_guard lk(g_mutex); g_motion_cfg[h].started = true; }
void StopSixAxis(uint32_t h) { std::lock_guard lk(g_mutex); g_motion_cfg[h].started = false; }

int GetSixAxisStates(uint32_t h, SixAxisState* out, int max) {
    std::lock_guard lk(g_mutex);
    (void)h;
    if (g_motion_count == 0) {
        // No samples yet: one at rest.
        if (max <= 0) return 0;
        out[0] = SixAxisState{};
        out[0].accel[2] = -1;
        out[0].direction[0][0] = out[0].direction[1][1] = out[0].direction[2][2] = 1;
        out[0].attributes = 1;
        return 1;
    }
    const int n = std::min(max, g_motion_count);
    for (int i = 0; i < n; ++i) out[i] = g_motion_hist[(g_motion_head - i + 32) % 32];
    return n;
}

bool IsSixAxisAtRest(uint32_t) {
    std::lock_guard lk(g_mutex);
    if (!g_has_motion || g_motion_count == 0) return true;
    const SixAxisState& s = g_motion_hist[g_motion_head];
    const float gn = std::sqrt(s.gyro[0] * s.gyro[0] + s.gyro[1] * s.gyro[1] + s.gyro[2] * s.gyro[2]);
    const float an = std::sqrt(s.accel[0] * s.accel[0] + s.accel[1] * s.accel[1] + s.accel[2] * s.accel[2]);
    return gn < 0.01f && std::fabs(an - 1.0f) < 0.05f;
}

void SetSixAxisFusion(uint32_t h, bool e) { std::lock_guard lk(g_mutex); g_motion_cfg[h].fusion = e; }
bool IsSixAxisFusionEnabled(uint32_t h) { std::lock_guard lk(g_mutex); return g_motion_cfg[h].fusion; }
void SetSixAxisFusionParameters(uint32_t h, float p, float r) {
    std::lock_guard lk(g_mutex);
    g_motion_cfg[h].revise_power = p;
    g_motion_cfg[h].revise_range = r;
}
void GetSixAxisFusionParameters(uint32_t h, float& p, float& r) {
    std::lock_guard lk(g_mutex);
    p = g_motion_cfg[h].revise_power;
    r = g_motion_cfg[h].revise_range;
}
void SetGyroZeroDriftMode(uint32_t h, int m) { std::lock_guard lk(g_mutex); g_motion_cfg[h].zero_drift = m; }
int GetGyroZeroDriftMode(uint32_t h) { std::lock_guard lk(g_mutex); return g_motion_cfg[h].zero_drift; }

void SendVibration(uint32_t handle, const VibrationValue& v) {
    std::lock_guard lk(g_mutex);
    const int dev = ((handle >> 16) & 0xFF) ? 1 : 0;
    g_vib[dev] = v;
    g_vib_time[dev] = Clock::now();
    if (v.amp_low > 0 || v.amp_high > 0) g_vib_dirty = true;
    g_info.rumble_commands++;
}

void ReleaseCursorOnExit() {
#ifdef _WIN32
    if (g_mouse_clipped.exchange(false)) ClipCursor(nullptr);
#endif
}

Config GetConfig() {
    std::lock_guard lk(g_mutex);
    return g_cfg;
}

Info GetInfo() {
    std::lock_guard lk(g_mutex);
    return g_info;
}

void InjectForTest(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry, int duration_ms) {
    std::lock_guard lk(g_mutex);
    g_inject = Step{buttons, lx || ly || rx || ry, lx, ly, rx, ry, duration_ms};
    g_inject_until = Clock::now() + std::chrono::milliseconds(duration_ms);
}

bool RemoteCommandForTest(const std::string& line) {
    std::lock_guard lk(g_mutex);
    return ParseRemoteLocked(line);
}

} // namespace nnshim::hid
