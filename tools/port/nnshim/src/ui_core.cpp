// nnshim::ui: pure logic (no guest, no Win32) for key-prompt icons and menu state. See ui.hpp.
#include "nnshim/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace nnshim::ui {
namespace {

template <class T> bool Rd(const uint8_t* b, size_t len, uint64_t off, T& out) {
    if (off > len || len - off < sizeof(T)) return false;
    std::memcpy(&out, b + off, sizeof(T));
    return true;
}

uint32_t BlockBytes(uint32_t fmt) {
    switch (fmt) {
    case 0x1A01: return 8;    // BC1
    case 0x1C01: return 16;   // BC3
    case 0x1D01: return 8;    // BC4
    case 0x1E01: return 16;   // BC5
    default: return 0;
    }
}

} // namespace

bool Pack::Load(const std::vector<uint8_t>& f, std::string* err) {
    entries_.clear();
    auto fail = [&](const char* m) {
        if (err) *err = m;
        entries_.clear();
        return false;
    };
    if (f.size() < 12 || std::memcmp(f.data(), "PLATECL1", 8) != 0) return fail("header");
    uint32_t n = 0;
    std::memcpy(&n, f.data() + 8, 4);
    size_t o = 12;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t nl = 0;
        if (!Rd(f.data(), f.size(), o, nl)) return fail("truncated");
        o += 2;
        if (f.size() - o < nl) return fail("truncated");
        PackEntry e;
        e.name.assign(reinterpret_cast<const char*>(f.data() + o), nl);
        o += nl;
        uint32_t size = 0;
        if (!Rd(f.data(), f.size(), o, e.fmt) || !Rd(f.data(), f.size(), o + 4, e.w) ||
            !Rd(f.data(), f.size(), o + 8, e.h) || !Rd(f.data(), f.size(), o + 12, size))
            return fail("truncated");
        o += 16;
        if (f.size() - o < size) return fail("truncated");
        e.data.assign(f.begin() + static_cast<ptrdiff_t>(o), f.begin() + static_cast<ptrdiff_t>(o + size));
        o += size;
        entries_[e.name].push_back(std::move(e));
    }
    return true;
}

const PackEntry* Pack::Find(const std::string& name, uint32_t fmt, uint32_t w, uint32_t h, size_t size) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return nullptr;
    for (const PackEntry& e : it->second)
        if (e.fmt == fmt && e.w == w && e.h == h && e.data.size() == size) return &e;
    return nullptr;
}

uint64_t SurfaceSize(uint32_t fmt, uint32_t w, uint32_t h, uint32_t bh_log2) {
    const uint32_t bpp = BlockBytes(fmt);
    if (!bpp || bh_log2 > 5) return 0;
    const uint64_t bw = (w + 3) / 4, bhh = (h + 3) / 4;
    const uint64_t gobs_w = (bw * bpp + 63) / 64;
    const uint64_t rows = 8ull << bh_log2;
    return gobs_w * ((bhh + rows - 1) / rows) * 512ull * (1ull << bh_log2);
}

bool ParseBntx(const uint8_t* b, size_t len, std::vector<BntxTex>& out) {
    out.clear();
    if (len < 0x40 || std::memcmp(b, "BNTX", 4) != 0) return false;
    uint32_t cnt = 0;
    uint64_t infop = 0;
    if (!Rd(b, len, 0x24, cnt) || !Rd(b, len, 0x28, infop) || cnt > 4096) return false;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint64_t o = 0;
        if (!Rd(b, len, infop + 8ull * i, o) || o + 0xA0 > len || std::memcmp(b + o, "BRTI", 4) != 0) return false;
        BntxTex t;
        uint16_t mips = 0;
        int32_t w = 0, h = 0, layout = 0;
        uint64_t nameo = 0, ptrs = 0, m0 = 0;
        uint16_t nl = 0;
        if (!Rd(b, len, o + 0x16, mips) || !Rd(b, len, o + 0x1C, t.fmt) || !Rd(b, len, o + 0x24, w) ||
            !Rd(b, len, o + 0x28, h) || !Rd(b, len, o + 0x34, layout) || !Rd(b, len, o + 0x60, nameo) ||
            !Rd(b, len, o + 0x70, ptrs) || !Rd(b, len, nameo, nl) || !Rd(b, len, ptrs, m0))
            return false;
        if (nameo + 2 + nl > len || w <= 0 || h <= 0) return false;
        t.name.assign(reinterpret_cast<const char*>(b + nameo + 2), nl);
        t.w = static_cast<uint32_t>(w);
        t.h = static_cast<uint32_t>(h);
        t.mips = mips;
        t.bh_log2 = static_cast<uint32_t>(layout) & 7;
        t.data_off = m0;
        t.size = SurfaceSize(t.fmt, t.w, t.h, t.bh_log2);
        out.push_back(std::move(t));
    }
    return true;
}

std::vector<TexHit> MatchBntx(const Pack& pack, const uint8_t* buf, size_t len) {
    std::vector<TexHit> out;
    std::vector<BntxTex> texs;
    if (pack.Empty() || !ParseBntx(buf, len, texs)) return out;
    for (const BntxTex& t : texs) {
        if (!t.size || t.data_off > len || len - t.data_off < t.size) continue;
        if (const PackEntry* e = pack.Find(t.name, t.fmt, t.w, t.h, static_cast<size_t>(t.size)))
            out.push_back({t.data_off, t.size, e});
    }
    return out;
}

int PatchBntx(const Pack& pack, uint8_t* buf, size_t len) {
    int n = 0;
    for (const TexHit& h : MatchBntx(pack, buf, len)) {
        std::memcpy(buf + h.data_off, h.entry->data.data(), h.entry->data.size());
        ++n;
    }
    return n;
}

void LiveSet::Add(uint64_t va, const uint8_t* orig, const uint8_t* kb, size_t size) {
    for (Item& it : items_)
        if (it.va == va) {   // same memory with another file: replace
            it.kb = kb;
            it.orig.assign(orig, orig + size);
            return;
        }
    if (items_.size() >= kMax) items_.erase(items_.begin());
    items_.push_back({va, kb, std::vector<uint8_t>(orig, orig + size)});
}

std::vector<std::pair<uint64_t, uint64_t>> SarcBntxRanges(const uint8_t* d, size_t len) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    if (len < 0x20 || std::memcmp(d, "SARC", 4) != 0) return out;
    uint16_t hl = 0;
    uint32_t dataoff = 0;
    if (!Rd(d, len, 4, hl) || !Rd(d, len, 0x0C, dataoff)) return out;
    if (hl + 12u > len || std::memcmp(d + hl, "SFAT", 4) != 0) return out;
    uint16_t n = 0;
    if (!Rd(d, len, hl + 6u, n)) return out;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t s = 0, e = 0;
        const uint64_t eo = hl + 12ull + 16ull * i;
        if (!Rd(d, len, eo + 8, s) || !Rd(d, len, eo + 12, e)) return out;
        const uint64_t a = static_cast<uint64_t>(dataoff) + s, z = static_cast<uint64_t>(dataoff) + e;
        if (z > len || a + 4 > z || std::memcmp(d + a, "BNTX", 4) != 0) continue;
        out.emplace_back(a, z);
    }
    return out;
}

std::vector<FilePatch> SarcPatches(const Pack& pack, const std::vector<uint8_t>& f) {
    std::vector<FilePatch> out;
    const uint8_t* d = f.data();
    const size_t len = f.size();
    if (pack.Empty()) return out;
    for (const auto& [a, z] : SarcBntxRanges(d, len)) {
        std::vector<BntxTex> texs;
        if (!ParseBntx(d + a, static_cast<size_t>(z - a), texs)) continue;
        for (const BntxTex& t : texs) {
            if (!t.size || t.data_off + t.size > z - a) continue;
            if (const PackEntry* pe = pack.Find(t.name, t.fmt, t.w, t.h, static_cast<size_t>(t.size)))
                out.push_back({a + t.data_off, t.size, pe->data.data()});
        }
    }
    std::sort(out.begin(), out.end(), [](const FilePatch& x, const FilePatch& y) { return x.file_off < y.file_off; });
    return out;
}

uint64_t ApplyPatches(const std::vector<FilePatch>& patches, uint64_t off, uint8_t* dst, uint64_t n) {
    uint64_t copied = 0;
    const uint64_t end = off + n;
    for (const FilePatch& p : patches) {
        const uint64_t a = std::max(off, p.file_off), z = std::min(end, p.file_off + p.size);
        if (a >= z) continue;
        std::memcpy(dst + (a - off), p.src + (a - p.file_off), static_cast<size_t>(z - a));
        copied += z - a;
    }
    return copied;
}

PromptMode PromptModeFromString(const std::string& s) {
    std::string l;
    for (char c : s) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // "teclado" / "mando": legacy aliases (Spanish config values).
    if (l == "keyboard" || l == "teclado" || l == "on") return PromptMode::Keyboard;
    if (l == "controller" || l == "mando" || l == "off" || l == "pad") return PromptMode::Controller;
    return PromptMode::Auto;
}

} // namespace nnshim::ui
