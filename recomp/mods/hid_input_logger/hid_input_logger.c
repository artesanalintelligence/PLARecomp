/* hid_input_logger: example mod for the recompiled PLA 1.1.1 (pla_mod_api.h).
 *
 * Hooks the PLT stubs of nn::hid::GetNpadState (the five overloads per
 * controller style) and nn::hid::GetNpadStyleSet in main:
 *   - pre:  saves the output pointer (X0) and the pointer to the controller id (X1),
 *           because X0 no longer holds them when the function returns;
 *   - post: reads the state written by the original (buttons at +8), counts the
 *           calls and, every "report_seconds", writes a line to the suyu log;
 *           optionally forces buttons ("force_buttons") during a time window,
 *           which the game sees as a real press.
 *
 * NpadXxxState (nn::hid): int64 samplingNumber @0, NpadButtonSet buttons @8 (A=bit0,
 * B=1, X=2, Y=3, L=6, R=7, ZL=8, ZR=9, Plus=10, Minus=11, Left=12, Up=13, Right=14,
 * Down=15), sticks @16/@24, attributes @32.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pla_mod_api.h"

enum { STYLE_FULLKEY, STYLE_HANDHELD, STYLE_JOYDUAL, STYLE_JOYLEFT, STYLE_JOYRIGHT, STYLE_COUNT };
static const char* const kStyleName[STYLE_COUNT] = {"FullKey", "Handheld", "JoyDual", "JoyLeft",
                                                    "JoyRight"};

static const pla_api* g_api;
static volatile LONG64 g_calls[STYLE_COUNT];
static volatile LONG64 g_buttons[STYLE_COUNT];
static volatile LONG64 g_style_calls;
static volatile LONG64 g_style_set;
static volatile LONG64 g_forced;
static volatile LONG64 g_next_report_ms;
static ULONGLONG g_t0;

/* config */
static ULONGLONG g_report_ms = 5000;
static uint64_t g_force_buttons;
static ULONGLONG g_force_from_ms, g_force_until_ms, g_force_repeat_ms;
static int g_skip_style_set;          /* replacement of GetNpadStyleSet (PLA_SKIP) */
static uint64_t g_skip_style_set_value;
static volatile LONG64 g_style_skipped;

/* Reads "key": number or "numeric string" from the compact JSON sent by the runtime. */
static int config_number(const char* json, const char* key, double* out) {
    char pattern[64];
    const char* p;
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    p = json ? strstr(json, pattern) : NULL;
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '"') ++p;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) *out = (double)strtoull(p, NULL, 16);
    else *out = strtod(p, NULL);
    return 1;
}

static void mod_log(int level, const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_api->log(level, line);
}

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* api, const char* config_json) {
    double v;
    if (!api || api->version != PLA_MOD_API_VERSION || api->size < sizeof(pla_api)) return 1;
    g_api = api;
    if (config_number(config_json, "report_seconds", &v) && v > 0) g_report_ms = (ULONGLONG)(v * 1000);
    if (config_number(config_json, "force_buttons", &v)) g_force_buttons = (uint64_t)v;
    if (config_number(config_json, "force_after_seconds", &v)) g_force_from_ms = (ULONGLONG)(v * 1000);
    g_force_until_ms = g_force_from_ms + 250;
    if (config_number(config_json, "force_for_seconds", &v)) g_force_until_ms = g_force_from_ms + (ULONGLONG)(v * 1000);
    /* force_repeat_seconds > 0: the same press repeats every N s (tests without a controller). */
    if (config_number(config_json, "force_repeat_seconds", &v) && v > 0) g_force_repeat_ms = (ULONGLONG)(v * 1000);
    if (config_number(config_json, "skip_style_set_value", &v) && v >= 0) {
        g_skip_style_set = 1;
        g_skip_style_set_value = (uint64_t)v;
    }
    g_t0 = GetTickCount64();
    g_next_report_ms = (LONG64)g_report_ms;
    mod_log(PLA_LOG_INFO, "ready: main at %#llx, report every %llu ms, force_buttons=%#llx in [%llu, %llu) ms",
         (unsigned long long)api->module_base("main"), g_report_ms,
         (unsigned long long)g_force_buttons, g_force_from_ms, g_force_until_ms);
    return 0;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    mod_log(PLA_LOG_INFO, "end: FullKey=%lld Handheld=%lld JoyDual=%lld JoyLeft=%lld JoyRight=%lld StyleSet=%lld (replaced %lld) forced=%lld",
         g_calls[0], g_calls[1], g_calls[2], g_calls[3], g_calls[4], g_style_calls, g_style_skipped, g_forced);
}

static void maybe_report(void) {
    const LONG64 now = (LONG64)(GetTickCount64() - g_t0);
    const LONG64 due = g_next_report_ms;
    int s;
    char text[400];
    size_t used = 0;
    if (now < due) return;
    /* Only one thread wins the report of this period. */
    if (InterlockedCompareExchange64(&g_next_report_ms, now + (LONG64)g_report_ms, due) != due) return;
    used += (size_t)snprintf(text + used, sizeof text - used, "t=%.1fs GetNpadState:", now / 1000.0);
    for (s = 0; s < STYLE_COUNT; ++s) {
        if (g_calls[s] == 0) continue;
        used += (size_t)snprintf(text + used, sizeof text - used, " %s=%lld (buttons %#llx)",
                                 kStyleName[s], g_calls[s], (unsigned long long)g_buttons[s]);
    }
    snprintf(text + used, sizeof text - used,
             " | GetNpadStyleSet=%lld (style %#llx, replaced %lld) | forced=%lld",
             g_style_calls, (unsigned long long)g_style_set, g_style_skipped, g_forced);
    g_api->log(PLA_LOG_INFO, text);
}

/* Common to the five overloads: GetNpadState(NpadXxxState* out, const NpadIdType& id). */
PLA_MOD_EXPORT pla_action npad_state_pre(pla_ctx* ctx) {
    uint64_t* data = g_api->call_data(ctx);
    data[0] = g_api->get_x(ctx, 0);
    data[1] = g_api->get_x(ctx, 1);
    return PLA_CONTINUE;
}

static void npad_state_post(pla_ctx* ctx, int style) {
    const uint64_t* data = g_api->call_data(ctx);
    const uint64_t state = data[0];
    uint64_t buttons = pla_read_u64(g_api, ctx, state + 8);
    InterlockedIncrement64(&g_calls[style]);
    g_buttons[style] = (LONG64)buttons;
    if (g_force_buttons) {
        const ULONGLONG t = GetTickCount64() - g_t0;
        const ULONGLONG tw = (g_force_repeat_ms && t >= g_force_from_ms)
                                 ? g_force_from_ms + (t - g_force_from_ms) % g_force_repeat_ms : t;
        if (tw >= g_force_from_ms && tw < g_force_until_ms) {
            buttons |= g_force_buttons;
            if (g_api->write_mem(ctx, state + 8, &buttons, 8) == 8 &&
                InterlockedIncrement64(&g_forced) == 1) {
                mod_log(PLA_LOG_INFO, "forcing buttons %#llx (%s, t=%.1fs)",
                     (unsigned long long)g_force_buttons, kStyleName[style], t / 1000.0);
            }
        }
    }
    maybe_report();
}

PLA_MOD_EXPORT void npad_fullkey_post(pla_ctx* ctx) { npad_state_post(ctx, STYLE_FULLKEY); }
PLA_MOD_EXPORT void npad_handheld_post(pla_ctx* ctx) { npad_state_post(ctx, STYLE_HANDHELD); }
PLA_MOD_EXPORT void npad_joydual_post(pla_ctx* ctx) { npad_state_post(ctx, STYLE_JOYDUAL); }
PLA_MOD_EXPORT void npad_joyleft_post(pla_ctx* ctx) { npad_state_post(ctx, STYLE_JOYLEFT); }
PLA_MOD_EXPORT void npad_joyright_post(pla_ctx* ctx) { npad_state_post(ctx, STYLE_JOYRIGHT); }

/* GetNpadStyleSet(const NpadIdType& id) -> NpadStyleSet in W0.
 * With "skip_style_set_value" the mod replaces the function: the original does not run
 * (nor the IPC to hid) and the caller receives that value. With the value the game already
 * sees (0x1 = FullKey in suyu with the default controller) the behavior does not change,
 * which is useful to check the PLA_SKIP path. */
PLA_MOD_EXPORT pla_action npad_style_set_pre(pla_ctx* ctx) {
    if (!g_skip_style_set) return PLA_CONTINUE;
    g_api->set_x(ctx, 0, g_skip_style_set_value);
    InterlockedIncrement64(&g_style_calls);
    InterlockedIncrement64(&g_style_skipped);
    g_style_set = (LONG64)g_skip_style_set_value;
    return PLA_SKIP;
}

PLA_MOD_EXPORT void npad_style_set_post(pla_ctx* ctx) {
    InterlockedIncrement64(&g_style_calls);
    g_style_set = (LONG64)(g_api->get_x(ctx, 0) & 0xffffffffu);
}
