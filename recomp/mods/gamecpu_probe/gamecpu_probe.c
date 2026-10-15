/* gamecpu_probe: measurement mod for the game-side CPU study (docs/port/PROGRESS-FPS-GAMECPU.md).
 *
 * - Thread creation: logs every nn::os::CreateThread / SetThreadName / SetThreadCoreMask call from main
 *   (ThreadType*, entry, argument, stack size, priority, ideal core, caller) and the result of
 *   GetThreadAvailableCoreMask. plahost's log maps the ThreadType* (the "arg" of its CreateThread line) to
 *   the GuestThread id.
 * - Waits: for the blocking nn::os calls of main (mutex, event, light event, condition variable, semaphore,
 *   multi-wait) it accumulates, per (guest thread, function, caller), the number of calls and the time spent
 *   inside (pre -> post). Yield/Sleep/Try* calls are counted the same way (spin loops show up as huge counts).
 *   Every `report_seconds` the top rows go to gamecpu_probe.log next to the DLL, then the counters restart.
 *
 * Measurement only: it changes nothing in the game (every hook returns PLA_CONTINUE). The post hooks cost a
 * little, so FPS figures are not taken with this mod enabled. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "pla_mod_api.h"

static const pla_api* api;
static uint64_t g_main;
static FILE* g_log;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static ULONGLONG g_report_ms = 10000;
static LARGE_INTEGER g_freq;

static void logf_(const char* fmt, ...) {
    va_list ap;
    if (!g_log) return;
    AcquireSRWLockExclusive(&g_log_lock);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    ReleaseSRWLockExclusive(&g_log_lock);
}

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

/* -------- per-thread id (plahost names its threads GuestThread<id>) -------- */
static __declspec(thread) int t_gid = -1;
static int guest_id(void) {
    if (t_gid < 0) {
        PWSTR d = NULL;
        t_gid = 0;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &d)) && d) {
            if (wcsncmp(d, L"GuestThread", 11) == 0) t_gid = _wtoi(d + 11);
            LocalFree(d);
        }
    }
    return t_gid;
}

/* -------- counters -------- */
enum { F_LOCK, F_WAITEV, F_TIMEDWAITEV, F_WAITCV, F_ACQSEM, F_WAITANY, F_WAITLEV, F_TRYLOCK, F_TRYWAITEV,
       F_TRYACQ, F_TRYWAITANY, F_YIELD, F_SLEEP, F_COUNT };
static const char* k_fname[F_COUNT] = {"LockMutex", "WaitEvent", "TimedWaitEvent", "WaitConditionVariable",
                                       "AcquireSemaphore", "WaitAny", "WaitLightEvent", "TryLockMutex",
                                       "TryWaitEvent", "TryAcquireSemaphore", "TryWaitAny", "YieldThread",
                                       "SleepThread"};
typedef struct { uint64_t key; uint64_t caller; int gid, f; LONG64 calls; LONG64 ticks; } Row;
#define NROWS 16384
static Row g_rows[NROWS];
static SRWLOCK g_rows_lock = SRWLOCK_INIT;
static ULONGLONG g_t0, g_last;
static volatile LONG64 g_next;

static Row* row_for(int gid, int f, uint64_t caller) {
    const uint64_t key = (caller << 16) ^ ((uint64_t)gid << 5) ^ (uint64_t)f ^ 1;
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 64; ++i) {
        Row* r = &g_rows[(h >> 50) + i & (NROWS - 1)];
        if (r->key == key) return r;
        if (r->key == 0) { r->key = key; r->caller = caller; r->gid = gid; r->f = f; return r; }
    }
    return NULL;
}

static int cmp_ticks(const void* a, const void* b) {
    const LONG64 x = ((const Row*)a)->ticks, y = ((const Row*)b)->ticks;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int cmp_calls(const void* a, const void* b) {
    const LONG64 x = ((const Row*)a)->calls, y = ((const Row*)b)->calls;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(ULONGLONG now) {
    static Row snap[NROWS];
    int n = 0;
    const double secs = (now - g_last) / 1000.0;
    AcquireSRWLockExclusive(&g_rows_lock);
    for (int i = 0; i < NROWS; ++i)
        if (g_rows[i].key && g_rows[i].calls) snap[n++] = g_rows[i];
    memset(g_rows, 0, sizeof g_rows);
    ReleaseSRWLockExclusive(&g_rows_lock);
    logf_("\n== t=%.0f s, window %.1f s: %d rows ==\n", (now - g_t0) / 1000.0, secs, n);
    qsort(snap, n, sizeof(Row), cmp_ticks);
    logf_("-- time inside blocking calls (ms per second of window) --\n");
    for (int i = 0; i < n && i < 45; ++i) {
        if (!snap[i].ticks) break;
        logf_("  T%-4d %-22s caller main+0x%07llx  %8.1f ms/s  %9.0f calls/s  %7.1f us/call\n", snap[i].gid,
              k_fname[snap[i].f], (unsigned long long)(snap[i].caller - g_main),
              snap[i].ticks * 1000.0 / g_freq.QuadPart / secs, snap[i].calls / secs,
              snap[i].ticks * 1e6 / g_freq.QuadPart / (double)snap[i].calls);
    }
    qsort(snap, n, sizeof(Row), cmp_calls);
    logf_("-- most frequent calls --\n");
    for (int i = 0; i < n && i < 30; ++i)
        logf_("  T%-4d %-22s caller main+0x%07llx  %9.0f calls/s\n", snap[i].gid, k_fname[snap[i].f],
              (unsigned long long)(snap[i].caller - g_main), snap[i].calls / secs);
    /* per-thread totals */
    {
        static double tot[4096];
        memset(tot, 0, sizeof tot);
        for (int i = 0; i < n; ++i) if (snap[i].gid >= 0 && snap[i].gid < 4096) tot[snap[i].gid] += (double)snap[i].ticks;
        logf_("-- per thread: ms per second inside blocking calls --\n ");
        for (int g = 0; g < 4096; ++g)
            if (tot[g] > 0) logf_(" T%d=%.0f", g, tot[g] * 1000.0 / g_freq.QuadPart / secs);
        logf_("\n");
    }
}

static void maybe_report(void) {
    const ULONGLONG now = GetTickCount64();
    const LONG64 next = g_next;
    if ((LONG64)now >= next &&
        InterlockedCompareExchange64(&g_next, (LONG64)(now + g_report_ms), next) == next) {
        report(now);
        g_last = now;
    }
}

static void count(pla_ctx* ctx, int f, LONG64 ticks) {
    const uint64_t caller = api->get_x(ctx, 30);
    AcquireSRWLockExclusive(&g_rows_lock);
    Row* r = row_for(guest_id(), f, caller);
    if (r) { r->calls++; r->ticks += ticks; }
    ReleaseSRWLockExclusive(&g_rows_lock);
}

/* blocking: pre stores the start time and the caller; post accumulates */
static pla_action wait_pre(pla_ctx* ctx, int f) {
    LARGE_INTEGER q;
    uint64_t* cd = api->call_data(ctx);
    QueryPerformanceCounter(&q);
    cd[0] = (uint64_t)q.QuadPart;
    cd[1] = api->get_x(ctx, 30);
    cd[2] = (uint64_t)f;
    return PLA_CONTINUE;
}
PLA_MOD_EXPORT void wait_post(pla_ctx* ctx) {
    LARGE_INTEGER q;
    uint64_t* cd = api->call_data(ctx);
    QueryPerformanceCounter(&q);
    AcquireSRWLockExclusive(&g_rows_lock);
    Row* r = row_for(guest_id(), (int)cd[2], cd[1]);
    if (r) { r->calls++; r->ticks += (LONG64)((uint64_t)q.QuadPart - cd[0]); }
    ReleaseSRWLockExclusive(&g_rows_lock);
    maybe_report();
}
#define WAIT_HOOK(name, f) PLA_MOD_EXPORT pla_action name(pla_ctx* ctx) { return wait_pre(ctx, f); }
WAIT_HOOK(lock_pre, F_LOCK)
WAIT_HOOK(waitev_pre, F_WAITEV)
WAIT_HOOK(timedwaitev_pre, F_TIMEDWAITEV)
WAIT_HOOK(waitcv_pre, F_WAITCV)
WAIT_HOOK(acqsem_pre, F_ACQSEM)
WAIT_HOOK(waitany_pre, F_WAITANY)
WAIT_HOOK(waitlev_pre, F_WAITLEV)
#define COUNT_HOOK(name, f) PLA_MOD_EXPORT pla_action name(pla_ctx* ctx) { count(ctx, f, 0); maybe_report(); return PLA_CONTINUE; }
COUNT_HOOK(trylock_pre, F_TRYLOCK)
COUNT_HOOK(trywaitev_pre, F_TRYWAITEV)
COUNT_HOOK(tryacq_pre, F_TRYACQ)
COUNT_HOOK(trywaitany_pre, F_TRYWAITANY)
COUNT_HOOK(yield_pre, F_YIELD)
COUNT_HOOK(sleep_pre, F_SLEEP)

/* -------- thread creation -------- */
static void read_str(pla_ctx* ctx, uint64_t va, char* out, size_t n) {
    size_t i = 0;
    for (; va && i + 1 < n; ++i) {
        char c;
        if (api->read_mem(ctx, va + i, &c, 1) != 1 || !c) break;
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[i] = 0;
}
static void create_common(pla_ctx* ctx, int has_core) {
    const uint64_t fn = api->get_x(ctx, 1);
    logf_("CreateThread T%d: thread=0x%llx entry=%s0x%llx arg=0x%llx stack=0x%llx size=0x%llx prio=%d core=%s%d caller=main+0x%llx\n",
          guest_id(), (unsigned long long)api->get_x(ctx, 0), fn - g_main < 0x4351000 ? "main+" : "",
          (unsigned long long)(fn - g_main < 0x4351000 ? fn - g_main : fn), (unsigned long long)api->get_x(ctx, 2),
          (unsigned long long)api->get_x(ctx, 3), (unsigned long long)api->get_x(ctx, 4), (int)api->get_x(ctx, 5),
          has_core ? "" : "default", has_core ? (int)api->get_x(ctx, 6) : 0,
          (unsigned long long)(api->get_x(ctx, 30) - g_main));
}
PLA_MOD_EXPORT pla_action create7_pre(pla_ctx* ctx) { create_common(ctx, 1); return PLA_CONTINUE; }
PLA_MOD_EXPORT pla_action create6_pre(pla_ctx* ctx) { create_common(ctx, 0); return PLA_CONTINUE; }
PLA_MOD_EXPORT pla_action setname_pre(pla_ctx* ctx) {
    char name[64];
    read_str(ctx, api->get_x(ctx, 1), name, sizeof name);
    logf_("SetThreadName thread=0x%llx name=\"%s\" caller=main+0x%llx\n", (unsigned long long)api->get_x(ctx, 0), name,
          (unsigned long long)(api->get_x(ctx, 30) - g_main));
    return PLA_CONTINUE;
}
PLA_MOD_EXPORT pla_action setcore_pre(pla_ctx* ctx) {
    logf_("SetThreadCoreMask thread=0x%llx ideal=%d mask=0x%llx caller=main+0x%llx\n",
          (unsigned long long)api->get_x(ctx, 0), (int)api->get_x(ctx, 1), (unsigned long long)api->get_x(ctx, 2),
          (unsigned long long)(api->get_x(ctx, 30) - g_main));
    return PLA_CONTINUE;
}
PLA_MOD_EXPORT pla_action coremask_pre(pla_ctx* ctx) {
    api->call_data(ctx)[0] = api->get_x(ctx, 30);
    return PLA_CONTINUE;
}
PLA_MOD_EXPORT void coremask_post(pla_ctx* ctx) {
    logf_("GetThreadAvailableCoreMask T%d -> 0x%llx caller=main+0x%llx\n", guest_id(),
          (unsigned long long)api->get_x(ctx, 0), (unsigned long long)(api->call_data(ctx)[0] - g_main));
}

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    char path[MAX_PATH];
    HMODULE self = NULL;
    const char* p;
    api = a;
    g_main = api->module_base("main");
    QueryPerformanceFrequency(&g_freq);
    g_t0 = g_last = GetTickCount64();
    g_next = (LONG64)(g_t0 + g_report_ms);
    p = config ? strstr(config, "\"report_seconds\"") : NULL;
    if (p && (p = strchr(p, ':'))) {
        while (*p == ':' || *p == ' ' || *p == '"') ++p;
        g_report_ms = (ULONGLONG)(atof(p) * 1000);
        g_next = (LONG64)(g_t0 + g_report_ms);
    }
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&pla_mod_init, &self);
    GetModuleFileNameA(self, path, sizeof path);
    {
        char* s = strrchr(path, '\\');
        if (s) strcpy(s + 1, "gamecpu_probe.log");
    }
    g_log = fopen(path, "w");
    logf_("gamecpu_probe: main at 0x%llx, report every %llu ms\n", (unsigned long long)g_main,
          (unsigned long long)g_report_ms);
    api->log(PLA_LOG_INFO, "gamecpu_probe: measuring thread creation and nn::os waits");
    return 0;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}
