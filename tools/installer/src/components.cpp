// Bundled components (see components.h).
// SPDX-License-Identifier: MIT
#include "components.h"

#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "engine.h"
#include "json_min.h"

namespace setup {

namespace {

const char kMagic[8] = {'P', 'L', 'A', 'R', 'S', 'E', 'T', 'U'};
constexpr uint64_t kFooter = 40;

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit File(const std::wstring& p, bool write = false) {
        h = write ? CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr)
                  : CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    ~File() {
        if (ok()) CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    uint64_t size() const {
        LARGE_INTEGER s{};
        GetFileSizeEx(h, &s);
        return (uint64_t)s.QuadPart;
    }
    bool ReadAt(uint64_t off, void* dst, DWORD n) const {
        OVERLAPPED o{};
        o.Offset = (DWORD)off;
        o.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h, dst, n, &got, &o) && got == n;
    }
};

// End of the payload: the file end, or the start of the Authenticode certificate table if the .exe is signed.
uint64_t PayloadEnd(const File& f) {
    const uint64_t size = f.size();
    IMAGE_DOS_HEADER dos{};
    if (!f.ReadAt(0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return size;
    IMAGE_NT_HEADERS64 nt{};
    if (!f.ReadAt((uint64_t)dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return size;
    const IMAGE_DATA_DIRECTORY& sec = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    if (sec.VirtualAddress && sec.Size && sec.VirtualAddress < size) return sec.VirtualAddress;  // file offset for this entry
    return size;
}

std::string Hex(const unsigned char* d, size_t n) {
    static const char* x = "0123456789abcdef";
    std::string r;
    for (size_t i = 0; i < n; ++i) {
        r += x[d[i] >> 4];
        r += x[d[i] & 15];
    }
    return r;
}

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void Add(const void* d, size_t n) { BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0); }
    std::string Hex32() {
        unsigned char out[32];
        BCryptFinishHash(h, out, 32, 0);
        return Hex(out, 32);
    }
};

std::wstring StatePath(const std::wstring& comp_dir, const Component& c) { return Join(Join(comp_dir, L".state"), W(c.id) + L".sha256"); }

std::wstring CachePath(const std::wstring& cache_dir, const Component& c) { return Join(cache_dir, W(c.file)); }

uint64_t FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

}  // namespace

// ---------------------------------------------------------------- manifest
bool Manifest::LoadFromExe(const std::wstring& exe) {
    File f(exe);
    if (!f.ok()) { error = "cannot open the setup file"; return false; }
    const uint64_t end = PayloadEnd(f);
    // Signing may pad: look for the footer in the last few bytes before the certificate table.
    uint64_t foot = 0;
    for (uint64_t back = 0; back <= 16 && end >= kFooter + back; back += 1) {
        unsigned char b[kFooter];
        if (!f.ReadAt(end - kFooter - back, b, (DWORD)kFooter)) break;
        if (!memcmp(b, kMagic, 8) && !memcmp(b + 32, kMagic, 8)) {
            foot = end - kFooter - back;
            break;
        }
    }
    if (!foot) { error = "this setup has no bundled components (developer build)"; return false; }
    unsigned char b[kFooter];
    f.ReadAt(foot, b, (DWORD)kFooter);
    uint64_t ioff, isz;
    memcpy(&ioff, b + 16, 8);
    memcpy(&isz, b + 24, 8);
    if (isz > (16u << 20) || ioff + isz > foot) { error = "damaged component index"; return false; }
    std::string txt((size_t)isz, '\0');
    if (!f.ReadAt(ioff, txt.data(), (DWORD)isz)) { error = "cannot read the component index"; return false; }
    JVal v;
    if (!ParseJson(txt, v)) { error = "damaged component index (JSON)"; return false; }
    release = v["release"].str();
    url_base = v["url_base"].str();
    light = v["light"].tri() == 1;
    for (auto& x : v["components"].a) {
        Component c;
        c.id = x["id"].str();
        c.name = x["name"].str();
        c.version = x["version"].str();
        c.file = x["file"].str();
        c.sha256 = x["sha256"].str();
        c.root = x["root"].str();
        c.license = x["license"].str();
        c.url = x["url"].str();
        c.size = (uint64_t)x["size"].num();
        c.unpacked = (uint64_t)x["unpacked"].num();
        c.embedded = x["offset"].exists();
        c.offset = (uint64_t)x["offset"].num();
        if (c.embedded && c.offset + c.size > ioff) { error = "damaged component index (offsets)"; return false; }
        items.push_back(c);
    }
    loaded = true;
    return true;
}

const Component* Manifest::Find(const std::string& id) const {
    for (auto& c : items)
        if (c.id == id) return &c;
    return nullptr;
}

std::wstring DefaultComponentsDir() { return Join(KnownFolder(CSIDL_LOCAL_APPDATA), L"PLARecompSetup\\components"); }
std::wstring DefaultCacheDir() { return Join(KnownFolder(CSIDL_LOCAL_APPDATA), L"PLARecompSetup\\downloads"); }

bool IsInstalled(const Component& c, const std::wstring& comp_dir) {
    const std::wstring st = StatePath(comp_dir, c);
    std::string got = U8(ReadFileTail(st, 128));
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
    return got == c.sha256 && IsDir(Join(comp_dir, W(c.root)));
}

Avail Availability(const Component& c, const std::wstring& comp_dir, const std::wstring& cache_dir) {
    if (IsInstalled(c, comp_dir)) return Avail::Installed;
    if (c.embedded) return Avail::Embedded;
    if (FileSize(CachePath(cache_dir, c)) == c.size) return Avail::Cached;
    return Avail::Download;
}

std::string Sha256File(const std::wstring& path, uint64_t offset, uint64_t size, std::atomic<uint64_t>* done,
                       const std::atomic<bool>* cancel) {
    File f(path);
    if (!f.ok()) return "";
    Sha256 h;
    std::vector<char> buf(1 << 20);
    uint64_t left = size;
    while (left) {
        if (cancel && cancel->load()) return "";
        const DWORD n = (DWORD)std::min<uint64_t>(left, buf.size());
        if (!f.ReadAt(offset + (size - left), buf.data(), n)) return "";
        h.Add(buf.data(), n);
        left -= n;
        if (done) *done += n;
    }
    return h.Hex32();
}

// ---------------------------------------------------------------- download (WinHTTP)
namespace {

struct Inet {
    HINTERNET h = nullptr;
    explicit Inet(HINTERNET x = nullptr) : h(x) {}
    ~Inet() {
        if (h) WinHttpCloseHandle(h);
    }
};

// One attempt: appends to <part> starting at its current size. Returns true when the body was fully read.
bool DownloadOnce(const std::wstring& url, const std::wstring& part, uint64_t size, std::atomic<uint64_t>& done,
                  const std::atomic<bool>& cancel, std::string& err, bool& fatal) {
    URL_COMPONENTS uc{sizeof uc};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "invalid URL: " + U8(url); fatal = true; return false; }
    Inet ses(WinHttpOpen(L"PLARecompSetup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h) ses.h = WinHttpOpen(L"PLARecompSetup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses.h) { err = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(ses.h, 15000, 15000, 30000, 60000);
    Inet con(WinHttpConnect(ses.h, host, uc.nPort, 0));
    if (!con.h) { err = Fmt("cannot connect (%lu)", GetLastError()); return false; }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    Inet req(WinHttpOpenRequest(con.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) { err = "WinHttpOpenRequest failed"; return false; }
    const uint64_t have = FileSize(part);
    if (have >= size && size) return true;
    std::wstring range;
    if (have) range = L"Range: bytes=" + std::to_wstring(have) + L"-\r\n";
    if (!WinHttpSendRequest(req.h, range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = Fmt("network error (%lu)", GetLastError());
        return false;
    }
    DWORD status = 0, sl = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
    bool append = false;
    if (status == 206) append = true;
    else if (status == 200) append = false;  // server ignored Range: start over
    else if (status == 416 && have) { DeleteFileW(part.c_str()); err = "HTTP 416, restarting"; return false; }
    else {
        err = Fmt("HTTP %lu", status);
        fatal = status == 404 || status == 403 || status == 410;
        return false;
    }
    HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) { err = "cannot write " + U8(part); fatal = true; return false; }
    LARGE_INTEGER z{};
    SetFilePointerEx(out, z, nullptr, FILE_END);
    done = append ? have : 0;
    std::vector<char> buf(1 << 16);
    bool ok = true;
    for (;;) {
        if (cancel.load()) { err = "cancelled"; ok = false; fatal = true; break; }
        DWORD n = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &n)) { err = Fmt("network error while reading (%lu)", GetLastError()); ok = false; break; }
        if (!n) break;
        DWORD w;
        if (!WriteFile(out, buf.data(), n, &w, nullptr) || w != n) { err = "disk write error"; ok = false; fatal = true; break; }
        done += n;
    }
    CloseHandle(out);
    return ok && FileSize(part) == size;
}

}  // namespace

bool Download(const std::wstring& url, const std::wstring& dest, uint64_t size, const std::string& sha256, std::atomic<uint64_t>& done,
              const std::atomic<bool>& cancel, std::string& err) {
    MakeDirs(Parent(dest));
    const std::wstring part = dest + L".part";
    for (int attempt = 0; attempt < 6; ++attempt) {
        bool fatal = false;
        if (attempt) {
            for (int i = 0; i < (1 << std::min(attempt, 4)) * 10 && !cancel.load(); ++i) Sleep(100);  // 2, 4, 8, 16 s
        }
        if (cancel.load()) { err = "cancelled"; return false; }
        if (DownloadOnce(url, part, size, done, cancel, err, fatal)) {
            if (Sha256File(part, 0, size, nullptr, nullptr) != sha256) {
                DeleteFileW(part.c_str());
                err = "downloaded file is damaged (SHA-256 mismatch)";
                continue;
            }
            DeleteFileW(dest.c_str());
            if (!MoveFileW(part.c_str(), dest.c_str())) { err = "cannot rename the downloaded file"; return false; }
            return true;
        }
        if (fatal) return false;
    }
    return false;
}

// ---------------------------------------------------------------- preparation
Preparer::~Preparer() {
    Cancel();
    if (thread_.joinable()) thread_.join();
}

void Preparer::Start(const Manifest& m, const std::vector<std::string>& ids, const std::wstring& comp_dir, const std::wstring& cache_dir,
                     const std::string& url_base) {
    if (thread_.joinable()) {
        Cancel();
        thread_.join();
    }
    cancel_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> g(mu_);
        snap_ = Snapshot{};
        snap_.running = true;
        snap_.count = (int)ids.size();
    }
    thread_ = std::thread([this, m, ids, comp_dir, cache_dir, url_base] { Work(m, ids, comp_dir, cache_dir, url_base); });
}

void Preparer::Cancel() { cancel_ = true; }

void Preparer::SetStage(const std::string& name, const char* stage, uint64_t total) {
    std::lock_guard<std::mutex> g(mu_);
    snap_.name = name;
    snap_.stage = stage;
    snap_.total = total;
    done_ = 0;
    t_stage_ = Now();
}

Preparer::Snapshot Preparer::Get() {
    std::lock_guard<std::mutex> g(mu_);
    Snapshot s = snap_;
    s.done = done_.load();
    const double dt = Now() - t_stage_;
    s.speed = dt > 0.5 ? s.done / dt : 0;
    return s;
}

void Preparer::Work(Manifest m, std::vector<std::string> ids, std::wstring comp_dir, std::wstring cache_dir, std::string url_base) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = true;
    for (size_t i = 0; i < ids.size() && ok; ++i) {
        {
            std::lock_guard<std::mutex> g(mu_);
            snap_.index = (int)i;
        }
        const Component* c = m.Find(ids[i]);
        if (!c) {
            std::lock_guard<std::mutex> g(mu_);
            snap_.error = "component not in this setup: " + ids[i];
            ok = false;
            break;
        }
        ok = Prepare(*c, comp_dir, cache_dir, url_base);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        snap_.running = false;
        snap_.ok = ok;
        snap_.failed = !ok;
        snap_.stage = ok ? "done" : snap_.stage;
        if (!ok && snap_.error.empty()) snap_.error = cancel_.load() ? "cancelled" : "failed";
    }
    running_ = false;
    CoUninitialize();
}

bool Preparer::Prepare(const Component& c, const std::wstring& comp_dir, const std::wstring& cache_dir, const std::string& url_base) {
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> g(mu_);
        snap_.error = c.name + ": " + e;
        return false;
    };
    if (IsInstalled(c, comp_dir)) return true;
    // 1. Source: the payload of this .exe, the download cache, or a download.
    std::wstring src;
    uint64_t off = 0;
    if (c.embedded) {
        src = ExePath();
        off = c.offset;
    } else {
        src = CachePath(cache_dir, c);
        if (FileSize(src) != c.size) {
            std::string base = url_base;
            if (!base.empty() && base.back() != '/') base += '/';
            const std::string url = !c.url.empty() ? c.url : base + c.file;
            if (url.find("://") == std::string::npos || base.find("PENDING") != std::string::npos)
                return fail("not bundled in this setup and no download address is configured");
            SetStage(c.name, "downloading", c.size);
            std::string err;
            if (!Download(W(url), src, c.size, c.sha256, done_, cancel_, err)) return fail("download failed: " + err);
        }
    }
    // 2. Verify.
    SetStage(c.name, "verifying", c.size);
    if (Sha256File(src, off, c.size, &done_, &cancel_) != c.sha256) {
        if (cancel_.load()) return fail("cancelled");
        if (!c.embedded) DeleteFileW(src.c_str());
        return fail(c.embedded ? "the setup file is damaged (SHA-256 mismatch); download it again" : "cached file is damaged; it will be downloaded again");
    }
    // 3. Unpack: stream the archive into %SystemRoot%\System32\tar.exe (xz support is built into Windows' bsdtar).
    SetStage(c.name, "unpacking", c.size);
    if (!MakeDirs(comp_dir)) return fail("cannot create " + U8(comp_dir));
    DeleteFileW(StatePath(comp_dir, c).c_str());
    const std::wstring tar = Join(KnownFolder(CSIDL_SYSTEM), L"tar.exe");
    std::wstring cmd = QuoteArg(tar) + L" -xf - -C " + QuoteArg(comp_dir);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return fail("CreatePipe failed");
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        CloseHandle(job);
        return fail(Fmt("cannot run tar.exe (error %lu); Windows 10 1803 or later is required", GetLastError()));
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    bool ok = true;
    {
        File f(src);
        std::vector<char> buf(1 << 20);
        uint64_t left = c.size;
        while (left && ok) {
            if (cancel_.load()) { ok = false; break; }
            const DWORD n = (DWORD)std::min<uint64_t>(left, buf.size());
            if (!f.ReadAt(off + (c.size - left), buf.data(), n)) { ok = false; break; }
            DWORD w = 0;
            if (!WriteFile(wr, buf.data(), n, &w, nullptr) || w != n) { ok = false; break; }
            left -= n;
            done_ += n;
        }
    }
    CloseHandle(wr);
    if (!ok) TerminateJobObject(job, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    if (!ok) return fail(cancel_.load() ? "cancelled" : "unpacking was interrupted");
    if (code != 0) return fail(Fmt("tar.exe failed (exit %lu)", code));
    if (!IsDir(Join(comp_dir, W(c.root)))) return fail("unpacked files are missing");
    MakeDirs(Join(comp_dir, L".state"));
    HANDLE st = CreateFileW(StatePath(comp_dir, c).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (st != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(st, c.sha256.data(), (DWORD)c.sha256.size(), &w, nullptr);
        CloseHandle(st);
    }
    return true;
}

}  // namespace setup
