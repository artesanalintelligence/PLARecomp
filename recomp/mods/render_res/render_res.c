/* render_res: configurable internal resolution for PLA 1.1.1 without re-exporting or recompiling main.
 *
 * The Switch "docked" resolution mods (Fl4sh_, pchtxt 1.1.1) change the 1920/1080 immediates at five
 * sites of main. Here the code is AOT (the immediates live inside the generated C), so instead of
 * patching instructions the game is allowed to write 1920/1080 and, in the first AOT block that follows
 * (the return of a BL), a `pre` hook rewrites the value already stored in memory:
 *
 *   site (Switch mod)         what it is                              hook (block)      value rewritten
 *   0x277BD44/50 mov w9/w8    framework config (window)                0x2779FD8         [sp+0x20]+0x18/+0x20 (u64)
 *   0x29E99E8/F4 movk x8      {handheld,docked} width/height table     0x29E9A38         [x21+0x3F4]/[x21+0x3FC] (u32)
 *   0x29F0410/14 mov w8/w9    render target descriptor (vf15)          0x29F0440         [x29-0xC0]/[x29-0xB8] (u64)
 *   0x29F0650/54 mov w8/w9    render target descriptor (vf15)          0x29F067C         [x29-0xC0]/[x29-0xB8] (u64)
 *   0x2B9A5FC/604 mov w8      render target descriptor                 0x2B9A638         [x19+0x7E8]/[x19+0x7F0] (u64)
 *   (not in the Switch mod)   960x540 / 480x270 effect RTs (15 callers) 0x2AC011C (entry) [x1]/[x1+8] -> size / 2 or / 4
 *
 * Every rewrite first checks that the stored value is 1920x1080 (if not, it touches nothing and says
 * so in the log): this protects against a wrong context.
 *
 * Also: the graphics memory reserve for render targets is enlarged according to the area (rr_gfxmem,
 * main+0x2A8C3E8) and dynamic resolution can be disabled (rr_dyn_*, config "dynamic_res").
 * Details and tests: docs/port/RESOLUTION.md.
 *
 * config: {"width": "1920", "height": "1080", "dynamic_res": "true"} (defaults = original game).
 * Extra keys for tests only: "probe": "true" (logs the created RTs and the state of the dynamic controller;
 * the RT dump also needs the hook main+0x2AC011C -> rr_probe_rt), "pool_rt" (% of the RT reserve),
 * "pool_a"/"pool_b" (% of the framework reserves; no useful effect). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "pla_mod_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const pla_api* api;
static uint64_t g_main;
/* g_w/g_h: effective internal resolution (mode table -> screen size; can change live).
 * g_rw/g_rh: size of the render targets, framework and memory reserve, fixed at boot = max(width, reserve_width).
 * Live changes of the internal resolution go up to g_rw (docs/port/LIVE-SETTINGS.md). */
static uint32_t g_w = 1920, g_h = 1080;
static uint32_t g_rw = 1920, g_rh = 1080;
static int g_dyn = 1;   /* 1 = dynamic resolution as in the original game */
static int g_probe = 0;
static long g_rt_pool_pct = -1;   /* % of the render target reserve; -1 = relative area */
static long g_pool_pct[2] = {-1, -1}; /* % of the two framework graphics memory reserves; -1 = automatic */
static volatile long g_hits[12];

#define DEF_W 1920u
#define DEF_H 1080u

static void logf_(int lvl, const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    api->log(lvl, line);
}

/* Looks up "key" in the config JSON and returns the value (quoted or not) in out. */
static int cfg_get(const char* json, const char* key, char* out, size_t n) {
    char pat[64];
    const char* p;
    size_t i = 0;
    if (!json) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return 0;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p == '"') ++p;
    while (*p && *p != '"' && *p != ',' && *p != '}' && *p != ' ' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

static int is_true(const char* s) { return s[0] == 't' || s[0] == 'T' || s[0] == '1' || s[0] == 'y' || s[0] == 's'; }

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    char v[32];
    long w = DEF_W, h = DEF_H;
    api = a;
    g_main = api->module_base("main");
    if (cfg_get(config, "width", v, sizeof v)) w = atol(v);
    if (cfg_get(config, "height", v, sizeof v)) h = atol(v);
    if (cfg_get(config, "dynamic_res", v, sizeof v)) g_dyn = is_true(v);
    if (cfg_get(config, "probe", v, sizeof v)) g_probe = is_true(v);
    if (cfg_get(config, "pool_a", v, sizeof v)) g_pool_pct[0] = atol(v); /* tests only */
    if (cfg_get(config, "pool_b", v, sizeof v)) g_pool_pct[1] = atol(v);
    if (cfg_get(config, "pool_rt", v, sizeof v)) g_rt_pool_pct = atol(v);
    /* 16:9 required; width a multiple of 16 (so the height, width*9/16, is an integer). */
    if (w < 640) w = 640;
    if (w > 3840) { /* beyond 4K the render targets do not fit in the game heap (untested) */
        logf_(PLA_LOG_WARNING, "render_res: width %ld > 3840; using 3840", w);
        w = 3840;
    }
    w &= ~15L;
    if (h != w * 9 / 16) {
        logf_(PLA_LOG_WARNING, "render_res: %ldx%ld is not 16:9 (or the width is not a multiple of 16); using %ldx%ld", w, h,
              w, w * 9 / 16);
        h = w * 9 / 16;
    }
    g_w = (uint32_t)w;
    g_h = (uint32_t)h;
    {
        long r = 0;
        if (cfg_get(config, "reserve_width", v, sizeof v)) r = atol(v);
        if (r > 3840) r = 3840;
        r &= ~15L;
        g_rw = r > (long)g_w ? (uint32_t)r : g_w;
        g_rh = g_rw * 9 / 16;
    }
    logf_(PLA_LOG_INFO, "render_res: internal resolution %ux%u, dynamic resolution %s%s (main=%#llx)", g_w, g_h,
          g_dyn ? "as in the game" : "DISABLED", g_probe ? ", probe" : "", (unsigned long long)g_main);
    if (g_rw != g_w)
        logf_(PLA_LOG_INFO, "render_res: render targets reserved at %ux%u (live headroom)", g_rw, g_rh);
    return 0;
}

/* Rewrites the width/height pair at va_w/va_h (size = 4 or 8 bytes) if it holds 1920/1080. */
static void patch_pair_to(pla_ctx* ctx, int site, const char* name, uint64_t va_w, uint64_t va_h, unsigned size,
                          uint32_t tw, uint32_t th) {
    uint64_t w = 0, h = 0;
    long n = _InterlockedIncrement(&g_hits[site]);
    if (api->read_mem(ctx, va_w, &w, size) != size || api->read_mem(ctx, va_h, &h, size) != size) {
        logf_(PLA_LOG_WARNING, "render_res: %s: could not read %#llx", name, (unsigned long long)va_w);
        return;
    }
    if (w != DEF_W || h != DEF_H) {
        if (n <= 4) logf_(PLA_LOG_WARNING, "render_res: %s: found %llux%llu (expected 1920x1080), leaving it alone", name,
                          (unsigned long long)w, (unsigned long long)h);
        return;
    }
    if (tw == DEF_W && th == DEF_H) {
        if (n <= 2) logf_(PLA_LOG_INFO, "render_res: %s: 1920x1080 (unchanged)", name);
        return;
    }
    w = tw;
    h = th;
    api->write_mem(ctx, va_w, &w, size);
    api->write_mem(ctx, va_h, &h, size);
    if (n <= 4) logf_(PLA_LOG_INFO, "render_res: %s: 1920x1080 -> %ux%u (call %ld)", name, tw, th, n);
}

/* Render targets, framework and reserve: the boot size with headroom. */
static void patch_pair(pla_ctx* ctx, int site, const char* name, uint64_t va_w, uint64_t va_h, unsigned size) {
    patch_pair_to(ctx, site, name, va_w, va_h, size, g_rw, g_rh);
}

/* main+0x2779FD8: return from main+0x277BD2C (fills the framework config at sp+0x20). */
PLA_MOD_EXPORT pla_action rr_framework(pla_ctx* ctx) {
    const uint64_t s = api->get_sp(ctx) + 0x20;
    static const uint64_t orig[2] = {0x6000000, 0x2B00000};
    int i;
    patch_pair(ctx, 0, "framework (0x277BD44)", s + 0x18, s + 0x20, 8);
    /* +0x08/+0x10: sizes of the two graphics memory reserves (96 and 43 MiB). At higher resolutions
     * the render targets do not fit in the first one (null allocations at main+0x2A8D750). */
    for (i = 0; i < 2; ++i) {
        uint64_t cur = 0, nv;
        long pct = g_pool_pct[i];
        if (pct < 0) pct = 100; /* no measurable effect on the null allocations: tests only */
        if (pct == 100) continue;
        if (api->read_mem(ctx, s + 0x8 + 8 * i, &cur, 8) != 8 || cur != orig[i]) {
            logf_(PLA_LOG_WARNING, "render_res: reserve %d: unexpected %#llx, leaving it alone", i, (unsigned long long)cur);
            continue;
        }
        nv = (cur * (uint64_t)pct / 100 + 0xFFFFF) & ~0xFFFFFull;
        api->write_mem(ctx, s + 0x8 + 8 * i, &nv, 8);
        logf_(PLA_LOG_INFO, "render_res: graphics reserve %d: %llu -> %llu MiB (%ld %%)", i,
              (unsigned long long)(cur >> 20), (unsigned long long)(nv >> 20), pct);
    }
    return PLA_CONTINUE;
}

/* main+0x2A8C3E8 (pre): creates the three graphics memory reserves (nvn::MemoryPool) of the render system from
 * the sizes at x1: [x1+0] flags 0x22 (uncached CPU, GPU), [x1+8] flags 0xA1 (GPU only, compressible: render
 * targets), [x1+0x10] flags 0x12. At higher resolutions the render target reserve runs short: the allocator
 * (main+0x2A919BC) returns null and the game reads [0+0x30] at main+0x2A8D750.
 * [x1+8] is enlarged according to the area (or with "pool_rt" in % for tests). */
PLA_MOD_EXPORT pla_action rr_gfxmem(pla_ctx* ctx) {
    const uint64_t c = api->get_x(ctx, 1);
    const uint64_t area_pct = ((uint64_t)g_rw * g_rh * 100 + DEF_W * DEF_H - 1) / (DEF_W * DEF_H);
    uint64_t sz[3] = {0}, nv;
    /* automatic: 100 % + 0.62 x (area - 100 %). Measured: 2560x1440 needs more than 250 MiB (with 445 nothing
     * fails); 3840x2160 works with 600 MiB (240 %) and with 1000 MiB (400 %) the game heap runs out. Since the
     * half/quarter-resolution RTs are scaled too (rr_rt_frac), 2560x1440 fails at 340 MiB (136 %) and works at
     * 363 MiB (145 %): the coefficient went from 0.47 to 0.62. */
    long pct = g_rt_pool_pct >= 0 ? g_rt_pool_pct : (area_pct > 100 ? 100 + (long)(area_pct - 100) * 62 / 100 : 100);
    const long n = _InterlockedIncrement(&g_hits[9]);
    if (api->read_mem(ctx, c, sz, sizeof sz) != sizeof sz) return PLA_CONTINUE;
    if (n <= 4)
        logf_(PLA_LOG_INFO, "render_res: graphics reserves: 0x22=%llu MiB, 0xA1 (RT)=%llu MiB, 0x12=%llu MiB",
              (unsigned long long)(sz[0] >> 20), (unsigned long long)(sz[1] >> 20), (unsigned long long)(sz[2] >> 20));
    if (pct <= 100 || sz[1] == 0) return PLA_CONTINUE;
    nv = (sz[1] * (uint64_t)pct / 100 + 0xFFFFF) & ~0xFFFFFull;
    api->write_mem(ctx, c + 8, &nv, 8);
    if (n <= 4)
        logf_(PLA_LOG_INFO, "render_res: render target reserve %llu -> %llu MiB (%ld %%)",
              (unsigned long long)(sz[1] >> 20), (unsigned long long)(nv >> 20), pct);
    return PLA_CONTINUE;
}

/* main+0x29E9A38: return of a BL after writing the table {1280,1920} / {720,1080} at x21+0x3F0/0x3F8. */
static volatile uint64_t g_obj;   /* object with the mode table (x21 here; param_1 of main+0x29EABA8) */
static volatile uint32_t g_cur_w = DEF_W, g_cur_h = DEF_H; /* what the docked entry of the table holds now */

PLA_MOD_EXPORT pla_action rr_mode_table(pla_ctx* ctx) {
    const uint64_t o = api->get_x(ctx, 21);
    patch_pair_to(ctx, 1, "mode table (0x29E99E8)", o + 0x3F4, o + 0x3FC, 4, g_w, g_h);
    g_obj = o;
    g_cur_w = g_w;
    g_cur_h = g_h;
    return PLA_CONTINUE;
}

/* main+0x29EABA8 (pre): FUN_29eaba8(obj) computes the screen size from the mode table (or UserResolutionWidth/Height):
 * [main+0x42EF568]+0x128/+0x12C and the "ResolutionWidth/Height" parameters. The game calls it at startup and when the
 * console mode changes (callback registered in vtable_71041830d0::vf5); render_res also calls it to change the
 * internal resolution live. Here it only logs (and checks that x0 is the object of the table). */
static volatile long g_ss_calls;
PLA_MOD_EXPORT pla_action rr_screen_size(pla_ctx* ctx) {
    const long n = _InterlockedIncrement(&g_ss_calls);
    const uint64_t o = api->get_x(ctx, 0);
    if (n <= 6) {
        uint32_t t[4] = {0};
        api->read_mem(ctx, o + 0x3F0, t, sizeof t);
        logf_(PLA_LOG_INFO, "render_res: screen size computed (#%ld) obj=%#llx (table obj %#llx) table %ux%u / %ux%u", n,
              (unsigned long long)o, (unsigned long long)g_obj, t[0], t[2], t[1], t[3]);
    }
    return PLA_CONTINUE;
}

/* main+0x29F0440 / 0x29F067C: return from main+0x1A34908; the RT descriptor is at x29-0xC0. */
PLA_MOD_EXPORT pla_action rr_rt_a(pla_ctx* ctx) {
    const uint64_t d = api->get_x(ctx, 29) - 0xC0;
    patch_pair(ctx, 2, "RT vf15 (0x29F0410)", d, d + 8, 8);
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT pla_action rr_rt_b(pla_ctx* ctx) {
    const uint64_t d = api->get_x(ctx, 29) - 0xC0;
    patch_pair(ctx, 3, "RT vf15 (0x29F0650)", d, d + 8, 8);
    return PLA_CONTINUE;
}

/* main+0x2B9A638: return from main+0x1A34908; descriptor at x19+0x7E8. */
PLA_MOD_EXPORT pla_action rr_rt_c(pla_ctx* ctx) {
    const uint64_t d = api->get_x(ctx, 19) + 0x7E8;
    patch_pair(ctx, 4, "RT (0x2B9A5FC)", d, d + 8, 8);
    return PLA_CONTINUE;
}

/* main+0x2AC011C (entry of the RT creator; x1 = descriptor, +0 width, +8 height, u64; x30 = caller): the
 * half/quarter-resolution effect RTs are fixed 960x540 / 480x270 descriptors, but the passes that draw into them set
 * the viewport from the screen size / 2. With larger RTs left at 960x540, a 2560x1440 screen drew them with a
 * 1280x720 viewport: their contents appeared at 4/3 of their position from the top-left corner, clipped (on the
 * mountain peak, a glassy copy of the player's model below and to the right). They are scaled with the render target
 * size. Matched by caller, not by size: RTs derived from the screen size also measure 960x540 at 3840x2160, and some
 * descriptors are reused (a second creation already sees the scaled size and is left alone). Callers = every
 * 960x540 / 480x270 creation logged by the probe at 2560x1440 (field, mountain peak). */
static const struct { uint32_t ret; uint32_t div; } k_frac_sites[] = {
    {0x29E1AFC, 2}, {0x29F08BC, 2}, {0x29F0944, 2}, {0x29F1954, 2}, {0x29F19D0, 2}, {0x29F1A50, 2}, {0x29F1AC8, 2},
    {0x29F1B40, 2}, {0x29F2F70, 2}, {0x29F3030, 2}, {0x29F3814, 2},
    {0x29F0D98, 4}, {0x29F525C, 4}, {0x2A00B08, 4}, {0x2A00C00, 4},
};
PLA_MOD_EXPORT pla_action rr_rt_frac(pla_ctx* ctx) {
    const uint64_t d = api->get_x(ctx, 1), ret = api->get_x(ctx, 30) - g_main;
    uint64_t wh[2] = {0};
    uint32_t div = 0;
    size_t i;
    long n;
    if (g_rw == DEF_W && g_rh == DEF_H) return PLA_CONTINUE;
    for (i = 0; i < sizeof k_frac_sites / sizeof k_frac_sites[0]; ++i)
        if (k_frac_sites[i].ret == ret) div = k_frac_sites[i].div;
    if (!div || api->read_mem(ctx, d, wh, sizeof wh) != sizeof wh || wh[0] != DEF_W / div || wh[1] != DEF_H / div)
        return PLA_CONTINUE;
    n = _InterlockedIncrement(&g_hits[10]);
    wh[0] = g_rw / div;
    wh[1] = g_rh / div;
    api->write_mem(ctx, d, wh, sizeof wh);
    if (n <= 8) logf_(PLA_LOG_INFO, "render_res: RT 1/%u (from main+%#llx): %ux%u -> %llux%llu (call %ld)", div,
                       (unsigned long long)ret, DEF_W / div, DEF_H / div, (unsigned long long)wh[0],
                       (unsigned long long)wh[1], n);
    return PLA_CONTINUE;
}

/* main+0x2B9EE64 (FUN_2b9e9a0, per frame): the game reads ResolutionWidth/Height into [sp+0x20]/[sp+0x58] (int) and
 * stores (w/1920, h/1080) in the scene constants ([x24+0x18A0]): the fraction of the render targets in use (on Switch
 * the RTs are always 1920x1080 and handheld draws 1280x720 in a corner). Screen-space shaders scale their UVs by it,
 * so with larger RTs the value must be relative to them (g_rw/g_rh), not to 1920: with 2560 wide it gave 1.33 and the
 * low-HP ink frame was drawn at 75 % of the screen. The inputs are rescaled so the game computes w/g_rw. */
static volatile long g_scale_hits;
PLA_MOD_EXPORT pla_action rr_res_scale(pla_ctx* ctx) {
    const uint64_t sp = api->get_sp(ctx);
    int32_t w = 0, h = 0, nw, nh;
    const long n = _InterlockedIncrement(&g_scale_hits);
    if (g_rw == DEF_W && g_rh == DEF_H) return PLA_CONTINUE;
    if (api->read_mem(ctx, sp + 0x20, &w, 4) != 4 || api->read_mem(ctx, sp + 0x58, &h, 4) != 4 || w <= 0 || h <= 0 ||
        w > 16384 || h > 16384)
        return PLA_CONTINUE;
    nw = (int32_t)(((int64_t)w * DEF_W + g_rw / 2) / g_rw);
    nh = (int32_t)(((int64_t)h * DEF_H + g_rh / 2) / g_rh);
    api->write_mem(ctx, sp + 0x20, &nw, 4);
    api->write_mem(ctx, sp + 0x58, &nh, 4);
    if (n <= 2) logf_(PLA_LOG_INFO, "render_res: resolution scale %dx%d of %ux%u -> constants %.3f x %.3f", w, h, g_rw, g_rh,
                      nw / (double)DEF_W, nh / (double)DEF_H);
    return PLA_CONTINUE;
}

/* ---- probe (tests only) ---- */

/* main+0x2AC011C (initializes an RT from a descriptor at x1): width/height and caller. */
PLA_MOD_EXPORT pla_action rr_probe_rt(pla_ctx* ctx) {
    uint64_t d[4] = {0};
    long n;
    if (!g_probe) return PLA_CONTINUE;
    n = _InterlockedIncrement(&g_hits[5]);
    if (n > 120) return PLA_CONTINUE;
    api->read_mem(ctx, api->get_x(ctx, 1), d, sizeof d);
    logf_(PLA_LOG_INFO, "render_res probe RT #%ld: %llux%llu [%#llx %#llx] from main+%#llx", n,
          (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3],
          (unsigned long long)(api->get_x(ctx, 30) - g_main));
    return PLA_CONTINUE;
}

static uint64_t rd64(pla_ctx* ctx, uint64_t va) {
    uint64_t v = 0;
    if (va) api->read_mem(ctx, va, &v, 8);
    return v;
}

/* ---- dynamic resolution ----
 * Controller (class without RTTI; `this` = [[...]+0x3A0]+0x110, the same object as the cheat
 * [[main+0x42ECFC8]+0xC8]):
 *   +0x00 requested mode, +0x28 forced mode (used if < 8), +0x34 level 0..3, +0x38/+0x40 waits,
 *   +0x08..+0x24 GPU time averages ([main+0x42EF568]+0x158, ns) against 33.3 ms.
 *   main+0xCC35A4 Update(this): every frame; only adapts in modes 1..3. Per level it sets the global
 *     parameters "3DBufferWidthScale"/"3DBufferHeightScale": 1.0/1.0; 0.9/0.85; 0.85/0.8; 0.8/0.7.
 *   main+0xCC3888 Apply(this): applies the mode (when it changes). Mode 7 = fixed scale 1.0 x 1.0
 *     (constants at main+0x3978760) and Update no longer adapts.
 * dynamic_res=false: forced mode (+0x28) = 7, applied once (Update is redirected to Apply). */
#define DYN_FULL_MODE 7u

static void dump_dyn(pla_ctx* ctx, uint64_t self, long n, const char* who) {
    uint32_t v[18] = {0};
    uint64_t g = rd64(ctx, g_main + 0x42EF568), gpu = g ? rd64(ctx, g + 0x158) : 0;
    float s3c;
    api->read_mem(ctx, self, v, sizeof v);
    memcpy(&s3c, &v[15], 4);
    logf_(PLA_LOG_INFO,
          "render_res probe %s #%ld this=%#llx mode=%u forced=%u level=%u wait=%u cooldown=%u frames=%u good=%u "
          "gpu=%.2f ms ratio=%.3f",
          who, n, (unsigned long long)self, v[0], v[10], v[13], v[14], v[16], v[8], v[9], (double)gpu / 1e6,
          (double)s3c);
}

/* ---- live settings (nvnvk in-game menu; docs/port/LIVE-SETTINGS.md) ----
 * nvnvk.dll exports nvnvk_live_version() and nvnvk_live_get("render_res.width", ...). Polled once per frame from the
 * dynamic resolution Update hook (the game's main loop); without nvnvk nothing changes. */
typedef uint32_t (*live_version_fn)(void);
typedef int (*live_get_fn)(const char* key, char* out, int cap);
typedef void (*live_report_fn)(const char* key, const char* value);
static live_version_fn g_live_version;
static live_get_fn g_live_get;
static live_report_fn g_live_report;
static volatile LONG g_report_pending = 1; /* tell nvnvk the game's screen size (part of the presented texture) */
static uint32_t g_live_seen;
static long g_live_probe;
static volatile LONG g_pending_res;  /* 1 = write the table and recompute the screen size */
static uint32_t g_pending_w, g_pending_h;
static volatile LONG g_dyn_want = -1; /* -1 = nothing to do; 0/1 = dynamic resolution changed live */

static void live_poll(void) {
    char v[32];
    uint32_t ver;
    if (!g_live_version) {
        HMODULE m = NULL;
        if (g_live_probe++ % 120 != 0) return;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvnvk.dll", &m) || !m) return;
        g_live_get = (live_get_fn)(void*)GetProcAddress(m, "nvnvk_live_get");
        g_live_version = g_live_get ? (live_version_fn)(void*)GetProcAddress(m, "nvnvk_live_version") : NULL;
        g_live_report = (live_report_fn)(void*)GetProcAddress(m, "nvnvk_live_report");
        if (!g_live_version) return;
        logf_(PLA_LOG_INFO, "render_res: live settings channel of nvnvk found");
    }
    if (g_report_pending && g_live_report && g_obj) {
        char s[32];
        g_report_pending = 0;
        snprintf(s, sizeof s, "%ux%u", g_cur_w, g_cur_h);
        g_live_report("render_res.screen", s);
    }
    ver = g_live_version();
    if (ver == g_live_seen) return;
    g_live_seen = ver;
    if (g_live_get("render_res.dynamic_res", v, sizeof v) > 0 && is_true(v) != g_dyn) {
        g_dyn = is_true(v);
        g_dyn_want = g_dyn;
        logf_(PLA_LOG_INFO, "render_res: live: dynamic resolution %s", g_dyn ? "on" : "off");
    }
    if (g_live_get("render_res.width", v, sizeof v) > 0) {
        long w = atol(v);
        if (w < 640) w = 640;
        w &= ~15L;
        if ((uint32_t)w > g_rw) {
            logf_(PLA_LOG_WARNING, "render_res: live: %ld wide is above the reserved %u: applies after restarting", w, g_rw);
        } else if ((uint32_t)w != g_w) {
            g_w = (uint32_t)w;
            g_h = g_w * 9 / 16;
            g_pending_w = g_w;
            g_pending_h = g_h;
            g_pending_res = 1;
            logf_(PLA_LOG_INFO, "render_res: live: internal resolution -> %ux%u", g_w, g_h);
        }
    }
}

/* Live resolution: rewrite the docked entry of the mode table and run the game's own screen size computation
 * (main+0x29EABA8, the same one it runs when the console mode changes). Returns 1 if it redirected. */
static int live_apply_res(pla_ctx* ctx) {
    uint32_t t[4] = {0};
    const uint64_t o = g_obj;
    if (!g_pending_res) return 0;
    g_pending_res = 0;
    if (!o || api->read_mem(ctx, o + 0x3F0, t, sizeof t) != sizeof t) {
        logf_(PLA_LOG_WARNING, "render_res: live: mode table not found; applies after restarting");
        return 0;
    }
    /* t = {handheld w, docked w, handheld h, docked h}: the docked entry must still be ours. */
    if (t[1] != g_cur_w || t[3] != g_cur_h) {
        logf_(PLA_LOG_WARNING, "render_res: live: mode table holds %ux%u (expected %ux%u); not touched", t[1], t[3],
              g_cur_w, g_cur_h);
        return 0;
    }
    t[1] = g_pending_w;
    t[3] = g_pending_h;
    api->write_mem(ctx, o + 0x3F4, &t[1], 4);
    api->write_mem(ctx, o + 0x3FC, &t[3], 4);
    g_cur_w = t[1];
    g_cur_h = t[3];
    g_report_pending = 1;
    api->set_x(ctx, 0, o);
    api->set_pc(ctx, g_main + 0x29EABA8); /* returns to Update's caller (X30 intact); Update skips one frame */
    return 1;
}

static void log_screen_size(pla_ctx* ctx) {
    static volatile long n;
    const uint64_t g = rd64(ctx, g_main + 0x42EF568);
    uint32_t wh[2] = {0};
    if (!g || api->read_mem(ctx, g + 0x128, wh, sizeof wh) != sizeof wh) return;
    if (_InterlockedIncrement(&n) <= 40) logf_(PLA_LOG_INFO, "render_res: screen size now %ux%u", wh[0], wh[1]);
}

/* main+0xCC35A4 (pre): per-frame Update. */
PLA_MOD_EXPORT pla_action rr_dyn_update(pla_ctx* ctx) {
    static volatile LONG check_size;
    const uint64_t self = api->get_x(ctx, 0);
    const long n = _InterlockedIncrement(&g_hits[6]);
    uint32_t forced = 0;
    if (g_probe && (n <= 3 || n % 600 == 0)) dump_dyn(ctx, self, n, "Update");
    if (check_size) {
        check_size = 0;
        log_screen_size(ctx);
    }
    live_poll();
    if (live_apply_res(ctx)) {
        check_size = 1;
        return PLA_REDIRECT;
    }
    if (!self) return PLA_CONTINUE;
    if (g_dyn) {
        /* Turned back on live: let the game choose the mode again (+0x28 = 8) and apply it. */
        if (g_dyn_want == 1) {
            g_dyn_want = -1;
            api->read_mem(ctx, self + 0x28, &forced, 4);
            if (forced != DYN_FULL_MODE) return PLA_CONTINUE;
            forced = 8;
            api->write_mem(ctx, self + 0x28, &forced, 4);
            logf_(PLA_LOG_INFO, "render_res: dynamic resolution back on (forced mode 8) at frame %ld", n);
            api->set_pc(ctx, g_main + 0xCC3888);
            return PLA_REDIRECT;
        }
        return PLA_CONTINUE;
    }
    g_dyn_want = -1;
    api->read_mem(ctx, self + 0x28, &forced, 4);
    if (forced == DYN_FULL_MODE) return PLA_CONTINUE; /* Update returns right away in mode 7 */
    forced = DYN_FULL_MODE;
    api->write_mem(ctx, self + 0x28, &forced, 4);
    if (_InterlockedIncrement(&g_hits[8]) <= 4)
        logf_(PLA_LOG_INFO, "render_res: dynamic resolution disabled (forced mode 7, scale 1.0) at frame %ld", n);
    api->set_pc(ctx, g_main + 0xCC3888); /* Apply(this) and return to the caller of Update (X30 intact) */
    return PLA_REDIRECT;
}

/* main+0xCC3888 (pre): Apply on mode change; with dynamic_res=false mode 7 is forced. */
PLA_MOD_EXPORT pla_action rr_dyn_apply(pla_ctx* ctx) {
    const uint64_t self = api->get_x(ctx, 0);
    const long n = _InterlockedIncrement(&g_hits[7]);
    if (g_probe && n <= 20) dump_dyn(ctx, self, n, "Apply");
    if (!g_dyn && self) {
        const uint32_t forced = DYN_FULL_MODE;
        api->write_mem(ctx, self + 0x28, &forced, 4);
    }
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    if (!api) return;
    logf_(PLA_LOG_INFO, "render_res: sites framework=%ld table=%ld rtA=%ld rtB=%ld rtC=%ld frac=%ld; dyn. Update=%ld Apply=%ld",
          g_hits[0], g_hits[1], g_hits[2], g_hits[3], g_hits[4], g_hits[10], g_hits[6], g_hits[7]);
}
