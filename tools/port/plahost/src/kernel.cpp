// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guest threads run 1:1 on host threads with their own GuestContext, as in the fork's v1.5
// runtime (recomp_host_threads.cpp, PLARecomp code) but without suyu's KThread behind them:
// every SVC is served on the thread that made it.

#include "kernel.h"

#include <algorithm>
#include <chrono>
#include <intrin.h>
#include <map>
#include <xmmintrin.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "log.h"
#include "memory.h"
#include "mods.h"
#include "recomp.h"
#include "svc.h"
#include "tune.h"

namespace plahost::kernel {

std::atomic<bool> g_exiting{false};

namespace {

std::mutex g_lock;

// ---- handle table (Horizon's format: index in bits 0-14, a linear id in bits 15-29) ----
struct Slot {
    std::shared_ptr<Object> obj;
    HandleKind kind = HandleKind::Plain;
    u16 linear = 0;
};
std::mutex g_handles_lock;
std::vector<Slot> g_slots(1); // index 0 unused
std::vector<u32> g_free;
u16 g_next_linear = 1;
size_t g_handle_count = 0;

std::shared_ptr<Process> g_process = std::make_shared<Process>();

std::mutex g_threads_lock;
std::vector<std::shared_ptr<Thread>> g_threads;
std::atomic<u64> g_next_thread_id{0x50};
thread_local Thread* t_current = nullptr;

int ChainBudget() {
    static const int budget = [] {
        const char* e = std::getenv("PLA_RECOMP_CHAIN_BUDGET");
        const int v = e && *e ? std::atoi(e) : 1024;
        return std::clamp(v, 1, 1 << 20);
    }();
    return budget;
}

// ---- guest memory callbacks (what the images' inline path declines) ----

void ReportUnmapped(Thread* t, u64 va, u32 size, const char* what) {
    static std::atomic<int> logs{0};
    if (logs.load(std::memory_order_relaxed) >= 16 || logs.fetch_add(1, std::memory_order_relaxed) >= 16) {
        return;
    }
    LOGE("Memory", "guest %s%u of unmapped %#llx (thread %llu pc=%#llx lr=%#llx)", what, size * 8, va,
         t ? t->id : 0, t ? t->ctx.pc : 0, t ? t->ctx.x[30] : 0);
}

u64 HostLoad(void* user, u64 va, u32 size) {
    va &= 0xFFFF'FFFF'FFFFULL;
    if (size == 0) { // guard-v2 instruction read: bit 32 = mapped
        return mem::IsMapped(va, 4) ? ((1ULL << 32) | mem::Load<u32>(va)) : 0;
    }
    if (!mem::IsMapped(va, size)) {
        ReportUnmapped(static_cast<Thread*>(user), va, size, "read");
        return 0;
    }
    switch (size) {
    case 1: return mem::Load<u8>(va);
    case 2: return mem::Load<u16>(va);
    case 4: return mem::Load<u32>(va);
    default: return mem::Load<u64>(va);
    }
}

void HostStore(void* user, u64 va, u32 size, u64 value) {
    va &= 0xFFFF'FFFF'FFFFULL;
    if (!mem::IsMapped(va, size)) {
        ReportUnmapped(static_cast<Thread*>(user), va, size, "write");
        return;
    }
    switch (size) {
    case 1: mem::StoreTo<u8>(va, static_cast<u8>(value)); break;
    case 2: mem::StoreTo<u16>(va, static_cast<u16>(value)); break;
    case 4: mem::StoreTo<u32>(va, static_cast<u32>(value)); break;
    default: mem::StoreTo<u64>(va, value); break;
    }
    if (mem::IsWatched(va) || mem::IsWatched(va + size - 1)) {
        recomp::OnCodeWrite(va);
    }
}

u64 ExclLoad(void* user, u64 va, u32 size) {
    auto* t = static_cast<Thread*>(user);
    const u64 v = HostLoad(user, va, size);
    t->excl_valid = true;
    t->excl_va = va;
    t->excl_size = size;
    t->excl_lo = v;
    return v;
}

u32 ExclStore(void* user, u64 va, u32 size, u64 value) {
    auto* t = static_cast<Thread*>(user);
    if (!t->excl_valid || t->excl_va != va || t->excl_size != size) {
        t->excl_valid = false;
        return 1;
    }
    t->excl_valid = false;
    va &= 0xFFFF'FFFF'FFFFULL;
    if (!mem::IsMapped(va, size)) {
        ReportUnmapped(t, va, size, "exclusive write");
        return 1;
    }
    void* p = mem::Ptr(va);
    bool ok;
    switch (size) {
    case 1: ok = _InterlockedCompareExchange8(static_cast<volatile char*>(p), static_cast<char>(value),
                                              static_cast<char>(t->excl_lo)) == static_cast<char>(t->excl_lo); break;
    case 2: ok = _InterlockedCompareExchange16(static_cast<volatile short*>(p), static_cast<short>(value),
                                               static_cast<short>(t->excl_lo)) == static_cast<short>(t->excl_lo); break;
    case 4: ok = _InterlockedCompareExchange(static_cast<volatile long*>(p), static_cast<long>(value),
                                             static_cast<long>(t->excl_lo)) == static_cast<long>(t->excl_lo); break;
    default: ok = _InterlockedCompareExchange64(static_cast<volatile long long*>(p), static_cast<long long>(value),
                                                static_cast<long long>(t->excl_lo)) == static_cast<long long>(t->excl_lo); break;
    }
    return ok ? 0u : 1u;
}

void ExclLoadPair(void* user, u64 va, u32 size, u64* lo, u64* hi) {
    auto* t = static_cast<Thread*>(user);
    *lo = HostLoad(user, va, size);
    *hi = HostLoad(user, va + size, size);
    t->excl_valid = true;
    t->excl_va = va;
    t->excl_size = size * 2;
    t->excl_lo = *lo;
    t->excl_hi = *hi;
}

u32 ExclStorePair(void* user, u64 va, u32 size, u64 lo, u64 hi) {
    auto* t = static_cast<Thread*>(user);
    if (!t->excl_valid || t->excl_va != va || t->excl_size != size * 2) {
        t->excl_valid = false;
        return 1;
    }
    t->excl_valid = false;
    va &= 0xFFFF'FFFF'FFFFULL;
    if (!mem::IsMapped(va, size * 2)) {
        ReportUnmapped(t, va, size * 2, "exclusive write");
        return 1;
    }
    void* p = mem::Ptr(va);
    if (size == 8) {
        if ((va & 15) == 0) {
            long long cmp[2] = {static_cast<long long>(t->excl_lo), static_cast<long long>(t->excl_hi)};
            return _InterlockedCompareExchange128(static_cast<volatile long long*>(p), static_cast<long long>(hi),
                                                  static_cast<long long>(lo), cmp)
                       ? 0u
                       : 1u;
        }
        // Unaligned 128-bit pair (not atomic on the console either).
        u64 cur[2];
        std::memcpy(cur, p, 16);
        if (cur[0] != t->excl_lo || cur[1] != t->excl_hi) return 1;
        const u64 nv[2] = {lo, hi};
        std::memcpy(p, nv, 16);
        return 0;
    }
    const u64 expect = static_cast<u32>(t->excl_lo) | (static_cast<u64>(static_cast<u32>(t->excl_hi)) << 32);
    const u64 nv = static_cast<u32>(lo) | (static_cast<u64>(static_cast<u32>(hi)) << 32);
    return _InterlockedCompareExchange64(static_cast<volatile long long*>(p), static_cast<long long>(nv),
                                         static_cast<long long>(expect)) == static_cast<long long>(expect)
               ? 0u
               : 1u;
}

void ClearExcl(void* user) {
    static_cast<Thread*>(user)->excl_valid = false;
}

u64 ReadCntpct(void*) {
    return GuestTicks();
}

void PrepareContext(Thread* t) {
    auto& b = t->bridge;
    b.user = t;
    b.load = HostLoad;
    b.store = HostStore;
    b.excl_load = ExclLoad;
    b.excl_store = ExclStore;
    b.clear_excl = ClearExcl;
    b.read_cntpct = ReadCntpct;
    b.excl_load_pair = ExclLoadPair;
    b.excl_store_pair = ExclStorePair;
    b.page_entries = mem::PageTable();
    b.page_entry_stride = sizeof(PageEntry);
    b.page_bits = kPageBits;
    b.pointer_mask = kEntryPtrMask;
    b.address_space_max = mem::kAddressSpaceEnd;
    b.guard_generation = nullptr;
    auto& c = t->ctx;
    c.host_mem = &b;
    c.fm_table = reinterpret_cast<const unsigned char*>(mem::PageTable());
    c.fm_limit = mem::kAddressSpaceEnd;
    c.pending_svc = kNoPendingSvc;
}

void SetHostPriority(s32 guest_prio) {
    int p = THREAD_PRIORITY_NORMAL;
    if (guest_prio <= 20) {
        p = THREAD_PRIORITY_HIGHEST;
    } else if (guest_prio <= 30) {
        p = THREAD_PRIORITY_ABOVE_NORMAL;
    } else if (guest_prio >= 50) {
        p = THREAD_PRIORITY_BELOW_NORMAL;
    }
    SetThreadPriority(GetCurrentThread(), p);
}

// FPX1: round to nearest, no DAZ/FTZ, every exception masked.
inline void EnsureFpEnv() {
    const unsigned csr = _mm_getcsr();
    if ((csr & 0xFFC0u) != 0x1F80u) {
        _mm_setcsr((csr & 0x3Fu) | 0x1F80u);
    }
}

void RunLoop(Thread* t) {
    GuestContext& c = t->ctx;
    const int budget = ChainBudget();
    while (true) {
        EnsureFpEnv();
        if (c.pc == kReturnTrap) {
            if (!mods::OnReturnTrap(&c)) {
                ParkCurrent("return trap with no matching hooked call");
            }
            continue;
        }
        if (c.pc == kUnresolvedImportTrap) {
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 16) {
                LOGE("Kernel", "thread %llu called an unresolved import (returns 0 to %#llx)", t->id, c.x[30]);
            }
            c.x[0] = 0;
            c.pc = c.x[30];
            continue;
        }
        const recomp::Image* im = recomp::ImageFor(c.pc);
        if (!im || ((c.pc - im->lo) & 3) || !im->index[(c.pc - im->lo) >> 2]) {
            char why[160];
            std::snprintf(why, sizeof why, "no recompiled block at pc=%#llx (lr=%#llx)", c.pc, c.x[30]);
            ParkCurrent(why);
        }
        c.chain_budget = budget;
        im->run_slice(&c);
        if (c.halted) {
            const int h = c.halted;
            c.halted = 0;
            if (h == kHaltIcIvau) {
                c.pending_svc = kNoPendingSvc;
                c.pc += 4;
                continue;
            }
            if (h == kHaltBreakpoint) {
                if (mods::Retired()) {
                    // Called a hooked function after the mods shut down (exit in progress).
                    while (true) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                }
                ParkCurrent("guest breakpoint / hook thunk at an unhooked pc");
            }
            if (h == kHaltUnhandled) {
                char why[160];
                std::snprintf(why, sizeof why, "unimplemented instruction %08x at pc=%#llx (no JIT in this host)",
                              mem::IsMapped(c.pc, 4) ? mem::Load<u32>(c.pc) : 0u, c.pc);
                ParkCurrent(why);
            }
            continue;
        }
        if (c.pending_svc != kNoPendingSvc) {
            const u32 imm = static_cast<u32>(c.pending_svc);
            c.pending_svc = kNoPendingSvc;
            if (imm == 0x10) { // GetCurrentProcessorNumber
                c.x[0] = static_cast<u64>(t->ideal_core >= 0 && t->ideal_core <= 2 ? t->ideal_core : 0);
                continue;
            }
            t->last_svc.store(imm, std::memory_order_relaxed);
            t->svc_since_ms.store(NowMs(), std::memory_order_relaxed);
            t->in_svc.store(true, std::memory_order_relaxed);
            if (tune::g_stats.load(std::memory_order_relaxed)) {
                const u64 t0 = tune::Qpc();
                svc::Call(t, imm);
                t->stat_svc_ticks[imm & 0x7F].fetch_add(tune::Qpc() - t0, std::memory_order_relaxed);
                t->stat_svc_n[imm & 0x7F].fetch_add(1, std::memory_order_relaxed);
            } else {
                svc::Call(t, imm);
            }
            t->in_svc.store(false, std::memory_order_relaxed);
            t->svc_count.fetch_add(1, std::memory_order_relaxed);
            if (t->exit_requested.load(std::memory_order_relaxed)) {
                return;
            }
        }
    }
}

void HostMain(std::shared_ptr<Thread> t) {
    t_current = t.get();
    wchar_t name[64];
    swprintf(name, 64, L"GuestThread%llu", t->id);
    SetThreadDescription(GetCurrentThread(), name);
    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    t->host_thread = self;
    SetHostPriority(t->prio.load());
    tune::OnGuestThreadStart(t.get(), t->is_main);
    RunLoop(t.get());
    {
        std::scoped_lock lk{g_lock};
        t->exited = true;
        t->signaled = true;
        for (Waiter* w : t->waiters) {
            w->woken = true;
            w->cv.notify_all();
        }
    }
    LOGI("Kernel", "thread %llu exited", t->id);
}

// TLS pages: 8 slots of 0x200 bytes per page.
std::mutex g_tls_lock;
std::vector<std::pair<u64, u8>> g_tls_pages; // page, used-slot bitmap

} // namespace

std::mutex& Lock() {
    return g_lock;
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u64 GuestTicks() {
    static const auto freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<u64>(f.QuadPart);
    }();
    static const auto base = [] {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<u64>(c.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const u64 d = static_cast<u64>(now.QuadPart) - base;
    u64 hi;
    const u64 lo = _umul128(d, 19'200'000ULL, &hi);
    u64 rem;
    return _udiv128(hi, lo, freq, &rem);
}

// ---- handles ----

u32 AddHandle(std::shared_ptr<Object> obj, HandleKind kind) {
    std::scoped_lock lk{g_handles_lock};
    u32 index;
    if (!g_free.empty()) {
        index = g_free.back();
        g_free.pop_back();
    } else {
        if (g_slots.size() >= 0x7FFF) {
            LOGC("Kernel", "handle table full");
            return 0;
        }
        index = static_cast<u32>(g_slots.size());
        g_slots.emplace_back();
    }
    const u16 linear = g_next_linear;
    g_next_linear = static_cast<u16>(g_next_linear == 0x7FFF ? 1 : g_next_linear + 1);
    g_slots[index] = Slot{std::move(obj), kind, linear};
    ++g_handle_count;
    return (u32{linear} << 15) | index;
}

std::shared_ptr<Object> GetHandle(u32 handle, HandleKind* kind) {
    if (kind) *kind = HandleKind::Plain;
    if (handle == kCurrentThread) {
        return t_current ? t_current->shared_from_this() : nullptr;
    }
    if (handle == kCurrentProcess) {
        return g_process;
    }
    const u32 index = handle & 0x7FFF;
    const u16 linear = static_cast<u16>((handle >> 15) & 0x7FFF);
    std::scoped_lock lk{g_handles_lock};
    if (index == 0 || index >= g_slots.size() || !g_slots[index].obj || g_slots[index].linear != linear) {
        return nullptr;
    }
    if (kind) *kind = g_slots[index].kind;
    return g_slots[index].obj;
}

bool CloseHandle(u32 handle) {
    if (handle == kCurrentThread || handle == kCurrentProcess) {
        return true;
    }
    std::shared_ptr<Object> drop;
    std::scoped_lock lk{g_handles_lock};
    const u32 index = handle & 0x7FFF;
    const u16 linear = static_cast<u16>((handle >> 15) & 0x7FFF);
    if (index == 0 || index >= g_slots.size() || !g_slots[index].obj || g_slots[index].linear != linear) {
        return false;
    }
    drop = std::move(g_slots[index].obj);
    g_slots[index] = Slot{};
    g_free.push_back(index);
    --g_handle_count;
    return true;
}

size_t HandleCount() {
    std::scoped_lock lk{g_handles_lock};
    return g_handle_count;
}

std::shared_ptr<Process> CurrentProcess() {
    return g_process;
}

// ---- events ----

std::shared_ptr<Event> NewEvent(std::string name) {
    auto ev = std::make_shared<Event>();
    ev->name = std::move(name);
    return ev;
}

void Signal(SyncObject& obj) {
    std::scoped_lock lk{g_lock};
    obj.signaled = true;
    const Thread* cur = t_current;
    for (Waiter* w : obj.waiters) {
        w->woken = true;
        w->waker = cur ? cur->id : 0;
        w->cv.notify_all();
    }
}

void Clear(SyncObject& obj) {
    std::scoped_lock lk{g_lock};
    obj.signaled = false;
}

u32 ReadableHandle(const std::shared_ptr<Event>& ev) {
    return AddHandle(ev, HandleKind::Readable);
}

// ---- waits ----

Result WaitSynchronization(Thread* self, const u32* handles, s32 count, s64 timeout_ns, s32* index) {
    if (count < 0 || count > 64) {
        return kResultOutOfRange;
    }
    std::vector<std::shared_ptr<SyncObject>> objs;
    objs.reserve(count);
    for (s32 i = 0; i < count; ++i) {
        HandleKind kind;
        auto obj = std::dynamic_pointer_cast<SyncObject>(GetHandle(handles[i], &kind));
        if (!obj || (dynamic_cast<Event*>(obj.get()) && kind == HandleKind::Writable)) {
            return kResultInvalidHandle;
        }
        objs.push_back(std::move(obj));
    }
    std::unique_lock lk{g_lock};
    if (self->cancel_pending) {
        self->cancel_pending = false;
        return kResultCancelled;
    }
    for (s32 i = 0; i < count; ++i) {
        if (objs[i]->signaled) {
            *index = i;
            return kOk;
        }
    }
    if (timeout_ns == 0) {
        return kResultTimedOut;
    }
    Waiter w;
    for (auto& o : objs) o->waiters.push_back(&w);
    self->current_wait = &w;
    self->wait_handles.assign(handles, handles + count);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns > 0 ? timeout_ns : 0);
    const u64 wait_t0 = tune::g_stats.load(std::memory_order_relaxed) ? tune::Qpc() : 0;
    Result r = kResultTimedOut;
    while (true) {
        bool any = false;
        for (s32 i = 0; i < count; ++i) {
            if (objs[i]->signaled) {
                *index = i;
                r = kOk;
                any = true;
                break;
            }
        }
        if (any) break;
        if (w.cancelled) {
            r = kResultCancelled;
            break;
        }
        if (timeout_ns > 0) {
            if (w.cv.wait_until(lk, deadline) == std::cv_status::timeout && !w.woken) {
                bool sig = false;
                for (auto& o : objs) sig |= o->signaled;
                if (!sig && !w.cancelled) {
                    r = kResultTimedOut;
                    break;
                }
            }
        } else {
            w.cv.wait(lk, [&] { return w.woken || w.cancelled; });
        }
        w.woken = false;
    }
    for (auto& o : objs) std::erase(o->waiters, &w);
    if (wait_t0 && r == kOk) self->stat_wait_by[w.waker & 0xFF].fetch_add(tune::Qpc() - wait_t0, std::memory_order_relaxed);
    self->current_wait = nullptr;
    self->wait_handles.clear();
    return r;
}

Result CancelSynchronization(u32 thread_handle) {
    auto t = Get<Thread>(thread_handle);
    if (!t) {
        return kResultInvalidHandle;
    }
    std::scoped_lock lk{g_lock};
    if (t->current_wait) {
        t->current_wait->cancelled = true;
        t->current_wait->cv.notify_all();
    } else {
        t->cancel_pending = true;
    }
    return kOk;
}

int WaitHost(SyncObject& obj, s64 timeout_ns) {
    std::unique_lock lk{g_lock};
    if (obj.signaled) return 1;
    if (timeout_ns == 0) return 0;
    Waiter w;
    obj.waiters.push_back(&w);
    const auto pred = [&] { return obj.signaled || g_exiting.load(); };
    bool ok;
    if (timeout_ns < 0) {
        w.cv.wait(lk, pred);
        ok = obj.signaled;
    } else {
        ok = w.cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), pred) && obj.signaled;
    }
    std::erase(obj.waiters, &w);
    return ok ? 1 : 0;
}

// ---- threads ----

Thread* Current() {
    return t_current;
}

std::shared_ptr<Thread> CreateThread(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core, Result* r) {
    *r = kOk;
    if (prio < 0 || prio > 63) {
        *r = kResultInvalidPriority;
        return nullptr;
    }
    const u64 tls = AllocTls();
    if (!tls) {
        *r = kResultOutOfMemory;
        return nullptr;
    }
    auto t = std::make_shared<Thread>();
    t->id = g_next_thread_id.fetch_add(1);
    t->prio = prio;
    t->ideal_core = core < 0 ? 0 : core;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->tls = tls;
    PrepareContext(t.get());
    auto& c = t->ctx;
    c.x[0] = arg;
    c.x[31] = stack_top;
    c.pc = entry;
    c.tpidrro_el0 = tls;
    // FPCR/FPSR start as the creator's (the kernel copies them); kill switch off (FPX1 on).
    if (Thread* cur = Current()) {
        c.fpcr = cur->ctx.fpcr & 0xFFFFFFFFULL;
        c.fpsr = cur->ctx.fpsr;
    }
    t->handle = AddHandle(t);
    if (!t->handle) {
        *r = kResultOutOfHandles;
        return nullptr;
    }
    {
        std::scoped_lock lk{g_threads_lock};
        g_threads.push_back(t);
    }
    return t;
}

Result StartThread(const std::shared_ptr<Thread>& t) {
    if (t->started.exchange(true)) {
        return kResultInvalidState;
    }
    LOGI("Kernel", "thread %llu start entry=%#llx arg=%#llx prio=%d core=%d", t->id, t->entry, t->arg,
         t->prio.load(), t->ideal_core);
    std::thread(HostMain, t).detach();
    return kOk;
}

std::vector<std::shared_ptr<Thread>> Threads() {
    std::scoped_lock lk{g_threads_lock};
    return g_threads;
}

u64 AllocTls() {
    std::scoped_lock lk{g_tls_lock};
    for (auto& [page, used] : g_tls_pages) {
        if (used != 0xFF) {
            for (int s = 0; s < 8; ++s) {
                if (!(used & (1 << s))) {
                    used |= static_cast<u8>(1 << s);
                    const u64 va = page + s * 0x200;
                    std::memset(mem::Ptr(va), 0, 0x200);
                    return va;
                }
            }
        }
    }
    const u64 page = mem::MapAnywhere(mem::kKernelMapStart, mem::kKernelMapSize, kPageSize, mem::ThreadLocal, mem::RW);
    if (!page) {
        return 0;
    }
    g_tls_pages.emplace_back(page, u8{1});
    return page;
}

void ParkCurrent(const char* why) {
    Thread* t = Current();
    LOGC("Kernel", "guest thread %llu stopped: %s; parked until the process ends", t ? t->id : 0, why);
    if (t) {
        LOGC("Kernel", "  pc=%#llx lr=%#llx sp=%#llx x0=%#llx x1=%#llx", t->ctx.pc, t->ctx.x[30], t->ctx.x[31],
             t->ctx.x[0], t->ctx.x[1]);
    }
    log::Flush();
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace plahost::kernel
