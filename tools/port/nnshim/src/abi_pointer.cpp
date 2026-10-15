// nnshim: mouse pointer inside the game's UI (family "ui"; docs/port/POINTER.md).
//
// Hook: nn::ui2d::Layout::Draw (main+0x1353e0, vf12 of nn::ui2d::Layout), "pre". At that point the
// global matrices of the panes are up to date for the frame: the pane tree is walked and the visible
// panes are recorded with their on-screen rectangle.
//
// Diagnostics (mod.json config):
//   "ui.pointer_dump_dir": "<dir>"          pane tree dumps and the animation log
//   "ui.pointer_dump_secs": "75,80.5,..."   seconds since the shim started at which a whole frame is
//                                           dumped (every ui2d layer drawn)
//   "ui.pointer_scan": "L_btn_main_05,..."  in each dump, search guest memory for pointers to the panes
//                                           with these names (to find the game objects that own them)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "abi_common.hpp"
#include "nnshim/hid.hpp"
#include "nnshim/pointer.hpp"

using namespace nnshim;

namespace {

namespace stdfs = std::filesystem;

uint64_t g_main = 0;

// ---- guest memory (fastmem arena: direct reads) ---------------------------------------------------------
template <class T> bool Rd(uint64_t va, T& out) {
    if (!va) return false;
    const void* p = g_api->host_pointer(va, sizeof(T));
    if (!p) return false;
    std::memcpy(&out, p, sizeof(T));
    return true;
}
bool RdBytes(uint64_t va, void* dst, size_t n) {
    if (!va) return false;
    const void* p = g_api->host_pointer(va, n);
    if (!p) return false;
    std::memcpy(dst, p, n);
    return true;
}

std::string Hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
    return b;
}

// ---- diagnostic dumps -------------------------------------------------------------------------------------
std::string g_dump_dir;
std::vector<double> g_dump_secs;   // pending, sorted
std::vector<std::string> g_scan_names;
std::mutex g_dump_mu;
bool g_dump_active = false;
int g_dump_frames = 0;
std::string g_dump_text;
std::set<uint64_t> g_dump_layouts, g_dump_infos;
std::vector<std::pair<std::string, uint64_t>> g_scan_hits;   // (name, pane) found in this dump
std::string g_dump_name;
int g_dump_count = 0;

// Pane type from its vtable (vf0 of each nn::ui2d class, main_functions_named.tsv).
const char* PaneType(uint64_t vt) {
    uint64_t f0 = 0;
    if (!Rd(vt, f0) || !g_main) return "?";
    switch (f0 - g_main) {
    case 0x1496ac: return "pic";
    case 0x14d274: return "txt";
    case 0x1530e8: return "wnd";
    case 0x148d14: return "prt";
    case 0x13254c: return "bnd";
    default: return "pan";
    }
}

void DumpPane(std::string& out, uint64_t pane, int depth, int budget[1]) {
    if (--budget[0] < 0 || depth > 40) return;
    uint64_t vt = 0;
    float sz[2] = {}, tr[3] = {}, sc[2] = {};
    uint8_t fl[4] = {};
    ptr::Mat34 m{};
    char name[25] = {};
    if (!Rd(pane, vt) || !RdBytes(pane + 0x50, sz, 8) || !RdBytes(pane + 0x58, fl, 4) || !RdBytes(pane + 0x70, &m, sizeof m) ||
        !RdBytes(pane + 0xb0, name, 24) || !RdBytes(pane + 0x30, tr, 12) || !RdBytes(pane + 0x48, sc, 8))
        return;
    const char* type = PaneType(vt);
    const ptr::Rect r = ptr::PaneGlobalRect(m, sz[0], sz[1], fl[3]);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%*s%s %-24s fl=%02x a=%u ga=%u bp=%02x sz=%.0fx%.0f t=%.1f,%.1f s=%.2f,%.2f g=[%.0f,%.0f..%.0f,%.0f] @%llx",
                  depth * 2, "", type, name, fl[0], fl[1], fl[2], fl[3], sz[0], sz[1], tr[0], tr[1], sc[0], sc[1], r.x0, r.y0,
                  r.x1, r.y1, static_cast<unsigned long long>(pane));
    out += buf;
    if (!std::strcmp(type, "txt")) {
        uint64_t tp = 0;
        if (Rd(pane + 0xd8, tp) && tp) {
            char16_t s[48] = {};
            if (RdBytes(tp, s, sizeof s - 2)) {
                std::u16string u;
                for (char16_t c : s) {
                    if (!c) break;
                    u.push_back(c == u'\n' ? u'|' : c);
                }
                out += " \"" + Utf16ToUtf8(u) + "\"";
            }
        }
    }
    out += "\n";
    for (const std::string& n : g_scan_names)
        if (n == name && (fl[0] & 0x81) == 0x81) g_scan_hits.emplace_back(n, pane);
    uint64_t head = pane + 0x20, node = 0;
    if (!Rd(pane + 0x28, node)) return;
    for (int i = 0; node && node != head && i < 512; ++i) {
        DumpPane(out, node - 8, depth + 1, budget);
        if (!Rd(node + 8, node)) break;
    }
}

void DumpLayout(uint64_t layout, uint64_t draw_info) {
    uint64_t root = 0;
    if (!Rd(layout + 0x18, root) || !root) return;
    std::string s = "== layout " + Hex(layout) + " root " + Hex(root) + " drawinfo " + Hex(draw_info) + "\n";
    char buf[64];
    float di[0x1a0 / 4] = {};
    if (g_dump_infos.insert(draw_info).second && g_dump_infos.size() <= 2 && RdBytes(draw_info, di, sizeof di)) {
        s += "   drawinfo:";
        for (int i = 0; i < 0x1a0 / 4; ++i) {
            std::snprintf(buf, sizeof buf, "%s%03x=%g", (i % 8) ? " " : "\n    ", i * 4, di[i]);
            s += buf;
        }
        s += "\n";
    }
    int budget[1] = {4000};
    DumpPane(s, root, 0, budget);
    g_dump_text += s;
}

// Search guest memory near `pane` (+-1 GiB, in 1 MiB chunks) for 8-byte values equal to it.
std::string ScanReferrers(const std::string& name, uint64_t pane) {
    std::string out = "== scan " + name + " @" + Hex(pane) + ":";
    const uint64_t chunk = 1ull << 20;
    const uint64_t lo = (pane > (1ull << 30) ? pane - (1ull << 30) : 0) & ~(chunk - 1), hi = pane + (1ull << 30);
    int hits = 0;
    for (uint64_t a = lo; a < hi && hits < 64; a += chunk) {
        const uint64_t* p = static_cast<const uint64_t*>(g_api->host_pointer(a, chunk));
        if (!p) continue;
        for (size_t i = 0; i < chunk / 8 && hits < 64; ++i)
            if (p[i] == pane) {
                const uint64_t at = a + i * 8;
                out += "\n   ref " + Hex(at) + ":";
                uint64_t q[16] = {};
                if (RdBytes(at - 0x40, q, sizeof q))
                    for (uint64_t v : q) out += " " + Hex(v);
                ++hits;
            }
    }
    return out + "\n";
}

std::vector<double> ParseSecs(const std::string& s) {
    std::vector<double> v;
    const char* p = s.c_str();
    while (*p) {
        char* e = nullptr;
        const double d = std::strtod(p, &e);
        if (e == p) {
            ++p;
            continue;
        }
        v.push_back(d);
        p = e;
    }
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::string> SplitNames(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            if (!cur.empty()) v.push_back(cur);
            cur.clear();
        } else if (s[i] != ' ') cur.push_back(s[i]);
    }
    return v;
}

// ---- animation log (LayoutAnimator::play) ------------------------------------------------------------------
std::mutex g_anim_mu;
std::FILE* g_anim_log = nullptr;
int g_anim_lines = 0;

void InitPointerRuntime(const cfg::Value& c);   // pointer runtime (below)

void InitPointer(const cfg::Value& c) {
    g_main = g_api->module_base("main");
    g_dump_dir = c.GetString("ui.pointer_dump_dir", "");
    g_dump_secs = ParseSecs(c.GetString("ui.pointer_dump_secs", ""));
    g_scan_names = SplitNames(c.GetString("ui.pointer_scan", ""));
    InitPointerRuntime(c);
    if (!g_dump_dir.empty()) {
        Log(PLA_LOG_INFO, "nnshim: pointer: pane dumps in %s (%zu)", g_dump_dir.c_str(), g_dump_secs.size());
        std::error_code ec;
        stdfs::create_directories(stdfs::u8path(g_dump_dir), ec);
        g_anim_log = _wfopen((stdfs::u8path(g_dump_dir) / "anim-log.txt").c_str(), L"wb");
    }
}

void ShutdownPointer() {
    std::lock_guard lk(g_anim_mu);
    if (g_anim_log) std::fclose(g_anim_log);
    g_anim_log = nullptr;
}

ModuleHooks g_ptr_module{"pointer", &InitPointer, &ShutdownPointer};

} // namespace

namespace {
void BuildSnapshot();   // pointer runtime (below)

// Once per frame: opens and closes the dump windows.
void DumpOnFrame() {
    if (g_dump_dir.empty()) return;
    std::string text, name;
    {
        std::lock_guard lk(g_dump_mu);
        if (g_dump_active) {
            if (++g_dump_frames < 3) return;
            g_dump_active = false;
            for (const auto& [n, p] : g_scan_hits) g_dump_text += ScanReferrers(n, p);
            g_scan_hits.clear();
            text.swap(g_dump_text);
            name = g_dump_name;
            g_dump_layouts.clear();
            g_dump_infos.clear();
        } else if (!g_dump_secs.empty() && UptimeMs() >= g_dump_secs.front() * 1000.0) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "panes-%02d-%06.1f.txt", g_dump_count++, g_dump_secs.front());
            g_dump_name = buf;
            g_dump_secs.erase(g_dump_secs.begin());
            g_dump_active = true;
            g_dump_frames = 0;
            g_dump_text.clear();
            g_dump_layouts.clear();
            return;
        } else return;
    }
    std::error_code ec;
    stdfs::create_directories(stdfs::u8path(g_dump_dir), ec);
    const stdfs::path out = stdfs::u8path(g_dump_dir) / name;
    if (std::FILE* f = _wfopen(out.c_str(), L"wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    Log(PLA_LOG_INFO, "nnshim: pointer: dumped %s (%zu bytes)", name.c_str(), text.size());
}
} // namespace

namespace nnshim {
// Once per frame (abi_ui.cpp, FrameTick, game main thread).
void PointerOnFrame() {
    DumpOnFrame();
    BuildSnapshot();
}
} // namespace nnshim

// ---- pointer runtime ---------------------------------------------------------------------------------------
// Game side: every frame, the visible panes (Layout::Draw, render thread) and the live UI-kit widgets
// (button update, main thread) are combined into a ptr::Snapshot of selectable items (FrameTick). Host side:
// the hid polling thread asks a ptr::Navigator which d-pad / A presses bring the highlight to the item
// under the pointer (HostPoll).
namespace {

// Settings ("ui.*" in the mod config or in controls.json).
bool g_ptr_enabled = true;
bool g_ptr_trace = false;
ptr::NavConfig g_nav_cfg;

// Button widget of the game's UI kit (vtable main+0x416d690 and subclasses; see POINTER.md section 2):
//   +0x08 owner (widget manager), +0x68 HashedName button id ("button_icon_main_05"),
//   +0xa0 u8 selected, +0xa2 u8 enabled, +0xa3 u8 pane visible, +0xa8 HashedName pane ("pane_<parts path>"),
//   +0xe8 LayoutAnimator*, +0x1f8 HashedName "select" animation, +0x218 "unselect".
// Its update (vf16, main+0x2708dd8) runs once per frame for every live button: that is the registry.
constexpr uint64_t kBtnSelected = 0xa0, kBtnVisible = 0xa3, kBtnPane = 0xa8, kBtnOwner = 0x08;

struct PaneRec {
    ptr::Rect r;           // normalized screen rect, clipped by scissor panes
    uint64_t pane = 0;
    uint64_t prefix = 0;   // hash of the key without the trailing number (identifies a list)
    int rule = -1;         // index in g_rules
    int layer = 0;         // draw order of its layout in the frame
    std::string key;       // "pane_<path>" (only with ui.pointer_trace)
};

// Navigation rules by pane key prefix ("ui.pointer_rules": "prefix:up,down,left,right[:noaccept];...").
// Default: the header tabs of the pause menu (inventory, party, ...) switch with ZL / ZR and a click only
// selects them.
struct NavRule {
    std::string prefix;
    uint64_t up = 0, down = 0, left = 0, right = 0;
    bool no_accept = false;
    uint64_t click = 0;   // "click=L": button of a click on it
};
std::vector<NavRule> g_rules;

std::vector<NavRule> ParseRules(const std::string& text) {
    std::vector<NavRule> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos || c1 == 0) continue;
        NavRule r;
        r.prefix = item.substr(0, c1);
        std::string rest = item.substr(c1 + 1), flags;
        if (const size_t c2 = rest.find(':'); c2 != std::string::npos) {
            flags = rest.substr(c2 + 1);
            rest = rest.substr(0, c2);
        }
        uint64_t* dst[4] = {&r.up, &r.down, &r.left, &r.right};
        size_t a = 0;
        for (int i = 0; i < 4; ++i) {
            size_t b = rest.find(',', a);
            if (b == std::string::npos) b = rest.size();
            *dst[i] = hid::ButtonsFromList(rest.substr(a, b - a));
            a = b + 1;
            if (a > rest.size()) break;
        }
        r.no_accept = flags.find("noaccept") != std::string::npos;
        if (const size_t k = flags.find("click="); k != std::string::npos) {
            const size_t e = flags.find_first_of(":, ", k);
            r.click = hid::ButtonsFromList(flags.substr(k + 6, e == std::string::npos ? std::string::npos : e - k - 6));
        }
        out.push_back(r);
    }
    return out;
}

// Context menu of the UI kit (bin/appli/uikit/context): rows T_text_NN under N_textParent; the cursor
// subtree is re-parented under the slot N_cursor_NN (under N_pos_00) of the highlighted row.
struct CtxMenu {
    uint64_t root = 0;
    int layer = 0;
    ptr::Rect window;
    bool has_window = false;
    std::map<int, ptr::Rect> rows;
    std::map<int, bool> slots;   // index -> holds the cursor
};

std::mutex g_rt_mu;   // protects the frame buffers below
// A draw pass = the layouts drawn in one frame (it ends when a layout is drawn again). The snapshot uses the last
// complete pass, so the layer (draw order) of every layout is consistent.
struct Pass {
    std::unordered_map<uint64_t, PaneRec> panes;   // by "pane_<path>" hash and by pane address
    std::vector<CtxMenu> ctx;
    ptr::Rect map;   // stick cursor (map)
    bool has_map = false;
};
Pass g_pass_cur, g_pass_done;
std::unordered_set<uint64_t> g_pass_layouts;
int g_pass_layer = 0;
std::unordered_set<uint64_t> g_widgets_cur, g_widgets_prev;
std::atomic<bool> g_rt_active{false};   // menu mode (decided by FrameTick): collect only then

std::mutex g_snap_mu;
ptr::Snapshot g_snap;
uint64_t g_seq = 0;

int TrailingNumber(const char* n, size_t len, size_t& stem) {
    size_t i = len;
    while (i > 0 && n[i - 1] >= '0' && n[i - 1] <= '9') --i;
    stem = i;
    if (i == len) return -1;
    return std::atoi(n + i);
}

bool StartsWith(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }

struct WalkCtx {
    ptr::Proj proj;
    std::unordered_map<uint64_t, PaneRec>* out = nullptr;
    CtxMenu ctx;
    int budget = 6000;
    // Map cursor: parts L_cursor_00 under N_cursor_00 with the crosshair P_cursor_cross_00 inside.
    bool map_cross = false, map_cursor = false;
    ptr::Rect map_rect;
    int layer = 0;
};

void Walk(WalkCtx& w, uint64_t pane, const std::string& path, const ptr::Rect& clip, const char* parent, int depth) {
    if (--w.budget < 0 || depth > 40) return;
    uint8_t fl[4] = {};
    if (!RdBytes(pane + 0x58, fl, 4) || (fl[0] & 0x81) != 0x81) return;   // not drawn this frame
    uint64_t vt = 0;
    float sz[2] = {};
    ptr::Mat34 m{};
    char name[25] = {};
    if (!Rd(pane, vt) || !RdBytes(pane + 0x50, sz, 8) || !RdBytes(pane + 0x70, &m, sizeof m) || !RdBytes(pane + 0xb0, name, 24))
        return;
    const size_t nlen = std::strlen(name);
    const bool parts = !std::strcmp(PaneType(vt), "prt");
    const ptr::Rect r = ptr::GlobalToScreen(ptr::PaneGlobalRect(m, sz[0], sz[1], fl[3]), w.proj);
    const ptr::Rect rc = ptr::Intersect(r, clip);
    std::string key = "pane_" + path;
    key.append(name, nlen);
    size_t stem = 0;
    const int num = TrailingNumber(name, nlen, stem);
    PaneRec rec{rc, pane, ptr::Fnv1a64(key.data(), key.size() - (nlen - stem))};
    rec.layer = w.layer;
    for (size_t i = 0; i < g_rules.size(); ++i)
        if (key.compare(0, g_rules[i].prefix.size(), g_rules[i].prefix) == 0) {
            rec.rule = static_cast<int>(i);
            break;
        }
    if (g_ptr_trace) rec.key = key;
    (*w.out)[pane] = rec;
    (*w.out)[ptr::Fnv1a64(key.data(), key.size())] = std::move(rec);
    // Context menu pieces.
    if (num >= 0 && StartsWith(name, "N_cursor_") && !std::strcmp(parent, "N_pos_00")) {
        uint64_t first = 0;
        Rd(pane + 0x28, first);
        w.ctx.slots[num] = first && first != pane + 0x20;
    } else if (num >= 0 && StartsWith(name, "T_text_") && !std::strcmp(parent, "N_textParent") && rc.W() > 0) {
        w.ctx.rows[num] = rc;
    } else if (!std::strcmp(name, "W_window_00")) {
        w.ctx.window = rc;
        w.ctx.has_window = true;
    } else if (!std::strcmp(name, "L_cursor_00") && !std::strcmp(parent, "N_cursor_00")) {
        w.map_cursor = true;
        w.map_rect = r;
    } else if (!std::strcmp(name, "P_cursor_cross_00")) {
        w.map_cross = true;
    }
    const ptr::Rect child_clip = StartsWith(name, "S_scissor") ? rc : clip;
    const std::string child_path = parts ? path + std::string(name, nlen) + "_" : path;
    const uint64_t head = pane + 0x20;
    uint64_t node = 0;
    if (!Rd(pane + 0x28, node)) return;
    for (int i = 0; node && node != head && i < 512; ++i) {
        Walk(w, node - 8, child_path, child_clip, name, depth + 1);
        if (!Rd(node + 8, node)) break;
    }
}

void CollectLayout(uint64_t layout, uint64_t draw_info) {
    uint64_t root = 0;
    if (!Rd(layout + 0x18, root) || !root) return;
    WalkCtx w;
    float pm[16] = {};
    if (RdBytes(draw_info + 0x10, pm, sizeof pm) && pm[0] != 0 && pm[5] != 0 && pm[15] != 0) {
        std::memcpy(w.proj.m, pm, sizeof pm);
        w.proj.valid = true;
    }
    std::unordered_map<uint64_t, PaneRec> panes;
    w.out = &panes;
    {
        std::lock_guard lk(g_rt_mu);
        if (!g_pass_layouts.insert(layout).second) {   // drawn again: the previous pass is complete
            g_pass_done = std::move(g_pass_cur);
            g_pass_cur = Pass{};
            g_pass_layouts.clear();
            g_pass_layouts.insert(layout);
            g_pass_layer = 0;
        }
        w.layer = g_pass_layer++;
    }
    Walk(w, root, "", ptr::Rect{-0.25f, -0.25f, 1.25f, 1.25f}, "", 0);
    std::lock_guard lk(g_rt_mu);
    for (auto& [k, v] : panes) g_pass_cur.panes[k] = std::move(v);
    if (!w.ctx.rows.empty() && !w.ctx.slots.empty()) {
        w.ctx.root = root;
        w.ctx.layer = w.layer;
        g_pass_cur.ctx.push_back(std::move(w.ctx));
    }
    if (w.map_cursor && w.map_cross) {
        g_pass_cur.map = w.map_rect;
        g_pass_cur.has_map = true;
    }
}

// The layout wrapper of the UI kit (owner of a widget's animations) keeps a table of its named panes: records of
// 0x30 bytes {HashedName name ("pane_<parts path>"), Pane*, wrapper*}. With it a widget's pane is found exactly
// (the same key can exist in several layouts, e.g. the header and the pocket arrows "L_cursor_l_00").
struct WrapperTable {
    uint64_t base = 0;
    std::unordered_map<uint64_t, uint32_t> index;   // name hash -> record index
};
std::unordered_map<uint64_t, WrapperTable> g_wrappers;   // game main thread only (BuildSnapshot)

bool IsPaneRecord(uint64_t rec, uint64_t wrapper) {
    uint64_t str = 0, owner = 0, len = 0;
    char head[5] = {};
    return Rd(rec + 0x28, owner) && owner == wrapper && Rd(rec + 8, str) && Rd(rec + 0x10, len) && len > 5 && len < 200 &&
           RdBytes(str, head, 5) && !std::memcmp(head, "pane_", 5);
}

const WrapperTable* FindWrapperTable(uint64_t wrapper) {
    auto it = g_wrappers.find(wrapper);
    if (it != g_wrappers.end()) return it->second.base ? &it->second : nullptr;
    WrapperTable t;
    uint64_t q[0x100] = {};
    if (RdBytes(wrapper, q, sizeof q))
        for (uint64_t v : q) {
            if (v < 0x1000000 || !IsPaneRecord(v, wrapper)) continue;
            // Walk back to the first record and forward to the last one.
            uint64_t b = v;
            for (int i = 0; i < 20000 && IsPaneRecord(b - 0x30, wrapper); ++i) b -= 0x30;
            t.base = b;
            for (uint32_t i = 0; i < 20000 && IsPaneRecord(b + 0x30ull * i, wrapper); ++i) {
                uint64_t h = 0;
                Rd(b + 0x30ull * i, h);
                t.index[h] = i;
            }
            break;
        }
    if (g_wrappers.size() > 4096) g_wrappers.clear();
    auto& slot = g_wrappers[wrapper];
    slot = std::move(t);
    if (g_ptr_trace && slot.base)
        Log(PLA_LOG_INFO, "nnshim: pointer: wrapper %llx: %zu named panes", static_cast<unsigned long long>(wrapper),
            slot.index.size());
    return slot.base ? &slot : nullptr;
}

// Wrapper of a widget: the object at +0xe8 (its layout handle), or one it points to, whose pane table holds the
// widget's pane name. Found once per handle and cached.
std::unordered_map<uint64_t, uint64_t> g_handle_wrapper;

uint64_t HandleWrapper(uint64_t handle) {
    auto it = g_handle_wrapper.find(handle);
    if (it != g_handle_wrapper.end()) return it->second;
    uint64_t found = 0;
    if (FindWrapperTable(handle)) found = handle;
    uint64_t q[0x40] = {};
    // The handle points to the LayoutAnimator (+0x58 in the inventory); the animator's entries (+0x1a0, 0x30 bytes
    // each) carry the wrapper at +0x20 (LayoutAnimator::play, main+0x273b1e8).
    if (!found && RdBytes(handle, q, sizeof q))
        for (uint64_t v : q) {
            uint64_t entries = 0, n = 0, w = 0;
            if (v < 0x1000000 || v == handle || !Rd(v + 0x1a0, entries) || !Rd(v + 0x1a8, n) || n == 0 || n > 100000 ||
                !Rd(entries + 0x20, w) || w < 0x1000000)
                continue;
            if (FindWrapperTable(w)) {
                found = w;
                break;
            }
        }
    if (!found && g_ptr_trace) {
        std::string line;
        for (uint64_t v : q) line += " " + Hex(v);
        Log(PLA_LOG_INFO, "nnshim: pointer: no pane table for handle %llx:%s", static_cast<unsigned long long>(handle), line.c_str());
    }
    if (g_handle_wrapper.size() > 4096) g_handle_wrapper.clear();
    g_handle_wrapper[handle] = found;
    return found;
}

// Pane* of a widget through its wrapper's pane table (0 = not found).
uint64_t WidgetPane(uint64_t wdg, uint64_t hash) {
    uint64_t handle = 0;
    if (!Rd(wdg + 0xe8, handle) || !handle) return 0;
    const uint64_t wrapper = HandleWrapper(handle);
    if (!wrapper) return 0;
    const WrapperTable* t = FindWrapperTable(wrapper);
    if (!t) return 0;
    auto it = t->index.find(hash);
    if (it == t->index.end()) return 0;
    const uint64_t rec = t->base + 0x30ull * it->second;
    uint64_t h = 0, pane = 0;
    if (!Rd(rec, h) || h != hash || !Rd(rec + 0x20, pane)) {   // table changed: rebuild next time
        g_wrappers.erase(wrapper);
        return 0;
    }
    return pane;
}

void AddCtxItems(const CtxMenu& c, ptr::Snapshot& s) {
    int highlighted = 0;
    for (const auto& [i, has] : c.slots) highlighted += has ? 1 : 0;
    if (highlighted != 1) return;
    for (const auto& [i, r] : c.rows) {
        ptr::Item it;
        it.r = r;
        if (c.has_window) {   // the whole row of the window is clickable
            it.r.x0 = std::min(r.x0, c.window.x0);
            it.r.x1 = std::max(r.x0, c.window.x1);
        }
        it.group = c.root;
        it.layer = 1 << 20;   // a context menu is modal and drawn over its list (the draw order is not reliable
                              // here: some layouts are drawn more than once per frame)
        it.id = static_cast<uint64_t>(i) + 1;
        auto sl = c.slots.find(i);
        it.selected = sl != c.slots.end() && sl->second;
        s.items.push_back(it);
    }
}

void TraceSnapshot(const ptr::Snapshot& s, const std::unordered_map<uint64_t, std::string>& keys) {
    static size_t last_n = static_cast<size_t>(-1);
    static uint64_t last_sel = 0;
    uint64_t sel = 0;
    for (const auto& it : s.items)
        if (it.selected) sel = sel * 31 + it.id;
    if (s.items.size() == last_n && sel == last_sel) return;
    last_n = s.items.size();
    last_sel = sel;
    std::string line;
    std::set<uint64_t> groups;
    for (const auto& it : s.items)
        if (groups.insert(it.group).second) {
            auto k = keys.find(it.id);
            char b[64];
            std::snprintf(b, sizeof b, " %.3f,%.3f..%.3f,%.3f L%d", it.r.x0, it.r.y0, it.r.x1, it.r.y1, it.layer);
            line += " {" + (k != keys.end() ? k->second : std::string("ctx")) + b + "}";
        }
    line += " sel:";
    for (const auto& it : s.items)
        if (it.selected) {
            char b[112];
            std::snprintf(b, sizeof b, " [%llx:%llx %.3f,%.3f..%.3f,%.3f]", static_cast<unsigned long long>(it.group & 0xffffff),
                          static_cast<unsigned long long>(it.id & 0xffffff), it.r.x0, it.r.y0, it.r.x1, it.r.y1);
            line += b;
        }
    Log(PLA_LOG_INFO, "nnshim: pointer: frame %llu items=%zu groups:%s", static_cast<unsigned long long>(s.seq),
        s.items.size(), line.c_str());
}

void BuildSnapshot() {
    const bool active = g_ptr_enabled && hid::MenuActive() && PLA_API_HAS(g_api, host_pointer);
    g_rt_active.store(active, std::memory_order_relaxed);
    std::unordered_map<uint64_t, PaneRec> panes;
    std::vector<CtxMenu> ctx;
    std::unordered_set<uint64_t> widgets;
    ptr::Rect map_cursor;
    bool has_map = false;
    {
        std::lock_guard lk(g_rt_mu);
        // Panes of the last complete draw pass; widgets updated in the last two frames.
        const Pass& p = g_pass_done.panes.empty() ? g_pass_cur : g_pass_done;
        panes = p.panes;
        ctx = p.ctx;
        map_cursor = p.map;
        has_map = p.has_map;
        if (!active) {   // menu closed: forget the screen
            g_pass_cur = Pass{};
            g_pass_done = Pass{};
            g_pass_layouts.clear();
        }
        widgets = g_widgets_prev;
        widgets.insert(g_widgets_cur.begin(), g_widgets_cur.end());
        g_widgets_prev.swap(g_widgets_cur);
        g_widgets_cur.clear();
    }
    ptr::Snapshot s;
    s.seq = ++g_seq;
    std::unordered_map<uint64_t, std::string> keys;   // trace only
    if (active) {
        for (uint64_t wdg : widgets) {
            uint8_t st[4] = {};
            uint64_t hash = 0, owner = 0;
            if (!RdBytes(wdg + kBtnSelected, st, 4) || !Rd(wdg + kBtnPane, hash) || !Rd(wdg + kBtnOwner, owner)) continue;
            if (!st[kBtnVisible - kBtnSelected]) continue;
            const uint64_t pane = WidgetPane(wdg, hash);
            auto it = pane ? panes.find(pane) : panes.find(hash);
            if (it == panes.end()) continue;
            const ptr::Rect& r = it->second.r;
            if (r.W() < 0.004f || r.H() < 0.004f) continue;   // clipped away (scrolled out of a list)
            ptr::Item item;
            item.r = r;
            item.group = owner ^ it->second.prefix;
            item.id = wdg;
            item.layer = it->second.layer;
            item.selected = st[0] != 0;
            int rule = it->second.rule;
            {   // rules by button id ("button_pocket_L_00", +0x68 HashedName of the widget) win over pane keys
                uint64_t sp = 0, sl = 0;
                char id[64] = {};
                if (Rd(wdg + 0x70, sp) && Rd(wdg + 0x78, sl) && sl > 0 && sl < 63 && RdBytes(sp, id, static_cast<size_t>(sl)))
                    if (g_ptr_trace) keys[wdg] = it->second.key + "/" + id;
                if (id[0])
                    for (size_t i = 0; i < g_rules.size(); ++i)
                        if (!g_rules[i].prefix.compare(0, 7, "button_") &&
                            !std::strncmp(id, g_rules[i].prefix.c_str(), g_rules[i].prefix.size())) {
                            rule = static_cast<int>(i);
                            break;
                        }
            }
            if (rule >= 0) {
                const NavRule& nr = g_rules[static_cast<size_t>(rule)];
                item.up = nr.up;
                item.down = nr.down;
                item.left = nr.left;
                item.right = nr.right;
                item.no_accept = nr.no_accept;
                item.accept = nr.click;
            }
            if (g_ptr_trace) {
                if (item.selected) {   // state of the highlighted button (decide state machine at +0xf0)
                    static std::unordered_map<uint64_t, std::string> last_by_group;
                    std::string& last = last_by_group[item.group];
                    uint8_t b[16] = {};
                    uint32_t state = 0;
                    uint8_t f258 = 0, f2a8[8] = {};
                    RdBytes(wdg + 0xa0, b, 8);
                    Rd(wdg + 0xf0, state);
                    Rd(wdg + 0x258, f258);
                    RdBytes(wdg + 0x2a8, f2a8, 8);
                    char line[160];
                    std::snprintf(line, sizeof line, "%s a0=%02x%02x%02x%02x st=%u f258=%u 2a8=%02x%02x%02x%02x%02x%02x",
                                  it->second.key.c_str(), b[0], b[1], b[2], b[3], state, f258, f2a8[0], f2a8[1], f2a8[2], f2a8[3],
                                  f2a8[4], f2a8[5]);
                    if (last != line) {
                        last = line;
                        Log(PLA_LOG_INFO, "nnshim: pointer: widget %s", line);
                    }
                }
            }
            s.items.push_back(item);
        }
        for (const CtxMenu& c : ctx) AddCtxItems(c, s);
        s.has_stick_cursor = has_map;
        s.stick_cursor = map_cursor;
    }
    if (g_ptr_trace && active) {
        static size_t last_w = 0;
        if (widgets.size() != last_w) {
            last_w = widgets.size();
            Log(PLA_LOG_INFO, "nnshim: pointer: %zu live widgets, %zu drawn panes", widgets.size(), panes.size() / 2);
        }
        TraceSnapshot(s, keys);
    }
    std::lock_guard lk(g_snap_mu);
    g_snap = std::move(s);
}

// Host side state (hid polling thread).
ptr::Navigator g_nav;
bool g_left_prev = false, g_owns_left = false, g_right_prev = false;
float g_back_left_ms = 0;
int g_back_ms = 250;
float g_press_x = 0, g_press_y = 0;
std::mutex g_virt_mu;
bool g_virt_set = false;
float g_virt_x = 0, g_virt_y = 0;

void InitPointerRuntime(const cfg::Value& c) {
    cfg::Value v;
    std::string controls;
    const bool have = LoadControlsFile(c, v, controls);
    auto gb = [&](const char* k, bool d) { return have ? v.GetBool(k, c.GetBool(k, d)) : c.GetBool(k, d); };
    auto gi = [&](const char* k, int d) {
        return static_cast<int>(have ? v.GetInt(k, c.GetInt(k, d)) : c.GetInt(k, d));
    };
    auto gs = [&](const char* k, const char* d) { return have ? v.GetString(k, c.GetString(k, d)) : c.GetString(k, d); };
    g_ptr_enabled = gb("ui.mouse_pointer", true);
    g_ptr_trace = gb("ui.pointer_trace", false);
    g_rules = ParseRules(gs("ui.pointer_rules",
                            "pane_L_menu_:,,ZL,ZR:noaccept;button_pocket_L:::click=L;button_pocket_R:::click=R"));
    g_nav_cfg.up = hid::BtnUp;
    g_nav_cfg.down = hid::BtnDown;
    g_nav_cfg.left = hid::BtnLeft;
    g_nav_cfg.right = hid::BtnRight;
    g_nav_cfg.accept = hid::ButtonsFromList(gs("ui.pointer_accept", "A"));
    g_nav_cfg.dwell_ms = gi("ui.pointer_dwell_ms", g_nav_cfg.dwell_ms);
    g_nav_cfg.press_ms = gi("ui.pointer_press_ms", g_nav_cfg.press_ms);
    g_nav_cfg.accept_ms = gi("ui.pointer_accept_ms", g_nav_cfg.accept_ms);
    g_back_ms = gi("ui.pointer_back_ms", g_back_ms);
    g_nav_cfg.settle_ms = gi("ui.pointer_settle_ms", g_nav_cfg.settle_ms);
    g_nav_cfg.gap_ms = gi("ui.pointer_gap_ms", g_nav_cfg.gap_ms);
    g_nav_cfg.hover_select = gb("ui.pointer_hover_select", true);
    g_nav_cfg.outside_click_accept = gs("ui.pointer_outside_click", "ignore") == std::string("accept");
    Log(PLA_LOG_INFO, "nnshim: pointer: %s (hover=%d dwell=%d ms press=%d ms)", g_ptr_enabled ? "on" : "off",
        g_nav_cfg.hover_select ? 1 : 0, g_nav_cfg.dwell_ms, g_nav_cfg.press_ms);
}

} // namespace

bool ptr::Enabled() { return g_ptr_enabled; }

int ptr::AcceptMs() { return g_nav_cfg.accept_ms; }

bool ptr::VirtualPos(float& x, float& y) {
    std::lock_guard lk(g_virt_mu);
    if (!g_virt_set) return false;
    x = g_virt_x;
    y = g_virt_y;
    return true;
}

// "ptr <x> <y>" (normalized 0..1) sets the test pointer; "ptr off" removes it.
bool ptr::RemoteCommand(const std::string& cmd, const std::string& args) {
    if (cmd != "ptr") return false;
    std::lock_guard lk(g_virt_mu);
    float x = 0, y = 0;
    if (std::sscanf(args.c_str(), "%f %f", &x, &y) == 2) {
        g_virt_set = true;
        g_virt_x = x;
        g_virt_y = y;
    } else g_virt_set = false;
    return true;
}

void ptr::TraceButtons(uint64_t buttons, int32_t lx, int32_t ly) {
    static uint64_t last = 0;
    static int32_t llx = 0, lly = 0;
    if (!g_ptr_trace || (buttons == last && lx == llx && ly == lly)) return;
    last = buttons;
    llx = lx;
    lly = ly;
    Log(PLA_LOG_INFO, "nnshim: pointer: mouse gives buttons=%llx stick=%d,%d", static_cast<unsigned long long>(buttons), lx, ly);
}

void ptr::HostReset() {
    g_nav.Reset();
    g_left_prev = g_owns_left = g_right_prev = false;
    g_back_left_ms = 0;
}

void ptr::HostPoll(const HostSample& in, HostResult& out) {
    out = HostResult{};
    ptr::Snapshot s;
    {
        std::lock_guard lk(g_snap_mu);
        s = g_snap;
    }
    ptr::Navigator::In ni;
    ni.has_pos = in.has_pos;
    ni.x = in.x;
    ni.y = in.y;
    ni.dt_ms = in.dt_ms;
    if (in.left && !g_left_prev) {   // press: the pointer owns it if it starts on an item (or items are shown)
        const int hv = in.has_pos ? ptr::HitTest(s, in.x, in.y) : -1;
        g_owns_left = hv >= 0 || (!s.items.empty() && !g_nav_cfg.outside_click_accept) || s.has_stick_cursor;
        g_press_x = in.x;
        g_press_y = in.y;
    }
    if (in.left && g_owns_left && s.has_stick_cursor) {   // a drag on the map: hand it to the menu mouse (pan)
        const float dx = (in.x - g_press_x) * g_nav_cfg.aspect, dy = in.y - g_press_y;
        if (std::sqrt(dx * dx + dy * dy) >= 0.03f) g_owns_left = false;
    }
    if (!in.left && g_left_prev && g_owns_left) {
        const float dx = (in.x - g_press_x) * g_nav_cfg.aspect, dy = in.y - g_press_y;
        ni.click = std::sqrt(dx * dx + dy * dy) < 0.03f;   // released close to where it was pressed
        if (ni.click) CounterAdd("ui.pointer.clicks");
    }
    out.owns_left = g_owns_left;
    if (in.right && !g_right_prev) g_back_left_ms = static_cast<float>(g_back_ms);   // short clicks: long enough B
    out.right = in.right || g_back_left_ms > 0;
    g_back_left_ms -= in.dt_ms;
    g_right_prev = in.right;
    if (!in.left && g_left_prev) g_owns_left = false;
    g_left_prev = in.left;
    ptr::Navigator::Out no;
    g_nav.Step(s, ni, g_nav_cfg, no);
    out.buttons = no.buttons;
    out.stick = no.stick;
    out.lx = no.lx;
    out.ly = no.ly;
    static uint64_t prev_buttons = 0;
    if (no.buttons && no.buttons != prev_buttons) {
        CounterAdd(no.buttons == g_nav_cfg.accept ? "ui.pointer.accepts" : "ui.pointer.presses");
        if (g_ptr_trace) {
            const int sel = no.hover >= 0 ? ptr::SelectedIn(s, s.items[no.hover].group) : -1;
            Log(PLA_LOG_INFO, "nnshim: pointer: press %llx at %.3f,%.3f hover=%llx selected=%llx items=%zu",
                static_cast<unsigned long long>(no.buttons), in.x, in.y,
                static_cast<unsigned long long>(no.hover >= 0 ? s.items[no.hover].id & 0xffffff : 0),
                static_cast<unsigned long long>(sel >= 0 ? s.items[sel].id & 0xffffff : 0), s.items.size());
        }
    }
    if (ni.click && g_ptr_trace)
        Log(PLA_LOG_INFO, "nnshim: pointer: click at %.3f,%.3f hover=%d items=%zu", in.x, in.y, no.hover, s.items.size());
    prev_buttons = no.buttons;
}

// ---- button widget update (vf16, main+0x2708dd8): registry of live buttons -----------------------------------
NNSHIM_ADAPTER(Ui, nnshim_ui_WidgetTick) {
    if (!g_rt_active.load(std::memory_order_relaxed)) return PLA_CONTINUE;
    const uint64_t w = g_api->get_x(ctx, 0);
    std::lock_guard lk(g_rt_mu);
    g_widgets_cur.insert(w);
    return PLA_CONTINUE;
}

// ---- nn::ui2d::Layout::Draw(DrawInfo&, CommandBuffer&) (main+0x1353e0) ---------------------------------------
NNSHIM_ADAPTER(Ui, nnshim_ui_LayoutDraw) {
    if (!PLA_API_HAS(g_api, host_pointer)) return PLA_CONTINUE;
    const uint64_t layout = g_api->get_x(ctx, 0), draw_info = g_api->get_x(ctx, 1);
    if (!g_dump_dir.empty()) {
        std::lock_guard lk(g_dump_mu);
        if (g_dump_active && g_dump_layouts.insert(layout).second) DumpLayout(layout, draw_info);
    }
    if (g_rt_active.load(std::memory_order_relaxed)) CollectLayout(layout, draw_info);
    return PLA_CONTINUE;
}

// ---- LayoutAnimator::play(const HashedName&) (main+0x273b1e8) --------------------------------------------
// HashedName = { u64 fnv1a64, const char* str, u64 len, RefCounted* }. Diagnostic log only.
NNSHIM_ADAPTER(Ui, nnshim_ui_AnimPlay) {
    if (!g_anim_log || !PLA_API_HAS(g_api, host_pointer)) return PLA_CONTINUE;
    const uint64_t animator = g_api->get_x(ctx, 0), hn = g_api->get_x(ctx, 1), lr = g_api->get_x(ctx, 30);
    uint64_t h[3] = {};
    RdBytes(hn, h, sizeof h);
    char str[65] = {};
    if (h[1] && h[2] && h[2] < 64) RdBytes(h[1], str, static_cast<size_t>(h[2]));
    // Matching entry (0x30 bytes: hash, ..., +0x20 owner object, +0x28 index).
    uint64_t entries = 0, n = 0, owner = 0, index = 0;
    Rd(animator + 0x1a0, entries);
    Rd(animator + 0x1a8, n);
    for (uint64_t i = 0; i < n && i < 4096; ++i) {
        uint64_t eh = 0;
        if (!Rd(entries + i * 0x30, eh)) break;
        if (eh == h[0]) {
            Rd(entries + i * 0x30 + 0x20, owner);
            Rd(entries + i * 0x30 + 0x28, index);
            break;
        }
    }
    std::lock_guard lk(g_anim_mu);
    if (g_anim_lines++ > 200000) return PLA_CONTINUE;
    if ((g_anim_lines % 64) == 0) std::fflush(g_anim_log);
    std::fprintf(g_anim_log, "%llu anim=%llx hash=%016llx str=%s owner=%llx idx=%u lr=main+%llx\n",
                 static_cast<unsigned long long>(UptimeMs()), static_cast<unsigned long long>(animator),
                 static_cast<unsigned long long>(h[0]), str, static_cast<unsigned long long>(owner),
                 static_cast<unsigned>(index & 0xffffffff), static_cast<unsigned long long>(lr - g_main));
    return PLA_CONTINUE;
}

// ---- generic object logger (diagnostics) -------------------------------------------------------------------
// "pre" for any game function: logs pc, x0, x1, lr for the first calls of each (pc, x0) pair, and the first
// 0x2c0 bytes of each new x0 object (with the HashedName strings it holds). Counts go to the log at shutdown.
namespace {
std::mutex g_obj_mu;
std::map<std::pair<uint64_t, uint64_t>, uint64_t> g_obj_calls;
std::set<uint64_t> g_obj_seen;
std::FILE* g_obj_log = nullptr;

void ObjDump(uint64_t obj) {
    uint64_t q[0x2c0 / 8] = {};
    if (!RdBytes(obj, q, sizeof q)) return;
    std::fprintf(g_obj_log, "  obj %llx:", static_cast<unsigned long long>(obj));
    for (size_t i = 0; i < 0x2c0 / 8; ++i)
        std::fprintf(g_obj_log, "%s%03zx=%llx", (i % 8) ? " " : "\n    ", i * 8, static_cast<unsigned long long>(q[i]));
    std::fprintf(g_obj_log, "\n");
    // HashedName = {hash, str, len, ref}: print every slot that looks like one.
    for (size_t i = 0; i + 2 < 0x2c0 / 8; ++i) {
        const uint64_t len = q[i + 2];
        if (!q[i + 1] || len == 0 || len > 96) continue;
        char s[97] = {};
        if (!RdBytes(q[i + 1], s, static_cast<size_t>(len))) continue;
        bool ok = true;
        for (size_t k = 0; k < len; ++k) ok = ok && s[k] >= 0x20 && s[k] < 0x7f;
        if (ok) std::fprintf(g_obj_log, "    name@%03zx = %s\n", i * 8, s);
    }
}
} // namespace

NNSHIM_EXPORT pla_action nnshim_ui_ObjLog(pla_ctx* ctx) {
    if (g_dump_dir.empty() || !PLA_API_HAS(g_api, host_pointer)) return PLA_CONTINUE;
    const uint64_t pc = g_api->get_pc(ctx) - g_main, x0 = g_api->get_x(ctx, 0), x1 = g_api->get_x(ctx, 1),
                   lr = g_api->get_x(ctx, 30) - g_main;
    std::lock_guard lk(g_obj_mu);
    if (!g_obj_log) {
        g_obj_log = _wfopen((stdfs::u8path(g_dump_dir) / "obj-log.txt").c_str(), L"wb");
        if (!g_obj_log) return PLA_CONTINUE;
    }
    const uint64_t n = ++g_obj_calls[{pc, x0}];
    if (n <= 3 || (n % 600) == 0) {
        std::fprintf(g_obj_log, "%llu pc=%llx x0=%llx x1=%llx lr=%llx n=%llu", static_cast<unsigned long long>(UptimeMs()),
                     static_cast<unsigned long long>(pc), static_cast<unsigned long long>(x0),
                     static_cast<unsigned long long>(x1), static_cast<unsigned long long>(lr),
                     static_cast<unsigned long long>(n));
        uint64_t in[4] = {};
        if (x1 > 0x1000000 && RdBytes(x1, in, sizeof in))
            std::fprintf(g_obj_log, " [x1]=%llx %llx %llx %llx", static_cast<unsigned long long>(in[0]),
                         static_cast<unsigned long long>(in[1]), static_cast<unsigned long long>(in[2]),
                         static_cast<unsigned long long>(in[3]));
        std::fprintf(g_obj_log, "\n");
    }
    if (g_obj_seen.insert(x0).second && g_obj_seen.size() < 400) ObjDump(x0);
    std::fflush(g_obj_log);
    return PLA_CONTINUE;
}

// ---- input manager probe (diagnostics): main+0xc9a9b4, once per frame, X0 = the game's input manager ----------
// While A is held in the shim's pad state (and 300 ms after), logs the manager's words +0x80..+0x120 to find
// where the game keeps the pressed / triggered / repeated buttons.
NNSHIM_EXPORT pla_action nnshim_ui_InputProbe(pla_ctx* ctx) {
    if (g_dump_dir.empty() || !PLA_API_HAS(g_api, host_pointer)) return PLA_CONTINUE;
    static int64_t last_a = -100000;
    static int lines = 0;
    hid::NpadState st{};
    hid::GetState(hid::StyleFullKey, hid::NpadIdNo1, st);
    const int64_t now = hid::NowMs();
    if (st.buttons & hid::BtnA) last_a = now;
    if (now - last_a > 300 || lines > 3000) return PLA_CONTINUE;
    ++lines;
    const uint64_t mgr = g_api->get_x(ctx, 0);
    uint64_t q[0x28] = {};
    RdBytes(mgr, q, sizeof q);
    std::lock_guard lk(g_obj_mu);
    if (!g_obj_log) g_obj_log = _wfopen((stdfs::u8path(g_dump_dir) / "obj-log.txt").c_str(), L"wb");
    if (!g_obj_log) return PLA_CONTINUE;
    std::fprintf(g_obj_log, "%lld in=%llx mgr=%llx", static_cast<long long>(now), static_cast<unsigned long long>(st.buttons),
                 static_cast<unsigned long long>(mgr));
    for (int i = 0; i < 0x28; ++i) std::fprintf(g_obj_log, " %llx", static_cast<unsigned long long>(q[i]));
    std::fprintf(g_obj_log, "\n");
    std::fflush(g_obj_log);
    return PLA_CONTINUE;
}

// ---- button input probe (diagnostics): vf24 of the UI kit button (main+0x27092fc), X1 = input words ------------
NNSHIM_EXPORT pla_action nnshim_ui_DecideProbe(pla_ctx* ctx) {
    if (g_dump_dir.empty() || !PLA_API_HAS(g_api, host_pointer)) return PLA_CONTINUE;
    static int64_t last_a = -100000;
    static int lines = 0;
    hid::NpadState st{};
    hid::GetState(hid::StyleFullKey, hid::NpadIdNo1, st);
    const int64_t now = hid::NowMs();
    if (st.buttons & hid::BtnA) last_a = now;
    if (now - last_a > 300 || lines > 3000) return PLA_CONTINUE;
    ++lines;
    const uint64_t w = g_api->get_x(ctx, 0), in = g_api->get_x(ctx, 1), lr = g_api->get_x(ctx, 30) - g_main;
    uint64_t iw[3] = {}, mask = 0;
    uint8_t f[4] = {};
    uint32_t state = 0;
    RdBytes(in, iw, sizeof iw);
    RdBytes(w + 0xa0, f, 4);
    Rd(w + 0xf0, state);
    Rd(w + 0x260, mask);
    char name[64] = {};
    uint64_t sp = 0, sl = 0;
    if (Rd(w + 0xb0, sp) && Rd(w + 0xb8, sl) && sl < 63) RdBytes(sp, name, static_cast<size_t>(sl));
    std::lock_guard lk(g_obj_mu);
    if (!g_obj_log) g_obj_log = _wfopen((stdfs::u8path(g_dump_dir) / "obj-log.txt").c_str(), L"wb");
    if (!g_obj_log) return PLA_CONTINUE;
    std::fprintf(g_obj_log, "%lld decide w=%llx %s in=%llx %llx %llx a0=%02x%02x%02x%02x st=%u mask=%llx lr=%llx\n",
                 static_cast<long long>(now), static_cast<unsigned long long>(w), name, static_cast<unsigned long long>(iw[0]),
                 static_cast<unsigned long long>(iw[1]), static_cast<unsigned long long>(iw[2]), f[0], f[1], f[2], f[3], state,
                 static_cast<unsigned long long>(mask), static_cast<unsigned long long>(lr));
    std::fflush(g_obj_log);
    return PLA_CONTINUE;
}
