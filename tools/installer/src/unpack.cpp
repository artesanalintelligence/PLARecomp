// In-process .tar.xz unpacking (see unpack.h).
// SPDX-License-Identifier: MIT
#include "unpack.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "engine.h"
#define XZ_USE_CRC64  // as in xz_unpack.c: declares xz_crc64_init()
#include "xz.h"

namespace setup {

namespace {

// Absolute path with the \\?\ prefix: no MAX_PATH limit, whatever the LongPathsEnabled policy says.
std::wstring LongPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
    full.resize(n);
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) full.pop_back();
    if (full.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

bool IsDirectory(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// p is a \\?\ path.
bool MakeDirsLong(const std::wstring& p) {
    if (CreateDirectoryW(p.c_str(), nullptr)) return true;
    const DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS) return IsDirectory(p);
    if (e != ERROR_PATH_NOT_FOUND) return false;
    const size_t s = p.find_last_of(L'\\');
    if (s == std::wstring::npos || s <= 7) return false;  // reached "\\?\C:" or "\\?\UNC"
    if (!MakeDirsLong(p.substr(0, s))) return false;
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

uint64_t Octal(const uint8_t* f, size_t n) {
    if (f[0] & 0x80) {  // GNU base-256
        uint64_t v = f[0] & 0x7f;
        for (size_t i = 1; i < n; ++i) v = (v << 8) | f[i];
        return v;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (f[i] == ' ' || f[i] == 0)) ++i;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; ++i) v = (v << 3) | (uint64_t)(f[i] - '0');
    return v;
}

std::string Field(const uint8_t* f, size_t n) {
    size_t k = 0;
    while (k < n && f[k]) ++k;
    return std::string((const char*)f, k);
}

class TarWriter {
public:
    explicit TarWriter(std::wstring root) : root_(std::move(root)) {
        while (!root_.empty() && root_.back() == L'\\') root_.pop_back();
    }
    ~TarWriter() { Close(); }

    bool Feed(const uint8_t* p, size_t n) {
        while (n) {
            if (ended_) return true;  // the zero padding after the end-of-archive blocks
            if (!in_body_) {
                const size_t k = std::min(n, sizeof hdr_ - hdr_n_);
                memcpy(hdr_ + hdr_n_, p, k);
                hdr_n_ += k;
                p += k;
                n -= k;
                if (hdr_n_ < sizeof hdr_) break;
                hdr_n_ = 0;
                if (!OnHeader()) return false;
                continue;
            }
            if (left_) {
                const size_t k = (size_t)std::min<uint64_t>(n, left_);
                if (!OnData(p, k)) return false;
                p += k;
                n -= k;
                left_ -= k;
            } else {
                const size_t k = (size_t)std::min<uint64_t>(n, pad_);
                p += k;
                n -= k;
                pad_ -= k;
            }
            if (!left_ && !pad_ && !EndEntry()) return false;
        }
        return true;
    }

    // True when the archive ended on an entry boundary.
    bool Complete() const { return ended_ || (!in_body_ && hdr_n_ == 0 && zeros_ > 0); }
    const std::string& Error() const { return err_; }

private:
    enum class Target { File, Meta, Skip };

    bool Fail(const std::string& e) {
        err_ = e;
        return false;
    }

    bool OnHeader() {
        bool zero = true;
        for (uint8_t b : hdr_) zero = zero && !b;
        if (zero) {
            if (++zeros_ == 2) ended_ = true;
            return true;
        }
        zeros_ = 0;
        uint32_t sum = 0;
        for (size_t i = 0; i < sizeof hdr_; ++i) sum += (i >= 148 && i < 156) ? ' ' : hdr_[i];
        if (sum != (uint32_t)Octal(hdr_ + 148, 8)) return Fail("the archive is damaged (bad tar header)");

        uint64_t size = Octal(hdr_ + 124, 12);
        if (pax_size_set_) size = pax_size_;
        const char type = (char)hdr_[156];
        std::string name = Field(hdr_, 100);
        if (!memcmp(hdr_ + 257, "ustar", 5) && hdr_[345]) name = Field(hdr_ + 345, 155) + "/" + name;
        if (!gnu_name_.empty()) name = gnu_name_;
        else if (!pax_path_.empty()) name = pax_path_;
        mtime_ = pax_mtime_set_ ? pax_mtime_ : (int64_t)Octal(hdr_ + 136, 12);

        target_ = Target::Skip;
        const bool meta = type == 'x' || type == 'L';
        if (meta) {
            if (size > (16u << 20)) return Fail("the archive is damaged (oversized tar metadata)");
            meta_kind_ = type;
            meta_.clear();
            target_ = Target::Meta;
        } else if (type == 'g' || type == 'K') {
            // global pax header / GNU long link name: not needed
        } else {
            pax_path_.clear();
            gnu_name_.clear();
            pax_size_set_ = pax_mtime_set_ = false;
            std::wstring path;
            if (!Resolve(name, path)) return false;
            if (type == '1' || type == '2') return Fail("links are not allowed in a component: " + name);
            if (type == '5') {
                if (!path.empty() && !MakeDirsLong(path)) return Fail(Fmt("cannot create the folder %s (error %lu)", name.c_str(), GetLastError()));
            } else if (type == '0' || type == '\0' || type == '7') {
                if (path.empty()) return Fail("the archive is damaged (file without a name)");
                if (!Open(path, name)) return false;
                target_ = Target::File;
            }
        }
        left_ = size;
        pad_ = (512 - size % 512) % 512;
        in_body_ = true;
        return left_ || pad_ ? true : EndEntry();
    }

    // Archive name -> \\?\ path under the root; "" for the root itself. Refuses anything that would leave the root.
    bool Resolve(const std::string& name, std::wstring& out) {
        out.clear();
        std::wstring rel;
        size_t i = 0;
        while (i <= name.size()) {
            size_t j = name.find_first_of("/\\", i);
            if (j == std::string::npos) j = name.size();
            const std::string part = name.substr(i, j - i);
            i = j + 1;
            if (part.empty() || part == ".") continue;
            if (part == ".." || part.find(':') != std::string::npos) return Fail("path not allowed in a component: " + name);
            rel += L"\\" + W(part);
        }
        if (!rel.empty()) out = root_ + rel;
        return true;
    }

    bool Open(const std::wstring& path, const std::string& name) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            out_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out_ != INVALID_HANDLE_VALUE) return true;
            const DWORD e = GetLastError();
            if (e == ERROR_PATH_NOT_FOUND) {
                MakeDirsLong(path.substr(0, path.find_last_of(L'\\')));
            } else if (e == ERROR_ACCESS_DENIED) {
                const DWORD a = GetFileAttributesW(path.c_str());
                if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_READONLY)) return OpenFailed(name, e);
                SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
            } else {
                return OpenFailed(name, e);
            }
        }
        return OpenFailed(name, GetLastError());
    }

    bool OpenFailed(const std::string& name, DWORD e) {
        std::string m = Fmt("cannot write %s (error %lu)", name.c_str(), e);
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED)
            m += ": the file is in use or blocked; close programs started from it (or the antivirus scan) and retry";
        return Fail(m);
    }

    bool OnData(const uint8_t* p, size_t n) {
        if (target_ == Target::Meta) {
            meta_.append((const char*)p, n);
        } else if (target_ == Target::File) {
            DWORD w = 0;
            if (!WriteFile(out_, p, (DWORD)n, &w, nullptr) || w != n) {
                const DWORD e = GetLastError();
                return Fail(e == ERROR_DISK_FULL ? std::string("the disk is full") : Fmt("write error %lu", e));
            }
        }
        return true;
    }

    bool EndEntry() {
        in_body_ = false;
        if (target_ == Target::Meta) {
            if (meta_kind_ == 'L') {
                gnu_name_ = Field((const uint8_t*)meta_.data(), meta_.size());
            } else {
                ParsePax();
            }
        } else if (target_ == Target::File) {
            if (mtime_ > 0) {
                const uint64_t t = (uint64_t)mtime_ * 10000000ull + 116444736000000000ull;
                FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
                SetFileTime(out_, nullptr, nullptr, &ft);
            }
            Close();
        }
        target_ = Target::Skip;
        return true;
    }

    // "<len> <key>=<value>\n" records.
    void ParsePax() {
        size_t i = 0;
        while (i < meta_.size()) {
            const size_t sp = meta_.find(' ', i);
            if (sp == std::string::npos) break;
            const size_t len = (size_t)strtoull(meta_.c_str() + i, nullptr, 10);
            if (!len || i + len > meta_.size()) break;
            const std::string rec = meta_.substr(sp + 1, i + len - sp - 2);  // without the trailing '\n'
            i += len;
            const size_t eq = rec.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = rec.substr(0, eq), val = rec.substr(eq + 1);
            if (key == "path") pax_path_ = val;
            else if (key == "size") pax_size_ = strtoull(val.c_str(), nullptr, 10), pax_size_set_ = true;
            else if (key == "mtime") pax_mtime_ = (int64_t)strtod(val.c_str(), nullptr), pax_mtime_set_ = true;
        }
    }

    void Close() {
        if (out_ != INVALID_HANDLE_VALUE) CloseHandle(out_);
        out_ = INVALID_HANDLE_VALUE;
    }

    std::wstring root_;
    std::string err_;
    uint8_t hdr_[512];
    size_t hdr_n_ = 0;
    bool in_body_ = false, ended_ = false;
    int zeros_ = 0;
    uint64_t left_ = 0, pad_ = 0;
    Target target_ = Target::Skip;
    char meta_kind_ = 0;
    std::string meta_, pax_path_, gnu_name_;
    uint64_t pax_size_ = 0;
    int64_t pax_mtime_ = 0, mtime_ = 0;
    bool pax_size_set_ = false, pax_mtime_set_ = false;
    HANDLE out_ = INVALID_HANDLE_VALUE;
};

const char* XzError(xz_ret r) {
    switch (r) {
    case XZ_MEM_ERROR: return "not enough memory to unpack";
    case XZ_MEMLIMIT_ERROR: return "the archive needs a larger dictionary than allowed";
    case XZ_FORMAT_ERROR: return "not an .xz archive";
    case XZ_OPTIONS_ERROR: return "the archive uses unsupported .xz options";
    default: return "the archive is damaged or incomplete";
    }
}

}  // namespace

bool UnpackTarXz(const std::wstring& src, uint64_t offset, uint64_t size, const std::wstring& dest, std::atomic<uint64_t>& done,
                 const std::atomic<bool>& cancel, std::string& err) {
    static std::once_flag crc_once;
    std::call_once(crc_once, [] {
        xz_crc32_init();
        xz_crc64_init();
    });
    const std::wstring root = LongPath(dest);
    if (!MakeDirsLong(root)) {
        err = Fmt("cannot create %s (error %lu)", U8(dest).c_str(), GetLastError());
        return false;
    }
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        err = Fmt("cannot open %s (error %lu)", U8(src).c_str(), GetLastError());
        return false;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(in, pos, nullptr, FILE_BEGIN);
    // Multi-call mode; the dictionary (192 MiB for the toolchain) is allocated from the stream header.
    xz_dec* dec = xz_dec_init(XZ_DYNALLOC, 1u << 30);
    if (!dec) {
        CloseHandle(in);
        err = "not enough memory to unpack";
        return false;
    }
    std::vector<uint8_t> ib(1 << 20), ob(1 << 20);
    xz_buf b{ib.data(), 0, 0, ob.data(), 0, ob.size()};
    TarWriter tar(root);
    uint64_t left = size;
    bool ok = true;
    for (;;) {
        if (cancel.load()) {
            err = "cancelled";
            ok = false;
            break;
        }
        if (b.in_pos == b.in_size && left) {
            const DWORD want = (DWORD)std::min<uint64_t>(left, ib.size());
            DWORD got = 0;
            if (!ReadFile(in, ib.data(), want, &got, nullptr) || got != want) {
                err = Fmt("read error %lu", GetLastError());
                ok = false;
                break;
            }
            b.in_pos = 0;
            b.in_size = got;
            left -= got;
            done += got;
        }
        const xz_ret r = xz_dec_run(dec, &b);
        if (b.out_pos) {
            if (!tar.Feed(ob.data(), b.out_pos)) {
                err = tar.Error();
                ok = false;
                break;
            }
            b.out_pos = 0;
        }
        if (r == XZ_STREAM_END) break;
        if (r == XZ_OK || r == XZ_UNSUPPORTED_CHECK) continue;
        err = XzError(r);
        ok = false;
        break;
    }
    xz_dec_end(dec);
    CloseHandle(in);
    if (ok && !tar.Complete()) {
        err = "the archive is incomplete";
        ok = false;
    }
    return ok;
}

}  // namespace setup
