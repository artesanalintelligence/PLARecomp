// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "memory.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "kernel.h"
#include "log.h"

#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif

namespace plahost::mem {
namespace {

using PFN_VirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using PFN_MapViewOfFile3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

PFN_VirtualAlloc2 pVirtualAlloc2 = nullptr;
PFN_MapViewOfFile3 pMapViewOfFile3 = nullptr;
PFN_UnmapViewOfFile2 pUnmapViewOfFile2 = nullptr;

constexpr u64 kPages = kAddressSpaceEnd >> kPageBits;
constexpr u64 kTableBytes = kPages * sizeof(PageEntry);

u8* g_base = nullptr;
PageEntry* g_table = nullptr;
std::recursive_mutex g_lock;
std::map<u64, Block> g_blocks; // keyed by base, covers [0, kAddressSpaceEnd)
u64 g_heap_size = 0;
u64 g_physical = 0;

// Physical backing.
HANDLE g_section = nullptr;
u8* g_backing = nullptr;
u64 g_backing_size = 0;
std::map<u64, u64> g_phys_free; // page index -> page count
std::vector<u16> g_phys_refs;   // per physical page
u64 g_phys_used = 0;

// Views mapped in the arena (guest va -> {size, physical offset}) and free placeholders.
struct View {
    u64 size;
    u64 phys;
};
std::map<u64, View> g_views;
std::map<u64, u64> g_free_ph; // guest va -> size

// FM2: unmapped guest pages that got a private zero page, and the code-write hook.
constexpr u64 kArenaBytes = kAddressSpaceEnd + kArenaGuard;
std::atomic<bool> g_fm2{false};
bool (*g_code_write)(u64, const void*) = nullptr;
std::set<u64> g_scratch;
std::atomic<u64> g_scratch_count{0};

bool Carve(u64 va, u64 size);
void ReturnPlaceholder(u64 va, u64 size);
LONG ArenaFault(EXCEPTION_POINTERS* ep, u64 va);
void CommitTableBytes(u64 lo, u64 n);

LONG CALLBACK TableFault(EXCEPTION_POINTERS* ep) {
    const auto* rec = ep->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto addr = static_cast<u8*>(reinterpret_cast<void*>(rec->ExceptionInformation[1]));
    const auto* t = reinterpret_cast<u8*>(g_table);
    if (!t || addr < t || addr >= t + kTableBytes) {
        if (g_fm2.load(std::memory_order_relaxed) && g_base && addr >= g_base && addr < g_base + kArenaBytes) {
            return ArenaFault(ep, static_cast<u64>(addr - g_base));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CommitTableBytes(AlignDown(static_cast<u64>(addr - t), kPageSize), kPageSize);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Commits page-table bytes [lo, lo+n). A commit fails while the system is out of commit (the page file is still
// growing, or other processes hold it); that used to be ignored and turned into an access violation on the first
// write to the entries. Retry for about 2 s, then end the process with a clear message.
void CommitTableBytes(u64 lo, u64 n) {
    void* const p = reinterpret_cast<u8*>(g_table) + lo;
    for (int i = 0;; ++i) {
        if (VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE)) {
            if (i) LOGW("Memory", "page table commit of %llu KiB succeeded after %d retries", n >> 10, i);
            return;
        }
        const DWORD err = GetLastError();
        if (i == 50) {
            char msg[384];
            std::snprintf(msg, sizeof msg,
                          "Out of memory: Windows could not commit %llu KiB for the guest page table (error %lu). "
                          "Close other programs or enlarge the page file (System > About > Advanced system settings > "
                          "Performance > Virtual memory) and start the game again.",
                          static_cast<unsigned long long>(n >> 10), err);
            LOGC("Memory", "%s", msg);
            log::Flush();
            MessageBoxA(nullptr, msg, "PLARecomp", MB_ICONERROR | MB_OK | MB_SYSTEMMODAL);
            TerminateProcess(GetCurrentProcess(), 2);
        }
        Sleep(i < 10 ? 1 : 40);
    }
}

void CommitTable(u64 va, u64 size) {
    const u64 first = (va >> kPageBits) * sizeof(PageEntry);
    const u64 last = (((va + size - 1) >> kPageBits) + 1) * sizeof(PageEntry);
    const u64 lo = AlignDown(first, kPageSize);
    const u64 hi = AlignUp(last, kPageSize);
    CommitTableBytes(lo, hi - lo);
}

// ---- physical pages ----

struct Run {
    u64 page; // physical page index
    u64 count;
};

bool AllocPhys(u64 pages, std::vector<Run>& out) {
    // One contiguous run when possible (a native renderer imports pools page-run by page-run).
    for (auto it = g_phys_free.begin(); it != g_phys_free.end(); ++it) {
        if (it->second >= pages) {
            const u64 at = it->first, len = it->second;
            g_phys_free.erase(it);
            if (len > pages) g_phys_free[at + pages] = len - pages;
            out.push_back({at, pages});
            return true;
        }
    }
    u64 left = pages;
    while (left && !g_phys_free.empty()) {
        auto it = g_phys_free.begin();
        const u64 at = it->first, len = it->second;
        const u64 take = std::min(len, left);
        g_phys_free.erase(it);
        if (len > take) g_phys_free[at + take] = len - take;
        out.push_back({at, take});
        left -= take;
    }
    if (left) {
        for (const auto& r : out) {
            g_phys_free[r.page] = r.count; // give back (no merge needed for a failure path)
        }
        out.clear();
        return false;
    }
    return true;
}

void FreePhysRun(u64 page, u64 count) {
    std::memset(g_backing + (page << kPageBits), 0, count << kPageBits); // pages are handed out zeroed
    auto next = g_phys_free.lower_bound(page);
    if (next != g_phys_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == page) {
            prev->second += count;
            if (next != g_phys_free.end() && prev->first + prev->second == next->first) {
                prev->second += next->second;
                g_phys_free.erase(next);
            }
            return;
        }
    }
    if (next != g_phys_free.end() && page + count == next->first) {
        const u64 len = next->second;
        g_phys_free.erase(next);
        g_phys_free[page] = count + len;
        return;
    }
    g_phys_free[page] = count;
}

void Ref(u64 page, u64 count) {
    for (u64 p = page; p < page + count; ++p) ++g_phys_refs[p];
    g_phys_used += count << kPageBits;
}

void Unref(u64 page, u64 count) {
    u64 run_start = 0, run_len = 0;
    for (u64 p = page; p < page + count; ++p) {
        if (g_phys_refs[p] && --g_phys_refs[p] == 0) {
            if (run_len && run_start + run_len == p) {
                ++run_len;
            } else {
                if (run_len) FreePhysRun(run_start, run_len);
                run_start = p;
                run_len = 1;
            }
            g_phys_used -= kPageSize;
        }
    }
    if (run_len) FreePhysRun(run_start, run_len);
}

// ---- placeholders and views ----

bool Carve(u64 va, u64 size) {
    auto it = g_free_ph.upper_bound(va);
    if (it == g_free_ph.begin()) return false;
    --it;
    const u64 ps = it->first, pl = it->second;
    if (va < ps || va + size > ps + pl) return false;
    g_free_ph.erase(it);
    if (va != ps || size != pl) {
        if (!VirtualFree(g_base + va, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOGC("Memory", "cannot split placeholder at %#llx+%#llx (error %lu)", va, size, GetLastError());
            return false;
        }
        if (va > ps) g_free_ph[ps] = va - ps;
        if (va + size < ps + pl) g_free_ph[va + size] = ps + pl - (va + size);
    }
    return true;
}

void ReturnPlaceholder(u64 va, u64 size) {
    u64 start = va, end = va + size;
    auto next = g_free_ph.lower_bound(va);
    bool merged = false;
    if (next != g_free_ph.end() && next->first == end) {
        end += next->second;
        g_free_ph.erase(next);
        merged = true;
    }
    auto it = g_free_ph.lower_bound(va);
    if (it != g_free_ph.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == start) {
            start = prev->first;
            g_free_ph.erase(prev);
            merged = true;
        }
    }
    if (merged && !VirtualFree(g_base + start, end - start, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        LOGW("Memory", "cannot coalesce placeholders at %#llx+%#llx (error %lu)", start, end - start, GetLastError());
    }
    g_free_ph[start] = end - start;
}

// FM2: gives the scratch pages inside [va, va+size) back to the placeholder list (real memory goes there).
void ReleaseScratch(u64 va, u64 size) {
    for (auto it = g_scratch.lower_bound(va); it != g_scratch.end() && *it < va + size;) {
        const u64 page = *it;
        if (!VirtualFree(g_base + page, kPageSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOGC("Memory", "cannot release the scratch page at %#llx (error %lu)", page, GetLastError());
            ++it;
            continue;
        }
        it = g_scratch.erase(it);
        ReturnPlaceholder(page, kPageSize);
    }
}

// FM2 fault inside the arena (guest va, or the guard past it). Runs on the faulting thread.
LONG ArenaFault(EXCEPTION_POINTERS* ep, u64 va) {
    const auto* rec = ep->ExceptionRecord;
    const bool write = rec->ExceptionInformation[0] == 1;
    const u64 page = AlignDown(va, kPageSize);
    std::scoped_lock lk{g_lock};
    if (page < kAddressSpaceEnd) {
        CommitTable(page, kPageSize);
        const PageEntry& e = g_table[page >> kPageBits];
        if (reinterpret_cast<const volatile u64&>(e.ptr) != 0) {
            // Mapped. A write to a write-protected code page, or a page mapped by another thread meanwhile.
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(g_base + page, &mbi, sizeof mbi);
            if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE) return EXCEPTION_CONTINUE_EXECUTION;
            if (!write || !reinterpret_cast<const volatile u64&>(e.watch)) return EXCEPTION_CONTINUE_SEARCH;
            DWORD old = 0;
            if (!VirtualProtect(g_base + page, kPageSize, PAGE_READWRITE, &old)) return EXCEPTION_CONTINUE_SEARCH;
            if (!(g_code_write && g_code_write(va, rec->ExceptionAddress))) {
                static std::atomic<int> logs{0};
                if (logs.fetch_add(1, std::memory_order_relaxed) < 8) {
                    LOGW("Memory", "host wrote code page %#llx (made writable)", page);
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (g_scratch.count(page)) return EXCEPTION_CONTINUE_EXECUTION; // another thread got here first
    if (!Carve(page, kPageSize) ||
        !pVirtualAlloc2(GetCurrentProcess(), g_base + page, kPageSize, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                        PAGE_READWRITE, nullptr, 0)) {
        LOGC("Memory", "cannot map a scratch page at %#llx (error %lu)", page, GetLastError());
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_scratch.insert(page);
    const u64 n = g_scratch_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0) {
        const kernel::Thread* t = kernel::Current();
        LOGE("Memory", "guest %s of unmapped %#llx: scratch page #%llu mapped (thread %llu pc=%#llx lr=%#llx)",
             write ? "write" : "read", va, n, t ? t->id : 0, t ? t->ctx.pc : 0, t ? t->ctx.x[30] : 0);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool MapView(u64 va, u64 size, u64 phys_page) {
    if (!g_scratch.empty()) ReleaseScratch(va, size);
    if (!Carve(va, size)) return false;
    if (!pMapViewOfFile3(g_section, GetCurrentProcess(), g_base + va, phys_page << kPageBits, size,
                         MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
        LOGC("Memory", "cannot map view at %#llx+%#llx (error %lu)", va, size, GetLastError());
        ReturnPlaceholder(va, size);
        return false;
    }
    g_views[va] = View{size, phys_page << kPageBits};
    return true;
}

std::map<u64, Block>::iterator SplitAt(u64 addr) {
    auto it = g_blocks.upper_bound(addr);
    --it;
    if (it->first == addr) return it;
    Block tail = it->second;
    const u64 cut = addr - it->first;
    it->second.size = cut;
    tail.base = addr;
    tail.size -= cut;
    return g_blocks.emplace(addr, tail).first;
}

bool Same(const Block& a, const Block& b) {
    return a.state == b.state && a.perm == b.perm && a.attr == b.attr;
}

void Assign(u64 va, u64 size, u32 state, u32 perm, u32 attr) {
    if (va + size < kAddressSpaceEnd) SplitAt(va + size);
    auto it = SplitAt(va);
    while (it != g_blocks.end() && it->first < va + size) it = g_blocks.erase(it);
    auto ins = g_blocks.emplace(va, Block{va, size, state, perm, attr}).first;
    if (ins != g_blocks.begin()) {
        auto prev = std::prev(ins);
        if (Same(prev->second, ins->second)) {
            prev->second.size += ins->second.size;
            g_blocks.erase(ins);
            ins = prev;
        }
    }
    auto next = std::next(ins);
    if (next != g_blocks.end() && Same(next->second, ins->second)) {
        ins->second.size += next->second.size;
        g_blocks.erase(next);
    }
}

bool AllState(u64 va, u64 size, u32 state) {
    auto it = g_blocks.upper_bound(va);
    --it;
    for (u64 at = va; at < va + size;) {
        if (it == g_blocks.end() || it->second.state != state) return false;
        at = it->first + it->second.size;
        ++it;
    }
    return true;
}

void SetEntries(u64 va, u64 size, const u64* phys_bytes /* first page's offset, or null = unmapped */) {
    CommitTable(va, size);
    const u64 ptr = phys_bytes ? (reinterpret_cast<u64>(g_base) | 1) : 0;
    u64 off = phys_bytes ? *phys_bytes : 0;
    for (u64 p = va >> kPageBits, e = (va + size) >> kPageBits; p < e; ++p) {
        g_table[p].rsv1 = phys_bytes ? off + 1 : 0;
        reinterpret_cast<volatile u64&>(g_table[p].ptr) = ptr;
        off += kPageSize;
    }
}

// Maps the runs at [va, ...) and records the page entries.
bool MapRuns(u64 va, const std::vector<Run>& runs) {
    u64 at = va;
    for (const auto& r : runs) {
        const u64 bytes = r.count << kPageBits;
        if (!MapView(at, bytes, r.page)) return false;
        const u64 phys = r.page << kPageBits;
        SetEntries(at, bytes, &phys);
        at += bytes;
    }
    return true;
}

// Unmaps every view overlapping [va, va+size), remapping the parts outside it.
void UnmapRange(u64 va, u64 size) {
    const u64 end = va + size;
    auto it = g_views.upper_bound(va);
    if (it != g_views.begin()) --it;
    std::vector<std::pair<u64, View>> hit;
    for (; it != g_views.end() && it->first < end; ++it) {
        if (it->first + it->second.size > va) hit.emplace_back(it->first, it->second);
    }
    for (const auto& [vs, v] : hit) {
        g_views.erase(vs);
        if (!pUnmapViewOfFile2(GetCurrentProcess(), g_base + vs, MEM_PRESERVE_PLACEHOLDER)) {
            LOGC("Memory", "cannot unmap view at %#llx (error %lu)", vs, GetLastError());
        }
        ReturnPlaceholder(vs, v.size);
        const u64 ve = vs + v.size;
        const u64 cut_lo = std::max(vs, va), cut_hi = std::min(ve, end);
        if (cut_lo > vs) MapView(vs, cut_lo - vs, v.phys >> kPageBits);
        if (ve > cut_hi) MapView(cut_hi, ve - cut_hi, (v.phys + (cut_hi - vs)) >> kPageBits);
        Unref((v.phys + (cut_lo - vs)) >> kPageBits, (cut_hi - cut_lo) >> kPageBits);
    }
    SetEntries(va, size, nullptr);
}

} // namespace

bool Init(u64 backing_bytes, std::string* error) {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    pVirtualAlloc2 = reinterpret_cast<PFN_VirtualAlloc2>(reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")));
    pMapViewOfFile3 = reinterpret_cast<PFN_MapViewOfFile3>(reinterpret_cast<void*>(GetProcAddress(kb, "MapViewOfFile3")));
    pUnmapViewOfFile2 = reinterpret_cast<PFN_UnmapViewOfFile2>(reinterpret_cast<void*>(GetProcAddress(kb, "UnmapViewOfFile2")));
    if (!pVirtualAlloc2 || !pMapViewOfFile3 || !pUnmapViewOfFile2) {
        *error = "this Windows version lacks memory placeholders (Windows 10 1803 or newer is needed)";
        return false;
    }
    g_backing_size = AlignUp(backing_bytes, 0x10000);
    g_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                   static_cast<DWORD>(g_backing_size >> 32), static_cast<DWORD>(g_backing_size), nullptr);
    if (!g_section) {
        *error = "cannot create the " + std::to_string(g_backing_size >> 20) + " MiB memory backing (error " +
                 std::to_string(GetLastError()) + "): is the page file large enough?";
        return false;
    }
    g_backing = static_cast<u8*>(MapViewOfFile(g_section, FILE_MAP_ALL_ACCESS, 0, 0, g_backing_size));
    if (!g_backing) {
        *error = "cannot map the memory backing (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    // The 64 KiB guard past the 39-bit space absorbs FM2 accesses that start just below 2^39 and run past it.
    g_base = static_cast<u8*>(pVirtualAlloc2(nullptr, nullptr, kArenaBytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                             PAGE_NOACCESS, nullptr, 0));
    if (!g_base) {
        *error = "cannot reserve the 512 GiB guest address space (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    g_table = static_cast<PageEntry*>(VirtualAlloc(nullptr, kTableBytes, MEM_RESERVE, PAGE_READWRITE));
    if (!g_table) {
        *error = "cannot reserve the page table (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    AddVectoredExceptionHandler(1, TableFault);
    g_blocks.clear();
    g_blocks.emplace(0, Block{0, kAddressSpaceEnd, Free, None, 0});
    g_free_ph.clear();
    g_free_ph[0] = kArenaBytes;
    g_phys_free.clear();
    g_phys_free[0] = g_backing_size >> kPageBits;
    g_phys_refs.assign(g_backing_size >> kPageBits, 0);
    LOGI("Memory", "guest address space at %p (39 bits), backing %llu MiB at %p, page table at %p", g_base,
         g_backing_size >> 20, g_backing, g_table);
    return true;
}

u8* Base() {
    return g_base;
}

const PageEntry* PageTable() {
    return g_table;
}

bool IsMapped(u64 va, u64 size) {
    if (size == 0 || va >= kAddressSpaceEnd || size > kAddressSpaceEnd - va) return false;
    for (u64 p = va >> kPageBits, e = (va + size - 1) >> kPageBits; p <= e; ++p) {
        if (reinterpret_cast<const volatile u64&>(g_table[p].ptr) == 0) return false;
    }
    return true;
}

void* HostPointer(u64 va, u64 size) {
    return IsMapped(va, size ? size : 1) ? g_base + va : nullptr;
}

void* BackingPointer(u64 va, u64 size) {
    if (!IsMapped(va, size ? size : 1)) return nullptr;
    const u64 first = va >> kPageBits;
    const u64 off0 = g_table[first].rsv1;
    if (!off0) return nullptr;
    for (u64 p = first + 1, e = (va + (size ? size : 1) - 1) >> kPageBits; p <= e; ++p) {
        if (g_table[p].rsv1 != off0 + ((p - first) << kPageBits)) return nullptr;
    }
    return g_backing + (off0 - 1) + (va & (kPageSize - 1));
}

Result Map(u64 va, u64 size, u32 state, u32 perm) {
    if ((va | size) & (kPageSize - 1) || size == 0 || va + size > kAddressSpaceEnd || va + size < va) {
        return kResultInvalidAddress;
    }
    std::scoped_lock lk{g_lock};
    if (!AllState(va, size, Free)) return kResultInvalidCurrentMemory;
    std::vector<Run> runs;
    if (!AllocPhys(size >> kPageBits, runs)) {
        LOGE("Memory", "out of guest memory mapping %#llx bytes at %#llx (%llu MiB in use)", size, va, g_phys_used >> 20);
        return kResultOutOfMemory;
    }
    for (const auto& r : runs) Ref(r.page, r.count);
    if (!MapRuns(va, runs)) {
        UnmapRange(va, size);
        return kResultOutOfMemory;
    }
    Assign(va, size, state, perm, 0);
    return kOk;
}

Result MapAlias(u64 dst, u64 src, u64 size, u32 state, u32 perm) {
    if ((dst | src | size) & (kPageSize - 1) || size == 0) return kResultInvalidAddress;
    std::scoped_lock lk{g_lock};
    if (!IsMapped(src, size)) return kResultInvalidCurrentMemory;
    if (!AllState(dst, size, Free)) return kResultInvalidCurrentMemory;
    std::vector<Run> runs;
    for (u64 p = src >> kPageBits, e = (src + size) >> kPageBits; p < e; ++p) {
        const u64 page = (g_table[p].rsv1 - 1) >> kPageBits;
        if (!runs.empty() && runs.back().page + runs.back().count == page) {
            ++runs.back().count;
        } else {
            runs.push_back({page, 1});
        }
    }
    for (const auto& r : runs) Ref(r.page, r.count);
    if (!MapRuns(dst, runs)) {
        UnmapRange(dst, size);
        return kResultOutOfMemory;
    }
    Assign(dst, size, state, perm, 0);
    return kOk;
}

Result Unmap(u64 va, u64 size) {
    if ((va | size) & (kPageSize - 1) || size == 0 || va + size > kAddressSpaceEnd) return kResultInvalidAddress;
    std::scoped_lock lk{g_lock};
    UnmapRange(va, size);
    Assign(va, size, Free, None, 0);
    return kOk;
}

void SetBlock(u64 va, u64 size, u32 state, u32 perm, u32 attr) {
    std::scoped_lock lk{g_lock};
    Assign(va, size, state, perm, attr);
}

Block Query(u64 va) {
    if (va >= kAddressSpaceEnd) return Block{kAddressSpaceEnd, 0 - kAddressSpaceEnd, Inaccessible, None, 0};
    std::scoped_lock lk{g_lock};
    auto it = g_blocks.upper_bound(va);
    --it;
    return it->second;
}

u64 FindFree(u64 start, u64 region, u64 size, u64 align) {
    std::scoped_lock lk{g_lock};
    auto it = g_blocks.upper_bound(start);
    --it;
    for (; it != g_blocks.end() && it->first < start + region; ++it) {
        const Block& b = it->second;
        if (b.state != Free) continue;
        const u64 lo = AlignUp(std::max(b.base, start), align);
        const u64 hi = std::min(b.base + b.size, start + region);
        if (lo < hi && hi - lo >= size) return lo;
    }
    return 0;
}

u64 MapAnywhere(u64 region_start, u64 region_size, u64 size, u32 state, u32 perm) {
    size = AlignUp(size, kPageSize);
    std::scoped_lock lk{g_lock};
    const u64 va = FindFree(region_start, region_size, size);
    if (!va || Map(va, size, state, perm) != kOk) return 0;
    return va;
}

Result SetHeapSize(u64 size, u64* out_addr) {
    if (size & 0x1FFFFF || size > kHeapSize) return kResultInvalidSize;
    std::scoped_lock lk{g_lock};
    Result r = kOk;
    if (size > g_heap_size) {
        r = Map(kHeapStart + g_heap_size, size - g_heap_size, Normal, RW);
    } else if (size < g_heap_size) {
        r = Unmap(kHeapStart + size, g_heap_size - size);
    }
    if (r == kOk) {
        g_heap_size = size;
        *out_addr = kHeapStart;
    }
    return r;
}

Result MapPhysical(u64 va, u64 size) {
    if ((va | size) & (kPageSize - 1) || size == 0) return kResultInvalidAddress;
    if (va < kAliasStart || va + size > kAliasStart + kAliasSize) return kResultInvalidMemoryRegion;
    std::scoped_lock lk{g_lock};
    std::vector<std::pair<u64, u64>> holes;
    for (u64 at = va; at < va + size;) {
        const Block b = Query(at);
        const u64 end = std::min(b.base + b.size, va + size);
        if (b.state == Free) {
            holes.emplace_back(at, end - at);
        } else if (b.state != Normal) {
            return kResultInvalidCurrentMemory;
        }
        at = end;
    }
    for (const auto& [a, s] : holes) {
        if (const Result r = Map(a, s, Normal, RW); r != kOk) return r;
        g_physical += s;
    }
    return kOk;
}

Result UnmapPhysical(u64 va, u64 size) {
    if ((va | size) & (kPageSize - 1) || size == 0) return kResultInvalidAddress;
    if (va < kAliasStart || va + size > kAliasStart + kAliasSize) return kResultInvalidMemoryRegion;
    std::scoped_lock lk{g_lock};
    for (u64 at = va; at < va + size;) {
        const Block b = Query(at);
        const u64 end = std::min(b.base + b.size, va + size);
        if (b.state == Normal) {
            Unmap(at, end - at);
            g_physical -= end - at;
        } else if (b.state != Free) {
            return kResultInvalidCurrentMemory;
        }
        at = end;
    }
    return kOk;
}

u64 NormalMemorySize() {
    return g_heap_size + g_physical;
}

u64 HeapSize() {
    return g_heap_size;
}

void Watch(u64 va, u64 size) {
    CommitTable(va, size);
    for (u64 p = va >> kPageBits, e = (va + size + kPageSize - 1) >> kPageBits; p < e; ++p) {
        reinterpret_cast<volatile u64&>(g_table[p].watch) = 1;
    }
}

void EnableFm2(bool (*code_write)(u64 va, const void* host_ip)) {
    g_code_write = code_write;
    g_fm2.store(true, std::memory_order_release);
}

bool Fm2() {
    return g_fm2.load(std::memory_order_relaxed);
}

void ProtectCode(u64 va, u64 size) {
    if (!Fm2() || size == 0) return;
    std::scoped_lock lk{g_lock};
    const u64 lo = AlignDown(va, kPageSize), hi = AlignUp(va + size, kPageSize);
    u64 pages = 0;
    auto it = g_views.upper_bound(lo);
    if (it != g_views.begin()) --it;
    for (; it != g_views.end() && it->first < hi; ++it) {
        const u64 a = std::max(lo, it->first), b = std::min(hi, it->first + it->second.size);
        if (a >= b) continue;
        DWORD old = 0;
        if (VirtualProtect(g_base + a, b - a, PAGE_READONLY, &old)) {
            pages += (b - a) >> kPageBits;
        } else {
            LOGW("Memory", "cannot write-protect code at %#llx+%#llx (error %lu)", a, b - a, GetLastError());
        }
    }
    LOGI("Memory", "FM2: code %#llx-%#llx write-protected (%llu pages)", lo, hi, pages);
}

bool Fm2SelfTest() {
    if (!Fm2()) return false;
    const u64 va = kKernelMapStart + kKernelMapSize - 0x10000; // free in the game's layout
    volatile u8* p = g_base + va;
    const u8 r0 = p[0];         // fault -> scratch page, reads zero
    p[8] = 0x55;                // scratch pages are writable
    const u8 r1 = p[8];
    const Result m = Map(va, kPageSize, Normal, RW); // replaces the scratch page with real memory
    const u8 r2 = p[8];         // fresh memory is zero
    p[8] = 0x66;
    const bool mapped = IsMapped(va, 1);
    Unmap(va, kPageSize);
    const u8 r3 = p[16];        // unmapped again -> scratch again
    volatile u8* g = g_base + kAddressSpaceEnd + 0x10; // the guard past 2^39
    const u8 r4 = g[0];
    const bool ok = r0 == 0 && r1 == 0x55 && m == kOk && r2 == 0 && mapped && r3 == 0 && r4 == 0 && !IsMapped(va, 1);
    LOGI("Memory", "FM2 self-test %s (r0=%u r1=%#x map=%#x r2=%u mapped=%d r3=%u guard=%u scratch=%llu)",
         ok ? "passed" : "FAILED", r0, r1, m, r2, mapped, r3, r4, ScratchPages());
    return ok;
}

u64 ScratchPages() {
    return g_scratch_count.load(std::memory_order_relaxed);
}

bool IsWatched(u64 va) {
    if (va >= kAddressSpaceEnd) return false;
    return reinterpret_cast<const volatile u64&>(g_table[va >> kPageBits].watch) != 0;
}

size_t Read(u64 va, void* dst, size_t size) {
    size_t done = 0;
    auto* out = static_cast<u8*>(dst);
    while (done < size) {
        const u64 at = va + done;
        const size_t chunk = std::min<size_t>(size - done, kPageSize - (at & (kPageSize - 1)));
        if (!IsMapped(at, chunk)) break;
        std::memcpy(out + done, g_base + at, chunk);
        done += chunk;
    }
    return done;
}

size_t Write(u64 va, const void* src, size_t size) {
    size_t done = 0;
    const auto* in = static_cast<const u8*>(src);
    while (done < size) {
        const u64 at = va + done;
        const size_t chunk = std::min<size_t>(size - done, kPageSize - (at & (kPageSize - 1)));
        if (!IsMapped(at, chunk)) break;
        std::memcpy(g_base + at, in + done, chunk);
        done += chunk;
    }
    return done;
}

std::string ReadString(u64 va, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        if (!IsMapped(va + i, 1)) break;
        const char c = static_cast<char>(g_base[va + i]);
        if (!c) break;
        s += c;
    }
    return s;
}

std::vector<Block> Blocks() {
    std::scoped_lock lk{g_lock};
    std::vector<Block> out;
    for (const auto& [b, blk] : g_blocks) out.push_back(blk);
    return out;
}

} // namespace plahost::mem
