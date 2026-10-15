// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rtld relocation pass below follows the one in the fork's ArmRecomp (core/arm/recomp,
// PLARecomp code, GPL): same symbol handling, same "rtld-only" policy.

#include "loader.h"

#include <cstdio>
#include <fstream>
#include <unordered_map>

#include "log.h"
#include "memory.h"

namespace plahost::loader {
namespace {

std::vector<u8> ReadFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <typename T>
T Get(const std::vector<u8>& d, size_t off) {
    T v{};
    if (off + sizeof(T) <= d.size()) {
        std::memcpy(&v, d.data() + off, sizeof(T));
    }
    return v;
}

// LZ4 block format (no frame).
bool Lz4Decompress(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    size_t ip = 0, op = 0;
    while (ip < src_size) {
        const u8 token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            u8 b;
            do {
                if (ip >= src_size) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > src_size || op + lit > dst_size) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= src_size) {
            break; // last sequence has no match
        }
        if (ip + 2 > src_size) return false;
        const size_t offset = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return false;
        size_t len = (token & 15);
        if (len == 15) {
            u8 b;
            do {
                if (ip >= src_size) return false;
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (op + len > dst_size) return false;
        for (size_t k = 0; k < len; ++k) {
            dst[op + k] = dst[op - offset + k];
        }
        op += len;
    }
    return op == dst_size;
}

// ---- rtld relocation ----



struct DynInfo {
    u64 mod_base = 0;
    u64 rela_va = 0, rela_sz = 0, rela_ent = 24, rela_sz_va = 0;
    u64 jmprel_va = 0, jmprel_sz = 0, jmprel_ent = 24, jmprel_sz_va = 0;
    u64 symtab_va = 0, strtab_va = 0;
    u64 trap_va = 0;
    std::string name;
};

u32 R32(u64 va) { return mem::Load<u32>(va); }
u64 R64(u64 va) { return mem::Load<u64>(va); }

u64 FindReturnStub(u64 base) {
    constexpr u32 kMovX0Zero = 0xD2800000, kRet = 0xD65F03C0;
    u64 bare = 0;
    for (u64 off = 0; off < 0x100000 && mem::IsMapped(base + off, 8); off += 4) {
        const u32 insn = R32(base + off);
        if (insn == kRet) {
            if (!bare) bare = base + off;
        } else if (insn == kMovX0Zero && R32(base + off + 4) == kRet) {
            return base + off;
        }
    }
    return bare;
}

bool ParseDynamic(u64 base, DynInfo& out) {
    u64 mod0 = 0;
    for (u64 off = 0; off < 0x4000; off += 4) {
        if (R32(base + off) == 0x30444F4Du) {
            mod0 = base + off;
            break;
        }
    }
    if (!mod0) return false;
    const u64 dyn = mod0 + R32(mod0 + 4);
    constexpr u64 DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
                  DT_RELAENT = 9, DT_PLTREL = 20, DT_JMPREL = 23, DT_REL = 17;
    out.mod_base = base;
    u64 pltrel = DT_RELA;
    for (u64 p = dyn;; p += 16) {
        const u64 tag = R64(p), val = R64(p + 8);
        if (tag == DT_NULL) break;
        if (tag == DT_RELA) out.rela_va = base + val;
        if (tag == DT_RELASZ) { out.rela_sz = val; out.rela_sz_va = p + 8; }
        if (tag == DT_RELAENT) out.rela_ent = val;
        if (tag == DT_JMPREL) out.jmprel_va = base + val;
        if (tag == DT_PLTRELSZ) { out.jmprel_sz = val; out.jmprel_sz_va = p + 8; }
        if (tag == DT_PLTREL) pltrel = val;
        if (tag == DT_SYMTAB) out.symtab_va = base + val;
        if (tag == DT_STRTAB) out.strtab_va = base + val;
        if (p - dyn > 0x1000) break;
    }
    out.jmprel_ent = pltrel == DT_REL ? 16 : 24;
    return true;
}

struct Sym {
    std::string name;
    u64 value = 0;
    bool defined = false;
    bool weak = false;
};

Sym ReadSym(const DynInfo& d, u32 index) {
    Sym s;
    if (!d.symtab_va) return s;
    const u64 va = d.symtab_va + u64{index} * 24;
    const u32 name_off = R32(va);
    s.defined = mem::Load<u16>(va + 6) != 0;
    s.value = R64(va + 8);
    s.weak = (mem::Load<u8>(va + 4) >> 4) == 2;
    if (d.strtab_va) {
        s.name = mem::ReadString(d.strtab_va + name_off, 512);
    }
    return s;
}

void IndexExports(const DynInfo& d, std::unordered_map<std::string, u64>& out) {
    if (!d.symtab_va || !d.strtab_va) return;
    u32 count = 8192;
    if (d.strtab_va > d.symtab_va) {
        count = static_cast<u32>(std::min<u64>((d.strtab_va - d.symtab_va) / 24, 65536));
    }
    for (u32 i = 1; i < count; ++i) {
        const Sym s = ReadSym(d, i);
        if (s.defined && !s.name.empty()) {
            out.emplace(s.name, d.mod_base + s.value);
        }
    }
}

void ApplyTable(const DynInfo& d, u64 table, u64 size, u64 ent,
                const std::unordered_map<std::string, u64>& exports, u32& applied, u32& unresolved) {
    constexpr u32 ABS64 = 0x101, GLOB_DAT = 0x401, JUMP_SLOT = 0x402, RELATIVE = 0x403, IRELATIVE = 0x408;
    for (u64 p = table; p < table + size; p += ent) {
        const u64 off = R64(p), info = R64(p + 8), addend = ent == 24 ? R64(p + 16) : 0;
        const u32 type = static_cast<u32>(info), sym = static_cast<u32>(info >> 32);
        const u64 where = d.mod_base + off;
        if (type == RELATIVE) {
            mem::StoreTo<u64>(where, d.mod_base + addend);
            ++applied;
        } else if (type == IRELATIVE) {
            LOGE("Loader", "IRELATIVE relocation at %s+%#llx not supported; trapped", d.name.c_str(), off);
            mem::StoreTo<u64>(where, d.trap_va ? d.trap_va : kUnresolvedImportTrap);
        } else if (type == GLOB_DAT || type == JUMP_SLOT || type == ABS64) {
            const Sym s = ReadSym(d, sym);
            const u64 add = type == ABS64 ? addend : 0;
            u64 synthetic = 0;
            bool is_synth = true;
            if (s.name == "__rela_dyn_start" || s.name == "__rel_dyn_start") synthetic = d.rela_va;
            else if (s.name == "__rela_dyn_end" || s.name == "__rel_dyn_end") synthetic = d.rela_va + d.rela_sz;
            else if (s.name == "__rela_plt_start" || s.name == "__rel_plt_start") synthetic = d.jmprel_va;
            else if (s.name == "__rela_plt_end" || s.name == "__rel_plt_end") synthetic = d.jmprel_va + d.jmprel_sz;
            else is_synth = false;
            if (is_synth && synthetic) {
                mem::StoreTo<u64>(where, synthetic + add);
                ++applied;
            } else if (s.defined || s.value != 0) {
                mem::StoreTo<u64>(where, d.mod_base + s.value + add);
                ++applied;
            } else if (auto it = exports.find(s.name); it != exports.end()) {
                mem::StoreTo<u64>(where, it->second + add);
                ++applied;
            } else if (type == ABS64 && sym == 0) {
                mem::StoreTo<u64>(where, add);
                ++applied;
            } else {
                ++unresolved;
                if (unresolved <= 30) {
                    LOGW("Loader", "unresolved symbol '%s' in %s", s.name.c_str(), d.name.c_str());
                }
                mem::StoreTo<u64>(where, s.weak ? 0 : (d.trap_va ? d.trap_va : kUnresolvedImportTrap));
            }
        }
    }
}

std::vector<Module> g_all;

} // namespace

bool LoadNpdm(const std::wstring& exefs, Npdm& out, std::string* error) {
    const auto d = ReadFile(exefs + L"\\main.npdm");
    if (d.size() < 0x80 || std::memcmp(d.data(), "META", 4) != 0) {
        *error = "exefs\\main.npdm is missing or invalid";
        return false;
    }
    const u8 flags = d[0xC];
    out.is_64bit = flags & 1;
    out.address_space = (flags >> 1) & 7;
    out.main_thread_priority = d[0xE];
    out.main_thread_core = d[0xF];
    out.system_resource_size = Get<u32>(d, 0x14);
    out.main_thread_stack_size = Get<u32>(d, 0x1C);
    const u32 aci = Get<u32>(d, 0x70);
    if (aci + 0x18 <= d.size() && std::memcmp(d.data() + aci, "ACI0", 4) == 0) {
        out.title_id = Get<u64>(d, aci + 0x10);
    }
    LOGI("Loader", "npdm: title %016llX 64-bit=%d address space=%u priority=%u core=%u stack=%#x resource=%#x",
         out.title_id, out.is_64bit, out.address_space, out.main_thread_priority, out.main_thread_core,
         out.main_thread_stack_size, out.system_resource_size);
    return true;
}

bool LoadModules(const std::wstring& exefs, std::vector<Module>& out, std::string* error) {
    static const char* const kOrder[] = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                         "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                         "subsdk8", "subsdk9", "sdk"};
    u64 next = mem::kCodeBase;
    for (const char* name : kOrder) {
        std::wstring wname(name, name + std::strlen(name));
        const auto d = ReadFile(exefs + L"\\" + wname);
        if (d.empty()) {
            continue;
        }
        if (d.size() < 0x100 || std::memcmp(d.data(), "NSO0", 4) != 0) {
            *error = std::string{"exefs\\"} + name + " is not an NSO";
            return false;
        }
        const u32 flags = Get<u32>(d, 0xC);
        struct Seg { u32 file_off, mem_off, size, extra, csize; } seg[3];
        for (int i = 0; i < 3; ++i) {
            seg[i] = {Get<u32>(d, 0x10 + i * 0x10), Get<u32>(d, 0x14 + i * 0x10), Get<u32>(d, 0x18 + i * 0x10),
                      Get<u32>(d, 0x1C + i * 0x10), Get<u32>(d, 0x60 + i * 4)};
        }
        const u64 bss = seg[2].extra;
        const u64 image = AlignUp(u64{seg[2].mem_off} + seg[2].size + bss, kPageSize);
        Module m;
        m.file = name;
        m.base = next;
        m.size = image;
        m.text_size = AlignUp(seg[0].size, kPageSize);
        m.ro_offset = seg[1].mem_off;
        m.ro_size = AlignUp(seg[1].size, kPageSize);
        m.data_offset = seg[2].mem_off;
        m.data_size = image - seg[2].mem_off;
        std::memcpy(m.build_id.data(), d.data() + 0x40, 0x20);
        if (mem::Map(m.base, image, mem::Code, mem::RW) != kOk) {
            *error = std::string{"cannot map "} + name;
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            if (u64{seg[i].file_off} + seg[i].csize > d.size() || u64{seg[i].mem_off} + seg[i].size > image) {
                *error = std::string{"exefs\\"} + name + ": bad segment";
                return false;
            }
            u8* dst = mem::Ptr(m.base + seg[i].mem_off);
            if (flags & (1u << i)) {
                if (!Lz4Decompress(d.data() + seg[i].file_off, seg[i].csize, dst, seg[i].size)) {
                    *error = std::string{"exefs\\"} + name + ": LZ4 data is corrupt";
                    return false;
                }
            } else {
                std::memcpy(dst, d.data() + seg[i].file_off, seg[i].size);
            }
        }
        // Permissions as the kernel sets them: .text RX, .rodata R, .data/.bss RW (state CodeData).
        mem::SetBlock(m.base, m.text_size, mem::Code, mem::RX, 0);
        mem::SetBlock(m.base + m.ro_offset, m.data_offset - m.ro_offset, mem::Code, mem::R, 0);
        mem::SetBlock(m.base + m.data_offset, m.data_size, mem::CodeData, mem::RW, 0);
        LOGI("Loader", "%-8s at %#llx size %#llx (text %#llx ro %#llx data+bss %#llx) build %s", name, m.base,
             m.size, m.text_size, m.ro_size, m.data_size, BuildIdHex(m).c_str());
        out.push_back(m);
        next = m.base + image;
    }
    if (out.empty() || out.front().file != "rtld") {
        *error = "exefs has no rtld";
        return false;
    }
    g_all = out;
    return true;
}

void RelocateRtld(const Module& rtld) {
    std::vector<DynInfo> dyns;
    std::unordered_map<std::string, u64> exports;
    for (const auto& m : g_all) {
        DynInfo d;
        if (ParseDynamic(m.base, d)) {
            d.trap_va = FindReturnStub(m.base);
            d.name = m.file;
            IndexExports(d, exports);
            dyns.push_back(d);
        }
    }
    for (const auto& d : dyns) {
        if (d.mod_base != rtld.base) {
            continue;
        }
        u32 applied = 0, unresolved = 0;
        if (d.rela_va && d.rela_sz) ApplyTable(d, d.rela_va, d.rela_sz, d.rela_ent, exports, applied, unresolved);
        if (d.jmprel_va && d.jmprel_sz) ApplyTable(d, d.jmprel_va, d.jmprel_sz, d.jmprel_ent, exports, applied, unresolved);
        if (d.rela_sz_va) mem::StoreTo<u64>(d.rela_sz_va, 0);
        if (d.jmprel_sz_va) mem::StoreTo<u64>(d.jmprel_sz_va, 0);
        LOGI("Loader", "rtld: %u relocations applied (%u unresolved); the other modules are left to rtld",
             applied, unresolved);
    }
}

std::string BuildIdHex(const Module& m) {
    std::string s;
    char b[3];
    for (u8 c : m.build_id) {
        std::snprintf(b, sizeof b, "%02X", c);
        s += b;
    }
    return s;
}

} // namespace plahost::loader
