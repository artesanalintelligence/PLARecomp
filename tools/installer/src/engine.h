// Setup engine: text and system helpers, settings, Python/repo detection (developer mode), the launcher.py
// subprocess (--eventos) with Job Object cancellation, and a phase simulation for UI tests.
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json_min.h"

namespace setup {

// ---- text ----
std::wstring W(const std::string& s);
std::string U8(const std::wstring& s);
std::string Fmt(const char* f, ...);
std::string Elapsed(double sec);    // "45 s", "12:05", "1 h 05 min"
std::string Approx(double sec);     // "~30 s", "~57 min", "~1 h 09 min"
std::string Size(double bytes);     // "8.2 GB", "34 MB"
std::wstring QuoteArg(const std::wstring& a);

// ---- system ----
std::wstring ExeDir();
std::wstring ExePath();
bool Exists(const std::wstring& p);
bool IsDir(const std::wstring& p);
std::wstring Parent(const std::wstring& p);
std::wstring Join(const std::wstring& a, const std::wstring& b);
bool MakeDirs(const std::wstring& p);
bool DiskSpace(const std::wstring& path, uint64_t& free, uint64_t& total);  // walks up to an existing folder
std::wstring VolumeRoot(const std::wstring& path);
uint64_t TotalRam();
std::wstring DefaultGameFolder();  // <fixed drive with most free space>\Games\PLARecomp
std::wstring ReadFileTail(const std::wstring& p, size_t max_bytes);
bool OpenInExplorer(const std::wstring& p);
bool CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args, const std::wstring& cwd,
                    const std::wstring& desc);
std::wstring KnownFolder(int csidl);
std::wstring EnvVar(const wchar_t* name);
// Removes Python/LLVM/CMake/Ninja/MSYS entries from this process' PATH (to simulate a clean machine).
void StripToolPaths();

// ---- settings (PLARecompSetup.ini next to the .exe, or %APPDATA%\PLARecompSetup) ----
struct Settings {
    std::wstring path;
    void Load();
    std::wstring Get(const wchar_t* k, const wchar_t* def = L"") const;
    void Set(const wchar_t* k, const std::wstring& v) const;
};

// ---- Python (developer mode) and scripts ----
struct Python {
    std::vector<std::wstring> argv;  // e.g. {"C:\\Python312\\python.exe"} or {"py", "-3"}
    std::string version;
    bool ok() const { return !argv.empty(); }
};
Python DetectSystemPython(const std::wstring& preferred);
Python ProbePython(const std::vector<std::wstring>& argv);
bool IsScriptsRoot(const std::wstring& root);  // has tools\launcher\launcher.py

// ---- user keys ----
struct KeysFound {
    std::wstring prod, title, where;  // where: "suyu", "yuzu", "Ryujinx", ".switch", "next to the setup"
};
KeysFound FindKeys();
bool KeysFileLooksValid(const std::wstring& p);  // has a header_key line (values are never read)

// ---- what launcher.py reports (via --eventos) ----
struct PhaseUi {
    std::string id, title, cost, state, skip, note, error, log, step;
    std::vector<std::string> steps;
    double minutes = 0, gb = 0, seconds = 0;
    double t0 = 0;
};
struct ReqUi {
    std::string name, detail, phases, help;
    int ok = -1;  // 1 ok, 0 missing, -1 warning
};
struct IdentUi {
    std::string state, build_id, base, update;
    std::vector<std::string> warnings, files, differences;
    bool has = false;
};
struct PlanUi {
    std::vector<PhaseUi> phases;
    double total_min = 0, peak_gb = 0, final_gb = 0;
    int jobs = 0;
    std::string work, dest, toolchain, toolchain_dir;
    bool has = false;
};

// One launcher run (or simulation). Lines are read on another thread; Drain() hands them to the UI thread.
class Run {
public:
    ~Run();
    bool Start(const std::vector<std::wstring>& argv, const std::wstring& cwd, const std::wstring& path_prefix, std::string& err);
    void Simulate(bool install, const std::string& fail_phase, double scale);
    void Cancel();
    bool Alive() const { return alive_.load(); }
    int ExitCode() const { return code_.load(); }
    bool Cancelled() const { return cancelled_.load(); }
    std::vector<std::string> Drain();

private:
    void Push(const std::string& l);
    void ReadOutput();
    void SimThread(bool install, std::string fail, double scale);
    HANDLE proc_ = nullptr, job_ = nullptr, read_ = nullptr;
    std::thread thread_;
    std::mutex mu_;
    std::deque<std::string> lines_;
    std::string rest_;
    std::atomic<bool> alive_{false}, cancelled_{false}, sim_{false};
    std::atomic<int> code_{-1};
};

double Now();  // seconds since start (monotonic)

}  // namespace setup
