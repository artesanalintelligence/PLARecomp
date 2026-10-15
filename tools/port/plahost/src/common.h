// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// plahost: the game's own host executable (stage 6 of docs/port/V15-DESIGN.md).
// Shared types: the recompiled images' guest context (ABI 6) and small helpers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

namespace plahost {

// ---- recompiled image ABI 6 (recomp_runtime.h of an export) -----------------------------

using BlockFn = void (*)(void*);

// RecompHostMem of the generated runtime: the host side of every guest access the inline
// page-table path declines (unmapped pages, watched code pages, page crossings).
struct RecompHostMem {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
    u64 (*excl_load)(void* user, u64 va, u32 size);
    u32 (*excl_store)(void* user, u64 va, u32 size, u64 value);
    void (*clear_excl)(void* user);
    u64 (*read_cntpct)(void* user);
    void (*excl_load_pair)(void* user, u64 va, u32 size, u64* lo, u64* hi);
    u32 (*excl_store_pair)(void* user, u64 va, u32 size, u64 lo, u64 hi);
    const void* page_entries;
    u64 page_entry_stride;
    u64 page_bits;
    u64 pointer_mask;
    u64 address_space_max;
    const u64* guard_generation;
};
static_assert(offsetof(RecompHostMem, excl_load_pair) == 56);
static_assert(offsetof(RecompHostMem, page_entries) == 72);
static_assert(offsetof(RecompHostMem, guard_generation) == 112);
static_assert(sizeof(RecompHostMem) == 120);

// The generated GuestContext, in full (the tail is the standalone runtime's and unused here,
// but the images may assume the size).
struct GuestContext {
    u64 x[32]; // x[31] = SP
    u64 pc;
    u8 n, z, c, v;
    u8* mem;
    u64 mem_size;
    u64 mem_base_vaddr;
    int halted;
    u64 pending_svc;
    u64 vreg[32][2];
    u64 tpidr_el0;
    const RecompHostMem* host_mem;
    u64 tpidrro_el0;
    u64 fpcr;
    u64 fpsr;
    int chain_budget;
    u32 fm_reserved;
    const unsigned char* fm_table;
    u64 fm_limit;
    char save_dir[512];
    u64 heap_base, heap_end, heap_cur;
    u32 ipc_cmd[64];
    void* save_handles[16];
    int save_handle_count;
};
static_assert(offsetof(GuestContext, pc) == 256);
static_assert(offsetof(GuestContext, halted) == 296);
static_assert(offsetof(GuestContext, pending_svc) == 304);
static_assert(offsetof(GuestContext, vreg) == 312);
static_assert(offsetof(GuestContext, tpidr_el0) == 824);
static_assert(offsetof(GuestContext, host_mem) == 832);
static_assert(offsetof(GuestContext, tpidrro_el0) == 840);
static_assert(offsetof(GuestContext, fpcr) == 848);
static_assert(offsetof(GuestContext, fpsr) == 856);
static_assert(offsetof(GuestContext, chain_budget) == 864);
static_assert(offsetof(GuestContext, fm_table) == 872);
static_assert(offsetof(GuestContext, fm_limit) == 880);

inline constexpr u64 kNoPendingSvc = ~0ULL;
inline constexpr int kHaltUnhandled = 2;
inline constexpr int kHaltBreakpoint = 3;
inline constexpr int kHaltIcIvau = 4;
// The return address a post-hooked call returns to (not a guest address).
inline constexpr u64 kReturnTrap = 0xFFFF'FFFE'0000'0000ULL;
// Target of an import nothing defines (calls return 0).
inline constexpr u64 kUnresolvedImportTrap = 0xFFFF'FFFF'0000'0000ULL;

// Page table layout the FM1 helpers fold in (RECOMP_FM_*): 4 KiB pages, 32-byte entries,
// pointer in bits 2-63 of word 0, GG1 watch word at +24.
inline constexpr u32 kPageBits = 12;
inline constexpr u64 kPageSize = 1ULL << kPageBits;
inline constexpr u32 kEntryStrideLog2 = 5;
inline constexpr u64 kEntryPtrMask = ~3ULL;
inline constexpr u64 kWatchOffset = 24;
inline constexpr u64 kFpxInhibit = 1ULL << 32;

struct PageEntry {
    u64 ptr;   // (host page - guest page) | attribute, 0 = unmapped
    u64 rsv1;
    u64 rsv2;
    u64 watch; // GG1: nonzero = stores go to the host
};
static_assert(sizeof(PageEntry) == 32);

// ---- Horizon results -------------------------------------------------------------------

using Result = u32;
inline constexpr Result kOk = 0;
constexpr Result MakeResult(u32 module, u32 desc) {
    return module | (desc << 9);
}
// Kernel (module 1)
inline constexpr Result kResultOutOfSessions = MakeResult(1, 7);
inline constexpr Result kResultInvalidArgument = MakeResult(1, 14);
inline constexpr Result kResultNotImplemented = MakeResult(1, 33);
inline constexpr Result kResultStopProcessingException = MakeResult(1, 54);
inline constexpr Result kResultNoSynchronizationObject = MakeResult(1, 57);
inline constexpr Result kResultTerminationRequested = MakeResult(1, 59);
inline constexpr Result kResultNoEvent = MakeResult(1, 70);
inline constexpr Result kResultInvalidSize = MakeResult(1, 101);
inline constexpr Result kResultInvalidAddress = MakeResult(1, 102);
inline constexpr Result kResultOutOfResource = MakeResult(1, 103);
inline constexpr Result kResultOutOfMemory = MakeResult(1, 104);
inline constexpr Result kResultOutOfHandles = MakeResult(1, 105);
inline constexpr Result kResultInvalidCurrentMemory = MakeResult(1, 106);
inline constexpr Result kResultInvalidNewMemoryPermission = MakeResult(1, 108);
inline constexpr Result kResultInvalidMemoryRegion = MakeResult(1, 110);
inline constexpr Result kResultInvalidPriority = MakeResult(1, 112);
inline constexpr Result kResultInvalidCoreId = MakeResult(1, 113);
inline constexpr Result kResultInvalidHandle = MakeResult(1, 114);
inline constexpr Result kResultInvalidPointer = MakeResult(1, 115);
inline constexpr Result kResultInvalidCombination = MakeResult(1, 116);
inline constexpr Result kResultTimedOut = MakeResult(1, 117);
inline constexpr Result kResultCancelled = MakeResult(1, 118);
inline constexpr Result kResultOutOfRange = MakeResult(1, 119);
inline constexpr Result kResultInvalidEnumValue = MakeResult(1, 120);
inline constexpr Result kResultNotFound = MakeResult(1, 121);
inline constexpr Result kResultBusy = MakeResult(1, 122);
inline constexpr Result kResultSessionClosed = MakeResult(1, 123);
inline constexpr Result kResultInvalidState = MakeResult(1, 125);
inline constexpr Result kResultLimitReached = MakeResult(1, 132);

inline u64 AlignUp(u64 v, u64 a) {
    return (v + a - 1) & ~(a - 1);
}
inline u64 AlignDown(u64 v, u64 a) {
    return v & ~(a - 1);
}

} // namespace plahost
