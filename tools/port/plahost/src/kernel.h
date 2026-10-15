// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kernel objects the game still needs: a handle table, events, guest threads (1:1 host
// threads), shared and transfer memory, IPC sessions, and waits on them.

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

namespace plahost::ipc {
class SessionState;
}

namespace plahost::kernel {

struct Object : std::enable_shared_from_this<Object> {
    virtual ~Object() = default;
    virtual const char* TypeName() const = 0;
};

struct Waiter {
    std::condition_variable cv;
    bool woken = false;
    bool cancelled = false;
    u64 waker = 0; // guest thread id that signalled it (0 = a host thread); for tune statistics
};

// An object WaitSynchronization can wait on. `signaled` and `waiters` are guarded by Lock().
struct SyncObject : Object {
    bool signaled = false;
    std::vector<Waiter*> waiters;
};

struct Event final : SyncObject {
    std::string name;
    const char* TypeName() const override { return "Event"; }
};

struct Thread;

struct SharedMemory final : Object {
    u64 size = 0;
    u64 mapped_va = 0; // where the guest mapped it (0 = not mapped)
    std::vector<u8> backing; // contents before the guest maps it
    std::string name;
    const char* TypeName() const override { return "SharedMemory"; }
};

struct TransferMemory final : Object {
    u64 address = 0;
    u64 size = 0;
    u32 perm = 0;
    const char* TypeName() const override { return "TransferMemory"; }
};

struct Process final : SyncObject {
    const char* TypeName() const override { return "Process"; }
};

struct ResourceLimit final : Object {
    const char* TypeName() const override { return "ResourceLimit"; }
};

struct Session final : Object {
    std::shared_ptr<ipc::SessionState> state;
    const char* TypeName() const override { return "Session"; }
};

enum class HandleKind : u8 { Plain, Readable, Writable };

inline constexpr u32 kCurrentThread = 0xFFFF8000;
inline constexpr u32 kCurrentProcess = 0xFFFF8001;

/// The kernel lock: object signal state and every wait.
std::mutex& Lock();

u32 AddHandle(std::shared_ptr<Object> obj, HandleKind kind = HandleKind::Plain);
std::shared_ptr<Object> GetHandle(u32 handle, HandleKind* kind = nullptr);
template <typename T>
std::shared_ptr<T> Get(u32 handle, HandleKind* kind = nullptr) {
    return std::dynamic_pointer_cast<T>(GetHandle(handle, kind));
}
bool CloseHandle(u32 handle);
size_t HandleCount();
std::shared_ptr<Process> CurrentProcess();

// ---- events ----
std::shared_ptr<Event> NewEvent(std::string name);
void Signal(SyncObject& obj);
void Clear(SyncObject& obj);
/// Adds the readable end of `ev` to the handle table (what a service returns as a copy handle).
u32 ReadableHandle(const std::shared_ptr<Event>& ev);

// ---- waits ----
/// svcWaitSynchronization for the calling guest thread.
Result WaitSynchronization(Thread* self, const u32* handles, s32 count, s64 timeout_ns, s32* index);
Result CancelSynchronization(u32 thread_handle);
/// A host thread waiting on one object: 1 signalled, 0 timeout.
int WaitHost(SyncObject& obj, s64 timeout_ns);

// ---- guest threads ----

struct Thread final : SyncObject {
    alignas(64) GuestContext ctx{};
    RecompHostMem bridge{};
    u64 id = 0;
    u32 handle = 0; // the handle the guest got for it
    std::atomic<s32> prio{44};
    s32 ideal_core = 0;
    u64 affinity = 7;
    u64 entry = 0, arg = 0, stack_top = 0;
    u64 tls = 0;
    // Exclusive monitor of this thread (STXR = compare-and-swap against what LDXR saw).
    bool excl_valid = false;
    u64 excl_va = 0;
    u32 excl_size = 0;
    u64 excl_lo = 0, excl_hi = 0;
    // Waits (guarded by Lock()).
    bool cancel_pending = false;
    Waiter* current_wait = nullptr;
    std::vector<u32> wait_handles;
    std::atomic<bool> started{false};
    std::atomic<bool> exit_requested{false};
    std::atomic<bool> exited{false};
    // Diagnostics.
    std::atomic<u32> last_svc{0};
    std::atomic<bool> in_svc{false};
    std::atomic<s64> svc_since_ms{0};
    std::atomic<u64> svc_count{0};
    std::atomic<u32> native_wait{0}; // 0 none, else the sync SVC it is blocked in
    std::string name;
    // Tuning statistics (tune.cpp; written only while tune::g_stats is set).
    void* host_thread = nullptr; // HANDLE of the host thread
    bool is_main = false;
    u64 stat_last_cycles = 0;
    std::atomic<u64> stat_svc_ticks[0x80]{};
    std::atomic<u32> stat_svc_n[0x80]{};
    std::atomic<u64> stat_wait_by[256]{}; // QPC ticks blocked, by the waking thread's id & 255 (0 = host thread)
    std::atomic<u64> stat_wake_lat{0};    // QPC ticks from being woken (sync SVCs) to running again
    std::atomic<u32> stat_wake_n{0};
    // FPS-3 sync profile (stats only): sync-lock (g_m) contention, waits caught by the spin vs slept,
    // wake latency of the slept ones, condvar waits that ended in a timeout.
    std::atomic<u64> stat_gm_ticks{0};
    std::atomic<u32> stat_gm_n{0};
    std::atomic<u32> stat_spin_hit{0};
    std::atomic<u32> stat_sleep_n{0};
    std::atomic<u64> stat_sleep_lat{0};
    std::atomic<u32> stat_timeout_n{0};
    std::atomic<u32> stat_hle_n{0};      // B1: guest sync primitives served by host hooks
    bool hot = false;                    // one of the game's job threads (tune placement): its own spin length
    const char* TypeName() const override { return "Thread"; }
};

Thread* Current();
/// Creates a guest thread (not started). Its handle is in the table.
std::shared_ptr<Thread> CreateThread(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core, Result* r);
Result StartThread(const std::shared_ptr<Thread>& t);
std::vector<std::shared_ptr<Thread>> Threads();
s64 NowMs();

/// The guest's CNTPCT_EL0 (19.2 MHz) from the host's performance counter.
u64 GuestTicks();

/// Thread-local regions (0x200 bytes each, zeroed) in the kernel map region.
u64 AllocTls();

/// Parks the calling guest thread for good (a fatal guest error), writing why.
[[noreturn]] void ParkCurrent(const char* why);

/// Set when the process is exiting: guest threads that reach the dispatcher stop.
extern std::atomic<bool> g_exiting;

} // namespace plahost::kernel
