// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loads the user's exefs (the NSO modules and main.npdm) into guest memory at the bases the
// recompiled images are told about, and relocates rtld itself (rtld relocates the rest, as on
// the console).

#pragma once

#include <array>
#include <string>
#include <vector>

#include "common.h"

namespace plahost::loader {

struct Module {
    std::string file;  // exefs file name: rtld, main, subsdk0.., sdk
    u64 base = 0;
    u64 size = 0;      // whole image (page aligned, with .bss)
    u64 text_size = 0; // page aligned
    u64 ro_offset = 0, ro_size = 0;
    u64 data_offset = 0, data_size = 0; // .data + .bss, page aligned
    std::array<u8, 0x20> build_id{};
};

struct Npdm {
    bool is_64bit = true;
    u32 address_space = 3;
    u8 main_thread_priority = 44;
    u8 main_thread_core = 0;
    u32 system_resource_size = 0;
    u32 main_thread_stack_size = 0x100000;
    u64 title_id = 0;
};

/// Reads exefs/main.npdm.
bool LoadNpdm(const std::wstring& exefs, Npdm& out, std::string* error);
/// Maps every NSO present (rtld, main, subsdk0-9, sdk) from kCodeBase on.
bool LoadModules(const std::wstring& exefs, std::vector<Module>& out, std::string* error);
/// rtld's own relocations (its bootstrap loop is the part recompiled code cannot redo), then
/// DT_RELASZ/DT_PLTRELSZ zeroed so it does not try again.
void RelocateRtld(const Module& rtld);

std::string BuildIdHex(const Module& m);

} // namespace plahost::loader
