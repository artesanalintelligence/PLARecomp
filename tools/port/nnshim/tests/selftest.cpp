// Test of the nn shim without the game: a fake pla_api (registers + guest memory in a vector)
// calls the adapters just like suyu's mod runtime would.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "nnshim/config.hpp"
#include "nnshim/fs.hpp"
#include "nnshim/hid.hpp"
#include "nnshim/pointer.hpp"
#include "nnshim/romfs.hpp"
#include "nnshim/runtime.hpp"
#include "nnshim/services.hpp"
#include "nnshim/table.h"
#include "nnshim/ui.hpp"
#include "pla_mod_api.h"

struct pla_ctx { uint64_t x[31]; uint64_t sp; uint64_t v[32][2]; };  // fake context (opaque in pla_mod_api.h)

#define ADAPTERS(X)                                                                                         \
    X(nnshim_fs_MountRom) X(nnshim_fs_MountSaveData) X(nnshim_fs_MountSaveDataReadOnly) X(nnshim_fs_EnsureSaveData) \
    X(nnshim_fs_OpenFile) X(nnshim_fs_ReadFileOut) X(nnshim_fs_WriteFile) X(nnshim_fs_GetFileSize)          \
    X(nnshim_fs_CloseFile) X(nnshim_fs_CreateFile) X(nnshim_fs_GetEntryType) X(nnshim_fs_OpenDirectory)     \
    X(nnshim_fs_ReadDirectory) X(nnshim_fs_CloseDirectory) X(nnshim_fs_Commit) X(nnshim_fs_Unmount)         \
    X(nnshim_fs_IsSaveDataExistingApp)                                                                      \
    X(nnshim_hid_GetNpadStyleSet) X(nnshim_hid_GetNpadState_FullKey) X(nnshim_hid_GetSixAxisSensorHandles)  \
    X(nnshim_hid_GetSixAxisSensorStates) X(nnshim_hid_EnqueueVibrationValue) X(nnshim_hid_GetVibrationDeviceHandles) \
    X(nnshim_time_UserGetCurrentTime) X(nnshim_time_ToCalendarTimeInUtc) X(nnshim_time_ToCalendarTime)      \
    X(nnshim_time_LoadTimeZoneRule) X(nnshim_time_ToPosixTime) X(nnshim_time_CreateWithStandardSystemClock) \
    X(nnshim_time_Initialize) X(nnshim_time_IsInitialized)                                                  \
    X(nnshim_settings_LanguageCodeMake) X(nnshim_settings_LanguageEqCode) X(nnshim_oe_GetDesiredLanguage)    \
    X(nnshim_oe_GetOperationMode) X(nnshim_oe_TryPopNotificationMessage) X(nnshim_account_TryOpenPreselectedUser) \
    X(nnshim_account_GetUserId) X(nnshim_account_GetNickname) X(nnshim_account_IsNetworkServiceAccountAvailable) \
    X(nnshim_account_AsyncContextHasDone) X(nnshim_account_EnsureNetworkServiceAccountIdTokenCacheAsync)    \
    X(nnshim_diag_AbortImplFormat) X(nnshim_diag_GetBacktrace) X(nnshim_err_ApplicationErrorArgCtor)        \
    X(nnshim_err_SetDialogMessage) X(nnshim_swkbd_ShowKeyboard) X(nnshim_swkbd_GetRequiredWorkBufferSizeInt) \
    X(nnshim_hid_BindNpadStyleSetUpdateEvent) X(nnshim_account_AsyncContextGetSystemEvent) X(nnshim_pl_GetSharedFontAddress) \
    X(nnshim_aoc_CountAddOnContent) X(nnshim_nifm_IsNetworkAvailable) X(nnshim_friends_GetFriendList) \
    X(nnshim_os_GenerateRandomBytes) X(nnshim_account_ListOpenUsers)

extern "C" {
uint32_t pla_mod_api_version(void);
int pla_mod_init(const pla_api*, const char*);
void pla_mod_shutdown(void);
#define DECL(n) pla_action n(pla_ctx*);
ADAPTERS(DECL)
#undef DECL
void nnshim_time_ToCalendarTimeInUtc_post(pla_ctx*);
void nnshim_time_UserGetCurrentTime_post(pla_ctx*);
void nnshim_oe_GetOperationMode_post(pla_ctx*);
void nnshim_account_GetUserId_post(pla_ctx*);
}

namespace {

constexpr uint64_t kBase = 0x10000000;
std::vector<uint8_t> g_mem(4 << 20);

uint64_t get_x(pla_ctx* c, unsigned n) { return n < 31 ? c->x[n] : 0; }
void set_x(pla_ctx* c, unsigned n, uint64_t v) { if (n < 31) c->x[n] = v; }
uint64_t get_sp(pla_ctx* c) { return c->sp; }
void set_sp(pla_ctx* c, uint64_t v) { c->sp = v; }
uint64_t zero(pla_ctx*) { return 0; }
void nop_set(pla_ctx*, uint64_t) {}
void get_v(pla_ctx* c, unsigned n, uint64_t out[2]) { out[0] = c->v[n & 31][0]; out[1] = c->v[n & 31][1]; }
void set_v(pla_ctx* c, unsigned n, uint64_t lo, uint64_t hi) { c->v[n & 31][0] = lo; c->v[n & 31][1] = hi; }
size_t read_mem(pla_ctx*, uint64_t va, void* dst, size_t n) {
    if (va < kBase || va - kBase >= g_mem.size()) return 0;
    n = std::min<size_t>(n, g_mem.size() - (va - kBase));
    std::memcpy(dst, &g_mem[va - kBase], n);
    return n;
}
size_t write_mem(pla_ctx*, uint64_t va, const void* src, size_t n) {
    if (va < kBase || va - kBase >= g_mem.size()) return 0;
    n = std::min<size_t>(n, g_mem.size() - (va - kBase));
    std::memcpy(&g_mem[va - kBase], src, n);
    return n;
}
uint64_t module_base(const char* m) { return std::strcmp(m, "main") == 0 ? kBase : 0; }
uint64_t g_call_data[4];
uint64_t* call_data(pla_ctx*) { return g_call_data; }
bool g_quiet = false;
void log_line(int, const char* m) { if (!g_quiet) std::printf("  [log] %s\n", m); }

pla_api MakeApi() {
    pla_api a{};
    a.version = PLA_MOD_API_VERSION;
    a.size = static_cast<uint32_t>(offsetof(pla_api, log) + sizeof(a.log));   // API 1.0 (without the extension)
    a.get_x = get_x; a.set_x = set_x; a.get_sp = get_sp; a.set_sp = set_sp; a.get_pc = zero; a.set_pc = nop_set;
    a.get_v = get_v; a.set_v = set_v; a.read_mem = read_mem; a.write_mem = write_mem; a.module_base = module_base;
    a.call_data = call_data; a.log = log_line;
    return a;
}
pla_api kApi = MakeApi();   // mutable: the event test adds the 1.1 extension

uint64_t g_heap = kBase + 0x1000;
uint64_t Alloc(size_t n) { uint64_t a = g_heap; g_heap += (n + 15) & ~size_t(15); return a; }
uint64_t Str(const char* s) { uint64_t a = Alloc(std::strlen(s) + 1); write_mem(nullptr, a, s, std::strlen(s) + 1); return a; }
uint64_t U16(const char16_t* s) {
    size_t n = 0;
    while (s[n]) ++n;
    uint64_t a = Alloc((n + 1) * 2);
    write_mem(nullptr, a, s, (n + 1) * 2);
    return a;
}
template <class T> T Load(uint64_t va) { T v{}; read_mem(nullptr, va, &v, sizeof v); return v; }
template <class T> void StoreG(uint64_t va, const T& v) { write_mem(nullptr, va, &v, sizeof v); }

int g_fail = 0, g_ok = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    ok ? ++g_ok : ++g_fail;
}

pla_action Call(pla_action (*fn)(pla_ctx*), pla_ctx& c, std::initializer_list<uint64_t> args) {
    unsigned i = 0;
    for (uint64_t a : args) c.x[i++] = a;
    return fn(&c);
}

std::string ReadFileText(const std::filesystem::path& p) {
    std::string s;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        char buf[256];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

void WriteText(const std::filesystem::path& p, const char* s) {
    std::filesystem::create_directories(p.parent_path());
    FILE* f = _wfopen(p.c_str(), L"wb");
    std::fputs(s, f);
    std::fclose(f);
}

// Minimal RomFS image (format of nnshim/romfs.hpp) from (path, content) pairs, to test the reader
// without the game. Single empty-bucket hash tables (the reader does not use them).
std::vector<uint8_t> BuildRomfs(const std::vector<std::pair<std::string, std::string>>& files) {
    struct D { std::string name; int parent; std::vector<int> dirs, files; uint32_t off = 0; };
    struct F { std::string name; int parent; std::string data; uint32_t off = 0; uint64_t doff = 0; };
    std::vector<D> ds{{"", 0, {}, {}}};
    std::vector<F> fs;
    for (const auto& [path, data] : files) {
        int cur = 0;
        size_t i = 0;
        for (size_t j; (j = path.find('/', i)) != std::string::npos; i = j + 1) {
            const std::string part = path.substr(i, j - i);
            int found = -1;
            for (int c : ds[cur].dirs)
                if (ds[c].name == part) found = c;
            if (found < 0) {
                found = static_cast<int>(ds.size());
                ds.push_back(D{part, cur, {}, {}});
                ds[cur].dirs.push_back(found);
            }
            cur = found;
        }
        ds[cur].files.push_back(static_cast<int>(fs.size()));
        fs.push_back(F{path.substr(i), cur, data});
    }
    auto pad4 = [](size_t n) { return (n + 3) & ~size_t(3); };
    uint32_t o = 0;
    for (D& d : ds) { d.off = o; o += static_cast<uint32_t>(0x18 + pad4(d.name.size())); }
    const uint32_t dsize = o;
    o = 0;
    uint64_t data = 0;
    for (F& f : fs) {
        f.off = o;
        o += static_cast<uint32_t>(0x20 + pad4(f.name.size()));
        f.doff = data;
        data = (data + f.data.size() + 15) & ~uint64_t(15);
    }
    const uint32_t fsize = o;
    const uint64_t data_off = 0x200, tables = data_off + ((data + 3) & ~uint64_t(3));
    std::vector<uint8_t> img(static_cast<size_t>(tables + 4 + dsize + 4 + fsize), 0);
    auto put32 = [&](uint64_t at, uint32_t v) { std::memcpy(&img[static_cast<size_t>(at)], &v, 4); };
    auto put64 = [&](uint64_t at, uint64_t v) { std::memcpy(&img[static_cast<size_t>(at)], &v, 8); };
    const uint64_t dh = tables, dm = dh + 4, fh = dm + dsize, fm = fh + 4;
    const uint64_t hdr[10] = {0x50, dh, 4, dm, dsize, fh, 4, fm, fsize, data_off};
    std::memcpy(img.data(), hdr, sizeof hdr);
    put32(dh, 0xFFFFFFFFu);
    put32(fh, 0xFFFFFFFFu);
    for (size_t k = 0; k < ds.size(); ++k) {
        const D& d = ds[k];
        uint32_t sib = 0xFFFFFFFFu;
        if (k) {
            const auto& sibs = ds[d.parent].dirs;
            const auto it = std::find(sibs.begin(), sibs.end(), static_cast<int>(k));
            if (it + 1 != sibs.end()) sib = ds[*(it + 1)].off;
        }
        const uint64_t at = dm + d.off;
        put32(at, ds[d.parent].off);
        put32(at + 4, sib);
        put32(at + 8, d.dirs.empty() ? 0xFFFFFFFFu : ds[d.dirs[0]].off);
        put32(at + 12, d.files.empty() ? 0xFFFFFFFFu : fs[d.files[0]].off);
        put32(at + 16, 0xFFFFFFFFu);
        put32(at + 20, static_cast<uint32_t>(d.name.size()));
        std::memcpy(&img[static_cast<size_t>(at + 0x18)], d.name.data(), d.name.size());
    }
    for (size_t k = 0; k < fs.size(); ++k) {
        const F& f = fs[k];
        const auto& sibs = ds[f.parent].files;
        const auto it = std::find(sibs.begin(), sibs.end(), static_cast<int>(k));
        const uint64_t at = fm + f.off;
        put32(at, ds[f.parent].off);
        put32(at + 4, it + 1 != sibs.end() ? fs[*(it + 1)].off : 0xFFFFFFFFu);
        put64(at + 8, f.doff);
        put64(at + 16, f.data.size());
        put32(at + 24, 0xFFFFFFFFu);
        put32(at + 28, static_cast<uint32_t>(f.name.size()));
        std::memcpy(&img[static_cast<size_t>(at + 0x20)], f.name.data(), f.name.size());
        if (!f.data.empty()) std::memcpy(&img[static_cast<size_t>(data_off + f.doff)], f.data.data(), f.data.size());
    }
    return img;
}

} // namespace

int main() {
    namespace stdfs = std::filesystem;
    const stdfs::path tmp = stdfs::temp_directory_path() / "nnshim_selftest";
    stdfs::remove_all(tmp);
    WriteText(tmp / "romfs" / "bin" / "hello.txt", "test romfs");
    // Legacy file name: the config asks for controls.json, which does not exist -> controles.json is used.
    WriteText(tmp / "controles.json", "\xEF\xBB\xBF" R"(// test controls
{
  "mouse": { "sensitivity": 1.75, // overrides the mod.json
             "invert_y": true, "release_key": "F2" },
  "keyboard_map": { "ZR": "R" }
}
)");

    // ---- config (nested JSON)
    {
        nnshim::cfg::Value v;
        const bool ok = nnshim::cfg::Parse(R"({"a":{"b":[1,2,{"c":"x\"y)" "\xC3\xA9" R"("}],"n":-2.5e1},"t":true,"h":"0x10"})", v);
        Check(ok && v.GetNumber("a.n", 0) == -25 && v.GetBool("t", false) && v.GetInt("h", 0) == 16 &&
                  v.Find("a.b")->arr[2].GetString("c") == "x\"y\xC3\xA9",
              "config: nested JSON, escapes and \\u");
        nnshim::cfg::Value bad;
        Check(!nnshim::cfg::Parse("{\"a\":", bad), "config: invalid JSON is rejected");
    }

    // ---- mouse pointer in the game UI (pointer_core.cpp)
    {
        namespace p = nnshim::ptr;
        p::Mat34 g{{{1, 0, 0, 100}, {0, 1, 0, -50}, {0, 0, 1, 0}}};
        const p::Rect gr = p::PaneGlobalRect(g, 200, 100, 0);   // centered
        const p::Rect tl = p::PaneGlobalRect(g, 200, 100, 1 | (1 << 2));   // left, top
        Check(gr.x0 == 0 && gr.x1 == 200 && gr.y0 == -100 && gr.y1 == 0 && tl.x0 == 100 && tl.x1 == 300 && tl.y0 == -150 &&
                  tl.y1 == -50,
              "pointer: pane rect from global matrix and base position");
        p::Proj pr;
        const float pm[4][4] = {{2.0f / 1920, 0, 0, 0}, {0, 2.0f / 1080, 0, 0}, {0, 0, -0.00125f, 0}, {0, 0, 0, 1}};
        std::memcpy(pr.m, pm, sizeof pm);
        pr.valid = true;
        const p::Rect sr = p::GlobalToScreen(p::Rect{-960, 0, 0, 540}, pr);
        Check(std::fabs(sr.x0) < 1e-5f && std::fabs(sr.x1 - 0.5f) < 1e-5f && std::fabs(sr.y0) < 1e-5f &&
                  std::fabs(sr.y1 - 0.5f) < 1e-5f,
              "pointer: projection to normalized screen (y down)");
        // 3x2 grid of overlapping items (hit boxes wider than the stride), item 0 highlighted.
        p::Snapshot s;
        for (int i = 0; i < 6; ++i) {
            const float cx = 0.2f + 0.2f * static_cast<float>(i % 3), cy = 0.3f + 0.3f * static_cast<float>(i / 3);
            p::Item it;
            it.r = p::Rect{cx - 0.15f, cy - 0.12f, cx + 0.15f, cy + 0.12f};
            it.group = 7;
            it.id = static_cast<uint64_t>(i) + 1;
            it.selected = i == 0;
            s.items.push_back(it);
        }
        Check(p::HitTest(s, 0.29f, 0.3f) == 0 && p::HitTest(s, 0.31f, 0.3f) == 1 && p::HitTest(s, 0.95f, 0.95f) == -1,
              "pointer: hit test splits overlapping boxes by nearest center");
        p::NavConfig nc;
        nc.up = 1;
        nc.down = 2;
        nc.left = 4;
        nc.right = 8;
        nc.accept = 16;
        p::Navigator nav;
        p::Navigator::In in;
        in.has_pos = true;
        in.x = 0.6f;   // item 5 (column 2, row 1)
        in.y = 0.6f;
        in.dt_ms = 10;
        std::vector<uint64_t> seq;
        int sel = 0;
        for (int step = 0; step < 400 && sel != 5; ++step) {
            s.seq++;
            p::Navigator::Out o;
            nav.Step(s, in, nc, o);
            if (o.buttons && (seq.empty() || seq.back() != o.buttons)) seq.push_back(o.buttons);
            // the "game" moves the highlight when a press ends
            static uint64_t held = 0;
            if (held && !o.buttons) {
                int c = sel % 3, r = sel / 3;
                if (held == 8 && c < 2) ++c;
                if (held == 4 && c > 0) --c;
                if (held == 2 && r < 1) ++r;
                if (held == 1 && r > 0) --r;
                sel = r * 3 + c;
                for (int i = 0; i < 6; ++i) s.items[i].selected = i == sel;
                seq.push_back(0);
            }
            held = o.buttons;
        }
        int ups = 0, downs = 0, rights = 0, lefts = 0;
        for (uint64_t b : seq) {
            ups += b == 1;
            downs += b == 2;
            lefts += b == 4;
            rights += b == 8;
        }
        Check(sel == 5 && downs == 1 && rights == 2 && ups == 0 && lefts == 0, "pointer: hover drives the d-pad to the item");
        // Click on the highlighted item = accept, once.
        p::Navigator::In ci = in;
        int accepts = 0;
        for (int step = 0; step < 60; ++step) {
            s.seq++;
            ci.click = step == 0;
            p::Navigator::Out o;
            nav.Step(s, ci, nc, o);
            if (o.buttons == 16 && (step == 0 || true)) ++accepts;
        }
        Check(nav.Accepts() == 1 && accepts > 0, "pointer: click on the highlighted item presses accept once");
        // Click outside every item: consumed (ignored) by default.
        p::Navigator::In oi;
        oi.has_pos = true;
        oi.x = 0.97f;
        oi.y = 0.97f;
        oi.click = true;
        oi.dt_ms = 10;
        p::Navigator::Out oo;
        nav.Step(s, oi, nc, oo);
        Check(oo.click_consumed && oo.buttons == 0, "pointer: click outside the items is ignored");
    }

    const std::string root = tmp.generic_string();
    std::string cfg = "{\"input_backend\":\"none\",\"romfs_dir\":\"" + root + "/romfs\",\"save_root\":\"" + root +
                      "/save\",\"trace_path\":\"" + root + "/trace.txt\",\"stats_path\":\"" + root +
                      "/stats.json\",\"err\":{\"dialog\":false,\"dump_dir\":\"" + root + "/dumps\"}," +
                      "\"swkbd\":{\"mode\":\"auto\",\"auto_text\":\"Ari\"},\"settings\":{\"language\":\"es-419\"}," +
                      "\"account\":{\"nickname\":\"Tester\"},\"time\":{\"timezone\":\"+02:00\"}," +
                      "\"hid\":{\"keyboard_map\":{\"A\":\"F5\"},\"controls_file\":\"" + root + "/controls.json\"," +
                      "\"mouse\":{\"enabled\":false,\"sensitivity\":2.5,\"buttons\":{\"left\":\"A+B\"}}}}";
    Check(pla_mod_api_version() == PLA_MOD_API_VERSION, "pla_mod_api_version");
    Check(pla_mod_init(&kApi, cfg.c_str()) == 0, "pla_mod_init (API 1.0, no extension)");

    pla_ctx c{};
    c.sp = kBase + 0x3F0000;
    // --- romfs
    Check(Call(nnshim_fs_MountRom, c, {Str("rom")}) == PLA_SKIP && c.x[0] == 0, "MountRom(rom)");
    const uint64_t hptr = Alloc(8);
    Check(Call(nnshim_fs_OpenFile, c, {hptr, Str("rom:/bin/hello.txt"), 1}) == PLA_SKIP && c.x[0] == 0, "OpenFile(rom:/bin/hello.txt)");
    const uint64_t h = Load<uint64_t>(hptr);
    Check(nnshim::fs::IsShimFile(h), "handle tagged as the shim's");
    const uint64_t szp = Alloc(8);
    Call(nnshim_fs_GetFileSize, c, {szp, h});
    Check(c.x[0] == 0 && Load<int64_t>(szp) == 10, "GetFileSize == 10");
    const uint64_t buf = Alloc(64), outp = Alloc(8);
    Call(nnshim_fs_ReadFileOut, c, {outp, h, 5, buf, 64});
    char got[8] = {};
    read_mem(nullptr, buf, got, 5);
    Check(c.x[0] == 0 && Load<uint64_t>(outp) == 5 && std::memcmp(got, "romfs", 5) == 0, "ReadFile(offset 5) == \"romfs\"");
    Check(Call(nnshim_fs_CloseFile, c, {h}) == PLA_SKIP, "CloseFile");
    Check(Call(nnshim_fs_OpenFile, c, {hptr, Str("rom:/bin/hello.txt"), 2}) == PLA_SKIP && c.x[0] == nnshim::fs_result::InvalidOperationForOpenMode,
          "rom is read-only");
    Check(Call(nnshim_fs_OpenFile, c, {hptr, Str("sd:/otro.bin"), 1}) == PLA_CONTINUE, "foreign mount -> PLA_CONTINUE (HLE)");
    Check(Call(nnshim_fs_CloseFile, c, {0x1234}) == PLA_CONTINUE, "foreign handle -> PLA_CONTINUE");
    const uint64_t typ = Alloc(4);
    Call(nnshim_fs_GetEntryType, c, {typ, Str("rom:/bin")});
    Check(c.x[0] == 0 && Load<int32_t>(typ) == 0, "GetEntryType(dir) == Directory");
    Call(nnshim_fs_GetEntryType, c, {typ, Str("rom:/does/not/exist")});
    Check(c.x[0] == nnshim::fs_result::PathNotFound, "GetEntryType(missing) == PathNotFound");
    Call(nnshim_fs_GetEntryType, c, {typ, Str("rom:/../../etc")});
    Check(c.x[0] == nnshim::fs_result::InvalidPath, "'..' outside the mount -> InvalidPath");
    {
        char b[5] = {};
        uint64_t n = 0;
        Check(nnshim::fs::ReadRomfsFile("/bin/hello.txt", 0, b, 4, n) && n == 4 && std::memcmp(b, "test", 4) == 0,
              "validation: direct read of the host RomFS");
    }

    // --- directories
    const uint64_t dh = Alloc(8);
    Call(nnshim_fs_OpenDirectory, c, {dh, Str("rom:/bin"), 3});
    Check(c.x[0] == 0, "OpenDirectory(rom:/bin)");
    const uint64_t ents = Alloc(4 * sizeof(nnshim::fs::DirectoryEntry)), cnt = Alloc(8);
    Call(nnshim_fs_ReadDirectory, c, {cnt, ents, Load<uint64_t>(dh), 4});
    nnshim::fs::DirectoryEntry e0 = Load<nnshim::fs::DirectoryEntry>(ents);
    Check(c.x[0] == 0 && Load<int64_t>(cnt) == 1 && std::strcmp(e0.name, "hello.txt") == 0 && e0.size == 10,
          "ReadDirectory -> hello.txt (10 bytes)");
    Check(Call(nnshim_fs_CloseDirectory, c, {Load<uint64_t>(dh)}) == PLA_SKIP, "CloseDirectory");

    // --- transactional saves
    const uint64_t uid = Alloc(16);
    const uint64_t uidv[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
    write_mem(nullptr, uid, uidv, 16);
    const nnshim::fs::Uid fuid{uidv[0], uidv[1]};
    const stdfs::path save_dir = stdfs::u8path(nnshim::fs::SaveDirFor(0x01001F5010DFA000ull, fuid));
    Check(Call(nnshim_fs_MountSaveData, c, {Str("save"), uid}) == PLA_SKIP && c.x[0] == nnshim::fs_result::TargetNotFound,
          "MountSaveData without EnsureSaveData -> TargetNotFound");
    Call(nnshim_fs_EnsureSaveData, c, {uid});
    Check(c.x[0] == 0, "EnsureSaveData");
    Call(nnshim_fs_MountSaveData, c, {Str("save"), uid});
    Check(c.x[0] == 0, "MountSaveData(save)");
    Call(nnshim_fs_CreateFile, c, {Str("save:/main"), 4});
    Check(c.x[0] == 0, "CreateFile(save:/main, 4)");
    Call(nnshim_fs_OpenFile, c, {hptr, Str("save:/main"), 2});
    const uint64_t wh = Load<uint64_t>(hptr);
    const uint64_t data = Str("ABCDEFGH"), opt = Alloc(4);
    Call(nnshim_fs_WriteFile, c, {wh, 0, data, 8, opt});
    Check(c.x[0] == nnshim::fs_result::FileExtensionWithoutOpenModeAllowAppend, "WriteFile past the end without AllowAppend fails");
    Call(nnshim_fs_WriteFile, c, {wh, 0, data, 4, opt});
    Check(c.x[0] == 0, "WriteFile(4 bytes)");
    Call(nnshim_fs_CloseFile, c, {wh});
    Check(!stdfs::exists(save_dir / "main"), "journal: without Commit the committed save is unchanged");
    Call(nnshim_fs_Commit, c, {Str("save")});
    Check(c.x[0] == 0 && ReadFileText(save_dir / "main") == "ABCD", "Commit publishes the save (ABCD)");
    Call(nnshim_fs_OpenFile, c, {hptr, Str("save:/main"), 1 | 2 | 4});
    Call(nnshim_fs_WriteFile, c, {Load<uint64_t>(hptr), 4, data + 4, 4, opt});
    Check(c.x[0] == 0, "WriteFile with AllowAppend extends the file");
    Call(nnshim_fs_GetFileSize, c, {szp, Load<uint64_t>(hptr)});
    Check(Load<int64_t>(szp) == 8, "final size == 8");
    Call(nnshim_fs_CloseFile, c, {Load<uint64_t>(hptr)});
    Call(nnshim_fs_Unmount, c, {Str("save")});
    Check(ReadFileText(save_dir / "main") == "ABCD" && !stdfs::exists(stdfs::path(save_dir) += ".work"),
          "Unmount without Commit discards the changes (still ABCD)");
    Call(nnshim_fs_MountSaveData, c, {Str("save"), uid});
    Call(nnshim_fs_OpenFile, c, {hptr, Str("save:/main"), 2});
    Call(nnshim_fs_WriteFile, c, {Load<uint64_t>(hptr), 0, Str("WXYZ"), 4, opt});
    Call(nnshim_fs_CloseFile, c, {Load<uint64_t>(hptr)});
    Call(nnshim_fs_Commit, c, {Str("save")});
    Call(nnshim_fs_Unmount, c, {Str("save")});
    Check(ReadFileText(save_dir / "main") == "WXYZ", "second Commit + Unmount: WXYZ");
    // Simulated crash between the two renames of Commit: <dir> is missing, <dir>.commit exists.
    {
        stdfs::path commit = save_dir, old = save_dir;
        commit += ".commit";
        old += ".old";
        stdfs::copy(save_dir, commit, stdfs::copy_options::recursive);
        WriteText(commit / "main", "NEW!");
        stdfs::rename(save_dir, old);
        Call(nnshim_fs_MountSaveData, c, {Str("save"), uid});
        Check(c.x[0] == 0 && ReadFileText(save_dir / "main") == "NEW!" && !stdfs::exists(old) && !stdfs::exists(commit),
              "recovery: Commit interrupted after the first rename -> new version");
        Call(nnshim_fs_Unmount, c, {Str("save")});
        stdfs::rename(save_dir, old);   // crash before the copy finished: only .old
        Call(nnshim_fs_MountSaveData, c, {Str("save"), uid});
        Check(c.x[0] == 0 && ReadFileText(save_dir / "main") == "NEW!", "recovery: only <dir>.old -> previous version");
        Call(nnshim_fs_Unmount, c, {Str("save")});
    }
    // Saves of other games (bonuses).
    {
        const uint64_t other = 0x0100ABF008968000ull;   // an arbitrary ApplicationId
        Check(Call(nnshim_fs_MountSaveDataReadOnly, c, {Str("other"), other, uid}) == PLA_SKIP &&
                  c.x[0] == nnshim::fs_result::TargetNotFound,
              "missing save of another game -> TargetNotFound");
        Call(nnshim_fs_IsSaveDataExistingApp, c, {other, uid});
        Check(c.x[0] == 0, "IsSaveDataExisting(other game) == false");
        WriteText(stdfs::u8path(nnshim::fs::SaveDirFor(other, fuid)) / "data", "x");
        Call(nnshim_fs_MountSaveDataReadOnly, c, {Str("other"), other, uid});
        Check(c.x[0] == 0, "copied save of another game -> mounted (read-only)");
        Call(nnshim_fs_OpenFile, c, {hptr, Str("other:/data"), 2});
        Check(c.x[0] == nnshim::fs_result::InvalidOperationForOpenMode, "save of another game: write rejected");
        Call(nnshim_fs_Unmount, c, {Str("other")});
    }

    // --- families: fs off -> everything to the original
    nnshim::SetFamilyMode(nnshim::Family::Fs, nnshim::Mode::Off);
    Check(Call(nnshim_fs_MountRom, c, {Str("rom2")}) == PLA_CONTINUE, "family fs=off -> PLA_CONTINUE");
    nnshim::SetFamilyMode(nnshim::Family::Fs, nnshim::Mode::Native);

    // --- hid
    const uint64_t idp = Alloc(4);
    const uint32_t no1 = 0, handheld = 0x20, no2 = 1;
    write_mem(nullptr, idp, &no1, 4);
    Call(nnshim_hid_GetNpadStyleSet, c, {idp});
    Check(c.x[0] == nnshim::hid::StyleFullKey, "GetNpadStyleSet(No1) == FullKey");
    write_mem(nullptr, idp, &handheld, 4);
    Call(nnshim_hid_GetNpadStyleSet, c, {idp});
    Check(c.x[0] == 0, "GetNpadStyleSet(Handheld) == 0 when docked (like the HLE)");
    const uint64_t st = Alloc(sizeof(nnshim::hid::NpadState));
    write_mem(nullptr, idp, &no1, 4);
    Call(nnshim_hid_GetNpadState_FullKey, c, {st, idp});
    Check((Load<nnshim::hid::NpadState>(st).attributes & 1) == 1, "GetNpadState(FullKey, No1): connected");
    write_mem(nullptr, idp, &no2, 4);
    Call(nnshim_hid_GetNpadState_FullKey, c, {st, idp});
    Check(Load<nnshim::hid::NpadState>(st).attributes == 0, "GetNpadState(FullKey, No2): disconnected");
    Check(nnshim::hid::RemoteCommandForTest("press A+ZR 500; stick L 0 32767 500"), "remote command accepted");
    write_mem(nullptr, idp, &no1, 4);
    Call(nnshim_hid_GetNpadState_FullKey, c, {st, idp});
    Check(Load<nnshim::hid::NpadState>(st).buttons == (nnshim::hid::BtnA | nnshim::hid::BtnZR), "remote command: A+ZR pressed");
    Check(nnshim::hid::ButtonFromName("Minus") == nnshim::hid::BtnMinus, "button names (mapping)");
    // --- mouse: config (mod.json + controls file with comments) and pure logic
    {
        namespace h = nnshim::hid;
        const h::Config hc = h::GetConfig();
        Check(hc.mouse.enabled == "off" && hc.mouse.left == "A+B" && hc.mouse.wheel_up == "L",
              "mouse: hid.mouse section of the mod.json (enabled=false -> off, buttons)");
        Check(std::fabs(hc.mouse.sensitivity - 1.75f) < 1e-6f && hc.mouse.invert_y && hc.mouse.release_key == "F2",
              "mouse: controls file (with // comments, legacy controles.json fallback) overrides the mod.json");
        Check(hc.keyboard_map.size() == 2 && hc.keyboard_map.back().first == "ZR" && hc.keyboard_map.back().second == "R",
              "keyboard: keyboard_map of the controls file adds to the mod.json one");
        Check(h::ButtonsFromList("A+ZR") == (h::BtnA | h::BtnZR) && h::ButtonsFromList("ZL, ZR") == (h::BtnZL | h::BtnZR) &&
                  h::ButtonsFromList("") == 0 && h::ButtonsFromList("LStickUp") == 0,
              "mouse: button list (only the 16 digital ones)");
        h::MouseConfig mc;
        mc.smoothing_ms = 0;
        mc.anti_deadzone = 0;
        h::MouseStick ms;
        ms.Reset();
        int32_t x = 1, y = 1;
        ms.Update(mc, 0, 0, 0.004f, x, y);
        Check(x == 0 && y == 0, "mouse still: stick centered");
        ms.Update(mc, 2, 0, 0.004f, x, y);   // 500 px/s
        Check(std::abs(x - 16384) <= 2 && y == 0, "mouse 500 px/s to the right (sens 1) = stick at 50 %");
        ms.Update(mc, 0, 8, 0.004f, x, y);   // 2000 px/s downwards
        Check(x == 0 && y == -32767, "fast mouse downwards = stick down at 100 % (saturates)");
        mc.invert_y = true;
        ms.Update(mc, 0, 8, 0.004f, x, y);
        Check(y == 32767, "mouse: invert_y");
        mc.invert_y = false;
        ms.Update(mc, 3, 3, 0.004f, x, y);   // diagonal
        Check(x > 0 && y < 0 && std::abs(x + y) <= 1, "mouse: diagonal keeps the direction");
        mc.curve = 2;
        ms.Update(mc, 2, 0, 0.004f, x, y);
        Check(std::abs(x - 8192) <= 2, "mouse: curve 2 (50 % -> 25 %)");
        mc.curve = 1;
        mc.anti_deadzone = 0.12f;
        ms.Update(mc, 0.4f, 0, 0.004f, x, y);   // 100 px/s -> 10 % -> 0.12 + 0.88*0.1
        Check(std::abs(x - static_cast<int32_t>(0.208f * 32767)) <= 3, "mouse: anti-deadzone");
        ms.Update(mc, 0.04f, 0, 0.004f, x, y);   // 10 px/s < min_speed
        Check(x == 0, "mouse: below min_speed, centered");
        mc.smoothing_ms = 25;
        ms.Reset();
        ms.Update(mc, 4, 0, 0.004f, x, y);
        const int32_t first = x;
        int32_t at20 = 0, at44 = 0;
        for (int i = 1; i <= 11; ++i) {
            ms.Update(mc, 0, 0, 0.004f, x, y);
            if (i == 5) at20 = x;
            if (i == 11) at44 = x;
        }
        Check(first > 0 && first < 32767 && at20 > 0 && at44 == 0,
              "mouse: smoothing (ramps up) and stop after idle_stop_ms without movement");
        h::WheelPulser wp;
        wp.Reset();
        wp.AddNotches(2);
        int presses = 0;
        uint64_t prev = 0, seen = 0;
        for (int t = 0; t < 400; t += 4) {
            const uint64_t b = wp.Step(4, h::BtnL, h::BtnR, 60);
            if (b && !prev) ++presses;
            seen |= b;
            prev = b;
        }
        wp.AddNotches(-1);
        const uint64_t down = wp.Step(4, h::BtnL, h::BtnR, 60);
        Check(presses == 2 && seen == h::BtnL && down == h::BtnR, "wheel: 2 notches up = 2 presses of L; down = R");
    }
    // ---- mouse in menus (MenuMouse) and menu state
    {
        namespace h = nnshim::hid;
        h::MouseConfig mc;
        h::MenuMouse mm;
        uint64_t b = 0;
        int32_t lx = 0, ly = 0;
        auto step = [&](bool left, bool right, float dx) {
            b = 0;
            lx = ly = 0;
            mm.Update(mc, left, right, false, dx, 0, 4, h::BtnA, h::BtnB, 0, b, lx, ly);
        };
        step(true, false, 0);
        const uint64_t while_down = b;
        step(true, false, 1);
        step(false, false, 0);
        const uint64_t on_release = b;
        bool pulse_ends = false;
        for (int i = 0; i < 40; ++i) {
            step(false, false, 0);
            if (!(b & h::BtnA)) pulse_ends = true;
        }
        Check(while_down == 0 && on_release == h::BtnA && pulse_ends && mm.Clicks() == 1,
              "menu: left click = short A on release");
        step(true, false, 0);
        int32_t maxx = 0;
        for (int i = 0; i < 20; ++i) {
            step(true, false, 4);   // 1000 px/s
            maxx = std::max(maxx, lx);
        }
        step(false, false, 0);
        Check(maxx > 16000 && !(b & h::BtnA) && mm.Clicks() == 1, "menu: drag = left stick, no A");
        {   // sustained 2.4 s drag at ~750 px/s: the stick must not drop to zero midway
            h::MenuMouse md;
            int nz = 0;
            for (int i = 0; i < 600; ++i) {
                uint64_t bb = 0;
                int32_t sx = 0, sy = 0;
                md.Update(mc, true, false, false, 2.5f, 1.67f, 4, h::BtnA, h::BtnB, 0, bb, sx, sy);
                if (sx || sy) ++nz;
            }
            std::printf("     (sustained drag: %d of 600 samples with stick)\n", nz);
            Check(nz > 580, "menu: sustained drag keeps the stick");
        }
        step(false, true, 0);
        Check(b == h::BtnB, "menu: right click = B");
        h::SetMenuState(true);
        const bool on = h::MenuActive();
        h::SetMenuState(false);
        Check(on && !h::MenuActive(), "menu: SetMenuState");
    }
    // ---- key prompts: synthetic pack, BNTX and SARC
    {
        namespace u = nnshim::ui;
        auto put = [](std::vector<uint8_t>& v, size_t off, const void* p, size_t n) {
            if (v.size() < off + n) v.resize(off + n);
            std::memcpy(v.data() + off, p, n);
        };
        auto u16 = [&](std::vector<uint8_t>& v, size_t off, uint16_t x) { put(v, off, &x, 2); };
        auto u32 = [&](std::vector<uint8_t>& v, size_t off, uint32_t x) { put(v, off, &x, 4); };
        auto u64 = [&](std::vector<uint8_t>& v, size_t off, uint64_t x) { put(v, off, &x, 8); };
        const uint64_t surf = u::SurfaceSize(0x1D01, 8, 8, 0);
        std::vector<uint8_t> bntx(0x200 + surf, 0x11);
        put(bntx, 0, "BNTX", 4);
        u32(bntx, 0x24, 1);
        u64(bntx, 0x28, 0x40);
        u64(bntx, 0x40, 0x50);
        put(bntx, 0x50, "BRTI", 4);
        u16(bntx, 0x50 + 0x16, 1);
        u32(bntx, 0x50 + 0x1C, 0x1D01);
        u32(bntx, 0x50 + 0x24, 8);
        u32(bntx, 0x50 + 0x28, 8);
        u32(bntx, 0x50 + 0x34, 0);
        u64(bntx, 0x50 + 0x60, 0x100);
        u64(bntx, 0x50 + 0x70, 0x120);
        u16(bntx, 0x100, 4);
        put(bntx, 0x102, "icon", 4);
        u64(bntx, 0x120, 0x200);
        std::vector<uint8_t> pk;
        put(pk, 0, "PLATECL1", 8);
        u32(pk, 8, 1);
        u16(pk, 12, 4);
        put(pk, 14, "icon", 4);
        u32(pk, 18, 0x1D01);
        u32(pk, 22, 8);
        u32(pk, 26, 8);
        u32(pk, 30, static_cast<uint32_t>(surf));
        pk.resize(34 + surf, 0xAB);
        u::Pack pack;
        std::string err;
        const bool loaded = pack.Load(pk, &err);
        std::vector<uint8_t> b1 = bntx;
        const int n = u::PatchBntx(pack, b1.data(), b1.size());
        Check(surf == 512 && loaded && n == 1 && b1[0x200] == 0xAB && b1[0x200 + surf - 1] == 0xAB && b1[0x1FF] == 0x11,
              "prompts: pack and replacement in a BNTX");
        std::vector<uint8_t> sarc(0x40, 0);
        put(sarc, 0, "SARC", 4);
        u16(sarc, 4, 0x14);
        u32(sarc, 0x0C, 0x40);
        put(sarc, 0x14, "SFAT", 4);
        u16(sarc, 0x18, 0x0C);
        u16(sarc, 0x1A, 1);
        u32(sarc, 0x20 + 8, 0);
        u32(sarc, 0x20 + 12, static_cast<uint32_t>(bntx.size()));
        put(sarc, 0x30, "SFNT", 4);
        sarc.insert(sarc.end(), bntx.begin(), bntx.end());
        const auto patches = u::SarcPatches(pack, sarc);
        std::vector<uint8_t> chunk(0x100, 0x11);   // chunked read that cuts through the icon
        const uint64_t copied = u::ApplyPatches(patches, 0x40 + 0x180, chunk.data(), chunk.size());
        Check(patches.size() == 1 && patches[0].file_off == 0x240 && copied == 0x80 && chunk[0x7F] == 0x11 &&
                  chunk[0x80] == 0xAB && chunk[0xFF] == 0xAB,
              "prompts: SARC patches applied to a partial read");
        Check(u::PromptModeFromString("Keyboard") == u::PromptMode::Keyboard &&
                  u::PromptModeFromString("controller") == u::PromptMode::Controller &&
                  u::KeyboardPrompts(u::PromptMode::Auto, false) && !u::KeyboardPrompts(u::PromptMode::Auto, true),
              "prompts: auto mode = keyboard without a controller");
        // Live swap: two "loaded" textures; one is reused for something else and gets forgotten.
        std::vector<uint8_t> mem(64, 0x11), kbv(16, 0xAB);
        u::LiveSet ls;
        ls.Add(0x1000, mem.data(), kbv.data(), 16);
        ls.Add(0x1020, mem.data() + 32, kbv.data(), 16);
        auto host = [&](uint64_t va, size_t) { return mem.data() + (va - 0x1000); };
        const int a = ls.Swap(true, host);
        mem[0x20] = 0x55;   // the second area is something else now
        const int b = ls.Swap(false, host);
        Check(a == 2 && b == 1 && mem[0] == 0x11 && mem[15] == 0x11 && mem[0x20] == 0x55 && mem[0x21] == 0xAB &&
                  ls.Size() == 1,
              "prompts: live swap only on memory that still holds the icon");
    }
    {
        const uint64_t hs = Alloc(16);
        const uint32_t fullkey = nnshim::hid::StyleFullKey;
        write_mem(nullptr, idp, &no1, 4);
        Call(nnshim_hid_GetSixAxisSensorHandles, c, {hs, 2, idp, fullkey});
        const int n = static_cast<int>(c.x[0]);
        const uint64_t ss = Alloc(4 * sizeof(nnshim::hid::SixAxisState));
        Call(nnshim_hid_GetSixAxisSensorStates, c, {ss, 4, hs});
        const auto s0 = Load<nnshim::hid::SixAxisState>(ss);
        Check(n == 1 && c.x[0] >= 1 && (s0.attributes & 1) && std::fabs(s0.accel[2] + 1) < 1e-3f && s0.direction[0][0] > 0.99f,
              "motion: 1 FullKey handle, idle state (acc z = -1, identity)");
        const uint64_t vh = Alloc(16);
        Call(nnshim_hid_GetVibrationDeviceHandles, c, {vh, 2, idp, fullkey});
        Check(c.x[0] == 2, "rumble: 2 actuators on FullKey");
        const float vv[4] = {0.5f, 160, 0.25f, 320};
        const uint64_t vp = Alloc(16);
        write_mem(nullptr, vp, vv, 16);
        const uint64_t before = nnshim::hid::GetInfo().rumble_commands;
        Call(nnshim_hid_EnqueueVibrationValue, c, {vh, vp});
        Check(c.x[0] == 0 && nnshim::hid::GetInfo().rumble_commands == before + 1, "rumble: EnqueueVibrationValue handled");
    }

    // --- time (fixed zone +02:00)
    {
        const uint64_t tp = Alloc(8);
        Call(nnshim_time_UserGetCurrentTime, c, {tp});
        const int64_t now = Load<int64_t>(tp);
        Check(c.x[0] == 0 && now > 1700000000, "time: GetCurrentTime ~ host clock");
        StoreG<int64_t>(tp, 1000000000);   // 2001-09-09 01:46:40 UTC (Sunday)
        Call(nnshim_time_ToCalendarTimeInUtc, c, {tp});
        nnshim::time::CalendarTime cal{};
        std::memcpy(&cal, &c.x[0], 8);
        Check(cal.year == 2001 && cal.month == 9 && cal.day == 9 && cal.hour == 1 && cal.minute == 46 && cal.second == 40,
              "time: ToCalendarTimeInUtc(1e9) = 2001-09-09 01:46:40");
        const uint64_t cp = Alloc(8), ip = Alloc(0x18);
        Call(nnshim_time_ToCalendarTime, c, {cp, ip, tp});
        const auto lc = Load<nnshim::time::CalendarTime>(cp);
        const auto li = Load<nnshim::time::CalendarAdditionalInfo>(ip);
        Check(lc.hour == 3 && li.utc_offset == 7200 && li.day_of_week == 0 && li.day_of_year == 251,
              "time: ToCalendarTime with +02:00 (03:46, Sunday, day 251)");
        Check(std::strncmp(li.tz_name, "+02", 8) == 0, "time: tzdata-style zone abbreviation (+02)");
        const uint64_t rule = Alloc(0x4000), loc = Str("Europe/Madrid");
        Call(nnshim_time_LoadTimeZoneRule, c, {rule, loc});
        const uint64_t cntp = Alloc(4), outt = Alloc(16);
        Call(nnshim_time_ToPosixTime, c, {cntp, outt, 2, cp, rule});
        Check(c.x[0] == 0 && Load<int32_t>(cntp) == 1 && Load<int64_t>(outt) == 1000000000, "time: ToPosixTime(ToCalendarTime(t)) == t");
        // PLA loads "GMT" for ToPosixTime: calendar time read as UTC, whatever the device zone is.
        const uint64_t grule = Alloc(0x4000), utccal = Alloc(8);
        Call(nnshim_time_LoadTimeZoneRule, c, {grule, Str("GMT")});
        StoreG(utccal, nnshim::time::CalendarTime{2001, 9, 9, 1, 46, 40, 0});
        Call(nnshim_time_ToPosixTime, c, {cntp, outt, 2, utccal, grule});
        const uint64_t erule = Alloc(0x4000);
        Call(nnshim_time_LoadTimeZoneRule, c, {erule, Str("Etc/GMT+4")});
        const uint64_t outt2 = Alloc(16);
        Call(nnshim_time_ToPosixTime, c, {cntp, outt2, 2, utccal, erule});
        Check(Load<int64_t>(outt) == 1000000000 && Load<int64_t>(outt2) == 1000000000 + 4 * 3600,
              "time: fixed rules GMT and Etc/GMT+4 (= UTC-4)");
        const uint64_t snap = Alloc(0xD0);
        Call(nnshim_time_CreateWithStandardSystemClock, c, {snap});
        const auto s = Load<nnshim::time::ClockSnapshot>(snap);
        Check(c.x[0] == 0 && s.user_time > 1700000000 && s.user_calendar.year >= 2026 && s.user_info.utc_offset == 7200,
              "time: ClockSnapshot::CreateWithStandardSystemClock");
        Call(nnshim_time_Initialize, c, {});
        Check(c.x[0] == 0, "time: Initialize");
        Call(nnshim_time_IsInitialized, c, {});
        Check(c.x[0] == 1, "time: IsInitialized");
    }

    // --- settings / oe
    {
        Call(nnshim_settings_LanguageCodeMake, c, {1});
        Check(nnshim::settings::UnpackCode(c.x[0]) == "en-US", "settings: LanguageCode::Make(AmericanEnglish) = en-US");
        const uint64_t lang = Alloc(4), code = Alloc(8);
        StoreG<int32_t>(lang, 5);
        write_mem(nullptr, code, "es\0\0\0\0\0", 8);
        Call(nnshim_settings_LanguageEqCode, c, {lang, code});
        Check(c.x[0] == 1, "settings: Language(Spanish) == \"es\"");
        Call(nnshim_oe_GetDesiredLanguage, c, {});
        Check(nnshim::settings::UnpackCode(c.x[0]) == "es", "oe: desired language es-419 -> es (game languages)");
        Call(nnshim_oe_GetOperationMode, c, {});
        Check(c.x[0] == 1, "oe: docked mode");
        const uint64_t msg = Alloc(4);
        Call(nnshim_oe_TryPopNotificationMessage, c, {msg});
        Check(c.x[0] == 0, "oe: no pending messages");
    }

    // --- account
    {
        const uint64_t uh = Alloc(0x18), uidp = Alloc(16), nick = Alloc(0x21), b = Alloc(1);
        Call(nnshim_account_TryOpenPreselectedUser, c, {uh});
        Check(c.x[0] == 1, "account: TryOpenPreselectedUser");
        Call(nnshim_account_GetUserId, c, {uidp, uh});
        Check(c.x[0] == 0 && Load<uint64_t>(uidp) == 0x3ECA2F644C7C2144ull, "account: GetUserId = suyu profile Uid");
        Call(nnshim_account_GetNickname, c, {nick, uidp});
        char nk[0x21] = {};
        read_mem(nullptr, nick, nk, 0x21);
        Check(std::strcmp(nk, "Tester") == 0, "account: GetNickname from config");
        Call(nnshim_account_IsNetworkServiceAccountAvailable, c, {b, uh});
        Check(c.x[0] == 0 && Load<uint8_t>(b) == 0, "account: network account unavailable");
        const uint64_t ctxa = Alloc(0x20);
        Call(nnshim_account_EnsureNetworkServiceAccountIdTokenCacheAsync, c, {ctxa, uh});
        Check(c.x[0] == ((59u << 9) | 124), "account: async task without network -> 2124-0059");
        Call(nnshim_account_AsyncContextHasDone, c, {ctxa, b});
        Check(c.x[0] == 0 && Load<uint8_t>(b) == 1, "account: empty AsyncContext -> HasDone");
        const uint64_t foreign = Alloc(0x18);
        Check(Call(nnshim_account_GetUserId, c, {uidp, foreign}) == PLA_CONTINUE, "account: foreign UserHandle -> PLA_CONTINUE");
    }

    // --- err (no dialog)
    {
        // Fake stack: two chained frames (X29 -> [previous fp, lr]).
        const uint64_t f1 = kBase + 0x3F8000, f2 = kBase + 0x3F8100;
        StoreG<uint64_t>(f1, f2);
        StoreG<uint64_t>(f1 + 8, kBase + 0x1234);
        StoreG<uint64_t>(f2, 0);
        StoreG<uint64_t>(f2 + 8, kBase + 0x5678);
        c.x[29] = f1;
        c.x[30] = kBase + 0x100;
        const uint64_t btp = Alloc(8 * 8);
        Call(nnshim_diag_GetBacktrace, c, {btp, 8});
        Check(c.x[0] == 3 && Load<uint64_t>(btp + 8) == kBase + 0x1234 && Load<uint64_t>(btp + 16) == kBase + 0x5678,
              "err: GetBacktrace follows the X29 chain");
        const uint64_t res = Alloc(4);
        StoreG<uint32_t>(res, (2u << 9) | 2);
        g_quiet = true;
        c.x[6] = 42;
        c.x[7] = Str("file.bin");
        double d = 1.5;
        std::memcpy(&c.v[0][0], &d, 8);
        const pla_action r = Call(nnshim_diag_AbortImplFormat, c,
                                  {Str("x != 0"), Str("Foo::Bar"), Str("foo.cpp"), 77, res, Str("n=%d f=%s v=%.1f")});
        g_quiet = false;
        bool dumped = false;
        std::string dump;
        for (const auto& e : stdfs::directory_iterator(tmp / "dumps")) { dumped = true; dump = ReadFileText(e.path()); }
        Check(r == PLA_CONTINUE && dumped && dump.find("n=42 f=file.bin v=1.5") != std::string::npos &&
                  dump.find("Foo::Bar") != std::string::npos && dump.find("2002-0002") != std::string::npos,
              "err: AbortImpl formats the guest printf and writes a dump");
        const uint64_t arg = Alloc(0x1014);
        Call(nnshim_err_ApplicationErrorArgCtor, c, {arg});
        Call(nnshim_err_SetDialogMessage, c, {arg, Str("test")});
        char m[5] = {};
        read_mem(nullptr, arg + 0x14, m, 5);
        Check(Load<uint16_t>(arg) == 0x102 && std::strcmp(m, "test") == 0, "err: ApplicationErrorArg");
    }

    // --- swkbd (auto mode)
    {
        const uint64_t arg = Alloc(0x500), sbuf = Alloc(0x7D4), str = Alloc(16);
        write_mem(nullptr, arg + 0x24, u"Player", 14);
        StoreG<uint32_t>(arg + 0x3AC, 10);
        const uint64_t sv[2] = {sbuf, 0x7D4};
        write_mem(nullptr, str, sv, 16);
        g_quiet = true;
        Call(nnshim_swkbd_ShowKeyboard, c, {str, arg});
        g_quiet = false;
        char16_t outs[4] = {};
        read_mem(nullptr, sbuf, outs, 8);
        Check(c.x[0] == 0 && outs[0] == u'A' && outs[1] == u'r' && outs[2] == u'i' && outs[3] == 0, "swkbd: auto mode -> \"Ari\"");
        Call(nnshim_swkbd_GetRequiredWorkBufferSizeInt, c, {0});
        Check(c.x[0] == 0x1000, "swkbd: GetRequiredWorkBufferSize(0)");
    }

    // --- system events with the (fake) pla_api 1.1 extension, and shared fonts
    {
        static uint64_t ev_created = 0, ev_signaled = 0, ev_destroyed = 0;
        kApi.size = static_cast<uint32_t>(sizeof(pla_api));
        kApi.event_create = [](const char*) -> uint64_t { return ++ev_created; };
        kApi.event_guest_handle = [](uint64_t e) -> uint32_t { return 0x100u + static_cast<uint32_t>(e); };
        kApi.event_from_guest_handle = [](uint32_t) -> uint64_t { return 0; };
        kApi.event_signal = [](uint64_t) -> int { ++ev_signaled; return 0; };
        kApi.event_clear = [](uint64_t) -> int { return 0; };
        kApi.event_wait = [](uint64_t, int64_t) -> int { return 1; };
        kApi.event_destroy = [](uint64_t) { ++ev_destroyed; };
        const uint64_t npad = Alloc(4), ev = Alloc(0x30);
        StoreG<uint32_t>(npad, 0);
        StoreG<uint8_t>(ev + 0x1C, 0xEE);
        const pla_action r = Call(nnshim_hid_BindNpadStyleSetUpdateEvent, c, {npad, ev, 1});
        Check(r == PLA_SKIP && Load<uint64_t>(ev) == ev && Load<uint64_t>(ev + 8) == ev && Load<uint8_t>(ev + 0x10) == 1 &&
                  Load<uint8_t>(ev + 0x11) == 1 && Load<uint8_t>(ev + 0x12) == 1 && Load<uint8_t>(ev + 0x13) == 0 &&
                  Load<uint32_t>(ev + 0x14) == 0x101 && Load<uint32_t>(ev + 0x18) == 0 && Load<uint8_t>(ev + 0x28) == 2 &&
                  ev_signaled == 1,
              "hid: BindNpadStyleSetUpdateEvent -> SystemEventType like the SDK (handle 0x101, AutoClear, signaled)");
        const uint64_t actx = Alloc(0x20), ev2 = Alloc(0x30);
        c.x[0] = 0xDEAD;
        Check(Call(nnshim_account_AsyncContextGetSystemEvent, c, {actx, ev2}) == PLA_SKIP && c.x[0] == 0 &&
                  Load<uint32_t>(ev2 + 0x14) == 0x102 && Load<uint8_t>(ev2 + 0x10) == 0 && Load<uint8_t>(ev2 + 0x28) == 2 &&
                  ev_signaled == 2,
              "account: AsyncContext::GetSystemEvent (empty context) -> signaled event, ManualClear");
        StoreG<uint64_t>(actx + 0x18, 0x1234);
        Check(Call(nnshim_account_AsyncContextGetSystemEvent, c, {actx, ev2}) == PLA_CONTINUE,
              "account: GetSystemEvent with an HLE object -> to the SDK");
        Check(Call(nnshim_pl_GetSharedFontAddress, c, {0}) == PLA_CONTINUE, "pl: GetSharedFontAddress without font.dir -> HLE");
        // Font image: plain .ttf -> header {0x18029a7f, size ^ 0x49621806} + data (multiple of 4).
        const stdfs::path fdir = tmp / "fonts";
        WriteText(fdir / "nintendo_udsg-r_std_003.ttf", "0123456789");
        WriteText(fdir / "nintendo_ext_003.ttf", "abcd");
        std::vector<uint8_t> img;
        std::vector<nnshim::font::Region> reg;
        const bool ok = nnshim::font::BuildSharedFontImage(fdir.u8string(), img, reg);
        uint32_t w0 = 0, w1 = 0, w2 = 0;
        if (img.size() >= 28) { std::memcpy(&w0, &img[0], 4); std::memcpy(&w1, &img[4], 4); std::memcpy(&w2, &img[16], 4); }
        Check(ok && reg.size() == 2 && reg[0].offset == 8 && reg[0].size == 8 && reg[1].offset == 24 && reg[1].size == 4 &&
                  img.size() == 28 && w0 == 0x18029a7f && w1 == (8u ^ 0x49621806u) && std::memcmp(&img[8], "01234567", 8) == 0 &&
                  w2 == 0x18029a7f && std::memcmp(&img[24], "abcd", 4) == 0,
              "pl: font image like the HLE (regions, header, plain data, missing fonts skipped)");
        // The same .ttf as an encrypted .bfttf (EncryptSharedFont) gives the same image.
        std::vector<uint8_t> enc;
        auto put = [&](uint32_t v) { for (int i = 0; i < 4; ++i) enc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
        auto sw = [](uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); };
        const uint32_t key = sw(0x7f9a0218u ^ 0x36f81a1eu);
        put(sw(0x36f81a1eu));
        put(sw(8u) ^ key);
        for (int i = 0; i < 2; ++i) { uint32_t w; std::memcpy(&w, "01234567" + 4 * i, 4); put(w ^ key); }
        stdfs::remove(fdir / "nintendo_udsg-r_std_003.ttf");
        if (FILE* f = _wfopen((fdir / "nintendo_udsg-r_std_003.bfttf").c_str(), L"wb")) { std::fwrite(enc.data(), 1, enc.size(), f); std::fclose(f); }
        std::vector<uint8_t> img2;
        nnshim::font::BuildSharedFontImage(fdir.u8string(), img2, reg);
        Check(img2 == img, "pl: encrypted .bfttf -> same image as the .ttf");
    }

    // --- guest printf format
    {
        struct Args final : nnshim::diag::ArgSource {
            uint64_t NextGp() override { return gp[i++]; }
            double NextFp() override { return 2.25; }
            std::string Str(uint64_t) override { return "s"; }
            uint64_t gp[4] = {static_cast<uint64_t>(-5), 0xABC, 0, 0};
            int i = 0;
        } a;
        Check(nnshim::diag::Format("%d %04x %5.2f %%", a) == "-5 0abc  2.25 %", "diag: Format (%d %04x %5.2f %%)");
    }

    // --- offline network/shop (stage 5)
    {
        pla_ctx c{};
        c.x[0] = 7;
        const bool a = Call(nnshim_aoc_CountAddOnContent, c, {}) == PLA_SKIP && c.x[0] == 0;
        c.x[0] = 1;
        const bool n = Call(nnshim_nifm_IsNetworkAvailable, c, {}) == PLA_SKIP && c.x[0] == 0;
        const uint64_t cnt = Alloc(4);
        StoreG<int32_t>(cnt, 5);
        const bool f = Call(nnshim_friends_GetFriendList, c, {cnt, 0, 0, 10, 0}) == PLA_SKIP && c.x[0] == 0 && Load<int32_t>(cnt) == 0;
        Check(a && n && f, "net: aoc 0, nifm offline, friends 0 friends");
    }

    // --- boot services (stage 5)
    {
        pla_ctx c{};
        const uint64_t buf = Alloc(64);
        std::vector<uint8_t> zero(64, 0), got(64);
        write_mem(nullptr, buf, zero.data(), 64);
        const bool r = Call(nnshim_os_GenerateRandomBytes, c, {buf, 64}) == PLA_SKIP;
        read_mem(nullptr, buf, got.data(), 64);
        const uint64_t cnt = Alloc(4), uids = Alloc(32);
        // No account::Initialize by the HLE (not called in this test): the shim answers with its user.
        const bool l = Call(nnshim_account_ListOpenUsers, c, {cnt, uids, 4}) == PLA_SKIP && Load<int32_t>(cnt) == 1;
        Check(r && got != zero && l, "boot: host GenerateRandomBytes and native ListOpenUsers");
    }

    // --- A/B validation of services (observe mode + post): the test simulates the "HLE"
    {
        using nnshim::CounterGet;
        pla_ctx c{};
        nnshim::SetFamilyMode(nnshim::Family::Time, nnshim::Mode::Observe);
        nnshim::SetFamilyMode(nnshim::Family::Applet, nnshim::Mode::Observe);
        nnshim::SetFamilyMode(nnshim::Family::Account, nnshim::Mode::Observe);
        const uint64_t tp = Alloc(8);
        StoreG<int64_t>(tp, 1000000000);
        const bool passed = Call(nnshim_time_ToCalendarTimeInUtc, c, {tp}) == PLA_CONTINUE;
        nnshim::time::CalendarTime cal{2001, 9, 9, 1, 46, 40, 0};
        uint64_t packed = 0;
        std::memcpy(&packed, &cal, 8);
        c.x[0] = packed;   // what the HLE would return
        nnshim_time_ToCalendarTimeInUtc_post(&c);
        cal.minute = 47;
        std::memcpy(&packed, &cal, 8);
        Call(nnshim_time_ToCalendarTimeInUtc, c, {tp});
        c.x[0] = packed;
        nnshim_time_ToCalendarTimeInUtc_post(&c);
        Check(passed && CounterGet("val.time.ToCalendarTimeInUtc.ok") == 1 && CounterGet("val.time.ToCalendarTimeInUtc.diff") == 1,
              "A/B: ToCalendarTimeInUtc (observe: to the HLE; post: equal and different)");
        const uint64_t now = Alloc(8);
        Call(nnshim_time_UserGetCurrentTime, c, {now});
        StoreG<int64_t>(now, nnshim::time::Now() + 1);
        c.x[0] = 0;
        nnshim_time_UserGetCurrentTime_post(&c);
        Check(CounterGet("val.time.UserGetCurrentTime.ok") == 1, "A/B: GetCurrentTime 1 s apart = equal");
        c.x[0] = 1;   // docked (default config)
        nnshim_oe_GetOperationMode_post(&c);
        c.x[0] = 0;
        nnshim_oe_GetOperationMode_post(&c);
        Check(CounterGet("val.oe.GetOperationMode.ok") == 1 && CounterGet("val.oe.GetOperationMode.diff") == 1,
              "A/B: GetOperationMode");
        const uint64_t uidp = Alloc(16), handle = Alloc(0x18);
        Call(nnshim_account_GetUserId, c, {uidp, handle});
        const uint64_t uid[2] = {0x3ECA2F644C7C2144ull, 0x155E1F13CFBBD8C8ull};
        write_mem(nullptr, uidp, uid, 16);
        c.x[0] = 0;
        nnshim_account_GetUserId_post(&c);
        Check(CounterGet("val.account.GetUserId.ok") == 1, "A/B: GetUserId (uid of the default config)");
        nnshim::SetFamilyMode(nnshim::Family::Time, nnshim::Mode::Native);
        nnshim::SetFamilyMode(nnshim::Family::Applet, nnshim::Mode::Native);
        nnshim::SetFamilyMode(nnshim::Family::Account, nnshim::Mode::Native);
    }

    // --- replacement table and statistics
    size_t n = 0;
    const nnshim_entry* t = nnshim_get_table(&n);
    size_t sdk = 0, plt = 0, internal = 0;
    for (size_t i = 0; i < n; ++i) {
        (t[i].hook_sdk ? sdk : plt)++;
        if (!std::strcmp(t[i].mangled, "_ZN2nn3hid6detail21EnqueueVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE"))
            internal = t[i].sdk_offset;
    }
    std::printf("table: %zu entries (%zu in sdk, %zu in the main PLT)\n", n, sdk, plt);
    Check(n >= 120 && internal == 0x220500, "table: >= 120 entries, including the SDK internal ones");
    const std::string stats = nnshim::StatsJson();
    Check(stats.find("\"nnshim_fs_OpenFile\"") != std::string::npos && stats.find("fs.commit.ok") != std::string::npos,
          "stats: per-adapter and free counters");

    pla_mod_shutdown();
    Check(ReadFileText(tmp / "trace.txt").find("open rom:/bin/hello.txt m=1 r=0") != std::string::npos, "trace: OpenFile recorded");
    Check(ReadFileText(tmp / "stats.json").find("\"adapters\"") != std::string::npos, "stats_path written on shutdown");
    // --- RomFS from romfs.bin (core, no adapters: the shim is already shut down)
    {
        namespace nfs = nnshim::fs;
        namespace r = nnshim::fs_result;
        const std::vector<uint8_t> img = BuildRomfs({{"bin/hello.txt", "test romfs"}, {"bin/sub/B.dat", "xyz123"},
                                                     {"top.txt", "top"}, {"bin/Zeta.txt", ""}});
        {
            FILE* f = _wfopen((tmp / "romfs.bin").c_str(), L"wb");
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);
        }
        nnshim::romfs::Image im;
        std::string err;
        Check(im.Open((tmp / "romfs.bin").u8string(), &err) && im.FileCount() == 4 && im.DirCount() == 3,
              "romfs.bin: 4 files and 3 directories");
        nnshim::romfs::FileInfo fi;
        Check(im.Lookup("BIN\\Sub\\b.DAT", &fi) == nnshim::romfs::Kind::File && fi.size == 6 &&
                  im.Lookup("/bin/sub/", nullptr) == nnshim::romfs::Kind::Dir &&
                  im.Lookup("bin/../top.txt") == nnshim::romfs::Kind::None,
              "romfs.bin: case-insensitive lookup, mixed separators, '..' rejected");
        im.Close();

        nfs::Config fc;
        fc.romfs_bin = (tmp / "romfs.bin").u8string();
        nfs::Initialize(fc);
        Check(nfs::RomfsConfigured() && nfs::MountRom("data").ours, "romfs.bin: MountRom");
        Check(nfs::RomSourceDescription().rfind("romfs.bin ", 0) == 0, "romfs.bin: source in the log");
        uint64_t fh = 0;
        Check(nfs::OpenFile(fh, "data:/bin/HELLO.TXT", nfs::OpenRead).result == 0, "romfs.bin: case-insensitive OpenFile");
        int64_t sz = 0;
        nfs::GetFileSize(fh, sz);
        char b[64] = {};
        uint64_t n = 0;
        Check(sz == 10 && nfs::ReadFile(fh, 5, b, 64, n).result == 0 && n == 5 && std::memcmp(b, "romfs", 5) == 0,
              "romfs.bin: GetFileSize and partial read at an offset");
        Check(nfs::ReadFile(fh, 10, b, 4, n).result == 0 && n == 0 && nfs::ReadFile(fh, 11, b, 4, n).result == r::OutOfRange,
              "romfs.bin: read at the end (0 bytes) and past the end (OutOfRange)");
        std::vector<uint8_t> whole;
        Check(nfs::RomFileId(fh) == "romfs.bin:/bin/HELLO.TXT" && nfs::ReadRomFileById(nfs::RomFileId(fh), whole, 1 << 20) &&
                  std::string(whole.begin(), whole.end()) == "test romfs",
              "romfs.bin: RomFileId + ReadRomFileById (for ui)");
        uint64_t wh = 0;
        Check(nfs::OpenFile(wh, "data:/bin/hello.txt", nfs::OpenWrite).result == r::InvalidOperationForOpenMode &&
                  nfs::CreateFile("data:/x", 1).result == r::InvalidOperationForOpenMode,
              "romfs.bin: read-only");
        nfs::EntryType et{};
        Check(nfs::GetEntryType("data:/bin/sub/", et).result == 0 && et == nfs::EntryDirectory &&
                  nfs::GetEntryType("data:/top.txt", et).result == 0 && et == nfs::EntryFile &&
                  nfs::GetEntryType("data:/nothing", et).result == r::PathNotFound &&
                  nfs::GetEntryType("data:/../x", et).result == r::InvalidPath,
              "romfs.bin: GetEntryType (dir, file, missing, '..')");
        uint64_t dh = 0;
        std::vector<nfs::DirectoryEntry> ents;
        nfs::OpenDirectory(dh, "data:/bin", nfs::DirDirectories | nfs::DirFiles);
        nfs::ReadDirectory(dh, ents, 16);
        nfs::CloseDirectory(dh);
        Check(ents.size() == 3 && std::strcmp(ents[0].name, "hello.txt") == 0 && ents[0].size == 10 &&
                  std::strcmp(ents[1].name, "sub") == 0 && ents[1].type == nfs::EntryDirectory &&
                  std::strcmp(ents[2].name, "Zeta.txt") == 0 && ents[2].size == 0,
              "romfs.bin: listing (NTFS order, sizes, types)");
        nfs::OpenDirectory(dh, "data:/", nfs::DirFiles);
        nfs::ReadDirectory(dh, ents, 16);
        nfs::CloseDirectory(dh);
        Check(ents.size() == 1 && std::strcmp(ents[0].name, "top.txt") == 0, "romfs.bin: root listing with files only");
        Check(nfs::ReadRomfsFile("/bin/sub/B.dat", 2, b, 10, n) && n == 4 && std::memcmp(b, "z123", 4) == 0,
              "romfs.bin: ReadRomfsFile (A/B validation)");
        nfs::Unmount("data");
        Check(nfs::ReadFile(fh, 0, b, 4, n).result == r::InvalidHandle, "romfs.bin: Unmount closes the handles");
        nfs::Finalize();

        // overlays (mods): replace and add files; the listing merges them
        WriteText(tmp / "mods1" / "bin" / "hello.txt", "mod!");
        WriteText(tmp / "mods1" / "bin" / "new.txt", "new");
        fc.romfs_overlays = {(tmp / "mods1").u8string()};
        nfs::Initialize(fc);
        nfs::MountRom("data");
        Check(nfs::OpenFile(fh, "data:/bin/hello.txt", nfs::OpenRead).result == 0 && nfs::ReadFile(fh, 0, b, 64, n).result == 0 &&
                  n == 4 && std::memcmp(b, "mod!", 4) == 0 && nfs::RomFileId(fh).find("mods1") != std::string::npos,
              "overlay: the mod file replaces the romfs.bin one");
        nfs::OpenDirectory(dh, "data:/bin", nfs::DirDirectories | nfs::DirFiles);
        nfs::ReadDirectory(dh, ents, 16);
        nfs::CloseDirectory(dh);
        bool hello4 = false, added = false;
        for (const auto& e : ents) {
            hello4 |= std::strcmp(e.name, "hello.txt") == 0 && e.size == 4;
            added |= std::strcmp(e.name, "new.txt") == 0;
        }
        Check(ents.size() == 4 && hello4 && added, "overlay: merged listing (replaced + added)");
        Check(nfs::OpenFile(fh, "data:/top.txt", nfs::OpenRead).result == 0, "overlay: everything else still comes from romfs.bin");
        nfs::Finalize();

        // the folder wins if it exists; invalid image -> HLE
        fc.romfs_overlays.clear();
        fc.romfs_dir = (tmp / "romfs").u8string();
        nfs::Initialize(fc);
        Check(nfs::RomSourceDescription().rfind("folder ", 0) == 0, "romfs_dir takes priority over romfs_bin");
        nfs::Finalize();
        fc.romfs_dir.clear();
        fc.romfs_bin = (tmp / "controles.json").u8string();
        nfs::Initialize(fc);
        Check(!nfs::RomfsConfigured() && !nfs::MountRom("data").ours &&
                  nfs::RomSourceDescription().find("[romfs.bin") != std::string::npos,
              "invalid romfs.bin: MountRom stays in the HLE and the error is logged");
        nfs::Finalize();
    }

    stdfs::remove_all(tmp);
    std::printf("%s (%d passed, %d failed)\n", g_fail ? "FAILED" : "OK", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
