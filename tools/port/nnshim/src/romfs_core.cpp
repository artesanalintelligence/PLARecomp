// nnshim::romfs: romfs.bin reader (see include/nnshim/romfs.hpp for the format).
// Own implementation based on the public description of the Switch RomFS format.
#include "nnshim/romfs.hpp"

#include <cstring>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nnshim::romfs {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint64_t kMaxMetaTable = 256ull << 20;   // sanity limit for the tables

struct Header {
    uint64_t header_size;
    uint64_t dir_hash_off, dir_hash_size;
    uint64_t dir_meta_off, dir_meta_size;
    uint64_t file_hash_off, file_hash_size;
    uint64_t file_meta_off, file_meta_size;
    uint64_t data_off;
};
static_assert(sizeof(Header) == 0x50, "RomFS header: 0x50 bytes");

uint32_t U32(const std::vector<uint8_t>& t, size_t o) {
    uint32_t v;
    std::memcpy(&v, t.data() + o, 4);
    return v;
}
uint64_t U64(const std::vector<uint8_t>& t, size_t o) {
    uint64_t v;
    std::memcpy(&v, t.data() + o, 8);
    return v;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Join(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + "/" + name;
}

#ifdef _WIN32
std::wstring Widen(const std::string& u8) {
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), w.data(), n);
    return w;
}
#endif

} // namespace

bool NormalizePath(const std::string& in, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i <= in.size()) {
        size_t j = i;
        while (j < in.size() && in[j] != '/' && in[j] != '\\') ++j;
        const size_t len = j - i;
        if (len == 2 && in[i] == '.' && in[i + 1] == '.') return false;
        if (len > 0 && !(len == 1 && in[i] == '.')) {
            if (!out.empty()) out += '/';
            out.append(in, i, len);
        }
        i = j + 1;
    }
    return true;
}

std::string DefaultImagePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return {};
    std::wstring exe(buf, n);
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    const std::wstring w = exe.substr(0, slash) + L"\\exefs\\romfs.bin";
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
#else
    return {};
#endif
}

Image::~Image() { Close(); }

void Image::Close() {
    if (handle_ != kNoHandle) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
        ::close(static_cast<int>(handle_));
#endif
    }
    handle_ = kNoHandle;
    path_.clear();
    image_size_ = 0;
    dirs_.clear();
    files_.clear();
    index_.clear();
}

bool Image::Read(uint64_t abs_offset, void* dst, uint64_t size, uint64_t& got) const {
    got = 0;
    if (handle_ == kNoHandle) return false;
    auto* p = static_cast<uint8_t*>(dst);
    while (got < size) {
        const uint64_t want64 = size - got;
        const uint32_t want = want64 > (1u << 30) ? (1u << 30) : static_cast<uint32_t>(want64);
        const uint64_t off = abs_offset + got;
#ifdef _WIN32
        // Handle opened with FILE_FLAG_OVERLAPPED: the kernel does not serialize reads from several threads
        // (it does with a synchronous handle, per file object). One event per thread to wait for the read.
        thread_local HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) return false;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        ov.hEvent = ev;
        DWORD n = 0;
        const HANDLE h = reinterpret_cast<HANDLE>(handle_);
        if (!::ReadFile(h, p + got, want, &n, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) e = GetOverlappedResult(h, &ov, &n, TRUE) ? ERROR_SUCCESS : GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            if (e != ERROR_SUCCESS) return false;
        }
#else
        const ssize_t n = ::pread(static_cast<int>(handle_), p + got, want, static_cast<off_t>(off));
        if (n < 0) return false;
#endif
        if (n == 0) break;
        got += static_cast<uint64_t>(n);
    }
    return true;
}

bool Image::ReadFile(const FileInfo& fi, uint64_t offset, void* dst, uint64_t size, uint64_t& got) const {
    got = 0;
    if (offset >= fi.size) return offset == fi.size || size == 0;
    const uint64_t n = size < fi.size - offset ? size : fi.size - offset;
    return Read(fi.offset + offset, dst, n, got) && got == n;
}

bool Image::Open(const std::string& u8path, std::string* err) {
    Close();
    auto fail = [&](const char* why) {
        if (err) *err = why;
        Close();
        return false;
    };
#ifdef _WIN32
    HANDLE h = CreateFileW(Widen(u8path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("could not open");
    handle_ = reinterpret_cast<intptr_t>(h);
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) return fail("could not read the size");
    image_size_ = static_cast<uint64_t>(sz.QuadPart);
#else
    const int fd = ::open(u8path.c_str(), O_RDONLY);
    if (fd < 0) return fail("could not open");
    handle_ = fd;
    struct stat st {};
    if (::fstat(fd, &st) != 0) return fail("could not read the size");
    image_size_ = static_cast<uint64_t>(st.st_size);
#endif
    path_ = u8path;

    Header hd{};
    uint64_t got = 0;
    if (!Read(0, &hd, sizeof hd, got) || got != sizeof hd) return fail("incomplete header");
    if (hd.header_size != sizeof hd) return fail("not a RomFS image (header size is not 0x50)");
    auto in_image = [&](uint64_t off, uint64_t size) {
        return off <= image_size_ && size <= image_size_ - off && size <= kMaxMetaTable;
    };
    if (!in_image(hd.dir_meta_off, hd.dir_meta_size) || !in_image(hd.file_meta_off, hd.file_meta_size) ||
        hd.data_off > image_size_ || hd.dir_meta_size < 0x18)
        return fail("metadata tables outside the image");

    std::vector<uint8_t> dmeta(static_cast<size_t>(hd.dir_meta_size)), fmeta(static_cast<size_t>(hd.file_meta_size));
    if (!Read(hd.dir_meta_off, dmeta.data(), dmeta.size(), got) || got != dmeta.size()) return fail("directory table read");
    if (!fmeta.empty() && (!Read(hd.file_meta_off, fmeta.data(), fmeta.size(), got) || got != fmeta.size()))
        return fail("file table read");

    // Walk from the root (offset 0) through the child and sibling lists.
    std::unordered_set<uint32_t> seen_dirs, seen_files;
    std::vector<std::pair<uint32_t, uint32_t>> stack;   // (offset in dmeta, index in dirs_)
    dirs_.push_back(Dir{});
    seen_dirs.insert(0);
    index_.emplace(std::string(), 0);
    stack.emplace_back(0u, 0u);
    while (!stack.empty()) {
        const auto [doff, di] = stack.back();
        stack.pop_back();
        if (static_cast<size_t>(doff) + 0x18 > dmeta.size()) return fail("directory outside the table");
        // subdirectories
        for (uint32_t c = U32(dmeta, doff + 8); c != kNone; c = U32(dmeta, c + 4)) {
            if (static_cast<size_t>(c) + 0x18 > dmeta.size() || !seen_dirs.insert(c).second)
                return fail("corrupt directory table");
            const uint32_t nlen = U32(dmeta, c + 0x14);
            if (static_cast<size_t>(c) + 0x18 + nlen > dmeta.size()) return fail("directory name outside the table");
            Dir d;
            d.name.assign(reinterpret_cast<const char*>(dmeta.data() + c + 0x18), nlen);
            d.path = Join(dirs_[di].path, d.name);
            const auto idx = static_cast<uint32_t>(dirs_.size());
            index_[Lower(d.path)] = static_cast<uint64_t>(idx) << 1;
            dirs_[di].dirs.push_back(idx);
            dirs_.push_back(std::move(d));
            stack.emplace_back(c, idx);
        }
        // files
        for (uint32_t f = U32(dmeta, doff + 0xC); f != kNone; f = U32(fmeta, f + 4)) {
            if (static_cast<size_t>(f) + 0x20 > fmeta.size() || !seen_files.insert(f).second)
                return fail("corrupt file table");
            const uint32_t nlen = U32(fmeta, f + 0x1C);
            if (static_cast<size_t>(f) + 0x20 + nlen > fmeta.size()) return fail("file name outside the table");
            File fe;
            fe.name.assign(reinterpret_cast<const char*>(fmeta.data() + f + 0x20), nlen);
            fe.path = Join(dirs_[di].path, fe.name);
            fe.info.offset = hd.data_off + U64(fmeta, f + 8);
            fe.info.size = U64(fmeta, f + 0x10);
            if (fe.info.offset > image_size_ || fe.info.size > image_size_ - fe.info.offset)
                return fail("file data outside the image");
            const auto idx = static_cast<uint32_t>(files_.size());
            index_[Lower(fe.path)] = (static_cast<uint64_t>(idx) << 1) | 1;
            dirs_[di].files.push_back(idx);
            files_.push_back(std::move(fe));
        }
    }
    if (err) err->clear();
    return true;
}

Kind Image::Lookup(const std::string& rel, FileInfo* fi) const {
    std::string norm;
    if (!NormalizePath(rel, norm)) return Kind::None;
    const auto it = index_.find(Lower(norm));
    if (it == index_.end()) return Kind::None;
    if (it->second & 1) {
        if (fi) *fi = files_[static_cast<size_t>(it->second >> 1)].info;
        return Kind::File;
    }
    return Kind::Dir;
}

bool Image::List(const std::string& rel, std::vector<DirEntry>& out) const {
    out.clear();
    std::string norm;
    if (!NormalizePath(rel, norm)) return false;
    const auto it = index_.find(Lower(norm));
    if (it == index_.end() || (it->second & 1)) return false;
    const Dir& d = dirs_[static_cast<size_t>(it->second >> 1)];
    for (uint32_t i : d.dirs) out.push_back(DirEntry{dirs_[i].name, true, 0});
    for (uint32_t i : d.files) out.push_back(DirEntry{files_[i].name, false, files_[i].info.size});
    return true;
}

void Image::ForEachFile(const std::function<void(const std::string&, const FileInfo&)>& fn) const {
    for (const File& f : files_) fn(f.path, f.info);
}

} // namespace nnshim::romfs
