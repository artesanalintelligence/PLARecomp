// nnshim: nn::hid adapters (input, motion and rumble).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef near  // minwindef.h: clashes with the `near` lambda in PostState
#undef far
#endif

#include "abi_common.hpp"
#include "nnshim/hid.hpp"
#include "nnshim/live.hpp"
#include "os_event.hpp"
#include "validate.hpp"

using namespace nnshim;

namespace {

// BindNpadStyleSetUpdateEvent events (runtime ids), released on shutdown.
std::mutex g_style_mu;
std::vector<uint64_t> g_style_events;

// Reads the input settings of `v` under `prefix` ("hid." in the mod.json, "" in the controls file).
void ApplyHidSection(const cfg::Value& v, const std::string& prefix, hid::Config& hc) {
    auto k = [&](const char* name) { return prefix + name; };
    hc.backend = v.GetString(k("backend").c_str(), hc.backend);
    hc.positional_face_buttons = v.GetBool(k("positional_face_buttons").c_str(), hc.positional_face_buttons);
    hc.mirror_handheld = v.GetBool(k("mirror_handheld").c_str(), hc.mirror_handheld);
    hc.deadzone = static_cast<int>(v.GetInt(k("deadzone").c_str(), hc.deadzone));
    hc.keyboard = v.GetBool(k("keyboard").c_str(), hc.keyboard);
    hc.keyboard_requires_focus = v.GetBool(k("keyboard_requires_focus").c_str(), hc.keyboard_requires_focus);
    hc.sdl_library = v.GetString(k("sdl_library").c_str(), hc.sdl_library);
    hc.motion = v.GetBool(k("motion").c_str(), hc.motion);
    hc.rumble = v.GetBool(k("rumble").c_str(), hc.rumble);
    hc.rumble_strength = static_cast<float>(v.GetNumber(k("rumble_strength").c_str(), hc.rumble_strength));
    hc.remote_port = static_cast<int>(v.GetInt(k("remote_port").c_str(), hc.remote_port));
    hc.poll_hz = static_cast<int>(v.GetInt(k("poll_hz").c_str(), hc.poll_hz));
    if (const cfg::Value* m = v.Find(k("keyboard_map").c_str()); m && m->IsObject())
        for (const auto& [key, val] : m->obj) hc.keyboard_map.emplace_back(key, val.str);
    if (const cfg::Value* m = v.Find(k("pad_map").c_str()); m && m->IsObject())
        for (const auto& [key, val] : m->obj) hc.pad_map.emplace_back(key, val.str);
    // Mouse. "enabled" accepts true/false besides "auto"/"on"/"off".
    hid::MouseConfig& mc = hc.mouse;
    const std::string mp = prefix + "mouse.";
    auto mk = [&](const char* name) { return mp + name; };
    if (const cfg::Value* e = v.Find(mk("enabled").c_str())) {
        if (e->type == cfg::Value::Bool) mc.enabled = e->b ? "on" : "off";
        else if (e->type == cfg::Value::String) mc.enabled = e->str;
    }
    mc.sensitivity = static_cast<float>(v.GetNumber(mk("sensitivity").c_str(), mc.sensitivity));
    mc.smoothing_ms = static_cast<float>(v.GetNumber(mk("smoothing_ms").c_str(), mc.smoothing_ms));
    mc.curve = static_cast<float>(v.GetNumber(mk("curve").c_str(), mc.curve));
    mc.anti_deadzone = static_cast<float>(v.GetNumber(mk("anti_deadzone").c_str(), mc.anti_deadzone));
    mc.min_speed = static_cast<float>(v.GetNumber(mk("min_speed").c_str(), mc.min_speed));
    mc.idle_stop_ms = static_cast<int>(v.GetInt(mk("idle_stop_ms").c_str(), mc.idle_stop_ms));
    mc.invert_x = v.GetBool(mk("invert_x").c_str(), mc.invert_x);
    mc.invert_y = v.GetBool(mk("invert_y").c_str(), mc.invert_y);
    mc.release_key = v.GetString(mk("release_key").c_str(), mc.release_key);
    mc.hold_release_key = v.GetString(mk("hold_release_key").c_str(), mc.hold_release_key);
    mc.capture_on_focus = v.GetBool(mk("capture_on_focus").c_str(), mc.capture_on_focus);
    mc.click_to_capture = v.GetBool(mk("click_to_capture").c_str(), mc.click_to_capture);
    mc.hide_cursor = v.GetBool(mk("hide_cursor").c_str(), mc.hide_cursor);
    mc.clip_cursor = v.GetBool(mk("clip_cursor").c_str(), mc.clip_cursor);
    if (const cfg::Value* b = v.Find(mk("buttons").c_str()); b && b->IsObject()) {
        mc.left = b->GetString("left", mc.left);
        mc.right = b->GetString("right", mc.right);
        mc.middle = b->GetString("middle", mc.middle);
        mc.x1 = b->GetString("x1", mc.x1);
        mc.x2 = b->GetString("x2", mc.x2);
        mc.wheel_up = b->GetString("wheel_up", mc.wheel_up);
        mc.wheel_down = b->GetString("wheel_down", mc.wheel_down);
    }
    mc.wheel_pulse_ms = static_cast<int>(v.GetInt(mk("wheel_pulse_ms").c_str(), mc.wheel_pulse_ms));
    // Mouse in menus: "ui" section (root of "config" in the mod.json; root of the controls file).
    const std::string up = prefix == "hid." ? "ui." : prefix + "ui.";
    auto uk = [&](const char* name) { return up + name; };
    mc.menu_enabled = v.GetBool(uk("mouse_menus").c_str(), mc.menu_enabled);
    mc.menu_drag = v.GetBool(uk("menu_drag").c_str(), mc.menu_drag);
    mc.menu_drag_threshold_px = static_cast<int>(v.GetInt(uk("menu_drag_threshold_px").c_str(), mc.menu_drag_threshold_px));
    mc.menu_drag_sensitivity =
        static_cast<float>(v.GetNumber(uk("menu_drag_sensitivity").c_str(), mc.menu_drag_sensitivity));
    mc.menu_click_ms = static_cast<int>(v.GetInt(uk("menu_click_ms").c_str(), mc.menu_click_ms));
    mc.menu_wheel_pulse_ms = static_cast<int>(v.GetInt(uk("menu_wheel_pulse_ms").c_str(), mc.menu_wheel_pulse_ms));
    if (const cfg::Value* b = v.Find(uk("menu_buttons").c_str()); b && b->IsObject()) {
        mc.menu_left = b->GetString("left", mc.menu_left);
        mc.menu_right = b->GetString("right", mc.menu_right);
        mc.menu_middle = b->GetString("middle", mc.menu_middle);
        mc.menu_wheel_up = b->GetString("wheel_up", mc.menu_wheel_up);
        mc.menu_wheel_down = b->GetString("wheel_down", mc.menu_wheel_down);
    }
}

#ifdef _WIN32
std::string DllDir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &m);
    wchar_t w[MAX_PATH] = {};
    if (!GetModuleFileNameW(m, w, MAX_PATH)) return {};
    std::wstring p = w;
    p = p.substr(0, p.find_last_of(L"\\/") + 1);
    const int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(std::strlen(out.c_str()));
    return out;
}
#endif

std::FILE* OpenUtf8(const std::string& path) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

// Default name of the controls file, and the legacy name still accepted when the new one is missing.
constexpr const char* kControlsFileName = "controls.json";
constexpr const char* kLegacyControlsFileName = "controles.json";

} // namespace

// Reads the controls file (JSON with // comments): "hid.controls_file", or controls.json next to the DLL.
// If that file does not exist and is named controls.json, the legacy controles.json in the same folder is
// used instead. false if there is none or it is not valid JSON (logged).
bool nnshim::LoadControlsFile(const cfg::Value& c, cfg::Value& v, std::string& path) {
    path = c.GetString("hid.controls_file", "");
#ifdef _WIN32
    if (path.empty()) path = DllDir() + kControlsFileName;
#endif
    if (path.empty() || path == "none") return false;
    std::FILE* f = OpenUtf8(path);
    if (!f) {
        const size_t slash = path.find_last_of("\\/");
        const size_t base = slash == std::string::npos ? 0 : slash + 1;
        if (path.compare(base, std::string::npos, kControlsFileName) != 0) return false;
        const std::string legacy = path.substr(0, base) + kLegacyControlsFileName;
        f = OpenUtf8(legacy);
        if (!f) return false;
        path = legacy;
    }
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);   // UTF-8 BOM
    std::string err;
    if (!cfg::Parse(text.c_str(), v, &err) || !v.IsObject()) {
        Log(PLA_LOG_ERROR, "nnshim: %s is not valid JSON (%s); using the controls of the mod.json", path.c_str(), err.c_str());
        return false;
    }
    return true;
}

namespace {

// Hand-written controls file: its keys (without "hid.") override those of the mod.json.
void ApplyControlsFile(const cfg::Value& c, hid::Config& hc) {
    cfg::Value v;
    std::string path;
    if (!LoadControlsFile(c, v, path)) return;
    ApplyHidSection(v, "", hc);
    Log(PLA_LOG_INFO, "nnshim: controls from %s", path.c_str());
}

hid::Config BuildHidConfig(const cfg::Value& c) {
    hid::Config hc;
    // Prototype compatibility: flat keys at the root of "config".
    hc.backend = c.GetString("input_backend", hc.backend);
    hc.positional_face_buttons = c.GetBool("positional_face_buttons", hc.positional_face_buttons);
    // Handheld (id 0x20) mirrored only in handheld mode: when docked (oe.docked, the default) the HLE
    // reports it disconnected (A/B abv6: styles 0, attributes 0) and the game reads No1 and Handheld every
    // frame; with the mirror it saw two controllers with the same press (risk of double input in menus).
    hc.mirror_handheld = !c.GetBool("oe.docked", true);
    hc.mirror_handheld = c.GetBool("mirror_handheld", hc.mirror_handheld);
    // "hid" section: { "backend", "deadzone", "keyboard", "motion", "rumble", "keyboard_map", "mouse": {...} }.
    ApplyHidSection(c, "hid.", hc);
    ApplyControlsFile(c, hc);
    if (FamilyMode(Family::Hid) == Mode::Off) hc.backend = "none";
    return hc;
}

// ---- live reload of the controls file (in-game menu of nvnvk saves it on close; the launcher too) ----------------
cfg::Value g_hid_modcfg;        // "config" of the mod.json (to rebuild the controls on top of it)
std::wstring g_controls_wpath;
uint64_t g_controls_stamp = 0;
int g_controls_tick = 0;

uint64_t FileStamp(const std::wstring& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) ^
           (static_cast<uint64_t>(a.nFileSizeLow) << 7);
#else
    (void)p;
    return 0;
#endif
}

void OnLiveControls(bool changed) {
    // Every second, or right away when the menu saved (version change).
    if (!changed && ++g_controls_tick % 10 != 0) return;
    const uint64_t st = FileStamp(g_controls_wpath);
    if (!st || st == g_controls_stamp) return;
    g_controls_stamp = st;
    hid::Reconfigure(BuildHidConfig(g_hid_modcfg));
    Log(PLA_LOG_INFO, "nnshim: controls reloaded live (controls file changed)");
}

void InitHid(const cfg::Value& c) {
    hid::Config hc = BuildHidConfig(c);
    hid::Initialize(hc);
    g_hid_modcfg = c;
    {
        cfg::Value v;
        std::string path;
        if (LoadControlsFile(c, v, path)) {
#ifdef _WIN32
            const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
            std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
            w.resize(wcslen(w.c_str()));
            g_controls_wpath = w;
#endif
            g_controls_stamp = FileStamp(g_controls_wpath);
            if (hc.backend != "none") live::Subscribe(&OnLiveControls);
        }
    }
    Log(PLA_LOG_INFO, "nnshim: hid backend=%s motion=%d rumble=%d remote_port=%d mouse=%s sens=%.2f", hc.backend.c_str(),
        hc.motion, hc.rumble, hc.remote_port, hc.mouse.enabled.c_str(), static_cast<double>(hc.mouse.sensitivity));
}

void ShutdownHid() {
    live::Stop();
    const hid::Info i = hid::GetInfo();
    Log(PLA_LOG_INFO, "nnshim: hid device=%s sdl2=%d motion=%d samples=%llu rumbles=%llu remote=%llu",
        i.device.c_str(), i.sdl_loaded, i.has_motion, static_cast<unsigned long long>(i.samples),
        static_cast<unsigned long long>(i.rumble_commands), static_cast<unsigned long long>(i.remote_commands));
    Log(PLA_LOG_INFO, "nnshim: mouse captures=%llu samples=%llu notches=%llu", static_cast<unsigned long long>(i.mouse_captures),
        static_cast<unsigned long long>(i.mouse_samples), static_cast<unsigned long long>(i.mouse_wheel_notches));
    CounterAdd("hid.samples", i.samples);
    CounterAdd("hid.rumble_commands", i.rumble_commands);
    CounterAdd("hid.mouse_captures", i.mouse_captures);
    CounterAdd("hid.mouse_samples", i.mouse_samples);
    CounterAdd("hid.overlay_samples", i.overlay_samples);
    CounterAdd("hid.overlay_opens", i.overlay_opens);
    Log(PLA_LOG_INFO, "nnshim: nvnvk menu opens=%llu blocked samples=%llu", static_cast<unsigned long long>(i.overlay_opens),
        static_cast<unsigned long long>(i.overlay_samples));
    {
        std::lock_guard lk(g_style_mu);
        if (osev::Available())
            for (uint64_t ev : g_style_events) g_api->event_destroy(ev);
        g_style_events.clear();
    }
    hid::Finalize();
}

ModuleHooks g_hid_module{"hid", &InitHid, &ShutdownHid};

uint32_t LoadU32(PlaGuest& g, uint64_t va) {
    uint32_t v = 0;
    g.load(va, v);
    return v;
}

pla_action GetNpadStateCommon(pla_ctx* ctx, hid::NpadStyle style) {
    PlaGuest g(ctx);
    const uint64_t out = g.x(0);
    hid::NpadState st;
    hid::GetState(style, LoadU32(g, g.x(1)), st);
    g.store(out, st);
    return PLA_SKIP;
}

} // namespace

// =========================== input (Npad) ==================================================
// uint32 nn::hid::GetNpadStyleSet(const uint32& npadId)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetNpadStyleSet) {
    PlaGuest g(ctx);
    g.ret(hid::GetStyleSet(LoadU32(g, g.x(0))));
    return PLA_SKIP;
}

// void nn::hid::GetNpadState(NpadXxxState* out, const uint32& npadId)
// void BindNpadStyleSetUpdateEvent(const NpadIdType&, os::SystemEventType*, os::EventClearMode)
// Kernel event created by the runtime (pla_api 1.1) and handed to the game as the SDK would.
// The shim's styles never change at runtime (FullKey on No1 and, with mirror_handheld, Handheld),
// so it is delivered signaled once, as the HLE does when a controller connects, so that the game
// reads GetNpadStyleSet. Without the 1.1 extension, falls through to the SDK (HLE).
NNSHIM_ADAPTER(Hid, nnshim_hid_BindNpadStyleSetUpdateEvent) {
    PlaGuest g(ctx);
    const uint64_t ev_va = g.x(1);
    const uint32_t mode = static_cast<uint32_t>(g.x(2));
    if (!ev_va) return PLA_CONTINUE;
    uint64_t ev = 0;
    const uint32_t h = osev::NewGuestEvent("nnshim:hid:style", true, &ev);
    if (!h) return PLA_CONTINUE;
    if (!osev::AttachReadableHandle(g, ev_va, h, true, mode)) {
        g_api->event_destroy(ev);
        return PLA_CONTINUE;
    }
    CounterAdd("hid.style_events", 1);
    std::lock_guard lk(g_style_mu);
    g_style_events.push_back(ev);
    return PLA_SKIP;
}

NNSHIM_ADAPTER(Hid, nnshim_hid_GetNpadState_FullKey) { return GetNpadStateCommon(ctx, hid::StyleFullKey); }
NNSHIM_ADAPTER(Hid, nnshim_hid_GetNpadState_Handheld) { return GetNpadStateCommon(ctx, hid::StyleHandheld); }
NNSHIM_ADAPTER(Hid, nnshim_hid_GetNpadState_JoyDual) { return GetNpadStateCommon(ctx, hid::StyleJoyDual); }
NNSHIM_ADAPTER(Hid, nnshim_hid_GetNpadState_JoyLeft) { return GetNpadStateCommon(ctx, hid::StyleJoyLeft); }
NNSHIM_ADAPTER(Hid, nnshim_hid_GetNpadState_JoyRight) { return GetNpadStateCommon(ctx, hid::StyleJoyRight); }

// =========================== motion (SixAxisSensor) =======================================
// int GetSixAxisSensorHandles(SixAxisSensorHandle* out, int count, const NpadIdType& id, NpadStyleSet style)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetSixAxisSensorHandles) {
    PlaGuest g(ctx);
    const int max = std::clamp(static_cast<int>(g.w(1)), 0, 8);
    uint32_t h[8] = {};
    const int n = hid::GetSixAxisHandles(h, max, LoadU32(g, g.x(2)), g.w(3));
    if (n) g.write(g.x(0), h, static_cast<size_t>(n) * 4);
    g.ret(static_cast<uint32_t>(n));
    return PLA_SKIP;
}

// void StartSixAxisSensor(const SixAxisSensorHandle&) / StopSixAxisSensor
NNSHIM_ADAPTER(Hid, nnshim_hid_StartSixAxisSensor) {
    PlaGuest g(ctx);
    hid::StartSixAxis(LoadU32(g, g.x(0)));
    return PLA_SKIP;
}
NNSHIM_ADAPTER(Hid, nnshim_hid_StopSixAxisSensor) {
    PlaGuest g(ctx);
    hid::StopSixAxis(LoadU32(g, g.x(0)));
    return PLA_SKIP;
}

// void GetSixAxisSensorState(SixAxisSensorState* out, const SixAxisSensorHandle&)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetSixAxisSensorState) {
    PlaGuest g(ctx);
    hid::SixAxisState s{};
    hid::GetSixAxisStates(LoadU32(g, g.x(1)), &s, 1);
    g.store(g.x(0), s);
    return PLA_SKIP;
}

// int GetSixAxisSensorStates(SixAxisSensorState* out, int count, const SixAxisSensorHandle&)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetSixAxisSensorStates) {
    PlaGuest g(ctx);
    const int max = std::clamp(static_cast<int>(g.w(1)), 0, 32);
    hid::SixAxisState s[32];
    const int n = hid::GetSixAxisStates(LoadU32(g, g.x(2)), s, max);
    if (n) g.write(g.x(0), s, sizeof(s[0]) * static_cast<size_t>(n));
    g.ret(static_cast<uint32_t>(n));
    return PLA_SKIP;
}

// bool IsSixAxisSensorAtRest(const SixAxisSensorHandle&)
NNSHIM_ADAPTER(Hid, nnshim_hid_IsSixAxisSensorAtRest) {
    PlaGuest g(ctx);
    g.ret(hid::IsSixAxisAtRest(LoadU32(g, g.x(0))) ? 1 : 0);
    return PLA_SKIP;
}

// void EnableSixAxisSensorFusion(const SixAxisSensorHandle&, bool)
NNSHIM_ADAPTER(Hid, nnshim_hid_EnableSixAxisSensorFusion) {
    PlaGuest g(ctx);
    hid::SetSixAxisFusion(LoadU32(g, g.x(0)), (g.w(1) & 0xFF) != 0);
    return PLA_SKIP;
}

// bool IsSixAxisSensorFusionEnabled(const SixAxisSensorHandle&)
NNSHIM_ADAPTER(Hid, nnshim_hid_IsSixAxisSensorFusionEnabled) {
    PlaGuest g(ctx);
    g.ret(hid::IsSixAxisFusionEnabled(LoadU32(g, g.x(0))) ? 1 : 0);
    return PLA_SKIP;
}

// void SetSixAxisSensorFusionParameters(const SixAxisSensorHandle&, float power, float range)  (S0, S1)
NNSHIM_ADAPTER(Hid, nnshim_hid_SetSixAxisSensorFusionParameters) {
    PlaGuest g(ctx);
    hid::SetSixAxisFusionParameters(LoadU32(g, g.x(0)), pla_get_s(g_api, ctx, 0), pla_get_s(g_api, ctx, 1));
    return PLA_SKIP;
}

// void GetSixAxisSensorFusionParameters(float* power, float* range, const SixAxisSensorHandle&)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetSixAxisSensorFusionParameters) {
    PlaGuest g(ctx);
    float p = 0, r = 0;
    hid::GetSixAxisFusionParameters(LoadU32(g, g.x(2)), p, r);
    g.store(g.x(0), p);
    g.store(g.x(1), r);
    return PLA_SKIP;
}

// void SetGyroscopeZeroDriftMode(const SixAxisSensorHandle&, const GyroscopeZeroDriftMode&)
NNSHIM_ADAPTER(Hid, nnshim_hid_SetGyroscopeZeroDriftMode) {
    PlaGuest g(ctx);
    hid::SetGyroZeroDriftMode(LoadU32(g, g.x(0)), static_cast<int>(LoadU32(g, g.x(1))));
    return PLA_SKIP;
}

// GyroscopeZeroDriftMode GetGyroscopeZeroDriftMode(const SixAxisSensorHandle&)   (enum in W0)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetGyroscopeZeroDriftMode) {
    PlaGuest g(ctx);
    g.ret(static_cast<uint32_t>(hid::GetGyroZeroDriftMode(LoadU32(g, g.x(0)))));
    return PLA_SKIP;
}

// =========================== rumble =======================================================
// VibrationPlayer/Node/Mixer/Target are SDK user code (they stay recompiled).
// Everything ends up in these internal SDK functions (not imported by main), hooked at their
// definition:
//   Result detail::EnqueueVibrationValue(const VibrationDeviceHandle&, const VibrationValue&)
//   Result detail::SendVibrationValue(const VibrationDeviceHandle&, const VibrationValue&)
//   Result detail::SendVibrationValues(const VibrationDeviceHandle*, const VibrationValue*, int)
NNSHIM_ADAPTER(Hid, nnshim_hid_EnqueueVibrationValue) {
    PlaGuest g(ctx);
    hid::VibrationValue v{};
    g.load(g.x(1), v);
    hid::SendVibration(LoadU32(g, g.x(0)), v);
    g.ret(0);
    return PLA_SKIP;
}

NNSHIM_ADAPTER(Hid, nnshim_hid_SendVibrationValue) {
    PlaGuest g(ctx);
    hid::VibrationValue v{};
    g.load(g.x(1), v);
    hid::SendVibration(LoadU32(g, g.x(0)), v);
    g.ret(0);
    return PLA_SKIP;
}

NNSHIM_ADAPTER(Hid, nnshim_hid_SendVibrationValues) {
    PlaGuest g(ctx);
    const int n = std::clamp(static_cast<int>(g.w(2)), 0, 16);
    for (int i = 0; i < n; ++i) {
        hid::VibrationValue v{};
        g.load(g.x(1) + static_cast<uint64_t>(i) * sizeof v, v);
        hid::SendVibration(LoadU32(g, g.x(0) + static_cast<uint64_t>(i) * 4), v);
    }
    g.ret(0);
    return PLA_SKIP;
}

// int GetVibrationDeviceHandles(VibrationDeviceHandle* out, int count, const NpadIdType&, NpadStyleSet)
NNSHIM_ADAPTER(Hid, nnshim_hid_GetVibrationDeviceHandles) {
    PlaGuest g(ctx);
    const int max = std::clamp(static_cast<int>(g.w(1)), 0, 8);
    uint32_t h[8] = {};
    // Same encoding as SixAxisSensorHandle (style, npad, device).
    const int n = hid::GetSixAxisHandles(h, max, LoadU32(g, g.x(2)), g.w(3));
    // A full controller has two actuators (left and right).
    int count = n;
    if (n == 1 && max >= 2 && (h[0] & 0xFF) == 3) {
        h[1] = h[0] | (1u << 16);
        count = 2;
    }
    if (count) g.write(g.x(0), h, static_cast<size_t>(count) * 4);
    g.ret(static_cast<uint32_t>(count));
    return PLA_SKIP;
}

// void InitializeVibrationDevice(const VibrationDeviceHandle&)
NNSHIM_ADAPTER(Hid, nnshim_hid_InitializeVibrationDevice) { return PLA_SKIP; }

// Result detail::GetVibrationDeviceInfo(VibrationDeviceInfo* out, const VibrationDeviceHandle&)
//   VibrationDeviceInfo = { uint32 deviceType (1 = LinearResonantActuator); uint32 position (1 left, 2 right) }
NNSHIM_ADAPTER(Hid, nnshim_hid_GetVibrationDeviceInfo) {
    PlaGuest g(ctx);
    const uint32_t h = LoadU32(g, g.x(1));
    const uint32_t info[2] = {1u, ((h >> 16) & 0xFF) ? 2u : 1u};
    g.write(g.x(0), info, sizeof info);
    g.ret(0);
    return PLA_SKIP;
}

// =========================== A/B validation (observe mode, gen_mod_json.py --validate) ======
// Controller style and idle state: in an A/B run without input both sides must agree (same
// connected style, attributes, no buttons, sticks centered within 1 %). With input (physical
// controller or UDP) buttons differ on purpose: the shim reads its own source.
NNSHIM_OBSERVER(nnshim_hid_GetNpadStyleSet) {
    PlaGuest g(ctx);
    g.call_data()[0] = LoadU32(g, g.x(0));
}
NNSHIM_EXPORT void nnshim_hid_GetNpadStyleSet_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint32_t id = static_cast<uint32_t>(g.call_data()[0]);
    const uint32_t shim = hid::GetStyleSet(id);
    ValidateResult("hid.GetNpadStyleSet", g.w(0) == shim, Fmt("id=%u styles=%#x", id, g.w(0)), Fmt("%#x", shim));
}

static void ObserveState(pla_ctx* ctx) {
    PlaGuest g(ctx);
    uint64_t* cd = g.call_data();
    cd[0] = g.x(0);
    cd[1] = LoadU32(g, g.x(1));
}
static void PostState(pla_ctx* ctx, hid::NpadStyle style, const char* name) {
    PlaGuest g(ctx);
    const uint64_t* cd = g.call_data();
    hid::NpadState h{}, s{};
    g.load(cd[0], h);
    hid::GetState(style, static_cast<uint32_t>(cd[1]), s);
    auto near = [](int32_t a, int32_t b) { return a - b <= 330 && b - a <= 330; };
    const bool eq = h.attributes == s.attributes && h.buttons == s.buttons && near(h.lx, s.lx) && near(h.ly, s.ly) &&
                    near(h.rx, s.rx) && near(h.ry, s.ry);
    ValidateResult(name, eq,
                   Fmt("id=%u attr=%#x btn=%#llx L=%d,%d R=%d,%d", static_cast<uint32_t>(cd[1]), h.attributes,
                       static_cast<unsigned long long>(h.buttons), h.lx, h.ly, h.rx, h.ry),
                   Fmt("attr=%#x btn=%#llx L=%d,%d R=%d,%d", s.attributes, static_cast<unsigned long long>(s.buttons),
                       s.lx, s.ly, s.rx, s.ry));
}
NNSHIM_OBSERVER(nnshim_hid_GetNpadState_FullKey) { ObserveState(ctx); }
NNSHIM_EXPORT void nnshim_hid_GetNpadState_FullKey_post(pla_ctx* ctx) { PostState(ctx, hid::StyleFullKey, "hid.GetNpadState_FullKey"); }
NNSHIM_OBSERVER(nnshim_hid_GetNpadState_Handheld) { ObserveState(ctx); }
NNSHIM_EXPORT void nnshim_hid_GetNpadState_Handheld_post(pla_ctx* ctx) { PostState(ctx, hid::StyleHandheld, "hid.GetNpadState_Handheld"); }
