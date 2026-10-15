/* engine_options: "engine" graphics options of PLA 1.1.1 for the launcher (without re-exporting main).
 *
 * Everything is applied through data, never by writing guest code:
 *
 *  1) RomFS file data, fixed up in memory right after it is read (before the game parses it). Hooks on the
 *     PLT stubs of main for nn::fs (OpenFile, ReadFile x2, CloseFile):
 *       - bin/field/camera/camera_table.bin (FlatBuffers, 22 exploration cameras): field 6 of each camera is
 *         the vertical field of view in degrees (50 in all of them). Option "fov" (multiplier).
 *       - bin/misc/app_config/<x>.bin (FlatBuffers "ConfigArchive": name + parameters as TEXT). The text is
 *         rewritten in place (it fits in the alignment padding; if it does not fit, a warning is logged and it
 *         is left alone). Options "lod", "anim_full_rate", the CPU options "cpu_preset", "npc_density" and
 *         "anim_lod", and the advanced "app_config".
 *  2) Post-processing parameters: post hook of vtable_7104182368::vf6 (main+0x29BD304), which copies the
 *     screen parameter group ("Global": DofEnable, BloomIntensity, Aberration...) into the render constant
 *     block (this+0x58). After the copy the fields are overwritten: "dof", "bloom", "chromatic_aberration".
 *  3) Texture LOD bias: pre hook at main+0x2A9B670 (return from nvnSamplerBuilderSetLodClamp, right before the
 *     sampler description bias at [x19+0x1C] is read). Option "texture_lod_bias" (added).
 *     It is the site of the "LOD" patch by Fl4sh_ (fmov s0,#-15).
 *  4) HUD: pre hook of nn::ui2d::Layout::Draw (main+0x1353E0, Layout vf12). The "hud_toggle_key" key toggles
 *     it: hidden = no ui2d layer is drawn (HUD, menus, texts).
 *
 * Defaults = original game. See docs/port/ENGINE-OPTIONS.md. */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pla_mod_api.h"
#pragma comment(lib, "user32.lib")

static const pla_api* api;
static int g_probe;

/* Log: the suyu log drops mod lines after startup (filter/rate), so every line also goes to
 * engine_options.log next to the DLL (rewritten every session). */
static FILE* g_logf;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static ULONGLONG g_t0;

static void open_log_file(void) {
    HMODULE self = NULL;
    char path[MAX_PATH];
    char* slash;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&open_log_file, &self) ||
        !GetModuleFileNameA(self, path, sizeof path))
        return;
    slash = strrchr(path, '\\');
    if (!slash) return;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "engine_options.log");
    g_logf = fopen(path, "w");
}

static void logf_(int lvl, const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    api->log(lvl, line);
    if (g_logf) {
        AcquireSRWLockExclusive(&g_log_lock);
        fprintf(g_logf, "[%8.3f] %s\n", (GetTickCount64() - g_t0) / 1000.0, line);
        fflush(g_logf);
        ReleaseSRWLockExclusive(&g_log_lock);
    }
}

/* ---- config (flat object of strings) -------------------------------------------------------------- */

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
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\r' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

static int is_true(const char* s) {
    return s[0] == 't' || s[0] == 'T' || s[0] == '1' || s[0] == 'y' || s[0] == 'Y' || s[0] == 's' || s[0] == 'S';
}

static float cfg_float(const char* json, const char* key, float def, float lo, float hi) {
    char v[64];
    float f;
    char* end;
    if (!cfg_get(json, key, v, sizeof v) || !v[0]) return def;
    f = strtof(v, &end);
    if (end == v || !isfinite(f)) {
        logf_(PLA_LOG_WARNING, "engine_options: %s=\"%s\" is not a number; using %g", key, v, def);
        return def;
    }
    if (f < lo || f > hi) {
        float c = f < lo ? lo : hi;
        logf_(PLA_LOG_WARNING, "engine_options: %s=%g out of [%g, %g]; using %g", key, f, lo, hi, c);
        f = c;
    }
    return f;
}

static int cfg_bool(const char* json, const char* key, int def) {
    char v[16];
    if (!cfg_get(json, key, v, sizeof v) || !v[0]) return def;
    return is_true(v);
}

/* ---- options -------------------------------------------------------------------------------------- */

static float g_fov = 1.0f;       /* field of view multiplier of the exploration cameras */
static float g_lod = 1.0f;       /* draw/load distance multiplier */
static float g_model_lod = 1.0f; /* model LOD switch distance multiplier (advanced "model_lod"; default = lod) */
static float g_tex_bias = 0.0f;  /* added to the LOD bias of every sampler */
static int g_dof = 1, g_bloom = 1, g_aberr = 1;
static int g_anim_full = 0;
static float g_npc_density = 1.0f; /* multiplier of the NPC load radii (CPU option) */
static int g_anim_lod = 0;         /* 0 default, 1 reduced, 2 low (animation rate levels; CPU option) */
static int g_shadow_cascades = 0;  /* 0 = as in the game; else number of shadow cascades (render init) */
static int g_shadow_res = 0;       /* 0 = as in the game; else shadow map resolution (render init) */

/* CPU class for "auto": physical cores the process may run on (its affinity mask, so a restricted run is
 * classified like the machine it imitates). < 8 cores = "performance" tier (e.g. 6-core laptops);
 * 8 or more = full quality. PLA_CPU_CLASS=low|high overrides it. Returns 1 for the performance tier. */
static int cpu_is_low(int* cores_out, int* threads_out) {
    char ev[16];
    DWORD_PTR pm = 0, sm = 0;
    DWORD len = 0;
    int cores = 0, threads = 0;
    unsigned char* buf;
    if (GetEnvironmentVariableA("PLA_CPU_CLASS", ev, sizeof ev) && ev[0]) {
        *cores_out = *threads_out = -1;
        return !_stricmp(ev, "low") || !_stricmp(ev, "performance");
    }
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    buf = len ? (unsigned char*)malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        DWORD off = 0;
        while (off < len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            KAFFINITY m = e->Processor.GroupMask[0].Mask;
            if (e->Processor.GroupMask[0].Group != 0) m = 0; /* group 0 only (the affinity mask covers group 0) */
            if (pm) m &= (KAFFINITY)pm;
            if (m) {
                ++cores;
                threads += (int)__popcnt64((unsigned __int64)m);
            }
            off += e->Size;
        }
    }
    free(buf);
    if (!cores) { /* fallback: logical CPUs in the mask */
        threads = (int)__popcnt64((unsigned __int64)pm);
        cores = threads;
    }
    *cores_out = cores;
    *threads_out = threads;
    return cores < 8;
}
static int g_hud_vk = 0;
static volatile LONG g_hud_hidden = 0;

/* app_config substitutions: file (without .bin), key, parameter index and new value (text),
 * or a multiplier over the original value (mul != 0). */
typedef struct {
    char file[48];
    char key[64];
    int idx;
    float mul;       /* != 0: value = original * mul */
    char val[24];    /* if mul == 0 */
    int raise_max;   /* raises DebugMax if the new value exceeds it (in case the game clamps) */
} cfg_patch;

#define MAX_PATCHES 64
static cfg_patch g_patches[MAX_PATCHES];
static int g_npatches;

static void add_patch(const char* file, const char* key, int idx, float mul, const char* val) {
    cfg_patch* p;
    if (g_npatches >= MAX_PATCHES) return;
    p = &g_patches[g_npatches++];
    memset(p, 0, sizeof *p);
    snprintf(p->file, sizeof p->file, "%s", file);
    snprintf(p->key, sizeof p->key, "%s", key);
    p->idx = idx;
    p->mul = mul;
    if (val) snprintf(p->val, sizeof p->val, "%s", val);
    p->raise_max = 1;
}

/* "app_config": "file/key/index=value;file/key/index=*1.5;..." (advanced, for testing). */
static void parse_raw_patches(const char* s) {
    char item[160];
    while (s && *s) {
        const char* e = strchr(s, ';');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char *a, *b, *c, *eq;
        if (n >= sizeof item) n = sizeof item - 1;
        memcpy(item, s, n);
        item[n] = 0;
        s = e ? e + 1 : NULL;
        a = item;
        b = strchr(a, '/');
        if (!b) continue;
        *b++ = 0;
        c = strchr(b, '/');
        eq = strchr(c ? c : b, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (c) *c++ = 0;
        if (*eq == '*')
            add_patch(a, b, c ? atoi(c) : 0, strtof(eq + 1, NULL), NULL);
        else
            add_patch(a, b, c ? atoi(c) : 0, 0.0f, eq);
    }
}

/* ---- keys ----------------------------------------------------------------------------------------- */

static int parse_vk(const char* s) {
    static const struct { const char* n; int vk; } named[] = {
        {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"RETURN", VK_RETURN}, {"ENTER", VK_RETURN}, {"ESCAPE", VK_ESCAPE},
        {"ESC", VK_ESCAPE}, {"BACK", VK_BACK}, {"BACKSPACE", VK_BACK}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"PAUSE", VK_PAUSE},
        {"SCROLL", VK_SCROLL}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD}, {"MINUS", VK_OEM_MINUS}, {"PLUS", VK_OEM_PLUS},
    };
    size_t i;
    if (!s || !s[0]) return 0;
    if (!_strnicmp(s, "VK_0x", 5)) return (int)strtol(s + 5, NULL, 16);
    if (!_strnicmp(s, "0x", 2)) return (int)strtol(s + 2, NULL, 16);
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (!_strnicmp(s, "NUMPAD", 6) && s[6] >= '0' && s[6] <= '9' && !s[7]) return VK_NUMPAD0 + (s[6] - '0');
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 'a' + 'A';
    for (i = 0; i < sizeof named / sizeof named[0]; ++i)
        if (!_stricmp(s, named[i].n)) return named[i].vk;
    return -1;
}

/* Called from the HUD hook (many times per frame): polls the key at most every 15 ms. */
static void poll_hud_key(void) {
    static volatile LONG64 last_ms;
    static int was_down;
    ULONGLONG now;
    LONG64 prev;
    HWND fg;
    DWORD pid = 0;
    int down;
    if (!g_hud_vk) return;
    now = GetTickCount64();
    prev = last_ms;
    if ((LONG64)now - prev < 15) return;
    if (_InterlockedCompareExchange64(&last_ms, (LONG64)now, prev) != prev) return; /* another thread is polling */
    fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    down = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_hud_vk) & 0x8000) != 0;
    if (down && !was_down) {
        LONG h = !g_hud_hidden;
        _InterlockedExchange(&g_hud_hidden, h);
        logf_(PLA_LOG_WARNING, "engine_options: HUD %s", h ? "hidden" : "visible");
    }
    was_down = down;
}

/* ---- FlatBuffers over the guest buffer (host pointer, bounds-checked) ----------------------------- */

typedef struct {
    uint8_t* b;
    size_t n;
} fbuf;

static int fb_u32(const fbuf* f, size_t o, uint32_t* v) {
    if (o + 4 > f->n) return 0;
    memcpy(v, f->b + o, 4);
    return 1;
}

/* Absolute position of field `idx` of the table at `t`, or 0 if absent. */
static size_t fb_field(const fbuf* f, size_t t, unsigned idx) {
    uint32_t so;
    int32_t s;
    size_t vt;
    uint16_t vlen, off;
    if (!fb_u32(f, t, &so)) return 0;
    s = (int32_t)so;
    if ((int64_t)t - s < 0 || (size_t)((int64_t)t - s) + 4 > f->n) return 0;
    vt = (size_t)((int64_t)t - s);
    memcpy(&vlen, f->b + vt, 2);
    if (4 + 2 * (size_t)idx + 2 > vlen || vt + vlen > f->n) return 0;
    memcpy(&off, f->b + vt + 4 + 2 * idx, 2);
    return off ? t + off : 0;
}

/* Follows a uoffset (reference field or vector element). */
static size_t fb_deref(const fbuf* f, size_t p) {
    uint32_t v;
    if (!p || !fb_u32(f, p, &v) || p + v >= f->n) return 0;
    return p + v;
}

/* Root table (uoffset at buffer position 0). */
static size_t fb_root(const fbuf* f) {
    uint32_t v;
    if (!fb_u32(f, 0, &v) || (size_t)v + 4 > f->n) return 0;
    return v;
}

/* Vector at `v`: returns the element count (0 if invalid). */
static uint32_t fb_vec(const fbuf* f, size_t v) {
    uint32_t n;
    if (!v || !fb_u32(f, v, &n) || v + 4 + (size_t)n * 4 > f->n) return 0;
    return n;
}

/* String at `s`: length and text. */
static int fb_str(const fbuf* f, size_t s, const char** txt, uint32_t* len) {
    uint32_t n;
    if (!s || !fb_u32(f, s, &n) || s + 4 + (size_t)n + 1 > f->n) return 0;
    *txt = (const char*)f->b + s + 4;
    *len = n;
    return 1;
}

/* Rewrites the string at `s` in place if it fits in its footprint (text + NUL + 4-byte alignment padding,
 * which must be zero). */
static int fb_str_write(fbuf* f, size_t s, const char* nv) {
    uint32_t n, cap, i;
    size_t end, nl = strlen(nv);
    if (!s || !fb_u32(f, s, &n)) return 0;
    end = (s + 4 + n + 1 + 3) & ~(size_t)3;
    if (end > f->n) end = f->n;
    for (i = (uint32_t)(s + 4 + n); i < end; ++i)
        if (f->b[i]) return 0; /* not padding: left alone */
    cap = (uint32_t)(end - (s + 4) - 1);
    if (nl > cap) return 0;
    memset(f->b + s + 4, 0, end - (s + 4));
    memcpy(f->b + s + 4, nv, nl);
    n = (uint32_t)nl;
    memcpy(f->b + s, &n, 4);
    return 1;
}

/* Shortest (most precise) text of x that fits in `cap` characters. */
static int fmt_fit(float x, char* out, size_t outn, size_t cap) {
    int prec;
    for (prec = 6; prec >= 1; --prec) {
        snprintf(out, outn, "%.*g", prec, x);
        if (strlen(out) <= cap) return 1;
    }
    return 0;
}

static size_t str_cap(const fbuf* f, size_t s) {
    uint32_t n;
    size_t end, i;
    if (!fb_u32(f, s, &n)) return 0;
    end = (s + 4 + n + 1 + 3) & ~(size_t)3;
    if (end > f->n) return 0;
    for (i = s + 4 + n; i < end; ++i)
        if (f->b[i]) return n;
    return end - (s + 4) - 1;
}

/* ---- per-file patches ----------------------------------------------------------------------------- */

static void patch_camera_table(fbuf* f) {
    size_t root = fb_root(f), vec;
    uint32_t n, i, changed = 0;
    float first_old = 0, first_new = 0;
    if (!root) return;
    vec = fb_deref(f, fb_field(f, root, 0));
    n = fb_vec(f, vec);
    for (i = 0; i < n; ++i) {
        size_t t = fb_deref(f, vec + 4 + 4 * (size_t)i);
        size_t p = t ? fb_field(f, t, 6) : 0;
        float v, nv;
        if (!p || p + 4 > f->n) continue;
        memcpy(&v, f->b + p, 4);
        if (!(v > 5.0f && v < 150.0f)) continue; /* does not look like an angle: left alone */
        nv = v * g_fov;
        if (nv < 10.0f) nv = 10.0f;
        if (nv > 150.0f) nv = 150.0f;
        if (!changed) first_old = v, first_new = nv;
        memcpy(f->b + p, &nv, 4);
        ++changed;
    }
    logf_(PLA_LOG_WARNING, "engine_options: camera_table: %u/%u cameras, fov %.1f -> %.1f degrees", changed, n,
          first_old, first_new);
}

static void patch_app_config(fbuf* f, const char* file) {
    size_t root = fb_root(f), vec;
    uint32_t n, i;
    int k;
    if (!root) return;
    vec = fb_deref(f, fb_field(f, root, 0));
    n = fb_vec(f, vec);
    if (!n) {
        logf_(PLA_LOG_WARNING, "engine_options: %s: does not look like a ConfigArchive", file);
        return;
    }
    for (k = 0; k < g_npatches; ++k) {
        cfg_patch* pp = &g_patches[k];
        int done = 0;
        if (_stricmp(pp->file, file)) continue;
        for (i = 0; i < n && !done; ++i) {
            size_t t = fb_deref(f, vec + 4 + 4 * (size_t)i);
            const char* name;
            uint32_t nlen, pn;
            size_t params, s;
            const char* old;
            uint32_t olen;
            char oldv[32], nv[32];
            float newf;
            if (!t || !fb_str(f, fb_deref(f, fb_field(f, t, 0)), &name, &nlen)) continue;
            if (nlen != strlen(pp->key) || memcmp(name, pp->key, nlen)) continue;
            done = 1;
            params = fb_deref(f, fb_field(f, t, 5));
            pn = fb_vec(f, params);
            if ((uint32_t)pp->idx >= pn) {
                logf_(PLA_LOG_WARNING, "engine_options: %s/%s: no parameter %d", file, pp->key, pp->idx);
                break;
            }
            s = fb_deref(f, params + 4 + 4 * (size_t)pp->idx);
            if (!fb_str(f, s, &old, &olen) || olen >= sizeof oldv) break;
            memcpy(oldv, old, olen);
            oldv[olen] = 0;
            if (pp->mul != 0.0f) {
                if (!oldv[0]) break;
                newf = strtof(oldv, NULL) * pp->mul;
                if (!fmt_fit(newf, nv, sizeof nv, str_cap(f, s))) {
                    logf_(PLA_LOG_WARNING, "engine_options: %s/%s[%d]: %g does not fit in the original text \"%s\"", file,
                          pp->key, pp->idx, newf, oldv);
                    break;
                }
            } else {
                snprintf(nv, sizeof nv, "%s", pp->val);
                newf = strtof(nv, NULL);
            }
            if (!fb_str_write(f, s, nv)) {
                logf_(PLA_LOG_WARNING, "engine_options: %s/%s[%d]: \"%s\" does not fit in the place of \"%s\"", file,
                      pp->key, pp->idx, nv, oldv);
                break;
            }
            /* DebugMax (field 4): if the new value exceeds it, raise it (in case the game clamps to the range). */
            if (pp->raise_max) {
                size_t sm = fb_deref(f, fb_field(f, t, 4));
                const char* mx;
                uint32_t mlen;
                char mxv[32], nmx[32];
                if (fb_str(f, sm, &mx, &mlen) && mlen && mlen < sizeof mxv) {
                    memcpy(mxv, mx, mlen);
                    mxv[mlen] = 0;
                    if (newf > strtof(mxv, NULL) && fmt_fit(newf, nmx, sizeof nmx, str_cap(f, sm)))
                        fb_str_write(f, sm, nmx);
                }
            }
            logf_(PLA_LOG_WARNING, "engine_options: %s/%s[%d]: \"%s\" -> \"%s\"", file, pp->key, pp->idx, oldv, nv);
        }
        if (!done) logf_(PLA_LOG_WARNING, "engine_options: %s: key %s not found", file, pp->key);
    }
}

/* ---- file tracking (nn::fs handles) --------------------------------------------------------------- */

enum { T_NONE = 0, T_CAMERA = 1, T_APPCFG_BASE = 16 };

#define MAX_TRACK 32
static struct {
    uint64_t handle;
    int target;
} g_track[MAX_TRACK];
static SRWLOCK g_track_lock = SRWLOCK_INIT;
static char g_appcfg_names[MAX_PATCHES][48];
static int g_nappcfg;
static int g_need_camera;
static volatile long g_open_logged;

static int target_for_path(const char* path) {
    const char* p;
    int i;
    if (g_need_camera && strstr(path, "field/camera/camera_table.bin")) return T_CAMERA;
    p = strstr(path, "misc/app_config/");
    if (p && g_nappcfg) {
        char name[64];
        size_t n;
        p += 16;
        n = strcspn(p, ".");
        if (n >= sizeof name) return T_NONE;
        memcpy(name, p, n);
        name[n] = 0;
        for (i = 0; i < g_nappcfg; ++i)
            if (!_stricmp(name, g_appcfg_names[i])) return T_APPCFG_BASE + i;
    }
    return T_NONE;
}

static void track_add(uint64_t h, int target) {
    int i, free_slot = -1;
    AcquireSRWLockExclusive(&g_track_lock);
    for (i = 0; i < MAX_TRACK; ++i) {
        if (g_track[i].handle == h) { free_slot = i; break; }
        if (!g_track[i].handle && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_track[free_slot].handle = h;
        g_track[free_slot].target = target;
    }
    ReleaseSRWLockExclusive(&g_track_lock);
}

static int track_find(uint64_t h) {
    int i, t = T_NONE;
    if (!h) return T_NONE;
    AcquireSRWLockShared(&g_track_lock);
    for (i = 0; i < MAX_TRACK; ++i)
        if (g_track[i].handle == h) { t = g_track[i].target; break; }
    ReleaseSRWLockShared(&g_track_lock);
    return t;
}

static void track_del(uint64_t h) {
    int i;
    AcquireSRWLockExclusive(&g_track_lock);
    for (i = 0; i < MAX_TRACK; ++i)
        if (g_track[i].handle == h) g_track[i].handle = 0;
    ReleaseSRWLockExclusive(&g_track_lock);
}

/* Model LOD (FUN_7101acd904, "screen size" mode 2): the projected size of the model is compared against
 * 3 thresholds per LOD type at main+0x397CAF0 (float[3][3], .rodata: {0.01,0.003,0.001}, {0.01,0.005,0.002},
 * {1.8,0.5,0.2}); first threshold below the size = LOD level. The size goes as 1/distance^2, so dividing the
 * thresholds by lod^2 moves every LOD switch lod times farther. Written once (data, not code; the AOT code reads
 * the table through a computed address). */
#define MODEL_LOD_TABLE 0x397CAF0ull
static volatile long g_model_lod_done;
static void apply_model_lod(pla_ctx* ctx) {
    static const float orig[9] = {0.01f, 0.003f, 0.001f, 0.01f, 0.005f, 0.002f, 1.8f, 0.5f, 0.2f};
    float cur[9], nv[9];
    const uint64_t va = api->module_base("main") + MODEL_LOD_TABLE;
    const float k = 1.0f / (g_model_lod * g_model_lod);
    int i;
    if (_InterlockedExchange(&g_model_lod_done, 1)) return;
    if (api->read_mem(ctx, va, cur, sizeof cur) != sizeof cur || memcmp(cur, orig, sizeof cur)) {
        logf_(PLA_LOG_WARNING, "engine_options: model LOD table at main+%#llx not found, left alone", MODEL_LOD_TABLE);
        return;
    }
    for (i = 0; i < 9; ++i) nv[i] = orig[i] * k;
    if (api->write_mem(ctx, va, nv, sizeof nv) != sizeof nv ||
        api->read_mem(ctx, va, cur, sizeof cur) != sizeof cur || memcmp(cur, nv, sizeof cur)) {
        logf_(PLA_LOG_WARNING, "engine_options: model LOD table could not be written");
        return;
    }
    logf_(PLA_LOG_WARNING, "engine_options: model LOD switch distances x%.3g (thresholds / %.3g)", g_model_lod, 1.0f / k);
}

/* Model LOD, "distance" mode 1 of FUN_7101acd904 (most field models: structures, rocks, props; ~140 per frame
 * measured): LOD i while the camera distance is under 100*(i+1) m (hard-coded immediate). Pre hook at the loop head
 * main+0x1ACDC10: on the first pass (x8 == 0; the back edge enters with x8 = i+1) s0 holds the distance, which is
 * divided by model_lod, so every switch happens model_lod times farther. */
PLA_MOD_EXPORT pla_action eo_lod_dist_pre(pla_ctx* ctx) {
    uint64_t v[2];
    float d;
    if (g_model_lod == 1.0f || api->get_x(ctx, 8) != 0) return PLA_CONTINUE;
    api->get_v(ctx, 0, v);
    memcpy(&d, &v[0], 4);
    d /= g_model_lod;
    memcpy(&v[0], &d, 4);
    api->set_v(ctx, 0, v[0], v[1]);
    return PLA_CONTINUE;
}

/* nn::fs::OpenFile(FileHandle* out, const char* path, int mode) [PLT main+0x32A38B0] */
PLA_MOD_EXPORT pla_action eo_open_pre(pla_ctx* ctx) {
    uint64_t* cd = api->call_data(ctx);
    char path[260];
    size_t got;
    int t;
    cd[0] = 0;
    cd[1] = 0;
    if (!g_model_lod_done && fabsf(g_model_lod - 1.0f) > 1e-4f) apply_model_lod(ctx);
    if (!g_need_camera && !g_nappcfg && !g_probe) return PLA_CONTINUE;
    got = api->read_mem(ctx, api->get_x(ctx, 1), path, sizeof path - 1);
    path[got] = 0;
    path[sizeof path - 1] = 0;
    t = target_for_path(path);
    if (g_probe && (g_probe > 1 || t || strstr(path, "camera") || strstr(path, "app_config")) &&
        _InterlockedIncrement(&g_open_logged) <= 3000)
        logf_(PLA_LOG_WARNING, "engine_options: OpenFile %s (target %d)", path, t);
    if (t) {
        cd[0] = api->get_x(ctx, 0);
        cd[1] = (uint64_t)t;
    }
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT void eo_open_post(pla_ctx* ctx) {
    uint64_t* cd = api->call_data(ctx);
    uint64_t h = 0;
    if (!cd[1] || (uint32_t)api->get_x(ctx, 0) != 0) return;
    if (api->read_mem(ctx, cd[0], &h, 8) == 8 && h) track_add(h, (int)cd[1]);
}

/* nn::fs::CloseFile(FileHandle) [PLT main+0x32A38E0] */
PLA_MOD_EXPORT pla_action eo_close_pre(pla_ctx* ctx) {
    uint64_t h = api->get_x(ctx, 0);
    if (track_find(h)) track_del(h);
    return PLA_CONTINUE;
}

static void read_pre_common(pla_ctx* ctx, uint64_t h, uint64_t off, uint64_t buf, uint64_t size, uint64_t outp) {
    uint64_t* cd = api->call_data(ctx);
    int t = track_find(h);
    cd[0] = 0;
    if (!t) return;
    cd[0] = (uint64_t)t | (off == 0 ? 0 : 0x80000000ull);
    cd[1] = buf;
    cd[2] = size;
    cd[3] = outp;
}

static void read_post_common(pla_ctx* ctx) {
    uint64_t* cd = api->call_data(ctx);
    int t = (int)(cd[0] & 0xFFFF);
    uint64_t size = cd[2];
    fbuf f;
    if (!t || (uint32_t)api->get_x(ctx, 0) != 0) return;
    if (cd[0] & 0x80000000ull) {
        logf_(PLA_LOG_WARNING, "engine_options: partial read (offset != 0) of target %d: not patched", t);
        return;
    }
    if (cd[3]) {
        uint64_t got = 0;
        if (api->read_mem(ctx, cd[3], &got, 8) == 8 && got && got < size) size = got;
    }
    if (size < 8 || size > (16u << 20)) return;
    f.b = (uint8_t*)api->host_pointer(cd[1], (size_t)size);
    f.n = (size_t)size;
    if (!f.b) {
        logf_(PLA_LOG_WARNING, "engine_options: no host pointer for the read buffer");
        return;
    }
    if (t == T_CAMERA)
        patch_camera_table(&f);
    else if (t >= T_APPCFG_BASE && t - T_APPCFG_BASE < g_nappcfg)
        patch_app_config(&f, g_appcfg_names[t - T_APPCFG_BASE]);
}

/* nn::fs::ReadFile(size_t* out, FileHandle, int64 off, void* buf, size_t size) [PLT main+0x32A3900] */
PLA_MOD_EXPORT pla_action eo_read5_pre(pla_ctx* ctx) {
    read_pre_common(ctx, api->get_x(ctx, 1), api->get_x(ctx, 2), api->get_x(ctx, 3), api->get_x(ctx, 4),
                    api->get_x(ctx, 0));
    return PLA_CONTINUE;
}
PLA_MOD_EXPORT void eo_read5_post(pla_ctx* ctx) { read_post_common(ctx); }

/* nn::fs::ReadFile(FileHandle, int64 off, void* buf, size_t size) [PLT main+0x32A4DA0] */
PLA_MOD_EXPORT pla_action eo_read4_pre(pla_ctx* ctx) {
    read_pre_common(ctx, api->get_x(ctx, 0), api->get_x(ctx, 1), api->get_x(ctx, 2), api->get_x(ctx, 3), 0);
    return PLA_CONTINUE;
}
PLA_MOD_EXPORT void eo_read4_post(pla_ctx* ctx) { read_post_common(ctx); }

/* ---- A/B test in a single session ---------------------------------------------------------------- */

/* "ab_secs": N > 0: for the first N seconds post-processing and the HUD stay as in the original game and
 * then the options are applied (before/after captures with the same scene, time of day and weather). */
static ULONGLONG g_ab_ms;
static int ab_before(void) { return g_ab_ms && GetTickCount64() - g_t0 < g_ab_ms; }

/* ---- live settings (nvnvk in-game menu; docs/port/LIVE-SETTINGS.md) ------------------------------- */

/* ---- live FOV: render camera projection (main+0x2994D60) ----
 * FUN_7102994d60(cam) builds the projection matrix of an engine camera: type [cam+0x230] (0 = perspective), near +0x234,
 * far +0x238, vertical FOV in radians +0x23c, aspect +0x240 -> matrix at +0x2b0. The exploration cameras copy their
 * angle from camera_table when they are created (so rewriting the table later does not reach them); a live change is
 * applied here instead: perspective cameras at the exploration angle (the boot angle of camera_table) get the new
 * angle; narrower angles (aiming / zoom, down to 60 % of it) get a proportionally smaller change, so blends stay
 * continuous. The new angle is written into the camera (it is what the game uses for culling too); the original is
 * remembered per camera and restored when the multiplier goes back to the boot value. */
static float g_fov_boot = 1.0f;   /* multiplier applied to camera_table at boot */
static struct {
    uint64_t cam;
    float orig, written;
} g_cams[16];
static SRWLOCK g_cams_lock = SRWLOCK_INIT;

static float fov_live_angle(float orig) {
    const float boot = 50.0f * g_fov_boot * 0.017453292f;
    const float ratio = g_fov / g_fov_boot;
    float t;
    if (fabsf(ratio - 1.0f) < 1e-4f || !(orig > 0.0f)) return orig;
    t = (orig / boot - 0.6f) / 0.4f;           /* 0 at 60 % of the exploration angle, 1 at the exploration angle */
    if (orig > boot) t = 1.0f - (orig / boot - 1.0f) / 0.3f; /* fades out above it (130 % = unchanged) */
    if (t <= 0.0f) return orig;
    if (t > 1.0f) t = 1.0f;
    return orig * (1.0f + (ratio - 1.0f) * t);
}

PLA_MOD_EXPORT pla_action eo_cam_proj_pre(pla_ctx* ctx) {
    static volatile long logged;
    const uint64_t cam = api->get_x(ctx, 0);
    float* p = (float*)api->host_pointer(cam + 0x230, 0x14);
    float cur, orig, nv;
    int i, slot = -1;
    if (!p || *(const int32_t*)p != 0) return PLA_CONTINUE; /* not perspective */
    cur = p[3];
    AcquireSRWLockExclusive(&g_cams_lock);
    for (i = 0; i < 16; ++i)
        if (g_cams[i].cam == cam) {
            slot = i;
            break;
        }
    if (slot < 0) {
        if (fabsf(g_fov - g_fov_boot) < 1e-4f) { /* nothing to do and nothing to restore */
            ReleaseSRWLockExclusive(&g_cams_lock);
            return PLA_CONTINUE;
        }
        for (i = 0; i < 16 && slot < 0; ++i)
            if (!g_cams[i].cam) slot = i;
        if (slot < 0) slot = (int)(cam >> 4) & 15; /* full: reuse one */
        g_cams[slot].cam = cam;
        g_cams[slot].orig = cur;
        g_cams[slot].written = -1.0f;
    }
    /* The game rewrote the angle (camera switch, zoom, blend): that is the new original. */
    orig = cur == g_cams[slot].written ? g_cams[slot].orig : cur;
    nv = fov_live_angle(orig);
    g_cams[slot].orig = orig;
    g_cams[slot].written = nv;
    if (nv == orig && fabsf(g_fov - g_fov_boot) < 1e-4f) g_cams[slot].cam = 0; /* back to normal: forget it */
    ReleaseSRWLockExclusive(&g_cams_lock);
    if (nv != cur) p[3] = nv;
    if (g_probe && nv != orig && _InterlockedIncrement(&logged) <= 20)
        logf_(PLA_LOG_WARNING, "engine_options: live FOV: camera %#llx %.2f -> %.2f degrees", (unsigned long long)cam,
              orig * 57.29578f, nv * 57.29578f);
    return PLA_CONTINUE;
}

typedef uint32_t (*live_version_fn)(void);
typedef int (*live_get_fn)(const char* key, char* out, int cap);
static live_version_fn g_live_version;
static live_get_fn g_live_get;
static uint32_t g_live_seen;
static ULONGLONG g_live_next;
static volatile LONG g_live_busy;

/* Live key lookup: "engine_options.<key>", falling back to the pre-rename prefix "motor_opciones.<key>" (nvnvk
 * builds from before the rename publish the settings of this mod under its old id). */
static int live_get(const char* key, char* out, int cap) {
    char k[96];
    int n;
    snprintf(k, sizeof k, "engine_options.%s", key);
    n = g_live_get(k, out, cap);
    if (n > 0) return n;
    snprintf(k, sizeof k, "motor_opciones.%s", key);
    return g_live_get(k, out, cap);
}

static int live_bool(const char* key, int cur) {
    char v[32];
    return live_get(key, v, sizeof v) > 0 ? is_true(v) : cur;
}

/* Called from the per-frame hooks; at most every 100 ms and by one thread at a time. */
static void live_poll(void) {
    const ULONGLONG now = GetTickCount64();
    char v[32];
    uint32_t ver;
    if (now < g_live_next || _InterlockedExchange(&g_live_busy, 1)) return;
    g_live_next = now + 100;
    if (!g_live_version) {
        HMODULE m = NULL;
        g_live_next = now + 1000;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvnvk.dll", &m) && m) {
            g_live_get = (live_get_fn)(void*)GetProcAddress(m, "nvnvk_live_get");
            g_live_version = g_live_get ? (live_version_fn)(void*)GetProcAddress(m, "nvnvk_live_version") : NULL;
            if (g_live_version) logf_(PLA_LOG_WARNING, "engine_options: live settings channel of nvnvk found");
        }
        if (!g_live_version) {
            g_live_busy = 0;
            return;
        }
    }
    ver = g_live_version();
    if (ver != g_live_seen) {
        g_live_seen = ver;
        g_bloom = live_bool("bloom", g_bloom);
        g_dof = live_bool("dof", g_dof);
        g_aberr = live_bool("chromatic_aberration", g_aberr);
        if (live_get("hud_hidden", v, sizeof v) > 0 && is_true(v) != (g_hud_hidden != 0)) {
            g_hud_hidden = is_true(v);
            logf_(PLA_LOG_WARNING, "engine_options: live: HUD %s", g_hud_hidden ? "hidden" : "shown");
        }
        if (live_get("fov", v, sizeof v) > 0) {
            float f = strtof(v, NULL);
            if (f < 0.5f) f = 0.5f;
            if (f > 2.0f) f = 2.0f;
            if (fabsf(f - g_fov) > 1e-4f) {
                g_fov = f; /* applied by eo_cam_proj_pre on the next frame */
                logf_(PLA_LOG_WARNING, "engine_options: live: FOV x%.3g (boot x%.3g)", f, g_fov_boot);
            }
        }
        /* texture_lod_bias: nvnvk adds the difference to its samplers (the game's samplers are already created);
         * lod / anim_full_rate: the game reads app_config only at startup. */
    }
    g_live_busy = 0;
}

/* ---- post-processing ------------------------------------------------------------------------------ */

/* vtable_7104182368::vf6(this, group) [main+0x29BD304]: copies the screen parameter group into the constant
 * block this+0x58. Fields (from the function): +0x714 BloomIntensity, +0x7B4/7B8/7BC DofFocus/
 * DofBlurBegin/DofBlurEnd, +0x7C0 DofEnable-0.5, +0x850 Aberration, +0x84C AberrationWeight. */
PLA_MOD_EXPORT pla_action eo_post_params_pre(pla_ctx* ctx) {
    uint64_t* cd = api->call_data(ctx);
    uint64_t blk = 0;
    cd[0] = 0;
    live_poll();
    if (g_dof && g_bloom && g_aberr && !g_probe) return PLA_CONTINUE;
    if (api->read_mem(ctx, api->get_x(ctx, 0) + 0x58, &blk, 8) == 8) cd[0] = blk;
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT void eo_post_params_post(pla_ctx* ctx) {
    static volatile long logged, last_sec = -1;
    uint64_t blk = api->call_data(ctx)[0];
    const float zero = 0.0f, dof_off = -0.5f;
    if (!blk) return;
    if (g_probe) {
        /* one line every 5 s with the values the game left */
        long sec = (long)((GetTickCount64() - g_t0) / 5000);
        if (sec != last_sec && _InterlockedExchange(&last_sec, sec) != sec && _InterlockedIncrement(&logged) <= 60) {
            float v[8] = {0};
            api->read_mem(ctx, blk + 0x714, &v[0], 4);
            api->read_mem(ctx, blk + 0x7B4, &v[1], 4);
            api->read_mem(ctx, blk + 0x7B8, &v[2], 4);
            api->read_mem(ctx, blk + 0x7BC, &v[3], 4);
            api->read_mem(ctx, blk + 0x7C0, &v[4], 4);
            api->read_mem(ctx, blk + 0x850, &v[5], 4);
            api->read_mem(ctx, blk + 0x84C, &v[6], 4);
            logf_(PLA_LOG_WARNING,
                  "engine_options: game post-processing: bloom %g, dof %g (focus %g, %g..%g), aberration %g (weight %g)",
                  v[0], v[4] + 0.5f, v[1], v[2], v[3], v[5], v[6]);
        }
    }
    if (ab_before()) return;
    if (!g_bloom) api->write_mem(ctx, blk + 0x714, &zero, 4);
    if (!g_dof) api->write_mem(ctx, blk + 0x7C0, &dof_off, 4);
    if (!g_aberr) {
        api->write_mem(ctx, blk + 0x850, &zero, 4);
        api->write_mem(ctx, blk + 0x84C, &zero, 4);
    }
}

/* ---- texture LOD bias ----------------------------------------------------------------------------- */

/* FUN_7102a9b5bc(out, device, desc): creates a sampler with nvnSamplerBuilder from the description `desc`
 * (x19). After creating it, it stores the description (content-keyed sampler cache), so the description must NOT
 * stay modified: the bias is changed right before it is read (main+0x2A9B670, return from SetLodClamp; next comes
 * ldr s0,[x19,#0x1c] -> SetLodBias) and restored on return from SetLodBias (main+0x2A9B684). */
#define MAX_BIAS_PENDING 16
static struct {
    uint64_t va;
    float orig;
} g_bias_pending[MAX_BIAS_PENDING];
static SRWLOCK g_bias_lock = SRWLOCK_INIT;

PLA_MOD_EXPORT pla_action eo_sampler_pre(pla_ctx* ctx) {
    static volatile long logged;
    uint64_t va;
    float b, nb;
    int i;
    if (g_tex_bias == 0.0f) return PLA_CONTINUE;
    va = api->get_x(ctx, 19) + 0x1C;
    if (api->read_mem(ctx, va, &b, 4) != 4 || !isfinite(b)) return PLA_CONTINUE;
    nb = b + g_tex_bias;
    if (nb < -15.0f) nb = -15.0f;
    if (nb > 15.0f) nb = 15.0f;
    AcquireSRWLockExclusive(&g_bias_lock);
    for (i = 0; i < MAX_BIAS_PENDING; ++i)
        if (!g_bias_pending[i].va) {
            g_bias_pending[i].va = va;
            g_bias_pending[i].orig = b;
            break;
        }
    ReleaseSRWLockExclusive(&g_bias_lock);
    if (i == MAX_BIAS_PENDING) return PLA_CONTINUE; /* no slot to restore it: left alone */
    api->write_mem(ctx, va, &nb, 4);
    if (_InterlockedIncrement(&logged) <= 3) logf_(PLA_LOG_WARNING, "engine_options: sampler: LOD bias %g -> %g", b, nb);
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT pla_action eo_sampler_restore_pre(pla_ctx* ctx) {
    uint64_t va;
    int i;
    if (g_tex_bias == 0.0f) return PLA_CONTINUE;
    va = api->get_x(ctx, 19) + 0x1C;
    AcquireSRWLockExclusive(&g_bias_lock);
    for (i = 0; i < MAX_BIAS_PENDING; ++i)
        if (g_bias_pending[i].va == va) {
            api->write_mem(ctx, va, &g_bias_pending[i].orig, 4);
            g_bias_pending[i].va = 0;
            break;
        }
    ReleaseSRWLockExclusive(&g_bias_lock);
    return PLA_CONTINUE;
}

/* ---- shadows (render init) ------------------------------------------------------------------------ */

/* FUN_71029d6a64(cfg) [main+0x29D6A64]: render system init. It publishes cfg+0x90 (int) as the render
 * setting "AvailableShadowCascades" and cfg+0x8C as "ShadowResolution"; the shadow passes size their cascade
 * arrays and maps from those settings (vtable_7104183a20::vf15, vtable_7104183ba0::vf15, ...). The pre hook
 * logs both and applies "shadow_cascades" (never above the game's count) / "shadow_resolution" when set. */
PLA_MOD_EXPORT pla_action eo_render_init_pre(pla_ctx* ctx) {
    const uint64_t cfg = api->get_x(ctx, 0);
    int32_t res = 0, casc = 0, nres, ncasc;
    if (api->read_mem(ctx, cfg + 0x8C, &res, 4) != 4 || api->read_mem(ctx, cfg + 0x90, &casc, 4) != 4) {
        logf_(PLA_LOG_WARNING, "engine_options: render init: cannot read the settings at %#llx", (unsigned long long)cfg);
        return PLA_CONTINUE;
    }
    ncasc = g_shadow_cascades && casc > 0 && g_shadow_cascades < casc ? g_shadow_cascades : casc;
    nres = g_shadow_res && res > 0 ? g_shadow_res : res;
    if (ncasc != casc) api->write_mem(ctx, cfg + 0x90, &ncasc, 4);
    if (nres != res) api->write_mem(ctx, cfg + 0x8C, &nres, 4);
    logf_(PLA_LOG_WARNING, "engine_options: render init: shadow cascades %d -> %d, shadow resolution %d -> %d", casc, ncasc,
          res, nres);
    return PLA_CONTINUE;
}

/* ---- HUD ------------------------------------------------------------------------------------------ */

/* nn::ui2d::Layout::Draw(DrawInfo&, gfx::CommandBuffer&) [main+0x1353E0, vf12]. */
PLA_MOD_EXPORT pla_action eo_layout_draw_pre(pla_ctx* ctx) {
    (void)ctx;
    poll_hud_key();
    live_poll();
    return g_hud_hidden && !ab_before() ? PLA_SKIP : PLA_CONTINUE;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    if (g_logf) {
        fclose(g_logf);
        g_logf = NULL;
    }
}

/* ---- init ----------------------------------------------------------------------------------------- */

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

static void add_appcfg_name(const char* file) {
    int i;
    for (i = 0; i < g_nappcfg; ++i)
        if (!_stricmp(g_appcfg_names[i], file)) return;
    if (g_nappcfg < MAX_PATCHES) snprintf(g_appcfg_names[g_nappcfg++], sizeof g_appcfg_names[0], "%s", file);
}

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    char v[512];
    int i;
    api = a;
    g_t0 = GetTickCount64();
    open_log_file();
    if (!PLA_API_HAS(api, host_pointer)) {
        api->log(PLA_LOG_ERROR, "engine_options: the runtime has no host_pointer (pla_api 1.1)");
        return 1;
    }
    g_probe = cfg_bool(config, "probe", 0);
    if (cfg_get(config, "probe", v, sizeof v) && !_stricmp(v, "all")) g_probe = 2; /* every opened path */
    g_fov = cfg_float(config, "fov", 1.0f, 0.5f, 2.0f);
    g_fov_boot = g_fov;
    g_lod = cfg_float(config, "lod", 1.0f, 0.5f, 4.0f);
    g_tex_bias = cfg_float(config, "texture_lod_bias", 0.0f, -4.0f, 4.0f);
    g_model_lod = cfg_float(config, "model_lod", g_lod, 0.05f, 16.0f);
    g_dof = cfg_bool(config, "dof", 1);
    g_bloom = cfg_bool(config, "bloom", 1);
    g_aberr = cfg_bool(config, "chromatic_aberration", 1);
    g_anim_full = cfg_bool(config, "anim_full_rate", 0);
    g_hud_hidden = cfg_bool(config, "hud_hidden", 0); /* initial HUD state (tests / photo mode) */
    if (cfg_get(config, "hud_toggle_key", v, sizeof v) && v[0]) {
        g_hud_vk = parse_vk(v);
        if (g_hud_vk <= 0) {
            logf_(PLA_LOG_WARNING, "engine_options: hud_toggle_key=\"%s\" is not a known key", v);
            g_hud_vk = 0;
        }
    }
    g_ab_ms = (ULONGLONG)(cfg_float(config, "ab_secs", 0.0f, 0.0f, 3600.0f) * 1000.0f);
    g_need_camera = fabsf(g_fov - 1.0f) > 1e-4f;

    /* CPU options (game-side work). "cpu_preset": "auto" (default) | "full" (= "default") | "performance".
     * "auto" picks "performance" on a CPU with fewer than 8 physical cores (cpu_is_low). Every CPU key below
     * accepts "auto" (or a missing key) = the value of the preset; an explicit value wins.
     *  - "npc_density" (0.25-1): multiplier of the village NPC / NPC-creature load radii (fewer characters
     *    loaded, animated and updated). Performance: 0.67, and never more than the game's own radius (1/lod).
     *  - "anim_lod": "default" | "reduced" | "low": fewer actors animated at the full / intermediate rates and a
     *    shorter base distance of the animation rate levels (field_anime_framerate_config). Performance: low.
     *  - "shadow_cascades" (1-4, "game"): shadow cascades created at render init (only lowers the game's count;
     *    the game uses 2 at 1024;
     *    1 loses the visible shadows with no measured gain). "shadow_resolution" (256-4096, "game"): shadow map
     *    size. Advanced keys, not in the menu; "auto" = as in the game.
     *  - "terrain_detail": "default" | "reduced": camera-to-terrain-LOD / stream-load points (field_common_config).
     *  - "grass_detail": "default" | "reduced": detail (grass) fade (field_common_config).
     *  - "ambient_voices": "default" | "reduced": ambient creature voices and sound occlusion ray length
     *    (env_poke_voice_config, sound_config). */
    {
        int cores = 0, threads = 0, low;
        const char* tier;
        v[0] = 0;
        cfg_get(config, "cpu_preset", v, sizeof v);
        if (!_stricmp(v, "performance")) low = 1, tier = "performance";
        else if (!_stricmp(v, "default") || !_stricmp(v, "full") || !_stricmp(v, "quality")) low = 0, tier = "full";
        else {
            low = cpu_is_low(&cores, &threads);
            tier = low ? "auto -> performance" : "auto -> full";
        }
        logf_(PLA_LOG_WARNING, "engine_options: CPU preset %s (%d cores / %d threads available)", tier, cores, threads);
#define EO_AUTO(key) (!cfg_get(config, key, v, sizeof v) || !v[0] || !_stricmp(v, "auto"))
        if (EO_AUTO("npc_density")) {
            g_npc_density = 1.0f;
            if (low) {
                g_npc_density = 0.67f;
                if (g_lod > 1.0f && 1.0f / g_lod < g_npc_density) g_npc_density = 1.0f / g_lod;
                if (g_npc_density < 0.25f) g_npc_density = 0.25f;
            }
        } else
            g_npc_density = cfg_float(config, "npc_density", 1.0f, 0.25f, 1.0f);
        g_anim_lod = low ? 2 : 0;
        if (!EO_AUTO("anim_lod")) g_anim_lod = !_stricmp(v, "low") ? 2 : !_stricmp(v, "reduced") ? 1 : 0;
        g_shadow_cascades = 0;
        if (!EO_AUTO("shadow_cascades") && _stricmp(v, "game"))
            g_shadow_cascades = (int)cfg_float(config, "shadow_cascades", 0.0f, 1.0f, 4.0f);
        g_shadow_res = 0;
        if (!EO_AUTO("shadow_resolution") && _stricmp(v, "game"))
            g_shadow_res = (int)cfg_float(config, "shadow_resolution", 0.0f, 256.0f, 4096.0f);
        {
            int terrain = 0, grass = 0, voices = 0;
            if (!EO_AUTO("terrain_detail")) terrain = !_stricmp(v, "reduced");
            if (!EO_AUTO("grass_detail")) grass = !_stricmp(v, "reduced");
            if (!EO_AUTO("ambient_voices")) voices = !_stricmp(v, "reduced");
            if (terrain) {
                add_patch("field_common_config", "kDistanceOfCameraToTerrainLodPoint", 0, 0.0f, "14");
                add_patch("field_common_config", "kDistanceOfCameraToStreamLoadPoint", 0, 0.0f, "14");
            }
            if (grass) {
                add_patch("field_common_config", "kDetailFadeCoefficient", 0, 0.0f, "1");
                add_patch("field_common_config", "kDetailFadeDistanceNear", 1, 0.0f, "1");
            }
            if (voices) {
                add_patch("env_poke_voice_config", "voice_register_cound_max", 0, 0.0f, "12");
                add_patch("sound_config", "max_raycast_distance", 0, 0.0f, "15");
                add_patch("sound_config", "max_raycast_distance_npc", 0, 0.0f, "15");
            }
            if (terrain || grass || voices)
                logf_(PLA_LOG_WARNING, "engine_options: terrain detail %s, grass detail %s, ambient voices %s",
                      terrain ? "reduced" : "default", grass ? "reduced" : "default", voices ? "reduced" : "default");
        }
#undef EO_AUTO
    }
    /* Draw/load distances (field_common_config, event_culling_config). */
    {
        const float npc = g_lod * g_npc_density;
        const float anim_dist = g_lod * (g_anim_lod == 2 ? 0.5f : g_anim_lod == 1 ? 0.75f : 1.0f);
        if (fabsf(g_lod - 1.0f) > 1e-4f) {
            add_patch("field_common_config", "kTreeVisibleDistance", 1, g_lod, NULL);
            add_patch("field_common_config", "kTreeFadeDistanceFar", 0, g_lod, NULL);
            add_patch("field_common_config", "kTreeFadeDistanceFar", 1, g_lod, NULL);
            add_patch("event_culling_config", "npc_distance", 0, g_lod, NULL);
            add_patch("event_culling_config", "bg_distance", 0, g_lod, NULL);
            add_patch("event_culling_config", "terrain_radius", 0, g_lod, NULL);
        }
        if (fabsf(npc - 1.0f) > 1e-4f) {
            add_patch("field_common_config", "kArea00NpcLoadRadius", 0, npc, NULL);
            add_patch("field_common_config", "kArea00NpcPokeLoadRadius", 0, npc, NULL);
        }
        if (fabsf(anim_dist - 1.0f) > 1e-4f)
            add_patch("field_anime_framerate_config", "distanceBaseValue", 0, anim_dist, NULL);
    }
    /* Animation rate levels: capacities (how many actors get each rate). */
    if (g_anim_lod == 2) {
        add_patch("field_anime_framerate_config", "framerateLv0capacity", 0, 0.0f, "2");
        add_patch("field_anime_framerate_config", "framerateLv1capacity", 0, 0.0f, "2");
        add_patch("field_anime_framerate_config", "framerateLv2capacity", 0, 0.0f, "4");
    } else if (g_anim_lod == 1) {
        add_patch("field_anime_framerate_config", "framerateLv0capacity", 0, 0.0f, "3");
        add_patch("field_anime_framerate_config", "framerateLv2capacity", 0, 0.0f, "6");
    }
    /* Full-rate animation for more creatures (field_anime_framerate_config); wins over anim_lod for Lv0. */
    if (g_anim_full) add_patch("field_anime_framerate_config", "framerateLv0capacity", 0, 0.0f, "99");
    if (cfg_get(config, "app_config", v, sizeof v) && v[0]) parse_raw_patches(v);
    for (i = 0; i < g_npatches; ++i) add_appcfg_name(g_patches[i].file);

    logf_(PLA_LOG_WARNING,
          "engine_options: fov x%.3g, lod x%.3g, texture LOD bias %+g, dof %s, bloom %s, aberration %s, "
          "full-rate animation %s, NPC density x%.3g, animation LOD %s, shadows %d/%d, HUD key %s (vk %#x), %d substitutions in %d files%s",
          g_fov, g_lod, g_tex_bias, g_dof ? "yes" : "no", g_bloom ? "yes" : "no", g_aberr ? "yes" : "no",
          g_anim_full ? "yes" : "no", g_npc_density, g_anim_lod == 2 ? "low" : g_anim_lod == 1 ? "reduced" : "default",
          g_shadow_cascades, g_shadow_res, g_hud_vk ? "yes" : "no", g_hud_vk, g_npatches, g_nappcfg, g_probe ? ", probe" : "");
    return 0;
}
