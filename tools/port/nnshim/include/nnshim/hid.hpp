// nnshim::hid: host controller state shaped like nn::hid.
//
// Public layout of NpadXxxState (switchbrew / libnx HidNpadCommonState), identical
// for FullKey, Handheld, JoyDual, JoyLeft and JoyRight (0x28 bytes):
//   +0x00 int64  samplingNumber
//   +0x08 uint64 buttons        (NpadButton: A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7
//                                ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14 Down=15,
//                                StickL{Left,Up,Right,Down}=16-19, StickR{...}=20-23)
//   +0x10 int32  analogStickL.x, +0x14 .y   (range ±0x7FFF, up = positive)
//   +0x18 int32  analogStickR.x, +0x1C .y
//   +0x20 uint32 attributes     (NpadAttribute: IsConnected=bit0, IsWired=bit1, ...)
//
// SixAxisSensorState (0x60, libnx HidSixAxisSensorState): delta_time, sampling_number,
// acceleration (G), angular velocity (turns/s), angle (turns), 3x3 direction matrix,
// attributes (IsConnected=bit0). VibrationValue (16 bytes): amp_low, freq_low, amp_high,
// freq_high.
//
// A host thread (not the guest's) polls the devices at ~250 Hz: XInput, SDL2 (loaded at
// runtime if SDL2.dll is present: Sony/Nintendo controllers, gyro and rumble), keyboard,
// mouse (captured cursor -> right stick and buttons; see MouseConfig) and, for automated
// tests, UDP commands on 127.0.0.1 ("remote_port").
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nnshim::hid {

enum NpadStyle : uint32_t {
    StyleFullKey = 1u << 0,
    StyleHandheld = 1u << 1,
    StyleJoyDual = 1u << 2,
    StyleJoyLeft = 1u << 3,
    StyleJoyRight = 1u << 4,
};

enum : uint32_t { NpadIdNo1 = 0, NpadIdHandheld = 0x20 };

enum NpadButton : uint64_t {
    BtnA = 1ull << 0, BtnB = 1ull << 1, BtnX = 1ull << 2, BtnY = 1ull << 3,
    BtnStickL = 1ull << 4, BtnStickR = 1ull << 5, BtnL = 1ull << 6, BtnR = 1ull << 7,
    BtnZL = 1ull << 8, BtnZR = 1ull << 9, BtnPlus = 1ull << 10, BtnMinus = 1ull << 11,
    BtnLeft = 1ull << 12, BtnUp = 1ull << 13, BtnRight = 1ull << 14, BtnDown = 1ull << 15,
    BtnStickLLeft = 1ull << 16, BtnStickLUp = 1ull << 17, BtnStickLRight = 1ull << 18, BtnStickLDown = 1ull << 19,
    BtnStickRLeft = 1ull << 20, BtnStickRUp = 1ull << 21, BtnStickRRight = 1ull << 22, BtnStickRDown = 1ull << 23,
};

#pragma pack(push, 1)
struct NpadState {               // = NpadFullKeyState / NpadHandheldState / NpadJoy*State
    int64_t sampling_number;
    uint64_t buttons;
    int32_t lx, ly;
    int32_t rx, ry;
    uint32_t attributes;
    uint32_t reserved;
};
struct SixAxisState {            // = nn::hid::SixAxisSensorState
    int64_t delta_time;          // ns between samples
    int64_t sampling_number;
    float accel[3];              // G
    float gyro[3];               // turns/s (1.0 = 360°/s)
    float angle[3];              // accumulated turns
    float direction[3][3];       // orientation matrix (rows = x, y, z axes)
    uint32_t attributes;         // bit0 IsConnected, bit1 IsInterpolated
    uint32_t reserved;
};
struct VibrationValue { float amp_low, freq_low, amp_high, freq_high; };
#pragma pack(pop)
static_assert(sizeof(NpadState) == 0x28, "NpadState: 0x28 bytes");
static_assert(sizeof(SixAxisState) == 0x60, "SixAxisSensorState: 0x60 bytes");
static_assert(sizeof(VibrationValue) == 0x10, "VibrationValue: 0x10 bytes");

// Mouse -> right stick (camera) and buttons. "hid.mouse" section of mod.json or "mouse" of controls.json.
struct MouseConfig {
    std::string enabled = "auto";      // "auto" (only with no controller connected) | "on" | "off"
    float sensitivity = 1.0f;          // at 1.0, 1000 px/s of mouse = full stick
    float smoothing_ms = 25.0f;        // exponential average of the speed (0 = no smoothing)
    float curve = 1.0f;                // response exponent (<1 boosts small movements)
    float anti_deadzone = 0.12f;       // minimum deflection once the mouse moves (game deadzone)
    float min_speed = 15.0f;           // px/s below which the stick returns to center
    int idle_stop_ms = 40;             // no movement for this long: stick snaps to center
    bool invert_x = false, invert_y = false;
    std::string release_key = "F1";    // toggles release / capture of the cursor
    std::string hold_release_key = "LALT"; // releases the cursor while held ("" = none)
    bool capture_on_focus = true;      // capture on (re)gaining focus; otherwise on a click in the window
    bool click_to_capture = true;      // with the cursor released, left click in the window = capture
    bool hide_cursor = true;
    bool clip_cursor = true;           // confines the cursor to the window while captured
    // Mouse buttons -> Switch buttons ("ZR", "A+B", "" = nothing).
    std::string left = "ZR", right = "ZL", middle = "", x1 = "", x2 = "";
    std::string wheel_up = "L", wheel_down = "R";   // each notch = one short press
    int wheel_pulse_ms = 60;
    // Mouse in menus ("ui" section of controls.json; see ui.hpp, MenuTracker). While the game is in a
    // menu: mouse motion does not move the camera; left click = menu_left (on release, if not dragged),
    // left drag = left stick (map, cursors), right click = menu_right, wheel = menu_wheel_up /
    // menu_wheel_down.
    bool menu_enabled = true;          // "ui.mouse_menus"
    std::string menu_left = "A", menu_right = "B", menu_middle = "";
    std::string menu_wheel_up = "Up", menu_wheel_down = "Down";
    bool menu_drag = true;             // left-button drag = left stick
    int menu_drag_threshold_px = 10;   // movement beyond which the click becomes a drag
    float menu_drag_sensitivity = 1.0f;
    int menu_click_ms = 80;            // duration of the menu_left press on release
    int menu_wheel_pulse_ms = 90;      // wheel in menus: press and gap (some list cursors animate ~0.1 s)
};

// Pure mouse logic (no Win32), for the unit tests.
class MouseStick {
public:
    void Reset() { vx_ = vy_ = 0; idle_ms_ = 1e9f; }
    // dx, dy: pixels since the previous call (y down = positive); dt in seconds.
    // Returns the stick deflection in ±0x7FFF (up = positive, like nn::hid).
    void Update(const MouseConfig& c, float dx, float dy, float dt, int32_t& rx, int32_t& ry);
private:
    float vx_ = 0, vy_ = 0;            // smoothed px/s
    float idle_ms_ = 1e9f;
};
class WheelPulser {
public:
    void Reset() { queue_ = 0; phase_ms_ = 0; on_ = false; }
    void AddNotches(int n);            // >0 up, <0 down (at most 4 pending)
    // Advances `dt_ms` and returns the buttons to press now.
    uint64_t Step(float dt_ms, uint64_t up_buttons, uint64_t down_buttons, int pulse_ms);
private:
    int queue_ = 0;                    // signed: pending notches
    int cur_ = 0;                      // direction of the current press
    float phase_ms_ = 0;
    bool on_ = false;
};
// "ZR", "A+B", "ZL,ZR" -> mask (only the 16 digital buttons).
uint64_t ButtonsFromList(const std::string& list);

struct Config {
    std::string backend = "auto";      // "auto" | "xinput" | "sdl2" | "keyboard" | "none"
    bool positional_face_buttons = true; // Xbox controller: south button -> Switch B (position), not A (label)
    bool mirror_handheld = false;      // the same controller also answers as Handheld (id 0x20); default = handheld
    int deadzone = 0x0F00;             // in ±0x7FFF units
    bool keyboard = true;              // the keyboard adds to the controller
    bool keyboard_requires_focus = true; // only when the process window is in the foreground
    std::string sdl_library;           // path to SDL2.dll; empty = next to the DLL or in PATH
    bool motion = true;                // gyro/accelerometer (SDL2)
    bool rumble = true;                // rumble (SDL2 or XInput)
    float rumble_strength = 1.0f;
    int remote_port = 0;               // >0: UDP input commands on 127.0.0.1:<port>
    int poll_hz = 250;
    // Configurable mapping: Switch button -> list of keys / controller elements.
    //   keys:       virtual key names ("RETURN", "SPACE", "A".."Z", "0".."9", "LEFT",
    //               "LSHIFT", "F1"...) or "VK_0x41";
    //   controller: south east west north lb rb lt rt start back ls rs dpup dpdown dpleft dpright.
    // Keyboard stick directions are "LStickUp", "LStickDown", ... "RStickRight".
    std::vector<std::pair<std::string, std::string>> keyboard_map;   // button -> "KEY[,KEY]"
    std::vector<std::pair<std::string, std::string>> pad_map;        // button -> "elem[,elem]"
    MouseConfig mouse;
};

// Releases the cursor clip (DllMain at process exit: the polling thread no longer runs).
void ReleaseCursorOnExit();

// Active configuration (copy), for diagnostics and tests.
Config GetConfig();

void Initialize(const Config& cfg);
void Finalize();
// Live reload (controls.json changed): the polling thread applies it before its next sample. Backend, SDL, motion,
// UDP port and poll rate keep their startup values.
void Reconfigure(const Config& cfg);

// Connected styles for an NpadId (what nn::hid::GetNpadStyleSet returns).
uint32_t GetStyleSet(uint32_t npad_id);

// Current state for (style, id). Always fills `out`; if the style is not connected
// on that id, attributes = 0 and buttons = 0 (as the SDK does).
void GetState(NpadStyle style, uint32_t npad_id, NpadState& out);

// ---- motion (SixAxisSensor) -------------------------------------------------------
// Own handle: byte0 = style (index), byte1 = npad id, byte2 = device (0 left, 1 right).
int GetSixAxisHandles(uint32_t* out, int max, uint32_t npad_id, uint32_t style_set);
void StartSixAxis(uint32_t handle);
void StopSixAxis(uint32_t handle);
// Fills up to `max` samples (most recent first). Returns how many.
int GetSixAxisStates(uint32_t handle, SixAxisState* out, int max);
bool IsSixAxisAtRest(uint32_t handle);
void SetSixAxisFusion(uint32_t handle, bool enabled);
bool IsSixAxisFusionEnabled(uint32_t handle);
void SetSixAxisFusionParameters(uint32_t handle, float revise_power, float revise_range);
void GetSixAxisFusionParameters(uint32_t handle, float& revise_power, float& revise_range);
void SetGyroZeroDriftMode(uint32_t handle, int mode);
int GetGyroZeroDriftMode(uint32_t handle);

// ---- rumble --------------------------------------------------------------------------
// handle = VibrationDeviceHandle (byte2 = device 0 left / 1 right).
void SendVibration(uint32_t handle, const VibrationValue& v);

// ---- diagnostics ----------------------------------------------------------------------------
struct Info {
    std::string device;      // "xinput:0", "sdl2:<name>", "keyboard", "none"
    bool has_motion = false;
    bool sdl_loaded = false;
    uint64_t samples = 0;
    uint64_t rumble_commands = 0;
    uint64_t remote_commands = 0;
    bool mouse_captured = false;       // right now
    uint64_t mouse_captures = 0;       // times the cursor was captured
    uint64_t mouse_samples = 0;        // samples with the cursor captured
    uint64_t mouse_wheel_notches = 0;
    uint64_t menu_mouse_samples = 0;   // mouse samples while the game is in a menu
    uint64_t menu_clicks = 0;          // clicks turned into menu_left
    uint64_t menu_drag_samples = 0;    // samples with a drag (left stick from the mouse)
    uint64_t overlay_samples = 0;      // samples with the nvnvk menu open (input blocked)
    uint64_t overlay_opens = 0;        // times the nvnvk menu was opened
};
Info GetInfo();

// Tests without devices: sets an injected state (added to the devices' state).
void InjectForTest(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry, int duration_ms);
// Tests: processes a command in the same format as the UDP channel.
bool RemoteCommandForTest(const std::string& line);
// Switch buttons by name ("A", "ZL", "Plus"...): for the mapping and the remote commands.
uint64_t ButtonFromName(const std::string& name);

// ---- state shared with nnshim::ui -----------------------------------------------------------
// Is a controller (XInput/SDL2) connected right now? (updated by the polling thread)
bool PadConnected();
// Milliseconds of a monotonic clock.
int64_t NowMs();
// Menu state (set by nnshim::ui once per game frame). No timeout: if the game stalls for a while
// (loading, hitch), the state stays as it was instead of changing in the middle of a drag.
void SetMenuState(bool on);
bool MenuActive();

// Pure mouse-in-menus logic (no Win32), for the unit tests. Inputs of one sample:
// physical buttons and displacement; output: Switch buttons and left stick.
class MenuMouse {
public:
    void Reset() { *this = MenuMouse{}; }
    // left_btn: menu_left mask; right/middle: already resolved masks. dt in ms.
    void Update(const MouseConfig& c, bool left, bool right, bool middle, float dx, float dy, float dt_ms,
                uint64_t left_btn, uint64_t right_btn, uint64_t middle_btn, uint64_t& buttons, int32_t& lx,
                int32_t& ly);
    bool Dragging() const { return dragging_; }
    uint64_t Clicks() const { return clicks_; }

private:
    bool left_prev_ = false, dragging_ = false;
    uint64_t clicks_ = 0;
    float drag_dist_ = 0;
    float pulse_ms_ = 0;               // pending menu_left press
    MouseStick stick_;
};

} // namespace nnshim::hid
