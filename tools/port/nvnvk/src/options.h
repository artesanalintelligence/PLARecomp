// In-game menu options (overlay): settings catalog, which file each setting lives in and how it is saved.
// Same model as the launcher (tools/launcher/gui): nvnvk's mod.json, render_res and engine_options, the nnshim's
// controls file and mod.json (plus its session templates) and PLA.ini. Edits are "surgical": only the text of the
// changed value is rewritten (comments, order and formatting stay intact).
// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <string>
#include <vector>

namespace nvnvk {

// JSON text (accepts // and /* */ comments) edited by key path.
class JsonText {
public:
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;  // temp file + replace; keeps the BOM and line endings
    bool Valid() const { return valid_; }
    // Raw value (quoted if it is a string). False if the path does not exist.
    bool Raw(const std::vector<std::string>& path, std::string* v) const;
    // Value as text (unquoted if it is a string); `def` if it does not exist.
    std::string Read(const std::vector<std::string>& path, const std::string& def) const;
    // Changes or creates the value. `quoted`: write it as a string (an existing key keeps its type).
    bool Put(const std::vector<std::string>& path, const std::string& value, bool quoted);
    std::string text;

private:
    bool Find(const std::vector<std::string>& path, size_t* vb, size_t* ve, size_t* obj_b, size_t* obj_e,
              size_t* last_end, size_t* level) const;
    bool bom_ = false, valid_ = false;
};

// Line-based INI (key=value in the [PLA] section).
class IniText {
public:
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    std::string Read(const std::string& key, const std::string& def) const;
    void Put(const std::string& key, const std::string& value);
    // Sets the value of every existing line with this key; adds nothing.
    void Update(const std::string& key, const std::string& value);
    std::string text;
};

enum class SettingFile { Nvnvk, RenderRes, Engine, Controls, Nnshim, PlaIni, Islands, Count };
enum class NewType { String, Bool, Number };

struct Setting {
    std::string id;
    SettingFile file = SettingFile::Nvnvk;
    std::vector<std::string> path;  // INI: path[0] = key, path[1..] = legacy names (read, kept in sync)
    std::string def;
    NewType type = NewType::String;
    bool live = false;      // applies immediately
    std::string value;      // current
    std::string original;   // at load time (to detect changes)
    bool exists = false;    // the file has the key
};

class Options {
public:
    // Locates the files (from the folders of nvnvk.dll and PLA.exe) and reads every setting.
    void Load(const std::string& pla_ini_cfg);
    const std::string& Get(const std::string& id) const;
    float GetF(const std::string& id) const;
    bool GetB(const std::string& id) const;
    void Set(const std::string& id, const std::string& v);
    void Restore(const std::string& id);
    void Base(const std::string& id, const std::string& v);  // real value when opening (counts as unchanged)
    std::string Default(const std::string& id) const;
    bool Changed(const std::string& id) const;
    bool Available(SettingFile f) const { return sources_[(int)f].available; }
    // Live settings channel: "<mod>.<key path>" (e.g. "render_res.width", "nnshim.audio.volume") for settings that live
    // in another mod's mod.json; empty for nvnvk's own settings, PLA.ini and the controls file.
    std::string LiveKey(const std::string& id) const;
    void SetLive(const std::string& id, bool live);  // whether the setting applies without restarting the game
    bool RestartPending() const;  // there are saved or unsaved changes that only apply after a restart
    // Writes what changed to disk (leaves <file>.bak-menu before the first change). Returns the number of files written.
    int Save(bool write, std::string* summary);
    std::vector<std::pair<std::string, bool>> Mods() const;  // (name, enabled) in the mods folder
    std::wstring dir_nvnvk, dir_mods;
    std::string controls_path;
    std::string engine_mod = "engine_options";  // engine options mod folder (legacy name: motor_opciones)
    bool regen_keys = false;  // the controls file changed: the key icons are rebuilt at the next start (play script / launcher)

private:
    struct Source {
        std::wstring path;
        bool available = false, ini = false, dirty = false;
        JsonText json;
        IniText initxt;
        std::vector<std::wstring> mirrors;  // session templates (same key paths)
    };
    void Def(const std::string& id, SettingFile f, std::vector<std::string> path, const std::string& def, NewType t, bool live);
    Source sources_[(int)SettingFile::Count];
    std::map<std::string, Setting> a_;
    bool restart_saved_ = false;
};

// Live settings channel (menu -> other mods, docs/port/LIVE-SETTINGS.md). The menu publishes every value it changes
// for render_res, engine_options and the nnshim; the mods poll nvnvk_live_version() and read the new values with
// nvnvk_live_get(). LiveSaved() bumps the version again after the files were written (on menu close).
void LivePublish(const std::string& key, const std::string& value);
void LiveSaved();
// Graphics presets (FPS-1). PresetValues: the settings (menu ids) a preset sets. ApplyAutoPreset: at startup, with
// preset "auto" on a low-end GPU, applies Performance to nvnvk's own settings and publishes the other mods' ones on the
// live channel (the files are not rewritten). g_auto_perf: it was applied (the menu shows those values).
const std::vector<std::pair<std::string, std::string>>& PresetValues(const std::string& preset);
void ApplyAutoPreset(bool low_end);
extern bool g_auto_perf;

// Key names in the nnshim format (VkFromName) and their readable label.
std::string KeyName(int vk, bool extended);
std::string KeyLabel(const std::string& name);

}  // namespace nvnvk
