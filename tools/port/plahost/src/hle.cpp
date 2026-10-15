// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// FPS-3 (B1): the SDK's internal mutex and condition-variable primitives as host code. Every nn::os mutex,
// condition variable and event of the game goes through six functions of the sdk module
// (InternalCriticalSectionImplByHorizon::Enter/TryEnter/Leave, InternalConditionVariableImplByHorizon::
// Signal/Broadcast/Wait); every caller reaches them through a PLT stub (an indirect branch), so replacing their
// block-index slots catches all calls. The host versions keep the guest memory protocol bit for bit (the lock
// word holds the owner's thread handle, bit 30 = waiters; the condvar word is 1 while it has waiters) and call
// the SVC cores of sync.cpp directly, so they interoperate with any guest code that still uses the SVCs. What
// they save: the recompiled code with its emulated exclusive accesses, the SVC exit/re-entry of the run loop,
// and, for a contended lock, a short barging spin on the host before the guest protocol's wait bit is set.

#include "hle.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "recomp.h"
#include "svc.h"
#include "tune.h"

namespace plahost::hle {
namespace {

constexpr u32 kWaitMask = 0x40000000;

enum Fn { kEnter, kTryEnter, kLeave, kSignal, kBroadcast, kWait, kCount };

struct Site {
    const char* name;
    u64 offset;     // in the sdk module
    u32 expect[2];  // first two instruction words
    BlockFn hook;
    BlockFn original = nullptr;
};

void Enter(void* raw);
void TryEnter(void* raw);
void Leave(void* raw);
void Signal(void* raw);
void Broadcast(void* raw);
void Wait(void* raw);

Site g_sites[kCount] = {
    {"InternalCriticalSection::Enter", 0x3516ac, {0xd100c3ff, 0xa9014ff4}, Enter},
    {"InternalCriticalSection::TryEnter", 0x351760, {0xd53bd068, 0xf940fd08}, TryEnter},
    {"InternalCriticalSection::Leave", 0x351790, {0xd10083ff, 0xa9017bfd}, Leave},
    {"InternalConditionVariable::Signal", 0x3510c0, {0xb9400008, 0x34000068}, Signal},
    {"InternalConditionVariable::Broadcast", 0x3510d4, {0xb9400008, 0x34000068}, Broadcast},
    {"InternalConditionVariable::Wait", 0x3510e8, {0xf81d0ff5, 0xa9014ff4}, Wait},
};

int g_spin_ns = 0;

inline GuestContext* Ctx(void* raw) {
    return static_cast<GuestContext*>(raw);
}

inline void Return(GuestContext* c) {
    c->pc = c->x[30];
}

// What the guest reads: ThreadType* at TLS + 0x1F8, its kernel handle at +0x1B0.
inline u32 CurrentHandle(GuestContext* c) {
    const u64 tt = mem::Load<u64>(c->tpidrro_el0 + 0x1F8);
    return mem::Load<u32>(tt + 0x1B0);
}

inline std::atomic<u32>* Word(u64 addr) {
    if ((addr & 3) != 0 || !mem::IsMapped(addr, 4)) return nullptr;
    return reinterpret_cast<std::atomic<u32>*>(mem::Ptr(addr));
}

inline void Count(kernel::Thread* t) {
    if (tune::g_stats.load(std::memory_order_relaxed)) t->stat_hle_n.fetch_add(1, std::memory_order_relaxed);
}

// Stats bookkeeping like the run loop's for a real SVC, so "in svc" figures stay comparable.
struct SvcTimer {
    kernel::Thread* t;
    u32 imm;
    u64 t0;
    SvcTimer(kernel::Thread* th, u32 i) : t(th), imm(i), t0(tune::g_stats.load(std::memory_order_relaxed) ? tune::Qpc() : 0) {}
    ~SvcTimer() {
        if (!t0) return;
        t->stat_svc_ticks[imm].fetch_add(tune::Qpc() - t0, std::memory_order_relaxed);
        t->stat_svc_n[imm].fetch_add(1, std::memory_order_relaxed);
    }
};

[[noreturn]] void Fatal(const char* what, Result r) {
    char why[160];
    std::snprintf(why, sizeof why, "HLE %s failed with result %#x (the guest would abort here)", what, r);
    kernel::ParkCurrent(why);
}

void Enter(void* raw) {
    GuestContext* c = Ctx(raw);
    const u64 addr = c->x[0];
    auto* word = Word(addr);
    if (!word) return g_sites[kEnter].original(raw);
    kernel::Thread* t = kernel::Current();
    const u32 h = CurrentHandle(c);
    Count(t);
    u32 v = 0;
    if (word->compare_exchange_strong(v, h, std::memory_order_acquire)) return Return(c);
    // Contended: barge in while the holder is about to leave (no wait bit set yet, so its Leave stays an
    // atomic store and needs no kernel call).
    if (g_spin_ns > 0 && !(v & kWaitMask)) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(g_spin_ns);
        for (u32 n = 1;; ++n) {
            YieldProcessor();
            v = word->load(std::memory_order_relaxed);
            if (v == 0) {
                if (word->compare_exchange_strong(v, h, std::memory_order_acquire)) return Return(c);
            } else if (v & kWaitMask) {
                break;
            }
            if ((n & 63) == 0 && std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    const u64 pt0 = sync::ProfOn() ? tune::Qpc() : 0;
    // The guest's protocol (InternalCriticalSectionImplByHorizon::Enter).
    v = word->load(std::memory_order_acquire);
    while (true) {
        if (v == 0) {
            if (word->compare_exchange_weak(v, h, std::memory_order_acquire)) break;
            continue;
        }
        if (!(v & kWaitMask) && !word->compare_exchange_weak(v, v | kWaitMask, std::memory_order_acq_rel)) continue;
        Result r;
        {
            SvcTimer st{t, 0x1A};
            r = sync::ArbitrateLockCore(t, v & ~kWaitMask, addr, h);
        }
        if (r != kOk) Fatal("ArbitrateLock", r);
        v = word->load(std::memory_order_acquire);
        if ((v & ~kWaitMask) == h) break; // ownership handed over by the unlocker
    }
    if (pt0) sync::ProfAdd(6, addr, c->x[29], tune::Qpc() - pt0, false);
    Return(c);
}

void TryEnter(void* raw) {
    GuestContext* c = Ctx(raw);
    auto* word = Word(c->x[0]);
    if (!word) return g_sites[kTryEnter].original(raw);
    Count(kernel::Current());
    u32 v = 0;
    c->x[0] = word->compare_exchange_strong(v, CurrentHandle(c), std::memory_order_acquire) ? 1 : 0;
    Return(c);
}

void Leave(void* raw) {
    GuestContext* c = Ctx(raw);
    const u64 addr = c->x[0];
    auto* word = Word(addr);
    if (!word) return g_sites[kLeave].original(raw);
    kernel::Thread* t = kernel::Current();
    const u32 h = CurrentHandle(c);
    Count(t);
    u32 v = word->load(std::memory_order_relaxed);
    while (v == h) {
        if (word->compare_exchange_weak(v, 0, std::memory_order_release)) return Return(c);
    }
    if (v & kWaitMask) {
        Result r;
        {
            SvcTimer st{t, 0x1B};
            r = sync::ArbitrateUnlockCore(addr);
        }
        if (r != kOk) Fatal("ArbitrateUnlock", r);
    }
    Return(c);
}

void SignalCount(void* raw, s32 count) {
    GuestContext* c = Ctx(raw);
    const u64 key = c->x[0];
    auto* word = Word(key);
    if (!word) return g_sites[count == 1 ? kSignal : kBroadcast].original(raw);
    kernel::Thread* t = kernel::Current();
    Count(t);
    if (word->load(std::memory_order_acquire) != 0) {
        SvcTimer st{t, 0x1D};
        sync::SignalKeyCore(key, count);
    }
    Return(c);
}

void Signal(void* raw) {
    SignalCount(raw, 1);
}

void Broadcast(void* raw) {
    SignalCount(raw, -1);
}

void Wait(void* raw) {
    GuestContext* c = Ctx(raw);
    const u64 cv = c->x[0], cs = c->x[1];
    auto* cs_word = Word(cs);
    if (!cs_word || !Word(cv)) return g_sites[kWait].original(raw);
    const u32 h = CurrentHandle(c);
    if ((cs_word->load(std::memory_order_relaxed) & ~kWaitMask) != h) return g_sites[kWait].original(raw); // guest aborts
    kernel::Thread* t = kernel::Current();
    Count(t);
    const u64 pt0 = sync::ProfOn() ? tune::Qpc() : 0;
    Result r;
    {
        SvcTimer st{t, 0x1C};
        r = sync::WaitKeyCore(t, cs, cv, h, -1);
    }
    if (r != kOk) Fatal("WaitProcessWideKeyAtomic", r);
    if (pt0) sync::ProfAdd(3, cv, c->x[29], tune::Qpc() - pt0, false);
    Return(c);
}

} // namespace

void Install() {
    if (!tune::Get().hle) return;
    const recomp::Image* sdk = recomp::FindImage("sdk");
    if (!sdk || !sdk->index) {
        LOGW("HLE", "sync HLE off: no sdk image index");
        return;
    }
    {
        const char* e = std::getenv("PLAHOST_HLE_SPIN_US"); // barging spin; default = PLAHOST_SPIN_US
        const int us = (e && *e) ? std::atoi(e) : tune::Get().spin_us;
        g_spin_ns = (us < 0 ? 0 : us > 1000 ? 1000 : us) * 1000;
    }
    // All or nothing: the six functions share the lock-word protocol, but each one alone is also correct.
    for (const Site& s : g_sites) {
        const u64 va = sdk->base + s.offset;
        if (va < sdk->lo || va > sdk->hi || !mem::IsMapped(va, 8) || mem::Load<u32>(va) != s.expect[0] ||
            mem::Load<u32>(va + 4) != s.expect[1] || !sdk->index[(va - sdk->lo) >> 2]) {
            LOGW("HLE", "sync HLE off: %s does not match at %#llx (another SDK build?)", s.name, static_cast<unsigned long long>(va));
            return;
        }
    }
    for (Site& s : g_sites) {
        BlockFn* slot = &sdk->index[(sdk->base + s.offset - sdk->lo) >> 2];
        s.original = *slot;
        *slot = s.hook;
    }
    LOGI("HLE", "sync HLE on: %d SDK mutex/condition-variable primitives run as host code (barging spin %d us)", kCount,
         g_spin_ns / 1000);
}

} // namespace plahost::hle
