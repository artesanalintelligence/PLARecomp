// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "common.h"

namespace plahost::kernel {
struct Thread;
}

namespace plahost::svc {

/// Services supervisor call `imm` for `t` (arguments and results in t->ctx.x[0..7]).
void Call(kernel::Thread* t, u32 imm);

/// Per-SVC counters (log at exit, hang reports).
std::string Report();

void SetMemoryLimits(u64 total_memory, u64 code_size, u64 main_stack_size, u64 title_id, u64 resource_size);

} // namespace plahost::svc

namespace plahost::sync {

/// The synchronization SVCs (0x1A-0x1D, 0x34, 0x35), served on guest memory.
void ArbitrateLock(kernel::Thread* t);
void ArbitrateUnlock(kernel::Thread* t);
void WaitProcessWideKey(kernel::Thread* t);
void SignalProcessWideKey(kernel::Thread* t);
void WaitForAddress(kernel::Thread* t);
void SignalToAddress(kernel::Thread* t);
/// Every native wait now (hang reports).
std::string DescribeWaits();

// FPS-3: the cores of the SVCs above, callable from the guest-function hooks (hle.cpp).
Result ArbitrateLockCore(kernel::Thread* t, u32 owner, u64 addr, u32 tag);
Result ArbitrateUnlockCore(u64 addr);
Result WaitKeyCore(kernel::Thread* t, u64 addr, u64 key, u32 tag, s64 timeout);
void SignalKeyCore(u64 key, s32 count);

// Sync profile (PLAHOST_SYNCPROF=1 with PLAHOST_STATS): per address and game caller.
bool ProfOn();
void ProfAdd(u32 kind, u64 addr, u64 fp, u64 ticks, bool from_svc = true);
std::string ProfReport(double secs, double tick_ms);

} // namespace plahost::sync
