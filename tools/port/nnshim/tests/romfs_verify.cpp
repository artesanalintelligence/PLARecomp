// Verifies the romfs.bin reader against the extracted folder (without the game).
//
//   nnshim_romfs_verify <romfs.bin> <romfs folder> [--sample N] [--random-reads N]
//
// 1. Index: same set of files (exact path) and same sizes; same directories.
// 2. Bytes: every file (or 1 in N with --sample N) read whole with romfs::Image and from the
//    folder, compared with memcmp, plus a 64-bit FNV-1a hash of everything read on each side.
// 3. Shim API (nnshim::fs with romfs_bin): OpenDirectory of every directory against the
//    folder (names, types, sizes and order) and random partial reads (random offset and
//    size, also past the end) against fread from the folder.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "nnshim/fs.hpp"
#include "nnshim/romfs.hpp"

namespace stdfs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double Secs(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

std::FILE* Open(const stdfs::path& p) {
#ifdef _WIN32
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(p.c_str(), "rb");
#endif
}

int g_fail = 0;
void Fail(const char* fmt, const std::string& a, unsigned long long x = 0, unsigned long long y = 0) {
    if (++g_fail <= 30) {
        std::printf("  MISMATCH: ");
        std::printf(fmt, a.c_str(), x, y);
        std::printf("\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <romfs.bin> <folder> [--sample N] [--random-reads N]\n", argv[0]);
        return 2;
    }
    const std::string bin = argv[1];
    const stdfs::path dir = stdfs::u8path(argv[2]);
    int sample = 1, random_reads = 20000;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--sample")) sample = std::max(1, std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--random-reads")) random_reads = std::atoi(argv[i + 1]);
    }

    // ---- 1. index
    auto t0 = Clock::now();
    nnshim::romfs::Image img;
    std::string err;
    if (!img.Open(bin, &err)) {
        std::printf("could not open %s: %s\n", bin.c_str(), err.c_str());
        return 1;
    }
    std::printf("image: %zu files, %zu directories, %.2f GB; index in %.1f ms\n", img.FileCount(), img.DirCount(),
                img.ImageSize() / 1e9, Secs(t0) * 1000);
    std::map<std::string, nnshim::romfs::FileInfo> files;
    img.ForEachFile([&](const std::string& p, const nnshim::romfs::FileInfo& fi) { files[p] = fi; });

    std::map<std::string, uint64_t> host_files;
    std::set<std::string> host_dirs;
    uint64_t host_bytes = 0;
    for (auto it = stdfs::recursive_directory_iterator(dir); it != stdfs::recursive_directory_iterator(); ++it) {
        const std::string rel = stdfs::relative(it->path(), dir).generic_u8string();
        if (it->is_directory()) host_dirs.insert(rel);
        else {
            host_files[rel] = it->file_size();
            host_bytes += it->file_size();
        }
    }
    std::printf("folder: %zu files, %zu directories (+ root), %.2f GB\n", host_files.size(), host_dirs.size(),
                host_bytes / 1e9);
    for (const auto& [p, fi] : files) {
        auto h = host_files.find(p);
        if (h == host_files.end()) Fail("%s only in the image", p);
        else if (h->second != fi.size) Fail("%s size image %llu folder %llu", p, fi.size, h->second);
    }
    for (const auto& [p, sz] : host_files)
        if (!files.count(p)) Fail("%s only in the folder", p);
    for (const std::string& d : host_dirs)
        if (img.Lookup(d) != nnshim::romfs::Kind::Dir) Fail("directory %s not in the image", d);
    if (img.DirCount() != host_dirs.size() + 1) Fail("%s: directories image %llu folder %llu", std::string("count"), img.DirCount(), host_dirs.size() + 1);
    std::printf("1. index: %s\n", g_fail ? "DIFFERENCES" : "identical (paths, sizes and directories)");

    // ---- 2. bytes
    std::vector<uint8_t> a(8u << 20), b(a.size());
    uint64_t ha = 0xCBF29CE484222325ull, hb = ha, bytes = 0;
    size_t checked = 0, idx = 0;
    double ta = 0, tb = 0;
    t0 = Clock::now();
    for (const auto& [p, fi] : files) {
        if (idx++ % static_cast<size_t>(sample)) continue;
        std::FILE* f = Open(dir / stdfs::u8path(p));
        if (!f) { Fail("%s cannot be opened in the folder", p); continue; }
        for (uint64_t off = 0; off < fi.size;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(a.size(), fi.size - off));
            uint64_t got = 0;
            auto t1 = Clock::now();
            const bool ok = img.ReadFile(fi, off, a.data(), n, got);
            ta += Secs(t1);
            t1 = Clock::now();
            const size_t gb = std::fread(b.data(), 1, n, f);
            tb += Secs(t1);
            if (!ok || got != n || gb != n || std::memcmp(a.data(), b.data(), n) != 0) {
                Fail("%s bytes differ from %llu", p, off);
                break;
            }
            ha = Fnv(ha, a.data(), n);
            hb = Fnv(hb, b.data(), n);
            off += n;
            bytes += n;
        }
        std::fclose(f);
        ++checked;
        if (checked % 2000 == 0) std::printf("   ... %zu files, %.2f GB, %.0f s\n", checked, bytes / 1e9, Secs(t0));
    }
    std::printf("2. bytes: %zu files, %.3f GB compared; FNV-1a image %016llx folder %016llx; "
                "read image %.1f s, folder %.1f s\n",
                checked, bytes / 1e9, static_cast<unsigned long long>(ha), static_cast<unsigned long long>(hb), ta, tb);

    // ---- 3. shim API
    nnshim::fs::Config fc;
    fc.romfs_bin = bin;
    nnshim::fs::Initialize(fc);
    nnshim::fs::MountRom("data");
    size_t dirs_ok = 0;
    std::vector<std::string> all_dirs(host_dirs.begin(), host_dirs.end());
    all_dirs.push_back("");
    for (const std::string& d : all_dirs) {
        uint64_t h = 0;
        if (nnshim::fs::OpenDirectory(h, "data:/" + d, nnshim::fs::DirDirectories | nnshim::fs::DirFiles).result) {
            Fail("OpenDirectory %s fails", d);
            continue;
        }
        std::vector<nnshim::fs::DirectoryEntry> ents;
        nnshim::fs::ReadDirectory(h, ents, 1 << 20);
        nnshim::fs::CloseDirectory(h);
        std::vector<std::string> mine, host;
        for (const auto& e : ents)
            mine.push_back(std::string(e.name) + (e.type == nnshim::fs::EntryDirectory ? "/" : ":" + std::to_string(e.size)));
        for (const auto& de : stdfs::directory_iterator(dir / stdfs::u8path(d)))
            host.push_back(de.path().filename().u8string() + (de.is_directory() ? "/" : ":" + std::to_string(de.file_size())));
        if (mine != host) Fail("listing of %s differs (%llu vs %llu entries)", d, mine.size(), host.size());
        else ++dirs_ok;
    }
    std::mt19937_64 rng(12345);
    std::vector<std::pair<std::string, uint64_t>> list(host_files.begin(), host_files.end());
    size_t reads_ok = 0;
    uint64_t rbytes = 0;
    std::vector<uint8_t> c(4u << 20), e(c.size());
    t0 = Clock::now();
    for (int i = 0; i < random_reads && !list.empty(); ++i) {
        const auto& [p, size] = list[rng() % list.size()];
        const uint64_t off = size ? rng() % (size + 16) : 0;   // sometimes past the end
        const uint64_t len = 1 + rng() % (i % 10 == 0 ? c.size() : 65536);
        uint64_t h = 0, got = 0;
        if (nnshim::fs::OpenFile(h, "data:/" + p, nnshim::fs::OpenRead).result) { Fail("OpenFile %s fails", p); continue; }
        const auto st = nnshim::fs::ReadFile(h, static_cast<int64_t>(off), c.data(), len, got);
        nnshim::fs::CloseFile(h);
        if (off > size) {
            if (st.result != nnshim::fs_result::OutOfRange) Fail("%s: reading past the end does not give OutOfRange (%llu > %llu)", p, off, size);
            else ++reads_ok;
            continue;
        }
        std::FILE* f = Open(dir / stdfs::u8path(p));
        _fseeki64(f, static_cast<long long>(off), SEEK_SET);
        const size_t ge = std::fread(e.data(), 1, static_cast<size_t>(len), f);
        std::fclose(f);
        if (st.result || got != ge || std::memcmp(c.data(), e.data(), ge) != 0) Fail("%s: partial read differs at %llu (+%llu)", p, off, len);
        else {
            ++reads_ok;
            rbytes += got;
        }
    }
    nnshim::fs::Finalize();
    std::printf("3. shim API: %zu/%zu identical listings (name, type, size and order); %zu/%d identical random "
                "partial reads (%.1f MB, %.1f s)\n",
                dirs_ok, all_dirs.size(), reads_ok, random_reads, rbytes / 1e6, Secs(t0));
    std::printf("%s (%d differences)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
