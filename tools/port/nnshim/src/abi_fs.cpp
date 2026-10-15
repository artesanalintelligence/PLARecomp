// nnshim: nn::fs adapters (mounts, files, directories) and A/B validation.
//
// Native mode: the shim serves the "rom" and save mounts from host folders.
// Observe mode: suyu's HLE serves everything; the shim traces every OpenFile
// ("open <path> m=<mode> r=<result>") and, with the "post" hooks added by
// gen_mod_json.py --fs-validate, compares byte by byte every RomFS read returned by
// the HLE against the extracted file in romfs_dir. The traces of both runs (observe and
// native) are compared with compare_traces.py.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "abi_common.hpp"
#include "nnshim/fs.hpp"
#include "nnshim/romfs.hpp"

using namespace nnshim;

namespace {

constexpr size_t kChunk = 1u << 20;

void InitFs(const cfg::Value& c) {
    fs::Config fc;
    fc.romfs_dir = c.GetString("romfs_dir", c.GetString("fs.romfs_dir"));
    fc.romfs_bin = c.GetString("romfs_bin", c.GetString("fs.romfs_bin"));
    if (fc.romfs_bin == "auto") fc.romfs_bin = romfs::DefaultImagePath();
    // romfs_overlays: list of folders or a single string.
    const cfg::Value* ov = c.Find("romfs_overlays");
    if (!ov) ov = c.Find("fs.romfs_overlays");
    if (ov && ov->type == cfg::Value::String && !ov->str.empty()) fc.romfs_overlays.push_back(ov->str);
    if (ov && ov->type == cfg::Value::Array)
        for (const cfg::Value& v : ov->arr)
            if (v.type == cfg::Value::String && !v.str.empty()) fc.romfs_overlays.push_back(v.str);
    fc.save_root = c.GetString("save_root", c.GetString("fs.save_root"));
    fc.journal = c.GetBool("fs.journal", true);
    const auto t0 = std::chrono::steady_clock::now();
    fs::Initialize(fc);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Log(PLA_LOG_INFO, "nnshim: fs romfs=%s save=%s journal=%d (%.1f ms)", fs::RomSourceDescription().c_str(),
        fc.save_root.empty() ? "(HLE)" : fc.save_root.c_str(), fc.journal, ms);
}

void ShutdownFs() {
    if (const uint64_t opens = CounterGet("fs.shim.opens"))
        Log(PLA_LOG_INFO, "nnshim: fs (shim): %llu opens (%.1f ms), %llu reads, %.1f MB in %.1f ms",
            static_cast<unsigned long long>(opens), CounterGet("fs.shim.open_us") / 1000.0,
            static_cast<unsigned long long>(CounterGet("fs.shim.reads")), CounterGet("fs.shim.read_bytes") / 1e6,
            CounterGet("fs.shim.read_us") / 1000.0);
    const uint64_t reads = CounterGet("fs.validate.reads"), bad = CounterGet("fs.validate.mismatch");
    if (reads || bad)
        Log(PLA_LOG_INFO, "nnshim: fs validation: %llu reads, %llu bytes compared, %llu mismatched, %llu unchecked",
            static_cast<unsigned long long>(reads), static_cast<unsigned long long>(CounterGet("fs.validate.bytes")),
            static_cast<unsigned long long>(bad), static_cast<unsigned long long>(CounterGet("fs.validate.unchecked")));
    fs::Finalize();
}

ModuleHooks g_fs_module{"fs", &InitFs, &ShutdownFs};

pla_action Done(Guest& g, const fs::Status& st) {
    if (!st.ours) return PLA_CONTINUE;
    g.ret(st.result);
    return PLA_SKIP;
}

bool Path(Guest& g, unsigned reg, std::string& out) { return g.cstr(g.x(reg), out); }

fs::Uid LoadUid(Guest& g, uint64_t va) {
    fs::Uid u{};
    g.load(va, u);
    return u;
}

// ---- observe-mode state (handles created by the HLE) -----------------------------------
std::mutex g_obs_mutex;
std::set<std::string> g_rom_mounts;                 // names mounted with MountRom (HLE)
struct ObsFile { std::string path; std::string rom_rel; };
std::map<uint64_t, ObsFile> g_obs_files;

std::string RomRelative(const std::string& path) {
    const auto colon = path.find(':');
    if (colon == std::string::npos) return {};
    std::lock_guard lk(g_obs_mutex);
    if (!g_rom_mounts.count(path.substr(0, colon))) return {};
    return path.substr(colon + 1);
}

// Compares `n` bytes of the guest buffer with the host RomFS file.
void ValidateRead(Guest& g, uint64_t handle, int64_t off, uint64_t buf, uint64_t n) {
    ObsFile f;
    {
        std::lock_guard lk(g_obs_mutex);
        auto it = g_obs_files.find(handle);
        if (it == g_obs_files.end()) { CounterAdd("fs.validate.unchecked"); return; }
        f = it->second;
    }
    if (f.rom_rel.empty()) { CounterAdd("fs.validate.unchecked"); return; }
    std::vector<uint8_t> a(static_cast<size_t>(std::min<uint64_t>(n, kChunk))), b(a.size());
    for (uint64_t done = 0; done < n;) {
        const uint64_t want = std::min<uint64_t>(n - done, kChunk);
        uint64_t got = 0;
        const size_t gg = g.read(buf + done, a.data(), static_cast<size_t>(want));
        if (!fs::ReadRomfsFile(f.rom_rel, off + static_cast<int64_t>(done), b.data(), want, got) || got != want ||
            gg != want || std::memcmp(a.data(), b.data(), static_cast<size_t>(want)) != 0) {
            CounterAdd("fs.validate.mismatch");
            Trace("mismatch %s off=%lld size=%llu (host=%llu)", f.path.c_str(), static_cast<long long>(off),
                  static_cast<unsigned long long>(n), static_cast<unsigned long long>(got));
            return;
        }
        done += want;
    }
    CounterAdd("fs.validate.reads");
    CounterAdd("fs.validate.bytes", n);
}

uint64_t MicrosSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}

// Time and volume of the reads served by the shim (to compare folder vs romfs.bin).
struct ReadTimer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    uint64_t* total;
    explicit ReadTimer(uint64_t* t) : total(t) {}
    ~ReadTimer() {
        CounterAdd("fs.shim.reads");
        CounterAdd("fs.shim.read_bytes", *total);
        CounterAdd("fs.shim.read_us", MicrosSince(t0));
    }
};

pla_action ReadCommon(Guest& g, uint64_t out_va, uint64_t h, int64_t off, uint64_t buf, uint64_t size) {
    if (!fs::IsShimFile(h)) return PLA_CONTINUE;
    uint64_t total = 0;
    ReadTimer timer(&total);
    fs::Status st = fs::Status::Ok();
    // pla_api 1.1: direct read into guest memory (no intermediate copy).
    if (void* p = size && PLA_API_HAS(g_api, host_pointer) ? g_api->host_pointer(buf, size) : nullptr) {
        st = fs::ReadFile(h, off, p, size, total);
        if (st.ours && st.result == 0 && total) UiOnRomRead(h, off, static_cast<uint8_t*>(p), total, buf);
        if (st.ours && st.result == 0 && out_va) g.store(out_va, total);
        if (st.ours && st.result == 0) CounterAdd("fs.native.read_bytes_direct", total);
        return Done(g, st);
    }
    std::vector<uint8_t> tmp(static_cast<size_t>(std::min<uint64_t>(size, kChunk)));
    while (total < size) {
        const uint64_t want = std::min<uint64_t>(size - total, kChunk);
        uint64_t got = 0;
        st = fs::ReadFile(h, off + static_cast<int64_t>(total), tmp.data(), want, got);
        if (!st.ours || st.result) break;
        if (got) UiOnRomRead(h, off + static_cast<int64_t>(total), tmp.data(), got, buf + total);
        if (got) g.write(buf + total, tmp.data(), static_cast<size_t>(got));
        total += got;
        if (got < want) break;   // end of file
    }
    if (st.ours && st.result == 0 && out_va) g.store(out_va, total);
    if (st.ours && st.result == 0) CounterAdd("fs.native.read_bytes", total);
    return Done(g, st);
}

} // namespace

// =========================== mounts =======================================================
// Result MountRom(const char* name)
NNSHIM_ADAPTER(Fs, nnshim_fs_MountRom) {
    PlaGuest g(ctx);
    std::string name;
    if (!Path(g, 0, name)) return PLA_CONTINUE;
    const auto st = fs::MountRom(name);
    if (st.ours) Trace("mount rom %s r=%#x", name.c_str(), st.result);
    return Done(g, st);
}
NNSHIM_OBSERVER(nnshim_fs_MountRom) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }

// Result MountRom(const char* name, void* cache, size_t cacheSize): the cache is unused.
NNSHIM_ADAPTER(Fs, nnshim_fs_MountRomCache) {
    PlaGuest g(ctx);
    std::string name;
    if (!Path(g, 0, name)) return PLA_CONTINUE;
    const auto st = fs::MountRom(name);
    if (st.ours) Trace("mount rom %s r=%#x", name.c_str(), st.result);
    return Done(g, st);
}
NNSHIM_OBSERVER(nnshim_fs_MountRomCache) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }

// MountRom post (observe mode): learns the name of the RomFS mount.
NNSHIM_EXPORT void nnshim_fs_MountRom_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t name_va = g.call_data()[0];
    std::string name;
    if (!name_va || !g.cstr(name_va, name)) return;
    const uint32_t r = g.w(0);
    Trace("mount rom %s r=%#x", name.c_str(), r);
    if (r == 0) {
        std::lock_guard lk(g_obs_mutex);
        g_rom_mounts.insert(name);
    }
}

// Result QueryMountRomCacheSize(size_t* out)
NNSHIM_ADAPTER(Fs, nnshim_fs_QueryMountRomCacheSize) {
    if (!fs::RomfsConfigured()) return PLA_CONTINUE;
    PlaGuest g(ctx);
    g.store<uint64_t>(g.x(0), 0x1000);   // token size: the game allocates it and passes it to MountRom
    g.ret(0);
    return PLA_SKIP;
}

// Result MountSaveData(const char* name, const account::Uid& uid)
NNSHIM_ADAPTER(Fs, nnshim_fs_MountSaveData) {
    PlaGuest g(ctx);
    std::string name;
    if (!Path(g, 0, name)) return PLA_CONTINUE;
    const auto st = fs::MountSaveData(name, LoadUid(g, g.x(1)), std::nullopt, false);
    if (st.ours) Trace("mount save %s r=%#x", name.c_str(), st.result);
    return Done(g, st);
}

// Result MountSaveDataReadOnly(const char* name, ApplicationId id, const account::Uid& uid)
// Saves of other games (bonuses): <save_root>/<ApplicationId>/<Uid>/ if the user
// copied them; otherwise TargetNotFound (what the game sees on a console without that save).
NNSHIM_ADAPTER(Fs, nnshim_fs_MountSaveDataReadOnly) {
    PlaGuest g(ctx);
    std::string name;
    if (!Path(g, 0, name)) return PLA_CONTINUE;
    const auto st = fs::MountSaveData(name, LoadUid(g, g.x(2)), g.x(1), true);
    if (st.ours) Trace("mount save-ro %s app=%016llx r=%#x", name.c_str(), static_cast<unsigned long long>(g.x(1)), st.result);
    return Done(g, st);
}

// Result EnsureSaveData(const account::Uid& uid)
NNSHIM_ADAPTER(Fs, nnshim_fs_EnsureSaveData) {
    PlaGuest g(ctx);
    return Done(g, fs::EnsureSaveData(LoadUid(g, g.x(0))));
}

// bool IsSaveDataExisting(const account::Uid& uid)
NNSHIM_ADAPTER(Fs, nnshim_fs_IsSaveDataExisting) {
    PlaGuest g(ctx);
    auto r = fs::IsSaveDataExisting(std::nullopt, LoadUid(g, g.x(0)));
    if (!r) return PLA_CONTINUE;
    g.ret(*r ? 1 : 0);
    return PLA_SKIP;
}

// bool IsSaveDataExisting(ApplicationId id, const account::Uid& uid)
NNSHIM_ADAPTER(Fs, nnshim_fs_IsSaveDataExistingApp) {
    PlaGuest g(ctx);
    auto r = fs::IsSaveDataExisting(g.x(0), LoadUid(g, g.x(1)));
    if (!r) return PLA_CONTINUE;
    Trace("exists app=%016llx -> %d", static_cast<unsigned long long>(g.x(0)), *r ? 1 : 0);
    g.ret(*r ? 1 : 0);
    return PLA_SKIP;
}

// void Unmount(const char* name)
NNSHIM_ADAPTER(Fs, nnshim_fs_Unmount) {
    PlaGuest g(ctx);
    std::string name;
    if (!Path(g, 0, name)) return PLA_CONTINUE;
    const bool ours = fs::Unmount(name).ours;
    if (ours) Trace("unmount %s", name.c_str());
    return ours ? PLA_SKIP : PLA_CONTINUE;
}

// Result Commit(const char* name)
NNSHIM_ADAPTER(Fs, nnshim_fs_Commit) {
    PlaGuest g(ctx);
    std::string name;
    if (!Path(g, 0, name)) return PLA_CONTINUE;
    const auto st = fs::Commit(name);
    if (st.ours) {
        Trace("commit %s r=%#x", name.c_str(), st.result);
        CounterAdd(st.result ? "fs.commit.failed" : "fs.commit.ok");
    }
    return Done(g, st);
}

// =========================== files ========================================================
// Result OpenFile(FileHandle* out, const char* path, int mode)
NNSHIM_ADAPTER(Fs, nnshim_fs_OpenFile) {
    PlaGuest g(ctx);
    std::string path;
    if (!Path(g, 1, path)) return PLA_CONTINUE;
    uint64_t h = 0;
    const int mode = static_cast<int>(g.w(2));
    const auto t0 = std::chrono::steady_clock::now();
    auto st = fs::OpenFile(h, path, mode);
    if (st.ours) {
        CounterAdd("fs.shim.opens");
        CounterAdd("fs.shim.open_us", MicrosSince(t0));
    }
    if (st.ours && st.result == 0) g.store(g.x(0), h);
    if (st.ours) Trace("open %s m=%d r=%#x", path.c_str(), mode, st.result);
    return Done(g, st);
}
NNSHIM_OBSERVER(nnshim_fs_OpenFile) {
    uint64_t* cd = g_api->call_data(ctx);
    cd[0] = g_api->get_x(ctx, 0);
    cd[1] = g_api->get_x(ctx, 1);
    cd[2] = g_api->get_x(ctx, 2);
}

// OpenFile post (observe mode): HLE handle -> path, to validate its reads.
NNSHIM_EXPORT void nnshim_fs_OpenFile_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g.call_data();
    std::string path;
    if (!cd[1] || !g.cstr(cd[1], path)) return;
    const uint32_t r = g.w(0);
    Trace("open %s m=%d r=%#x", path.c_str(), static_cast<int>(cd[2]), r);
    if (r != 0) return;
    uint64_t h = 0;
    g.load(cd[0], h);
    ObsFile f{path, RomRelative(path)};
    std::lock_guard lk(g_obs_mutex);
    g_obs_files[h] = std::move(f);
}

// void CloseFile(FileHandle h)   (FileHandle = { void* }: passed in X0)
NNSHIM_ADAPTER(Fs, nnshim_fs_CloseFile) {
    PlaGuest g(ctx);
    return fs::CloseFile(g.x(0)).ours ? PLA_SKIP : PLA_CONTINUE;
}
NNSHIM_OBSERVER(nnshim_fs_CloseFile) {
    std::lock_guard lk(g_obs_mutex);
    g_obs_files.erase(g_api->get_x(ctx, 0));
}

// Result ReadFile(size_t* outRead, FileHandle h, int64 offset, void* buf, size_t size)
NNSHIM_ADAPTER(Fs, nnshim_fs_ReadFileOut) {
    PlaGuest g(ctx);
    return ReadCommon(g, g.x(0), g.x(1), static_cast<int64_t>(g.x(2)), g.x(3), g.x(4));
}
NNSHIM_OBSERVER(nnshim_fs_ReadFileOut) {
    uint64_t* cd = g_api->call_data(ctx);
    for (unsigned i = 0; i < 4; ++i) cd[i] = g_api->get_x(ctx, i);   // outRead, h, off, buf
}
NNSHIM_EXPORT void nnshim_fs_ReadFileOut_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g.call_data();
    if (g.w(0) != 0 || !cd[0]) return;
    uint64_t n = 0;
    g.load(cd[0], n);
    ValidateRead(g, cd[1], static_cast<int64_t>(cd[2]), cd[3], n);
}

// Result ReadFile(FileHandle h, int64 offset, void* buf, size_t size)
NNSHIM_ADAPTER(Fs, nnshim_fs_ReadFile) {
    PlaGuest g(ctx);
    return ReadCommon(g, 0, g.x(0), static_cast<int64_t>(g.x(1)), g.x(2), g.x(3));
}
NNSHIM_OBSERVER(nnshim_fs_ReadFile) {
    uint64_t* cd = g_api->call_data(ctx);
    for (unsigned i = 0; i < 4; ++i) cd[i] = g_api->get_x(ctx, i);   // h, off, buf, size
}
NNSHIM_EXPORT void nnshim_fs_ReadFile_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g.call_data();
    if (g.w(0) != 0) return;
    ValidateRead(g, cd[0], static_cast<int64_t>(cd[1]), cd[2], cd[3]);
}

// Result WriteFile(FileHandle h, int64 offset, const void* buf, size_t size, const WriteOption& opt)
NNSHIM_ADAPTER(Fs, nnshim_fs_WriteFile) {
    PlaGuest g(ctx);
    const uint64_t h = g.x(0);
    if (!fs::IsShimFile(h)) return PLA_CONTINUE;
    const int64_t off = static_cast<int64_t>(g.x(1));
    const uint64_t buf = g.x(2), size = g.x(3);
    int32_t flags = 0;
    g.load(g.x(4), flags);
    if (const void* p = size && PLA_API_HAS(g_api, host_pointer) ? g_api->host_pointer(buf, size) : nullptr) {
        const fs::Status st = fs::WriteFile(h, off, p, size, flags);
        if (st.ours && st.result == 0) CounterAdd("fs.native.write_bytes", size);
        return Done(g, st);
    }
    std::vector<uint8_t> tmp(static_cast<size_t>(std::min<uint64_t>(size, kChunk)));
    fs::Status st = fs::Status::Ok();
    for (uint64_t done = 0; done < size;) {
        const uint64_t n = std::min<uint64_t>(size - done, kChunk);
        if (g.read(buf + done, tmp.data(), static_cast<size_t>(n)) != n) {
            st = fs::Status::Err(fs_result::InvalidArgument);
            break;
        }
        const bool last = done + n == size;
        st = fs::WriteFile(h, off + static_cast<int64_t>(done), tmp.data(), n, last ? flags : 0);
        if (!st.ours || st.result) break;
        done += n;
    }
    if (st.ours && st.result == 0) CounterAdd("fs.native.write_bytes", size);
    return Done(g, st);
}

// Result FlushFile(FileHandle h)
NNSHIM_ADAPTER(Fs, nnshim_fs_FlushFile) {
    PlaGuest g(ctx);
    return Done(g, fs::FlushFile(g.x(0)));
}

// Result GetFileSize(int64* out, FileHandle h)
NNSHIM_ADAPTER(Fs, nnshim_fs_GetFileSize) {
    PlaGuest g(ctx);
    int64_t size = 0;
    auto st = fs::GetFileSize(g.x(1), size);
    if (st.ours && st.result == 0) g.store(g.x(0), size);
    return Done(g, st);
}

// Result SetFileSize(FileHandle h, int64 size)
NNSHIM_ADAPTER(Fs, nnshim_fs_SetFileSize) {
    PlaGuest g(ctx);
    return Done(g, fs::SetFileSize(g.x(0), static_cast<int64_t>(g.x(1))));
}

// =========================== entries and directories ======================================
// Result GetEntryType(DirectoryEntryType* out, const char* path)   (4-byte enum)
NNSHIM_ADAPTER(Fs, nnshim_fs_GetEntryType) {
    PlaGuest g(ctx);
    std::string path;
    if (!Path(g, 1, path)) return PLA_CONTINUE;
    fs::EntryType t{};
    auto st = fs::GetEntryType(path, t);
    if (st.ours && st.result == 0) g.store<int32_t>(g.x(0), t);
    if (st.ours) Trace("entrytype %s r=%#x", path.c_str(), st.result);
    return Done(g, st);
}

// Result CreateFile(const char* path, int64 size)
NNSHIM_ADAPTER(Fs, nnshim_fs_CreateFile) {
    PlaGuest g(ctx);
    std::string path;
    if (!Path(g, 0, path)) return PLA_CONTINUE;
    const auto st = fs::CreateFile(path, static_cast<int64_t>(g.x(1)));
    if (st.ours) Trace("create %s size=%lld r=%#x", path.c_str(), static_cast<long long>(g.x(1)), st.result);
    return Done(g, st);
}

#define PATH_ONLY(NAME, CALL, TAG)                                                     \
    NNSHIM_ADAPTER(Fs, NAME) {                                                         \
        PlaGuest g(ctx);                                                               \
        std::string path;                                                              \
        if (!Path(g, 0, path)) return PLA_CONTINUE;                                    \
        const auto st = CALL(path);                                                    \
        if (st.ours) Trace(TAG " %s r=%#x", path.c_str(), st.result);                  \
        return Done(g, st);                                                            \
    }
PATH_ONLY(nnshim_fs_DeleteFile, fs::DeleteFile, "delete")
PATH_ONLY(nnshim_fs_CreateDirectory, fs::CreateDirectory, "mkdir")
PATH_ONLY(nnshim_fs_DeleteDirectoryRecursively, fs::DeleteDirectoryRecursively, "rmdir")

// Result OpenDirectory(DirectoryHandle* out, const char* path, int mode)
NNSHIM_ADAPTER(Fs, nnshim_fs_OpenDirectory) {
    PlaGuest g(ctx);
    std::string path;
    if (!Path(g, 1, path)) return PLA_CONTINUE;
    uint64_t h = 0;
    auto st = fs::OpenDirectory(h, path, static_cast<int>(g.w(2)));
    if (st.ours && st.result == 0) g.store(g.x(0), h);
    if (st.ours) Trace("opendir %s r=%#x", path.c_str(), st.result);
    return Done(g, st);
}

// Result ReadDirectory(int64* outCount, DirectoryEntry* entries, DirectoryHandle h, int64 max)
NNSHIM_ADAPTER(Fs, nnshim_fs_ReadDirectory) {
    PlaGuest g(ctx);
    std::vector<fs::DirectoryEntry> ents;
    auto st = fs::ReadDirectory(g.x(2), ents, static_cast<int64_t>(g.x(3)));
    if (st.ours && st.result == 0) {
        if (!ents.empty()) g.write(g.x(1), ents.data(), ents.size() * sizeof(ents[0]));
        g.store<int64_t>(g.x(0), static_cast<int64_t>(ents.size()));
    }
    return Done(g, st);
}

// void CloseDirectory(DirectoryHandle h)
NNSHIM_ADAPTER(Fs, nnshim_fs_CloseDirectory) {
    PlaGuest g(ctx);
    return fs::CloseDirectory(g.x(0)).ours ? PLA_SKIP : PLA_CONTINUE;
}
