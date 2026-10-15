// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Horizon's synchronization SVCs on guest memory, ported from the fork's v1.5 runtime
// (core/arm/recomp/recomp_native_sync.cpp, PLARecomp code, GPL-3.0) without the HLE kernel:
// mutex tag = owner handle | 0x40000000 while there are waiters, condition variable key = 1
// while it has waiters, the three arbitration types of each address call. Waiters are kept per
// address in priority order (no priority inheritance).
//
// FPS-3: the waiter lists are split into lock stripes by address (PLAHOST_SHARD, needs the wake-unlocked
// path), a waker only enters the kernel when its waiter is really asleep, and the cores are callable
// directly from the guest-function hooks of hle.cpp (B1).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "kernel.h"
#include "memory.h"
#include "recomp.h"
#include "svc.h"
#include "tune.h"

#pragma comment(lib, "synchronization.lib")

namespace plahost::sync {
namespace {

using kernel::Thread;

constexpr u32 kWaitMask = 0x40000000;

enum class Kind : u8 { Mutex, CondVar, Arbiter };

// Waiter::flag in the wake-unlocked mode.
constexpr u32 kFlagWaiting = 0;  // spinning (or about to sleep)
constexpr u32 kFlagWoken = 1;
constexpr u32 kFlagSleeping = 2; // inside WaitOnAddress: the waker must call WakeByAddressSingle

struct Waiter {
    Kind kind{};
    u64 key{};
    u64 mutex_addr{};
    u32 tag{};
    s32 prio{};
    std::atomic<int> state{0}; // 0 waiting, 1 claimed by a waker, 2 cancelled (timeout)
    Result result{kOk};
    std::condition_variable cv;
    u64 tid{};
    std::chrono::steady_clock::time_point since{};
    u64 waker{};                // guest thread id that woke it (0 = host thread); tune statistics
    u64 wake_qpc{};             // when it was woken (statistics: wake-up latency)
    std::atomic<u32> flag{kFlagWaiting};
    u32 lock_a = 0, lock_b = 0; // the stripes guarding this waiter (two for a condvar waiter: key and mutex)
};

// wake_unlocked mode: the wakes decided under a stripe lock are delivered (WakeByAddressSingle) only after it
// is released, so the woken thread does not run straight into the lock its waker still holds. Only waiters
// that went to sleep are queued; a spinning waiter sees its flag without a system call. WakeByAddressSingle
// only uses the address as a key, so a waiter that already returned (and whose stack slot is gone) is harmless.
thread_local std::vector<std::atomic<u32>*> t_pending_wakes;

void FlushWakes() {
    for (auto* a : t_pending_wakes) WakeByAddressSingle(a);
    t_pending_wakes.clear();
}

struct FlushWakesOnExit {
    ~FlushWakesOnExit() { if (!t_pending_wakes.empty()) FlushWakes(); }
};

using List = std::vector<Waiter*>;

struct alignas(64) Stripe {
    std::mutex m;
    std::unordered_map<u64, List> mutexes, conds, arbs;
};
constexpr u32 kStripes = 64;
Stripe g_stripes[kStripes];

u32 StripeMask() {
    static const u32 mask = [] {
        const char* e = std::getenv("PLAHOST_SHARD");
        const bool on = (e && *e) ? std::atoi(e) != 0 : tune::Get().shard_default;
        return on && tune::Get().wake_unlocked ? kStripes - 1 : 0u;
    }();
    return mask;
}

u32 StripeOf(u64 addr) {
    return static_cast<u32>(((addr >> 2) * 0x9E3779B97F4A7C15ULL) >> 58) & StripeMask();
}

void LockStripe(std::mutex& m) {
    if (m.try_lock()) return;
    if (tune::g_stats.load(std::memory_order_relaxed)) {
        const u64 t0 = tune::Qpc();
        m.lock();
        if (Thread* t = kernel::Current()) {
            t->stat_gm_ticks.fetch_add(tune::Qpc() - t0, std::memory_order_relaxed);
            t->stat_gm_n.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    m.lock();
}

// One or two stripe locks, always taken in ascending stripe order (no deadlock).
class Locks {
public:
    Locks(u32 a, u32 b) {
        if (a > b) std::swap(a, b);
        a_ = &g_stripes[a].m;
        b_ = a == b ? nullptr : &g_stripes[b].m;
        lock();
    }
    explicit Locks(u32 a) : Locks(a, a) {}
    ~Locks() { if (held_) unlock(); }
    Locks(const Locks&) = delete;
    Locks& operator=(const Locks&) = delete;
    void lock() {
        LockStripe(*a_);
        if (b_) LockStripe(*b_);
        held_ = true;
    }
    void unlock() {
        if (b_) b_->unlock();
        a_->unlock();
        held_ = false;
    }
    std::mutex& first() { return *a_; }
    bool single() const { return b_ == nullptr; }
    void forget() { held_ = false; }

private:
    std::mutex* a_ = nullptr;
    std::mutex* b_ = nullptr;
    bool held_ = false;
};

bool Claim(Waiter* w) {
    int expected = 0;
    return w->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
}

std::atomic<u32>* Word(u64 addr) {
    if ((addr & 3) != 0 || !mem::IsMapped(addr, 4)) {
        return nullptr;
    }
    return reinterpret_cast<std::atomic<u32>*>(mem::Ptr(addr));
}

Result BadAddress(u64 addr) {
    return (addr & 3) ? kResultInvalidAddress : kResultInvalidCurrentMemory;
}

void Insert(List& list, Waiter* w) {
    auto it = std::find_if(list.begin(), list.end(), [w](const Waiter* o) { return o->prio > w->prio; });
    list.insert(it, w);
}

bool Remove(std::unordered_map<u64, List>& map, u64 key, Waiter* w) {
    auto it = map.find(key);
    if (it == map.end()) return false;
    auto pos = std::find(it->second.begin(), it->second.end(), w);
    if (pos == it->second.end()) return false;
    it->second.erase(pos);
    if (it->second.empty()) map.erase(it);
    return true;
}

Waiter* PopFront(std::unordered_map<u64, List>& map, u64 key, bool* more) {
    *more = false;
    auto it = map.find(key);
    while (it != map.end() && !it->second.empty()) {
        Waiter* w = it->second.front();
        it->second.erase(it->second.begin());
        const bool claimed = Claim(w);
        *more = !it->second.empty();
        if (it->second.empty()) {
            map.erase(it);
            it = map.end();
        }
        if (claimed) return w;
    }
    return nullptr;
}

void Wake(Waiter* w, Result r) {
    w->result = r;
    const kernel::Thread* cur = kernel::Current();
    w->waker = cur ? cur->id : 0;
    if (tune::g_stats.load(std::memory_order_relaxed)) w->wake_qpc = tune::Qpc();
    if (tune::Get().wake_unlocked) {
        if (w->flag.exchange(kFlagWoken, std::memory_order_acq_rel) == kFlagSleeping) t_pending_wakes.push_back(&w->flag);
    } else {
        w->cv.notify_one();
    }
}

std::unordered_map<u64, List>& MapFor(const Waiter& w) {
    Stripe& s = g_stripes[StripeOf(w.key)];
    return w.kind == Kind::Mutex ? s.mutexes : w.kind == Kind::CondVar ? s.conds : s.arbs;
}

int SpinUs(Thread* t) {
    const auto& cfg = tune::Get();
    return (t && t->hot) ? cfg.hot_spin_us : cfg.wait_spin_us;
}

Result BlockUnlocked(Locks& lk, Waiter& w, s64 timeout_ns, Thread* t) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + nanoseconds(timeout_ns > 0 ? timeout_ns : 0);
    lk.unlock();
    FlushWakes();
    const bool stats = tune::g_stats.load(std::memory_order_relaxed);
    if (const int spin = SpinUs(t); spin > 0) {
        const auto end = steady_clock::now() + microseconds(spin);
        for (u32 n = 1; w.flag.load(std::memory_order_acquire) != kFlagWoken; ++n) {
            YieldProcessor();
            if ((n & 63) == 0 && steady_clock::now() >= end) break;
        }
        if (stats && w.flag.load(std::memory_order_acquire) == kFlagWoken && t) {
            t->stat_spin_hit.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool slept = false;
    while (true) {
        u32 f = w.flag.load(std::memory_order_acquire);
        if (f == kFlagWoken) {
            if (slept && stats && t && w.wake_qpc) {
                t->stat_sleep_n.fetch_add(1, std::memory_order_relaxed);
                const u64 now = tune::Qpc();
                if (now > w.wake_qpc) t->stat_sleep_lat.fetch_add(now - w.wake_qpc, std::memory_order_relaxed);
            }
            return w.result; // returns without the stripe locks: the callers only read the result
        }
        DWORD ms = 100;
        if (timeout_ns > 0) {
            const auto now = steady_clock::now();
            if (now >= deadline) {
                lk.lock();
                // Under the waiter's locks a claimed waiter (state 1) has already been woken (flag set).
                if (w.state.load(std::memory_order_acquire) == 1 || w.flag.load(std::memory_order_acquire) == kFlagWoken) {
                    lk.unlock();
                    return w.result;
                }
                Remove(MapFor(w), w.key, &w);
                w.state.store(2);
                if (stats && t) t->stat_timeout_n.fetch_add(1, std::memory_order_relaxed);
                lk.unlock();
                return kResultTimedOut;
            }
            const auto left = duration_cast<microseconds>(deadline - now).count();
            ms = static_cast<DWORD>(std::min<long long>(100, (left + 999) / 1000));
        }
        if (f == kFlagWaiting) {
            if (!w.flag.compare_exchange_strong(f, kFlagSleeping, std::memory_order_acq_rel)) continue; // woken meanwhile
        }
        slept = true;
        u32 sleeping = kFlagSleeping;
        WaitOnAddress(&w.flag, &sleeping, sizeof sleeping, ms);
    }
}

Result Block(Locks& lk, Waiter& w, s64 timeout_ns, Thread* t) {
    using namespace std::chrono;
    if (tune::Get().wake_unlocked) return BlockUnlocked(lk, w, timeout_ns, t);
    // Condition-variable path: only with one global stripe (StripeMask() == 0), so lk is a single mutex.
    std::unique_lock<std::mutex> ul(lk.first(), std::adopt_lock);
    lk.forget();
    const auto deadline = steady_clock::now() + nanoseconds(timeout_ns > 0 ? timeout_ns : 0);
    while (w.state.load(std::memory_order_acquire) != 1) {
        if (timeout_ns > 0) {
            const auto now = steady_clock::now();
            if (now >= deadline) {
                Remove(MapFor(w), w.key, &w);
                w.state.store(2);
                return kResultTimedOut;
            }
            w.cv.wait_for(ul, std::min<steady_clock::duration>(deadline - now, milliseconds(100)));
        } else {
            w.cv.wait_for(ul, milliseconds(100));
        }
    }
    return w.result;
}

Waiter* NewWaiter(Waiter& w, Kind kind, u64 key, Thread* t) {
    w.kind = kind;
    w.key = key;
    w.prio = t->prio.load(std::memory_order_relaxed);
    w.tid = t->id;
    w.since = std::chrono::steady_clock::now();
    return &w;
}

s64 SpinNs() {
    static const s64 v = static_cast<s64>(tune::Get().spin_us) * 1000;
    return v;
}

void RecordWait(Thread* t, const Waiter& w, u64 t0, Result r) {
    if (!t0 || r != kOk) return;
    const u64 now = tune::Qpc();
    t->stat_wait_by[w.waker & 0xFF].fetch_add(now - t0, std::memory_order_relaxed);
    if (w.wake_qpc && now > w.wake_qpc) {
        t->stat_wake_lat.fetch_add(now - w.wake_qpc, std::memory_order_relaxed);
        t->stat_wake_n.fetch_add(1, std::memory_order_relaxed);
    }
}

// Under the locks of the condvar key's stripe and of w->mutex_addr's stripe.
void SignalImplLocked(Waiter* w) {
    auto* word = Word(w->mutex_addr);
    if (!word) {
        Wake(w, kResultInvalidCurrentMemory);
        return;
    }
    u32 prev = word->load(std::memory_order_seq_cst);
    while (true) {
        if (prev == 0) {
            if (word->compare_exchange_weak(prev, w->tag, std::memory_order_seq_cst)) {
                Wake(w, kOk);
                return;
            }
        } else if ((prev & kWaitMask) ||
                   word->compare_exchange_weak(prev, prev | kWaitMask, std::memory_order_seq_cst)) {
            break;
        }
    }
    w->kind = Kind::Mutex;
    w->key = w->mutex_addr;
    w->state.store(0, std::memory_order_release);
    Insert(g_stripes[StripeOf(w->mutex_addr)].mutexes[w->mutex_addr], w);
}

// ---- sync profile (PLAHOST_SYNCPROF=1 with PLAHOST_STATS): contended addresses and their game callers ----

struct ProfEntry {
    std::atomic<u64> key{0};
    u64 addr = 0, c1 = 0, c2 = 0;
    u32 kind = 0;
    std::atomic<u64> n{0}, ticks{0};
};
constexpr u32 kProfSize = 4096;
ProfEntry g_prof[kProfSize];
std::atomic<int> g_prof_on{-1};

} // namespace

bool ProfOn() {
    int v = g_prof_on.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = std::getenv("PLAHOST_SYNCPROF");
        v = (e && *e && std::atoi(e) != 0 && tune::Get().stats_secs > 0) ? 1 : 0;
        g_prof_on.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

namespace {

u64 LoadGuest64(u64 va) {
    return (va && (va & 7) == 0 && mem::IsMapped(va, 8)) ? mem::Load<u64>(va) : 0;
}

// The two return addresses above the nn::os function: from an SVC, fp (x29) is the frame of the SDK's internal
// primitive (skipped); from a hook, the primitive never ran and fp is already the nn::os function's frame.
void Callers(u64 fp, bool from_svc, u64* c1, u64* c2) {
    *c1 = *c2 = 0;
    if (from_svc) fp = LoadGuest64(fp);
    if (!fp) return;
    *c1 = LoadGuest64(fp + 8);
    const u64 fp1 = LoadGuest64(fp);
    *c2 = fp1 ? LoadGuest64(fp1 + 8) : 0;
}

} // namespace

void ProfAdd(u32 kind, u64 addr, u64 fp, u64 ticks, bool from_svc) {
    if (!ProfOn()) return;
    u64 c1, c2;
    Callers(fp, from_svc, &c1, &c2);
    const u64 key = ((addr * 0x9E3779B97F4A7C15ULL) ^ (c1 * 0xC2B2AE3D27D4EB4FULL) ^ (c2 * 0x165667B19E3779F9ULL) ^ kind) | 1;
    for (u32 i = 0, h = static_cast<u32>(key >> 40) % kProfSize; i < 64; ++i, h = (h + 1) % kProfSize) {
        ProfEntry& e = g_prof[h];
        u64 k = e.key.load(std::memory_order_acquire);
        if (k == 0) {
            u64 zero = 0;
            if (e.key.compare_exchange_strong(zero, ~0ULL, std::memory_order_acq_rel)) {
                e.addr = addr;
                e.c1 = c1;
                e.c2 = c2;
                e.kind = kind;
                e.key.store(key, std::memory_order_release);
                k = key;
            } else {
                k = zero;
            }
        }
        if (k == key) {
            e.n.fetch_add(1, std::memory_order_relaxed);
            e.ticks.fetch_add(ticks, std::memory_order_relaxed);
            return;
        }
    }
}

std::string ProfReport(double secs, double tick_ms) {
    if (!ProfOn()) return {};
    struct Row { u32 kind; u64 addr, c1, c2, n, ticks; };
    std::vector<Row> rows;
    for (auto& e : g_prof) {
        const u64 k = e.key.load(std::memory_order_acquire);
        if (k == 0 || k == ~0ULL) continue;
        const u64 n = e.n.exchange(0, std::memory_order_relaxed);
        const u64 tk = e.ticks.exchange(0, std::memory_order_relaxed);
        if (n) rows.push_back({e.kind, e.addr, e.c1, e.c2, n, tk});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.n > b.n; });
    const auto where = [](u64 va, char* out, size_t sz) {
        const recomp::Image* im = recomp::ImageFor(va);
        if (im) std::snprintf(out, sz, "%s+%#llx", im->name.c_str(), static_cast<unsigned long long>(va - im->base));
        else std::snprintf(out, sz, "%#llx", static_cast<unsigned long long>(va));
    };
    static const char* kinds[] = {"?", "lock(1a)", "unlock(1b)", "cvwait(1c)", "signal(1d)", "arbwait(34)", "enter", "leave", "cvsig"};
    std::string out;
    char b[400], w1[64], w2[64];
    for (size_t i = 0; i < rows.size() && i < 24; ++i) {
        const Row& r = rows[i];
        where(r.c1, w1, sizeof w1);
        where(r.c2, w2, sizeof w2);
        std::snprintf(b, sizeof b, "SYNCPROF %-11s addr %#llx from %s < %s: %.0f/s %.1f ms/s\n", kinds[r.kind < 9 ? r.kind : 0],
                      static_cast<unsigned long long>(r.addr), w1, w2, r.n / secs, r.ticks * tick_ms / secs);
        out += b;
    }
    return out;
}

// ---- cores (also called by the guest-function hooks, hle.cpp) ----

Result ArbitrateLockCore(Thread* t, u32 owner, u64 addr, u32 tag) {
    auto* word = Word(addr);
    if (!word) return BadAddress(addr);
    if (const s64 spin = SpinNs(); spin > 0) {
        const u32 contended = owner | kWaitMask;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin);
        for (u32 n = 1;; ++n) {
            YieldProcessor();
            if (word->load(std::memory_order_acquire) != contended) return kOk;
            if ((n & 63) == 0 && std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    FlushWakesOnExit flush_wakes;
    const u32 s = StripeOf(addr);
    Locks lk{s};
    if (word->load(std::memory_order_seq_cst) != (owner | kWaitMask)) return kOk;
    Waiter w;
    NewWaiter(w, Kind::Mutex, addr, t);
    w.tag = tag;
    w.lock_a = w.lock_b = s;
    Insert(g_stripes[s].mutexes[addr], &w);
    t->native_wait = 0x1A;
    const u64 t0 = tune::g_stats.load(std::memory_order_relaxed) ? tune::Qpc() : 0;
    const Result r = Block(lk, w, -1, t);
    RecordWait(t, w, t0, r);
    t->native_wait = 0;
    return r;
}

Result ArbitrateUnlockCore(u64 addr) {
    auto* word = Word(addr);
    if (!word) return BadAddress(addr);
    FlushWakesOnExit flush_wakes;
    const u32 s = StripeOf(addr);
    Locks lk{s};
    bool more = false;
    Waiter* next = PopFront(g_stripes[s].mutexes, addr, &more);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!next) {
        word->store(0, std::memory_order_seq_cst);
    } else {
        word->store(next->tag | (more ? kWaitMask : 0), std::memory_order_seq_cst);
        Wake(next, kOk);
    }
    return kOk;
}

Result WaitKeyCore(Thread* t, u64 addr, u64 key, u32 tag, s64 timeout) {
    auto* word = Word(addr);
    auto* key_word = Word(key);
    if (!word || !key_word) return BadAddress(addr);
    FlushWakesOnExit flush_wakes;
    const u32 sm = StripeOf(addr), sk = StripeOf(key);
    Locks lk{sm, sk};
    bool more = false;
    Waiter* next = PopFront(g_stripes[sm].mutexes, addr, &more);
    const u32 next_value = next ? (next->tag | (more ? kWaitMask : 0)) : 0;
    if (next) Wake(next, kOk);
    key_word->store(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    word->store(next_value, std::memory_order_seq_cst);
    if (timeout == 0) return kResultTimedOut;
    Waiter w;
    NewWaiter(w, Kind::CondVar, key, t);
    w.mutex_addr = addr;
    w.tag = tag;
    w.lock_a = sk;
    w.lock_b = sm;
    Insert(g_stripes[sk].conds[key], &w);
    t->native_wait = 0x1C;
    const u64 t0 = tune::g_stats.load(std::memory_order_relaxed) ? tune::Qpc() : 0;
    const Result r = Block(lk, w, timeout, t);
    RecordWait(t, w, t0, r);
    t->native_wait = 0;
    return r;
}

void SignalKeyCore(u64 key, s32 count) {
    FlushWakesOnExit flush_wakes;
    const u32 sk = StripeOf(key);
    u32 sm = sk;
    s32 n = 0;
    while (true) {
        Locks lk{sk, sm};
        auto& conds = g_stripes[sk].conds;
        bool relock = false;
        while (count <= 0 || n < count) {
            auto it = conds.find(key);
            if (it == conds.end() || it->second.empty()) break;
            const u32 s = StripeOf(it->second.front()->mutex_addr);
            if (s != sk && s != sm) { // this waiter's mutex lives in a stripe not held: take it too, in order
                sm = s;
                relock = true;
                break;
            }
            bool more = false;
            Waiter* w = PopFront(conds, key, &more);
            if (!w) break;
            SignalImplLocked(w);
            ++n;
        }
        if (relock) continue;
        auto it = conds.find(key);
        if (it == conds.end() || it->second.empty()) {
            if (it != conds.end()) conds.erase(it);
            if (auto* kw = Word(key)) kw->store(0, std::memory_order_seq_cst);
        }
        return;
    }
}

// ---- the SVCs ----

void ArbitrateLock(Thread* t) {
    auto& x = t->ctx.x;
    const u64 t0 = ProfOn() ? tune::Qpc() : 0;
    const u64 addr = x[1];
    x[0] = ArbitrateLockCore(t, static_cast<u32>(x[0]), addr, static_cast<u32>(x[2]));
    if (t0) ProfAdd(1, addr, t->ctx.x[29], tune::Qpc() - t0);
}

void ArbitrateUnlock(Thread* t) {
    auto& x = t->ctx.x;
    const u64 t0 = ProfOn() ? tune::Qpc() : 0;
    const u64 addr = x[0];
    x[0] = ArbitrateUnlockCore(addr);
    if (t0) ProfAdd(2, addr, t->ctx.x[29], tune::Qpc() - t0);
}

void WaitProcessWideKey(Thread* t) {
    auto& x = t->ctx.x;
    const u64 t0 = ProfOn() ? tune::Qpc() : 0;
    const u64 key = x[1];
    x[0] = WaitKeyCore(t, x[0], key, static_cast<u32>(x[2]), static_cast<s64>(x[3]));
    if (t0) ProfAdd(3, key, t->ctx.x[29], tune::Qpc() - t0);
}

void SignalProcessWideKey(Thread* t) {
    auto& x = t->ctx.x;
    const u64 t0 = ProfOn() ? tune::Qpc() : 0;
    SignalKeyCore(x[0], static_cast<s32>(x[1]));
    if (t0) ProfAdd(4, x[0], t->ctx.x[29], tune::Qpc() - t0);
}

void WaitForAddress(Thread* t) {
    auto& x = t->ctx.x;
    const u64 addr = x[0];
    const u32 type = static_cast<u32>(x[1]);
    const s32 value = static_cast<s32>(x[2]);
    const s64 timeout = static_cast<s64>(x[3]);
    auto* word = Word(addr);
    if (!word) {
        x[0] = BadAddress(addr);
        return;
    }
    const u64 pt0 = ProfOn() ? tune::Qpc() : 0;
    FlushWakesOnExit flush_wakes;
    const u32 s = StripeOf(addr);
    Locks lk{s};
    switch (type) {
    case 0:
        if (static_cast<s32>(word->load(std::memory_order_seq_cst)) >= value) {
            x[0] = kResultInvalidState;
            return;
        }
        break;
    case 1: {
        u32 cur = word->load(std::memory_order_seq_cst);
        while (true) {
            if (static_cast<s32>(cur) >= value) {
                x[0] = kResultInvalidState;
                return;
            }
            if (word->compare_exchange_weak(cur, cur - 1, std::memory_order_seq_cst)) break;
        }
        break;
    }
    case 2:
        if (static_cast<s32>(word->load(std::memory_order_seq_cst)) != value) {
            x[0] = kResultInvalidState;
            return;
        }
        break;
    default:
        x[0] = kResultInvalidEnumValue;
        return;
    }
    if (timeout == 0) {
        x[0] = kResultTimedOut;
        return;
    }
    Waiter w;
    NewWaiter(w, Kind::Arbiter, addr, t);
    w.lock_a = w.lock_b = s;
    Insert(g_stripes[s].arbs[addr], &w);
    t->native_wait = 0x34;
    const u64 t0 = tune::g_stats.load(std::memory_order_relaxed) ? tune::Qpc() : 0;
    x[0] = Block(lk, w, timeout, t);
    RecordWait(t, w, t0, static_cast<Result>(x[0]));
    t->native_wait = 0;
    if (pt0) ProfAdd(5, addr, t->ctx.x[29], tune::Qpc() - pt0);
}

void SignalToAddress(Thread* t) {
    auto& x = t->ctx.x;
    const u64 addr = x[0];
    const u32 type = static_cast<u32>(x[1]);
    const s32 value = static_cast<s32>(x[2]);
    const s32 count = static_cast<s32>(x[3]);
    auto* word = Word(addr);
    if (!word) {
        x[0] = BadAddress(addr);
        return;
    }
    FlushWakesOnExit flush_wakes;
    const u32 s = StripeOf(addr);
    Locks lk{s};
    auto& arbs = g_stripes[s].arbs;
    switch (type) {
    case 0:
        break;
    case 1: {
        u32 expected = static_cast<u32>(value);
        if (!word->compare_exchange_strong(expected, expected + 1, std::memory_order_seq_cst)) {
            x[0] = kResultInvalidState;
            return;
        }
        break;
    }
    case 2: {
        const auto it = arbs.find(addr);
        const bool any = it != arbs.end() && !it->second.empty();
        s32 new_value;
        if (count <= 0) {
            new_value = any ? value - 2 : value + 1;
        } else if (any) {
            s32 tmp = 0;
            for (size_t i = 1; i < it->second.size(); ++i) {
                if (tmp++ >= count) break;
            }
            new_value = tmp < count ? value - 1 : value;
        } else {
            new_value = value + 1;
        }
        u32 expected = static_cast<u32>(value);
        if (new_value != value) {
            if (!word->compare_exchange_strong(expected, static_cast<u32>(new_value), std::memory_order_seq_cst)) {
                x[0] = kResultInvalidState;
                return;
            }
        } else if (word->load(std::memory_order_seq_cst) != expected) {
            x[0] = kResultInvalidState;
            return;
        }
        break;
    }
    default:
        x[0] = kResultInvalidEnumValue;
        return;
    }
    s32 n = 0;
    bool more = true;
    while (more && (count <= 0 || n < count)) {
        Waiter* w = PopFront(arbs, addr, &more);
        if (!w) break;
        Wake(w, kOk);
        ++n;
    }
    x[0] = kOk;
}

std::string DescribeWaits() {
    std::string out = "\n== native sync waits\n";
    const auto now = std::chrono::steady_clock::now();
    const char* kinds[] = {"mutex", "condvar", "arbiter"};
    char buf[256];
    const auto dump = [&](const char* title, const std::unordered_map<u64, List>& map) {
        if (map.empty()) return;
        std::snprintf(buf, sizeof buf, "  %s (%zu keys)\n", title, map.size());
        out += buf;
        for (const auto& [key, list] : map) {
            auto* w0 = Word(key);
            std::snprintf(buf, sizeof buf, "    key %#llx word %#010x:", key, w0 ? w0->load() : 0u);
            out += buf;
            for (const Waiter* w : list) {
                std::snprintf(buf, sizeof buf, " [t%llu %s prio %d tag %#x %lld ms]", w->tid,
                              kinds[static_cast<int>(w->kind)], w->prio, w->tag,
                              static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - w->since).count()));
                out += buf;
                if (w->kind == Kind::CondVar) {
                    // Its mutex: free (0) while every thread waits = the wake-up never came (lost, or never sent).
                    auto* mw = Word(w->mutex_addr);
                    std::snprintf(buf, sizeof buf, "{mutex %#llx = %#x}", w->mutex_addr, mw ? mw->load() : 0xFFFFFFFFu);
                    out += buf;
                }
            }
            out += "\n";
        }
    };
    for (u32 s = 0; s <= StripeMask(); ++s) {
        std::unique_lock lk{g_stripes[s].m, std::defer_lock};
        for (int i = 0; i < 200 && !lk.try_lock(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!lk.owns_lock()) {
            std::snprintf(buf, sizeof buf, "  stripe %u: lock HELD for more than 2 s\n", s);
            out += buf;
            continue;
        }
        dump("mutexes", g_stripes[s].mutexes);
        dump("condition variables", g_stripes[s].conds);
        dump("arbiters", g_stripes[s].arbs);
    }
    return out;
}

} // namespace plahost::sync
