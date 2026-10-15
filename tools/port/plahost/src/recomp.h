// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The recompiled images (recompiled_*.dll beside the exe): loading, the ABI 6 handshakes
// (FM1 fastmem page table, GG1 generation guard, FPX1 native FP, FM2 direct fastmem, guard v2) and lookup.

#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "loader.h"

namespace plahost::recomp {

struct Image {
    std::string name; // "rtld", "main", "subsdk0".., "sdk" (exefs names)
    void* module = nullptr;
    BlockFn (*lookup)(u64) = nullptr;
    void (*set_base)(u64) = nullptr;
    BlockFn run_slice = nullptr;
    int (*get_index)(u64*, u64*, BlockFn**) = nullptr;
    u32* gg_word = nullptr;
    u64 gg_lo = 0, gg_end = 0;
    u64 base = 0;
    u64 lo = 0, hi = 0; // absolute span of the block index
    bool fm2 = false;   // FM2 image: plain host accesses at mem::Base() + va
    uintptr_t dll_lo = 0, dll_hi = 0; // the DLL's own image in host memory
    BlockFn* index = nullptr;
};

bool LoadImages(const std::wstring& dir, std::string* error);
/// Tells every image its module's base and activates the generation guard. Must run before
/// any block executes.
bool Attach(const std::vector<loader::Module>& modules, std::string* error);

std::vector<Image>& Images();
const Image* FindImage(const std::string& name);

/// The image whose block index covers `pc`, or nullptr.
inline const Image* ImageFor(u64 pc);

/// A covered block's slot is non-null.
bool Covered(u64 pc);

/// Called after a store to a watched (code) page: code changed under the images.
void OnCodeWrite(u64 va);

// ---- inline lookup ---
extern const Image* g_sorted[16];
extern int g_sorted_count;

inline const Image* ImageFor(u64 pc) {
    for (int i = 0; i < g_sorted_count; ++i) {
        const Image* im = g_sorted[i];
        if (pc >= im->lo && pc <= im->hi) {
            return im;
        }
    }
    return nullptr;
}

} // namespace plahost::recomp
