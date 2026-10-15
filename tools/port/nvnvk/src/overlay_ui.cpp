// In-game overlay menu: the UI (start screen, tabbed options, quit confirmation, help bar). Style inspired by the
// "recompiled" projects' menus (dark modal with tabs and a description on the right), drawn with Dear ImGui and Lato
// fonts. All text is our own: no logos or game assets.
// SPDX-License-Identifier: MIT
#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <intrin.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "nvnvk.h"
#include "options.h"
#include "vkctx.h"

namespace nvnvk {

extern ImFont* g_ov_fonts[3];

namespace {

// ---- palette (on a near-black background) ----
const ImVec4 kAccent{0.725f, 0.490f, 0.949f, 1.0f};   // violet
const ImVec4 kAccent2{0.855f, 0.729f, 0.969f, 1.0f};  // light violet
const ImVec4 kWarn{0.914f, 0.804f, 0.208f, 1.0f};     // yellow (applies after restart)
const ImVec4 kDanger{0.973f, 0.376f, 0.224f, 1.0f};   // orange (quit)
const ImVec4 kWhite{0.949f, 0.949f, 0.949f, 1.0f};
const ImVec4 kModal{0.031f, 0.027f, 0.051f, 0.88f};

enum { kFontReg = 0, kFontBold = 1, kFontBlack = 2 };

struct Desc {
    std::string title, text;
    bool restart = false;
};

struct Ui {
    Options op;
    std::string pla_ini, version;
    bool save = true;
    float s = 1.0f, style_s = 0.0f;
    int tab = 0, desc_tab = -1;  // desc_tab: tab of the description shown
    bool scroll_top = false;
    int prev_mode = -1, drawn_mode = Overlay::kStart;
    bool focus_first = false;
    bool confirm_exit = false, confirm_open = false;
    std::string capture_id;
    bool capture_add = false;
    int capture_frame = 0;
    Desc desc, desc_hover, desc_focus;
    std::set<ImGuiID> row_focus;
    bool fullscreen = false;
    int win_w = 0, win_h = 0;
    std::string saved_msg;
    double saved_t = 0;
    HWND hwnd = nullptr;
    std::vector<std::pair<std::string, bool>> mods;  // read when the options open
    float boot_tex_bias = 0.0f;  // engine_options texture_lod_bias when the game started (baked into the game's samplers)
    int rr_reserve_w = 1920;     // render_res: render targets reserved at boot (live changes up to this width)
} U;

float S(float v) { return v * U.s; }
ImU32 C(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
ImVec2 Measure(int f, float size, const char* t) { return g_ov_fonts[f]->CalcTextSizeA(S(size), FLT_MAX, 0.0f, t); }
void Text(ImDrawList* dl, int f, float size, ImVec2 p, ImU32 col, const char* t) { dl->AddText(g_ov_fonts[f], S(size), p, col, t); }
float Ease(float t) { return 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f); }

std::string Num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    std::string s = b;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

bool NavVisible() { return GImGui->NavCursorVisible; }

// ---------------------------------------------------------------- live apply
void SetOpt(Overlay& ov, const std::string& id, const std::string& v);

// Graphics presets (FPS-1): choosing Quality/Performance sets its settings; touching one of them by hand turns the
// preset into "Custom".
bool g_applying_preset = false;
bool InPreset(const std::string& id) {
    for (const auto& kv : PresetValues("performance"))
        if (kv.first == id) return true;
    return false;
}

void Apply(Overlay& ov, const std::string& id) {
    Options& o = U.op;
    if (id == "preset") {
        const std::string p = o.Get(id);
        g_applying_preset = true;
        for (const auto& [k, v] : PresetValues(p))
            if (k.compare(0, 3, "rr_") ? (k.compare(0, 3, "mo_") || o.Available(SettingFile::Engine)) : o.Available(SettingFile::RenderRes))
                SetOpt(ov, k, v);
        g_applying_preset = false;
    } else if (!g_applying_preset && InPreset(id) && (o.Get("preset") == "quality" || o.Get("preset") == "performance")) {
        o.Set("preset", "custom");
    }
    if (id == "aa") {
        const std::string& v = o.Get(id);
        g_cfg.aa = v == "fxaa" ? 1 : (v == "smaa" ? 2 : 0);
    } else if (id == "aa_hud") {
        g_cfg.aa_hud = o.GetB(id);
    } else if (id == "sharpen") {
        g_cfg.sharpen = o.GetF(id);
    } else if (id == "upscale") {
        g_cfg.upscale_sharp = o.Get(id) == "sharp";
    } else if (id == "fps_limit") {
        g_cfg.fps_limit = (uint32_t)std::max(0, std::atoi(o.Get(id).c_str()));
        // PLA.ini FPS cap (read by PLA.exe at startup; the launcher writes it too): kept in sync.
        if (o.Available(SettingFile::PlaIni)) o.Set("ini_fps_cap", o.Get(id));
    } else if (id == "vsync") {
        g_cfg.vsync = o.GetB(id);
        g_present.RequestRecreate();
    } else if (id == "show_fps") {
        g_cfg.show_fps = o.GetB(id);
    } else if (id == "brightness") {
        g_cfg.brightness = o.GetF(id);
    } else if (id == "contrast") {
        g_cfg.contrast = o.GetF(id);
    } else if (id == "saturation") {
        g_cfg.saturation = o.GetF(id);
    } else if (id == "overlay_blur") {
        ov.blur_ = o.GetB(id);
    } else if (id == "anisotropy") {
        g_cfg.anisotropy = (uint32_t)std::max(0, std::atoi(o.Get(id).c_str()));
    } else if (id == "mo_texture_lod_bias") {
        // The boot value is already in the samplers the game created; nvnvk adds the difference to its own samplers.
        g_cfg.tex_lod_bias = o.GetF(id) - U.boot_tex_bias;
    } else if (id == "rr_width" || id == "rr_height") {
        const bool live = std::atoi(o.Get("rr_width").c_str()) <= U.rr_reserve_w;
        o.SetLive("rr_width", live);
        o.SetLive("rr_height", live);
    }
    // Settings of other mods (render_res, engine_options, nnshim): published on the live channel; each mod applies what
    // it can right away (docs/port/LIVE-SETTINGS.md).
    const std::string lk = o.LiveKey(id);
    if (!lk.empty()) LivePublish(lk, o.Get(id));
}

void SetOpt(Overlay& ov, const std::string& id, const std::string& v) {
    if (U.op.Get(id) == v) return;
    U.op.Set(id, v);
    Apply(ov, id);
}

// Real values of the live options (if the file lacks the key, nvnvk uses its internal value).
void SyncLive(Overlay& ov) {
    Options& o = U.op;
    o.Base("aa", g_cfg.aa == 1 ? "fxaa" : (g_cfg.aa == 2 ? "smaa" : "off"));
    o.Base("aa_hud", g_cfg.aa_hud ? "true" : "false");
    if (std::fabs(o.GetF("sharpen") - g_cfg.sharpen) > 1e-4f) o.Base("sharpen", Num(g_cfg.sharpen));
    o.Base("upscale", g_cfg.upscale_sharp ? "sharp" : "bilinear");
    if ((uint32_t)std::atoi(o.Get("fps_limit").c_str()) != g_cfg.fps_limit) o.Base("fps_limit", std::to_string(g_cfg.fps_limit));
    o.Base("vsync", g_cfg.vsync ? "true" : "false");
    o.Base("show_fps", g_cfg.show_fps ? "true" : "false");
    if (std::fabs(o.GetF("brightness") - g_cfg.brightness) > 1e-4f) o.Base("brightness", Num(g_cfg.brightness));
    if (std::fabs(o.GetF("contrast") - g_cfg.contrast) > 1e-4f) o.Base("contrast", Num(g_cfg.contrast));
    if (std::fabs(o.GetF("saturation") - g_cfg.saturation) > 1e-4f) o.Base("saturation", Num(g_cfg.saturation));
    o.Base("overlay_blur", ov.blur_ ? "true" : "false");
    o.Base("anisotropy", std::to_string(g_cfg.anisotropy));
    if (g_auto_perf) {
        // Auto preset applied at startup on a low-end GPU: show the values in effect (the files keep theirs).
        if (o.Available(SettingFile::RenderRes)) {
            o.Base("rr_width", "1280");
            o.Base("rr_height", "720");
            o.Base("rr_dynamic", "true");
        }
        if (o.Available(SettingFile::Engine)) o.Base("mo_dof", "false");
    }
    {
        const bool live = std::atoi(o.Get("rr_width").c_str()) <= U.rr_reserve_w;
        o.SetLive("rr_width", live);
        o.SetLive("rr_height", live);
    }
    // Window: real state.
    if (U.hwnd) {
        RECT rc{};
        GetClientRect(U.hwnd, &rc);
        U.win_w = rc.right - rc.left;
        U.win_h = rc.bottom - rc.top;
        const LONG_PTR st = GetWindowLongPtrW(U.hwnd, GWL_STYLE);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(U.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        RECT wr{};
        GetWindowRect(U.hwnd, &wr);
        U.fullscreen = !(st & WS_CAPTION) && wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                       wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    }
}

void SaveNow() {
    std::string summary;
    const int n = U.op.Save(U.save, &summary);
    if (n > 0) LiveSaved();
    if (!summary.empty()) {
        NVK_INFO("menu: settings %s (%d files):\n%s", U.save ? "saved" : "NOT saved (overlay_save=false)", n, summary.c_str());
        U.saved_msg = U.save ? "Settings saved" : "Settings not saved (test mode)";
        U.saved_t = ImGui::GetTime();
    }
}

// ---------------------------------------------------------------- key and controller glyphs
bool IsArrow(const std::string& n) { return n == "UP" || n == "DOWN" || n == "LEFT" || n == "RIGHT"; }

void Arrow(ImDrawList* dl, ImVec2 c, float r, int dir, ImU32 col) {
    // dir: 0 up, 1 down, 2 left, 3 right
    ImVec2 a, b, d;
    switch (dir) {
    case 0: a = {c.x, c.y - r}; b = {c.x - r, c.y + r * 0.6f}; d = {c.x + r, c.y + r * 0.6f}; break;
    case 1: a = {c.x, c.y + r}; b = {c.x + r, c.y - r * 0.6f}; d = {c.x - r, c.y - r * 0.6f}; break;
    case 2: a = {c.x - r, c.y}; b = {c.x + r * 0.6f, c.y + r}; d = {c.x + r * 0.6f, c.y - r}; break;
    default: a = {c.x + r, c.y}; b = {c.x - r * 0.6f, c.y - r}; d = {c.x - r * 0.6f, c.y + r}; break;
    }
    dl->AddTriangleFilled(a, b, d, col);
}

// Drawn key (body with border). Returns the width.
float Keycap(ImDrawList* dl, ImVec2 p, float h, const std::string& name, ImU32 border, float font_px = 15.0f) {
    const std::string label = KeyLabel(name);
    float w = h;
    if (!IsArrow(name)) w = std::max(h, Measure(kFontBold, font_px, label.c_str()).x + S(12));
    dl->AddRectFilled(p, {p.x + w, p.y + h}, C(kWhite, 0.10f), S(5));
    dl->AddRect(p, {p.x + w, p.y + h}, border, S(5), 0, std::max(1.0f, S(1.4f)));
    dl->AddLine({p.x + S(3), p.y + h - S(2)}, {p.x + w - S(3), p.y + h - S(2)}, C(kWhite, 0.25f), std::max(1.0f, S(1.5f)));
    const ImVec2 c{p.x + w * 0.5f, p.y + h * 0.47f};
    if (IsArrow(name)) {
        Arrow(dl, c, h * 0.2f, name == "UP" ? 0 : name == "DOWN" ? 1 : name == "LEFT" ? 2 : 3, C(kWhite));
    } else {
        const ImVec2 ts = Measure(kFontBold, font_px, label.c_str());
        Text(dl, kFontBold, font_px, {c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, C(kWhite), label.c_str());
    }
    return w;
}

// Controller button: "A", "B", "LB", "RB" or "DPAD". Returns the width.
float PadGlyph(ImDrawList* dl, ImVec2 p, float h, const char* b) {
    const std::string n = b;
    if (n == "DPAD") {
        const float t = h * 0.34f, c = h * 0.5f;
        dl->AddRectFilled({p.x + c - t * 0.5f, p.y}, {p.x + c + t * 0.5f, p.y + h}, C(kWhite, 0.85f), S(2));
        dl->AddRectFilled({p.x, p.y + c - t * 0.5f}, {p.x + h, p.y + c + t * 0.5f}, C(kWhite, 0.85f), S(2));
        return h;
    }
    if (n == "LB" || n == "RB") {
        const float w = h * 1.6f;
        dl->AddRectFilled(p, {p.x + w, p.y + h}, C(kWhite, 0.85f), h * 0.45f,
                          n == "LB" ? ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft
                                    : ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight);
        const ImVec2 ts = Measure(kFontBlack, 13, b);
        Text(dl, kFontBlack, 13, {p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f}, C({0.06f, 0.05f, 0.09f, 1}), b);
        return w;
    }
    const ImVec2 c{p.x + h * 0.5f, p.y + h * 0.5f};
    dl->AddCircleFilled(c, h * 0.5f, C(kWhite, 0.88f), 24);
    const ImVec2 ts = Measure(kFontBlack, 15, b);
    Text(dl, kFontBlack, 15, {c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, C({0.06f, 0.05f, 0.09f, 1}), b);
    return h;
}

// ---------------------------------------------------------------- option rows
struct Row {
    ImVec2 p;
    float w = 0, h = 0;
    ImGuiID rid = 0;
    bool focus_now = false, hot = false, disabled = false;
    Desc d;
} R;

void RowBegin(const char* title, const char* desc, bool restart, bool disabled = false) {
    ImGui::PushID(title);
    R = Row{};
    R.p = ImGui::GetCursorScreenPos();
    R.w = ImGui::GetContentRegionAvail().x;
    R.h = S(54);
    R.rid = ImGui::GetID("##row");
    R.disabled = disabled;
    R.d = Desc{title, desc ? desc : "", restart};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                     ImGui::IsMouseHoveringRect(R.p, {R.p.x + R.w, R.p.y + R.h});
    const bool foc = U.row_focus.count(R.rid) && NavVisible();
    if (hov || foc) dl->AddRectFilled(R.p, {R.p.x + R.w, R.p.y + R.h}, C({0.745f, 0.722f, 0.859f, foc ? 0.14f : 0.08f}), S(8));
    if (foc) dl->AddRectFilled({R.p.x, R.p.y + S(10)}, {R.p.x + S(3), R.p.y + R.h - S(10)}, C(kAccent), S(2));
    const ImVec2 ts = Measure(kFontReg, 20, title);
    const float ty = R.p.y + (R.h - ts.y) * 0.5f;
    Text(dl, kFontReg, 20, {R.p.x + S(18), ty}, C(kWhite, disabled ? 0.35f : (hov || foc ? 1.0f : 0.86f)), title);
    if (restart) {
        // "After restart" mark: a yellow dot after the title.
        dl->AddCircleFilled({R.p.x + S(18) + ts.x + S(10), ty + ts.y * 0.55f}, S(3.5f), C(kWarn, disabled ? 0.3f : 0.9f), 12);
    }
    if (hov) U.desc_hover = R.d;
    ImGui::SetCursorScreenPos({R.p.x + R.w * 0.46f, R.p.y + (R.h - S(36)) * 0.5f});
    if (disabled) ImGui::BeginDisabled();
}

void Track() {
    if (ImGui::IsItemFocused()) R.focus_now = true;
}

void RowEnd() {
    if (R.disabled) ImGui::EndDisabled();
    if (R.focus_now) {
        U.row_focus.insert(R.rid);
        if (NavVisible()) U.desc_focus = R.d;
    } else {
        U.row_focus.erase(R.rid);
    }
    ImGui::SetCursorScreenPos({R.p.x, R.p.y + R.h + S(2)});
    ImGui::Dummy({R.w, 0.0f});
    ImGui::PopID();
}

void Section(const char* t) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += S(12);
    Text(ImGui::GetWindowDrawList(), kFontBold, 15, {p.x + S(18), p.y}, C(kAccent2, 0.9f), t);
    const float h = Measure(kFontBold, 15, t).y;
    ImGui::SetCursorScreenPos({p.x, p.y + h + S(8)});
    ImGui::Dummy({1, 0});
}

void Note(const char* t, const ImVec4& col = kWhite, float a = 0.55f) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x - S(36);
    ImGui::SetCursorScreenPos({p.x + S(18), p.y + S(4)});
    ImGui::PushFont(g_ov_fonts[kFontReg], S(16));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col.x, col.y, col.z, a));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy({1, S(4)});
}

// "Pill" button (to pick between values). Returns true when pressed.
bool Pill(const char* label, bool sel, float minw = 0.0f) {
    const ImVec2 ts = Measure(kFontReg, 18, label);
    const ImVec2 sz{std::max(minw, ts.x + S(30)), S(36)};
    const bool pr = ImGui::InvisibleButton(label, sz, ImGuiButtonFlags_EnableNav);
    Track();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dis = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? 0.4f : 1.0f;
    if (sel) {
        dl->AddRectFilled(a, b, C(kAccent, (act ? 0.40f : 0.26f) * dis), S(18));
        dl->AddRect(a, b, C(kAccent, 0.95f * dis), S(18), 0, std::max(1.0f, S(1.5f)));
    } else {
        dl->AddRectFilled(a, b, C(kWhite, (act ? 0.16f : hov ? 0.10f : 0.035f) * dis), S(18));
        dl->AddRect(a, b, C(kWhite, (hov ? 0.35f : 0.16f) * dis), S(18), 0, std::max(1.0f, S(1.0f)));
    }
    Text(dl, sel ? kFontBold : kFontReg, 18, {a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f},
         C(kWhite, (sel || hov ? 1.0f : 0.7f) * dis), label);
    return pr;
}

struct Opt {
    const char* value;
    const char* label;
};

void Pills(Overlay& ov, const std::string& id, std::initializer_list<Opt> opts, const std::string* cur_override = nullptr) {
    const std::string cur = cur_override ? *cur_override : U.op.Get(id);
    int i = 0;
    for (const Opt& o : opts) {
        if (i++) ImGui::SameLine(0, S(8));
        if (Pill(o.label, cur == o.value)) SetOpt(ov, id, o.value);
    }
}

void Toggle(Overlay& ov, const std::string& id, const char* on = "On", const char* off = "Off") {
    const std::string cur = U.op.GetB(id) ? "true" : "false";
    if (Pill(on, cur == "true", S(130))) SetOpt(ov, id, "true");
    ImGui::SameLine(0, S(8));
    if (Pill(off, cur == "false", S(130))) SetOpt(ov, id, "false");
}

// Small button with a drawn symbol. kind: 0 minus, 1 plus, 2 left arrow, 3 right arrow, 4 cross.
bool IconButton(const char* id, int kind, float size, bool nav = true) {
    const bool pr = ImGui::InvisibleButton(id, {size, size}, nav ? ImGuiButtonFlags_EnableNav : 0);
    Track();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dis = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? 0.4f : 1.0f;
    dl->AddRectFilled(a, b, C(kWhite, (act ? 0.18f : hov ? 0.12f : 0.05f) * dis), size * 0.5f);
    const ImU32 col = C(kWhite, (hov ? 1.0f : 0.8f) * dis);
    const float r = size * 0.22f, th = std::max(1.5f, S(2.2f));
    switch (kind) {
    case 0: dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, th); break;
    case 1:
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, th);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, th);
        break;
    case 2: Arrow(dl, c, r * 0.9f, 2, col); break;
    case 3: Arrow(dl, c, r * 0.9f, 3, col); break;
    default:
        dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, th);
        dl->AddLine({c.x - r, c.y + r}, {c.x + r, c.y - r}, col, th);
        break;
    }
    return pr;
}

void Slider(Overlay& ov, const std::string& id, float mn, float mx, float step, const char* fmt, float mul = 1.0f) {
    float v = U.op.GetF(id);
    const float h = S(36);
    float nv = v;
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    if (IconButton("##minus", 0, h)) nv = v - step;
    ImGui::SameLine(0, S(10));
    const float tw = S(230);
    ImGui::InvisibleButton("##track", {tw, h});
    Track();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin();
    if (act) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - a.x) / tw, 0.0f, 1.0f);
        nv = mn + t * (mx - mn);
    }
    ImGui::SameLine(0, S(10));
    if (IconButton("##plus", 1, h)) nv = v + step;
    ImGui::PopItemFlag();
    nv = std::clamp(std::round(nv / step) * step, mn, mx);
    if (std::fabs(nv - v) > step * 0.01f) {
        SetOpt(ov, id, Num(nv));
        v = nv;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dis = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? 0.4f : 1.0f;
    const float cy = a.y + h * 0.5f, t = (v - mn) / (mx - mn);
    dl->AddRectFilled({a.x, cy - S(3)}, {a.x + tw, cy + S(3)}, C(kWhite, 0.14f * dis), S(3));
    dl->AddRectFilled({a.x, cy - S(3)}, {a.x + tw * t, cy + S(3)}, C(kAccent, 0.95f * dis), S(3));
    dl->AddCircleFilled({a.x + tw * t, cy}, S(act ? 10.5f : 9.0f), C(hov || act ? kAccent2 : kWhite, dis), 24);
    char b[32];
    std::snprintf(b, sizeof b, fmt, v * mul);
    ImGui::SameLine(0, S(14));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = Measure(kFontBold, 18, b);
    Text(dl, kFontBold, 18, {p.x, p.y + (h - ts.y) * 0.5f}, C(kWhite, 0.9f * dis), b);
    ImGui::Dummy({ts.x, h});
}

// "< value >" selector (long lists). `opts`: (value, label).
bool Cycle(const std::vector<std::pair<std::string, std::string>>& opts, const std::string& cur, std::string* out, float w = 0.0f) {
    int idx = -1;
    for (int i = 0; i < (int)opts.size(); ++i)
        if (opts[i].first == cur) idx = i;
    const float h = S(36);
    if (w <= 0) w = S(250);
    bool ch = false;
    if (IconButton("##prev", 2, h)) {
        idx = idx <= 0 ? (int)opts.size() - 1 : idx - 1;
        ch = true;
    }
    ImGui::SameLine(0, S(8));
    const bool pr = ImGui::InvisibleButton("##value", {w, h}, ImGuiButtonFlags_EnableNav);
    Track();
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (pr) {
        idx = (idx + 1) % (int)opts.size();
        ch = true;
    }
    ImGui::SameLine(0, S(8));
    if (IconButton("##next", 3, h)) {
        idx = (idx + 1) % (int)opts.size();
        ch = true;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dis = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? 0.4f : 1.0f;
    dl->AddRectFilled(a, b, C(kWhite, (hov ? 0.09f : 0.04f) * dis), S(8));
    dl->AddRect(a, b, C(kWhite, 0.16f * dis), S(8));
    const std::string label = idx >= 0 ? opts[idx].second : (cur.empty() ? "—" : cur);
    const ImVec2 ts = Measure(kFontBold, 18, label.c_str());
    Text(dl, kFontBold, 18, {a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f}, C(kWhite, dis), label.c_str());
    if (ch && idx >= 0) *out = opts[idx].first;
    return ch && idx >= 0;
}

std::vector<std::string> Split(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v) {
        if (c == ',' || c == ' ' || c == '|') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += (char)toupper((unsigned char)c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void StartCapture(Overlay& ov, const std::string& id, bool add) {
    U.capture_id = id;
    U.capture_add = add;
    U.capture_frame = ImGui::GetFrameCount();
    ov.captured_vk_ = 0;
    ov.capturing_ = true;
}

void EndCapture(Overlay& ov) {
    U.capture_id.clear();
    ov.capturing_ = false;
    ov.captured_vk_ = 0;
}

void KeyBind(Overlay& ov, const std::string& id, const std::set<std::string>& dup) {
    const std::vector<std::string> keys = Split(U.op.Get(id));
    const bool cap = U.capture_id == id;
    const float h = S(36), w = S(250);
    if (ImGui::InvisibleButton("##key", {w, h}, ImGuiButtonFlags_EnableNav)) StartCapture(ov, id, false);
    Track();
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, C(cap ? kAccent : kWhite, cap ? 0.18f : (hov ? 0.09f : 0.04f)), S(8));
    dl->AddRect(a, b, C(cap ? kAccent : kWhite, cap ? 0.9f : 0.16f), S(8), 0, std::max(1.0f, S(cap ? 1.5f : 1.0f)));
    if (cap) {
        const float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 6.0f);
        const char* t = "Press a key…";
        const ImVec2 ts = Measure(kFontReg, 17, t);
        Text(dl, kFontReg, 17, {a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f}, C(kAccent2, pulse), t);
    } else if (keys.empty()) {
        const char* t = "Unassigned";
        const ImVec2 ts = Measure(kFontReg, 17, t);
        Text(dl, kFontReg, 17, {a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f}, C(kWhite, 0.45f), t);
    } else {
        float x = a.x + S(10);
        for (const std::string& k : keys) {
            if (x > b.x - S(40)) break;
            x += Keycap(dl, {x, a.y + S(5)}, h - S(10), k, dup.count(k) ? C(kDanger, 0.95f) : C(kWhite, 0.6f)) + S(6);
        }
    }
    ImGui::SameLine(0, S(8));
    if (IconButton("##add", 1, h)) StartCapture(ov, id, true);
    ImGui::SameLine(0, S(8));
    if (IconButton("##remove", 4, h)) {
        if (keys.size() > 1) {
            std::string v;
            for (size_t i = 0; i + 1 < keys.size(); ++i) v += (i ? "," : "") + keys[i];
            SetOpt(ov, id, v);
        } else {
            SetOpt(ov, id, U.op.Default(id));
        }
    }
}

void ProcessCapture(Overlay& ov) {
    if (U.capture_id.empty()) return;
    const int vk = ov.captured_vk_.exchange(0);
    if (vk) {
        if (vk != VK_ESCAPE) {
            const std::string name = KeyName(vk, ov.captured_ext_.load());
            std::vector<std::string> keys = Split(U.op.Get(U.capture_id));
            std::string v = name;
            if (U.capture_add && !keys.empty() && std::find(keys.begin(), keys.end(), name) == keys.end()) {
                v.clear();
                for (auto& k : keys) v += k + ",";
                v += name;
            } else if (U.capture_add && !keys.empty()) {
                v = U.op.Get(U.capture_id);
            }
            SetOpt(ov, U.capture_id, v);
            NVK_INFO("menu: %s = %s", U.capture_id.c_str(), v.c_str());
        }
        EndCapture(ov);
        return;
    }
    if (ImGui::GetFrameCount() > U.capture_frame + 1 &&
        (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        EndCapture(ov);
}

// ---------------------------------------------------------------- tabs
const char* kTabs[] = {"General", "Graphics", "Display", "Controls", "Sound", "Engine & mods"};
constexpr int kTabCount = 6;

void TabGeneral(Overlay& ov) {
    Section("MENU");
    RowBegin("Start screen on launch",
             "Shows the start screen (Play, Options, Controls, Quit) over the game's startup. While it is open, the game "
             "receives no keys or buttons.\n\nApplies on the next launch.",
             false);
    Toggle(ov, "overlay_start", "Show", "Skip");
    RowEnd();
    RowBegin("Blur the game behind the menu",
             "Dims and blurs the game while the menu is open. Turn it off to see the game sharp behind the options.",
             false);
    Toggle(ov, "overlay_blur");
    RowEnd();
    Section("SYSTEM");
    RowBegin("System language",
             "The language the console reports to the game (it uses it on its language selection screen). \"System\" = "
             "the Windows language. A save that is already started keeps the language you chose in the game. The game "
             "reads it only when it starts.",
             true, !U.op.Available(SettingFile::Nnshim));
    {
        std::string out;
        static const std::vector<std::pair<std::string, std::string>> kLang = {
            {"host", "System"},  {"es", "Spanish"},        {"en-US", "English (US)"}, {"en-GB", "English (UK)"},
            {"fr", "French"},    {"de", "German"},         {"it", "Italian"},         {"ja", "Japanese"},
            {"ko", "Korean"},    {"zh-Hans", "Chinese (Simplified)"}, {"zh-Hant", "Chinese (Traditional)"}};
        if (Cycle(kLang, U.op.Get("language"), &out)) SetOpt(ov, "language", out);
    }
    RowEnd();
    Section("ABOUT");
    {
        std::string t = "PLARecomp. Native PC port with its own renderer (nvnvk, Vulkan).\n\n"
                        "Open this menu: Esc key, or hold Minus + Plus on the controller.\n\n"
                        "Interface: Dear ImGui (MIT). Typeface: Lato (SIL Open Font License 1.1).\n\nVersion: " +
                        U.version;
        RowBegin("Version", t.c_str(), false);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Text(ImGui::GetWindowDrawList(), kFontReg, 18, {p.x, p.y + S(7)}, C(kWhite, 0.6f), U.version.c_str());
        ImGui::Dummy({S(10), S(36)});
        RowEnd();
    }
}

void TabGraphics(Overlay& ov) {
    Section("PRESET");
    RowBegin("Graphics preset",
             "Quality: native 1920x1080, FXAA, 16x anisotropic filtering, depth of field.\n\nPerformance (laptops, integrated "
             "GPUs): 1280x720 internal resolution with dynamic resolution, no anisotropic filtering, no FXAA or sharpening, "
             "bilinear scaling, no depth of field.\n\nAuto: Performance on an integrated or low-VRAM GPU (decided at startup), "
             "otherwise the settings below. Changing any of those settings by hand makes the preset Custom.\n\nApplies immediately.",
             false);
    Pills(ov, "preset", {{"auto", "Auto"}, {"quality", "Quality"}, {"performance", "Performance"}, {"custom", "Custom"}});
    RowEnd();
    Section("ANTIALIASING AND SHARPNESS");
    RowBegin("Antialiasing",
             "Smooths jagged edges in the final pass (FXAA). Almost no cost.\n\nApplies immediately.", false);
    Pills(ov, "aa", {{"off", "Off"}, {"fxaa", "FXAA"}});
    RowEnd();
    RowBegin("Keep the HUD sharp",
             "With antialiasing on, leaves the text and icons of the game's interface untouched (sharper) and only smooths "
             "the scene.\n\nApplies immediately.",
             false, U.op.Get("aa") == "off");
    Toggle(ov, "aa_hud");
    RowEnd();
    RowBegin("Sharpening", "Enhances detail (contrast-adaptive sharpening, no halos). 0 = off.\n\nApplies immediately.", false);
    Slider(ov, "sharpen", 0.0f, 1.0f, 0.05f, "%.0f %%", 100.0f);
    RowEnd();
    RowBegin("Scaling to the window",
             "How the image is fitted when the internal resolution differs from the window. \"Sharp\" uses a Lanczos filter "
             "without halos; \"Bilinear\" is softer. If the game renders at a higher resolution than the window, it is "
             "reduced by area (supersampling).\n\nApplies immediately.",
             false);
    Pills(ov, "upscale", {{"bilinear", "Bilinear"}, {"sharp", "Sharp"}});
    RowEnd();
    Section("TEXTURES AND RESOLUTION");
    RowBegin("Anisotropic filtering",
             "Sharpness of textures seen at an angle (the ground in the distance). \"Game\" keeps the game's own value.\n\n"
             "Applies immediately.",
             false);
    Pills(ov, "anisotropy", {{"0", "Game"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}});
    RowEnd();
    const bool rr = U.op.Available(SettingFile::RenderRes);
    const int cur_w = std::atoi(U.op.Get("rr_width").c_str());
    const bool rr_restart = cur_w > U.rr_reserve_w;
    {
        char hr[64];
        std::snprintf(hr, sizeof hr, "%d x %d", U.rr_reserve_w, U.rr_reserve_w * 9 / 16);
        const std::string d =
            rr ? std::string("Resolution the game renders at (the Switch uses 1920x1080 at most). Higher = cleaner image; 4K "
                             "needs much more GPU and memory.\n\nApplies immediately up to the live headroom reserved when the "
                             "game started (") + hr + "); higher values (marked \"restart\") apply after restarting the game."
               : "Requires the render_res mod (install it from the launcher, Graphics tab).";
        RowBegin("Internal resolution", d.c_str(), rr_restart, !rr);
    }
    {
        static const std::vector<std::pair<std::string, std::string>> kRes = {
            {"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"},
            {"1920x1080", "1920 x 1080 (native)"}, {"2560x1440", "2560 x 1440"}, {"3200x1800", "3200 x 1800"}, {"3840x2160", "3840 x 2160"}};
        std::vector<std::pair<std::string, std::string>> opts = kRes;
        for (auto& o : opts)
            if (std::atoi(o.first.c_str()) > U.rr_reserve_w) o.second += " (restart)";
        std::string out;
        if (Cycle(opts, U.op.Get("rr_width") + "x" + U.op.Get("rr_height"), &out)) {
            const size_t x = out.find('x');
            SetOpt(ov, "rr_width", out.substr(0, x));
            SetOpt(ov, "rr_height", out.substr(x + 1));
        }
    }
    RowEnd();
    RowBegin("Dynamic resolution",
             rr ? "Like on the Switch, the game lowers the 3D resolution in heavy scenes. \"Fixed\" always renders at the chosen "
                  "resolution.\n\nApplies immediately."
                : "Requires the render_res mod.",
             false, !rr);
    {
        const std::string cur = U.op.GetB("rr_dynamic") ? "true" : "false";
        if (Pill("Dynamic", cur == "true", S(130))) SetOpt(ov, "rr_dynamic", "true");
        ImGui::SameLine(0, S(8));
        if (Pill("Fixed", cur == "false", S(130))) SetOpt(ov, "rr_dynamic", "false");
    }
    RowEnd();
    RowBegin("Live resolution headroom",
             rr ? "The game's render targets are reserved at this size when it starts, so the internal resolution can then be "
                  "changed in this menu without restarting, up to this size. More headroom uses more video memory even "
                  "while rendering at a lower resolution. \"Internal\" = only the resolution chosen at startup."
                : "Requires the render_res mod.",
             true, !rr);
    Pills(ov, "rr_reserve", {{"0", "Internal"}, {"2560", "1440p"}, {"3840", "4K"}});
    RowEnd();
}

void TabDisplay(Overlay& ov) {
    Section("WINDOW");
    RowBegin("Fullscreen",
             "Borderless fullscreen on the window's monitor. Also saved for the next launch.\n\nApplies immediately.",
             false);
    if (Pill("On", U.fullscreen, S(130)) && !U.fullscreen) {
        PostMessageW(U.hwnd, kOvMsgFullscreen, 1, 0);
        U.fullscreen = true;
        if (U.op.Available(SettingFile::PlaIni)) U.op.Set("ini_fullscreen", "1");
    }
    ImGui::SameLine(0, S(8));
    if (Pill("Windowed", !U.fullscreen, S(130)) && U.fullscreen) {
        PostMessageW(U.hwnd, kOvMsgFullscreen, 0, 0);
        U.fullscreen = false;
        if (U.op.Available(SettingFile::PlaIni)) U.op.Set("ini_fullscreen", "0");
    }
    RowEnd();
    RowBegin("Window size", "Size of the game area in windowed mode.\n\nApplies immediately.", false, U.fullscreen);
    {
        std::vector<std::pair<std::string, std::string>> kWin = {
            {"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"}, {"1920x1080", "1920 x 1080"}, {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160"}};
        const std::string cur = std::to_string(U.win_w) + "x" + std::to_string(U.win_h);
        bool found = false;
        for (auto& o : kWin) found |= o.first == cur;
        if (!found) kWin.insert(kWin.begin(), {cur, std::to_string(U.win_w) + " x " + std::to_string(U.win_h) + " (current)"});
        std::string out;
        if (Cycle(kWin, cur, &out)) {
            const size_t x = out.find('x');
            const int ww = std::atoi(out.substr(0, x).c_str()), hh = std::atoi(out.substr(x + 1).c_str());
            PostMessageW(U.hwnd, kOvMsgClientSize, (WPARAM)ww, (LPARAM)hh);
            U.win_w = ww;
            U.win_h = hh;
            SetOpt(ov, "window_width", std::to_string(ww));
            SetOpt(ov, "window_height", std::to_string(hh));
        }
    }
    RowEnd();
    Section("FRAMES");
    RowBegin("Vertical sync",
             "Syncs with the monitor refresh (no tearing, but limits the FPS to the refresh rate).\n\nApplies immediately.",
             false);
    Toggle(ov, "vsync");
    RowEnd();
    RowBegin("FPS limit",
             "Frames per second cap. \"Unlimited\" lets the game run as fast as it can (it uses the real time of each "
             "frame).\n\nApplies immediately. If PLA.ini had a cap at startup, that cap stays until the game restarts.",
             false);
    {
        static const std::vector<std::pair<std::string, std::string>> kFps = {
            {"0", "Unlimited"}, {"30", "30 FPS"}, {"60", "60 FPS"}, {"120", "120 FPS"}, {"144", "144 FPS"}, {"165", "165 FPS"}, {"240", "240 FPS"}};
        std::string out;
        if (Cycle(kFps, U.op.Get("fps_limit"), &out)) SetOpt(ov, "fps_limit", out);
    }
    RowEnd();
    RowBegin("Show FPS", "Frames per second counter at the top left.\n\nApplies immediately.", false);
    Toggle(ov, "show_fps");
    RowEnd();
    Section("COLOR");
    RowBegin("Brightness", "Adds or removes brightness in the final image.\n\nApplies immediately.", false);
    Slider(ov, "brightness", -0.5f, 0.5f, 0.05f, "%+.0f", 100.0f);
    RowEnd();
    RowBegin("Contrast", "1.00 = as in the game.\n\nApplies immediately.", false);
    Slider(ov, "contrast", 0.5f, 1.5f, 0.05f, "%.2f");
    RowEnd();
    RowBegin("Saturation", "1.00 = as in the game; 0 = black and white.\n\nApplies immediately.", false);
    Slider(ov, "saturation", 0.0f, 2.0f, 0.05f, "%.2f");
    RowEnd();
}

void TabControls(Overlay& ov) {
    const bool ok = U.op.Available(SettingFile::Controls);
    if (!ok) {
        Note("The controls file was not found (the nnshim is not active or has no controls file). Create it from the "
             "launcher: Options › Controls.",
             kWarn, 0.9f);
        return;
    }
    Note("Click a key (or press Enter / A) and then the new key. \"+\" adds another key to the same button; \"x\" removes "
         "the last one. Esc is reserved for this menu. Key and mouse changes apply when you close this menu; the on-screen "
         "button icons change after restarting the game.");
    std::set<std::string> dup;
    {
        std::map<std::string, int> cnt;
        static const char* kAll[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "StickL", "StickR",
                                     "Up", "Down", "Left", "Right", "LStickUp", "LStickDown", "LStickLeft", "LStickRight",
                                     "RStickUp", "RStickDown", "RStickLeft", "RStickRight"};
        for (const char* b : kAll)
            for (const std::string& k : Split(U.op.Get(std::string("key_") + b))) cnt[k]++;
        for (auto& [k, n] : cnt)
            if (n > 1) dup.insert(k);
    }
    RowBegin("Use the keyboard", "The keyboard works together with the controller. Turn it off if you only play with a controller.\n\nApplies when you close this menu.", false);
    Toggle(ov, "keyboard");
    RowEnd();
    struct B {
        const char* id;
        const char* label;
    };
    auto group = [&](const char* title, std::initializer_list<B> bs) {
        Section(title);
        for (const B& b : bs) {
            const std::string id = std::string("key_") + b.id;
            std::string d = std::string("Key for the Switch button ") + b.label + "." +
                            (dup.empty() ? "" : "\n\nKeys in orange are assigned to more than one button.") +
                            "\n\nApplies when you close this menu.";
            RowBegin(b.label, d.c_str(), false, !U.op.GetB("keyboard"));
            KeyBind(ov, id, dup);
            RowEnd();
        }
    };
    group("BUTTONS", {{"A", "A"}, {"B", "B"}, {"X", "X"}, {"Y", "Y"}, {"L", "L"}, {"R", "R"}, {"ZL", "ZL"}, {"ZR", "ZR"},
                      {"Plus", "Plus (+)"}, {"Minus", "Minus (−)"}, {"StickL", "Left stick press"}, {"StickR", "Right stick press"}});
    group("D-PAD", {{"Up", "D-pad up"}, {"Down", "D-pad down"}, {"Left", "D-pad left"}, {"Right", "D-pad right"}});
    group("LEFT STICK (MOVE)", {{"LStickUp", "Forward"}, {"LStickDown", "Back"}, {"LStickLeft", "Left"}, {"LStickRight", "Right"}});
    group("RIGHT STICK (CAMERA)", {{"RStickUp", "Camera up"}, {"RStickDown", "Camera down"}, {"RStickLeft", "Camera left"}, {"RStickRight", "Camera right"}});
    Section("MOUSE");
    RowBegin("Mouse for the camera",
             "\"Automatic\" = only when no controller is connected. While the mouse is captured, the cursor is hidden inside "
             "the window (this menu releases it).\n\nApplies when you close this menu.",
             false);
    Pills(ov, "mouse_enabled", {{"auto", "Automatic"}, {"on", "Always"}, {"off", "Never"}});
    RowEnd();
    RowBegin("Sensitivity", "Camera turn speed with the mouse.\n\nApplies when you close this menu.", false);
    Slider(ov, "mouse_sensitivity", 0.1f, 5.0f, 0.1f, "%.1f");
    RowEnd();
    RowBegin("Invert X axis", "Inverts the horizontal camera turn with the mouse.\n\nApplies when you close this menu.", false);
    Toggle(ov, "mouse_invert_x", "Yes", "No");
    RowEnd();
    RowBegin("Invert Y axis", "Inverts the vertical camera turn with the mouse.\n\nApplies when you close this menu.", false);
    Toggle(ov, "mouse_invert_y", "Yes", "No");
    RowEnd();
    RowBegin("Mouse in the game menus",
             "In menus, dialogs and battles: left click = A, right click = B, wheel = up/down.\n\nApplies when you close "
             "this menu.",
             false);
    Toggle(ov, "ui_mouse_menus");
    RowEnd();
    Section("ICONS");
    RowBegin("Button icons",
             "Which icons the game shows: keyboard keys or controller buttons. \"Automatic\" = keys when no controller is "
             "connected as each screen loads. The key icons are regenerated when the game is started with the play script or "
             "the launcher.",
             true);
    Pills(ov, "ui_button_prompts", {{"auto", "Automatic"}, {"keyboard", "Keyboard"}, {"controller", "Controller"}});
    RowEnd();
}

void TabSound(Overlay& ov) {
    const bool ok = U.op.Available(SettingFile::Nnshim);
    RowBegin("Master volume", "Volume of the whole game (the music / effects / voices mix is in the game's own options).\n\nApplies immediately.", false, !ok);
    Slider(ov, "audio_volume", 0.0f, 1.0f, 0.05f, "%.0f %%", 100.0f);
    RowEnd();
    // Mute = volume 0 with the audio device open (live both ways). An older setting (backend "null", written by
    // earlier versions of the launcher) does not open the device at all: turning sound back on then needs a restart.
    const bool backend_off = U.op.Get("audio_backend") == "null";
    RowBegin("Mute",
             "No sound. Applies immediately.\n\nIf the game was started with the audio device turned off (older setting), "
             "turning the sound back on needs a restart.",
             U.op.Changed("audio_backend"), !ok);
    {
        const bool mute = U.op.GetB("audio_mute") || backend_off;
        if (Pill("Yes", mute, S(130))) SetOpt(ov, "audio_mute", "true");
        ImGui::SameLine(0, S(8));
        if (Pill("No", !mute, S(130))) {
            SetOpt(ov, "audio_mute", "false");
            if (backend_off) SetOpt(ov, "audio_backend", "xaudio2");
        }
    }
    RowEnd();
    Note("Sound plays on the Windows default output device.");
}

// CPU class used by engine_options for cpu_preset "auto" (same rule: physical cores the process may run on,
// < 8 = performance tier; PLA_CPU_CLASS=low|high overrides). Only for the menu label.
bool CpuIsLow() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    char ev[16] = {};
    if (GetEnvironmentVariableA("PLA_CPU_CLASS", ev, sizeof ev) && ev[0]) {
        cached = (_stricmp(ev, "low") == 0 || _stricmp(ev, "performance") == 0) ? 1 : 0;
        return cached == 1;
    }
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<unsigned char> buf(len ? len : 1);
    int cores = 0;
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
        for (DWORD off = 0; off < len;) {
            auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
            KAFFINITY m = e->Processor.GroupMask[0].Group == 0 ? e->Processor.GroupMask[0].Mask : 0;
            if (pm) m &= (KAFFINITY)pm;
            if (m) ++cores;
            off += e->Size;
        }
    }
    if (!cores) cores = (int)__popcnt64((unsigned long long)pm);
    cached = cores < 8 ? 1 : 0;
    return cached == 1;
}

// Pills for an engine CPU knob whose "auto" follows the CPU preset. Values not in the list (older files) show
// as the first entry whose value matches `alias` pairs.
void CpuPills(Overlay& ov, const std::string& id, std::initializer_list<Opt> opts) {
    std::string cur = U.op.Get(id);
    if (cur.empty()) cur = "auto";
    Pills(ov, id, opts, &cur);
}

void TabEngine(Overlay& ov) {
    const bool ok = U.op.Available(SettingFile::Engine);
    Section("ENGINE OPTIONS (engine_options MOD)");
    if (!ok) Note("The engine_options mod is not installed (install it from the launcher, Engine tab).", kWarn, 0.9f);
    RowBegin("Field of view", "Camera field of view while exploring (100 % = as in the game).\n\nApplies immediately.", false, !ok);
    Slider(ov, "mo_fov", 0.5f, 2.0f, 0.05f, "%.0f %%", 100.0f);
    RowEnd();
    RowBegin("Draw distance", "Multiplies the distance at which trees, characters and creatures are drawn (1.0 = as in the game). The game reads these distances only when it starts.", true, !ok);
    Slider(ov, "mo_lod", 0.5f, 4.0f, 0.25f, "%.2fx");
    RowEnd();
    RowBegin("Distant texture sharpness", "Texture level-of-detail bias (negative = sharper; -1 recommended).\n\nApplies immediately.", false, !ok);
    Slider(ov, "mo_texture_lod_bias", -4.0f, 4.0f, 0.5f, "%+.1f");
    RowEnd();
    RowBegin("Full-rate distant animation", "Distant creatures animate at the same rate as nearby ones. The game reads this only when it starts.", true, !ok);
    Toggle(ov, "mo_anim_full_rate", "Yes", "No");
    RowEnd();
    RowBegin("Bloom", "Soft glow around the sky, water and lights.\n\nApplies immediately.", false, !ok);
    Toggle(ov, "mo_bloom");
    RowEnd();
    RowBegin("Depth of field", "Blur of distant objects in cutscenes and events.\n\nApplies immediately.", false, !ok);
    Toggle(ov, "mo_dof");
    RowEnd();
    RowBegin("Chromatic aberration", "Color fringing used by some effects and events.\n\nApplies immediately.", false, !ok);
    Toggle(ov, "mo_chromatic_aberration");
    RowEnd();
    RowBegin("Hide game HUD", "Hides the game's interface (photo mode). Applies immediately and is kept for the next start.", false, !ok);
    Toggle(ov, "mo_hud_hidden", "Yes", "No");
    RowEnd();
    Section("CPU WORK (FOR WEAKER PROCESSORS)");
    {
        const bool low = CpuIsLow();
        std::string cur = U.op.Get("mo_cpu_preset");
        if (cur.empty() || cur == "auto") cur = "auto";
        else if (cur == "default" || cur == "quality") cur = "full";
        static std::string auto_label;
        auto_label = std::string("Auto (") + (low ? "Performance" : "Full") + ")";
        RowBegin("CPU preset",
                 "How much work the game itself does per frame: characters loaded around the player, distant animation and "
                 "the options below set to \"Auto\".\n\nAuto = Performance on processors with fewer than 8 cores (most laptops), "
                 "Full otherwise. Full = as in the game. The game reads this only when it starts.",
                 true, !ok);
        if (Pill(auto_label.c_str(), cur == "auto")) SetOpt(ov, "mo_cpu_preset", "auto");
        ImGui::SameLine(0, S(8));
        if (Pill("Full", cur == "full", S(110))) SetOpt(ov, "mo_cpu_preset", "full");
        ImGui::SameLine(0, S(8));
        if (Pill("Performance", cur == "performance")) SetOpt(ov, "mo_cpu_preset", "performance");
        RowEnd();
    }
    RowBegin("Crowd density",
             "How far around the player village characters and their creatures are loaded and updated (100 % = the game's "
             "radius times the draw distance).\n\nAuto: 100 % with the Full preset; with Performance 67 %, and never more "
             "than the game's own radius when the draw distance is raised. The game reads this only when it starts.",
             true, !ok);
    CpuPills(ov, "mo_npc_density", {{"auto", "Auto"}, {"1.0", "100 %"}, {"0.67", "67 %"}, {"0.5", "50 %"}, {"0.25", "25 %"}});
    RowEnd();
    RowBegin("Distant animation detail",
             "How many characters animate at full rate and how far the animation rate levels reach (distant characters "
             "animate with fewer updates per second).\n\nAuto: Full with the Full preset, Low with Performance. The game "
             "reads this only when it starts.",
             true, !ok);
    CpuPills(ov, "mo_anim_lod", {{"auto", "Auto"}, {"default", "Full"}, {"reduced", "Reduced"}, {"low", "Low"}});
    RowEnd();
    RowBegin("Native islands",
             "Runs the game's hottest animation work (skeleton and skin matrices) as native code instead of the translated "
             "code: same results, a few percent more FPS.\n\nTurn off only to rule it out when something looks wrong. "
             "Applies when the game starts.",
             true, !U.op.Available(SettingFile::Islands));
    Toggle(ov, "islands_enabled", "On", "Off");
    RowEnd();
    Section("INSTALLED MODS");
    for (auto& [n, on] : U.mods) {
        RowBegin(n.c_str(), "Mods in the game's mods folder. Turn them on or off from the launcher (Advanced › Mods).", false);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled({p.x + S(8), p.y + S(18)}, S(5), C(on ? ImVec4{0.36f, 0.85f, 0.52f, 1} : kWhite, on ? 1.0f : 0.3f), 12);
        Text(dl, kFontReg, 18, {p.x + S(22), p.y + S(7)}, C(kWhite, on ? 0.85f : 0.45f), on ? "On" : "Off");
        ImGui::Dummy({S(10), S(36)});
        RowEnd();
    }
}

std::vector<std::string> TabIds(int tab) {
    switch (tab) {
    case 0: return {"overlay_start", "overlay_blur", "language"};
    case 1: return {"preset", "aa", "aa_hud", "sharpen", "upscale", "anisotropy", "rr_width", "rr_height", "rr_dynamic", "rr_reserve"};
    case 2: return {"vsync", "fps_limit", "show_fps", "brightness", "contrast", "saturation"};
    case 3: {
        std::vector<std::string> v = {"keyboard", "mouse_enabled", "mouse_sensitivity", "mouse_invert_x", "mouse_invert_y", "ui_button_prompts", "ui_mouse_menus"};
        for (const char* b : {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "StickL", "StickR", "Up", "Down", "Left", "Right",
                              "LStickUp", "LStickDown", "LStickLeft", "LStickRight", "RStickUp", "RStickDown", "RStickLeft", "RStickRight"})
            v.push_back(std::string("key_") + b);
        return v;
    }
    case 4: return {"audio_volume", "audio_mute"};
    default: return {"mo_fov", "mo_lod", "mo_texture_lod_bias", "mo_anim_full_rate", "mo_bloom", "mo_dof", "mo_chromatic_aberration", "mo_hud_hidden",
                     "mo_cpu_preset", "mo_npc_density", "mo_anim_lod", "islands_enabled"};
    }
}

// Text button (modal footer, confirmation).
bool TextButton(const char* label, const ImVec4& col, bool filled, float minw = 0.0f) {
    const ImVec2 ts = Measure(kFontBold, 18, label);
    const ImVec2 sz{std::max(minw, ts.x + S(40)), S(42)};
    const bool pr = ImGui::InvisibleButton(label, sz, ImGuiButtonFlags_EnableNav);
    const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused(), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (filled) dl->AddRectFilled(a, b, C(col, act ? 1.0f : hov ? 0.95f : 0.8f), S(10));
    else dl->AddRectFilled(a, b, C(kWhite, act ? 0.16f : hov ? 0.11f : 0.05f), S(10));
    dl->AddRect(a, b, C(filled ? col : kWhite, filled ? 1.0f : 0.25f), S(10), 0, std::max(1.0f, S(1.2f)));
    Text(dl, kFontBold, 18, {a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f},
         filled ? C({0.05f, 0.04f, 0.08f, 1}) : C(kWhite, hov ? 1.0f : 0.85f), label);
    return pr;
}

// Bottom help bar (keys or controller, whichever was used last).
void HelpBar(Overlay& ov, ImDrawList* dl, float cx, float y, bool tabs) {
    const bool pad = ov.last_input_.load() == 1;
    const float h = S(26), gap = S(36);
    struct Item {
        std::vector<std::string> glyphs;
        const char* text;
    };
    std::vector<Item> items;
    if (pad) {
        items.push_back({{"DPAD"}, "Navigate"});
        items.push_back({{"A"}, "Accept"});
        items.push_back({{"B"}, "Back"});
        if (tabs) items.push_back({{"LB", "RB"}, "Tabs"});
    } else {
        items.push_back({{"UP", "DOWN", "LEFT", "RIGHT"}, "Navigate"});
        items.push_back({{"RETURN"}, "Accept"});
        items.push_back({{"ESCAPE"}, "Back"});
        if (tabs) items.push_back({{"Q", "E"}, "Tabs"});
    }
    // Measure first, then draw centered.
    auto glyph_w = [&](const std::string& g) {
        if (pad) return g == "LB" || g == "RB" ? h * 1.6f : h;
        if (IsArrow(g)) return h;
        return std::max(h, Measure(kFontBold, 14, KeyLabel(g).c_str()).x + S(12));
    };
    float total = 0;
    for (auto& it : items) {
        for (auto& g : it.glyphs) total += glyph_w(g) + S(4);
        total += S(6) + Measure(kFontReg, 17, it.text).x + gap;
    }
    total -= gap;
    float x = cx - total * 0.5f;
    for (auto& it : items) {
        for (auto& g : it.glyphs) {
            x += (pad ? PadGlyph(dl, {x, y}, h, g.c_str()) : Keycap(dl, {x, y}, h, g, C(kWhite, 0.6f), 14)) + S(4);
        }
        x += S(6);
        const ImVec2 ts = Measure(kFontReg, 17, it.text);
        Text(dl, kFontReg, 17, {x, y + (h - ts.y) * 0.5f}, C(kWhite, 0.75f), it.text);
        x += ts.x + gap;
    }
}

void Back(Overlay& ov) {
    if (!U.capture_id.empty()) {
        EndCapture(ov);
        return;
    }
    if (U.confirm_exit) {
        U.confirm_exit = false;
        return;
    }
    const int m = ov.mode_.load();
    if (m == Overlay::kOptions) ov.mode_ = ov.from_start_.load() ? Overlay::kStart : Overlay::kClosed;
}

void QuitGame(Overlay& ov) {
    SaveNow();
    NVK_INFO("menu: quit game");
    ov.mode_ = Overlay::kClosed;
    if (U.hwnd) PostMessageW(U.hwnd, WM_CLOSE, 0, 0);
    std::thread([] {
        Sleep(10000);  // in case the game does not close on WM_CLOSE
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
}

// Quit confirmation modal (on top of everything; captures navigation).
void ConfirmExit(Overlay& ov, int w, int h) {
    if (U.confirm_exit && !U.confirm_open) {
        ImGui::OpenPopup("##quit");
        U.confirm_open = true;
    }
    const ImVec2 sz{S(560), S(250)};
    ImGui::SetNextWindowPos({(w - sz.x) * 0.5f, (h - sz.y) * 0.5f});
    ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kModal);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16));
    if (ImGui::BeginPopupModal("##quit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (!U.confirm_exit) {
            ImGui::CloseCurrentPopup();
            U.confirm_open = false;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        dl->AddRect(p, {p.x + sz.x, p.y + sz.y}, C(kWhite, 0.2f), S(16), 0, std::max(1.0f, S(1.1f)));
        Text(dl, kFontBold, 28, {p.x + S(32), p.y + S(30)}, C(kWhite), "Quit the game?");
        ImGui::SetCursorScreenPos({p.x + S(32), p.y + S(80)});
        ImGui::PushFont(g_ov_fonts[kFontReg], S(18));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.7f));
        ImGui::PushTextWrapPos(sz.x - S(32));
        ImGui::TextUnformatted("The game will close. Anything you have not saved in the game will be lost. Your settings in "
                               "this menu are already saved.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({p.x + sz.x - S(32) - S(160) - S(12) - S(200), p.y + sz.y - S(32) - S(42)});
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (TextButton("Cancel", kWhite, false, S(160))) U.confirm_exit = false;
        ImGui::SameLine(0, S(12));
        if (TextButton("Quit game", kDanger, true, S(200))) QuitGame(ov);
        ImGui::EndPopup();
    } else {
        U.confirm_open = false;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------- screens
void DrawOptions(Overlay& ov, int w, int h) {
    ImDrawList* bg = ImGui::GetWindowDrawList();
    const float margin = S(56);
    const float mw = std::min((float)w - 2 * margin, S(1640));
    const float mh = (float)h - margin - S(96);
    const float slide = (1.0f - Ease(ov.Blend())) * S(24);
    const ImVec2 a{((float)w - mw) * 0.5f, margin + slide}, b{a.x + mw, a.y + mh};
    bg->AddRectFilled({a.x + S(6), a.y + S(10)}, {b.x + S(6), b.y + S(14)}, C({0, 0, 0, 1}, 0.35f), S(18));  // shadow
    bg->AddRectFilled(a, b, C(kModal), S(16));
    bg->AddRect(a, b, C(kWhite, 0.2f), S(16), 0, std::max(1.0f, S(1.1f)));
    // Tabs
    const float tab_h = S(66);
    const bool pad = ov.last_input_.load() == 1;
    float x = a.x + S(20);
    {
        const float gh = S(24), gy = a.y + (tab_h - gh) * 0.5f;
        x += (pad ? PadGlyph(bg, {x, gy}, gh, "LB") : Keycap(bg, {x, gy}, gh, "Q", C(kWhite, 0.5f), 13)) + S(10);
    }
    for (int i = 0; i < kTabCount; ++i) {
        const ImVec2 ts = Measure(kFontBold, 21, kTabs[i]);
        const ImVec2 ta{x, a.y}, tb{x + ts.x + S(44), a.y + tab_h};
        ImGui::SetCursorScreenPos(ta);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", {tb.x - ta.x, tab_h}) && U.tab != i) {
            U.tab = i;
            U.scroll_top = true;  // with the mouse: no keyboard focus
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = U.tab == i;
        Text(bg, kFontBold, 21, {ta.x + S(22), ta.y + (tab_h - ts.y) * 0.5f}, C(kWhite, sel ? 1.0f : hov ? 0.9f : 0.55f), kTabs[i]);
        if (sel) bg->AddRectFilled({ta.x + S(14), tb.y - S(4)}, {tb.x - S(14), tb.y - S(1)}, C(kAccent), S(2));
        x = tb.x;
    }
    {
        const float gh = S(24), gy = a.y + (tab_h - gh) * 0.5f;
        x += S(6);
        if (pad) PadGlyph(bg, {x, gy}, gh, "RB");
        else Keycap(bg, {x, gy}, gh, "E", C(kWhite, 0.5f), 13);
    }
    // Icon buttons: quit game and close.
    {
        const float isz = S(42);
        ImGui::SetCursorScreenPos({b.x - S(16) - isz, a.y + (tab_h - isz) * 0.5f});
        if (IconButton("##close", 4, isz)) Back(ov);
        if (ImGui::IsItemHovered()) U.desc_hover = Desc{"Close", ov.from_start_.load() ? "Back to the start screen." : "Closes the menu and returns to the game (also with Esc).", false};
        ImGui::SetCursorScreenPos({b.x - S(16) - isz * 2 - S(10), a.y + (tab_h - isz) * 0.5f});
        const bool pr = ImGui::InvisibleButton("##quitgame", {isz, isz}, ImGuiButtonFlags_EnableNav);
        const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        const ImVec2 ia = ImGui::GetItemRectMin();
        const ImVec2 c{ia.x + isz * 0.5f, ia.y + isz * 0.5f};
        bg->AddRectFilled(ia, {ia.x + isz, ia.y + isz}, C(hov ? kDanger : kWhite, hov ? 0.22f : 0.05f), isz * 0.5f);
        // Power symbol: arc open at the top + line.
        const float r = isz * 0.23f, th = std::max(1.5f, S(2.2f));
        const ImU32 col = C(hov ? kDanger : kWhite, hov ? 1.0f : 0.8f);
        bg->PathArcTo(c, r, -IM_PI * 0.5f + 0.75f, IM_PI * 1.5f - 0.75f, 24);
        bg->PathStroke(col, 0, th);
        bg->AddLine({c.x, c.y - r * 1.25f}, {c.x, c.y - r * 0.2f}, col, th);
        if (pr) U.confirm_exit = true;
        if (ImGui::IsItemHovered()) U.desc_hover = Desc{"Quit game", "Closes the game (asks for confirmation).", false};
    }
    bg->AddLine({a.x, a.y + tab_h}, {b.x, a.y + tab_h}, C(kWhite, 0.1f), std::max(1.0f, S(1.1f)));
    // Content: list (left) and description (right)
    const float foot_h = S(62);
    const ImVec2 ca{a.x, a.y + tab_h + S(1)}, cb{b.x, b.y};
    bg->AddRectFilled(ca, cb, C({0, 0, 0, 1}, 0.30f), S(16), ImDrawFlags_RoundCornersBottom);
    const float list_w = (cb.x - ca.x) * 0.60f;
    ImGui::SetCursorScreenPos({ca.x + S(16), ca.y + S(12)});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(8), 0});
    if (ImGui::BeginChild("##list", {list_w - S(16), cb.y - ca.y - foot_h - S(12)}, ImGuiChildFlags_NavFlattened)) {
        if (U.focus_first || U.scroll_top) ImGui::SetScrollY(0);
        U.scroll_top = false;
        ImGui::Dummy({1, S(2)});
        if (U.focus_first) ImGui::SetKeyboardFocusHere(0);  // on open or tab change: focus the first option
        switch (U.tab) {
        case 0: TabGeneral(ov); break;
        case 1: TabGraphics(ov); break;
        case 2: TabDisplay(ov); break;
        case 3: TabControls(ov); break;
        case 4: TabSound(ov); break;
        default: TabEngine(ov); break;
        }
        ImGui::Dummy({1, S(16)});
        // Auto-scroll: the row focused with keyboard or controller stays in view (ImGui already does it while navigating).
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    U.focus_first = false;
    // Description (cleared on tab change)
    if (U.desc_tab != U.tab) {
        U.desc = Desc{};
        U.desc_tab = U.tab;
    }
    {
        const Desc& d = (NavVisible() && !U.desc_focus.title.empty()) ? U.desc_focus : (!U.desc_hover.title.empty() ? U.desc_hover : U.desc);
        if (!d.title.empty()) U.desc = d;
        const float dx = ca.x + list_w + S(12), dw = cb.x - dx - S(28);
        bg->AddLine({dx - S(6), ca.y + S(20)}, {dx - S(6), cb.y - foot_h - S(10)}, C(kWhite, 0.08f), std::max(1.0f, S(1.0f)));
        if (!U.desc.title.empty()) {
            float y = ca.y + S(28);
            Text(bg, kFontBold, 24, {dx + S(12), y}, C(kWhite), U.desc.title.c_str());
            y += Measure(kFontBold, 24, U.desc.title.c_str()).y + S(14);
            // With the yellow mark, an "Applies after restarting..." sentence in the text is redundant.
            std::string txt = U.desc.text;
            if (U.desc.restart) {
                size_t k = txt.find("Applies after restarting");
                if (k != std::string::npos) txt.erase(k);
                while (!txt.empty() && (txt.back() == '\n' || txt.back() == ' ')) txt.pop_back();
            }
            bg->AddText(g_ov_fonts[kFontReg], S(18), {dx + S(12), y}, C(kWhite, 0.72f), txt.c_str(), nullptr, dw - S(12));
            y += g_ov_fonts[kFontReg]->CalcTextSizeA(S(18), FLT_MAX, dw - S(12), txt.c_str()).y + S(18);
            if (U.desc.restart) {
                bg->AddCircleFilled({dx + S(18), y + S(10)}, S(4), C(kWarn), 12);
                Text(bg, kFontBold, 16, {dx + S(30), y}, C(kWarn, 0.95f), "Applies after restarting the game");
            }
        } else {
            Text(bg, kFontReg, 18, {dx + S(12), ca.y + S(28)}, C(kWhite, 0.5f), "Hover or focus an option to see its description.");
        }
    }
    // Footer: restart notice, saved notice and "Restore defaults".
    {
        const float fy = cb.y - foot_h;
        bg->AddLine({ca.x + S(16), fy}, {cb.x - S(16), fy}, C(kWhite, 0.08f), std::max(1.0f, S(1.0f)));
        float lx = ca.x + S(28);
        const float ty = fy + (foot_h - Measure(kFontReg, 17, "A").y) * 0.5f;
        if (U.op.RestartPending()) {
            bg->AddCircleFilled({lx + S(4), fy + foot_h * 0.5f}, S(4), C(kWarn), 12);
            Text(bg, kFontReg, 17, {lx + S(16), ty}, C(kWarn, 0.95f), "Some changes apply after restarting the game");
        } else {
            Text(bg, kFontReg, 17, {lx, ty}, C(kWhite, 0.45f), "Changes are saved when you close the menu.");
        }
        if (!U.saved_msg.empty() && ImGui::GetTime() - U.saved_t < 3.0) {
            const float al = (float)std::min(1.0, 3.0 - (ImGui::GetTime() - U.saved_t));
            const ImVec2 ts = Measure(kFontBold, 17, U.saved_msg.c_str());
            Text(bg, kFontBold, 17, {(ca.x + cb.x - ts.x) * 0.5f, ty}, C(kAccent2, al), U.saved_msg.c_str());
        }
        const char* lbl = "Restore defaults";
        const ImVec2 ts = Measure(kFontBold, 18, lbl);
        ImGui::SetCursorScreenPos({cb.x - S(28) - ts.x - S(40), fy + (foot_h - S(42)) * 0.5f});
        if (TextButton(lbl, kWhite, false)) {
            for (const std::string& id : TabIds(U.tab)) {
                U.op.Restore(id);
                Apply(ov, id);
            }
        }
        if (ImGui::IsItemHovered()) U.desc_hover = Desc{"Restore defaults", "Sets the options of this tab back to the recommended values.", false};
    }
    HelpBar(ov, bg, w * 0.5f, b.y + S(30), true);
}

void DrawStart(Overlay& ov, int w, int h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fw = (float)w, fh = (float)h;
    // Gradient: darker on the left (where the title is) and at the bottom.
    dl->AddRectFilledMultiColor({0, 0}, {fw, fh}, C({0.02f, 0.016f, 0.04f, 0.78f}), C({0.02f, 0.016f, 0.04f, 0.30f}),
                                C({0.02f, 0.016f, 0.04f, 0.55f}), C({0.02f, 0.016f, 0.04f, 0.88f}));
    // Decorative (abstract) rings behind the title.
    {
        const ImVec2 c{fw * 0.27f, fh * 0.46f};
        const float t = (float)ImGui::GetTime();
        for (int i = 0; i < 3; ++i) {
            const float r = fh * (0.26f + 0.08f * i);
            const float a0 = t * (0.10f + 0.05f * i) * (i % 2 ? -1.0f : 1.0f);
            for (int k = 0; k < 6; ++k) {
                const float s0 = a0 + k * (IM_PI / 3.0f);
                dl->PathArcTo(c, r, s0, s0 + IM_PI / 3.0f * 0.62f, 32);
                dl->PathStroke(C(kAccent, 0.10f - 0.025f * i), 0, std::max(1.0f, S(2.0f - 0.5f * i)));
            }
        }
    }
    const float lx = fw * 0.08f;
    float y = fh * 0.36f;
    Text(dl, kFontBlack, 84, {lx, y}, C(kWhite), "PLARecomp");
    y += Measure(kFontBlack, 84, "P").y + S(18);
    dl->AddRectFilled({lx, y}, {lx + S(120), y + S(3)}, C(kAccent), S(2));
    y += S(18);
    Text(dl, kFontReg, 20, {lx, y}, C(kWhite, 0.6f), "Native PC port · Vulkan renderer");
    // Menu on the right
    struct It {
        const char* t;
        int act;
    };
    static const It kItems[] = {{"Play", 0}, {"Options", 1}, {"Controls", 2}, {"Quit", 3}};
    const float rx = fw * 0.90f;
    float my = fh * 0.40f;
    for (const It& it : kItems) {
        const ImVec2 ts = Measure(kFontBold, 40, it.t);
        const float iw = ts.x + S(60), ih = ts.y + S(18);
        ImGui::SetCursorScreenPos({rx - iw, my});
        ImGui::PushID(it.act);
        if (U.focus_first && it.act == 0) ImGui::SetKeyboardFocusHere(0);
        const bool pr = ImGui::InvisibleButton("##it", {iw, ih}, ImGuiButtonFlags_EnableNav);
        const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && NavVisible();
        ImGui::PopID();
        const bool on = hov || foc;
        if (on) {
            dl->AddRectFilledMultiColor({rx - iw - S(40), my}, {rx + S(20), my + ih}, C(kAccent, 0.0f), C(kAccent, 0.22f),
                                        C(kAccent, 0.22f), C(kAccent, 0.0f));
            Text(dl, kFontBold, 40, {rx - ts.x - S(36), my + S(9)}, C(kAccent2), "•");
        }
        Text(dl, kFontBold, 40, {rx - ts.x, my + S(9)}, C(on ? kWhite : kWhite, on ? 1.0f : 0.75f), it.t);
        if (pr) {
            switch (it.act) {
            case 0: ov.mode_ = Overlay::kClosed; break;
            case 1:
            case 2:
                U.tab = it.act == 2 ? 3 : 0;
                ov.from_start_ = true;
                ov.mode_ = Overlay::kOptions;
                break;
            default: U.confirm_exit = true; break;
            }
        }
        my += ih + S(10);
    }
    U.focus_first = false;
    const std::string v = "v" + U.version;
    Text(dl, kFontReg, 16, {S(28), fh - S(28) - Measure(kFontReg, 16, "v").y}, C(kWhite, 0.45f), v.c_str());
    HelpBar(ov, dl, fw * 0.5f, fh - S(54), false);
}

void ApplyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 16;
    st.ChildRounding = 12;
    st.FrameRounding = 8;
    st.PopupRounding = 12;
    st.ScrollbarRounding = 6;
    st.GrabRounding = 6;
    st.WindowBorderSize = 0;
    st.WindowPadding = {0, 0};
    st.DisabledAlpha = 1.0f;  // disabled rows are dimmed by hand
    st.Colors[ImGuiCol_NavCursor] = kAccent;
    st.Colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    st.Colors[ImGuiCol_ScrollbarGrab] = {1, 1, 1, 0.18f};
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = {1, 1, 1, 0.3f};
    st.Colors[ImGuiCol_ScrollbarGrabActive] = kAccent;
    st.Colors[ImGuiCol_Text] = kWhite;
    st.ScaleAllSizes(U.s);
    st.FontSizeBase = 20.0f * U.s;
}
}  // namespace

void OverlayUiInit(Overlay& ov, const std::string& pla_ini, bool save, const std::string& version) {
    U.pla_ini = pla_ini;
    U.save = save;
    U.version = version.empty() ? std::string("nvnvk · ") + __DATE__ : version;
    U.hwnd = ov.hwnd_;
    U.op.Load(pla_ini);
    // Boot values of the other mods' settings (they read their mod.json when the game started).
    if (U.op.Available(SettingFile::Engine)) U.boot_tex_bias = U.op.GetF("mo_texture_lod_bias");
    if (U.op.Available(SettingFile::RenderRes))
        U.rr_reserve_w = std::max(std::atoi(U.op.Get("rr_width").c_str()), std::atoi(U.op.Get("rr_reserve").c_str()));
    SyncLive(ov);
    U.focus_first = true;
}

void OverlayUiShutdown() {}

void OverlayUiScript(Overlay& ov, const std::string& a) {
    // Test script actions (overlay_script): open, close, start, play, tab=N, dump=name, quit?, noquit, set=id:value,
    // save, key=Name (key press for ImGui navigation), pad=1/0 (icons), mouse=x,y (0..1), click.
    // Older scripts used Spanish action names; they are mapped to the English ones (kLegacy).
    const size_t eq = a.find('=');
    std::string k = a.substr(0, eq), v = eq == std::string::npos ? "" : a.substr(eq + 1);
    static const std::pair<const char*, const char*> kLegacy[] = {
        {"abrir", "open"}, {"cerrar", "close"}, {"inicio", "start"}, {"jugar", "play"}, {"salir?", "quit?"},
        {"nosalir", "noquit"}, {"guardar", "save"}, {"tecla", "key"}, {"raton", "mouse"}, {"clic", "click"},
        {"abajo", "down"}, {"arriba", "up"}, {"izquierda", "left"}, {"derecha", "right"}, {"intro", "enter"}};
    for (auto& [o, n] : kLegacy) {
        if (k == o) k = n;
        if (v == o) v = n;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (k == "open") {
        ov.from_start_ = false;
        ov.mode_ = Overlay::kOptions;
    } else if (k == "close") {
        ov.mode_ = Overlay::kClosed;
    } else if (k == "start") {
        ov.mode_ = Overlay::kStart;
    } else if (k == "play") {
        ov.mode_ = Overlay::kClosed;
    } else if (k == "tab") {
        U.tab = std::clamp(std::atoi(v.c_str()), 0, kTabCount - 1);
        U.focus_first = true;
    } else if (k == "dump") {
        ov.dump_req_ = v;
    } else if (k == "quit?") {
        U.confirm_exit = true;
    } else if (k == "noquit") {
        U.confirm_exit = false;
    } else if (k == "set") {
        const size_t c = v.find(':');
        if (c != std::string::npos) SetOpt(ov, v.substr(0, c), v.substr(c + 1));
    } else if (k == "save") {
        SaveNow();
    } else if (k == "pad") {
        ov.last_input_ = std::atoi(v.c_str());
    } else if (k == "key") {
        static const std::pair<const char*, ImGuiKey> kKeys[] = {
            {"down", ImGuiKey_DownArrow}, {"up", ImGuiKey_UpArrow}, {"left", ImGuiKey_LeftArrow},
            {"right", ImGuiKey_RightArrow}, {"enter", ImGuiKey_Enter}, {"q", ImGuiKey_Q}, {"e", ImGuiKey_E}};
        for (auto& [n, key] : kKeys)
            if (v == n) {
                io.AddKeyEvent(key, true);
                io.AddKeyEvent(key, false);
                ov.last_input_ = 0;
            }
    } else if (k == "mouse") {
        const size_t c = v.find(',');
        if (c != std::string::npos) io.AddMousePosEvent(std::atof(v.c_str()) * io.DisplaySize.x, std::atof(v.c_str() + c + 1) * io.DisplaySize.y);
    } else if (k == "click") {
        io.AddMouseButtonEvent(0, true);
        io.AddMouseButtonEvent(0, false);
    }
}

void OverlayUiFrame(Overlay& ov, HWND hwnd, int w, int h, double) {
    U.hwnd = hwnd;
    U.s = std::clamp(std::min(h / 1000.0f, w / 1600.0f), 0.62f, 3.0f);
    if (U.s != U.style_s) {
        ApplyStyle();
        U.style_s = U.s;
    }
    ImGui::GetStyle().Alpha = Ease(ov.Blend());
    // Screen changes (open/close): reload options, focus, save when leaving the options.
    const int mode = ov.mode_.load();
    if (mode != U.prev_mode) {
        if (U.prev_mode == Overlay::kOptions && mode != Overlay::kOptions) {
            EndCapture(ov);
            SaveNow();
        }
        if (mode == Overlay::kOptions && U.prev_mode != Overlay::kOptions) {
            if (!ov.from_start_.load() && U.prev_mode != -1) U.tab = U.tab;  // remembers the last tab
            U.op.Load(U.pla_ini);
            SyncLive(ov);
            U.mods = U.op.Mods();
            U.desc = U.desc_hover = U.desc_focus = Desc{};
        }
        if (mode != Overlay::kClosed) {
            U.focus_first = true;
            U.drawn_mode = mode;
        }
        U.prev_mode = mode;
    }
    ProcessCapture(ov);
    // Back: Esc (WndProc) or controller B.
    int backs = ov.esc_pending_.exchange(0);
    if (mode != Overlay::kClosed && U.capture_id.empty() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ++backs;
    for (int i = 0; i < backs; ++i) Back(ov);
    // Tabs with Q/E or LB/RB.
    if (ov.mode_.load() == Overlay::kOptions && U.capture_id.empty() && !U.confirm_exit) {
        int d = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) d = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) d = 1;
        if (d) {
            U.tab = (U.tab + d + kTabCount) % kTabCount;
            U.focus_first = true;
        }
    }
    U.desc_hover = Desc{};
    U.desc_focus = Desc{};
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)w, (float)h});
    ImGui::Begin("##menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    const bool closing = ov.mode_.load() == Overlay::kClosed;
    if (closing) ImGui::BeginDisabled();
    const int dm = closing ? U.drawn_mode : ov.mode_.load();
    if (dm == Overlay::kStart) DrawStart(ov, w, h);
    else DrawOptions(ov, w, h);
    if (closing) ImGui::EndDisabled();
    ConfirmExit(ov, w, h);
    ImGui::End();
}

}  // namespace nvnvk
