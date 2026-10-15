// nnshim::romfs: reader for a Switch RomFS image (romfs.bin, IVFC level 3 already decrypted).
//
// Format (all little-endian):
//   header (0x50): header size (0x50), and (offset, size) pairs for
//     the directory hash table, the directory metadata table, the file hash table and
//     the file metadata table; finally, the offset of the data region.
//   directory: u32 parent, u32 sibling, u32 first subdirectory, u32 first file,
//     u32 next in hash bucket, u32 name length, name (padded to 4 bytes).
//   file: u32 parent, u32 sibling, u64 offset (relative to the data region),
//     u64 size, u32 next in hash bucket, u32 name length, name.
//   0xFFFFFFFF = "none". The root directory is at offset 0, unnamed.
//
// Opening reads only the two metadata tables (about 1.1 MB for this game) and builds an
// in-memory index (ASCII-lowercase path -> entry), so the image's hash tables
// are not needed. Lookup is case-insensitive, like the extracted folder on NTFS.
// Data is not loaded: each read is positional (ReadFile with OVERLAPPED on Windows, pread
// elsewhere) on a single handle, with no shared state: it can be read from several threads.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nnshim::romfs {

struct FileInfo {
    uint64_t offset = 0;   // absolute within the image
    uint64_t size = 0;
};

struct DirEntry {
    std::string name;
    bool is_dir = false;
    uint64_t size = 0;
};

enum class Kind { None, File, Dir };

// Normalizes a relative RomFS path: '/' or '\\' separators, no empty parts or ".";
// returns false if it contains "..". The result neither starts nor ends with '/' ("" = root).
bool NormalizePath(const std::string& in, std::string& out);

// Default image path ("romfs_bin": "auto"): <executable folder>/exefs/romfs.bin,
// the same one the HLE uses in the dist. Empty if it cannot be determined.
std::string DefaultImagePath();

class Image {
public:
    Image() = default;
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // Opens the image and builds the index. `err` receives the reason on failure.
    bool Open(const std::string& u8path, std::string* err = nullptr);
    void Close();
    bool IsOpen() const { return handle_ != kNoHandle; }
    const std::string& Path() const { return path_; }

    // `rel` relative to the root (normalized here). `fi` may be nullptr.
    Kind Lookup(const std::string& rel, FileInfo* fi = nullptr) const;
    // Direct entries of a directory, in image order. false if it is not a directory.
    bool List(const std::string& rel, std::vector<DirEntry>& out) const;
    // Absolute positional read; `got` < size only at the end of the image or on error.
    bool Read(uint64_t abs_offset, void* dst, uint64_t size, uint64_t& got) const;
    // Reads `size` bytes of file `fi` from `offset` (clipped to the file size).
    bool ReadFile(const FileInfo& fi, uint64_t offset, void* dst, uint64_t size, uint64_t& got) const;

    size_t FileCount() const { return files_.size(); }
    size_t DirCount() const { return dirs_.size(); }
    uint64_t ImageSize() const { return image_size_; }
    // Iterates over all files (path with original case, '/' separator).
    void ForEachFile(const std::function<void(const std::string&, const FileInfo&)>& fn) const;

private:
    struct Dir {
        std::string name;
        std::string path;                 // full path with original case
        std::vector<uint32_t> dirs;       // indices into dirs_
        std::vector<uint32_t> files;      // indices into files_
    };
    struct File {
        std::string name;
        std::string path;
        FileInfo info;
    };
    static constexpr intptr_t kNoHandle = -1;

    intptr_t handle_ = kNoHandle;         // HANDLE (Windows) or descriptor (POSIX)
    std::string path_;
    uint64_t image_size_ = 0;
    std::vector<Dir> dirs_;
    std::vector<File> files_;
    // lowercase path -> (index << 1) | is_file
    std::unordered_map<std::string, uint64_t> index_;
};

} // namespace nnshim::romfs
