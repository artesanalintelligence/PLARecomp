// nnshim: keyboard and mouse UI ("ui" family; gen_mod_json.py --ui). See include/nnshim/ui.hpp.
//
// Settings (controls file, "ui" section, or "ui.*" in the mod.json "config"):
//   "button_prompts": "auto" | "keyboard" | "controller"   key prompts (auto = no controller connected)
//   "mouse_menus": true                                    mouse in menus (applied by hid_core.cpp)
//   "prompt_pack": "<path of key_prompts.bin>"             default: key_prompts.bin next to the controls
//                                                          file (or the legacy teclas.bin if missing)
//   "menu_rule": "menu" | "nofield"                        what counts as a menu (see below)
// In the mod.json:
//   "ui.dump_dir": "<folder>"    saves the BNTX with button icons that the game decompresses from the
//                                GFLXPACKs (gen_key_prompts.py --dump adds them to key_prompts.bin: in the
//                                RomFS they are Oodle-compressed). Existing files are not rewritten.
//   "ui.trace": true             logs every menu / field mode change
//   "ui.menu_off_frames": 2      consecutive frames without a menu signal to return to field mode
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "abi_common.hpp"
#include "nnshim/fs.hpp"
#include "nnshim/hid.hpp"
#include "nnshim/ui.hpp"

using namespace nnshim;

namespace {

namespace stdfs = std::filesystem;

ui::Pack g_pack;
std::atomic<bool> g_pack_ok{false};
ui::PromptMode g_mode = ui::PromptMode::Auto;
std::string g_dump_dir;
bool g_trace = false;
// Menu state from game hooks (measured with probes, see the keyboard UI doc, section 3):
//   main+0xc392d4  once per frame only while a pause menu is open (bag, map, settings...);
//   main+0x2505ff8 once per frame only while the player is in control (on foot, riding, flying, climbing);
//                  stops in menus, dialogs, battles and cutscenes;
//   main+0x1363b58 once per frame, always.
// "menu_rule": "menu" = only the first one; "nofield" (default) = also when the player has not been in
// control for at least 3 frames and field_gap_ms (dialogs: click = A; battles: wheel = pick a move).
// The state is decided in FrameTick, once per frame, and does not expire by time (a hitch won't flip it).
std::string g_menu_rule = "nofield";
int g_field_gap_ms = 250, g_menu_off_frames = 2;
std::atomic<int64_t> g_last_field_ms{0};
std::atomic<int> g_frames_since_field{0};
std::atomic<int> g_frames_since_menu_tick{1000};
int g_frames_not_menu = 0;   // FrameTick only (one thread: the game's main thread)
uint64_t g_main = 0;

std::mutex g_mu;   // guards the following
std::map<std::string, std::shared_ptr<const std::vector<ui::FilePatch>>> g_arc_cache;
std::set<std::string> g_dumped;

bool KeyboardNow() { return ui::KeyboardPrompts(g_mode, hid::PadConnected()); }

// Live swap (only with button_prompts = "auto"): prompts already loaded and a thread that rewrites them
// in game memory when a controller is connected or disconnected (see ui::LiveSet).
std::mutex g_live_mu;
ui::LiveSet g_live;
std::atomic<bool> g_live_stop{false};
ExitSafeThread g_live_thread;

// Off by default ("ui.live_swap"): with nvnvk it is not visible (the GPU uses the texture pool copy in
// VRAM, which is never re-read; see the keyboard UI doc, section 6).
bool g_live_swap = false;
bool LiveEnabled() { return g_live_swap && g_mode == ui::PromptMode::Auto && PLA_API_HAS(g_api, host_pointer); }

void LiveAdd(uint64_t va, const uint8_t* orig, const uint8_t* kb, size_t size) {
    std::lock_guard lk(g_live_mu);
    g_live.Add(va, orig, kb, size);
}

void LiveThread() {
    bool kb = KeyboardNow();
    while (!g_live_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const bool now = KeyboardNow();
        if (now == kb) continue;
        kb = now;
        int n;
        size_t live;
        {
            std::lock_guard lk(g_live_mu);
            n = g_live.Swap(now, [](uint64_t va, size_t sz) { return static_cast<uint8_t*>(g_api->host_pointer(va, sz)); });
            live = g_live.Size();
        }
        CounterAdd("ui.live.swaps", static_cast<uint64_t>(n));
        Log(PLA_LOG_INFO, "nnshim: ui: prompts -> %s (%d loaded textures rewritten, %zu watched)",
            now ? "keyboard" : "controller", n, live);
    }
}

bool ReadHostFile(const std::string& u8path, std::vector<uint8_t>& out) {
    out.clear();
    std::error_code ec;
    const stdfs::path p = stdfs::u8path(u8path);
    const auto size = stdfs::file_size(p, ec);
    if (ec || size == 0 || size > (256ull << 20)) return false;
#ifdef _WIN32
    std::FILE* f = _wfopen(p.c_str(), L"rb");
#else
    std::FILE* f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return false;
    out.resize(static_cast<size_t>(size));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool EndsWithArc(const std::string& s) {
    if (s.size() < 4) return false;
    const char* e = s.c_str() + s.size() - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'a' && (e[2] | 0x20) == 'r' && (e[3] | 0x20) == 'c';
}

bool InterestingName(const std::string& n) {
    return n.find("button") != std::string::npos || n.find("btn") != std::string::npos ||
           n.find("key") != std::string::npos || n.find("stick") != std::string::npos;
}

// Saves the BNTX if it has button icons we don't know yet (once per first name).
void MaybeDump(const uint8_t* buf, size_t len) {
    if (g_dump_dir.empty()) return;
    std::vector<ui::BntxTex> texs;
    if (!ui::ParseBntx(buf, len, texs) || texs.empty()) return;
    bool interesting = false;
    for (const auto& t : texs) interesting = interesting || InterestingName(t.name);
    if (!interesting) return;
    const std::string key = texs.front().name + "_" + std::to_string(len);
    {
        std::lock_guard lk(g_mu);
        if (!g_dumped.insert(key).second) return;
    }
    std::string file;
    for (char c : key) file += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    std::error_code ec;
    stdfs::create_directories(stdfs::u8path(g_dump_dir), ec);
    const stdfs::path out = stdfs::u8path(g_dump_dir) / (file + ".bntx");
    if (stdfs::exists(out, ec) && stdfs::file_size(out, ec) == len) return;   // already dumped in another session
#ifdef _WIN32
    std::FILE* f = _wfopen(out.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(out.c_str(), "wb");
#endif
    if (!f) return;
    std::fwrite(buf, 1, len, f);
    std::fclose(f);
    std::string names;
    for (const auto& t : texs)
        if (InterestingName(t.name)) names += " " + t.name;
    Log(PLA_LOG_INFO, "nnshim: ui: dumped %s (%zu bytes):%s", out.u8string().c_str(), len, names.c_str());
}

void InitUi(const cfg::Value& c) {
    g_main = g_api->module_base("main");
    std::string mode = c.GetString("ui.button_prompts", "auto");
    std::string pack = c.GetString("ui.prompt_pack", "");
    g_dump_dir = c.GetString("ui.dump_dir", "");
    g_trace = c.GetBool("ui.trace", false);
    g_live_swap = c.GetBool("ui.live_swap", false);
    g_menu_off_frames = static_cast<int>(c.GetInt("ui.menu_off_frames", g_menu_off_frames));
    g_menu_rule = c.GetString("ui.menu_rule", g_menu_rule);
    g_field_gap_ms = static_cast<int>(c.GetInt("ui.field_gap_ms", g_field_gap_ms));
    cfg::Value v;
    std::string controls;
    if (LoadControlsFile(c, v, controls)) {
        mode = v.GetString("ui.button_prompts", mode);
        pack = v.GetString("ui.prompt_pack", pack);
        g_menu_rule = v.GetString("ui.menu_rule", g_menu_rule);
    }
    if (pack.empty() && !controls.empty())   // default: next to the controls file
        pack = (stdfs::u8path(controls).parent_path() / "key_prompts.bin").u8string();
    if (!pack.empty()) {
        // A missing key_prompts.bin falls back to the legacy name teclas.bin in the same folder.
        const stdfs::path p = stdfs::u8path(pack);
        std::error_code ec;
        if (p.filename() == "key_prompts.bin" && !stdfs::exists(p, ec) && stdfs::exists(p.parent_path() / "teclas.bin", ec))
            pack = (p.parent_path() / "teclas.bin").u8string();
    }
    g_mode = ui::PromptModeFromString(mode);
    Log(PLA_LOG_INFO, "nnshim: ui: menus by %s", g_menu_rule.c_str());
    std::vector<uint8_t> data;
    std::string err;
    if (!pack.empty() && ReadHostFile(pack, data) && g_pack.Load(data, &err)) {
        g_pack_ok = true;
        Log(PLA_LOG_INFO, "nnshim: ui: prompts=%s pack %s (%zu textures)", mode.c_str(), pack.c_str(), g_pack.Size());
        if (LiveEnabled()) {
            g_live_stop = false;
            g_live_thread = std::thread(LiveThread);
        }
    } else {
        Log(PLA_LOG_INFO, "nnshim: ui: prompts=%s without key-prompt pack (%s%s%s)", mode.c_str(),
            pack.empty() ? "no path" : pack.c_str(), err.empty() ? "" : ": ", err.c_str());
    }
}

void ShutdownUi() {
    g_live_stop = true;
    if (g_live_thread.joinable()) g_live_thread.join();
    Log(PLA_LOG_INFO,
        "nnshim: ui: arcs patched=%llu bytes=%llu gfpak bntx=%llu sarc=%llu (bntx %llu) textures=%llu menu=%llu clicks=%llu "
        "drag=%llu",
        static_cast<unsigned long long>(CounterGet("ui.arc.reads_patched")),
        static_cast<unsigned long long>(CounterGet("ui.arc.bytes")),
        static_cast<unsigned long long>(CounterGet("ui.gfpak.bntx")),
        static_cast<unsigned long long>(CounterGet("ui.gfpak.sarc")),
        static_cast<unsigned long long>(CounterGet("ui.gfpak.sarc_bntx")),
        static_cast<unsigned long long>(CounterGet("ui.gfpak.textures")),
        static_cast<unsigned long long>(CounterGet("ui.menu.ticks") + CounterGet("ui.menu.nofield")),
        static_cast<unsigned long long>(hid::GetInfo().menu_clicks),
        static_cast<unsigned long long>(hid::GetInfo().menu_drag_samples));
    std::lock_guard lk(g_mu);
    g_arc_cache.clear();
}

ModuleHooks g_ui_module{"ui", &InitUi, &ShutdownUi};

} // namespace

void nnshim::UiOnRomRead(uint64_t handle, int64_t off, uint8_t* dst, uint64_t n, uint64_t guest_va) {
    if (!g_pack_ok.load(std::memory_order_relaxed) || off < 0) return;
    const bool kb = KeyboardNow(), live = LiveEnabled() && guest_va;
    if (!kb && !live) return;
    const std::string path = fs::RomFileId(handle);
    if (!EndsWithArc(path)) return;
    std::shared_ptr<const std::vector<ui::FilePatch>> patches;
    {
        std::lock_guard lk(g_mu);
        auto it = g_arc_cache.find(path);
        if (it != g_arc_cache.end()) patches = it->second;
    }
    if (!patches) {
        std::vector<uint8_t> file;
        auto list = std::make_shared<std::vector<ui::FilePatch>>();
        if (fs::ReadRomFileById(path, file, 256ull << 20)) *list = ui::SarcPatches(g_pack, file);
        patches = list;
        std::lock_guard lk(g_mu);
        g_arc_cache.emplace(path, patches);
        if (!list->empty()) Log(PLA_LOG_INFO, "nnshim: ui: %s: %zu prompts", path.c_str(), list->size());
    }
    if (patches->empty()) return;
    if (live)   // whole prompts inside this read: remember where they land in the guest
        for (const ui::FilePatch& p : *patches)
            if (p.file_off >= static_cast<uint64_t>(off) && p.file_off + p.size <= static_cast<uint64_t>(off) + n) {
                const uint64_t rel = p.file_off - static_cast<uint64_t>(off);
                LiveAdd(guest_va + rel, dst + rel, p.src, static_cast<size_t>(p.size));
            }
    if (!kb) return;
    const uint64_t done = ui::ApplyPatches(*patches, static_cast<uint64_t>(off), dst, n);
    if (done) {
        CounterAdd("ui.arc.reads_patched");
        CounterAdd("ui.arc.bytes", done);
    }
}

// ---- GFPack::LoadFile (main+0x2975d5c): bool (GFPack* pack, int index, void* dst) ------------------
// pack->[0] = GFLXPACK header in memory; header+0x18 = file table (0x18-byte entries:
// u16 level, u16 compression, u32 decompressed size, u32 compressed size, u32 padding, u64 data).
NNSHIM_ADAPTER(Ui, nnshim_ui_GfpakLoad) {
    PlaGuest g(ctx);
    uint64_t* cd = g.call_data();
    cd[0] = g.x(0);
    cd[1] = g.x(1) & 0xFFFFFFFFu;
    cd[2] = g.x(2);
    return PLA_CONTINUE;
}

NNSHIM_EXPORT void nnshim_ui_GfpakLoad_post(pla_ctx* ctx) {
    if (FamilyMode(Family::Ui) != Mode::Native) return;
    PlaGuest g(ctx);
    if (!(g.x(0) & 1)) return;
    const bool patch = g_pack_ok.load(std::memory_order_relaxed) && KeyboardNow();
    const bool live = g_pack_ok.load(std::memory_order_relaxed) && LiveEnabled();
    if (!patch && !live && g_dump_dir.empty()) return;
    const uint64_t* cd = g.call_data();
    uint64_t hdr = 0, table = 0;
    uint32_t size = 0;
    const int32_t idx = static_cast<int32_t>(static_cast<uint32_t>(cd[1]));
    if (!cd[0] || !cd[2] || idx < 0 || !g.load(cd[0], hdr) || !hdr || !g.load(hdr + 0x18, table) || !table ||
        !g.load(table + 0x18ull * static_cast<uint64_t>(idx) + 4, size) || size < 0x40)
        return;
    uint8_t* p = PLA_API_HAS(g_api, host_pointer) ? static_cast<uint8_t*>(g_api->host_pointer(cd[2], size)) : nullptr;
    std::vector<uint8_t> copy;
    if (!p) {
        char magic[4] = {};
        if (g.read(cd[2], magic, 4) != 4 || (std::memcmp(magic, "BNTX", 4) != 0 && std::memcmp(magic, "SARC", 4) != 0))
            return;
        copy.resize(size);
        if (g.read(cd[2], copy.data(), size) != size) return;
        p = copy.data();
    }
    int n = 0;
    // A BNTX at p[base, base + len): register it (live swap) and replace the icons found in the pack.
    auto handle = [&](uint64_t base, size_t len) {
        MaybeDump(p + base, len);
        if (!patch && !(live && copy.empty())) return;
        for (const ui::TexHit& h : ui::MatchBntx(g_pack, p + base, len)) {
            uint8_t* t = p + base + h.data_off;
            if (live && copy.empty()) LiveAdd(cd[2] + base + h.data_off, t, h.entry->data.data(), h.entry->data.size());
            if (patch) {
                std::memcpy(t, h.entry->data.data(), h.entry->data.size());
                ++n;
            }
        }
    };
    if (std::memcmp(p, "BNTX", 4) == 0) {
        CounterAdd("ui.gfpak.bntx");
        handle(0, size);
    } else if (std::memcmp(p, "SARC", 4) == 0) {
        // Packed layouts (SARC with timg/*.bntx inside).
        CounterAdd("ui.gfpak.sarc");
        for (const auto& [a, z] : ui::SarcBntxRanges(p, size)) {
            CounterAdd("ui.gfpak.sarc_bntx");
            handle(a, static_cast<size_t>(z - a));
        }
    }
    if (n) {
        CounterAdd("ui.gfpak.textures", static_cast<uint64_t>(n));
        if (!copy.empty()) g.write(cd[2], copy.data(), size);
    }
}

// ---- probe with a time series (diagnostics) ---------------------------------------------------------
// Generic "pre": counts calls per function and every second logs the counts of the last second
// ("ui: probes t=<s>: main+0x...=n ..."). Used to find functions that only run in the field or only
// in menus (gen_mod_json.py --probe-file with "pre": "nnshim_ui_Probe").
namespace {
std::mutex g_probe_mu;
std::map<uint64_t, uint32_t> g_probe_counts;
int64_t g_probe_window = 0, g_probe_t0 = 0;
} // namespace

NNSHIM_EXPORT pla_action nnshim_ui_Probe(pla_ctx* ctx) {
    const uint64_t pc = g_api->get_pc(ctx);
    const int64_t now = hid::NowMs();
    std::string line;
    {
        std::lock_guard lk(g_probe_mu);
        g_probe_counts[pc]++;
        if (!g_probe_t0) g_probe_t0 = g_probe_window = now;
        if (now - g_probe_window >= 1000) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "t=%lld:", static_cast<long long>((now - g_probe_t0) / 1000));
            line = buf;
            for (const auto& [a, n] : g_probe_counts) {
                std::snprintf(buf, sizeof buf, " %llx=%u", static_cast<unsigned long long>(a - g_main), n);
                line += buf;
            }
            g_probe_counts.clear();
            g_probe_window = now;
        }
    }
    if (!line.empty()) Log(PLA_LOG_INFO, "nnshim: ui: probes %s menu=%d", line.c_str(), hid::MenuActive() ? 1 : 0);
    return PLA_CONTINUE;
}

// ---- menu state from game hooks ----------------------------------------------------------------------
namespace nnshim {
void PointerOnFrame();   // abi_pointer.cpp (mouse pointer in the game UI)
}
namespace {
bool g_menu_trace_state = false;

void TraceMenu() {
    if (!g_trace) return;
    const bool m = hid::MenuActive();
    if (m != g_menu_trace_state) {
        g_menu_trace_state = m;
        Log(PLA_LOG_INFO, "nnshim: ui: menu %s", m ? "ON" : "OFF");
    }
}
} // namespace

// main+0xc392d4: runs once per frame while a pause menu is open.
NNSHIM_ADAPTER(Ui, nnshim_ui_MenuTick) {
    g_frames_since_menu_tick.store(0, std::memory_order_relaxed);
    CounterAdd("ui.menu.ticks");
    return PLA_CONTINUE;
}

// main+0x2505ff8: runs once per frame while the player is in control.
NNSHIM_ADAPTER(Ui, nnshim_ui_FieldTick) {
    // "Loading finished" mark to measure load times (trace and log, only once).
    static std::atomic<bool> first{true};
    if (first.load(std::memory_order_relaxed) && first.exchange(false)) {
        Log(PLA_LOG_INFO, "nnshim: ui: first frame with the player in control (uptime %llu ms)",
            static_cast<unsigned long long>(UptimeMs()));
        Trace("first frame with the player in control");
    }
    g_last_field_ms.store(hid::NowMs(), std::memory_order_relaxed);
    g_frames_since_field.store(0, std::memory_order_relaxed);
    return PLA_CONTINUE;
}

// main+0x1363b58: once per frame, always: decides this frame's menu state.
//   menu = MenuTick in the last 2 frames, or (menu_rule = "nofield") the player has been out of control
//          (no FieldTick) for >= 3 frames and field_gap_ms.
// Returning to field mode takes menu_off_frames consecutive frames without a menu signal.
NNSHIM_ADAPTER(Ui, nnshim_ui_FrameTick) {
    const int menu_frames = g_frames_since_menu_tick.fetch_add(1, std::memory_order_relaxed);
    const int field_frames = g_frames_since_field.fetch_add(1, std::memory_order_relaxed) + 1;
    if (menu_frames > 1000000) g_frames_since_menu_tick.store(1000, std::memory_order_relaxed);
    if (field_frames > 1000000) g_frames_since_field.store(1000, std::memory_order_relaxed);
    bool menu = menu_frames <= 2;
    if (!menu && g_menu_rule == "nofield") {
        const int64_t now = hid::NowMs(), field = g_last_field_ms.load(std::memory_order_relaxed);
        menu = field_frames >= 3 && (!field || now - field > g_field_gap_ms);
        if (menu) CounterAdd("ui.menu.nofield");
    }
    if (menu) g_frames_not_menu = 0;
    else ++g_frames_not_menu;
    if (menu && !hid::MenuActive()) hid::SetMenuState(true);
    else if (!menu && hid::MenuActive() && g_frames_not_menu >= g_menu_off_frames) hid::SetMenuState(false);
    TraceMenu();
    PointerOnFrame();
    return PLA_CONTINUE;
}
