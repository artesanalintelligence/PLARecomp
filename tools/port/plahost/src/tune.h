// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host tuning for weak machines (few cores, hybrid CPUs, laptops on battery): CPU/power detection at start,
// per-thread scheduling policy for guest threads, and optional thread statistics (PLAHOST_STATS=<seconds>).

#pragma once

#include <atomic>

#include "common.h"

namespace plahost::kernel {
struct Thread;
}

namespace plahost::tune {

struct Config {
    int cpus = 0;            // logical CPUs in the process affinity mask
    int cores = 0;           // physical cores among them
    bool hybrid = false;     // both P- and E-cores in the mask
    bool on_battery = false;
    bool spin_auto = false;  // FPS-3: PLAHOST_SPIN_AUTO=1 (default 0): 1000 us wait spin on <= 8 cores without SMT
    int spin_us = 20;        // ArbitrateLock spin before blocking (PLA_NATIVE_SPIN_US overrides)
    bool power_optout = true;   // opt out of EcoQoS / power throttling (PLAHOST_POWER=0 disables)
    bool wake_unlocked = false; // sync waits: wake after releasing the lock (PLAHOST_WAKE=1)
    int wait_spin_us = 0;       // sync waits: spin on the wake flag before sleeping (PLAHOST_WAIT_SPIN_US)
    int stats_secs = 0;         // PLAHOST_STATS=<seconds>: per-thread CPU/wait report in the log
    bool mmcss = false;         // PLAHOST_MMCSS=1: MMCSS "Games" for the guest main thread
    int prio_mode = 0;          // PLAHOST_PRIO: 0 = default mapping, 1 = main thread one level higher
    bool hr_sleep = false;      // PLAHOST_HRSLEEP=1: svcSleepThread on a high-resolution waitable timer
    int place = 0;              // PLAHOST_PLACE=1: ideal processor per guest core / nvnvk submit on distinct physical cores;
                                // 2: the three busiest guest threads (the job threads) on reserved physical cores, the rest elsewhere;
                                // 3: soft, the job threads' ideal processors on distinct preferred physical cores only
    // FPS-3
    bool shard_default = false; // PLAHOST_SHARD: sync waiter lists split in lock stripes (default off)
    int hot_spin_us = 0;        // PLAHOST_HOT_SPIN_US: waiter spin of the job threads (default 2000 on AC, else wait_spin_us)
    int hle = 0;                // PLAHOST_HLE=1: the SDK's internal mutex / condition variable primitives run as host code
};

/// Detects the machine and reads the environment once; logs one summary line. Call early in Run().
void Init();
const Config& Get();

/// On the guest thread itself, right after it starts.
void OnGuestThreadStart(kernel::Thread* t, bool is_main);

/// From the watchdog once per second (writes the stats report every stats_secs).
void Tick();

inline std::atomic<bool> g_stats{false};

/// svcSleepThread with ns > 0 (PLAHOST_HRSLEEP=1: high-resolution waitable timer instead of Sleep's 1 ms steps).
void SleepNs(s64 ns);

/// QueryPerformanceCounter ticks (for the statistics).
u64 Qpc();

} // namespace plahost::tune
