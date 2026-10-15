// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host tuning for weak machines; see tune.h.

#include "tune.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include <intrin.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>

#include "kernel.h"
#include "log.h"
#include "svc.h"

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "dxgi.lib")

namespace plahost::tune {
namespace {

Config g_cfg;
constexpr int kDefaultWaitSpinUs = 400; // waiter-side spin before sleeping (e6 proxy A/B, LOWEND__scr3: 400 > 150 > 50)
constexpr int kDefaultPlace = 0;        // PLAHOST_PLACE default
std::vector<int> g_core_cpu; // first usable logical CPU of each physical core, faster cores (EfficiencyClass) first
struct CoreInfo {
    KAFFINITY mask = 0; // usable logical CPUs of this physical core
    int eff = 0;        // EfficiencyClass (higher = faster core on hybrid CPUs)
    int sched = 0;      // best SchedulingClass of its CPU sets (favoured / preferred cores rank higher)
};
std::vector<CoreInfo> g_cores; // fastest first
u64 g_qpf = 1;
u64 g_tsc0 = 0, g_qpc0 = 0;

int EnvInt(const char* name, int def) {
    const char* e = std::getenv(name);
    return e && *e ? std::atoi(e) : def;
}

std::string CpuBrand() {
    int r[4];
    char b[49] = {};
    __cpuid(r, 0x80000000);
    if (static_cast<unsigned>(r[0]) < 0x80000004u) return "?";
    for (int i = 0; i < 3; ++i) {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(b + i * 16, r, 16);
    }
    std::string s = b;
    s.erase(0, s.find_first_not_of(' '));
    return s;
}

void DetectCpus() {
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    g_cfg.cpus = static_cast<int>(__popcnt64(proc));
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<char> buf(len);
    auto* base = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len)) return;
    int lo = 255, hi = -1;
    for (DWORD off = 0; off < len;) {
        auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        const auto& pr = p->Processor;
        if (pr.GroupMask[0].Group == 0 && (pr.GroupMask[0].Mask & proc)) {
            ++g_cfg.cores;
            const KAFFINITY m = pr.GroupMask[0].Mask & proc;
            g_core_cpu.push_back(static_cast<int>(_tzcnt_u64(m)) | (pr.EfficiencyClass << 8));
            g_cores.push_back({m, pr.EfficiencyClass, 0});
            lo = std::min<int>(lo, pr.EfficiencyClass);
            hi = std::max<int>(hi, pr.EfficiencyClass);
        }
        off += p->Size;
    }
    g_cfg.hybrid = hi > lo;
    std::stable_sort(g_core_cpu.begin(), g_core_cpu.end(), [](int a, int b) { return (a >> 8) > (b >> 8); });
    for (int& c : g_core_cpu) c &= 0xFF;
    // Preferred cores (Intel favoured cores / AMD CPPC ranking): the CPU sets' SchedulingClass.
    ULONG clen = 0;
    GetSystemCpuSetInformation(nullptr, 0, &clen, GetCurrentProcess(), 0);
    std::vector<char> cbuf(clen);
    if (clen && GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(cbuf.data()), clen, &clen, GetCurrentProcess(), 0)) {
        for (ULONG off = 0; off < clen;) {
            auto* c = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(cbuf.data() + off);
            if (c->Type == CpuSetInformation && c->CpuSet.Group == 0) {
                for (auto& core : g_cores) {
                    if (core.mask & (KAFFINITY{1} << c->CpuSet.LogicalProcessorIndex)) core.sched = std::max<int>(core.sched, c->CpuSet.SchedulingClass);
                }
            }
            off += c->Size;
        }
    }
    std::stable_sort(g_cores.begin(), g_cores.end(), [](const CoreInfo& a, const CoreInfo& b) {
        return a.eff != b.eff ? a.eff > b.eff : a.sched > b.sched;
    });
}

// ---- FPS-3 (B2): the game's job threads (the three busiest guest threads) and their placement ----

std::vector<ULONG> CpuSetIds(KAFFINITY mask) {
    std::vector<ULONG> ids;
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<char> buf(len);
    if (!len || !GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data()), len, &len, GetCurrentProcess(), 0)) return ids;
    for (ULONG off = 0; off < len;) {
        auto* c = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
        if (c->Type == CpuSetInformation && c->CpuSet.Group == 0 && (mask & (KAFFINITY{1} << c->CpuSet.LogicalProcessorIndex))) {
            ids.push_back(c->CpuSet.Id);
        }
        off += c->Size;
    }
    return ids;
}

std::vector<std::shared_ptr<kernel::Thread>> g_hot; // current job threads, busiest first
std::unordered_map<u64, u64> g_class_cycles;
u64 g_class_tsc = 0;

void ApplyPlacement() {
    if (g_cfg.place == 3 && g_cores.size() >= 4) {
        // Soft placement: each job thread gets a different preferred physical core as its ideal processor; no
        // CPU sets, so the scheduler may still move any thread anywhere when that core is busy.
        for (size_t i = 0; i < g_hot.size(); ++i) {
            PROCESSOR_NUMBER pn{};
            pn.Number = static_cast<BYTE>(_tzcnt_u64(g_cores[i % g_cores.size()].mask));
            SetThreadIdealProcessorEx(static_cast<HANDLE>(g_hot[i]->host_thread), &pn, nullptr);
        }
        return;
    }
    if (g_cfg.place != 2 || g_cores.size() < 4) return;
    const size_t reserved = std::min<size_t>(3, g_cores.size() - 1);
    KAFFINITY job_mask = 0, other_mask = 0;
    for (size_t i = 0; i < g_cores.size(); ++i) (i < reserved ? job_mask : other_mask) |= g_cores[i].mask;
    static const std::vector<ULONG> job_ids = CpuSetIds(job_mask), other_ids = CpuSetIds(other_mask);
    if (job_ids.empty() || other_ids.empty()) return;
    std::vector<DWORD> hot_tids;
    for (size_t i = 0; i < g_hot.size(); ++i) {
        HANDLE h = static_cast<HANDLE>(g_hot[i]->host_thread);
        hot_tids.push_back(GetThreadId(h));
        SetThreadSelectedCpuSets(h, job_ids.data(), static_cast<ULONG>(job_ids.size()));
        PROCESSOR_NUMBER pn{};
        pn.Number = static_cast<BYTE>(_tzcnt_u64(g_cores[i % reserved].mask));
        SetThreadIdealProcessorEx(h, &pn, nullptr);
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    int others = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        if (std::find(hot_tids.begin(), hot_tids.end(), te.th32ThreadID) != hot_tids.end()) continue;
        HANDLE h = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SetThreadSelectedCpuSets(h, other_ids.data(), static_cast<ULONG>(other_ids.size()))) ++others;
        CloseHandle(h);
    }
    CloseHandle(snap);
    static size_t logged = 0;
    if (logged < 8) {
        ++logged;
        std::string l;
        char b[64];
        for (const auto& t : g_hot) {
            std::snprintf(b, sizeof b, " t%llu", static_cast<unsigned long long>(t->id));
            l += b;
        }
        LOGI("Tune", "placement: job threads%s on CPUs %#llx, %d other threads on %#llx", l.c_str(),
             static_cast<unsigned long long>(job_mask), others, static_cast<unsigned long long>(other_mask));
    }
}

// Every 2 s for the first minute, then every 10 s: the three guest threads with the most CPU cycles (at least a
// quarter of a core) are the job threads. Used for their spin length and (PLAHOST_PLACE=2) for placement.
void Classify(int n) {
    const bool want = g_cfg.place >= 2 || g_cfg.hot_spin_us != g_cfg.wait_spin_us;
    if (!want || (n > 60 ? n % 10 : n % 2) != 0) return;
    const u64 tsc = __rdtsc();
    const double interval = g_class_tsc ? static_cast<double>(tsc - g_class_tsc) : 0.0;
    g_class_tsc = tsc;
    struct Busy { std::shared_ptr<kernel::Thread> t; u64 d; };
    std::vector<Busy> busy;
    const auto threads = kernel::Threads();
    for (const auto& t : threads) {
        if (!t->host_thread || t->exited) continue;
        ULONG64 cyc = 0;
        QueryThreadCycleTime(static_cast<HANDLE>(t->host_thread), &cyc);
        u64& last = g_class_cycles[t->id];
        const u64 d = last ? cyc - last : 0;
        last = cyc;
        if (interval > 0 && d > interval * 0.25) busy.push_back({t, d});
    }
    std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.d > b.d; });
    if (busy.size() > 3) busy.resize(3);
    std::vector<std::shared_ptr<kernel::Thread>> hot;
    for (auto& b : busy) hot.push_back(b.t);
    std::vector<std::shared_ptr<kernel::Thread>> a = hot, b = g_hot;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    const bool changed = a != b;
    if (changed && hot.size() == 3) {
        for (const auto& t : g_hot) t->hot = false;
        g_hot = hot;
        for (const auto& t : g_hot) t->hot = true;
    }
    if (changed || n % 10 == 0) ApplyPlacement();
}

void SetIdealCore(HANDLE h, int slot) {
    if (g_core_cpu.empty()) return;
    PROCESSOR_NUMBER pn{};
    pn.Group = 0;
    pn.Number = static_cast<BYTE>(g_core_cpu[static_cast<size_t>(slot) % g_core_cpu.size()]);
    SetThreadIdealProcessorEx(h, &pn, nullptr);
}

// nvnvk's submit thread gets its own physical core (slot 3: after the three guest cores).
void PlaceHostThreads() {
    static bool done = false;
    if (done) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR w = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
            if (wcscmp(w, L"nvnvk submit") == 0) {
                SetIdealCore(h, 3);
                done = true;
                LOGI("Tune", "nvnvk submit thread %lu: ideal CPU %d", te.th32ThreadID, g_core_cpu.empty() ? -1 : g_core_cpu[3 % g_core_cpu.size()]);
            }
            LocalFree(w);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

void LogGpus() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return;
    std::string list;
    std::vector<u64> seen;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        a->Release();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        const u64 luid = (static_cast<u64>(static_cast<u32>(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
        if (std::find(seen.begin(), seen.end(), luid) != seen.end()) continue;
        seen.push_back(luid);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        char b[200];
        std::snprintf(b, sizeof b, "%s%s (%llu MiB dedicated)", list.empty() ? "" : "; ", name,
                      static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20));
        list += b;
    }
    f->Release();
    LOGI("Tune", "GPUs, high-performance first: %s", list.c_str());
}

void OptOutPowerThrottling() {
    PROCESS_POWER_THROTTLING_STATE ps{};
    ps.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    ps.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    ps.StateMask = 0; // never throttle: high QoS, timer resolution honoured even when not visible
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &ps, sizeof ps)) {
        LOGW("Tune", "SetProcessInformation(ProcessPowerThrottling) failed: %lu", GetLastError());
    }
}

// ---- statistics ----

struct HostThread {
    HANDLE h = nullptr;
    u64 last = 0;
    std::string name;
};
std::unordered_map<DWORD, HostThread> g_host_threads;
u64 g_last_report_qpc = 0, g_last_report_tsc = 0;
u64 g_last_proc_100ns = 0;

std::string ThreadName(HANDLE h) {
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
        char b[128];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
        out = b;
        LocalFree(w);
    }
    return out;
}

void Report() {
    const u64 qpc = Qpc(), tsc = __rdtsc();
    if (!g_last_report_qpc) {
        g_last_report_qpc = qpc;
        g_last_report_tsc = tsc;
    }
    const double secs = static_cast<double>(qpc - g_last_report_qpc) / g_qpf;
    const double core_cycles = static_cast<double>(tsc - g_last_report_tsc); // one core busy for the interval
    g_last_report_qpc = qpc;
    g_last_report_tsc = tsc;
    if (secs <= 0 || core_cycles <= 0) return;
    const double tick_ms = 1000.0 / g_qpf;
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const u64 p100 = (static_cast<u64>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<u64>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
    const double proc_cores = g_last_proc_100ns ? (p100 - g_last_proc_100ns) / (secs * 1e7) : 0.0;
    g_last_proc_100ns = p100;
    LOGI("Tune", "STATS interval %.1f s: process %.2f cores busy (of %d logical)", secs, proc_cores, g_cfg.cpus);
    std::vector<DWORD> guest_tids;
    char b[1400];
    for (const auto& t : kernel::Threads()) {
        if (!t->host_thread || t->exited) continue;
        HANDLE h = static_cast<HANDLE>(t->host_thread);
        guest_tids.push_back(GetThreadId(h));
        ULONG64 cyc = 0;
        QueryThreadCycleTime(h, &cyc);
        const double cpu = t->stat_last_cycles ? (cyc - t->stat_last_cycles) / core_cycles * 100.0 : 0.0;
        t->stat_last_cycles = cyc;
        struct Item { u32 key; u64 ticks; u32 n; };
        std::vector<Item> svcs, wakers;
        u64 wait_total = 0;
        for (u32 i = 0; i < 0x80; ++i) {
            const u64 tk = t->stat_svc_ticks[i].exchange(0, std::memory_order_relaxed);
            const u32 n = t->stat_svc_n[i].exchange(0, std::memory_order_relaxed);
            if (n) svcs.push_back({i, tk, n});
        }
        for (u32 i = 0; i < 256; ++i) {
            const u64 tk = t->stat_wait_by[i].exchange(0, std::memory_order_relaxed);
            if (tk) {
                wakers.push_back({i, tk, 0});
                wait_total += tk;
            }
        }
        u64 svc_total = 0;
        for (auto& s : svcs) svc_total += s.ticks;
        if (cpu < 2.0 && svc_total * tick_ms < secs * 50.0) continue; // idle or nearly
        std::sort(svcs.begin(), svcs.end(), [](const Item& a, const Item& z) { return a.ticks > z.ticks; });
        std::sort(wakers.begin(), wakers.end(), [](const Item& a, const Item& z) { return a.ticks > z.ticks; });
        int n = std::snprintf(b, sizeof b, "STATS t%llu%s p%d: cpu %.0f%% | in svc %.0f%%:", static_cast<unsigned long long>(t->id),
                              t->is_main ? " main" : "", t->prio.load(), cpu, svc_total * tick_ms / (secs * 10.0));
        for (size_t i = 0; i < svcs.size() && i < 5 && n < 900; ++i) {
            n += std::snprintf(b + n, sizeof b - n, " %02x %.0fms/%u", svcs[i].key, svcs[i].ticks * tick_ms / secs, svcs[i].n);
        }
        const u64 wl = t->stat_wake_lat.exchange(0, std::memory_order_relaxed);
        const u32 wn = t->stat_wake_n.exchange(0, std::memory_order_relaxed);
        n += std::snprintf(b + n, sizeof b - n, " (per s) | wake-up latency %.1f us x %.0f/s = %.0f ms/s | woken by:",
                           wn ? wl * tick_ms * 1000.0 / wn : 0.0, wn / secs, wl * tick_ms / secs);
        for (size_t i = 0; i < wakers.size() && i < 5 && n < 980; ++i) {
            n += std::snprintf(b + n, sizeof b - n, " t%u %.0fms", wakers[i].key, wakers[i].ticks * tick_ms / secs);
        }
        {
            const u64 gt = t->stat_gm_ticks.exchange(0, std::memory_order_relaxed);
            const u32 gn = t->stat_gm_n.exchange(0, std::memory_order_relaxed);
            const u32 sh = t->stat_spin_hit.exchange(0, std::memory_order_relaxed);
            const u32 sn = t->stat_sleep_n.exchange(0, std::memory_order_relaxed);
            const u64 sl = t->stat_sleep_lat.exchange(0, std::memory_order_relaxed);
            const u32 to = t->stat_timeout_n.exchange(0, std::memory_order_relaxed);
            const u32 hn = t->stat_hle_n.exchange(0, std::memory_order_relaxed);
            std::snprintf(b + n, sizeof b - n, " | sync lock contended %.0f/s %.1f ms/s | waits spin-caught %.0f/s slept %.0f/s "
                          "(wake %.1f us) timeout %.0f/s | hle %.0f/s",
                          gn / secs, gt * tick_ms / secs, sh / secs, sn / secs, sn ? sl * tick_ms * 1000.0 / sn : 0.0, to / secs, hn / secs);
        }
        log::Write(log::Info, "Tune", "%s", b);
    }
    // Host threads (nvnvk submit/pipelines, audio, driver threads, ...).
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    std::string line;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        if (std::find(guest_tids.begin(), guest_tids.end(), te.th32ThreadID) != guest_tids.end()) continue;
        auto& ht = g_host_threads[te.th32ThreadID];
        if (!ht.h) {
            ht.h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!ht.h) continue;
        }
        ht.name = ThreadName(ht.h);
        ULONG64 cyc = 0;
        if (!QueryThreadCycleTime(ht.h, &cyc)) continue;
        const double cpu = ht.last ? (cyc - ht.last) / core_cycles * 100.0 : 0.0;
        ht.last = cyc;
        if (cpu >= 2.0) {
            std::snprintf(b, sizeof b, " [%s tid %lu %.0f%%]", ht.name.empty() ? "?" : ht.name.c_str(), te.th32ThreadID, cpu);
            line += b;
        }
    }
    CloseHandle(snap);
    log::Write(log::Info, "Tune", "STATS host threads >= 2%%:%s", line.c_str());
    const std::string prof = sync::ProfReport(secs, tick_ms);
    if (!prof.empty()) log::Write(log::Info, "Tune", "sync profile (top contended addresses):\n%s", prof.c_str());
}

} // namespace

u64 Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<u64>(v.QuadPart);
}

const Config& Get() {
    return g_cfg;
}

void Init() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = static_cast<u64>(f.QuadPart);
    g_qpc0 = Qpc();
    g_tsc0 = __rdtsc();
    DetectCpus();
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) g_cfg.on_battery = ps.ACLineStatus == 0;
    g_cfg.spin_us = std::clamp(EnvInt("PLA_NATIVE_SPIN_US", 20), 0, 1000);
    g_cfg.power_optout = EnvInt("PLAHOST_POWER", 1) != 0;
    // Hand-off policy (docs/port/PROGRESS-FPS-LOWEND.md): with 6+ usable logical CPUs, sync waiters spin briefly on
    // their own wake flag and are woken after the lock is released; with fewer CPUs the spinning would compete with
    // the threads doing the work, so the previous condition-variable path is kept.
    const bool roomy = g_cfg.cpus >= 6;
    g_cfg.wake_unlocked = EnvInt("PLAHOST_WAKE", roomy ? 1 : 0) != 0;
    // FPS-3 (B4): on a few cores without SMT (every usable logical CPU is its own physical core) waiters spin
    // 1000 us instead of 400: the job threads hand a contended lock over ~1,700 times per frame and a sleep +
    // wake-up there costs 20-100 us (6 E-cores: field +11 FPS, town +2.5 FPS). With SMT the longer spin steals
    // the siblings' issue slots (6C/12T: -5 %), so it stays at 400 there. PLAHOST_SPIN_AUTO=0 turns this off;
    // PLAHOST_WAIT_SPIN_US overrides both.
    int wait_default = g_cfg.wake_unlocked ? kDefaultWaitSpinUs : 0;
    g_cfg.spin_auto = EnvInt("PLAHOST_SPIN_AUTO", 0) != 0 && g_cfg.wake_unlocked && g_cfg.cores > 0 &&
                      g_cfg.cores == g_cfg.cpus && g_cfg.cpus <= 8 && !g_cfg.on_battery;
    if (g_cfg.spin_auto) wait_default = 1000;
    g_cfg.wait_spin_us = std::clamp(EnvInt("PLAHOST_WAIT_SPIN_US", wait_default), 0, 5000);
    g_cfg.stats_secs = std::max(0, EnvInt("PLAHOST_STATS", 0));
    g_cfg.mmcss = EnvInt("PLAHOST_MMCSS", 0) != 0;
    g_cfg.prio_mode = EnvInt("PLAHOST_PRIO", 0);
    g_cfg.hr_sleep = EnvInt("PLAHOST_HRSLEEP", 0) != 0;
    g_cfg.place = EnvInt("PLAHOST_PLACE", kDefaultPlace);
    g_cfg.shard_default = false;
    // FPS-3 (B4): the three job threads (the busiest guest threads, see Classify) spin 2000 us before sleeping;
    // they hand their job-queue mutexes over ~1,700 times per frame. Only 3 spinners, so SMT siblings keep their
    // issue slots (6 E-cores field +12 FPS, 6C/12T field +2.5 FPS). Off on battery; PLAHOST_HOT_SPIN_US overrides.
    const int hot_default = (g_cfg.wake_unlocked && !g_cfg.on_battery) ? std::max(g_cfg.wait_spin_us, 2000) : g_cfg.wait_spin_us;
    g_cfg.hot_spin_us = std::clamp(EnvInt("PLAHOST_HOT_SPIN_US", hot_default), 0, 5000);
    g_cfg.hle = EnvInt("PLAHOST_HLE", 0);
    if (g_cfg.power_optout) OptOutPowerThrottling();
    g_stats = g_cfg.stats_secs > 0;
    LOGI("Tune", "CPU %s: %d logical / %d cores usable%s; %s; spin %d us, power opt-out %d, wake unlocked %d, wait spin %d us%s, "
         "mmcss %d, prio mode %d, hr sleep %d, place %d, stats %d s, hot spin %d us, hle %d",
         CpuBrand().c_str(), g_cfg.cpus, g_cfg.cores, g_cfg.hybrid ? " (hybrid P/E)" : "", g_cfg.on_battery ? "ON BATTERY" : "on AC",
         g_cfg.spin_us, g_cfg.power_optout, g_cfg.wake_unlocked, g_cfg.wait_spin_us, g_cfg.spin_auto ? " (auto: no SMT)" : "", g_cfg.mmcss, g_cfg.prio_mode, g_cfg.hr_sleep, g_cfg.place, g_cfg.stats_secs, g_cfg.hot_spin_us, g_cfg.hle);
    LogGpus();
    if (g_cfg.on_battery) LOGW("Tune", "running on battery: Windows limits the CPU and GPU; plug in for full speed");
}

void OnGuestThreadStart(kernel::Thread* t, bool is_main) {
    if (g_cfg.place == 1) SetIdealCore(GetCurrentThread(), t->ideal_core >= 0 ? t->ideal_core : 0);
    if (g_cfg.power_optout) {
        THREAD_POWER_THROTTLING_STATE ts{};
        ts.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        ts.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        ts.StateMask = 0;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &ts, sizeof ts);
    }
    if (is_main && g_cfg.prio_mode == 1) {
        SetThreadPriority(GetCurrentThread(), std::min(GetThreadPriority(GetCurrentThread()) + 1, THREAD_PRIORITY_HIGHEST));
    }
    if (is_main && g_cfg.mmcss) {
        DWORD idx = 0;
        if (!AvSetMmThreadCharacteristicsW(L"Games", &idx)) LOGW("Tune", "MMCSS Games failed: %lu", GetLastError());
    }
}

void SleepNs(s64 ns) {
    if (g_cfg.hr_sleep) {
        thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -std::max<s64>(1, ns / 100); // relative, 100 ns units
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

void Tick() {
    static int n = 0;
    if (g_cfg.place == 1 && n < 60) PlaceHostThreads();
    Classify(n);
    if (g_cfg.stats_secs > 0 && ++n % g_cfg.stats_secs == 0) Report();
}

} // namespace plahost::tune
