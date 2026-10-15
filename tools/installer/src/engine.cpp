// Setup engine (see engine.h).
// SPDX-License-Identifier: MIT
#include "engine.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <set>

namespace setup {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- text
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}

std::string U8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::string Fmt(const char* f, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

std::string Elapsed(double sec) {
    if (sec < 0) sec = 0;
    const int s = (int)(sec + 0.5);
    if (s < 60) return Fmt("%d s", s);
    if (s < 3600) return Fmt("%d:%02d", s / 60, s % 60);
    return Fmt("%d h %02d min", s / 3600, (s % 3600) / 60);
}

std::string Approx(double sec) {
    if (sec < 60) return Fmt("~%d s", std::max(5, (int)(sec / 5 + 0.5) * 5));
    const int m = (int)(sec / 60 + 0.5);
    if (m < 60) return Fmt("~%d min", m);
    return Fmt("~%d h %02d min", m / 60, m % 60);
}

std::string Size(double b) {
    const double gb = b / 1073741824.0, mb = b / 1048576.0;
    if (gb >= 10) return Fmt("%.0f GB", gb);
    if (gb >= 1) return Fmt("%.1f GB", gb);
    if (mb >= 1) return Fmt("%.0f MB", mb);
    return Fmt("%.0f KB", b / 1024.0);
}

std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += c; bs = 0; continue; }
        r.append(bs, L'\\');
        bs = 0;
        r += c;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    static LARGE_INTEGER t0 = [] { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

// ---------------------------------------------------------------- system
std::wstring ExePath() {
    wchar_t b[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, b, (DWORD)std::size(b));
    return std::wstring(b, n);
}

std::wstring ExeDir() { return Parent(ExePath()); }

bool Exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDir(const std::wstring& p) {
    const DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Parent(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    if (k == std::wstring::npos) return L"";
    if (k == 2 && p.size() > 1 && p[1] == L':') return p.substr(0, 3);
    return p.substr(0, k);
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool MakeDirs(const std::wstring& p) {
    if (p.empty() || IsDir(p)) return true;
    const std::wstring up = Parent(p);
    if (!up.empty() && up != p) MakeDirs(up);
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring VolumeRoot(const std::wstring& path) {
    wchar_t b[MAX_PATH];
    if (!path.empty() && GetVolumePathNameW(path.c_str(), b, MAX_PATH)) return b;
    return path.size() >= 3 ? path.substr(0, 3) : path;
}

bool DiskSpace(const std::wstring& path, uint64_t& free, uint64_t& total) {
    std::wstring p = path;
    while (!p.empty() && !IsDir(p)) {
        const std::wstring q = Parent(p);
        if (q == p) break;
        p = q;
    }
    if (p.empty()) p = VolumeRoot(path);
    ULARGE_INTEGER l{}, t{};
    if (!GetDiskFreeSpaceExW(p.c_str(), &l, &t, nullptr)) return false;
    free = l.QuadPart;
    total = t.QuadPart;
    return true;
}

uint64_t TotalRam() {
    MEMORYSTATUSEX m{sizeof m};
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
}

std::wstring DefaultGameFolder() {
    wchar_t best = L'C';
    uint64_t max = 0;
    const DWORD u = GetLogicalDrives();
    for (int i = 2; i < 26; ++i) {
        if (!(u & (1u << i))) continue;
        const wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER l{};
        if (GetDiskFreeSpaceExW(root, &l, nullptr, nullptr) && l.QuadPart > max) {
            max = l.QuadPart;
            best = root[0];
        }
    }
    return std::wstring(1, best) + L":\\Games\\PLARecomp";
}

std::wstring ReadFileTail(const std::wstring& p, size_t max_bytes) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    const int64_t from = std::max<int64_t>(0, sz.QuadPart - (int64_t)max_bytes);
    LARGE_INTEGER pos;
    pos.QuadPart = from;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    std::string buf((size_t)(sz.QuadPart - from), '\0');
    DWORD got = 0;
    if (!buf.empty()) ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(h);
    buf.resize(got);
    if (from > 0) {
        const size_t k = buf.find('\n');
        if (k != std::string::npos) buf.erase(0, k + 1);
    }
    return W(buf);
}

bool OpenInExplorer(const std::wstring& p) { return (INT_PTR)ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32; }

std::wstring KnownFolder(int csidl) {
    wchar_t b[MAX_PATH];
    return SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, b)) ? b : L"";
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t b[32768];
    const DWORD n = GetEnvironmentVariableW(name, b, (DWORD)std::size(b));
    return n && n < std::size(b) ? std::wstring(b, n) : L"";
}

void StripToolPaths() {
    const std::wstring path = EnvVar(L"PATH");
    std::wstring out;
    size_t a = 0;
    while (a <= path.size()) {
        size_t b = path.find(L';', a);
        if (b == std::wstring::npos) b = path.size();
        std::wstring e = path.substr(a, b - a), low = e;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        const bool tool = low.find(L"python") != std::wstring::npos || low.find(L"llvm") != std::wstring::npos ||
                          low.find(L"cmake") != std::wstring::npos || low.find(L"ninja") != std::wstring::npos ||
                          low.find(L"msys") != std::wstring::npos || low.find(L"devkitpro") != std::wstring::npos ||
                          low.find(L"windowsapps") != std::wstring::npos || low.find(L"visual studio") != std::wstring::npos;
        if (!e.empty() && !tool) out += (out.empty() ? L"" : L";") + e;
        a = b + 1;
    }
    SetEnvironmentVariableW(L"PATH", out.c_str());
}

bool CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args, const std::wstring& cwd,
                    const std::wstring& desc) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetWorkingDirectory(cwd.c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    ComPtr<IPersistFile> pf;
    if (FAILED(sl.As(&pf))) return false;
    MakeDirs(Parent(lnk));
    return SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
}

// ---------------------------------------------------------------- settings
void Settings::Load() {
    path = Join(ExeDir(), L"PLARecompSetup.ini");
    // Only read next to the .exe if it already exists there (developer builds write it); otherwise %APPDATA%.
    if (!Exists(path)) {
        const std::wstring d = Join(KnownFolder(CSIDL_APPDATA), L"PLARecompSetup");
        MakeDirs(d);
        path = Join(d, L"PLARecompSetup.ini");
    }
    if (!Exists(path)) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            // UTF-16 with BOM so WritePrivateProfileString keeps non-ASCII paths.
            const wchar_t ini[] = L"\xFEFF; Setup settings (written automatically).\r\n[Setup]\r\n";
            DWORD w;
            WriteFile(h, ini, (DWORD)(wcslen(ini) * sizeof(wchar_t)), &w, nullptr);
            CloseHandle(h);
        }
    }
}

std::wstring Settings::Get(const wchar_t* k, const wchar_t* def) const {
    wchar_t b[4096];
    GetPrivateProfileStringW(L"Setup", k, def, b, (DWORD)std::size(b), path.c_str());
    return b;
}

void Settings::Set(const wchar_t* k, const std::wstring& v) const { WritePrivateProfileStringW(L"Setup", k, v.c_str(), path.c_str()); }

// ---------------------------------------------------------------- Python (developer mode)
namespace {

std::string Output(const std::vector<std::wstring>& argv, DWORD ms = 8000) {
    std::wstring cmd;
    for (auto& a : argv) cmd += (cmd.empty() ? L"" : L" ") + QuoteArg(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::string out;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        char b[512];
        DWORD n;
        while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) out.append(b, n);
        if (WaitForSingleObject(pi.hProcess, ms) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return out;
}

}  // namespace

Python ProbePython(const std::vector<std::wstring>& argv) {
    Python p;
    if (argv.empty() || argv[0].empty()) return p;
    std::wstring low = argv[0];
    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
    if (low.find(L"windowsapps") != std::wstring::npos) return p;  // Microsoft Store alias: opens the Store
    auto v = argv;
    v.push_back(L"-c");
    v.push_back(L"import sys;print('%d.%d.%d'%sys.version_info[:3])");
    std::string s = Output(v);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    int a = 0, b = 0, c = 0;
    if (std::sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c) == 3 && (a > 3 || (a == 3 && b >= 10))) {
        p.argv = argv;
        p.version = s;
    }
    return p;
}

Python DetectSystemPython(const std::wstring& preferred) {
    std::vector<std::vector<std::wstring>> c;
    if (!preferred.empty()) c.push_back({preferred});
    wchar_t b[MAX_PATH];
    if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, b, nullptr)) c.push_back({b, L"-3"});
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, b, nullptr)) c.push_back({b});
    for (auto& x : c) {
        Python p = ProbePython(x);
        if (p.ok()) return p;
    }
    return {};
}

bool IsScriptsRoot(const std::wstring& root) { return !root.empty() && Exists(Join(root, L"tools\\launcher\\launcher.py")); }

// ---------------------------------------------------------------- keys
bool KeysFileLooksValid(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string buf(256 * 1024, '\0');
    DWORD n = 0;
    ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr);
    CloseHandle(h);
    buf.resize(n);
    // Only checks that a "header_key = ..." line exists; key values are never parsed or shown.
    size_t a = 0;
    while (a < buf.size()) {
        size_t b = buf.find('\n', a);
        if (b == std::string::npos) b = buf.size();
        std::string l = buf.substr(a, b - a);
        const size_t s = l.find_first_not_of(" \t");
        if (s != std::string::npos && l.compare(s, 10, "header_key") == 0 && l.find('=') != std::string::npos) return true;
        a = b + 1;
    }
    return false;
}

KeysFound FindKeys() {
    const std::wstring appdata = KnownFolder(CSIDL_APPDATA), home = KnownFolder(CSIDL_PROFILE);
    struct Cand {
        std::wstring dir;
        const wchar_t* who;
    };
    std::vector<Cand> c;
    for (const wchar_t* emu : {L"suyu", L"yuzu", L"sudachi", L"citron", L"eden", L"torzu"})
        c.push_back({Join(Join(appdata, emu), L"keys"), emu});
    c.push_back({Join(appdata, L"Ryujinx\\system"), L"Ryujinx"});
    c.push_back({Join(home, L".switch"), L".switch"});
    c.push_back({ExeDir(), L"next to the setup"});
    for (auto& x : c) {
        const std::wstring prod = Join(x.dir, L"prod.keys");
        if (Exists(prod) && KeysFileLooksValid(prod)) {
            KeysFound k;
            k.prod = prod;
            const std::wstring title = Join(x.dir, L"title.keys");
            if (Exists(title)) k.title = title;
            k.where = x.who;
            return k;
        }
    }
    return {};
}

// ---------------------------------------------------------------- subprocess
Run::~Run() {
    Cancel();
    if (thread_.joinable()) thread_.join();
    if (proc_) CloseHandle(proc_);
    if (job_) CloseHandle(job_);
}

void Run::Push(const std::string& l) {
    std::lock_guard<std::mutex> g(mu_);
    lines_.push_back(l);
}

std::vector<std::string> Run::Drain() {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<std::string> r(lines_.begin(), lines_.end());
    lines_.clear();
    return r;
}

bool Run::Start(const std::vector<std::wstring>& argv, const std::wstring& cwd, const std::wstring& path_prefix, std::string& err) {
    std::wstring cmd;
    for (auto& a : argv) cmd += (cmd.empty() ? L"" : L" ") + QuoteArg(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE wr;
    if (!CreatePipe(&read_, &wr, &sa, 1 << 16)) { err = "CreatePipe failed"; return false; }
    SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);
    // Job Object: Cancel() ends the whole tree (python, powershell, the compiler, the exporter...).
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    if (!path_prefix.empty()) {
        // Bundled Python first: the scripts call "python" (export-pla.ps1, build-modules.ps1).
        SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
        const std::wstring path = EnvVar(L"PATH");
        if (path.rfind(path_prefix + L";", 0) != 0) SetEnvironmentVariableW(L"PATH", (path_prefix + L";" + path).c_str());
    }
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        err = Fmt("could not start (error %lu): ", GetLastError()) + U8(cmd);
        CloseHandle(read_);
        read_ = nullptr;
        return false;
    }
    AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    proc_ = pi.hProcess;
    alive_ = true;
    Push("$ " + U8(cmd));
    thread_ = std::thread([this] { ReadOutput(); });
    return true;
}

void Run::ReadOutput() {
    char b[4096];
    DWORD n;
    while (ReadFile(read_, b, sizeof b, &n, nullptr) && n) {
        rest_.append(b, n);
        size_t k;
        while ((k = rest_.find('\n')) != std::string::npos) {
            std::string l = rest_.substr(0, k);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            Push(l);
            rest_.erase(0, k + 1);
        }
    }
    if (!rest_.empty()) Push(rest_);
    WaitForSingleObject(proc_, INFINITE);
    DWORD c = 1;
    GetExitCodeProcess(proc_, &c);
    CloseHandle(read_);
    read_ = nullptr;
    code_ = (int)c;
    alive_ = false;
}

void Run::Cancel() {
    if (!alive_.load()) return;
    cancelled_ = true;
    if (job_ && !sim_.load()) TerminateJobObject(job_, 1);
}

// ---------------------------------------------------------------- simulation
namespace {
struct SimPhase {
    const char* id;
    const char* cost;
    double minutes, gb, sec;
    const char* steps[3];
};
// Simulated NSZ input with the headless exporter: the "base" and "suyu" phases are skipped, so not listed.
const SimPhase kSim[] = {
    {"prepare", "cheap", 0.5, 0.2, 1.0, {"decompress the update (NSZ)", nullptr}},
    {"extract", "cheap", 0.5, 0.25, 1.2, {"ExeFS, main.elf and target.json", "compare SHA-256 and build ids", nullptr}},
    {"roots", "medium", 3, 0.1, 1.6, {"code targets", "main.roots", nullptr}},
    {"bake", "cheap", 0.2, 0.07, 0.8, {"check original bytes", "write patched main", nullptr}},
    {"export", "heavy", 5, 10.8, 3.0, {"AOT export (headless)", "RomFS with the update applied", "baked patches present in the C (43)"}},
    {"compile", "heavy", 56.7, 7.0, 9.0, {"portable toolchain, 3 jobs", nullptr}},
    {"assemble", "medium", 2, 2.1, 1.8, {"game folder base", "nnshim mod.json", "nvnvk mod.json"}},
    {"verify", "cheap", 0.2, 0, 0.6, {"check files and write the manifest", nullptr}},
};
std::set<std::string> g_sim_done;  // survives between runs: lets the UI test "Resume"
std::mutex g_sim_mu;

std::string JStr(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    return r + "\"";
}
}  // namespace

void Run::Simulate(bool install, const std::string& fail, double scale) {
    sim_ = true;
    alive_ = true;
    thread_ = std::thread([this, install, fail, scale] { SimThread(install, fail, scale); });
}

void Run::SimThread(bool install, std::string fail, double scale) {
    auto sleep = [&](double s) {
        const double end = Now() + s * scale;
        while (Now() < end) {
            if (cancelled_.load()) return false;
            Sleep(15);
        }
        return true;
    };
    {
        std::string f;
        double tmin = 0, tgb = 0;
        std::lock_guard<std::mutex> g(g_sim_mu);
        for (const SimPhase& s : kSim) {
            std::string steps;
            for (const char* p : s.steps)
                if (p) steps += (steps.empty() ? "" : ",") + JStr(p);
            f += (f.empty() ? "" : ",") +
                 Fmt("{\"id\":\"%s\",\"title\":\"%s\",\"cost\":\"%s\",\"minutes\":%g,\"gb\":%g,\"state\":\"%s\",\"skip\":\"\",\"steps\":[%s],\"note\":\"\"}",
                     s.id, s.id, s.cost, s.minutes, s.gb, g_sim_done.count(s.id) ? "done" : "pending", steps.c_str());
            tmin += s.minutes;
            tgb += s.gb;
        }
        Push("== simulation");
        Push("@@ {\"ev\":\"ident\",\"state\":\"supported\",\"warnings\":[],\"files\":[{\"path\":\"(sim)\\\\a.nsz\",\"format\":\"nsz\",\"size\":2362232012},"
             "{\"path\":\"(sim)\\\\b.nsz\",\"format\":\"nsz\",\"size\":52428800}],\"base\":{\"version\":0,\"display\":\"1.0.0\"},"
             "\"update\":{\"version\":262144,\"display\":\"1.1.1\"},\"differences\":[],\"build_id\":\"AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E\"}");
        Push("@@ {\"ev\":\"req\",\"name\":\"RAM\",\"ok\":true,\"detail\":\"32 GB\",\"phases\":\"compile\",\"help\":\"\"}");
        Push("@@ {\"ev\":\"plan\",\"phases\":[" + f + "]," +
             Fmt("\"total_min\":%.1f,\"peak_gb\":%.1f,\"final_gb\":8.2,\"work\":\"(sim)\",\"dest\":\"(sim)\",\"jobs\":3,\"toolchain\":\"portable\",\"toolchain_dir\":\"\"}",
                 tmin, tgb));
    }
    if (!install) {
        Push("@@ {\"ev\":\"end\",\"rc\":0,\"dry_run\":true}");
        code_ = 0;
        alive_ = false;
        return;
    }
    int rc = 0;
    for (const SimPhase& s : kSim) {
        {
            std::lock_guard<std::mutex> g(g_sim_mu);
            if (g_sim_done.count(s.id)) {
                Push(Fmt("@@ {\"ev\":\"phase\",\"id\":\"%s\",\"state\":\"done\",\"previous\":true}", s.id));
                continue;
            }
        }
        Push(Fmt("[%s] (%s)", s.id, s.cost));
        Push(Fmt("@@ {\"ev\":\"phase\",\"id\":\"%s\",\"state\":\"running\",\"log\":\"\"}", s.id));
        const double t0 = Now();
        bool ok = true;
        int np = 0;
        for (const char* p : s.steps) np += p ? 1 : 0;
        for (int i = 0; i < np && ok; ++i) {
            Push(Fmt("  - %s", s.steps[i]));
            Push("@@ {\"ev\":\"step\",\"desc\":" + JStr(s.steps[i]) + ",\"log\":\"\"}");
            if (std::string(s.id) == "compile") {
                const int total = 412;
                for (int k = 1; k <= total && ok; k += 3) {
                    Push(Fmt("    [%d/%d] src/recompiled_image_%d.c (simulated)", k, total, k));
                    ok = sleep(s.sec / (total / 3.0));
                }
            } else {
                for (int k = 0; k < 4 && ok; ++k) {
                    Push(Fmt("    (simulated) %s: %d %%", s.steps[i], (k + 1) * 25));
                    ok = sleep(s.sec / np / 4);
                }
            }
            if (ok && fail == s.id) {
                Push(Fmt("ERROR in [%s]: '%s' exited with code 1 (simulated)", s.id, s.steps[i]));
                Push(Fmt("@@ {\"ev\":\"phase\",\"id\":\"%s\",\"state\":\"failed\",\"error\":%s,\"log\":\"\"}", s.id,
                         JStr(Fmt("'%s' exited with code 1 (simulated)", s.steps[i])).c_str()));
                rc = 1;
                ok = false;
            }
        }
        if (cancelled_.load()) { rc = 1; break; }
        if (rc) break;
        {
            std::lock_guard<std::mutex> g(g_sim_mu);
            g_sim_done.insert(s.id);
        }
        Push(Fmt("@@ {\"ev\":\"phase\",\"id\":\"%s\",\"state\":\"done\",\"seconds\":%.1f}", s.id, Now() - t0));
        Push(Fmt("  ok in %.0f s", Now() - t0));
    }
    if (!cancelled_.load()) {
        if (rc == 0) {
            std::lock_guard<std::mutex> g(g_sim_mu);
            g_sim_done.clear();
        }
        Push(Fmt("@@ {\"ev\":\"end\",\"rc\":%d,\"dest\":\"(sim)\",\"exe\":\"\",\"exe_args\":\"--nvnvk\"}", rc));
    }
    code_ = cancelled_.load() ? 1 : rc;
    alive_ = false;
}

}  // namespace setup
