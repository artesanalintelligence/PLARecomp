// Bundled components (Python, scripts, platform, portable toolchain).
//
// A release .exe carries them as a payload appended after the PE image:
//   [PE image][blob 0][blob 1]...[index JSON][footer]
//   footer (40 bytes) = "PLARSETU" | u32 version | u32 0 | u64 index offset | u64 index size | "PLARSETU"
// Each blob is a .tar.xz (LZMA2 + x86 BCJ) with a single top folder. The index lists id, name, version, file name,
// size, SHA-256, top folder, unpacked size and the blob offset. A "light" build has the index but no blobs: the
// components are then taken from the download cache or fetched from url_base + file (fallback only).
// If the .exe is Authenticode-signed, the certificate table follows the footer; the reader skips it.
// Blobs are verified (SHA-256) and then streamed into Windows' own tar.exe (libarchive, xz support built in).
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace setup {

struct Component {
    std::string id, name, version, file, sha256, root, license, url;
    uint64_t size = 0, unpacked = 0, offset = 0;
    bool embedded = false;
};

struct Manifest {
    std::string release, url_base, error;
    bool light = false, loaded = false;
    std::vector<Component> items;
    bool LoadFromExe(const std::wstring& exe);
    const Component* Find(const std::string& id) const;
};

enum class Avail { Installed, Embedded, Cached, Download };
Avail Availability(const Component& c, const std::wstring& comp_dir, const std::wstring& cache_dir);
bool IsInstalled(const Component& c, const std::wstring& comp_dir);
std::wstring DefaultComponentsDir();  // %LOCALAPPDATA%\PLARecompSetup\components
std::wstring DefaultCacheDir();       // %LOCALAPPDATA%\PLARecompSetup\downloads

// Makes a list of components ready (verify → download if needed → unpack) on a worker thread.
class Preparer {
public:
    struct Snapshot {
        std::string name, stage;  // stage: "verifying", "downloading", "unpacking", "done"
        uint64_t done = 0, total = 0;
        double speed = 0;  // bytes/s (downloads)
        int index = 0, count = 0;
        bool running = false, ok = false, failed = false;
        std::string error;
    };
    ~Preparer();
    void Start(const Manifest& m, const std::vector<std::string>& ids, const std::wstring& comp_dir, const std::wstring& cache_dir,
               const std::string& url_base);
    void Cancel();
    Snapshot Get();
    bool Running() const { return running_.load(); }

private:
    void Work(Manifest m, std::vector<std::string> ids, std::wstring comp_dir, std::wstring cache_dir, std::string url_base);
    bool Prepare(const Component& c, const std::wstring& comp_dir, const std::wstring& cache_dir, const std::string& url_base);
    void SetStage(const std::string& name, const char* stage, uint64_t total);
    std::thread thread_;
    std::mutex mu_;
    Snapshot snap_;
    std::atomic<bool> running_{false}, cancel_{false};
    std::atomic<uint64_t> done_{0};
    double t_stage_ = 0;
};

// HTTP(S) download with resume (Range on <dest>.part), retries and SHA-256 check. WinHTTP only.
bool Download(const std::wstring& url, const std::wstring& dest, uint64_t size, const std::string& sha256, std::atomic<uint64_t>& done,
              const std::atomic<bool>& cancel, std::string& err);
std::string Sha256File(const std::wstring& path, uint64_t offset, uint64_t size, std::atomic<uint64_t>* done,
                       const std::atomic<bool>* cancel);

}  // namespace setup
