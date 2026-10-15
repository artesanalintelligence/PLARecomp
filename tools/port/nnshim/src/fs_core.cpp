// nnshim::fs: nn::fs over std::filesystem.
//
// SDK semantics reproduced (the ones the game can notice):
//  - OpenFile without OpenAllowAppend cannot write past the end of the file;
//  - ReadFile past the end returns fewer bytes (or OutOfRange if offset > size);
//  - CreateFile fails if it exists (PathAlreadyExists), DeleteFile if it does not (PathNotFound);
//  - the "rom" mount is read-only;
//  - journaled saves: writes go to a working copy and only Commit makes them
//    permanent, atomically (new directory + rename); Unmount without Commit discards them.
//
// The "rom" mount comes from the extracted folder (romfs_dir) or, if absent, from the romfs.bin image
// (romfs_bin, reader in nnshim/romfs.hpp). In both cases, paths are first looked up in the
// romfs_overlays folders (mods): a file there replaces the original and directory listings
// merge the entries.
#include "nnshim/fs.hpp"
#include "nnshim/romfs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nnshim::fs {
namespace stdfs = std::filesystem;
using namespace nnshim::fs_result;

namespace {

struct Mount {
    stdfs::path root;            // what the game sees (journaled saves: the working copy)
    bool read_only;
    stdfs::path committed;       // journaled saves: the committed directory; empty otherwise
    bool rom = false;            // RomFS mount (romfs_overlays apply)
    bool bin = false;            // RomFS served from the romfs.bin image (empty root)
};

struct OpenFileRec {
    std::FILE* fp = nullptr;     // nullptr if the file comes from romfs.bin
    int mode = 0;
    stdfs::path host;
    std::string mount;           // mount name (to close on Unmount)
    std::string rom_id;          // RomFS files: identifier for RomFileId
    bool bin = false;
    romfs::FileInfo bin_info;
};

struct OpenDirRec {
    std::vector<DirectoryEntry> entries;
    size_t next = 0;
};

std::mutex g_mutex;
Config g_cfg;
std::map<std::string, Mount> g_mounts;
std::map<uint64_t, std::unique_ptr<OpenFileRec>> g_files;
std::map<uint64_t, std::unique_ptr<OpenDirRec>> g_dirs;
uint64_t g_next_id = 1;
// RomFS source (decided by Initialize): folder, image or none (HLE).
bool g_rom_dir = false;
std::shared_ptr<const romfs::Image> g_img;   // copied under g_mutex and read outside it
std::string g_img_error;
constexpr const char* kBinIdPrefix = "romfs.bin:/";

std::string Hex(uint64_t v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llX", width, static_cast<unsigned long long>(v));
    return buf;
}

stdfs::path SaveDir(uint64_t app_id, const Uid& uid) {
    return stdfs::u8path(g_cfg.save_root) / Hex(app_id, 16) / (Hex(uid.hi, 16) + Hex(uid.lo, 16));
}

// "<mount>:/<path>" -> host path. Rejects ".." so it cannot leave the mount.
// Returns nullptr if the mount is not the shim's.
// `rel_out` (optional): the relative path normalized with '/' (the one romfs.bin and overlays use).
const Mount* Resolve(const std::string& guest, stdfs::path& out, Result& err, std::string* rel_out = nullptr) {
    err = ResultSuccess;
    const auto colon = guest.find(':');
    if (colon == std::string::npos) return nullptr;
    const auto it = g_mounts.find(guest.substr(0, colon));
    if (it == g_mounts.end()) return nullptr;
    std::string rel = guest.substr(colon + 1);
    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) rel.erase(rel.begin());
    stdfs::path p = stdfs::u8path(rel).lexically_normal();
    for (const auto& part : p)
        if (part == "..") { err = InvalidPath; break; }
    out = it->second.root / p;
    if (rel_out && !romfs::NormalizePath(rel, *rel_out)) err = InvalidPath;
    return &it->second;
}

// Result of looking up a path in the RomFS (overlays first, then the folder or the image).
struct RomHit {
    romfs::Kind kind = romfs::Kind::None;
    bool bin = false;            // file from the image (info); otherwise from a folder (host)
    stdfs::path host;
    romfs::FileInfo info;
};

romfs::Kind HostKind(const stdfs::path& p) {
    std::error_code ec;
    const auto st = stdfs::status(p, ec);
    if (ec || st.type() == stdfs::file_type::not_found) return romfs::Kind::None;
    if (stdfs::is_directory(st)) return romfs::Kind::Dir;
    return stdfs::is_regular_file(st) ? romfs::Kind::File : romfs::Kind::None;
}

// With g_mutex held. `host` = the host path returned by Resolve (folder case only).
RomHit RomLookup(bool bin, const std::string& rel, const stdfs::path& host) {
    RomHit hit;
    for (const std::string& o : g_cfg.romfs_overlays) {
        const stdfs::path p = rel.empty() ? stdfs::u8path(o) : stdfs::u8path(o) / stdfs::u8path(rel);
        const romfs::Kind k = HostKind(p);
        if (k == romfs::Kind::None) continue;
        hit.kind = k;
        hit.host = p;
        return hit;
    }
    if (bin) {
        if (g_img) hit.kind = g_img->Lookup(rel, &hit.info);
        hit.bin = hit.kind == romfs::Kind::File;
        return hit;
    }
    hit.kind = HostKind(host);
    hit.host = host;
    return hit;
}

// ASCII uppercase to sort like NTFS (the order the extracted folder gave).
bool NtfsLess(const DirectoryEntry& a, const DirectoryEntry& b) {
    for (size_t i = 0;; ++i) {
        unsigned char x = static_cast<unsigned char>(a.name[i]), y = static_cast<unsigned char>(b.name[i]);
        if (x >= 'a' && x <= 'z') x = static_cast<unsigned char>(x - 32);
        if (y >= 'a' && y <= 'z') y = static_cast<unsigned char>(y - 32);
        if (x != y || !x) return x < y;
    }
}

DirectoryEntry MakeEntry(const std::string& name, bool is_dir, int64_t size) {
    DirectoryEntry e{};
    std::strncpy(e.name, name.c_str(), sizeof(e.name) - 1);
    e.type = is_dir ? EntryDirectory : EntryFile;
    e.size = is_dir ? 0 : size;
    return e;
}

void ListHostDir(const stdfs::path& dir, std::vector<DirectoryEntry>& out) {
    std::error_code ec;
    for (const auto& de : stdfs::directory_iterator(dir, ec)) {
        const bool is_dir = de.is_directory(ec);
        out.push_back(MakeEntry(de.path().filename().u8string(), is_dir, is_dir ? 0 : static_cast<int64_t>(de.file_size(ec))));
    }
}

Status ToStatus(const std::error_code& ec) {
    if (!ec) return Status::Ok();
    if (ec == std::errc::no_such_file_or_directory) return Status::Err(PathNotFound);
    if (ec == std::errc::file_exists) return Status::Err(PathAlreadyExists);
    if (ec == std::errc::directory_not_empty) return Status::Err(DirectoryNotEmpty);
    if (ec == std::errc::no_space_on_device) return Status::Err(UsableSpaceNotEnough);
    return Status::Err(TargetLocked);
}

int64_t HostSize(std::FILE* fp) {
#ifdef _WIN32
    const int64_t cur = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_END);
    const int64_t end = _ftelli64(fp);
    _fseeki64(fp, cur, SEEK_SET);
#else
    const int64_t cur = ftello(fp);
    fseeko(fp, 0, SEEK_END);
    const int64_t end = ftello(fp);
    fseeko(fp, cur, SEEK_SET);
#endif
    return end;
}

bool HostSeek(std::FILE* fp, int64_t off) {
#ifdef _WIN32
    return _fseeki64(fp, off, SEEK_SET) == 0;
#else
    return fseeko(fp, off, SEEK_SET) == 0;
#endif
}

std::FILE* HostOpen(const stdfs::path& p, bool write) {
#ifdef _WIN32
    return _wfopen(p.c_str(), write ? L"r+b" : L"rb");
#else
    return std::fopen(p.c_str(), write ? "r+b" : "rb");
#endif
}

// Copies a file tree making sure the data reaches the disk (for Commit).
bool CopyTreeDurable(const stdfs::path& src, const stdfs::path& dst, std::error_code& ec) {
    stdfs::create_directories(dst, ec);
    if (ec) return false;
    for (auto it = stdfs::recursive_directory_iterator(src, ec); !ec && it != stdfs::recursive_directory_iterator();
         it.increment(ec)) {
        const stdfs::path rel = stdfs::relative(it->path(), src, ec);
        if (ec) return false;
        const stdfs::path out = dst / rel;
        if (it->is_directory(ec)) {
            stdfs::create_directories(out, ec);
            if (ec) return false;
            continue;
        }
#ifdef _WIN32
        std::FILE* in = _wfopen(it->path().c_str(), L"rb");
        std::FILE* o = _wfopen(out.c_str(), L"wb");
#else
        std::FILE* in = std::fopen(it->path().c_str(), "rb");
        std::FILE* o = std::fopen(out.c_str(), "wb");
#endif
        if (!in || !o) {
            if (in) std::fclose(in);
            if (o) std::fclose(o);
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
        static thread_local char buf[1 << 16];
        size_t n;
        bool ok = true;
        while ((n = std::fread(buf, 1, sizeof buf, in)) > 0)
            if (std::fwrite(buf, 1, n, o) != n) { ok = false; break; }
        std::fclose(in);
        std::fflush(o);
#ifdef _WIN32
        ok = ok && _commit(_fileno(o)) == 0;
#else
        ok = ok && fsync(fileno(o)) == 0;
#endif
        std::fclose(o);
        if (!ok) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    return !ec;
}

stdfs::path WithSuffix(const stdfs::path& p, const char* suffix) {
    stdfs::path r = p;
    r += suffix;
    return r;
}

// Possible states after an interruption in the middle of Commit (see Commit):
//   <dir> exists                       -> leftover .commit/.old: deleted;
//   <dir> missing, .commit exists      -> the new copy was complete: publish it;
//   <dir> missing, only .old           -> roll back to the previous version.
void RecoverLocked(const stdfs::path& dir) {
    std::error_code ec;
    const stdfs::path tmp = WithSuffix(dir, ".commit"), old = WithSuffix(dir, ".old");
    if (stdfs::exists(dir, ec)) {
        stdfs::remove_all(tmp, ec);
        stdfs::remove_all(old, ec);
        return;
    }
    if (stdfs::exists(tmp, ec)) {
        stdfs::rename(tmp, dir, ec);
        if (!ec) stdfs::remove_all(old, ec);
        return;
    }
    if (stdfs::exists(old, ec)) stdfs::rename(old, dir, ec);
}

bool UnderPath(const stdfs::path& p, const stdfs::path& root) {
    const auto& rs = root.native();
    const auto& ps = p.native();
    return ps.size() > rs.size() && ps.compare(0, rs.size(), rs) == 0 &&
           (ps[rs.size()] == '/' || ps[rs.size()] == '\\');
}

} // namespace

void RecoverSave(const std::string& dir) {
    std::lock_guard lk(g_mutex);
    RecoverLocked(stdfs::u8path(dir));
}

std::string SaveDirFor(uint64_t app_id, const Uid& uid) {
    std::lock_guard lk(g_mutex);
    return SaveDir(app_id, uid).u8string();
}

bool ReadRomfsFile(const std::string& rel_path, int64_t offset, void* dst, uint64_t size, uint64_t& got) {
    got = 0;
    if (offset < 0) return false;
    RomHit hit;
    std::shared_ptr<const romfs::Image> img;
    {
        std::lock_guard lk(g_mutex);
        if (!g_rom_dir && !g_img) return false;
        std::string rel;
        if (!romfs::NormalizePath(rel_path, rel)) return false;
        const stdfs::path host = g_rom_dir ? stdfs::u8path(g_cfg.romfs_dir) / stdfs::u8path(rel) : stdfs::path();
        hit = RomLookup(!g_rom_dir, rel, host);
        img = g_img;
    }
    if (hit.kind != romfs::Kind::File) return false;
    if (hit.bin) return img && img->ReadFile(hit.info, static_cast<uint64_t>(offset), dst, size, got);
    std::FILE* f = HostOpen(hit.host, false);
    if (!f) return false;
    const bool ok = HostSeek(f, offset);
    if (ok) got = std::fread(dst, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    return ok;
}

void Initialize(const Config& cfg) {
    std::lock_guard lk(g_mutex);
    g_cfg = cfg;
    g_img.reset();
    g_img_error.clear();
    std::error_code ec;
    const bool dir_ok = !cfg.romfs_dir.empty() && stdfs::is_directory(stdfs::u8path(cfg.romfs_dir), ec);
    if (!dir_ok && !cfg.romfs_bin.empty()) {
        auto img = std::make_shared<romfs::Image>();
        if (img->Open(cfg.romfs_bin, &g_img_error)) g_img = std::move(img);
    }
    // Folder configured but missing and no image: mount anyway (as before), and
    // opens fail with PathNotFound.
    g_rom_dir = dir_ok || (!cfg.romfs_dir.empty() && !g_img);
}

void Finalize() {
    std::lock_guard lk(g_mutex);
    for (auto& [h, f] : g_files)
        if (f->fp) std::fclose(f->fp);
    g_files.clear();
    g_dirs.clear();
    g_mounts.clear();
    g_img.reset();
    g_rom_dir = false;
}

bool RomfsConfigured() {
    std::lock_guard lk(g_mutex);
    if (g_img) return true;
    return g_rom_dir && stdfs::is_directory(stdfs::u8path(g_cfg.romfs_dir));
}

std::string RomSourceDescription() {
    std::lock_guard lk(g_mutex);
    std::string s;
    if (g_img) {
        char buf[160];
        std::snprintf(buf, sizeof buf, " (%zu files, %zu directories, %.2f GB)", g_img->FileCount(), g_img->DirCount(),
                      static_cast<double>(g_img->ImageSize()) / 1e9);
        s = "romfs.bin " + g_img->Path() + buf;
    } else if (g_rom_dir) {
        s = "folder " + g_cfg.romfs_dir;
    } else {
        s = "(HLE)";
    }
    if (!g_img_error.empty()) s += " [romfs.bin " + g_cfg.romfs_bin + ": " + g_img_error + "]";
    for (const std::string& o : g_cfg.romfs_overlays) s += " + overlay " + o;
    return s;
}

Status MountRom(const std::string& name) {
    std::lock_guard lk(g_mutex);
    if (!g_rom_dir && !g_img) return Status::NotOurs();
    if (g_mounts.count(name)) return Status::Err(MountNameAlreadyExists);
    Mount m{g_rom_dir ? stdfs::u8path(g_cfg.romfs_dir) : stdfs::path(), true, {}};
    m.rom = true;
    m.bin = !g_rom_dir;
    g_mounts[name] = std::move(m);
    return Status::Ok();
}

Status MountSaveData(const std::string& name, const Uid& uid, std::optional<uint64_t> app_id, bool read_only) {
    std::lock_guard lk(g_mutex);
    if (g_cfg.save_root.empty()) return Status::NotOurs();
    if (g_mounts.count(name)) return Status::Err(MountNameAlreadyExists);
    const uint64_t app = app_id.value_or(g_cfg.title_id);
    const stdfs::path dir = SaveDir(app, uid);
    RecoverLocked(dir);
    std::error_code ec;
    if (!stdfs::is_directory(dir, ec)) {
        // Our own save is created by EnsureSaveData; another game's (the bonuses for
        // having saves of other titles) only exists if the user copied it there.
        return Status::Err(TargetNotFound);
    }
    if (read_only || !g_cfg.journal) {
        g_mounts[name] = Mount{dir, read_only, {}};
        return Status::Ok();
    }
    // Fresh working copy: anything not committed with Commit is lost on unmount.
    const stdfs::path work = WithSuffix(dir, ".work");
    stdfs::remove_all(work, ec);
    ec.clear();
    if (!CopyTreeDurable(dir, work, ec)) return ToStatus(ec ? ec : std::make_error_code(std::errc::io_error));
    g_mounts[name] = Mount{work, false, dir};
    return Status::Ok();
}

Status EnsureSaveData(const Uid& uid) {
    std::lock_guard lk(g_mutex);
    if (g_cfg.save_root.empty()) return Status::NotOurs();
    const stdfs::path dir = SaveDir(g_cfg.title_id, uid);
    RecoverLocked(dir);
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    return ToStatus(ec);
}

std::optional<bool> IsSaveDataExisting(std::optional<uint64_t> app_id, const Uid& uid) {
    std::lock_guard lk(g_mutex);
    if (g_cfg.save_root.empty()) return std::nullopt;
    const stdfs::path dir = SaveDir(app_id.value_or(g_cfg.title_id), uid);
    RecoverLocked(dir);
    std::error_code ec;
    return stdfs::is_directory(dir, ec);
}

Status Unmount(const std::string& name) {
    std::lock_guard lk(g_mutex);
    auto it = g_mounts.find(name);
    if (it == g_mounts.end()) return Status::NotOurs();
    const Mount m = it->second;
    g_mounts.erase(it);
    // Handles the game left open on that mount: close them (the SDK would treat it
    // as a programming error; here it is enough not to leak the descriptors).
    for (auto f = g_files.begin(); f != g_files.end();) {
        if (f->second->mount == name || (!m.root.empty() && UnderPath(f->second->host, m.root))) {
            if (f->second->fp) std::fclose(f->second->fp);
            f = g_files.erase(f);
        } else {
            ++f;
        }
    }
    if (!m.committed.empty()) {
        std::error_code ec;
        stdfs::remove_all(m.root, ec);   // uncommitted changes: discarded
    }
    return Status::Ok();
}

// Transactional Commit: working copy -> <dir>.commit (with the data on disk) and
// publication with two renames. At any interruption point, RecoverLocked leaves <dir> as the
// complete previous version or the complete new one, never half-written.
Status Commit(const std::string& name) {
    std::lock_guard lk(g_mutex);
    auto it = g_mounts.find(name);
    if (it == g_mounts.end()) return Status::NotOurs();
    const Mount& m = it->second;
    for (auto& [h, f] : g_files)
        if (f->fp && (f->mode & OpenWrite)) std::fflush(f->fp);
    if (m.read_only || m.committed.empty()) return Status::Ok();   // no journal: already on disk
    const stdfs::path dir = m.committed, tmp = WithSuffix(dir, ".commit"), old = WithSuffix(dir, ".old");
    std::error_code ec;
    stdfs::remove_all(tmp, ec);
    ec.clear();
    if (!CopyTreeDurable(m.root, tmp, ec)) {
        stdfs::remove_all(tmp, ec);
        return Status::Err(UsableSpaceNotEnough);
    }
    stdfs::remove_all(old, ec);
    ec.clear();
    stdfs::rename(dir, old, ec);
    if (ec) {
        stdfs::remove_all(tmp, ec);
        return Status::Err(TargetLocked);
    }
    stdfs::rename(tmp, dir, ec);
    if (ec) {
        std::error_code ec2;
        stdfs::rename(old, dir, ec2);   // roll back
        return Status::Err(TargetLocked);
    }
    stdfs::remove_all(old, ec);
    return Status::Ok();
}

Status OpenFile(uint64_t& out_handle, const std::string& path, int mode) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    std::string rel;
    const auto colon = path.find(':');
    const auto mit = colon == std::string::npos ? g_mounts.end() : g_mounts.find(path.substr(0, colon));
    const bool rom = mit != g_mounts.end() && mit->second.rom;
    const Mount* m = Resolve(path, host, err, rom ? &rel : nullptr);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    if ((mode & OpenWrite) && m->read_only) return Status::Err(InvalidOperationForOpenMode);
    auto rec = std::make_unique<OpenFileRec>();
    rec->mode = mode;
    rec->mount = mit->first;
    if (rom) {
        const RomHit hit = RomLookup(m->bin, rel, host);
        if (hit.kind != romfs::Kind::File) return Status::Err(PathNotFound);
        if (hit.bin) {
            rec->bin = true;
            rec->bin_info = hit.info;
            rec->rom_id = kBinIdPrefix + rel;
        } else {
            host = hit.host;
            rec->rom_id = host.u8string();
        }
    } else {
        std::error_code ec;
        if (!stdfs::is_regular_file(host, ec)) return Status::Err(PathNotFound);
    }
    if (!rec->bin) {
        rec->fp = HostOpen(host, (mode & OpenWrite) != 0);
        if (!rec->fp) return Status::Err(TargetLocked);
        rec->host = host;
    }
    const uint64_t h = kFileTag | g_next_id++;
    g_files[h] = std::move(rec);
    out_handle = h;
    return Status::Ok();
}

std::string RomFileId(uint64_t handle) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(handle);
    if (it == g_files.end() || (it->second->mode & OpenWrite)) return {};
    return it->second->rom_id;
}

bool ReadRomFileById(const std::string& id, std::vector<uint8_t>& out, uint64_t max_size) {
    out.clear();
    if (id.compare(0, std::strlen(kBinIdPrefix), kBinIdPrefix) == 0) {
        std::shared_ptr<const romfs::Image> img;
        {
            std::lock_guard lk(g_mutex);
            img = g_img;
        }
        romfs::FileInfo fi;
        if (!img || img->Lookup(id.substr(std::strlen(kBinIdPrefix)), &fi) != romfs::Kind::File) return false;
        if (fi.size == 0 || fi.size > max_size) return false;
        out.resize(static_cast<size_t>(fi.size));
        uint64_t got = 0;
        if (!img->ReadFile(fi, 0, out.data(), fi.size, got) || got != fi.size) {
            out.clear();
            return false;
        }
        return true;
    }
    std::error_code ec;
    const stdfs::path p = stdfs::u8path(id);
    const auto size = stdfs::file_size(p, ec);
    if (ec || size == 0 || size > max_size) return false;
    std::FILE* f = HostOpen(p, false);
    if (!f) return false;
    out.resize(static_cast<size_t>(size));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

Status CloseFile(uint64_t handle) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(handle);
    if (it == g_files.end()) return IsShimFile(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    if (it->second->fp) std::fclose(it->second->fp);
    g_files.erase(it);
    return Status::Ok();
}

Status ReadFile(uint64_t handle, int64_t offset, void* dst, uint64_t size, uint64_t& out_read) {
    std::unique_lock lk(g_mutex);
    out_read = 0;
    auto it = g_files.find(handle);
    if (it == g_files.end()) return IsShimFile(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    OpenFileRec& f = *it->second;
    if (!(f.mode & OpenRead)) return Status::Err(InvalidOperationForOpenMode);
    if (offset < 0) return Status::Err(OutOfRange);
    if (f.bin) {
        // romfs.bin: positional read without the lock (the image handle is stateless).
        const romfs::FileInfo fi = f.bin_info;
        const std::shared_ptr<const romfs::Image> img = g_img;
        lk.unlock();
        if (static_cast<uint64_t>(offset) > fi.size || !img) return Status::Err(OutOfRange);
        if (!img->ReadFile(fi, static_cast<uint64_t>(offset), dst, size, out_read)) return Status::Err(TargetLocked);
        return Status::Ok();
    }
    const int64_t fsize = HostSize(f.fp);
    if (offset > fsize) return Status::Err(OutOfRange);
    if (!HostSeek(f.fp, offset)) return Status::Err(OutOfRange);
    out_read = std::fread(dst, 1, static_cast<size_t>(size), f.fp);
    return Status::Ok();
}

Status WriteFile(uint64_t handle, int64_t offset, const void* src, uint64_t size, int option_flags) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(handle);
    if (it == g_files.end()) return IsShimFile(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    OpenFileRec& f = *it->second;
    if (!(f.mode & OpenWrite)) return Status::Err(InvalidOperationForOpenMode);
    if (offset < 0) return Status::Err(OutOfRange);
    const int64_t fsize = HostSize(f.fp);
    if (offset + static_cast<int64_t>(size) > fsize && !(f.mode & OpenAllowAppend))
        return Status::Err(FileExtensionWithoutOpenModeAllowAppend);
    if (!HostSeek(f.fp, offset)) return Status::Err(OutOfRange);
    if (std::fwrite(src, 1, static_cast<size_t>(size), f.fp) != size) return Status::Err(UsableSpaceNotEnough);
    if (option_flags & WriteFlush) std::fflush(f.fp);
    return Status::Ok();
}

Status FlushFile(uint64_t handle) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(handle);
    if (it == g_files.end()) return IsShimFile(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    if (it->second->fp) std::fflush(it->second->fp);   // fflush(nullptr) would flush every FILE
    return Status::Ok();
}

Status GetFileSize(uint64_t handle, int64_t& out_size) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(handle);
    if (it == g_files.end()) return IsShimFile(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    out_size = it->second->bin ? static_cast<int64_t>(it->second->bin_info.size) : HostSize(it->second->fp);
    return Status::Ok();
}

Status SetFileSize(uint64_t handle, int64_t size) {
    std::lock_guard lk(g_mutex);
    auto it = g_files.find(handle);
    if (it == g_files.end()) return IsShimFile(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    OpenFileRec& f = *it->second;
    if (!(f.mode & OpenWrite)) return Status::Err(InvalidOperationForOpenMode);
    std::fflush(f.fp);
    std::error_code ec;
    stdfs::resize_file(f.host, static_cast<uintmax_t>(size), ec);
    return ToStatus(ec);
}

Status GetEntryType(const std::string& path, EntryType& out) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    std::string rel;
    const Mount* m = Resolve(path, host, err, &rel);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    if (m->rom) {
        const RomHit hit = RomLookup(m->bin, rel, host);
        if (hit.kind == romfs::Kind::None) return Status::Err(PathNotFound);
        out = hit.kind == romfs::Kind::Dir ? EntryDirectory : EntryFile;
        return Status::Ok();
    }
    std::error_code ec;
    const auto st = stdfs::status(host, ec);
    if (ec || st.type() == stdfs::file_type::not_found) return Status::Err(PathNotFound);
    out = stdfs::is_directory(st) ? EntryDirectory : EntryFile;
    return Status::Ok();
}

Status CreateFile(const std::string& path, int64_t size) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    const Mount* m = Resolve(path, host, err);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    if (m->read_only) return Status::Err(InvalidOperationForOpenMode);
    std::error_code ec;
    if (stdfs::exists(host, ec)) return Status::Err(PathAlreadyExists);
    if (!stdfs::is_directory(host.parent_path(), ec)) return Status::Err(PathNotFound);
#ifdef _WIN32
    std::FILE* fp = _wfopen(host.c_str(), L"wb");
#else
    std::FILE* fp = std::fopen(host.c_str(), "wb");
#endif
    if (!fp) return Status::Err(TargetLocked);
    std::fclose(fp);
    stdfs::resize_file(host, static_cast<uintmax_t>(size), ec);
    return ToStatus(ec);
}

Status DeleteFile(const std::string& path) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    const Mount* m = Resolve(path, host, err);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    if (m->read_only) return Status::Err(InvalidOperationForOpenMode);
    std::error_code ec;
    if (!stdfs::is_regular_file(host, ec)) return Status::Err(PathNotFound);
    stdfs::remove(host, ec);
    return ToStatus(ec);
}

Status CreateDirectory(const std::string& path) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    const Mount* m = Resolve(path, host, err);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    if (m->read_only) return Status::Err(InvalidOperationForOpenMode);
    std::error_code ec;
    if (stdfs::exists(host, ec)) return Status::Err(PathAlreadyExists);
    if (!stdfs::is_directory(host.parent_path(), ec)) return Status::Err(PathNotFound);
    stdfs::create_directory(host, ec);
    return ToStatus(ec);
}

Status DeleteDirectoryRecursively(const std::string& path) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    const Mount* m = Resolve(path, host, err);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    if (m->read_only) return Status::Err(InvalidOperationForOpenMode);
    std::error_code ec;
    if (!stdfs::is_directory(host, ec)) return Status::Err(PathNotFound);
    stdfs::remove_all(host, ec);
    return ToStatus(ec);
}

Status OpenDirectory(uint64_t& out_handle, const std::string& path, int mode) {
    std::lock_guard lk(g_mutex);
    stdfs::path host;
    Result err;
    std::string rel;
    const Mount* m = Resolve(path, host, err, &rel);
    if (!m) return Status::NotOurs();
    if (err) return Status::Err(err);
    std::vector<DirectoryEntry> all;
    if (m->rom) {
        // Base (folder or image) + overlay folders; an overlay entry replaces the one with the
        // same name (case-insensitive) from the layers below.
        bool found = false;
        auto merge = [&](std::vector<DirectoryEntry>&& layer) {
            if (!found) {
                found = true;
                all = std::move(layer);
                return;
            }
            for (DirectoryEntry& e : layer) {
                auto same = [&](const DirectoryEntry& x) { return !NtfsLess(x, e) && !NtfsLess(e, x); };
                auto old = std::find_if(all.begin(), all.end(), same);
                if (old != all.end()) *old = e;
                else all.push_back(e);
            }
        };
        if (m->bin) {
            std::vector<romfs::DirEntry> ents;
            if (g_img && g_img->List(rel, ents)) {
                std::vector<DirectoryEntry> layer;
                for (const auto& e : ents) layer.push_back(MakeEntry(e.name, e.is_dir, static_cast<int64_t>(e.size)));
                std::sort(layer.begin(), layer.end(), NtfsLess);
                merge(std::move(layer));
            }
        } else if (HostKind(host) == romfs::Kind::Dir) {
            std::vector<DirectoryEntry> layer;
            ListHostDir(host, layer);
            merge(std::move(layer));
        }
        for (auto o = g_cfg.romfs_overlays.rbegin(); o != g_cfg.romfs_overlays.rend(); ++o) {
            const stdfs::path p = rel.empty() ? stdfs::u8path(*o) : stdfs::u8path(*o) / stdfs::u8path(rel);
            if (HostKind(p) != romfs::Kind::Dir) continue;
            std::vector<DirectoryEntry> layer;
            ListHostDir(p, layer);
            merge(std::move(layer));
        }
        if (!found) return Status::Err(PathNotFound);
    } else {
        std::error_code ec;
        if (!stdfs::is_directory(host, ec)) return Status::Err(PathNotFound);
        ListHostDir(host, all);
    }
    auto rec = std::make_unique<OpenDirRec>();
    for (const DirectoryEntry& e : all) {
        const bool is_dir = e.type == EntryDirectory;
        if (is_dir && !(mode & DirDirectories)) continue;
        if (!is_dir && !(mode & DirFiles)) continue;
        rec->entries.push_back(e);
    }
    const uint64_t h = kDirTag | g_next_id++;
    g_dirs[h] = std::move(rec);
    out_handle = h;
    return Status::Ok();
}

Status ReadDirectory(uint64_t handle, std::vector<DirectoryEntry>& out, int64_t max_entries) {
    std::lock_guard lk(g_mutex);
    out.clear();
    auto it = g_dirs.find(handle);
    if (it == g_dirs.end()) return IsShimDir(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    OpenDirRec& d = *it->second;
    while (d.next < d.entries.size() && static_cast<int64_t>(out.size()) < max_entries)
        out.push_back(d.entries[d.next++]);
    return Status::Ok();
}

Status CloseDirectory(uint64_t handle) {
    std::lock_guard lk(g_mutex);
    if (!g_dirs.erase(handle)) return IsShimDir(handle) ? Status::Err(InvalidHandle) : Status::NotOurs();
    return Status::Ok();
}

} // namespace nnshim::fs
