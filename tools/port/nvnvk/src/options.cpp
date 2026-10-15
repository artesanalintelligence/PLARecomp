// In-game menu options: JSON/INI editing and the settings catalog (see options.h).
// SPDX-License-Identifier: MIT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>

#include "nvnvk.h"

namespace nvnvk {
namespace fs = std::filesystem;

namespace {
bool LoadFileText(const std::wstring& path, std::string* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    *out = std::move(s);
    return true;
}

bool StoreFileText(const std::wstring& path, const std::string& s) {
    const std::wstring tmp = path + L".tmp-menu";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    std::fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- reading JSON with comments, keeping positions ----
size_t SkipBlanks(const std::string& t, size_t p) {
    while (p < t.size()) {
        const char c = t[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++p;
        } else if (c == '/' && p + 1 < t.size() && t[p + 1] == '/') {
            while (p < t.size() && t[p] != '\n') ++p;
        } else if (c == '/' && p + 1 < t.size() && t[p + 1] == '*') {
            const size_t e = t.find("*/", p + 2);
            p = e == std::string::npos ? t.size() : e + 2;
        } else {
            break;
        }
    }
    return p;
}

// p at the opening quote; returns the position after the closing quote.
size_t StringEnd(const std::string& t, size_t p, std::string* content) {
    std::string out;
    for (++p; p < t.size() && t[p] != '"'; ++p) {
        if (t[p] == '\\' && p + 1 < t.size()) {
            ++p;
            const char e = t[p];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
            out += t[p];
        }
    }
    if (content) *content = out;
    return p < t.size() ? p + 1 : p;
}

size_t ValueEnd(const std::string& t, size_t p) {
    if (p >= t.size()) return p;
    if (t[p] == '"') return StringEnd(t, p, nullptr);
    if (t[p] == '{' || t[p] == '[') {
        int level = 0;
        while (p < t.size()) {
            const char c = t[p];
            if (c == '"') {
                p = StringEnd(t, p, nullptr);
                continue;
            }
            if (c == '/' && p + 1 < t.size() && (t[p + 1] == '/' || t[p + 1] == '*')) {
                p = SkipBlanks(t, p);
                continue;
            }
            if (c == '{' || c == '[') ++level;
            if (c == '}' || c == ']') {
                if (--level == 0) return p + 1;
            }
            ++p;
        }
        return p;
    }
    while (p < t.size() && t[p] != ',' && t[p] != '}' && t[p] != ']' && t[p] != ' ' && t[p] != '\t' && t[p] != '\r' &&
           t[p] != '\n' && t[p] != '/')
        ++p;
    return p;
}

std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

bool IsLiteral(const std::string& v) {
    if (v == "true" || v == "false" || v == "null") return true;
    if (v.empty()) return false;
    char* e = nullptr;
    std::strtod(v.c_str(), &e);
    return e && *e == 0;
}

size_t LineStart(const std::string& t, size_t p) {
    while (p > 0 && t[p - 1] != '\n') --p;
    return p;
}

std::string Indent(const std::string& t, size_t p) {
    size_t b = LineStart(t, p), e = b;
    while (e < t.size() && (t[e] == ' ' || t[e] == '\t')) ++e;
    return t.substr(b, e - b);
}

std::string NewlineOf(const std::string& t) { return t.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }
}  // namespace

// ------------------------------------------------------------------ JsonText
bool JsonText::Load(const std::wstring& path) {
    valid_ = false;
    if (!LoadFileText(path, &text)) return false;
    bom_ = text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF;
    if (bom_) text.erase(0, 3);
    const size_t p = SkipBlanks(text, 0);
    valid_ = p < text.size() && text[p] == '{' && ValueEnd(text, p) <= text.size();
    return valid_;
}

bool JsonText::Save(const std::wstring& path) const {
    return StoreFileText(path, bom_ ? "\xEF\xBB\xBF" + text : text);
}

bool JsonText::Find(const std::vector<std::string>& path, size_t* vb, size_t* ve, size_t* obj_b, size_t* obj_e,
                    size_t* last_end, size_t* level) const {
    const std::string& t = text;
    size_t ob = SkipBlanks(t, 0);
    if (ob >= t.size() || t[ob] != '{') return false;
    for (size_t i = 0; i < path.size(); ++i) {
        size_t p = ob + 1, last = std::string::npos;
        bool found = false;
        for (;;) {
            p = SkipBlanks(t, p);
            if (p >= t.size()) return false;
            if (t[p] == '}') break;
            if (t[p] != '"') return false;
            std::string key;
            p = StringEnd(t, p, &key);
            p = SkipBlanks(t, p);
            if (p >= t.size() || t[p] != ':') return false;
            const size_t b = SkipBlanks(t, p + 1), e = ValueEnd(t, b);
            last = e;
            if (key == path[i]) {
                if (i + 1 == path.size()) {
                    *vb = b;
                    *ve = e;
                    return true;
                }
                if (t[b] != '{') return false;
                ob = b;
                found = true;
                break;
            }
            p = SkipBlanks(t, e);
            if (p < t.size() && t[p] == ',') ++p;
        }
        if (!found) {
            *obj_b = ob;
            *obj_e = p;
            *last_end = last;
            *level = i;
            return false;
        }
    }
    return false;
}

bool JsonText::Raw(const std::vector<std::string>& path, std::string* v) const {
    size_t vb, ve, ob, oe, le, lv;
    if (!valid_ || !Find(path, &vb, &ve, &ob, &oe, &le, &lv)) return false;
    *v = text.substr(vb, ve - vb);
    return true;
}

std::string JsonText::Read(const std::vector<std::string>& path, const std::string& def) const {
    std::string v;
    if (!Raw(path, &v)) return def;
    if (!v.empty() && v[0] == '"') {
        std::string c;
        StringEnd(v, 0, &c);
        return c;
    }
    return v;
}

bool JsonText::Put(const std::vector<std::string>& path, const std::string& value, bool quoted) {
    if (!valid_) return false;
    size_t vb = 0, ve = 0, ob = 0, oe = 0, le = std::string::npos, lv = 0;
    if (Find(path, &vb, &ve, &ob, &oe, &le, &lv)) {
        const bool was_string = text[vb] == '"';
        const bool quotes = was_string || !IsLiteral(value);
        text.replace(vb, ve - vb, quotes ? Quote(value) : value);
        return true;
    }
    if (oe >= text.size() || text[oe] != '}') return false;
    std::string v = quoted || !IsLiteral(value) ? Quote(value) : value;
    for (size_t k = path.size() - 1; k > lv; --k) v = "{ " + Quote(path[k]) + ": " + v + " }";
    const std::string member = Quote(path[lv]) + ": " + v;
    const std::string nl = NewlineOf(text);
    const size_t ls = LineStart(text, oe);
    const bool only_blank = text.find_first_not_of(" \t", ls) == oe;
    if (only_blank && ls > ob) {
        std::string ind = Indent(text, oe) + "  ";
        if (le != std::string::npos) {
            // Indentation of the last member (looks for the start of its line).
            ind = Indent(text, le - 1);
        }
        text.insert(ls, ind + member + nl);
        if (le != std::string::npos) text.insert(le, ",");
    } else {
        text.insert(oe, (le != std::string::npos ? ", " : " ") + member + " ");
    }
    return true;
}

// ------------------------------------------------------------------ IniText
bool IniText::Load(const std::wstring& path) { return LoadFileText(path, &text); }
bool IniText::Save(const std::wstring& path) const { return StoreFileText(path, text); }

namespace {
bool KeyLine(const std::string& t, size_t b, size_t e, const std::string& key, size_t* vb, size_t* ve) {
    size_t p = b;
    while (p < e && (t[p] == ' ' || t[p] == '\t')) ++p;
    if (p >= e || t[p] == ';' || t[p] == '#' || t[p] == '[') return false;
    const size_t eq = t.find('=', p);
    if (eq == std::string::npos || eq >= e) return false;
    size_t ke = eq;
    while (ke > p && (t[ke - 1] == ' ' || t[ke - 1] == '\t')) --ke;
    if (_stricmp(t.substr(p, ke - p).c_str(), key.c_str()) != 0) return false;
    *vb = eq + 1;
    size_t x = e;
    while (x > *vb && (t[x - 1] == '\r' || t[x - 1] == '\n')) --x;
    *ve = x;
    return true;
}
}  // namespace

std::string IniText::Read(const std::string& key, const std::string& def) const {
    size_t b = 0;
    while (b < text.size()) {
        size_t e = text.find('\n', b);
        e = e == std::string::npos ? text.size() : e + 1;
        size_t vb, ve;
        if (KeyLine(text, b, e, key, &vb, &ve)) {
            std::string v = text.substr(vb, ve - vb);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
            return v;
        }
        b = e;
    }
    return def;
}

void IniText::Update(const std::string& key, const std::string& value) {
    size_t b = 0;
    while (b < text.size()) {
        size_t e = text.find('\n', b);
        e = e == std::string::npos ? text.size() : e + 1;
        size_t vb, ve;
        if (KeyLine(text, b, e, key, &vb, &ve)) {
            text.replace(vb, ve - vb, value);
            e = vb + value.size();
            const size_t nl = text.find('\n', e);
            e = nl == std::string::npos ? text.size() : nl + 1;
        }
        b = e;
    }
}

void IniText::Put(const std::string& key, const std::string& value) {
    size_t b = 0, section_end = std::string::npos;
    bool in_pla = false;
    while (b < text.size()) {
        size_t e = text.find('\n', b);
        e = e == std::string::npos ? text.size() : e + 1;
        size_t vb, ve;
        if (KeyLine(text, b, e, key, &vb, &ve)) {
            text.replace(vb, ve - vb, value);
            return;
        }
        const size_t p = text.find_first_not_of(" \t", b);
        if (p < e && text[p] == '[') {
            in_pla = text.compare(p, 5, "[PLA]") == 0;
        } else if (in_pla && p < e && text[p] != '\r' && text[p] != '\n') {
            section_end = e;
        }
        b = e;
    }
    const std::string nl = NewlineOf(text);
    std::string line = key + "=" + value + nl;
    if (section_end == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += nl;
        text += line;
    } else {
        if (section_end == text.size() && !text.empty() && text.back() != '\n') text += nl, section_end = text.size();
        text.insert(section_end, line);
    }
}

// ------------------------------------------------------------------ Options
void Options::Def(const std::string& id, SettingFile f, std::vector<std::string> path, const std::string& def, NewType t,
                  bool live) {
    Setting x;
    x.id = id;
    x.file = f;
    x.path = std::move(path);
    x.def = def;
    x.type = t;
    x.live = live;
    const Source& s = sources_[(int)f];
    std::string raw;
    if (s.available && s.ini) {
        // English key first, then its legacy names.
        std::string v = "\x01";
        for (size_t k = 0; k < x.path.size() && v == "\x01"; ++k) v = s.initxt.Read(x.path[k], "\x01");
        x.exists = v != "\x01";
        x.value = x.exists ? v : def;
    } else if (s.available && s.json.Raw(x.path, &raw)) {
        x.exists = true;
        x.value = s.json.Read(x.path, def);
    } else {
        x.value = def;
    }
    x.original = x.value;
    a_[id] = x;
}

void Options::Load(const std::string& pla_ini_cfg) {
    a_.clear();
    {
        HMODULE m = nullptr;
        wchar_t buf[MAX_PATH * 2] = {};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&KeyName), &m);
        GetModuleFileNameW(m, buf, (DWORD)std::size(buf));
        dir_nvnvk = fs::path(buf).parent_path().wstring();
        dir_mods = fs::path(dir_nvnvk).parent_path().wstring();
    }
    auto json = [&](SettingFile f, const std::wstring& path) {
        Source& s = sources_[(int)f];
        s = Source{};
        s.path = path;
        s.available = s.json.Load(path);
    };
    // Engine options mod: "engine_options", or its legacy folder name when only that one is installed.
    engine_mod = "engine_options";
    if (!fs::exists(dir_mods + L"\\engine_options\\mod.json") && fs::exists(dir_mods + L"\\motor_opciones\\mod.json"))
        engine_mod = "motor_opciones";
    json(SettingFile::Nvnvk, dir_nvnvk + L"\\mod.json");
    json(SettingFile::RenderRes, dir_mods + L"\\render_res\\mod.json");
    json(SettingFile::Engine, dir_mods + L"\\" + Wide(engine_mod) + L"\\mod.json");
    json(SettingFile::Nnshim, dir_mods + L"\\nnshim\\mod.json");
    json(SettingFile::Islands, dir_mods + L"\\islands\\mod.json");
    // Controls file: the path configured in the nnshim (config.hid.controls_file); otherwise the default file next to
    // it (controls.json, or the legacy controles.json).
    controls_path = sources_[(int)SettingFile::Nnshim].json.Read({"config", "hid", "controls_file"}, "");
    std::wstring ctl = Wide(controls_path);
    if (ctl.empty()) {
        for (const wchar_t* n : {L"\\nnshim\\controls.json", L"\\nnshim\\controles.json"})
            if (fs::exists(dir_mods + n)) {
                ctl = dir_mods + n;
                break;
            }
    }
    controls_path = Utf8(ctl);
    json(SettingFile::Controls, ctl);
    {
        Source& s = sources_[(int)SettingFile::PlaIni];
        s = Source{};
        s.ini = true;
        if (pla_ini_cfg == "none") {
            s.available = false;
        } else {
            std::wstring r = Wide(pla_ini_cfg);
            if (r.empty()) {
                wchar_t buf[MAX_PATH * 2] = {};
                GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
                r = fs::path(buf).parent_path().wstring() + L"\\PLA.ini";
            }
            s.path = r;
            s.available = s.initxt.Load(r);
        }
    }
    // nnshim session templates (the play scripts copy them over its mod.json on every session): the mod-play*.json
    // (legacy: mod-jugar*.json) in the controls file's folder when it is outside the game folder (same as the launcher).
    if (!ctl.empty()) {
        std::error_code ec;
        const fs::path dir = fs::path(ctl).parent_path();
        const std::wstring game = fs::path(dir_mods).parent_path().parent_path().wstring();
        const std::wstring d = dir.wstring();
        if (_wcsnicmp(d.c_str(), game.c_str(), game.size()) != 0) {
            for (auto& e : fs::directory_iterator(dir, ec)) {
                const std::wstring n = e.path().filename().wstring();
                const bool tmpl = _wcsnicmp(n.c_str(), L"mod-play", 8) == 0 || _wcsnicmp(n.c_str(), L"mod-jugar", 9) == 0;
                if (tmpl && e.path().extension() == L".json")
                    sources_[(int)SettingFile::Nnshim].mirrors.push_back(e.path().wstring());
            }
        }
    }

    using F = SettingFile;
    using T = NewType;
    // General
    Def("overlay_start", F::Nvnvk, {"config", "overlay_start"}, "true", T::String, true);
    Def("overlay_blur", F::Nvnvk, {"config", "overlay_blur"}, "true", T::String, true);
    Def("language", F::Nnshim, {"config", "settings", "language"}, "host", T::String, false);
    // Graphics (nvnvk)
    Def("preset", F::Nvnvk, {"config", "preset"}, "auto", T::String, true);
    Def("aa", F::Nvnvk, {"config", "aa"}, "fxaa", T::String, true);
    Def("aa_hud", F::Nvnvk, {"config", "aa_hud"}, "true", T::String, true);
    Def("sharpen", F::Nvnvk, {"config", "sharpen"}, "0", T::String, true);
    Def("upscale", F::Nvnvk, {"config", "upscale"}, "sharp", T::String, true);
    Def("anisotropy", F::Nvnvk, {"config", "anisotropy"}, "16", T::String, true);
    // Internal resolution: live up to the headroom reserved at boot (overlay_ui.cpp updates the flag).
    Def("rr_width", F::RenderRes, {"config", "width"}, "1920", T::String, true);
    Def("rr_height", F::RenderRes, {"config", "height"}, "1080", T::String, true);
    Def("rr_dynamic", F::RenderRes, {"config", "dynamic_res"}, "true", T::Bool, true);
    Def("rr_reserve", F::RenderRes, {"config", "reserve_width"}, "0", T::String, false);
    // Display
    Def("vsync", F::Nvnvk, {"config", "vsync"}, "false", T::Bool, true);
    Def("fps_limit", F::Nvnvk, {"config", "fps_limit"}, "0", T::String, true);
    Def("show_fps", F::Nvnvk, {"config", "show_fps"}, "false", T::String, true);
    Def("brightness", F::Nvnvk, {"config", "brightness"}, "0", T::String, true);
    Def("contrast", F::Nvnvk, {"config", "contrast"}, "1", T::String, true);
    Def("saturation", F::Nvnvk, {"config", "saturation"}, "1", T::String, true);
    Def("window_width", F::Nvnvk, {"config", "window_width"}, "1280", T::String, true);
    Def("window_height", F::Nvnvk, {"config", "window_height"}, "720", T::String, true);
    // PLA.ini keys read by PLA.exe (fullscreen and nvnvk FPS cap): their names are defined by the runtime.
    // English name first; the legacy name is still read and kept in sync when present.
    Def("ini_fullscreen", F::PlaIni, {"fullscreen", "pantalla_completa"}, "0", T::String, true);
    Def("ini_fps_cap", F::PlaIni, {"nvnvk_fps_cap", "tope_fps_nvnvk"}, "0", T::String, true);
    // Controls (the nnshim's controls file)
    static const char* kBtn[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "StickL", "StickR",
                                 "Up", "Down", "Left", "Right", "LStickUp", "LStickDown", "LStickLeft", "LStickRight",
                                 "RStickUp", "RStickDown", "RStickLeft", "RStickRight"};
    static const char* kDef[] = {"RETURN", "BACK", "LSHIFT", "SPACE", "Q", "E", "1", "3", "TAB", "M", "Z", "C",
                                 "UP", "DOWN", "LEFT", "RIGHT", "W", "S", "A", "D", "I", "K", "J", "L"};
    Def("keyboard", F::Controls, {"keyboard"}, "true", T::Bool, true);
    for (int i = 0; i < 24; ++i) Def(std::string("key_") + kBtn[i], F::Controls, {"keyboard_map", kBtn[i]}, kDef[i], T::String, true);
    Def("mouse_enabled", F::Controls, {"mouse", "enabled"}, "auto", T::String, true);
    Def("mouse_sensitivity", F::Controls, {"mouse", "sensitivity"}, "1.0", T::Number, true);
    Def("mouse_invert_x", F::Controls, {"mouse", "invert_x"}, "false", T::Bool, true);
    Def("mouse_invert_y", F::Controls, {"mouse", "invert_y"}, "false", T::Bool, true);
    Def("ui_button_prompts", F::Controls, {"ui", "button_prompts"}, "auto", T::String, false);
    Def("ui_mouse_menus", F::Controls, {"ui", "mouse_menus"}, "true", T::Bool, true);
    // Sound (nnshim)
    Def("audio_volume", F::Nnshim, {"config", "audio", "volume"}, "1", T::Number, true);
    Def("audio_backend", F::Nnshim, {"config", "audio", "backend"}, "xaudio2", T::String, false);
    Def("audio_mute", F::Nnshim, {"config", "audio", "mute"}, "false", T::Bool, true);
    // Engine (engine_options)
    Def("mo_fov", F::Engine, {"config", "fov"}, "1.0", T::String, true);
    Def("mo_lod", F::Engine, {"config", "lod"}, "1.0", T::String, false);
    Def("mo_texture_lod_bias", F::Engine, {"config", "texture_lod_bias"}, "0", T::String, true);
    Def("mo_anim_full_rate", F::Engine, {"config", "anim_full_rate"}, "false", T::String, false);
    Def("mo_bloom", F::Engine, {"config", "bloom"}, "true", T::String, true);
    Def("mo_dof", F::Engine, {"config", "dof"}, "true", T::String, true);
    Def("mo_chromatic_aberration", F::Engine, {"config", "chromatic_aberration"}, "true", T::String, true);
    Def("mo_hud_toggle_key", F::Engine, {"config", "hud_toggle_key"}, "", T::String, false);
    Def("mo_hud_hidden", F::Engine, {"config", "hud_hidden"}, "false", T::String, true);
    // CPU work of the game (FPS-4): "auto" = the CPU preset; the preset's "auto" = by detected CPU class.
    Def("mo_cpu_preset", F::Engine, {"config", "cpu_preset"}, "auto", T::String, false);
    Def("mo_npc_density", F::Engine, {"config", "npc_density"}, "auto", T::String, false);
    Def("mo_anim_lod", F::Engine, {"config", "anim_lod"}, "auto", T::String, false);
    // terrain_detail / grass_detail / ambient_voices stay advanced engine_options keys (no measured gain; not in the menu).
    // Native islands mod (FPS-4): the whole mod on/off (its hooks are bound at boot).
    Def("islands_enabled", F::Islands, {"enabled"}, "true", T::Bool, false);
}

const std::string& Options::Get(const std::string& id) const {
    static const std::string empty;
    auto it = a_.find(id);
    return it == a_.end() ? empty : it->second.value;
}
float Options::GetF(const std::string& id) const { return (float)std::atof(Get(id).c_str()); }
bool Options::GetB(const std::string& id) const {
    const std::string& v = Get(id);
    return v == "true" || v == "1";
}

void Options::Set(const std::string& id, const std::string& v) {
    auto it = a_.find(id);
    if (it == a_.end()) return;
    it->second.value = v;
}

void Options::Restore(const std::string& id) {
    auto it = a_.find(id);
    if (it != a_.end()) it->second.value = it->second.def;
}

void Options::Base(const std::string& id, const std::string& v) {
    auto it = a_.find(id);
    if (it != a_.end()) it->second.value = it->second.original = v;
}

std::string Options::Default(const std::string& id) const {
    auto it = a_.find(id);
    return it == a_.end() ? std::string() : it->second.def;
}

bool Options::Changed(const std::string& id) const {
    auto it = a_.find(id);
    return it != a_.end() && it->second.value != it->second.original;
}

std::string Options::LiveKey(const std::string& id) const {
    auto it = a_.find(id);
    if (it == a_.end()) return {};
    const Setting& a = it->second;
    const char* mod = a.file == SettingFile::RenderRes ? "render_res"
                      : a.file == SettingFile::Engine  ? "engine_options"
                      : a.file == SettingFile::Nnshim  ? "nnshim"
                                                       : nullptr;
    if (!mod || a.path.size() < 2 || a.path[0] != "config") return {};
    std::string k = mod;
    for (size_t i = 1; i < a.path.size(); ++i) k += "." + a.path[i];
    return k;
}

void Options::SetLive(const std::string& id, bool live) {
    auto it = a_.find(id);
    if (it != a_.end()) it->second.live = live;
}

bool Options::RestartPending() const {
    if (restart_saved_) return true;
    for (auto& [id, a] : a_)
        if (!a.live && a.value != a.original) return true;
    return false;
}

int Options::Save(bool write, std::string* summary) {
    int written = 0;
    for (int fi = 0; fi < (int)SettingFile::Count; ++fi) {
        Source& s = sources_[fi];
        std::vector<Setting*> changes;
        for (auto& [id, a] : a_)
            if ((int)a.file == fi && a.value != a.original) changes.push_back(&a);
        if (changes.empty()) continue;
        if (!s.available) continue;
        auto apply_json = [&](JsonText& j) {
            for (Setting* a : changes) j.Put(a->path, a->value, a->type == NewType::String);
        };
        if (s.ini) {
            // Writes the English key; a legacy line (older PLA.ini) gets the same value, so any PLA.exe agrees.
            for (Setting* a : changes) {
                s.initxt.Put(a->path[0], a->value);
                for (size_t k = 1; k < a->path.size(); ++k) s.initxt.Update(a->path[k], a->value);
            }
        } else {
            apply_json(s.json);
        }
        if (summary) {
            *summary += Utf8(s.path) + ":";
            for (Setting* a : changes) *summary += " " + a->id + "=" + a->value;
            *summary += "\n";
        }
        if (write) {
            const std::wstring bak = s.path + L".bak-menu";
            if (!fs::exists(bak)) CopyFileW(s.path.c_str(), bak.c_str(), TRUE);
            const bool ok = s.ini ? s.initxt.Save(s.path) : s.json.Save(s.path);
            if (ok) ++written;
            else NVK_WARN("menu: could not write %s", Utf8(s.path).c_str());
            for (const std::wstring& e : s.mirrors) {
                JsonText j;
                if (!j.Load(e)) continue;
                apply_json(j);
                if (j.Save(e)) ++written;
            }
        }
        for (Setting* a : changes) {
            if (!a->live) restart_saved_ = true;
            if (fi == (int)SettingFile::Controls) regen_keys = true;
            a->original = a->value;
            a->exists = true;
        }
    }
    return written;
}

std::vector<std::pair<std::string, bool>> Options::Mods() const {
    std::vector<std::pair<std::string, bool>> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir_mods, ec)) {
        if (!e.is_directory()) continue;
        JsonText j;
        if (!j.Load((e.path() / L"mod.json").wstring())) continue;
        const bool on = j.Read({"enabled"}, "true") != "false" && !fs::exists(e.path() / L"disabled");
        out.emplace_back(Utf8(e.path().filename().wstring()), on);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------------ keys
std::string KeyName(int vk, bool extended) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUMPAD" + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_RETURN: return "RETURN";
    case VK_BACK: return "BACK";
    case VK_TAB: return "TAB";
    case VK_ESCAPE: return "ESCAPE";
    case VK_SPACE: return "SPACE";
    case VK_LEFT: return "LEFT";
    case VK_UP: return "UP";
    case VK_RIGHT: return "RIGHT";
    case VK_DOWN: return "DOWN";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PAGEUP";
    case VK_NEXT: return "PAGEDOWN";
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    case VK_OEM_COMMA: return "COMMA";
    case VK_OEM_PERIOD: return "PERIOD";
    case VK_OEM_MINUS: return "MINUS";
    case VK_OEM_PLUS: return "PLUS";
    case VK_LSHIFT: return "LSHIFT";
    case VK_RSHIFT: return "RSHIFT";
    case VK_CONTROL: return extended ? "RCTRL" : "LCTRL";
    case VK_LCONTROL: return "LCTRL";
    case VK_RCONTROL: return "RCTRL";
    case VK_MENU: return extended ? "RALT" : "LALT";
    case VK_LMENU: return "LALT";
    case VK_RMENU: return "RALT";
    default: break;
    }
    char b[16];
    std::snprintf(b, sizeof b, "VK_0x%02X", vk & 0xFF);
    return b;
}

std::string KeyLabel(const std::string& n) {
    static const std::pair<const char*, const char*> k[] = {
        {"RETURN", "Enter"}, {"ENTER", "Enter"}, {"BACK", "Backspace"}, {"BACKSPACE", "Backspace"}, {"TAB", "Tab"},
        {"ESCAPE", "Esc"}, {"ESC", "Esc"}, {"SPACE", "Space"}, {"HOME", "Home"}, {"END", "End"},
        {"PAGEUP", "Page Up"}, {"PRIOR", "Page Up"}, {"PAGEDOWN", "Page Down"}, {"NEXT", "Page Down"}, {"INSERT", "Insert"},
        {"DELETE", "Delete"}, {"COMMA", ","}, {"PERIOD", "."}, {"MINUS", "-"}, {"PLUS", "+"}, {"LSHIFT", "Left Shift"},
        {"RSHIFT", "Right Shift"}, {"SHIFT", "Shift"}, {"LCTRL", "Left Ctrl"}, {"LCONTROL", "Left Ctrl"},
        {"RCTRL", "Right Ctrl"}, {"RCONTROL", "Right Ctrl"}, {"LALT", "Left Alt"}, {"LMENU", "Left Alt"},
        {"RALT", "Right Alt"}, {"RMENU", "Right Alt"}, {"UP", "Up"}, {"DOWN", "Down"}, {"LEFT", "Left"},
        {"RIGHT", "Right"}};
    std::string u = n;
    for (auto& c : u) c = (char)toupper((unsigned char)c);
    for (auto& [a, b] : k)
        if (u == a) return b;
    if (u.rfind("NUMPAD", 0) == 0) return "Num " + u.substr(6);
    return n;
}

// ------------------------------------------------------------------ live settings channel
namespace {
std::mutex g_live_mu;
std::map<std::string, std::string> g_live;
std::atomic<uint32_t> g_live_version{0};
}  // namespace

void LivePublish(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        g_live[key] = value;
        // Engine options mod builds that still poll their legacy key prefix ("motor_opciones.").
        static const std::string kEngine = "engine_options.";
        if (key.compare(0, kEngine.size(), kEngine) == 0) g_live["motor_opciones." + key.substr(kEngine.size())] = value;
    }
    g_live_version.fetch_add(1);
    NVK_INFO("live: %s = %s", key.c_str(), value.c_str());
}

void LiveSaved() { g_live_version.fetch_add(1); }

bool g_auto_perf = false;

const std::vector<std::pair<std::string, std::string>>& PresetValues(const std::string& preset) {
    // Performance: lower internal resolution with dynamic resolution, no anisotropic filtering, final-pass effects off
    // (FXAA, sharpening, Lanczos), no depth of field. Quality: the shipped defaults.
    static const std::vector<std::pair<std::string, std::string>> kPerf = {
        {"aa", "off"}, {"sharpen", "0"}, {"upscale", "bilinear"}, {"anisotropy", "0"},
        {"rr_width", "1280"}, {"rr_height", "720"}, {"rr_dynamic", "true"}, {"mo_dof", "false"}};
    static const std::vector<std::pair<std::string, std::string>> kQuality = {
        {"aa", "fxaa"}, {"sharpen", "0"}, {"upscale", "sharp"}, {"anisotropy", "16"},
        {"rr_width", "1920"}, {"rr_height", "1080"}, {"rr_dynamic", "true"}, {"mo_dof", "true"}};
    static const std::vector<std::pair<std::string, std::string>> kNone;
    return preset == "performance" ? kPerf : preset == "quality" ? kQuality : kNone;
}

void ApplyAutoPreset(bool low_end) {
    if (g_cfg.preset != "auto" || !low_end) return;
    g_auto_perf = true;
    g_cfg.aa = 0;
    g_cfg.sharpen = 0;
    g_cfg.upscale_sharp = false;
    g_cfg.anisotropy = 0;
    // render_res lowers the resolution live (the targets were reserved at the configured size); engine_options turns
    // depth of field off on its next poll.
    LivePublish("render_res.width", "1280");
    LivePublish("render_res.height", "720");
    LivePublish("render_res.dynamic_res", "true");
    LivePublish("engine_options.dof", "false");
    NVK_INFO("preset auto: low-end GPU -> Performance (1280x720 dynamic, no AA/sharpening/anisotropy, no depth of field)");
}

}  // namespace nvnvk

// Exports for the other mods (looked up with GetModuleHandle("nvnvk.dll") + GetProcAddress).
namespace nvnvk {
std::atomic<uint32_t> g_present_crop{0};
}

// nvnvk_live_report: a mod tells nvnvk something about the game's state. Known key: "render_res.screen" = "WxH", the
// part of the presented texture the game draws to (top left); nvnvk presents only that part.
extern "C" __declspec(dllexport) void nvnvk_live_report(const char* key, const char* value) {
    if (!key || !value) return;
    if (std::strcmp(key, "render_res.screen") == 0) {
        int w = 0, h = 0;
        if (std::sscanf(value, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w < 65536 && h < 65536) {
            nvnvk::g_present_crop.store(((uint32_t)w << 16) | (uint32_t)h);
            NVK_INFO("live: presented area %dx%d (render_res)", w, h);
        }
    }
}

// nvnvk_live_version: changes whenever the menu changes a published setting or saves the files.
extern "C" __declspec(dllexport) uint32_t nvnvk_live_version(void) { return nvnvk::g_live_version.load(); }

// nvnvk_live_get: copies the current value of `key` (NUL-terminated, truncated to cap-1) into `out` and returns its
// length, or -1 if the menu has not published that key in this session (the mod keeps its own value).
extern "C" __declspec(dllexport) int nvnvk_live_get(const char* key, char* out, int cap) {
    if (!key || !out || cap <= 0) return -1;
    std::lock_guard<std::mutex> lk(nvnvk::g_live_mu);
    auto it = nvnvk::g_live.find(key);
    if (it == nvnvk::g_live.end()) return -1;
    const int n = (int)std::min<size_t>(it->second.size(), (size_t)cap - 1);
    std::memcpy(out, it->second.data(), (size_t)n);
    out[n] = 0;
    return n;
}
