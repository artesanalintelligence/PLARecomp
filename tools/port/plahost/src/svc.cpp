// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The supervisor calls the game makes (Horizon's AArch64 SVC ABI, as documented on switchbrew).

#include "svc.h"
#include "tune.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"

namespace plahost::svc {
namespace {

using kernel::Thread;

std::array<std::atomic<u64>, 0x80> g_counts{};
u64 g_total_memory = 3285ULL << 20;
u64 g_code_size = 0;
u64 g_main_stack = 0;
u64 g_title_id = 0;
u64 g_resource_size = 0;
u64 g_process_local = 0;
std::array<u64, 4> g_entropy{};
constexpr u64 kProcessId = 0x51;

void Unknown(Thread* t, u32 imm) {
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 32) {
        LOGW("SVC", "svc %#x not implemented (thread %llu pc=%#llx x0=%#llx x1=%#llx)", imm, t->id, t->ctx.pc,
             t->ctx.x[0], t->ctx.x[1]);
    }
    t->ctx.x[0] = kResultNotImplemented;
}

Result QueryMemory(u64 out, u64 addr) {
    const mem::Block b = mem::Query(addr);
    struct Info { u64 base, size; u32 state, attr, perm, ipc, device, pad; } info{
        b.base, b.size, b.state, b.attr, b.perm, 0, 0, 0};
    // svc::MemoryState is the low byte of the kernel state: the type, which is what Block keeps.
    if (mem::Write(out, &info, sizeof info) != sizeof info) {
        return kResultInvalidCurrentMemory;
    }
    return kOk;
}

Result MapMemory(u64 dst, u64 src, u64 size) {
    if ((dst | src | size) & (kPageSize - 1) || !size) return kResultInvalidAddress;
    if (dst < mem::kStackStart || dst + size > mem::kStackStart + mem::kStackSize) return kResultInvalidMemoryRegion;
    if (!mem::IsMapped(src, size)) return kResultInvalidCurrentMemory;
    // The same physical pages at dst; the source stays mapped but locked (no access).
    if (const Result r = mem::MapAlias(dst, src, size, mem::Stack, mem::RW); r != kOk) return r;
    mem::SetBlock(src, size, mem::Normal, mem::None, mem::Locked);
    return kOk;
}

Result UnmapMemory(u64 dst, u64 src, u64 size) {
    if ((dst | src | size) & (kPageSize - 1) || !size) return kResultInvalidAddress;
    const mem::Block b = mem::Query(dst);
    if (b.state != mem::Stack || !mem::IsMapped(dst, size)) return kResultInvalidCurrentMemory;
    mem::Unmap(dst, size);
    mem::SetBlock(src, size, mem::Normal, mem::RW, 0);
    return kOk;
}

Result MapSharedMemory(u32 handle, u64 addr, u64 size) {
    auto shm = kernel::Get<kernel::SharedMemory>(handle);
    if (!shm) return kResultInvalidHandle;
    if (size != AlignUp(shm->size, kPageSize)) return kResultInvalidSize;
    if (const Result r = mem::Map(addr, size, mem::Shared, mem::RW); r != kOk) return r;
    if (!shm->backing.empty()) {
        std::memcpy(mem::Ptr(addr), shm->backing.data(), std::min<u64>(size, shm->backing.size()));
    }
    shm->mapped_va = addr;
    LOGI("SVC", "shared memory %s mapped at %#llx (%#llx bytes)", shm->name.c_str(), addr, size);
    return kOk;
}

Result GetInfo(Thread* t, u32 type, u32 handle, u64 sub, u64* out) {
    const u64 used = g_code_size + g_main_stack + mem::NormalMemorySize() + g_resource_size;
    switch (type) {
    case 0: *out = 0x7; return kOk;                                     // CoreMask
    case 1: *out = 0xFFFFFFFFFFFFFFFFULL << 24 >> 4; return kOk;       // PriorityMask (24-59)
    case 2: *out = mem::kAliasStart; return kOk;
    case 3: *out = mem::kAliasSize; return kOk;
    case 4: *out = mem::kHeapStart; return kOk;
    case 5: *out = mem::kHeapSize; return kOk;
    case 6: case 21: *out = g_total_memory; return kOk;                // Total(NonSystem)MemorySize
    case 7: *out = used; return kOk;                                   // UsedMemorySize
    case 22: *out = used - g_resource_size; return kOk;                // UsedNonSystemMemorySize
    case 8: *out = 0; return kOk;                                      // DebuggerAttached
    case 9:                                                            // ResourceLimit
        *out = kernel::AddHandle(std::make_shared<kernel::ResourceLimit>());
        return kOk;
    case 10: *out = 0; return kOk;                                     // IdleTickCount
    case 11:                                                           // RandomEntropy
        if (sub >= 4) return kResultInvalidCombination;
        *out = g_entropy[sub];
        return kOk;
    case 12: *out = mem::kAslrStart; return kOk;
    case 13: *out = mem::kAslrSize; return kOk;
    case 14: *out = mem::kStackStart; return kOk;
    case 15: *out = mem::kStackSize; return kOk;
    case 16: *out = g_resource_size; return kOk;                       // SystemResourceSizeTotal
    case 17: *out = 0; return kOk;                                     // SystemResourceSizeUsed
    case 18: *out = g_title_id; return kOk;                            // ProgramId
    case 19: *out = 0; return kOk;                                     // InitialProcessIdRange
    case 20: *out = g_process_local; return kOk;                       // UserExceptionContextAddress
    case 23: *out = 1; return kOk;                                     // IsApplication
    case 24: *out = 0x80; return kOk;                                  // FreeThreadCount
    case 25: case 0xF0000002: {                                        // ThreadTickCount
        *out = kernel::GuestTicks();
        return kOk;
    }
    case 26: *out = 1; return kOk;                                     // IsSvcPermitted
    case 28: *out = 0; return kOk;                                     // AliasRegionExtraSize
    }
    (void)t;
    (void)handle;
    LOGW("SVC", "GetInfo type %u sub %llu not implemented", type, sub);
    return kResultInvalidEnumValue;
}

} // namespace

void SetMemoryLimits(u64 total_memory, u64 code_size, u64 main_stack_size, u64 title_id, u64 resource_size) {
    g_total_memory = total_memory;
    g_code_size = code_size;
    g_main_stack = main_stack_size;
    g_title_id = title_id;
    g_resource_size = resource_size;
    g_process_local = kernel::AllocTls();
    std::random_device rd;
    for (auto& e : g_entropy) e = (u64{rd()} << 32) | rd();
}

void Call(Thread* t, u32 imm) {
    auto& x = t->ctx.x;
    g_counts[imm & 0x7F].fetch_add(1, std::memory_order_relaxed);
    switch (imm) {
    case 0x01: { // SetHeapSize
        u64 addr = 0;
        x[0] = mem::SetHeapSize(x[1], &addr);
        x[1] = addr;
        LOGI("SVC", "SetHeapSize(%#llx) -> %#x at %#llx", t->ctx.x[1] ? mem::HeapSize() : 0, static_cast<u32>(x[0]), addr);
        return;
    }
    case 0x02: { // SetMemoryPermission
        const mem::Block b = mem::Query(x[0]);
        mem::SetBlock(x[0], x[1], b.state, static_cast<u32>(x[2]), b.attr);
        x[0] = kOk;
        return;
    }
    case 0x03: { // SetMemoryAttribute
        const u64 addr = x[0], size = x[1];
        const u32 mask = static_cast<u32>(x[2]), value = static_cast<u32>(x[3]);
        if (!mem::IsMapped(addr, size)) {
            x[0] = kResultInvalidCurrentMemory;
            return;
        }
        for (u64 at = addr; at < addr + size;) {
            const mem::Block b = mem::Query(at);
            const u64 end = std::min(b.base + b.size, addr + size);
            mem::SetBlock(at, end - at, b.state, b.perm, (b.attr & ~mask) | (value & mask));
            at = end;
        }
        x[0] = kOk;
        return;
    }
    case 0x04: x[0] = MapMemory(x[0], x[1], x[2]); return;
    case 0x05: x[0] = UnmapMemory(x[0], x[1], x[2]); return;
    case 0x06: x[0] = QueryMemory(x[0], x[2]); x[1] = 0; return;
    case 0x07: host::Exit(0, "the game called ExitProcess");
    case 0x08: { // CreateThread
        Result r;
        auto nt = kernel::CreateThread(x[1], x[2], x[3], static_cast<s32>(x[4]), static_cast<s32>(x[5]), &r);
        x[0] = r;
        x[1] = nt ? nt->handle : 0;
        if (nt) {
            LOGI("SVC", "CreateThread entry=%#llx arg=%#llx stack=%#llx prio=%d core=%d -> tid %llu handle %#x",
                 nt->entry, nt->arg, nt->stack_top, nt->prio.load(), static_cast<s32>(t->ctx.x[5]), nt->id, nt->handle);
        }
        return;
    }
    case 0x09: {
        auto nt = kernel::Get<Thread>(static_cast<u32>(x[0]));
        x[0] = nt ? kernel::StartThread(nt) : kResultInvalidHandle;
        return;
    }
    case 0x0A: // ExitThread
        t->exit_requested = true;
        return;
    case 0x0B: { // SleepThread
        const s64 ns = static_cast<s64>(x[0]);
        if (ns > 0) {
            tune::SleepNs(ns);
        } else if (!SwitchToThread()) {
            Sleep(0);
        }
        return;
    }
    case 0x0C: {
        auto th = kernel::Get<Thread>(static_cast<u32>(x[1]));
        x[0] = th ? kOk : kResultInvalidHandle;
        x[1] = th ? static_cast<u64>(th->prio.load()) : 0;
        return;
    }
    case 0x0D: {
        auto th = kernel::Get<Thread>(static_cast<u32>(x[0]));
        if (th) th->prio = static_cast<s32>(x[1]);
        x[0] = th ? kOk : kResultInvalidHandle;
        return;
    }
    case 0x0E: {
        auto th = kernel::Get<Thread>(static_cast<u32>(x[2]));
        x[0] = th ? kOk : kResultInvalidHandle;
        x[1] = th ? static_cast<u64>(static_cast<u32>(th->ideal_core)) : 0;
        x[2] = th ? th->affinity : 0;
        return;
    }
    case 0x0F: {
        auto th = kernel::Get<Thread>(static_cast<u32>(x[0]));
        if (th) {
            const s32 core = static_cast<s32>(x[1]);
            if (core >= 0) th->ideal_core = core;
            th->affinity = x[2];
        }
        x[0] = th ? kOk : kResultInvalidHandle;
        return;
    }
    case 0x11: { // SignalEvent
        kernel::HandleKind kind;
        auto ev = kernel::Get<kernel::Event>(static_cast<u32>(x[0]), &kind);
        if (!ev) {
            x[0] = kResultInvalidHandle;
            return;
        }
        kernel::Signal(*ev);
        x[0] = kOk;
        return;
    }
    case 0x12: { // ClearEvent
        auto ev = kernel::Get<kernel::Event>(static_cast<u32>(x[0]));
        if (!ev) {
            x[0] = kResultInvalidHandle;
            return;
        }
        kernel::Clear(*ev);
        x[0] = kOk;
        return;
    }
    case 0x13: x[0] = MapSharedMemory(static_cast<u32>(x[0]), x[1], x[2]); return;
    case 0x14: { // UnmapSharedMemory
        auto shm = kernel::Get<kernel::SharedMemory>(static_cast<u32>(x[0]));
        if (!shm) {
            x[0] = kResultInvalidHandle;
            return;
        }
        x[0] = mem::Unmap(x[1], x[2]);
        shm->mapped_va = 0;
        return;
    }
    case 0x15: { // CreateTransferMemory
        auto tm = std::make_shared<kernel::TransferMemory>();
        tm->address = x[1];
        tm->size = x[2];
        tm->perm = static_cast<u32>(x[3]);
        x[0] = kOk;
        x[1] = kernel::AddHandle(tm);
        return;
    }
    case 0x16: x[0] = kernel::CloseHandle(static_cast<u32>(x[0])) ? kOk : kResultInvalidHandle; return;
    case 0x17: { // ResetSignal
        kernel::HandleKind kind;
        auto obj = std::dynamic_pointer_cast<kernel::SyncObject>(kernel::GetHandle(static_cast<u32>(x[0]), &kind));
        if (!obj) {
            x[0] = kResultInvalidHandle;
            return;
        }
        std::scoped_lock lk{kernel::Lock()};
        if (!obj->signaled) {
            x[0] = kResultInvalidState;
        } else {
            obj->signaled = false;
            x[0] = kOk;
        }
        return;
    }
    case 0x18: { // WaitSynchronization
        const s32 count = static_cast<s32>(x[2]);
        if (count < 0 || count > 64) {
            x[0] = kResultOutOfRange;
            return;
        }
        u32 handles[64]{};
        if (count && mem::Read(x[1], handles, count * 4u) != count * 4u) {
            x[0] = kResultInvalidPointer;
            return;
        }
        s32 index = -1;
        x[0] = kernel::WaitSynchronization(t, handles, count, static_cast<s64>(x[3]), &index);
        x[1] = static_cast<u64>(static_cast<s64>(index));
        return;
    }
    case 0x19: x[0] = kernel::CancelSynchronization(static_cast<u32>(x[0])); return;
    case 0x1A: sync::ArbitrateLock(t); return;
    case 0x1B: sync::ArbitrateUnlock(t); return;
    case 0x1C: sync::WaitProcessWideKey(t); return;
    case 0x1D: sync::SignalProcessWideKey(t); return;
    case 0x1E: x[0] = kernel::GuestTicks(); return;
    case 0x1F: { // ConnectToNamedPort
        const std::string name = mem::ReadString(x[1], 12);
        auto svc = ipc::ConnectNamedPort(name);
        if (!svc) {
            LOGW("SVC", "ConnectToNamedPort(%s): no such port", name.c_str());
            x[0] = kResultNotFound;
            return;
        }
        x[0] = kOk;
        x[1] = ipc::NewSession(svc);
        return;
    }
    case 0x21: x[0] = ipc::SendSyncRequest(t, static_cast<u32>(x[0]), t->ctx.tpidrro_el0); return;
    case 0x22: x[0] = ipc::SendSyncRequest(t, static_cast<u32>(x[2]), x[0]); return;
    case 0x24: x[0] = kOk; x[1] = kProcessId; return;
    case 0x25: { // GetThreadId
        auto th = kernel::Get<Thread>(static_cast<u32>(x[1]));
        x[0] = th ? kOk : kResultInvalidHandle;
        x[1] = th ? th->id : 0;
        return;
    }
    case 0x26: { // Break
        const u64 reason = x[0];
        LOGC("SVC", "Break(reason %#llx, %#llx, %#llx) from thread %llu pc=%#llx lr=%#llx", reason, x[1], x[2], t->id,
             t->ctx.pc, t->ctx.x[30]);
        if (reason & 0x80000000ULL) { // notification only
            x[0] = kOk;
            return;
        }
        kernel::ParkCurrent("svcBreak");
    }
    case 0x27: { // OutputDebugString
        const std::string s = mem::ReadString(x[0], std::min<u64>(x[1], 0x1000));
        LOGI("Guest", "%s", s.c_str());
        x[0] = kOk;
        return;
    }
    case 0x28: x[0] = kOk; return; // ReturnFromException
    case 0x29: {
        u64 v = 0;
        x[0] = GetInfo(t, static_cast<u32>(x[1]), static_cast<u32>(x[2]), x[3], &v);
        x[1] = v;
        return;
    }
    case 0x2C: x[0] = mem::MapPhysical(x[0], x[1]); return;
    case 0x2D: x[0] = mem::UnmapPhysical(x[0], x[1]); return;
    case 0x30: x[0] = kOk; x[1] = static_cast<u32>(x[2]) == 0 ? g_total_memory : 0x400; return; // GetResourceLimitLimitValue
    case 0x31: x[0] = kOk; x[1] = static_cast<u32>(x[2]) == 0 ? g_code_size + mem::NormalMemorySize() : 0x40; return;
    case 0x32: x[0] = kOk; return; // SetThreadActivity
    case 0x34: sync::WaitForAddress(t); return;
    case 0x35: sync::SignalToAddress(t); return;
    case 0x36: x[0] = kOk; return; // SynchronizePreemptionState
    case 0x45: { // CreateEvent
        auto ev = kernel::NewEvent("guest");
        x[0] = kOk;
        x[1] = kernel::AddHandle(ev, kernel::HandleKind::Writable);
        x[2] = kernel::AddHandle(ev, kernel::HandleKind::Readable);
        return;
    }
    }
    Unknown(t, imm);
}

std::string Report() {
    std::string out = "SVCs:\n";
    for (u32 i = 0; i < g_counts.size(); ++i) {
        if (const u64 n = g_counts[i].load(std::memory_order_relaxed)) {
            char b[64];
            std::snprintf(b, sizeof b, "  svc %#04x %12llu\n", i, static_cast<unsigned long long>(n));
            out += b;
        }
    }
    return out;
}

} // namespace plahost::svc
