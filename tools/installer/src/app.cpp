// Setup wizard (see app.h). Five steps shown as tabs, like the in-game menu:
// Welcome → Your game (dump and keys) → Check (disk, RAM, your files, included components) → Install → Done.
// Everything except the user's dump and keys ships inside this .exe (components.h). The phase logic is
// tools/launcher/launcher.py from the bundled scripts: it runs with --eventos and the UI draws what it reports.
// SPDX-License-Identifier: MIT
#include "app.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "components.h"
#include "engine.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui.h"

namespace setup {

using namespace ui;
using Microsoft::WRL::ComPtr;

namespace {

enum Step { kWelcome = 0, kGame, kCheck, kInstall, kDone, kSteps };
const char* const kStepName[kSteps] = {"Welcome", "Your game", "Check", "Install", "Done"};
enum InstState { kIdle, kRunning, kStopped, kFailed, kFinished };
const char* const kTitleId = "01001F5010DFA000";  // the profile's title (the setup never names the game)

// English names and descriptions of launcher.py phases (its own texts are not shown).
struct PhaseText {
    const char* title;
    const char* note;
};
const std::map<std::string, PhaseText> kPhaseText = {
    {"_components", {"Prepare setup files", "Unpacks the included platform and compiler (or downloads them if this is a light setup)."}},
    {"prepare", {"Prepare your dump", "Links your files and decompresses the update if it is NSZ/XCZ."}},
    {"extract", {"Extract and verify the program", "Reads the program files of update 1.1.1 and checks their hashes and build ids."}},
    {"roots", {"Analyze the program", "Finds extra entry points for the recompiler (no external tools)."}},
    {"bake", {"Apply the built-in patches", "Unlocked frame rate patches, written into the program the exporter reads."}},
    {"base", {"Decompress the base game", "Only for XCZ dumps (NSZ is read directly): writes the uncompressed base game to the work folder."}},
    {"suyu", {"Prepare the exporter", "Only with the fallback GUI exporter: an isolated copy of it with your keys and the update."}},
    {"export", {"Export the recompiled code", "Translates the program to C (about a minute, no window) and writes the game data with the update applied."}},
    {"compile", {"Compile the modules", "The long step. You can keep using the PC, it will just be slower. If you stop, resuming only compiles what is missing."}},
    {"assemble", {"Assemble the game folder", "Copies the compiled modules, the platform and the game data into the game folder."}},
    {"verify", {"Verify", "Checks the game folder and writes its manifest."}},
};

std::string PhaseTitle(const std::string& id) {
    auto it = kPhaseText.find(id);
    return it != kPhaseText.end() ? it->second.title : id;
}
std::string PhaseNote(const std::string& id) {
    auto it = kPhaseText.find(id);
    return it != kPhaseText.end() ? it->second.note : "";
}

struct Desc {
    std::string title, text;
};

struct State {
    HWND hwnd = nullptr;
    Options op;
    Settings cfg;
    Step step = kWelcome;
    int max_step = 0;
    double t_change = 0;
    bool exit = false;
    uint64_t ram = 0;

    // runtime: bundled components (default) or a developer checkout
    Manifest man;
    bool bundle = false;
    std::wstring comp_dir, cache_dir;
    std::string url_base;
    Preparer prep;
    int prep_goal = 0;  // 1: what identification needs, 2: what the install needs
    std::atomic<bool> dev_ready{false};
    std::thread dev_thread;
    Python dev_py;
    std::wstring dev_repo;

    // paths (UTF-8 for ImGui)
    char dump[1024] = {}, keys[1024] = {}, tkeys[1024] = {}, dest[1024] = {}, work[1024] = {};
    KeysFound found;

    // check (launcher.py --dry-run --eventos)
    std::unique_ptr<Run> chk;
    double chk_at = -1;
    bool chk_done = false;
    std::string chk_err;
    IdentUi ident;
    std::vector<ReqUi> reqs;
    PlanUi plan;
    std::deque<std::string> chk_out;

    // options
    int jobs = 0;          // 0 = automatic (RAM / 8 GB)
    bool vs_tools = false; // advanced: Visual Studio + LLVM instead of the included portable compiler
    bool clean = true;

    // install
    std::unique_ptr<Run> run;
    InstState is = kIdle;
    bool in_components = false;
    double t_comp0 = 0;
    PlanUi pi;
    int cur = -1;
    double t_start = 0, t_end = 0;
    std::deque<std::string> log;
    std::vector<std::string> log_tail;
    std::wstring log_file;
    double log_read = 0;
    std::string sub;
    float sub_frac = -1;
    std::string error, error_log, reason;
    bool log_open = false;
    int log_tab = 0;
    bool log_bottom = true;
    bool ask_stop = false, ask_exit = false;
    std::string exe_args = "--nvnvk";

    // done
    bool sc_desktop = true, sc_start = true;
    std::string done_msg;
    std::atomic<int64_t> game_size{-1};
    std::thread size_thread;

    Desc desc, desc_hover;
    bool has_previous = false, has_installed = false;

    // screenshot script
    int script = 0;
    double script_t = 0;
    std::wstring capture_req;
} E;

// ---------------------------------------------------------------- helpers
void Copy(char* dst, size_t n, const std::string& s) { std::snprintf(dst, n, "%s", s.c_str()); }
std::wstring WB(const char* b) { return W(b); }

std::wstring DestW() { return WB(E.dest); }
std::wstring WorkW() {
    if (E.work[0]) return WB(E.work);
    std::wstring d = DestW();
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d + L"-work";
}
std::wstring KeysW() { return E.keys[0] ? WB(E.keys) : E.found.prod; }
std::wstring TitleKeysW() { return E.tkeys[0] ? WB(E.tkeys) : (E.keys[0] ? L"" : E.found.title); }

void Go(Step s) {
    if (s == E.step) return;
    E.step = s;
    E.max_step = std::max(E.max_step, (int)s);
    E.t_change = ImGui::GetTime();
    E.desc = {};
    E.desc_hover = {};
}

std::wstring Pick(bool folder, const wchar_t* title, const std::wstring& start, bool keys = false) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return L"";
    DWORD f = 0;
    d->GetOptions(&f);
    d->SetOptions(f | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    d->SetTitle(title);
    if (!folder) {
        const COMDLG_FILTERSPEC dumps[] = {{L"Switch dumps (*.nsp, *.nsz, *.xci, *.xcz)", L"*.nsp;*.nsz;*.xci;*.xcz"}, {L"All files", L"*.*"}};
        const COMDLG_FILTERSPEC kf[] = {{L"Keys (*.keys)", L"*.keys"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, keys ? kf : dumps);
    }
    std::wstring s = start;
    while (!s.empty() && !IsDir(s)) {
        const std::wstring q = Parent(s);
        if (q == s) break;
        s = q;
    }
    if (!s.empty()) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(s.c_str(), nullptr, IID_PPV_ARGS(&si)))) d->SetFolder(si.Get());
    }
    if (d->Show(E.hwnd) != S_OK) return L"";
    ComPtr<IShellItem> r;
    if (FAILED(d->GetResult(&r))) return L"";
    PWSTR p = nullptr;
    if (FAILED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) return L"";
    std::wstring out = p;
    CoTaskMemFree(p);
    return out;
}

void Save() {
    if (!E.op.capture.empty()) return;  // the screenshot script never touches the user's settings
    E.cfg.Set(L"dump", WB(E.dump));
    E.cfg.Set(L"keys", WB(E.keys));
    E.cfg.Set(L"title_keys", WB(E.tkeys));
    E.cfg.Set(L"game_folder", WB(E.dest));
    E.cfg.Set(L"work_folder", WB(E.work));
    E.cfg.Set(L"jobs", std::to_wstring(E.jobs));
    E.cfg.Set(L"compiler", E.vs_tools ? L"vs" : L"portable");
    E.cfg.Set(L"clean", E.clean ? L"1" : L"0");
}

// ---- runtime paths ----
const Component* Comp(const char* id) { return E.man.Find(id); }
std::wstring CompRoot(const char* id) {
    const Component* c = Comp(id);
    return c ? Join(E.comp_dir, W(c->root)) : L"";
}
std::vector<std::wstring> PythonArgv() {
    if (E.bundle) return {Join(CompRoot("python"), L"python.exe")};
    return E.dev_py.argv;
}
std::wstring ScriptsRoot() { return E.bundle ? CompRoot("scripts") : E.dev_repo; }
std::wstring PathPrefix() { return E.bundle ? CompRoot("python") : L""; }
bool ReadyForIdentify() {
    if (E.bundle) {
        const Component *p = Comp("python"), *s = Comp("scripts");
        return p && s && IsInstalled(*p, E.comp_dir) && IsInstalled(*s, E.comp_dir);
    }
    return E.dev_ready.load() && E.dev_py.ok() && IsScriptsRoot(E.dev_repo);
}

void StartPrep(int goal) {
    if (!E.bundle) return;
    std::vector<std::string> ids = goal == 1 ? std::vector<std::string>{"python", "scripts"}
                                             : std::vector<std::string>{"python", "scripts", "platform", "toolchain"};
    // Optional: the PGO profile (build/pgo). Without it, or when it does not match, the compile phase builds
    // without PGO (launcher.py pgo_choice).
    if (goal != 1 && Comp("pgo")) ids.push_back("pgo");
    E.prep_goal = goal;
    E.prep.Start(E.man, ids, E.comp_dir, E.cache_dir, E.url_base);
}

std::vector<std::wstring> LauncherArgs() {
    std::vector<std::wstring> a = PythonArgv();
    a.push_back(Join(ScriptsRoot(), L"tools\\launcher\\launcher.py"));
    a.push_back(L"--input");
    a.push_back(WB(E.dump));
    a.push_back(L"--dest");
    a.push_back(DestW());
    a.push_back(L"--work");
    a.push_back(WorkW());
    if (!KeysW().empty()) { a.push_back(L"--keys"); a.push_back(KeysW()); }
    if (!TitleKeysW().empty()) { a.push_back(L"--title-keys"); a.push_back(TitleKeysW()); }
    if (E.jobs > 0) { a.push_back(L"--jobs"); a.push_back(std::to_wstring(E.jobs)); }
    if (E.bundle) {
        a.push_back(L"--fork");
        a.push_back(Join(ScriptsRoot(), L"mk8-recomp"));
        if (E.vs_tools) {
            a.push_back(L"--toolchain");
            a.push_back(L"vs");
        } else {
            a.push_back(L"--toolchain");
            a.push_back(L"portable");
            a.push_back(L"--toolchain-dir");
            a.push_back(CompRoot("toolchain"));
        }
    } else {
        a.push_back(L"--toolchain");
        a.push_back(E.vs_tools ? L"vs" : L"auto");
    }
    // Custom compile command (PLARecompSetup.ini compile_cmd=...): see docs/port/LAUNCHER.md §8.
    const std::wstring cc = E.cfg.Get(L"compile_cmd");
    if (!cc.empty()) { a.push_back(L"--build-cmd"); a.push_back(cc); }
    a.push_back(L"--events");
    return a;
}

void RequestCheck(double delay = 0.25) { E.chk_at = ImGui::GetTime() + delay; }

// Dump check log: <data dir>\logs\check.log (next to the components folder), so a tester can send it.
std::wstring CheckLogPath() { return Join(Join(E.comp_dir, L".."), L"logs\\check.log"); }

void SaveCheckLog() {
    const std::wstring dir = Join(Join(E.comp_dir, L".."), L"logs");
    CreateDirectoryW(Join(E.comp_dir, L"..").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string s = "dump: " + std::string(E.dump) + "\r\n";
    for (const auto& l : E.chk_out) s += l + "\r\n";
    if (!E.chk_err.empty()) s += "ERROR: " + E.chk_err + "\r\n";
    HANDLE h = CreateFileW(CheckLogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
    CloseHandle(h);
}

void FailCheck(const std::string& why) {
    E.chk_err = why;
    E.chk_done = true;
    SaveCheckLog();
    E.chk_err += "\nDetails: " + U8(CheckLogPath());
}

void Handle(const std::string& l, bool install);

void StartCheck() {
    E.chk_at = -1;
    if (E.chk) E.chk->Cancel();
    E.chk = std::make_unique<Run>();
    E.chk_done = false;
    E.chk_err.clear();
    E.ident = {};
    E.reqs.clear();
    E.plan = {};
    E.chk_out.clear();
    if (!E.dump[0]) return;
    if (!ReadyForIdentify()) {
        if (E.op.simulate) {
            E.chk->Simulate(false, "", 0.2);
            return;
        }
        FailCheck(E.bundle ? "The setup files are not ready." : "Developer mode: Python 3.10+ or the scripts were not found.");
        return;
    }
    std::vector<std::wstring> a = LauncherArgs();
    a.push_back(L"--dry-run");
    std::string err;
    if (!E.chk->Start(a, ScriptsRoot(), PathPrefix(), err)) FailCheck("could not start the check: " + err);
}

void LaunchInstaller() {
    E.in_components = false;
    E.run = std::make_unique<Run>();
    std::string err;
    if (E.op.simulate) {
        E.run->Simulate(true, E.op.sim_fail, E.op.sim_scale);
        return;
    }
    std::vector<std::wstring> a = LauncherArgs();
    a.push_back(L"--allow-heavy");
    if (E.clean) a.push_back(L"--clean");
    if (!E.op.only.empty()) { a.push_back(L"--only"); a.push_back(W(E.op.only)); }
    if (!E.run->Start(a, ScriptsRoot(), PathPrefix(), err)) {
        E.is = kFailed;
        E.error = err;
    }
}

void StartInstall() {
    Save();
    E.is = kRunning;
    E.pi = E.plan;
    for (auto& f : E.pi.phases)
        if (f.state == "done") f.seconds = -1;
    if (E.bundle) {
        PhaseUi c;
        c.id = "_components";
        c.minutes = 0.5;
        c.state = "pending";
        E.pi.phases.insert(E.pi.phases.begin(), c);
    }
    E.cur = -1;
    E.t_start = ImGui::GetTime();
    E.t_end = 0;
    E.log.clear();
    E.log_tail.clear();
    E.log_file.clear();
    E.sub.clear();
    E.sub_frac = -1;
    E.error.clear();
    E.error_log.clear();
    E.reason.clear();
    E.ask_stop = false;
    E.run.reset();
    if (E.bundle) {
        E.in_components = true;
        E.t_comp0 = ImGui::GetTime();
        E.pi.phases[0].state = "running";
        E.pi.phases[0].t0 = ImGui::GetTime();
        E.cur = 0;
        StartPrep(2);
    } else {
        LaunchInstaller();
    }
    Go(kInstall);
}

int PhaseIdx(const std::string& id) {
    for (size_t i = 0; i < E.pi.phases.size(); ++i)
        if (E.pi.phases[i].id == id) return (int)i;
    return -1;
}

void ParseUnits(const std::string& l) {
    const size_t a = l.find('[');
    int n = 0, m = 0;
    float pct = -1.f;
    // build_modules.py writes "[n/m p%]" (p weighted by bytes of C); Ninja and older drivers "[n/m]".
    const int got = a != std::string::npos ? std::sscanf(l.c_str() + a, "[%d/%d %f%%]", &n, &m, &pct) : 0;
    if (got >= 2 && m > 0) {
        E.sub = Fmt("%d of %d units", n, m);
        E.sub_frac = got == 3 && pct >= 0.f && pct <= 100.f ? pct / 100.f : (float)n / (float)m;
    }
}

void Handle(const std::string& l0, bool install) {
    if (l0.find('\n') != std::string::npos) {
        size_t a = 0;
        while (a <= l0.size()) {
            const size_t b = std::min(l0.find('\n', a), l0.size());
            Handle(l0.substr(a, b - a), install);
            a = b + 1;
        }
        return;
    }
    const std::string& l = l0;
    if (l.rfind("@@ ", 0) != 0) {
        auto& dst = install ? E.log : E.chk_out;
        dst.push_back(l);
        while (dst.size() > 4000) dst.pop_front();
        if (install) ParseUnits(l);
        return;
    }
    JVal v;
    if (!ParseJson(l.substr(3), v)) return;
    const std::string ev = v["ev"].str();
    if (ev == "ident") {
        IdentUi& d = E.ident;
        d = IdentUi{};  // the install run emits the identification again: replace it, don't append to it
        d.has = true;
        d.state = v["state"].str();
        d.build_id = v["build_id"].str();
        d.base = v["base"].exists() ? v["base"]["display"].str("?") : "";
        d.update = v["update"].exists() ? v["update"]["display"].str("?") : "";
        for (auto& x : v["warnings"].a) d.warnings.push_back(x.str());
        for (auto& x : v["differences"].a) d.differences.push_back(x.str());
        for (auto& x : v["files"].a) {
            std::string f = x["format"].str();
            for (char& ch : f) ch = (char)toupper((unsigned char)ch);
            d.files.push_back(f + " " + Size(x["size"].num()));
        }
    } else if (ev == "req") {
        ReqUi r;
        r.name = v["name"].str();
        r.ok = v["ok"].tri();
        r.detail = v["detail"].str();
        r.phases = v["phases"].str();
        r.help = v["help"].str();
        E.reqs.push_back(r);
    } else if (ev == "plan") {
        PlanUi p;
        p.has = true;
        p.total_min = v["total_min"].num();
        p.peak_gb = v["peak_gb"].num();
        p.final_gb = v["final_gb"].num();
        p.jobs = (int)v["jobs"].num();
        p.work = v["work"].str();
        p.dest = v["dest"].str();
        p.toolchain = v["toolchain"].str();
        p.toolchain_dir = v["toolchain_dir"].str();
        for (auto& x : v["phases"].a) {
            PhaseUi f;
            f.id = x["id"].str();
            f.cost = x["cost"].str();
            f.minutes = x["minutes"].num();
            f.gb = x["gb"].num();
            f.state = x["state"].str();
            f.skip = x["skip"].str();
            for (auto& y : x["steps"].a) f.steps.push_back(y.str());
            p.phases.push_back(f);
        }
        if (install) {
            // Keep the components pseudo-phase and what this session already knows; the launcher is authoritative.
            if (!E.pi.phases.empty() && E.pi.phases[0].id == "_components") p.phases.insert(p.phases.begin(), E.pi.phases[0]);
            for (auto& f : p.phases) {
                const int i = PhaseIdx(f.id);
                if (i >= 0 && E.pi.phases[i].seconds < 0) f.seconds = -1;
            }
            E.pi = p;
        } else {
            E.plan = p;
        }
    } else if (ev == "phase" && install) {
        const int i = PhaseIdx(v["id"].str());
        if (i < 0) return;
        PhaseUi& f = E.pi.phases[i];
        f.state = v["state"].str();
        if (f.state == "running") {
            f.t0 = ImGui::GetTime();
            f.step.clear();
            f.log = v["log"].str();
            E.cur = i;
            E.sub.clear();
            E.sub_frac = -1;
            E.log_file = W(f.log);
            E.log_tail.clear();
            E.log_read = 0;
        } else if (f.state == "done") {
            f.seconds = v["previous"].tri() == 1 ? -1 : v["seconds"].num();
            E.sub.clear();
            E.sub_frac = -1;
        } else if (f.state == "failed") {
            f.error = v["error"].str();
            E.error = f.error;
            E.error_log = v["log"].str();
        }
    } else if (ev == "step" && install) {
        if (E.cur >= 0) E.pi.phases[E.cur].step = v["desc"].str();
    } else if (ev == "end") {
        if (install) {
            if (v["exe_args"].exists()) E.exe_args = v["exe_args"].str();
            if ((int)v["rc"].num(-1) != 0) E.reason = v["reason"].str();
        }
    } else if (ev == "heavy" && install) {
        E.reason = "The launcher stopped before a heavy phase (" + v["id"].str() + ").";
    }
}

void ReadPhaseLog() {
    if (E.log_file.empty() || ImGui::GetTime() - E.log_read < 0.75) return;
    E.log_read = ImGui::GetTime();
    const std::string s = U8(ReadFileTail(E.log_file, 48 * 1024));
    E.log_tail.clear();
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        std::string l = s.substr(a, b - a);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        const size_t k = l.find_last_of('\r');  // Ninja rewrites the line with \r: keep the last one
        if (k != std::string::npos) l = l.substr(k + 1);
        E.log_tail.push_back(l);
        a = b + 1;
    }
    if (E.log_tail.size() > 600) E.log_tail.erase(E.log_tail.begin(), E.log_tail.end() - 600);
    for (auto it = E.log_tail.rbegin(); it != E.log_tail.rend(); ++it)
        if (it->find('[') != std::string::npos) {
            const float before = E.sub_frac;
            ParseUnits(*it);
            if (E.sub_frac != before || E.sub_frac >= 0) break;
        }
}

void Update() {
    // Setup files for identification (bundle mode): start as soon as the user reaches "Your game".
    if (E.bundle && E.step >= kGame && E.prep_goal == 0) StartPrep(1);
    if (E.chk_at >= 0 && ImGui::GetTime() >= E.chk_at && ReadyForIdentify()) StartCheck();
    if (E.chk) {
        for (auto& l : E.chk->Drain()) Handle(l, false);
        if (!E.chk->Alive() && !E.chk_done && E.chk_err.empty()) {
            for (auto& l : E.chk->Drain()) Handle(l, false);
            E.chk_done = true;
            if (!E.ident.has) {
                std::string why = "The check ended without a result (exit code " + std::to_string(E.chk->ExitCode()) + ").";
                for (auto it = E.chk_out.rbegin(); it != E.chk_out.rend(); ++it)
                    if (!it->empty()) { why += "\n" + *it; break; }
                FailCheck(why);
            } else {
                SaveCheckLog();
            }
        }
    }
    // Install: components first (bundle mode), then launcher.py.
    if (E.is == kRunning && E.in_components) {
        const Preparer::Snapshot s = E.prep.Get();
        if (!s.running && E.prep_goal == 2) {
            if (s.ok) {
                PhaseUi& c = E.pi.phases[0];
                c.state = "done";
                c.seconds = ImGui::GetTime() - c.t0;
                E.log.push_back("Setup files ready in " + U8(E.comp_dir));
                LaunchInstaller();
            } else {
                E.in_components = false;
                E.pi.phases[0].state = "failed";
                E.is = s.error == "cancelled" || s.error.find("cancelled") != std::string::npos ? kStopped : kFailed;
                E.error = s.error;
                E.t_end = ImGui::GetTime();
            }
        }
    }
    if (E.run) {
        for (auto& l : E.run->Drain()) Handle(l, true);
        if (E.is == kRunning) ReadPhaseLog();
        if (E.is == kRunning && !E.in_components && !E.run->Alive()) {
            for (auto& l : E.run->Drain()) Handle(l, true);
            E.t_end = ImGui::GetTime();
            if (E.run->Cancelled()) {
                E.is = kStopped;
                if (E.cur >= 0 && E.pi.phases[E.cur].state == "running") E.pi.phases[E.cur].state = "pending";
            } else if (E.run->ExitCode() == 0) {
                E.is = kFinished;
                E.game_size = -1;
                if (!E.op.simulate) {
                    const std::wstring d = DestW();
                    if (E.size_thread.joinable()) E.size_thread.join();
                    E.size_thread = std::thread([d] {
                        std::error_code ec;
                        int64_t t = 0;
                        for (auto it = std::filesystem::recursive_directory_iterator(d, ec); !ec && it != std::filesystem::recursive_directory_iterator();
                             it.increment(ec))
                            if (it->is_regular_file(ec)) t += (int64_t)it->file_size(ec);
                        E.game_size = t;
                    });
                }
                if (!E.op.simulate && E.op.only.empty() && E.op.capture.empty()) {
                    E.cfg.Set(L"installed", DestW());
                    E.cfg.Set(L"exe_args", W(E.exe_args));
                }
                Go(kDone);
            } else {
                E.is = kFailed;
                if (E.error.empty()) E.error = E.reason.empty() ? "Setup ended with exit code " + std::to_string(E.run->ExitCode()) + "." : E.reason;
            }
        }
    }
}

// ---------------------------------------------------------------- rows with a description (like the in-game menu)
struct Row {
    ImVec2 p;
    float w = 0, h = 0;
} R;

void RowBegin(const char* title, const std::string& dtitle, const std::string& dtext, float h = 64.0f, float label_frac = 0.30f) {
    ImGui::PushID(title);
    R.p = ImGui::GetCursorScreenPos();
    R.w = ImGui::GetContentRegionAvail().x;
    R.h = S(h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(R.p, {R.p.x + R.w, R.p.y + R.h});
    if (hov) {
        dl->AddRectFilled(R.p, {R.p.x + R.w, R.p.y + R.h}, C({0.745f, 0.722f, 0.859f, 1.0f}, 0.07f), S(8));
        E.desc_hover = {dtitle, dtext};
    }
    const ImVec2 ts = Measure(kReg, 19, title);
    Text(dl, kReg, 19, {R.p.x + S(18), R.p.y + (R.h - ts.y) * 0.5f}, C(kWhite, hov ? 1.0f : 0.86f), title);
    ImGui::SetCursorScreenPos({R.p.x + R.w * label_frac, R.p.y + (R.h - S(36)) * 0.5f});
}

void RowEnd() {
    ImGui::SetCursorScreenPos({R.p.x, R.p.y + R.h + S(2)});
    ImGui::Dummy({R.w, 0.0f});
    ImGui::PopID();
}

void Section(const char* t) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += S(10);
    Text(ImGui::GetWindowDrawList(), kBold, 15, {p.x + S(18), p.y}, C(kAccent2, 0.9f), t);
    ImGui::SetCursorScreenPos({p.x, p.y + Measure(kBold, 15, t).y + S(8)});
    ImGui::Dummy({1, 0});
}

// Status line: icon + title + detail (one line), with a hover description.
void StatusRow(const char* id, int state, const std::string& title, const std::string& detail, const std::string& dtext) {
    ImGui::PushID(id);
    ImDrawList* cl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = S(52);
    ImGui::InvisibleButton("##r", {w, h});
    if (ImGui::IsItemHovered()) {
        cl->AddRectFilled(p, {p.x + w, p.y + h}, C({0.745f, 0.722f, 0.859f, 1.0f}, 0.07f), S(8));
        E.desc_hover = {title, dtext.empty() ? detail : dtext};
    }
    StatusIcon(cl, {p.x + S(28), p.y + h * 0.5f}, S(10), state);
    Text(cl, kReg, 17, {p.x + S(48), p.y + S(6)}, C(kWhite, 0.92f), title.c_str());
    cl->PushClipRect({p.x + S(48), p.y}, {p.x + w - S(10), p.y + h}, true);
    Text(cl, kReg, 14, {p.x + S(48), p.y + S(29)}, C(kWhite, 0.5f), detail.c_str());
    cl->PopClipRect();
    ImGui::PopID();
}

void Card(ImDrawList* dl, ImVec2 a, ImVec2 b, const ImVec4& border = kWhite, float border_a = 0.12f) {
    dl->AddRectFilled(a, b, C(kWhite, 0.035f), S(12));
    dl->AddRect(a, b, C(border, border_a), S(12), 0, std::max(1.0f, S(1.0f)));
}

void Description(ImDrawList* dl, ImVec2 a, float w, float y_max) {
    const Desc& d = !E.desc_hover.title.empty() ? E.desc_hover : E.desc;
    if (d.title.empty()) return;
    float y = a.y;
    Text(dl, kBold, 22, {a.x, y}, C(kWhite), d.title.c_str(), w);
    y += Measure(kBold, 22, d.title.c_str(), w).y + S(10);
    if (y < y_max) Text(dl, kReg, 17, {a.x, y}, C(kWhite, 0.72f), d.text.c_str(), w);
}

// Bytes that the not-yet-unpacked components will take on disk.
uint64_t ComponentsToUnpack() {
    uint64_t t = 0;
    for (auto& c : E.man.items)
        if (!IsInstalled(c, E.comp_dir)) t += c.unpacked;
    return t;
}
uint64_t ComponentsToDownload() {
    uint64_t t = 0;
    for (auto& c : E.man.items)
        if (Availability(c, E.comp_dir, E.cache_dir) == Avail::Download) t += c.size;
    return t;
}

std::string PrepText(const Preparer::Snapshot& s) {
    if (s.stage == "downloading")
        return Fmt("Downloading %s: %s of %s", s.name.c_str(), Size((double)s.done).c_str(), Size((double)s.total).c_str()) +
               (s.speed > 0 ? Fmt(" (%s/s)", Size(s.speed).c_str()) : "");
    if (s.stage == "verifying") return "Verifying " + s.name + "…";
    if (s.stage == "unpacking") return Fmt("Unpacking %s… %d%%", s.name.c_str(), s.total ? (int)(100.0 * s.done / s.total) : 0);
    return "Preparing setup files…";
}

// ---------------------------------------------------------------- pages
void PageWelcome(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float lw = (b.x - a.x) * 0.56f;
    const float width = lw - S(36);
    ImVec2 p{a.x + S(36), a.y + S(34)};
    Text(dl, kBlack, 50, p, C(kWhite), "PLARecomp Setup");
    p.y += Measure(kBlack, 50, "P").y;
    Text(dl, kBold, 26, p, C(kAccent2), "A native PC build from your own game dump");
    p.y += Measure(kBold, 26, "P").y + S(16);
    dl->AddRectFilled(p, {p.x + S(110), p.y + S(3)}, C(kAccent), S(2));
    p.y += S(22);
    auto paragraph = [&](const char* t, float sz = 18, float al = 0.78f) {
        Text(dl, kReg, sz, p, C(kWhite, al), t, width);
        p.y += Measure(kReg, sz, t, width).y + S(12);
    };
    paragraph("This setup turns your own dump of the game into a native PC version: it checks your files, recompiles the game "
         "code on this PC and assembles a folder that is ready to play. Everything it needs is included.");
    paragraph("Everything is generated here from your files. This setup contains no game code, data or keys, and what it "
         "produces is for your own use only.",
         16, 0.55f);
    {
        p.y += S(10);
        struct Figure {
            std::string v;
            const char* t;
        };
        const bool has = E.plan.has;
        const Figure fs[] = {{has ? Approx(E.plan.total_min * 60) : "~1 h", "to install (mostly compiling)"},
                             {has ? "~" + Fmt("%.0f GB", E.plan.peak_gb) : "~27 GB", "free space while installing"},
                             {has ? "~" + Fmt("%.1f GB", E.plan.final_gb) : "~8 GB", "used by the game when done"}};
        const float cw = (width - S(24)) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            const ImVec2 ca{p.x + i * (cw + S(12)), p.y}, cb{ca.x + cw, ca.y + S(96)};
            Card(dl, ca, cb);
            Text(dl, kBlack, 30, {ca.x + S(16), ca.y + S(12)}, C(kAccent2), fs[i].v.c_str());
            Text(dl, kReg, 15, {ca.x + S(16), ca.y + S(54)}, C(kWhite, 0.6f), fs[i].t, cw - S(28));
        }
        p.y += S(96) + S(22);
    }
    if (E.has_installed || E.has_previous) {
        const ImVec2 cb{p.x + width, p.y + S(70)};
        Card(dl, p, cb, kAccent, 0.5f);
        const std::string d = U8(E.cfg.Get(E.has_installed ? L"installed" : L"game_folder"));
        Text(dl, kBold, 17, {p.x + S(16), p.y + S(12)}, C(kWhite), E.has_installed ? "A finished installation was found" : "An unfinished installation was found");
        Text(dl, kReg, 15, {p.x + S(16), p.y + S(38)}, C(kWhite, 0.55f), d.c_str());
        const char* lbl = E.has_installed ? "Go to Play" : "Resume";
        const float bw = Measure(kBold, 18, lbl).x + S(40);
        ImGui::SetCursorScreenPos({cb.x - bw - S(14), p.y + (S(70) - S(42)) * 0.5f});
        if (TextButton(lbl, kAccent, true)) {
            if (E.has_installed) {
                Copy(E.dest, sizeof E.dest, d);
                E.is = kFinished;
                E.max_step = kDone;
                Go(kDone);
            } else {
                E.max_step = kCheck;
                RequestCheck(0);
                Go(kCheck);
            }
        }
    }

    const float rx = a.x + lw + S(30), rw = b.x - rx - S(36);
    float y = a.y + S(40);
    Text(dl, kBold, 22, {rx, y}, C(kWhite), "What you need");
    y += S(40);
    struct Item {
        const char* t;
        std::string d;
    };
    const Item it[] = {
        {"Your game dump", Fmt("The base game and update 1.1.1 (title %s) as NSP, NSZ, XCI or XCZ, in one folder or as separate files.", kTitleId)},
        {"Your console keys", "prod.keys dumped from YOUR console. They are found automatically in the usual emulator folders."},
        {"Windows 10 or 11, 64-bit", "16 GB of RAM at least (32 GB compiles twice as fast) and a GPU with Vulkan."},
        {"Nothing else", "Python, the compiler and the platform are included in this setup. Nothing is installed system-wide."},
    };
    for (auto& x : it) {
        StatusIcon(dl, {rx + S(10), y + S(11)}, S(10), 1);
        Text(dl, kBold, 18, {rx + S(32), y}, C(kWhite), x.t);
        y += S(26);
        Text(dl, kReg, 15, {rx + S(32), y}, C(kWhite, 0.6f), x.d.c_str(), rw - S(32));
        y += Measure(kReg, 15, x.d.c_str(), rw - S(32)).y + S(16);
    }
    y += S(8);
    dl->AddLine({rx, y}, {rx + rw, y}, C(kWhite, 0.08f));
    y += S(16);
    Text(dl, kReg, 16, {rx, y}, C(kAccent2, 0.9f), "Graphics, controls and sound options are inside the game: press Esc while playing.", rw);
}

void PathRow(const char* title, char* buf, size_t n, const char* hint, const char* dt, const char* dx, int kind) {
    // kind: 0 dump (file or folder), 1 keys file, 2 folder
    RowBegin(title, dt, dx, 66, 0.27f);
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float bw = kind == 0 ? S(214) : S(104);
    const float fw = R.p.x + R.w - x0 - bw - S(14);
    bool changed = Field("##field", buf, n, fw, hint);
    ImGui::SameLine(0, S(8));
    std::wstring r;
    if (kind == 0) {
        if (Pill("File…", false, S(100))) r = Pick(false, L"Choose your game dump", WB(buf));
        ImGui::SameLine(0, S(6));
        if (Pill("Folder…", false, S(100))) r = Pick(true, L"Choose the folder with your dump", WB(buf));
    } else if (kind == 1) {
        if (Pill("Choose…", false, S(96))) r = Pick(false, L"Choose your keys file", WB(buf), true);
    } else {
        if (Pill("Change…", false, S(96))) r = Pick(true, L"Choose a folder", WB(buf));
    }
    if (!r.empty()) {
        Copy(buf, n, U8(r));
        changed = true;
    }
    if (changed) {
        Save();
        RequestCheck();
    }
    RowEnd();
}

std::string IdentStateText(const IdentUi& d) {
    if (d.state == "supported") return "Supported";
    if (d.base.empty() && d.update.empty()) return "Game not found";
    if (d.state == "incomplete") return "Incomplete";
    if (d.state == "unsupported") return "Unsupported version";
    if (d.state == "no keys") return "Keys missing";
    if (d.state == "pending") return "Could not be read";
    if (d.state == "empty") return "No dump found";
    return d.state;
}

// The launcher's identification warnings are technical; the common ones are rewritten here in plain words.
std::string IdentWarning(const std::string& w) {
    if (w.find("The base game") != std::string::npos) return Fmt("The base game (title %s) is missing.", kTitleId);
    if (w.find("The update 1.1.1") != std::string::npos) return "Update 1.1.1 is missing.";
    if (w.find("The update is v") != std::string::npos) return "The update is not version 1.1.1.";
    if (w.find("Your prod.keys cannot decrypt") != std::string::npos) return w;
    if (w.find("header_key") != std::string::npos || w.find("key") != std::string::npos) return "Your keys could not decrypt the dump.";
    return w;
}

void PageGame(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float lw = (b.x - a.x) * 0.60f;
    E.desc = {"Your game", "Choose where your dump is. Your keys are found automatically in the usual places; choose them only if "
                           "they are somewhere else. The dump is checked right away, without copying or writing anything."};
    ImGui::SetCursorScreenPos({a.x + S(16), a.y + S(14)});
    ImGui::BeginChild("##list", {lw - S(16), b.y - a.y - S(20)}, ImGuiChildFlags_NavFlattened);
    Section("DUMP AND KEYS");
    PathRow("Dump", E.dump, sizeof E.dump, "NSP/NSZ/XCI/XCZ file, or a folder with the base game and the update", "Game dump",
            Fmt("A file or a folder with the base game (%s) and update 1.1.1. Formats: NSP, NSZ, XCI and XCZ. It is identified "
                "by its content, not by its name.", kTitleId)
                .c_str(),
            0);
    const std::string khint = E.found.prod.empty() ? std::string("Not found automatically: choose your prod.keys")
                                                   : "Found (" + U8(E.found.where) + "): " + U8(E.found.prod);
    PathRow("prod.keys", E.keys, sizeof E.keys, khint.c_str(), "Your console keys (prod.keys)",
            "The keys of YOUR console. They are looked up in %APPDATA%\\suyu\\keys, yuzu, sudachi, citron, eden, torzu, "
            "%APPDATA%\\Ryujinx\\system, %USERPROFILE%\\.switch and next to this setup. They are never copied elsewhere or shown.",
            1);
    const std::string thint = E.found.title.empty() || E.keys[0] ? std::string("Optional") : "Found: " + U8(E.found.title);
    PathRow("title.keys", E.tkeys, sizeof E.tkeys, thint.c_str(), "title.keys (optional)",
            "Only needed if your dump uses title keys that are not inside the dump itself.", 1);
    Section("WHERE TO INSTALL");
    PathRow("Game folder", E.dest, sizeof E.dest, "A new folder for the game", "Game folder",
            "The game ends up here, ready to play (about 8 GB). A fast drive (SSD) and a short path work best.", 2);
    const std::string whint = "Automatic: " + U8(WorkW());
    PathRow("Work folder", E.work, sizeof E.work, whint.c_str(), "Work folder",
            "Temporary files of the installation (exported code, compiled objects). It needs a lot of space while installing; "
            "most of it is deleted at the end. Empty = next to the game folder. Keep the path short.",
            2);
    ImGui::EndChild();

    const float rx = a.x + lw + S(20), rw = b.x - rx - S(28);
    dl->AddLine({rx - S(10), a.y + S(20)}, {rx - S(10), b.y - S(20)}, C(kWhite, 0.08f));
    float y = a.y + S(26);
    float ch = S(200);
    const Preparer::Snapshot ps = E.prep.Get();
    const bool preparing = E.bundle && !ReadyForIdentify();
    const bool checking = (E.chk && E.chk->Alive()) || E.chk_at >= 0;
    if (preparing) {
        const bool failed = ps.failed;
        StatusIcon(dl, {rx + S(12), y + S(12)}, S(11), failed ? 0 : 2);
        Text(dl, kBold, 19, {rx + S(34), y}, C(failed ? kDanger : kWhite), failed ? "Setup files could not be prepared" : "Preparing setup files…");
        const std::string t = failed ? ps.error : PrepText(ps);
        Text(dl, kReg, 15, {rx + S(34), y + S(28)}, C(kWhite, 0.6f), t.c_str(), rw - S(34));
        if (!failed && ps.total) Bar(dl, {rx + S(34), y + S(54)}, {rx + rw, y + S(62)}, (float)ps.done / (float)ps.total, kAccent, true);
        ch = S(76);
    } else if (!E.dump[0]) {
        Text(dl, kReg, 17, {rx, y}, C(kWhite, 0.5f), "Choose your dump to check it.", rw);
        ch = S(30);
    } else if (checking) {
        StatusIcon(dl, {rx + S(12), y + S(12)}, S(11), 2);
        Text(dl, kBold, 19, {rx + S(34), y}, C(kWhite), "Reading the dump…");
        Text(dl, kReg, 15, {rx + S(34), y + S(28)}, C(kWhite, 0.55f), "Identified in place, nothing is extracted (a few seconds).", rw - S(34));
        ch = S(60);
    } else if (!E.chk_err.empty()) {
        StatusIcon(dl, {rx + S(12), y + S(12)}, S(11), 0);
        Text(dl, kBold, 19, {rx + S(34), y}, C(kDanger), "Could not check the dump");
        Text(dl, kReg, 15, {rx + S(34), y + S(28)}, C(kWhite, 0.7f), E.chk_err.c_str(), rw - S(34));
        ch = S(40) + Measure(kReg, 15, E.chk_err.c_str(), rw - S(34)).y;
    } else if (E.ident.has) {
        const bool ok = E.ident.state == "supported";
        const bool none = E.ident.base.empty() && E.ident.update.empty();
        StatusIcon(dl, {rx + S(12), y + S(13)}, S(12), ok ? 1 : (E.ident.state == "incomplete" && !none ? -1 : 0));
        Text(dl, kBold, 22, {rx + S(36), y}, C(ok ? kAccent2 : kDanger), IdentStateText(E.ident).c_str());
        float yy = y + S(36);
        auto line = [&](const char* k, const std::string& v) {
            if (v.empty() || yy > b.y - S(170)) return;
            Text(dl, kReg, 15, {rx, yy}, C(kWhite, 0.5f), k);
            Text(dl, kReg, 15, {rx + S(110), yy}, C(kWhite, 0.85f), v.c_str(), rw - S(110));
            yy += Measure(kReg, 15, v.c_str(), rw - S(110)).y + S(5);
        };
        line("Title", kTitleId);
        line("Base game", E.ident.base.empty() ? "not found" : E.ident.base);
        line("Update", E.ident.update.empty() ? "not found" : E.ident.update);
        if (!E.ident.build_id.empty()) line("Build id", E.ident.build_id.substr(0, 16) + "…");
        std::string files;
        for (size_t k = 0; k < E.ident.files.size() && k < 4; ++k) files += (files.empty() ? "" : ", ") + E.ident.files[k];
        if (E.ident.files.size() > 4) files += Fmt(" and %d more", (int)E.ident.files.size() - 4);
        line("Files", files);
        for (auto& x : E.ident.warnings) line("Note", IdentWarning(x));
        if (!E.ident.differences.empty()) line("Hashes", "do not match version 1.1.1");
        ch = yy - y;
    }
    y += ch + S(26);
    dl->AddLine({rx, y - S(12)}, {rx + rw, y - S(12)}, C(kWhite, 0.08f));
    Description(dl, {rx, y}, rw, b.y - S(20));
}

bool IdentOk() { return E.ident.has && E.ident.state == "supported"; }
bool KeysOk() { return !KeysW().empty() && Exists(KeysW()); }

struct Space {
    uint64_t free = 0, total = 0, need = 0;
    bool ok = true;
};
Space WorkSpace() {
    Space s;
    DiskSpace(WorkW(), s.free, s.total);
    s.need = (uint64_t)(E.plan.peak_gb * 1073741824.0);
    // Setup files go to the components folder; count them if they share the volume.
    if (E.bundle && VolumeRoot(E.comp_dir) == VolumeRoot(WorkW())) s.need += ComponentsToUnpack();
    s.ok = !E.plan.has || s.free >= s.need;
    return s;
}

int Blockers() {
    int n = 0;
    n += !IdentOk();
    n += !KeysOk();
    n += !WorkSpace().ok;
    if (E.bundle && ComponentsToDownload() > 0 && E.url_base.find("://") == std::string::npos) ++n;
    for (auto& r : E.reqs)
        if (r.ok == 0 && (r.name.rfind("Windows", 0) == 0)) ++n;
    return n;
}

bool ReadyToInstall() {
    return E.chk_done && E.chk_err.empty() && E.plan.has && E.dest[0] && Blockers() == 0 && !(E.chk && E.chk->Alive());
}

void PageCheck(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float lw = (b.x - a.x) * 0.58f;
    E.desc = {"Check", "What the installation needs from you and from this PC. Everything else is included in this setup."};
    ImGui::SetCursorScreenPos({a.x + S(16), a.y + S(14)});
    ImGui::BeginChild("##list", {lw - S(16), b.y - a.y - S(20)}, ImGuiChildFlags_NavFlattened);
    const bool checking = (E.chk && E.chk->Alive()) || E.chk_at >= 0;
    if (checking && !E.plan.has) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        StatusIcon(ImGui::GetWindowDrawList(), {p.x + S(30), p.y + S(30)}, S(11), 2);
        Text(ImGui::GetWindowDrawList(), kReg, 18, {p.x + S(52), p.y + S(19)}, C(kWhite, 0.8f), "Checking…");
        ImGui::Dummy({1, S(60)});
    }
    Section("YOUR FILES");
    StatusRow("dump", IdentOk() ? 1 : (E.ident.has ? 0 : 3), "Game dump",
              IdentOk() ? Fmt("Title %s, update %s", kTitleId, E.ident.update.c_str()) : (E.ident.has ? IdentStateText(E.ident) : "not checked yet"),
              "Your dump is identified by its content. Go back to «Your game» to change it.");
    StatusRow("keys", KeysOk() ? 1 : 0, "Console keys",
              KeysOk() ? (E.keys[0] ? std::string("chosen: ") + E.keys : "found (" + U8(E.found.where) + "): " + U8(E.found.prod))
                       : "not found: choose your prod.keys in «Your game»",
              "Keys cannot be included or downloaded: they come from your own console.");
    Section("THIS PC");
    {
        const Space sp = WorkSpace();
        StatusRow("disk", E.plan.has ? (sp.ok ? 1 : 0) : 3, "Disk space",
                  E.plan.has ? "Needs ~" + Size((double)sp.need) + " on " + U8(VolumeRoot(WorkW())) + ", " + Size((double)sp.free) + " free" : "…",
                  "Peak space while installing (the work folder plus the game folder), plus the setup files if they go to the same drive. "
                  "When finished the game uses about 8 GB and most temporary files are deleted.");
        StatusRow("ram", E.ram >= (16ull << 30) ? 1 : -1, "Memory",
                  Fmt("%.0f GB: compile jobs are chosen automatically from the free memory", E.ram / 1073741824.0),
                  "The game code is compiled in small pieces (about 0.5 GB of RAM each), as many at once as the processor "
                  "cores and the free memory allow. With less memory or fewer cores it takes longer.");
    }
    for (auto& r : E.reqs) {
        // Only the coordination warnings of this machine matter here (another heavy job or the game running).
        if (r.ok == -1 && (r.name.find("locks") != std::string::npos || r.name.find("processes") != std::string::npos))
            StatusRow(r.name.c_str(), -1, "Another heavy task is running", r.detail,
                      "Exporting and compiling need a lot of memory. Better wait until the game or the other task has finished.");
    }
    if (E.bundle) {
        Section("INCLUDED IN THIS SETUP");
        for (auto& c : E.man.items) {
            const Avail av = Availability(c, E.comp_dir, E.cache_dir);
            std::string d;
            int st = 1;
            if (av == Avail::Installed) d = "ready";
            else if (av == Avail::Embedded) d = "included, will be unpacked (" + Size((double)c.unpacked) + ")";
            else if (av == Avail::Cached) d = "downloaded, will be unpacked (" + Size((double)c.unpacked) + ")";
            else {
                d = "will be downloaded (" + Size((double)c.size) + ")";
                st = E.url_base.find("://") != std::string::npos ? 3 : 0;
            }
            StatusRow(c.id.c_str(), st, c.name + " " + c.version, d,
                      c.name + " " + c.version + ", license " + c.license + ". Unpacked into " + U8(E.comp_dir) + ".");
        }
    }
    if (E.chk_done && E.chk_err.empty()) {
        Section("OPTIONS");
        RowBegin("Compile jobs", "Compile jobs",
                 "How many pieces are compiled at once. «Automatic» uses every processor core while the free memory allows "
                 "(about 0.5 GB per piece). Fewer jobs = slower, but the PC stays more responsive.",
                 56, 0.40f);
        const int prev = E.jobs;
        if (Pill("Automatic", E.jobs == 0, S(110))) E.jobs = 0;
        for (int j : {2, 4, 8, 16}) {
            ImGui::SameLine(0, S(6));
            if (Pill(std::to_string(j).c_str(), E.jobs == j, S(40))) E.jobs = j;
        }
        RowEnd();
        if (prev != E.jobs) { Save(); RequestCheck(0.1); }
        RowBegin("Compiler", "Compiler (advanced)",
                 "Included: the portable compiler that ships with this setup (clang-cl + lld-link). Nothing to install.\n"
                 "Visual Studio + LLVM: only if you already have VS 2022 Build Tools, LLVM, CMake and Ninja installed.",
                 56, 0.40f);
        const bool prev_vs = E.vs_tools;
        if (Pill("Included", !E.vs_tools, S(110))) E.vs_tools = false;
        ImGui::SameLine(0, S(6));
        if (Pill("Visual Studio + LLVM", E.vs_tools)) E.vs_tools = true;
        RowEnd();
        if (prev_vs != E.vs_tools) { Save(); RequestCheck(0.1); }
        RowBegin("Delete temporary files", "Delete temporary files when done",
                 "Deletes the exported code and the compiled objects at the end (about 10 GB). If you turn it off, a later "
                 "reinstall is faster.",
                 56, 0.40f);
        if (Pill("Yes", E.clean, S(70))) { E.clean = true; Save(); }
        ImGui::SameLine(0, S(6));
        if (Pill("No", !E.clean, S(70))) { E.clean = false; Save(); }
        RowEnd();
    }
    ImGui::Dummy({1, S(12)});
    ImGui::EndChild();

    const float rx = a.x + lw + S(20), rw = b.x - rx - S(28);
    dl->AddLine({rx - S(10), a.y + S(20)}, {rx - S(10), b.y - S(20)}, C(kWhite, 0.08f));
    float y = a.y + S(26);
    if (!E.desc_hover.title.empty() || !E.plan.has) {
        Description(dl, {rx, y}, rw, b.y - S(20));
        return;
    }
    const Space sp = WorkSpace();
    Text(dl, kBold, 20, {rx, y}, C(kWhite), "Disk space");
    y += S(32);
    {
        const ImVec2 ba{rx, y}, bb{rx + rw, y + S(14)};
        dl->AddRectFilled(ba, bb, C(kWhite, 0.07f), S(7));
        if (sp.total) {
            const float used = (float)((double)(sp.total - sp.free) / sp.total), need = (float)std::min(1.0, (double)sp.need / sp.total);
            const float xu = ba.x + rw * used;
            dl->AddRectFilled(ba, {xu, bb.y}, C(kWhite, 0.28f), S(7));
            dl->AddRectFilled({xu, ba.y}, {std::min(bb.x, xu + rw * need), bb.y}, C(sp.ok ? kAccent : kDanger, 0.95f), S(4));
        }
        y += S(24);
    }
    const std::string l1 = "While installing: ~" + Size((double)sp.need) + " (peak)";
    const std::string l2 = "When done: ~" + Fmt("%.1f GB", E.plan.final_gb) + (E.clean ? " (temporary files deleted)" : " + temporary files (~10 GB)");
    const std::string l3 = "Free on " + U8(VolumeRoot(WorkW())) + ": " + Size((double)sp.free) + " of " + Size((double)sp.total);
    dl->AddRectFilled({rx, y + S(5)}, {rx + S(10), y + S(15)}, C(sp.ok ? kAccent : kDanger), S(2));
    Text(dl, kReg, 16, {rx + S(18), y}, C(kWhite, 0.85f), l1.c_str());
    y += S(24);
    Text(dl, kReg, 16, {rx + S(18), y}, C(kWhite, 0.65f), l2.c_str(), rw - S(18));
    y += Measure(kReg, 16, l2.c_str(), rw - S(18)).y + S(4);
    dl->AddRectFilled({rx, y + S(5)}, {rx + S(10), y + S(15)}, C(kWhite, 0.28f), S(2));
    Text(dl, kReg, 16, {rx + S(18), y}, C(kWhite, 0.65f), l3.c_str());
    y += S(36);
    double comp_min = 0;
    for (auto& f : E.plan.phases)
        if (f.id == "compile") comp_min = f.minutes;
    Text(dl, kBold, 20, {rx, y}, C(kWhite), "Time");
    y += S(32);
    const std::string t1 = Approx(E.plan.total_min * 60) + " in total";
    const std::string t2 = "Compiling: " + Approx(comp_min * 60) +
                           (E.plan.jobs > 0 ? Fmt(" with %d jobs", E.plan.jobs) : std::string(" (automatic jobs)"));
    Text(dl, kReg, 16, {rx, y}, C(kWhite, 0.85f), t1.c_str());
    y += S(24);
    Text(dl, kReg, 16, {rx, y}, C(kWhite, 0.65f), t2.c_str(), rw);
    y += S(40);
    Text(dl, kBold, 20, {rx, y}, C(kWhite), "Phases");
    y += S(32);
    for (auto& f : E.plan.phases) {
        if (y > b.y - S(30)) break;
        const int st = f.state == "done" ? 1 : f.state == "skipped" ? 4 : 3;
        StatusIcon(dl, {rx + S(8), y + S(10)}, S(7), st);
        Text(dl, kReg, 15, {rx + S(24), y}, C(kWhite, st == 4 ? 0.4f : 0.8f), PhaseTitle(f.id).c_str());
        const std::string m = st == 1 ? "done" : st == 4 ? "not needed" : Approx(f.minutes * 60);
        const ImVec2 ms = Measure(kReg, 14, m.c_str());
        Text(dl, kReg, 14, {rx + rw - ms.x, y + S(1)}, C(kWhite, 0.45f), m.c_str());
        y += S(23);
    }
}

double PhaseSec(const PhaseUi& f) { return std::max(1.0, f.minutes * 60.0); }

float Progress(double& left) {
    double total = 0, done = 0;
    left = 0;
    const double now = ImGui::GetTime();
    for (size_t i = 0; i < E.pi.phases.size(); ++i) {
        const PhaseUi& f = E.pi.phases[i];
        if (f.state == "skipped") continue;
        const double s = PhaseSec(f);
        total += s;
        if (f.state == "done") {
            done += s;
        } else if (f.state == "running" && E.is == kRunning) {
            double part = std::min(now - f.t0, s * 0.97);
            if (f.id == "_components") {
                const Preparer::Snapshot ps = E.prep.Get();
                part = s * std::min(0.97, (ps.index + (ps.total ? (double)ps.done / ps.total : 0)) / std::max(1, ps.count));
            }
            if (E.sub_frac >= 0 && f.id == "compile") part = std::max(part * 0.5, s * E.sub_frac * 0.97);
            done += part;
            left += std::max(s - part, s * 0.03);
        } else {
            left += s;
        }
    }
    return total > 0 ? (float)(done / total) : 0.0f;
}

void LogPanel(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, C({0.022f, 0.018f, 0.04f, 1.0f}), S(12));
    dl->AddRect(a, b, C(kWhite, 0.14f), S(12));
    ImGui::SetCursorScreenPos({a.x + S(14), a.y + S(10)});
    const std::string phase = E.cur >= 0 ? "Phase: " + PhaseTitle(E.pi.phases[E.cur].id) : "Phase";
    if (Pill("Setup", E.log_tab == 0, S(90))) E.log_tab = 0;
    ImGui::SameLine(0, S(6));
    if (Pill((phase + "##ph").c_str(), E.log_tab == 1, S(110), !E.log_file.empty())) E.log_tab = 1;
    ImGui::SameLine(0, S(14));
    if (Pill("Copy", false, S(70))) {
        std::string t;
        if (E.log_tab == 0) for (auto& l : E.log) t += l + "\n";
        else for (auto& l : E.log_tail) t += l + "\n";
        ImGui::SetClipboardText(t.c_str());
    }
    ImGui::SameLine(0, S(6));
    const std::wstring logs = Join(WorkW(), L"logs");
    if (Pill("Open folder", false, S(110), IsDir(logs))) OpenInExplorer(logs);
    {
        const float isz = S(34);
        ImGui::SetCursorScreenPos({b.x - S(12) - isz, a.y + S(10)});
        if (IconButton("##closelog", 4, isz)) E.log_open = false;
    }
    const ImVec2 la{a.x + S(14), a.y + S(56)};
    ImGui::SetCursorScreenPos(la);
    ImGui::PushFont(g_fonts[kReg], S(14));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, S(1)});
    if (ImGui::BeginChild("##loglines", {b.x - la.x - S(10), b.y - la.y - S(10)})) {
        const auto draw = [](const std::string& l) {
            ImVec4 col{1, 1, 1, 0.72f};
            if (l.find("ERROR") != std::string::npos || l.find("error") != std::string::npos) col = {kDanger.x, kDanger.y, kDanger.z, 0.95f};
            else if (l.rfind("[", 0) == 0 || l.rfind("$ ", 0) == 0) col = {kAccent2.x, kAccent2.y, kAccent2.z, 0.9f};
            else if (l.find("WARNING") != std::string::npos || l.find("warning") != std::string::npos) col = {kWarn.x, kWarn.y, kWarn.z, 0.9f};
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.c_str());
            ImGui::PopStyleColor();
        };
        if (E.log_tab == 0) for (auto& l : E.log) draw(l);
        else for (auto& l : E.log_tail) draw(l);
        if (E.log_bottom) ImGui::SetScrollHereY(1.0f);
        E.log_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
}

void PageInstall(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    double left = 0;
    const float frac = E.is == kFinished ? 1.0f : Progress(left);
    const double now = E.is == kRunning ? ImGui::GetTime() : (E.t_end > 0 ? E.t_end : ImGui::GetTime());
    const PhaseUi* cur = E.cur >= 0 ? &E.pi.phases[E.cur] : nullptr;
    float y = a.y + S(28);
    const float x0 = a.x + S(36), w = b.x - a.x - S(72);
    std::string title, sub;
    ImVec4 tcol = kWhite;
    if (E.is == kRunning) {
        title = cur ? PhaseTitle(cur->id) : "Starting…";
        sub = E.in_components ? PrepText(E.prep.Get()) : (cur ? PhaseNote(cur->id) : "Reading the dump and the plan…");
    } else if (E.is == kStopped) {
        title = "Installation stopped";
        sub = "Everything done so far is kept. Press «Resume» to continue" + std::string(cur ? " from «" + PhaseTitle(cur->id) + "»." : ".");
        tcol = kWarn;
    } else if (E.is == kFailed) {
        title = "Something went wrong";
        sub = E.error;
        tcol = kDanger;
    } else if (E.is == kFinished) {
        title = "Installation complete";
        sub = "All done.";
    }
    Text(dl, kBlack, 32, {x0, y}, C(tcol), title.c_str(), w);
    y += Measure(kBlack, 32, title.c_str(), w).y + S(6);
    Text(dl, kReg, 17, {x0, y}, C(kWhite, 0.65f), sub.c_str(), w);
    y += Measure(kReg, 17, sub.c_str(), w).y + S(18);
    Bar(dl, {x0, y}, {x0 + w, y + S(14)}, frac, E.is == kFailed ? kDanger : E.is == kStopped ? kWarn : kAccent, E.is == kRunning);
    y += S(24);
    {
        const std::string pct = Fmt("%d %%", (int)(frac * 100 + 0.5f));
        Text(dl, kBold, 16, {x0, y}, C(kWhite, 0.9f), pct.c_str());
        std::string t = "Elapsed " + Elapsed(now - E.t_start);
        if (E.is == kRunning) t += left < 60 ? "  ·  Less than a minute left" : "  ·  " + Approx(left) + " left";
        const ImVec2 ts = Measure(kReg, 16, t.c_str());
        Text(dl, kReg, 16, {x0 + w - ts.x, y}, C(kWhite, 0.65f), t.c_str());
        y += S(36);
    }
    const float lw = w * 0.55f, log_h = S(44), ly0 = y;
    for (size_t i = 0; i < E.pi.phases.size(); ++i) {
        const PhaseUi& f = E.pi.phases[i];
        const bool skip = f.state == "skipped";
        int st = f.state == "done" ? 1 : f.state == "failed" ? 0 : skip ? 4 : 3;
        if (f.state == "running") st = E.is == kRunning ? 2 : 3;
        const float rh = S(29);
        if (y + rh > b.y - log_h - S(6)) break;
        if ((int)i == E.cur && E.is == kRunning) dl->AddRectFilled({x0 - S(10), y - S(3)}, {x0 + lw, y + rh - S(3)}, C(kAccent, 0.10f), S(8));
        StatusIcon(dl, {x0 + S(10), y + S(11)}, S(9), st);
        Text(dl, (int)i == E.cur ? kBold : kReg, 17, {x0 + S(30), y}, C(kWhite, skip ? 0.38f : st == 3 ? 0.62f : 0.92f), PhaseTitle(f.id).c_str());
        std::string m;
        if (st == 1) m = f.seconds >= 0 ? Elapsed(f.seconds) : "already done";
        else if (st == 2) m = Elapsed(now - f.t0) + " / " + Approx(PhaseSec(f));
        else if (skip) m = "not needed";
        else if (st == 0) m = "failed";
        else m = Approx(PhaseSec(f));
        const ImVec2 ms = Measure(kReg, 15, m.c_str());
        Text(dl, kReg, 15, {x0 + lw - ms.x - S(12), y + S(2)}, C(st == 0 ? kDanger : kWhite, 0.55f), m.c_str());
        y += rh;
    }
    {
        const float rx = x0 + lw + S(30), rw = x0 + w - rx;
        float yy = ly0;
        dl->AddLine({rx - S(14), ly0}, {rx - S(14), b.y - log_h - S(10)}, C(kWhite, 0.08f));
        if (E.is == kRunning && cur) {
            Text(dl, kBold, 19, {rx, yy}, C(kWhite), "Now");
            yy += S(30);
            if (E.in_components) {
                const Preparer::Snapshot ps = E.prep.Get();
                const std::string t = Fmt("Component %d of %d", ps.index + 1, ps.count);
                Text(dl, kReg, 16, {rx, yy}, C(kAccent2), t.c_str());
                yy += S(22);
                Bar(dl, {rx, yy}, {rx + rw, yy + S(8)}, ps.total ? (float)ps.done / (float)ps.total : 0.0f, kAccent2, false);
                yy += S(22);
            }
            if (!E.sub.empty() && cur->id == "compile") {
                Text(dl, kReg, 16, {rx, yy}, C(kAccent2), E.sub.c_str());
                yy += S(22);
                Bar(dl, {rx, yy}, {rx + rw, yy + S(8)}, E.sub_frac, kAccent2, false);
                yy += S(22);
            }
            const std::string note = PhaseNote(cur->id);
            Text(dl, kReg, 16, {rx, yy}, C(kWhite, 0.65f), note.c_str(), rw);
        } else if (E.is == kStopped) {
            Text(dl, kReg, 16, {rx, yy}, C(kWhite, 0.65f),
                 "You can close the setup: next time it offers to continue. Finished phases are not repeated.", rw);
        } else if (E.is == kFailed) {
            Text(dl, kBold, 19, {rx, yy}, C(kDanger), "What to do");
            yy += S(30);
            Text(dl, kReg, 16, {rx, yy}, C(kWhite, 0.65f),
                 "Open the log to see the full error. Fix the cause (disk space, the game running…) and press «Retry»: it "
                 "continues from the phase that failed.",
                 rw);
        }
    }
    {
        const ImVec2 ra{x0 - S(10), b.y - log_h}, rb{x0 + w + S(10), b.y - S(6)};
        ImGui::SetCursorScreenPos(ra);
        if (ImGui::InvisibleButton("##log", {rb.x - ra.x, rb.y - ra.y}, ImGuiButtonFlags_EnableNav)) E.log_open = !E.log_open;
        const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        dl->AddRectFilled(ra, rb, C(kWhite, hov ? 0.08f : 0.04f), S(10));
        Arrow(dl, {ra.x + S(20), (ra.y + rb.y) * 0.5f}, S(5), E.log_open ? 1 : 3, C(kWhite, 0.8f));
        std::string last;
        for (auto it = E.log.rbegin(); it != E.log.rend(); ++it)
            if (!it->empty() && it->rfind("$ ", 0) != 0) { last = *it; break; }
        Text(dl, kBold, 16, {ra.x + S(36), ra.y + S(10)}, C(kWhite, 0.85f), "Log");
        dl->PushClipRect({ra.x + S(90), ra.y}, {rb.x - S(10), rb.y}, true);
        Text(dl, kReg, 15, {ra.x + S(90), ra.y + S(11)}, C(kWhite, 0.45f), last.c_str());
        dl->PopClipRect();
    }
    if (E.log_open) {
        const float top = std::max(a.y + S(80), b.y - log_h - (b.y - a.y) * 0.62f);
        LogPanel(dl, {x0 - S(10), top}, {x0 + w + S(10), b.y - log_h - S(8)});
    }
}

void Play() {
    if (E.op.simulate) {
        E.done_msg = "(simulation) The game would start now: PLA.exe " + E.exe_args + ".";
        return;
    }
    const std::wstring d = DestW(), exe = Join(d, L"PLA.exe");
    std::wstring cmd = QuoteArg(exe) + L" " + W(E.exe_args);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, d.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        E.exit = true;
    } else {
        E.done_msg = Fmt("Could not start the game (error %lu).", GetLastError());
    }
}

void MakeShortcuts() {
    if (E.op.simulate) return;
    const std::wstring d = DestW(), exe = Join(d, L"PLA.exe");
    const wchar_t* name = L"PLARecomp.lnk";
    int ok = 0, n = 0;
    if (E.sc_desktop) { ++n; ok += CreateShortcut(Join(KnownFolder(CSIDL_DESKTOPDIRECTORY), name), exe, W(E.exe_args), d, L"PLARecomp"); }
    if (E.sc_start) { ++n; ok += CreateShortcut(Join(KnownFolder(CSIDL_PROGRAMS), name), exe, W(E.exe_args), d, L"PLARecomp"); }
    if (n) E.done_msg = ok == n ? "Shortcuts created." : "Some shortcut could not be created.";
}

void PageDone(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    const float cx = (a.x + b.x) * 0.5f;
    float y = a.y + S(50);
    const float r = S(40);
    const float t = Ease((float)(ImGui::GetTime() - E.t_change) / 0.5f);
    dl->AddCircleFilled({cx, y + r}, r * (0.6f + 0.4f * t), C(kAccent, 0.95f), 48);
    dl->AddCircle({cx, y + r}, r * 1.35f, C(kAccent, 0.25f * t), 48, S(2));
    Check(dl, {cx, y + r}, r * 0.75f * t, C(kInk), S(5));
    y += r * 2 + S(26);
    const char* title = "Ready to play!";
    ImVec2 ts = Measure(kBlack, 40, title);
    Text(dl, kBlack, 40, {cx - ts.x * 0.5f, y}, C(kWhite), title);
    y += ts.y + S(8);
    std::string d = E.op.simulate ? std::string("(simulation) ") + E.dest : std::string(E.dest);
    const int64_t sz = E.game_size.load();
    if (sz > 0) d += "  ·  " + Size((double)sz);
    ts = Measure(kReg, 17, d.c_str());
    Text(dl, kReg, 17, {cx - ts.x * 0.5f, y}, C(kWhite, 0.6f), d.c_str());
    y += ts.y + S(30);
    const float w = S(560);
    ImGui::SetCursorScreenPos({cx - w * 0.5f, y});
    ImGui::BeginGroup();
    auto row = [&](const char* t, bool& v) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Text(dl, kReg, 18, {p.x, p.y + S(7)}, C(kWhite, 0.86f), t);
        ImGui::SetCursorScreenPos({p.x + w - S(160) - S(8), p.y});
        ImGui::PushID(t);
        if (Pill("Yes", v, S(80))) v = true;
        ImGui::SameLine(0, S(6));
        if (Pill("No", !v, S(80))) v = false;
        ImGui::PopID();
        ImGui::SetCursorScreenPos({p.x, p.y + S(46)});
    };
    row("Desktop shortcut", E.sc_desktop);
    row("Start menu shortcut", E.sc_start);
    ImGui::EndGroup();
    y = ImGui::GetCursorScreenPos().y + S(16);
    {
        const ImVec2 ca{cx - w * 0.5f, y}, cb{cx + w * 0.5f, y + S(92)};
        Card(dl, ca, cb, kAccent, 0.4f);
        float kx = ca.x + S(18);
        kx += Keycap(dl, {kx, ca.y + S(18)}, S(28), "Esc", C(kWhite, 0.6f), 15) + S(12);
        Text(dl, kBold, 18, {kx, ca.y + S(20)}, C(kWhite), "Options are inside the game");
        Text(dl, kReg, 15, {ca.x + S(18), ca.y + S(56)}, C(kWhite, 0.6f),
             "Graphics, display, controls, sound and engine: press Esc while playing (or hold Minus + Plus on a controller).", cb.x - ca.x - S(36));
        y = cb.y + S(18);
    }
    if (!E.done_msg.empty()) {
        ts = Measure(kBold, 16, E.done_msg.c_str());
        Text(dl, kBold, 16, {cx - ts.x * 0.5f, y}, C(kAccent2), E.done_msg.c_str());
    }
}

// ---------------------------------------------------------------- frame: panel, tabs, footer
void Tabs(ImDrawList* dl, ImVec2 a, ImVec2 b, float tab_h) {
    float x = a.x + S(22);
    {
        const ImVec2 c{x + S(14), a.y + tab_h * 0.5f};
        dl->AddCircle(c, S(13), C(kAccent), 32, S(2.4f));
        dl->AddLine({c.x, c.y - S(6)}, {c.x, c.y + S(3)}, C(kAccent2), S(2.4f));
        Arrow(dl, {c.x, c.y + S(4)}, S(4.5f), 1, C(kAccent2));
        x += S(40);
    }
    const bool locked = E.is == kRunning;
    for (int i = 0; i < kSteps; ++i) {
        const char* lbl = kStepName[i];
        const ImVec2 ts = Measure(kBold, 19, lbl);
        const float num_w = S(26);
        const ImVec2 ta{x, a.y}, tb{x + num_w + ts.x + S(40), a.y + tab_h};
        const bool reachable = i <= E.max_step && !locked && (i != kInstall || E.is != kIdle) && (i != kDone || E.is == kFinished);
        ImGui::SetCursorScreenPos(ta);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", {tb.x - ta.x, tab_h}) && reachable) Go((Step)i);
        const bool hov = ImGui::IsItemHovered() && reachable;
        ImGui::PopID();
        const bool sel = (int)E.step == i;
        const bool done = i < (int)E.step || (i == kInstall && E.is == kFinished);
        const ImVec2 nc{ta.x + S(29), a.y + tab_h * 0.5f};
        if (done && !sel) {
            StatusIcon(dl, nc, S(10), 1, 0.85f);
        } else {
            dl->AddCircle(nc, S(10), C(sel ? kAccent : kWhite, sel ? 1.0f : 0.3f), 24, S(1.6f));
            const std::string n = std::to_string(i + 1);
            const ImVec2 ns = Measure(kBold, 13, n.c_str());
            Text(dl, kBold, 13, {nc.x - ns.x * 0.5f, nc.y - ns.y * 0.5f}, C(kWhite, sel ? 1.0f : 0.5f), n.c_str());
        }
        Text(dl, kBold, 19, {ta.x + S(20) + num_w, ta.y + (tab_h - ts.y) * 0.5f}, C(kWhite, sel ? 1.0f : hov ? 0.9f : (i <= E.max_step ? 0.6f : 0.32f)), lbl);
        if (sel) dl->AddRectFilled({ta.x + S(14), tb.y - S(4)}, {tb.x - S(14), tb.y - S(1)}, C(kAccent), S(2));
        x = tb.x;
    }
    const float isz = S(40);
    ImGui::SetCursorScreenPos({b.x - S(16) - isz, a.y + (tab_h - isz) * 0.5f});
    if (IconButton("##quit", 4, isz) && AppRequestClose()) E.exit = true;
}

bool ConfirmBox(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* title, const char* text, const char* yes, const char* no, bool& open) {
    bool r = false;
    dl->AddRectFilled(a, b, C({0, 0, 0, 1}, 0.55f), S(16));
    const float w = S(560), h = S(220);
    const ImVec2 ca{(a.x + b.x - w) * 0.5f, (a.y + b.y - h) * 0.5f}, cb{ca.x + w, ca.y + h};
    dl->AddRectFilled({ca.x + S(6), ca.y + S(10)}, {cb.x + S(6), cb.y + S(14)}, C({0, 0, 0, 1}, 0.35f), S(16));
    dl->AddRectFilled(ca, cb, C({0.05f, 0.045f, 0.08f, 1.0f}), S(16));
    dl->AddRect(ca, cb, C(kWhite, 0.2f), S(16));
    Text(dl, kBlack, 26, {ca.x + S(28), ca.y + S(24)}, C(kWhite), title);
    Text(dl, kReg, 17, {ca.x + S(28), ca.y + S(66)}, C(kWhite, 0.7f), text, w - S(56));
    const float bw1 = Measure(kBold, 18, yes).x + S(40), bw2 = Measure(kBold, 18, no).x + S(40);
    ImGui::SetCursorScreenPos({cb.x - S(28) - bw1 - S(10) - bw2, cb.y - S(66)});
    if (TextButton(no, kWhite, false)) open = false;
    ImGui::SameLine(0, S(10));
    if (TextButton(yes, kDanger, true)) { r = true; open = false; }
    return r;
}

void StopInstall() {
    if (E.in_components) E.prep.Cancel();
    if (E.run) E.run->Cancel();
}

void Footer(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddLine({a.x + S(16), a.y}, {b.x - S(16), a.y}, C(kWhite, 0.08f));
    const float fh = b.y - a.y, by = a.y + (fh - S(42)) * 0.5f;
    std::string status;
    ImVec4 scol = kWhite;
    float salpha = 0.5f;
    struct Btn {
        const char* t;
        bool filled;
        ImVec4 col;
        bool on;
        int action;
    };
    std::vector<Btn> bs;
    switch (E.step) {
    case kWelcome:
        status = E.bundle ? "Everything needed is included in this setup" + std::string(E.man.light ? " (light build: missing parts are downloaded)" : "")
                          : (E.op.simulate ? "Simulation" : "Developer mode: scripts from " + U8(E.dev_repo));
        bs.push_back({"Start", true, kAccent, true, 1});
        break;
    case kGame: {
        const bool ok = E.chk_done && IdentOk();
        if (E.chk && E.chk->Alive()) status = "Checking…";
        else if (ok && KeysOk()) { status = Fmt("Supported dump: title %s, update %s", kTitleId, E.ident.update.c_str()); scol = kAccent2; salpha = 0.95f; }
        else if (ok) { status = "Choose your prod.keys to continue"; scol = kDanger; salpha = 0.9f; }
        else if (E.ident.has) { status = "This dump cannot be used: " + IdentStateText(E.ident); scol = kDanger; salpha = 0.9f; }
        bs.push_back({"Next", true, kAccent, ok && KeysOk() && E.dest[0], 2});
        bs.push_back({"Back", false, kWhite, true, -1});
        break;
    }
    case kCheck: {
        bool resume = false;
        for (auto& f : E.plan.phases) resume |= f.state == "done";
        const int nb = Blockers();
        if (E.chk && E.chk->Alive()) status = "Checking…";
        else if (nb) { status = Fmt("%d item(s) need your attention: hover them for details", nb); scol = kDanger; salpha = 0.9f; }
        else if (ReadyToInstall()) { status = "Ready to install"; scol = kAccent2; salpha = 0.95f; }
        bs.push_back({resume ? "Resume installation" : "Install", true, kAccent, ReadyToInstall(), 3});
        bs.push_back({"Back", false, kWhite, true, -1});
        break;
    }
    case kInstall:
        if (E.is == kRunning) {
            status = "You can stop at any time: it resumes where it left off.";
            bs.push_back({"Stop", false, kDanger, true, 4});
        } else if (E.is == kStopped) {
            bs.push_back({"Resume", true, kAccent, true, 5});
            bs.push_back({"Back", false, kWhite, true, -1});
        } else if (E.is == kFailed) {
            status = E.error_log.empty() ? "" : "Log: " + E.error_log;
            bs.push_back({"Retry", true, kAccent, true, 5});
            bs.push_back({"Show log", false, kWhite, true, 6});
            bs.push_back({"Back", false, kWhite, true, -1});
        } else if (E.is == kFinished) {
            bs.push_back({"Next", true, kAccent, true, 7});
        }
        break;
    case kDone:
        status = "Game folder: " + std::string(E.dest);
        bs.push_back({"Play", true, kAccent, true, 8});
        bs.push_back({"Open folder", false, kWhite, true, 9});
        bs.push_back({"Close", false, kWhite, true, 10});
        break;
    default: break;
    }
    float x = b.x - S(24);
    int action = 0;
    for (auto& bt : bs) {
        const float bw = std::max(S(120), Measure(kBold, 18, bt.t).x + S(40));
        x -= bw;
        ImGui::SetCursorScreenPos({x, by});
        if (TextButton(bt.t, bt.col, bt.filled, S(120), bt.on)) action = bt.action;
        x -= S(10);
    }
    dl->PushClipRect({a.x, a.y}, {x - S(10), b.y}, true);
    const ImVec2 ts = Measure(kReg, 16, status.c_str());
    Text(dl, kReg, 16, {a.x + S(28), a.y + (fh - ts.y) * 0.5f}, C(scol, salpha), status.c_str());
    dl->PopClipRect();
    switch (action) {
    case -1: if (E.step > kWelcome) Go((Step)(E.step - 1)); break;
    case 1:
        Go(kGame);
        if (E.dump[0] && !E.chk_done && !(E.chk && E.chk->Alive())) RequestCheck(0);
        break;
    case 2: Go(kCheck); break;
    case 3: StartInstall(); break;
    case 4: E.ask_stop = true; break;
    case 5: StartInstall(); break;  // resume/retry: the launcher's state says what is done
    case 6: E.log_open = true; E.log_tab = E.log_file.empty() ? 0 : 1; break;
    case 7: Go(kDone); break;
    case 8: MakeShortcuts(); Play(); break;
    case 9: OpenInExplorer(DestW()); break;
    case 10: MakeShortcuts(); E.exit = true; break;
    default: break;
    }
}

// ---------------------------------------------------------------- screenshot script (--capture)
void Capture(const wchar_t* name) { E.capture_req = name; }

void Script() {
    if (E.op.capture.empty()) return;
    const double t = ImGui::GetTime() - E.script_t;
    const double tc = ImGui::GetTime() - E.t_change;
    auto next = [&] { ++E.script; E.script_t = ImGui::GetTime(); };
    const bool chk_ready = E.chk_done && !(E.chk && E.chk->Alive()) && E.chk_at < 0;
    const wchar_t* pre = E.op.simulate ? L"sim-" : L"auto-";
    auto cap = [&](const wchar_t* n) { Capture((std::wstring(pre) + n).c_str()); };
    static std::wstring hold;
    switch (E.script) {
    case 0: if (t > 1.2 && (E.bundle || E.dev_ready.load())) { hold = std::wstring(pre) + L"01-welcome"; Capture(hold.c_str()); next(); } break;
    case 1: Go(kGame); RequestCheck(0); next(); break;
    case 2:
        if (E.bundle && !ReadyForIdentify() && E.prep.Get().stage == "unpacking" && t > 0.3) { hold = std::wstring(pre) + L"02-preparing"; Capture(hold.c_str()); next(); }
        else if (ReadyForIdentify()) next();
        break;
    case 3: if (chk_ready && E.chk_at < 0 && t > 0.8 && ReadyForIdentify()) { hold = std::wstring(pre) + L"03-your-game"; Capture(hold.c_str()); next(); } break;
    case 4: Go(kCheck); next(); break;
    case 5: if (t > 1.0) { hold = std::wstring(pre) + L"04-check"; Capture(hold.c_str()); next(); } break;
    case 6:
        if (!E.op.simulate && !ReadyToInstall()) { E.exit = true; break; }  // nothing to install: keep the check screenshot
        StartInstall();
        next();
        break;
    case 7:
        if (E.in_components && E.prep.Get().stage == "unpacking" && t > 0.3) { hold = std::wstring(pre) + L"05-setup-files"; Capture(hold.c_str()); next(); }
        else if (!E.in_components && t > 0.5) next();
        break;
    case 8:
        if (E.is == kRunning && !E.in_components && E.cur >= 0 && E.pi.phases[E.cur].state == "running" && E.pi.phases[E.cur].id != "_components" &&
            ImGui::GetTime() - E.pi.phases[E.cur].t0 > 0.6) {
            hold = std::wstring(pre) + L"06-installing";
            Capture(hold.c_str());
            next();
        } else if (E.is != kRunning) next();
        break;
    case 9:
        if (E.op.simulate && E.cur >= 0 && E.pi.phases[E.cur].id == "compile" && E.sub_frac > 0.35f) { E.log_open = true; next(); }
        else if (!E.op.simulate && E.is != kRunning) { Go(kInstall); next(); }
        break;
    case 10:
        if (t > 0.6) {
            hold = std::wstring(pre) + (E.op.simulate ? L"07-log" : L"07-finished");
            Capture(hold.c_str());
            next();
        }
        break;
    case 11: E.log_open = true; E.log_tab = E.op.simulate ? 0 : 1; next(); break;
    case 12:
        if (t > 0.8) {
            hold = std::wstring(pre) + L"08-log";
            Capture(hold.c_str());
            E.script = E.op.simulate ? 13 : 30;
            E.script_t = ImGui::GetTime();
        }
        break;
    // simulation only: stop, resume, done, failure
    case 13: E.log_open = false; E.ask_stop = true; next(); break;
    case 14: if (t > 0.4) { hold = std::wstring(pre) + L"09-stop"; Capture(hold.c_str()); next(); } break;
    case 15: E.ask_stop = false; StopInstall(); next(); break;
    case 16: if (E.is == kStopped && t > 0.5) { hold = std::wstring(pre) + L"10-stopped"; Capture(hold.c_str()); next(); } break;
    case 17: StartInstall(); next(); break;
    case 18: if (E.step == kDone && tc > 1.0) { hold = std::wstring(pre) + L"11-done"; Capture(hold.c_str()); next(); } break;
    case 19: E.op.sim_fail = "assemble"; E.is = kIdle; Go(kCheck); StartInstall(); next(); break;
    case 20: if (E.is == kFailed && t > 0.3) { hold = std::wstring(pre) + L"12-error"; Capture(hold.c_str()); next(); } break;
    case 21: E.script = 30; E.script_t = ImGui::GetTime(); break;
    case 30: if (t > 0.3) E.exit = true; break;
    default: break;
    }
    (void)cap;
}

}  // namespace

// ---------------------------------------------------------------- API
void AppInit(HWND hwnd, const Options& op) {
    E.hwnd = hwnd;
    E.op = op;
    if (!op.data_dir.empty()) {
        MakeDirs(op.data_dir);
        E.cfg.path = Join(op.data_dir, L"PLARecompSetup.ini");
        if (!Exists(E.cfg.path)) {
            HANDLE h = CreateFileW(E.cfg.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                const wchar_t ini[] = L"\xFEFF[Setup]\r\n";
                DWORD w;
                WriteFile(h, ini, (DWORD)(wcslen(ini) * sizeof(wchar_t)), &w, nullptr);
                CloseHandle(h);
            }
        }
    } else {
        E.cfg.Load();
    }
    E.ram = TotalRam();
    E.found = FindKeys();
    auto put = [](char* buf, size_t n, const std::wstring& cli, const wchar_t* key, const std::wstring& def = L"") {
        std::wstring v = !cli.empty() ? cli : E.cfg.Get(key);
        if (v.empty()) v = def;
        Copy(buf, n, U8(v));
    };
    put(E.dump, sizeof E.dump, op.dump, L"dump");
    put(E.keys, sizeof E.keys, op.keys, L"keys");
    put(E.tkeys, sizeof E.tkeys, op.title_keys, L"title_keys");
    put(E.dest, sizeof E.dest, op.dest, L"game_folder", DefaultGameFolder());
    put(E.work, sizeof E.work, op.work, L"work_folder");
    E.jobs = _wtoi(E.cfg.Get(L"jobs", L"0").c_str());
    E.vs_tools = E.cfg.Get(L"compiler", L"portable") == L"vs";
    E.clean = E.cfg.Get(L"clean", L"1") != L"0";
    const std::wstring ea = E.cfg.Get(L"exe_args");
    if (!ea.empty()) E.exe_args = U8(ea);

    // Runtime: developer checkout (--dev-repo or repo= in the settings) or the bundled components.
    std::wstring dev = !op.dev_repo.empty() ? op.dev_repo : E.cfg.Get(L"repo");
    E.man.LoadFromExe(ExePath());
    E.bundle = dev.empty() && E.man.loaded;
    if (!E.bundle && dev.empty()) dev = L"";  // neither: the check page reports it
    E.comp_dir = !op.data_dir.empty() ? Join(op.data_dir, L"components") : E.cfg.Get(L"components_dir", DefaultComponentsDir().c_str());
    E.cache_dir = !op.data_dir.empty() ? Join(op.data_dir, L"downloads") : E.cfg.Get(L"download_cache", DefaultCacheDir().c_str());
    E.url_base = !op.url_base.empty() ? op.url_base : U8(E.cfg.Get(L"url_base", W(E.man.url_base).c_str()));
    if (!E.bundle) {
        const std::wstring py_pref = E.cfg.Get(L"python");
        E.dev_thread = std::thread([dev, py_pref] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            E.dev_repo = dev;
            E.dev_py = DetectSystemPython(py_pref);
            E.dev_ready = true;
            CoUninitialize();
        });
    }
    if (op.capture.empty()) {
        const std::wstring inst = E.cfg.Get(L"installed");
        E.has_installed = !inst.empty() && Exists(Join(inst, L"launcher-manifest.json")) && Exists(Join(inst, L"PLA.exe"));
        E.has_previous = !E.has_installed && E.dest[0] && Exists(Join(WorkW(), L"state.json")) && !Exists(Join(DestW(), L"launcher-manifest.json"));
    }
}

void AppFrame(int w, int h) {
    E.desc_hover = {};
    Update();
    Script();
    if (E.exit) return;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)w, (float)h});
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Background(dl, (float)w, (float)h);
    const float mx = S(36), mt = S(30), mb = S(62);
    const ImVec2 a{mx, mt}, b{(float)w - mx, (float)h - mb};
    dl->AddRectFilled({a.x + S(6), a.y + S(10)}, {b.x + S(6), b.y + S(14)}, C({0, 0, 0, 1}, 0.35f), S(18));
    dl->AddRectFilled(a, b, C(kModal), S(16));
    dl->AddRect(a, b, C(kWhite, 0.2f), S(16), 0, std::max(1.0f, S(1.1f)));
    const float tab_h = S(64), foot_h = S(72);
    Tabs(dl, a, b, tab_h);
    dl->AddLine({a.x, a.y + tab_h}, {b.x, a.y + tab_h}, C(kWhite, 0.1f), std::max(1.0f, S(1.1f)));
    const ImVec2 ca{a.x, a.y + tab_h + 1}, cb{b.x, b.y - foot_h};
    dl->AddRectFilled(ca, {b.x, b.y}, C({0, 0, 0, 1}, 0.28f), S(16), ImDrawFlags_RoundCornersBottom);
    const float k = Ease((float)(ImGui::GetTime() - E.t_change) / 0.22f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.25f + 0.75f * k);
    const ImVec2 off{0, (1.0f - k) * S(14)};
    const ImVec2 pa{ca.x + off.x, ca.y + off.y}, pb{cb.x + off.x, cb.y + off.y};
    switch (E.step) {
    case kWelcome: PageWelcome(dl, pa, pb); break;
    case kGame: PageGame(dl, pa, pb); break;
    case kCheck: PageCheck(dl, pa, pb); break;
    case kInstall: PageInstall(dl, pa, pb); break;
    case kDone: PageDone(dl, pa, pb); break;
    default: break;
    }
    ImGui::PopStyleVar();
    Footer(dl, {a.x, b.y - foot_h}, b);
    HelpBar(dl, w * 0.5f, b.y + S(19));
    ImGui::End();
    if (E.ask_stop || E.ask_exit) {
        ImGui::SetNextWindowPos(a);
        ImGui::SetNextWindowSize({b.x - a.x, b.y - a.y});
        ImGui::SetNextWindowFocus();
        ImGui::Begin("##confirm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImDrawList* cd = ImGui::GetWindowDrawList();
        if (E.ask_stop) {
            if (ConfirmBox(cd, a, b, "Stop the installation?",
                           "Everything done so far is kept. Resuming continues from the current phase (compiling only redoes what is missing).", "Stop",
                           "Keep installing", E.ask_stop))
                StopInstall();
        } else if (ConfirmBox(cd, a, b, "Quit the setup?", "The installation is running. It will be stopped, and next time you open the setup you can continue where it left off.",
                              "Stop and quit", "Keep installing", E.ask_exit)) {
            StopInstall();
            E.exit = true;
        }
        ImGui::End();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
        if (E.ask_stop) E.ask_stop = false;
        else if (E.ask_exit) E.ask_exit = false;
        else if (E.log_open) E.log_open = false;
        else if (E.step > kWelcome && E.step <= kCheck) Go((Step)(E.step - 1));
    }
}

bool AppRequestClose() {
    if (E.is == kRunning) {
        E.ask_exit = true;
        return false;
    }
    return true;
}

bool AppWantsExit() { return E.exit; }

std::wstring AppCaptureRequest() {
    std::wstring r = E.capture_req;
    E.capture_req.clear();
    return r;
}

void AppShutdown() {
    E.prep.Cancel();
    if (E.chk) E.chk->Cancel();
    if (E.run) E.run->Cancel();
    E.chk.reset();
    E.run.reset();
    if (E.dev_thread.joinable()) E.dev_thread.join();
    if (E.size_thread.joinable()) E.size_thread.join();
}

}  // namespace setup
