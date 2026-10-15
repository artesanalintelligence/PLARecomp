// nnshim::fs: nn::fs over host folders.
//
// Mounts:    "rom"  (nn::fs::MountRom)       -> folder with the extracted RomFS (romfs_dir) or the
//                                               romfs.bin image (romfs_bin), read-only; the
//                                               romfs_overlays folders (mods) are checked first
//            "save" (nn::fs::MountSaveData)  -> <save_root>/<TitleId>/<Uid>/
//            other MountSaveDataReadOnly     -> <save_root>/<ApplicationId>/<Uid>/ (if it exists;
//                                               otherwise TargetNotFound, which the game handles)
//
// Transactional saves (journal = true, default): a writable mount works on a copy
// "<Uid>.work"; Commit copies it to "<Uid>.commit" and publishes it with two renames
// (<Uid> -> <Uid>.old, <Uid>.commit -> <Uid>); Unmount without Commit discards the changes, as
// on the console. Mounting recovers an interrupted Commit (see RecoverSave).
// Guest paths are "<mount>:/<path>". A path whose mount is not the shim's
// returns NotOurs, and the adapter passes the call through to the SDK (suyu HLE).
//
// Handles: opaque 64-bit values tagged in the top 16 bits (kFileTag /
// kDirTag), to tell them apart from handles created by the original SDK.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nnshim/result.hpp"

namespace nnshim::fs {

constexpr uint64_t kFileTag = 0xF5F1ull << 48;
constexpr uint64_t kDirTag = 0xF5D1ull << 48;
constexpr uint64_t kTagMask = 0xFFFFull << 48;
inline bool IsShimFile(uint64_t h) { return (h & kTagMask) == kFileTag; }
inline bool IsShimDir(uint64_t h) { return (h & kTagMask) == kDirTag; }

enum OpenMode : int { OpenRead = 1, OpenWrite = 2, OpenAllowAppend = 4 };
enum DirMode : int { DirDirectories = 1, DirFiles = 2 };
enum EntryType : int8_t { EntryDirectory = 0, EntryFile = 1 };
enum WriteOptionFlag : int { WriteFlush = 1 };

#pragma pack(push, 1)
struct DirectoryEntry {          // nn::fs::DirectoryEntry (0x310 bytes)
    char name[0x301];
    uint8_t attributes;
    uint8_t reserved0[2];
    int8_t type;                 // EntryType
    uint8_t reserved1[3];
    int64_t size;
};
#pragma pack(pop)
static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry: 0x310 bytes");

struct Uid { uint64_t hi, lo; }; // nn::account::Uid (16 bytes)

struct Config {
    std::string romfs_dir;       // extracted folder; if it exists, it takes priority over romfs_bin
    std::string romfs_bin;       // romfs.bin image (own reader, nnshim/romfs.hpp)
                                 // both empty (or unusable) = MountRom stays in the HLE
    std::vector<std::string> romfs_overlays;   // folders with files that replace or add to
                                               // the RomFS ones (mods); the first one wins
    std::string save_root;       // empty = saves stay in the HLE
    uint64_t title_id = 0x01001F5010DFA000ull;
    bool journal = true;         // transactional saves (atomic Commit)
};

// Result of an operation: NotOurs = "not the shim's, let the original handle it".
struct Status {
    bool ours;
    Result result;
    static Status NotOurs() { return {false, 0}; }
    static Status Ok() { return {true, ResultSuccess}; }
    static Status Err(Result r) { return {true, r}; }
};

void Initialize(const Config& cfg);
void Finalize();

// Mounts
bool RomfsConfigured();
Status MountRom(const std::string& name);
Status MountSaveData(const std::string& name, const Uid& uid, std::optional<uint64_t> app_id, bool read_only);
Status EnsureSaveData(const Uid& uid);
std::optional<bool> IsSaveDataExisting(std::optional<uint64_t> app_id, const Uid& uid);
Status Unmount(const std::string& name);
Status Commit(const std::string& name);

// Files
Status OpenFile(uint64_t& out_handle, const std::string& path, int mode);
Status CloseFile(uint64_t handle);
// Stable identifier (UTF-8) of a file opened by the shim on a read-only mount
// (the RomFS): the host path if it comes from a folder, or "romfs.bin:/<path>" if it comes from the
// image; empty if the handle is not the shim's or the mount is writable. For nnshim::ui.
std::string RomFileId(uint64_t handle);
// Reads a whole file given by RomFileId (at most `max_size` bytes; if larger, false).
bool ReadRomFileById(const std::string& id, std::vector<uint8_t>& out, uint64_t max_size);
// Source of the shim's RomFS, for the log: "folder <path>", "romfs.bin <path> (N files...)",
// "(HLE)" or the error opening the image.
std::string RomSourceDescription();
Status ReadFile(uint64_t handle, int64_t offset, void* dst, uint64_t size, uint64_t& out_read);
Status WriteFile(uint64_t handle, int64_t offset, const void* src, uint64_t size, int option_flags);
Status FlushFile(uint64_t handle);
Status GetFileSize(uint64_t handle, int64_t& out_size);
Status SetFileSize(uint64_t handle, int64_t size);

// Entries
Status GetEntryType(const std::string& path, EntryType& out);
Status CreateFile(const std::string& path, int64_t size);
Status DeleteFile(const std::string& path);
Status CreateDirectory(const std::string& path);
Status DeleteDirectoryRecursively(const std::string& path);

// Validation (A/B tests): host path of a RomFS file ("<rom mount>:/x" already
// without the mount) and direct read, bypassing the game's handles.
bool ReadRomfsFile(const std::string& rel_path, int64_t offset, void* dst, uint64_t size, uint64_t& got);
// Recovers an interrupted Commit in `dir` (<save_root>/<app>/<uid>). Exposed for tests.
void RecoverSave(const std::string& dir);
// Host path of the save (<save_root>/<app>/<uid>) for tests and tools.
std::string SaveDirFor(uint64_t app_id, const Uid& uid);

// Directories
Status OpenDirectory(uint64_t& out_handle, const std::string& path, int mode);
Status ReadDirectory(uint64_t handle, std::vector<DirectoryEntry>& out, int64_t max_entries);
Status CloseDirectory(uint64_t handle);

} // namespace nnshim::fs
