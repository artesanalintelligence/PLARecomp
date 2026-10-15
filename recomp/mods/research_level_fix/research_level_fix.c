/* research_level_fix: avoids the ~25 s freeze on the screen where research is reported to the professor
 * (T1, sprint 3).
 *
 * main+0x102f528 (evaluation of the research tasks of the creature catalog) calls, for every task
 * (x27 = 0..n), main+0x1030984, which returns in w0 the levels GAINED: (u8)new - (u8)saved.
 * The caller uses w0 as the u32 counter of a loop (main+0x10303e4..0x1030418: "raise the 3-bit level
 * field, cap 6") without checking the sign. If the computed level is LOWER than the saved one
 * (which happens with saves whose progress does not match the tables, e.g. the downloaded one), w0 = -1
 * -> 2^32 iterations: ~25 s at 0 FPS on the PC (~40 s on the console). It is not a port bug: the
 * original code does the same; we fix it here.
 *
 * Fix: `post` hook of main+0x1030984 that, if (int32)w0 < 0, returns 0 (no level was gained; the saved
 * level is not touched). config: {"clamp": true|false, "log_max": N}. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pla_mod_api.h"

static const pla_api* api;
static int g_clamp = 1;
static long g_log_max = 20;
static unsigned long long g_calls, g_negative, g_logged;

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    const char* p;
    api = a;
    p = config ? strstr(config, "\"clamp\"") : NULL;
    if (p && (p = strchr(p, ':'))) {
        while (*++p == ' ') {}
        g_clamp = (*p == 't' || *p == '1');
    }
    p = config ? strstr(config, "\"log_max\"") : NULL;
    if (p && (p = strchr(p, ':'))) g_log_max = atol(p + 1);
    return 0;
}

PLA_MOD_EXPORT pla_action levels_gained_pre(pla_ctx* ctx) {
    uint64_t* d = api->call_data(ctx);
    d[0] = api->get_x(ctx, 1);
    d[1] = api->get_x(ctx, 2);
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT void levels_gained_post(pla_ctx* ctx) {
    const int32_t r = (int32_t)(uint32_t)api->get_x(ctx, 0);
    ++g_calls;
    if (r >= 0) return;
    ++g_negative;
    if ((long)g_logged < g_log_max) {
        const uint64_t* d = api->call_data(ctx);
        char line[160];
        ++g_logged;
        snprintf(line, sizeof line, "research_level_fix: levels gained %d (w1=%u task=%u)%s", r,
                 (unsigned)d[0], (unsigned)d[1], g_clamp ? " -> 0" : " (not corrected)");
        api->log(PLA_LOG_WARNING, line);
    }
    if (g_clamp) api->set_x(ctx, 0, 0);
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    char line[160];
    if (!api) return;
    snprintf(line, sizeof line, "research_level_fix: %llu calls, %llu negative (%s)", g_calls, g_negative,
             g_clamp ? "corrected" : "not corrected");
    api->log(PLA_LOG_INFO, line);
}
