/* os_profile: counts the calls from main to every nn::os function (and pthread / nnos) to size
 * v1.5 (docs/port/V15-DESIGN.md). A single "pre" for all PLT stubs: it does nothing (PLA_CONTINUE);
 * the mod runtime already logs "main+0x... was called N time(s)" on shutdown.
 * Also, every `report_seconds` it logs the call counts of the most used ones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "pla_mod_api.h"

static const pla_api* api;
static uint64_t g_main;
#define NSLOTS 16384 /* PLT of main: 0x32a2000..0x32a6000, 16-byte stubs */
static volatile LONG64 g_count[NSLOTS];
static volatile LONG64 g_last_report;
static ULONGLONG g_t0;
static ULONGLONG g_report_ms = 10000;

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    const char* p;
    api = a;
    g_main = api->module_base("main");
    g_t0 = GetTickCount64();
    p = config ? strstr(config, "\"report_seconds\"") : NULL;
    if (p && (p = strchr(p, ':'))) g_report_ms = (ULONGLONG)(atof(p + 1) * 1000);
    return 0;
}

typedef struct { LONG64 v; int i; } Row;
static int cmp_row(const void* a, const void* b) {
    const LONG64 x = ((const Row*)a)->v, y = ((const Row*)b)->v;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(ULONGLONG now) {
    static Row rows[NSLOTS];
    int n = 0, i;
    char line[160];
    for (i = 0; i < NSLOTS; ++i)
        if (g_count[i]) { rows[n].v = g_count[i]; rows[n].i = i; ++n; }
    qsort(rows, (size_t)n, sizeof(Row), cmp_row);
    snprintf(line, sizeof line, "os_profile t=%.0fs (cumulative; PLT offset in main: calls)", (now - g_t0) / 1000.0);
    api->log(PLA_LOG_INFO, line);
    for (i = 0; i < n && i < 25; ++i) {
        snprintf(line, sizeof line, "  main+%#x %lld", 0x32a2000 + rows[i].i * 16, (long long)rows[i].v);
        api->log(PLA_LOG_INFO, line);
    }
}

PLA_MOD_EXPORT pla_action count_pre(pla_ctx* ctx) {
    const uint64_t off = api->get_pc(ctx) - g_main;
    if (off >= 0x32a2000 && off < 0x32a2000 + NSLOTS * 16) InterlockedIncrement64(&g_count[(off - 0x32a2000) / 16]);
    if (g_report_ms) {
        const ULONGLONG now = GetTickCount64();
        const LONG64 last = g_last_report;
        if (now - g_t0 >= (ULONGLONG)last + g_report_ms &&
            InterlockedCompareExchange64(&g_last_report, (LONG64)(now - g_t0), last) == last)
            report(now);
    }
    return PLA_CONTINUE;
}
