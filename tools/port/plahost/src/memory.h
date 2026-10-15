// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The guest address space: one 39-bit host placeholder reservation where guest page va lives at
// base + va (what the recompiled images' fastmem path and every stored host pointer assume), the
// page table the images read (lazily committed), and a block map with Horizon's memory states for
// svcQueryMemory. Guest pages are views of one "physical" backing section (also mapped whole),
// as on the console: aliases (svcMapMemory) share pages, and a native renderer can import the
// whole backing view once (nvnvk's host_backing_pointer).

#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace plahost::mem {

enum State : u32 {
    Free = 0x00,
    Io = 0x01,
    Static = 0x02,
    Code = 0x03,
    CodeData = 0x04,
    Normal = 0x05,
    Shared = 0x06,
    AliasCode = 0x08,
    AliasCodeData = 0x09,
    Ipc = 0x0A,
    Stack = 0x0B,
    ThreadLocal = 0x0C,
    Transfered = 0x0D,
    SharedTransfered = 0x0E,
    SharedCode = 0x0F,
    Inaccessible = 0x10,
};

enum Perm : u32 { None = 0, R = 1, W = 2, X = 4, RW = 3, RX = 5 };

enum Attr : u32 { Locked = 1, IpcLocked = 2, DeviceShared = 4, Uncached = 8 };

struct Block {
    u64 base = 0;
    u64 size = 0;
    u32 state = Free;
    u32 perm = None;
    u32 attr = 0;
};

// Region layout (39-bit address space, as Horizon lays it out; fixed instead of randomized).
inline constexpr u64 kAddressSpaceEnd = 1ULL << 39;
inline constexpr u64 kAslrStart = 0x8000000;                  // Map39Bit / alias code region
inline constexpr u64 kAslrSize = kAddressSpaceEnd - kAslrStart;
inline constexpr u64 kCodeBase = 0x80000000;                   // first module (rtld)
inline constexpr u64 kAliasStart = 0x10'0000'0000;             // 64 GiB
inline constexpr u64 kAliasSize = 64ULL << 30;
inline constexpr u64 kHeapStart = kAliasStart + kAliasSize;    // 8 GiB
inline constexpr u64 kHeapSize = 8ULL << 30;
inline constexpr u64 kStackStart = kHeapStart + kHeapSize;     // 2 GiB
inline constexpr u64 kStackSize = 2ULL << 30;
inline constexpr u64 kKernelMapStart = 0x24'0000'0000;         // 64 GiB
inline constexpr u64 kKernelMapSize = 64ULL << 30;

/// `backing_bytes`: size of the physical backing (all guest memory together).
bool Init(u64 backing_bytes, std::string* error);

u8* Base();
inline u8* Ptr(u64 va) {
    return Base() + va;
}
const PageEntry* PageTable();

/// True if every page of [va, va+size) is mapped (guest-accessible).
bool IsMapped(u64 va, u64 size);
/// Host pointer for a mapped range, or nullptr.
void* HostPointer(u64 va, u64 size);
/// Pointer into the backing view for [va, va+size) if those pages are contiguous there.
void* BackingPointer(u64 va, u64 size);

/// Maps fresh zeroed memory at [va, va+size), which must be Free.
Result Map(u64 va, u64 size, u32 state, u32 perm);
/// Unmaps [va, va+size) (its physical pages are zeroed and freed unless still aliased).
Result Unmap(u64 va, u64 size);
/// Maps the physical pages of [src, src+size) again at dst (Free), as svcMapMemory does.
Result MapAlias(u64 dst, u64 src, u64 size, u32 state, u32 perm);
/// Changes the recorded state/permission/attribute of a mapped range (no data change).
void SetBlock(u64 va, u64 size, u32 state, u32 perm, u32 attr);
Block Query(u64 va);
/// First free range of `size` bytes inside [start, start+region) aligned to `align`.
u64 FindFree(u64 start, u64 region, u64 size, u64 align = kPageSize);
/// Maps `size` bytes of `state` memory in a region (searching for room); 0 on failure.
u64 MapAnywhere(u64 region_start, u64 region_size, u64 size, u32 state, u32 perm);

// Heap and physical memory (svcSetHeapSize / svcMapPhysicalMemory).
Result SetHeapSize(u64 size, u64* out_addr);
Result MapPhysical(u64 va, u64 size);
Result UnmapPhysical(u64 va, u64 size);
u64 NormalMemorySize(); // heap + physical memory (for GetInfo)
u64 HeapSize();

/// GG1: marks [va, va+size) as watched code (stores reach the host).
void Watch(u64 va, u64 size);
bool IsWatched(u64 va);

// ---- FM2 (direct fastmem) ----
// FM2 images access guest memory at Base() + (va & (2^39 - 1)) with no check of their own. With FM2 on:
// - an access to an unmapped guest page (or to the 64 KiB guard past the arena) faults on the host and the
//   fault handler maps a private zero "scratch" page there (logged, counted); Map/MapAlias replace it with
//   real memory when the guest maps that page later;
// - watched code pages are write-protected (ProtectCode); a write fault on one makes the page writable again
//   and, if the writer was recompiled guest code, calls the code-write hook (GG1 generation), as the FM1
//   store helpers did through the host callback.
inline constexpr u64 kArenaGuard = 0x10000;
/// `code_write(va, host_ip)` returns true if the faulting instruction was guest code and handled the write.
void EnableFm2(bool (*code_write)(u64 va, const void* host_ip));
bool Fm2();
/// Write-protects the mapped pages of [va, va+size) (FM2 only; call after Watch).
void ProtectCode(u64 va, u64 size);
/// Unmapped guest pages that received a scratch page so far.
u64 ScratchPages();
/// Diagnostics (PLAHOST_FM2_SELFTEST=1): unmapped read/write, replacement by Map, guard past 2^39.
bool Fm2SelfTest();

// Checked copies (page by page; return the bytes copied).
size_t Read(u64 va, void* dst, size_t size);
size_t Write(u64 va, const void* src, size_t size);
std::string ReadString(u64 va, size_t max);

template <typename T>
T Load(u64 va) {
    T v{};
    std::memcpy(&v, Ptr(va), sizeof(T));
    return v;
}
template <typename T>
void StoreTo(u64 va, T v) {
    std::memcpy(Ptr(va), &v, sizeof(T));
}

/// Every block (diagnostics).
std::vector<Block> Blocks();

} // namespace plahost::mem
