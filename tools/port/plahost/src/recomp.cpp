// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recomp.h"

#include <atomic>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "log.h"
#include "memory.h"

namespace plahost::recomp {

const Image* g_sorted[16];
int g_sorted_count = 0;

namespace {

std::vector<Image> g_images;

constexpr unsigned kAbi = 6;
constexpr unsigned kFeatureFastmem = 1, kFeatureGuardGen = 2, kFeatureFpx = 4, kFeatureFm2 = 8;
constexpr u32 kGuardHostVersion = 2;
constexpr u32 kGenerationStable = 2; // the generation every image verifies once against
constexpr u32 kVerifyAlways = 0xFFFFFFFFu;

// FM2 code-write hook (memory.cpp's fault handler): a write fault on a write-protected code page whose
// instruction lies in a recompiled image is a guest store to code, as FM1's store helpers reported it.
bool CodeWriteFromGuest(u64 va, const void* host_ip) {
    const auto ip = reinterpret_cast<uintptr_t>(host_ip);
    for (const auto& im : g_images) {
        if (ip >= im.dll_lo && ip < im.dll_hi) {
            OnCodeWrite(va);
            return true;
        }
    }
    return false;
}

template <typename F>
F Sym(HMODULE h, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(h, name)));
}

} // namespace

std::vector<Image>& Images() {
    return g_images;
}

const Image* FindImage(const std::string& name) {
    for (const auto& im : g_images) {
        if (im.name == name) return &im;
    }
    return nullptr;
}

bool LoadImages(const std::wstring& dir, std::string* error) {
    struct Want { const wchar_t* dll; const char* name; };
    static const Want kDlls[] = {
        {L"recompiled_rtld.dll", "rtld"},       {L"recompiled_image.dll", "main"},
        {L"recompiled_subsdk0.dll", "subsdk0"}, {L"recompiled_subsdk1.dll", "subsdk1"},
        {L"recompiled_subsdk2.dll", "subsdk2"}, {L"recompiled_subsdk3.dll", "subsdk3"},
        {L"recompiled_subsdk4.dll", "subsdk4"}, {L"recompiled_subsdk5.dll", "subsdk5"},
        {L"recompiled_subsdk6.dll", "subsdk6"}, {L"recompiled_subsdk7.dll", "subsdk7"},
        {L"recompiled_subsdk8.dll", "subsdk8"}, {L"recompiled_subsdk9.dll", "subsdk9"},
        {L"recompiled_sdk.dll", "sdk"},
    };
    unsigned fpx_answer = 0;
    int fpx_modules = -1;
    for (const auto& w : kDlls) {
        const std::wstring path = dir + L"\\" + w.dll;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) {
            const DWORD le = GetLastError();
            *error = std::string{"cannot load recompiled image "} + w.name + " (error " + std::to_string(le) + ")";
            if (le == ERROR_DLL_INIT_FAILED) {
                // recomp_isa_guard (tools/toolchain/crt): built for a newer CPU level than this one.
                *error += ": it was compiled for a newer CPU than this one; run the setup again on this PC";
            }
            return false;
        }
        Image im;
        im.name = w.name;
        im.module = h;
        im.lookup = Sym<BlockFn (*)(u64)>(h, "recomp_image_lookup");
        im.set_base = Sym<void (*)(u64)>(h, "recomp_image_set_base");
        im.run_slice = Sym<BlockFn>(h, "recomp_image_run_slice");
        im.get_index = Sym<int (*)(u64*, u64*, BlockFn**)>(h, "recomp_image_index");
        const auto abi = Sym<unsigned (*)()>(h, "recomp_image_abi");
        const auto features = Sym<unsigned (*)()>(h, "recomp_image_features");
        const auto guard_v2 = Sym<unsigned (*)(unsigned)>(h, "recomp_image_guard_v2");
        const auto gg = Sym<u32* (*)(u32, u64*, u64*, const u64**)>(h, "recomp_image_guard_gen_v1");
        const auto fastmem = Sym<unsigned (*)(u32, u32, u64, u32, u32)>(h, "recomp_image_fastmem_v1");
        const auto fpx = Sym<unsigned (*)(u32, u32, u64)>(h, "recomp_image_fpx_v1");
        const auto fm2 = Sym<unsigned (*)(u64, u32)>(h, "recomp_image_fm2_v1");
        if (!im.lookup || !im.set_base || !im.run_slice || !im.get_index || !abi || !guard_v2) {
            *error = std::string{"recompiled image "} + w.name + " lacks the exports this host needs";
            return false;
        }
        if (abi() != kAbi) {
            *error = std::string{"recompiled image "} + w.name + " is ABI " + std::to_string(abi()) +
                     "; this host needs ABI 6 (re-export with the installer)";
            return false;
        }
        const unsigned f = features ? features() : 0;
        if (f & ~(kFeatureFastmem | kFeatureGuardGen | kFeatureFpx | kFeatureFm2)) {
            *error = std::string{"recompiled image "} + w.name + " needs features this host lacks";
            return false;
        }
        if (guard_v2(0) != 2) {
            *error = std::string{"recompiled image "} + w.name + " refused guard v2";
            return false;
        }
        guard_v2(2);
        if (f & kFeatureGuardGen) {
            const u64* base_ptr = nullptr;
            im.gg_word = gg ? gg(kGuardHostVersion, &im.gg_lo, &im.gg_end, &base_ptr) : nullptr;
            if (!im.gg_word) {
                *error = std::string{"recompiled image "} + w.name + " refused the generation guard";
                return false;
            }
        }
        if (!(f & kFeatureFastmem) || !fastmem ||
            fastmem(kPageBits, kEntryStrideLog2, kEntryPtrMask, offsetof(GuestContext, fm_table),
                    offsetof(GuestContext, fm_limit)) != 1) {
            *error = std::string{"recompiled image "} + w.name + " does not match this host's fastmem layout";
            return false;
        }
        if (f & kFeatureFm2) {
            // The whole 39-bit space is identity-mapped at mem::Base() (memory.cpp); the image takes the base.
            if (!fm2 || fm2(reinterpret_cast<u64>(mem::Base()), 39) != 1) {
                *error = std::string{"recompiled image "} + w.name + " refused the FM2 handshake";
                return false;
            }
            im.fm2 = true;
        }
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const u8*>(h) + dos->e_lfanew);
            im.dll_lo = reinterpret_cast<uintptr_t>(h);
            im.dll_hi = im.dll_lo + nt->OptionalHeader.SizeOfImage;
        }
        const bool has_fpx = (f & kFeatureFpx) != 0;
        if (has_fpx) {
            fpx_answer = fpx ? fpx(offsetof(GuestContext, fpcr), offsetof(GuestContext, fpsr), kFpxInhibit) : 0;
            if (!fpx_answer) {
                *error = std::string{"recompiled image "} + w.name + " refused the FPX1 handshake";
                return false;
            }
        }
        if (fpx_modules >= 0 && fpx_modules != (has_fpx ? 1 : 0)) {
            *error = "recompiled images mix FPX1 and non-FPX1 modules";
            return false;
        }
        fpx_modules = has_fpx ? 1 : 0;
        LOGI("Recomp", "image %-8s loaded (ABI 6, features %#x)", w.name, f);
        g_images.push_back(im);
    }
    if (g_images.empty()) {
        *error = "no recompiled_*.dll beside the executable";
        return false;
    }
    LOGI("Recomp", "%zu images; FPX1 %s (%#x)", g_images.size(), fpx_modules == 1 ? "on" : "off", fpx_answer);
    size_t fm2_images = 0;
    for (const auto& im : g_images) fm2_images += im.fm2 ? 1 : 0;
    if (fm2_images) {
        mem::EnableFm2(CodeWriteFromGuest);
        LOGI("Recomp", "FM2 direct fastmem: %zu of %zu images, base %p", fm2_images, g_images.size(), mem::Base());
        if (const char* t = std::getenv("PLAHOST_FM2_SELFTEST"); t && *t == '1') mem::Fm2SelfTest();
    }
    return true;
}

bool Attach(const std::vector<loader::Module>& modules, std::string* error) {
    for (auto& im : g_images) {
        const loader::Module* m = nullptr;
        for (const auto& mod : modules) {
            if (mod.file == im.name) m = &mod;
        }
        if (!m) {
            *error = "recompiled image " + im.name + " has no module in exefs";
            return false;
        }
        im.base = m->base;
        im.set_base(m->base);
        if (!im.get_index(&im.lo, &im.hi, &im.index) || !im.index) {
            *error = "recompiled image " + im.name + " has no block index";
            return false;
        }
        if (im.gg_word) {
            // Code never changes under this host (no JIT, text pages are only ever read):
            // every block verifies its words once, then skips. Stores to text reach the host.
            mem::Watch(im.base + im.gg_lo, im.gg_end - im.gg_lo);
            // FM2 stores never reach the host: code pages are write-protected instead (no-op without FM2).
            mem::ProtectCode(im.base + im.gg_lo, im.gg_end - im.gg_lo);
            std::atomic_ref<u32>(*im.gg_word).store(kGenerationStable, std::memory_order_release);
        }
        LOGI("Recomp", "image %-8s base %#llx index %#llx-%#llx", im.name.c_str(), im.base, im.lo, im.hi);
    }
    g_sorted_count = 0;
    for (auto& im : g_images) {
        if (g_sorted_count < 16) g_sorted[g_sorted_count++] = &im;
    }
    return true;
}

bool Covered(u64 pc) {
    const Image* im = ImageFor(pc);
    if (!im || ((pc - im->lo) & 3)) return false;
    return im->index[(pc - im->lo) >> 2] != nullptr;
}

void OnCodeWrite(u64 va) {
    static std::atomic<int> logs{0};
    for (auto& im : g_images) {
        if (im.gg_word && va >= im.base + im.gg_lo && va < im.base + im.gg_end) {
            if (logs.fetch_add(1) < 8) {
                LOGW("Recomp", "guest wrote code at %#llx (%s): its blocks verify again", va, im.name.c_str());
            }
            std::atomic_ref<u32>(*im.gg_word).store(kVerifyAlways, std::memory_order_release);
        }
    }
}

} // namespace plahost::recomp
