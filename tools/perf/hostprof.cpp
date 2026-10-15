// hostprof: sampling profiler for an already running Windows process (no ETW or WPA).
//
// On each interval it briefly suspends every thread that has consumed cycles since the previous
// sample, reads its context and walks the stack with dbghelp (StackWalk64). At the end it groups by
// thread (name from GetThreadDescription): samples, CPU, module and "self" function, and inclusive
// functions. Threads with no new cycles count as "idle" (they are not suspended).
//
//   hostprof.exe <pid> <seconds> <output.txt> [interval_ms=2] [depth=20] [top=25] [symbol_path]
//
// HOSTPROF_RAW=<file>: additionally dumps every busy sample, one per line:
//   <thread> TAB <module>+0x<rva> TAB <module>+0x<rva>...   (top first; "?" + absolute address if there is no module)
// to cross it later with a custom map (e.g. recompiled blocks without a PDB).
//
// HOSTPROF_TRACE=<file>: timeline; every sample (busy or not) with its absolute QPC:
//   <qpc> TAB <thread> TAB <busy 0/1> TAB <top function> | <function> | ...   (up to 10 symbolized frames)
// HOSTPROF_ALL_NAMES=<text>: threads whose name contains <text> are always sampled, even if
//   blocked (to see what they wait on during a hitch), without sampling all the others.
// HOSTPROF_ONLY=<a,b,...>: only threads whose name contains one of those texts are sampled (less cost, more resolution);
//   "@module" selects the threads that start inside that module (e.g. @nvnvk). Unnamed threads are called <module>:tid<n>.
// HOSTPROF_START_QPC=<qpc>: wait until that QPC before starting (to align with another trace).
// HOSTPROF_GUEST=<file>: for plahost "GuestThread*" threads, one line per busy sample with the first host frames
//   (module+rva, comma-separated) so tools/perf/guest-prof.py can charge each sample to its recompiled block and
//   game function (runtime helpers are charged to the calling block). EXPERIMENTAL extra fields: it also looks
//   for the thread's GuestContext (host registers / host stack, validated by its layout) and dumps pc, x30, x29,
//   sp and return-address candidates from the guest stack. Measured 2026-10-09: those values are stale or wrong
//   (the recompiled blocks keep guest registers in host registers), so guest-prof.py's "inclusive" column only
//   repeats the self column; a real guest call stack needs runtime support (see PROGRESS-FPS-GAMECPU.md).
//
// Samples with a kernel wait at the top (ntdll!Zw*/Nt*) do not count as "busy";
// function percentages are over the busy samples.
//
// Build (x64 Native Tools):  cl /O2 /EHsc /std:c++20 hostprof.cpp dbghelp.lib
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

struct ThreadInfo {
    DWORD tid = 0;
    HANDLE h = nullptr;
    std::string name;
    std::string start_mod;
    bool skip = false;
    ULONG64 last_cycles = 0;
    ULONG64 total_cycles = 0;
    unsigned samples = 0;
    unsigned idle = 0;
    std::vector<std::vector<DWORD64>> stacks;
    std::vector<long long> qpcs;
    std::vector<char> busyflag;
    DWORD64 guest_ctx = 0;      // address of this thread's GuestContext in the host process (0 = not found yet)
    unsigned guest_tries = 0;
    std::vector<std::vector<DWORD64>> guest;  // per busy sample: guest pc, x30, then frame-record return addresses
};

// plahost GuestContext layout (tools/port/plahost/src/common.h): x[32] (x[31] = sp), pc @256, mem @272,
// mem_size @280, mem_base_vaddr @288, halted @296, pending_svc @304, vreg @312, tpidr_el0 @824, host_mem @832,
// tpidrro_el0 @840.
struct GuestCtxHead { DWORD64 x[32]; DWORD64 pc; unsigned char nzcv[4]; DWORD64 mem, mem_size, mem_base; };
static bool ReadGuestCtx(HANDLE proc, DWORD64 a, GuestCtxHead& g, DWORD64* tpidrro) {
    if (!a || (a & 63)) return false;
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(a), &g, sizeof g, &got) || got != sizeof g) return false;
    DWORD64 tail[3]{};
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(a + 824), tail, sizeof tail, &got) || got != sizeof tail) return false;
    if (tpidrro) *tpidrro = tail[2];
    return true;
}
static bool LooksLikeGuestCtx(const GuestCtxHead& g, DWORD64 tpidrro) {
    return g.mem && g.mem_size >= (1ull << 30) && g.pc >= 0x80000000ull && g.pc < 0x90000000ull && !(g.pc & 3) &&
           g.x[31] && !(g.x[31] & 15) && tpidrro && g.mem_base < g.mem_size;
}

static std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: hostprof <pid> <seconds> <output> [interval_ms] [depth] [top]\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    const double seconds = std::atof(argv[2]);
    const char* out_path = argv[3];
    const int interval_ms = argc > 4 ? std::atoi(argv[4]) : 2;
    const int depth = argc > 5 ? std::atoi(argv[5]) : 20;
    const int top = argc > 6 ? std::atoi(argv[6]) : 25;
    const char* sym_path = argc > 7 ? argv[7] : nullptr;
    // HOSTPROF_ALL=1: blocked threads are also sampled (to diagnose hangs).
    const bool dump_all = std::getenv("HOSTPROF_ALL") != nullptr;
    const char* all_names = std::getenv("HOSTPROF_ALL_NAMES");
    const char* guest_path = std::getenv("HOSTPROF_GUEST");
    std::unordered_set<DWORD64> guest_used;  // GuestContexts already attributed to a thread

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        std::fprintf(stderr, "OpenProcess %lu failed (%lu)\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(proc, sym_path, TRUE)) {
        std::fprintf(stderr, "SymInitialize failed (%lu)\n", GetLastError());
    }
    auto get_desc = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

    std::map<DWORD, ThreadInfo> threads;
    auto refresh = [&] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof(te)};
        std::unordered_set<DWORD> alive;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            alive.insert(te.th32ThreadID);
            auto& t = threads[te.th32ThreadID];
            if (t.h || t.skip) continue;
            t.tid = te.th32ThreadID;
            t.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, te.th32ThreadID);
            if (t.h && get_desc) {
                PWSTR d = nullptr;
                if (SUCCEEDED(get_desc(t.h, &d)) && d) {
                    t.name = Narrow(d);
                    LocalFree(d);
                }
            }
            if (t.name.empty()) t.name = "tid" + std::to_string(t.tid);
            // Module of the thread start address (for HOSTPROF_ONLY=@module and for naming unnamed threads).
            {
                typedef LONG(NTAPI * NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
                static auto qit = reinterpret_cast<NtQIT>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
                PVOID sa = nullptr;
                if (t.h && qit && qit(t.h, 9 /*ThreadQuerySetWin32StartAddress*/, &sa, sizeof sa, nullptr) == 0 && sa) {
                    SymRefreshModuleList(proc);
                    const DWORD64 base = SymGetModuleBase64(proc, (DWORD64)sa);
                    IMAGEHLP_MODULE64 mi{sizeof(mi)};
                    if (base && SymGetModuleInfo64(proc, base, &mi)) {
                        t.start_mod = mi.ModuleName;
                        if (t.name.rfind("tid", 0) == 0) t.name = t.start_mod + ":" + t.name;
                    }
                }
            }
            // Modules present in the current stack of the thread (for HOSTPROF_ONLY=%module: worker threads of a mod created
            // with std::thread, whose start address is in ucrtbase).
            auto stack_has = [&](const char* modname) -> bool {
                if (!t.h || SuspendThread(t.h) == static_cast<DWORD>(-1)) return false;
                bool found = false;
                CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(t.h, &c)) {
                    STACKFRAME64 f{};
                    f.AddrPC.Offset = c.Rip; f.AddrPC.Mode = AddrModeFlat;
                    f.AddrFrame.Offset = c.Rbp; f.AddrFrame.Mode = AddrModeFlat;
                    f.AddrStack.Offset = c.Rsp; f.AddrStack.Mode = AddrModeFlat;
                    SymRefreshModuleList(proc);
                    for (int i = 0; i < 48 && !found; ++i) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, t.h, &f, &c, nullptr, SymFunctionTableAccess64,
                                         SymGetModuleBase64, nullptr) || f.AddrPC.Offset == 0) break;
                        const DWORD64 b = SymGetModuleBase64(proc, f.AddrPC.Offset);
                        IMAGEHLP_MODULE64 mi{sizeof(mi)};
                        if (b && SymGetModuleInfo64(proc, b, &mi) && _stricmp(mi.ModuleName, modname) == 0) found = true;
                    }
                }
                ResumeThread(t.h);
                return found;
            };
            if (const char* only = std::getenv("HOSTPROF_ONLY")) {
                bool keep = false;
                std::string list = only, tok;
                for (size_t i = 0; i <= list.size(); ++i) {
                    if (i == list.size() || list[i] == ',') { if (!tok.empty() && (tok[0] == '@' ? _stricmp(t.start_mod.c_str(), tok.c_str() + 1) == 0
                                          : tok[0] == '%' ? (t.name.find(':tid') != std::string::npos || t.name.rfind("tid", 0) == 0) && stack_has(tok.c_str() + 1) && (t.name = std::string(tok.c_str() + 1) + "-" + t.name, true)
                                          : t.name.find(tok) != std::string::npos)) keep = true; tok.clear(); }
                    else tok += list[i];
                }
                if (!keep) { CloseHandle(t.h); t.h = nullptr; t.skip = true; continue; }
            }
            if (t.h) QueryThreadCycleTime(t.h, &t.last_cycles);
        }
        CloseHandle(snap);
        SymRefreshModuleList(proc);
    };
    refresh();

    if (const char* sq = std::getenv("HOSTPROF_START_QPC")) {
        const long long target = std::atoll(sq);
        LARGE_INTEGER q;
        for (;;) { QueryPerformanceCounter(&q); if (q.QuadPart >= target) break; Sleep(1); }
        refresh();
    }
    timeBeginPeriod(1);  // without this Sleep(4) sleeps ~15.6 ms and the trace loses resolution
    const auto start = std::chrono::steady_clock::now();
    auto next_refresh = start + std::chrono::seconds(1);
    unsigned ticks = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        if (std::chrono::steady_clock::now() > next_refresh) {
            refresh();
            next_refresh += std::chrono::seconds(1);
        }
        ++ticks;
        for (auto& [tid, t] : threads) {
            if (!t.h) continue;
            ULONG64 cyc = 0;
            if (!QueryThreadCycleTime(t.h, &cyc)) continue;
            const ULONG64 delta = cyc - t.last_cycles;
            t.last_cycles = cyc;
            t.total_cycles += delta;
            const bool forced = all_names && t.name.find(all_names) != std::string::npos;
            if (delta == 0 && !dump_all && !forced) {
                ++t.idle;
                continue;
            }
            if (SuspendThread(t.h) == static_cast<DWORD>(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t.h, &ctx)) {
                std::vector<DWORD64> st;
                STACKFRAME64 f{};
                f.AddrPC.Offset = ctx.Rip;
                f.AddrPC.Mode = AddrModeFlat;
                f.AddrFrame.Offset = ctx.Rbp;
                f.AddrFrame.Mode = AddrModeFlat;
                f.AddrStack.Offset = ctx.Rsp;
                f.AddrStack.Mode = AddrModeFlat;
                st.push_back(ctx.Rip);
                for (int i = 1; i < depth; ++i) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, t.h, &f, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                        break;
                    if (f.AddrPC.Offset == 0) break;
                    if (i > 1 || f.AddrPC.Offset != ctx.Rip) st.push_back(f.AddrPC.Offset);
                }
                t.stacks.push_back(std::move(st));
                if (guest_path && t.name.rfind("GuestThread", 0) == 0) {
                    std::vector<DWORD64> gs;
                    GuestCtxHead g{};
                    DWORD64 tro = 0;
                    if (!t.guest_ctx && t.guest_tries < 2000) {
                        ++t.guest_tries;
                        // ctx registers at sample time (StackWalk64 changed 'ctx'; read again).
                        CONTEXT c2{}; c2.ContextFlags = CONTEXT_FULL;
                        if (GetThreadContext(t.h, &c2)) {
                            const DWORD64 regs[] = {c2.Rbx, c2.Rsi, c2.Rdi, c2.R12, c2.R13, c2.R14, c2.R15, c2.Rbp,
                                                    c2.Rcx, c2.Rdx, c2.R8, c2.R9, c2.R10, c2.R11, c2.Rax};
                            for (DWORD64 r : regs)
                                if (!guest_used.count(r) && ReadGuestCtx(proc, r, g, &tro) && LooksLikeGuestCtx(g, tro)) { t.guest_ctx = r; break; }
                            // Not in a register (e.g. blocked in a wait): look for a pointer to it in the host stack
                            // (the run loop's frame keeps &thread->ctx or the thread).
                            if (!t.guest_ctx) {
                                static thread_local std::vector<DWORD64> buf(8192);
                                SIZE_T got = 0;
                                ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(c2.Rsp), buf.data(), buf.size() * 8, &got);
                                if (!got) ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(c2.Rsp), buf.data(), 4096 - (c2.Rsp & 4095), &got);
                                for (size_t k = 0; k < got / 8 && !t.guest_ctx; ++k) {
                                    const DWORD64 v = buf[k];
                                    if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
                                    for (DWORD64 cand : {v, v + 64, v + 128, v + 192}) {  // &ctx, or Thread* (ctx after the base)
                                        if ((cand & 63) == 0 && !guest_used.count(cand) && ReadGuestCtx(proc, cand, g, &tro) && LooksLikeGuestCtx(g, tro)) {
                                            t.guest_ctx = cand;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (t.guest_ctx) guest_used.insert(t.guest_ctx);
                    if (t.guest_ctx && ReadGuestCtx(proc, t.guest_ctx, g, &tro)) {
                        // The recompiled code may keep x29/x30/sp in host registers inside a block, so the frame
                        // chain is not reliable; dump pc, x30, x29, sp and then every value in the guest stack
                        // (up to 16 KiB above sp) that points into the code range (return-address candidates;
                        // guest-prof.py keeps those preceded by a BL/BLR).
                        gs.push_back(g.pc);
                        gs.push_back(g.x[30]);
                        gs.push_back(g.x[29]);
                        gs.push_back(g.x[31]);
                        const DWORD64 sp = g.x[31] & ~7ull;
                        static thread_local std::vector<DWORD64> sbuf(512);
                        for (int page = 0; page < 4 && gs.size() < 200; ++page) {
                            const DWORD64 va = page == 0 ? sp : ((sp & ~4095ull) + 4096ull * page);
                            const DWORD64 end = (va & ~4095ull) + 4096;
                            if (va < g.mem_base || end - g.mem_base > g.mem_size) break;
                            SIZE_T got = 0;
                            if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(g.mem + (va - g.mem_base)), sbuf.data(),
                                                   end - va, &got) || !got) break;
                            for (size_t k = 0; k < got / 8 && gs.size() < 200; ++k) {
                                const DWORD64 v = sbuf[k];
                                if (v >= 0x80004000ull && v < 0x86000000ull && !(v & 3)) gs.push_back(v);
                            }
                        }
                    }
                    t.guest.push_back(std::move(gs));
                } else if (guest_path) {
                    t.guest.emplace_back();
                }
                LARGE_INTEGER q; QueryPerformanceCounter(&q);
                t.qpcs.push_back(q.QuadPart);
                t.busyflag.push_back(delta != 0);
                ++t.samples;
            }
            ResumeThread(t.h);
        }
        Sleep(interval_ms);
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // Symbolization with a cache.
    std::unordered_map<DWORD64, std::string> fn_cache, mod_cache;
    auto mod_of = [&](DWORD64 a) -> std::string {
        DWORD64 base = SymGetModuleBase64(proc, a);
        auto it = mod_cache.find(base);
        if (it != mod_cache.end()) return it->second;
        IMAGEHLP_MODULE64 mi{sizeof(mi)};
        std::string m = "?";
        if (base && SymGetModuleInfo64(proc, base, &mi)) m = mi.ModuleName;
        mod_cache[base] = m;
        return m;
    };
    auto fn_of = [&](DWORD64 a) -> std::string {
        auto it = fn_cache.find(a);
        if (it != fn_cache.end()) return it->second;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
        auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
        s->SizeOfStruct = sizeof(SYMBOL_INFO);
        s->MaxNameLen = 511;
        DWORD64 disp = 0;
        std::string r = mod_of(a) + "!";
        if (SymFromAddr(proc, a, &disp, s)) r += s->Name; else r += "?";
        fn_cache[a] = r;
        return r;
    };

    auto line_of = [&](DWORD64 a) -> std::string {
        IMAGEHLP_LINE64 li{sizeof(li)};
        DWORD disp = 0;
        std::string r = fn_of(a);
        if (SymGetLineFromAddr64(proc, a, &disp, &li)) {
            const char* f = li.FileName;
            const char* slash = std::strrchr(f, '\\');
            r += " (" + std::string(slash ? slash + 1 : f) + ":" + std::to_string(li.LineNumber) + ")";
        }
        return r;
    };

    FILE* o = std::fopen(out_path, "w");
    if (!o) o = stdout;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    std::fprintf(o, "hostprof pid=%lu duration=%.1fs interval=%dms ticks=%u depth=%d\n",
                 pid, elapsed, interval_ms, ticks, depth);
    std::vector<ThreadInfo*> order;
    for (auto& [tid, t] : threads) order.push_back(&t);
    std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->total_cycles > b->total_cycles; });
    // Summary: Gcycles/s ~ fraction of a core at ~3-6 GHz.
    std::fprintf(o, "\n== threads (Gcycles/s; samples with CPU / ticks) ==\n");
    for (auto* t : order) {
        if (t->total_cycles == 0) continue;
        std::fprintf(o, "%-28s tid=%-6lu %6.2f Gc/s  samples=%-6u idle=%u\n", t->name.c_str(), t->tid,
                     t->total_cycles / elapsed / 1e9, t->samples, t->idle);
    }
    for (auto* t : order) {
        if (t->samples < 20) continue;
        std::map<std::string, unsigned> self_mod, self_fn, incl_fn, self_line, whole;
        unsigned busy = 0;
        for (auto& st : t->stacks) {
            const std::string top_fn = fn_of(st[0]);
            if (!dump_all && (top_fn.rfind("ntdll!Zw", 0) == 0 || top_fn.rfind("ntdll!Nt", 0) == 0)) {
                continue;
            }
            ++busy;
            self_mod[mod_of(st[0])]++;
            self_fn[fn_of(st[0])]++;
            self_line[line_of(st[0])]++;
            {
                std::string key;
                for (size_t k = 0; k < st.size() && k < 12; ++k) key += "\n        " + line_of(st[k]);
                whole[key]++;
            }
            std::unordered_set<std::string> seen;
            for (auto a : st) {
                auto f = fn_of(a);
                if (seen.insert(f).second) incl_fn[f]++;
            }
        }
        auto dump = [&](const char* title, std::map<std::string, unsigned>& m, int n) {
            std::vector<std::pair<unsigned, std::string>> v;
            for (auto& [k, c] : m) v.push_back({c, k});
            std::sort(v.rbegin(), v.rend());
            std::fprintf(o, "  -- %s --\n", title);
            for (int i = 0; i < n && i < (int)v.size(); ++i)
                std::fprintf(o, "    %5.1f%%  %s\n", 100.0 * v[i].first / std::max(1u, busy), v[i].second.c_str());
        };
        std::fprintf(o, "\n== %s (tid %lu): %u samples, busy %.1f%%, %.2f Gc/s ==\n", t->name.c_str(),
                     t->tid, t->samples, 100.0 * busy / std::max(1u, t->samples),
                     t->total_cycles / elapsed / 1e9);
        dump("self by module", self_mod, 10);
        dump("self by function", self_fn, top);
        dump("inclusive by function", incl_fn, top);
        dump("self by line", self_line, 12);
        dump("most frequent stacks (12 frames)", whole, 3);
    }
    if (o != stdout) std::fclose(o);
    if (const char* raw_path = std::getenv("HOSTPROF_RAW")) {
        if (FILE* r = std::fopen(raw_path, "w")) {
            for (auto* t : order) {
                for (auto& st : t->stacks) {
                    const std::string top_fn = fn_of(st[0]);
                    if (!dump_all && (top_fn.rfind("ntdll!Zw", 0) == 0 || top_fn.rfind("ntdll!Nt", 0) == 0)) continue;
                    std::fprintf(r, "%s", t->name.c_str());
                    for (auto a : st) {
                        const DWORD64 base = SymGetModuleBase64(proc, a);
                        if (base) std::fprintf(r, "\t%s+0x%llx", mod_of(a).c_str(), (unsigned long long)(a - base));
                        else std::fprintf(r, "\t?+0x%llx", (unsigned long long)a);
                    }
                    std::fprintf(r, "\n");
                }
            }
            std::fclose(r);
        }
    }
    if (guest_path) {
        if (FILE* r = std::fopen(guest_path, "w")) {
            for (auto* t : order) {
                if (t->name.rfind("GuestThread", 0) != 0) continue;
                std::fprintf(r, "# %s ctx=0x%llx tries=%u\n", t->name.c_str(), (unsigned long long)t->guest_ctx, t->guest_tries);
                for (size_t i = 0; i < t->stacks.size() && i < t->guest.size(); ++i) {
                    const auto& st = t->stacks[i];
                    const std::string top_fn = fn_of(st[0]);
                    if (!dump_all && (top_fn.rfind("ntdll!Zw", 0) == 0 || top_fn.rfind("ntdll!Nt", 0) == 0)) continue;
                    std::fprintf(r, "%s\t", t->name.c_str());
                    for (size_t k = 0; k < st.size() && k < 4; ++k) {  // host frames, comma-separated
                        const DWORD64 base = SymGetModuleBase64(proc, st[k]);
                        std::fprintf(r, "%s%s+0x%llx", k ? "," : "", mod_of(st[k]).c_str(),
                                     (unsigned long long)(base ? st[k] - base : st[k]));
                    }
                    for (auto a : t->guest[i]) std::fprintf(r, "\t%llx", (unsigned long long)a);
                    std::fprintf(r, "\n");
                }
            }
            std::fclose(r);
        }
    }
    if (const char* tr_path = std::getenv("HOSTPROF_TRACE")) {
        if (FILE* r = std::fopen(tr_path, "w")) {
            for (auto* t : order) {
                for (size_t i = 0; i < t->stacks.size(); ++i) {
                    std::fprintf(r, "%lld\t%s\t%d\t", t->qpcs[i], t->name.c_str(), (int)t->busyflag[i]);
                    for (size_t k = 0; k < t->stacks[i].size() && k < 10; ++k)
                        std::fprintf(r, "%s%s", k ? " | " : "", fn_of(t->stacks[i][k]).c_str());
                    std::fprintf(r, "\n");
                }
            }
            std::fclose(r);
        }
    }
    for (auto& [tid, t] : threads) if (t.h) CloseHandle(t.h);
    SymCleanup(proc);
    CloseHandle(proc);
    return 0;
}
