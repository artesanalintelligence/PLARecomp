/* battle_target_fix: freeze in battle with some move animations at high frame rates (Judgment, 2026-10-18).
 *
 * vtable_71040cb7e8::vf11 (main+0x953D38) is a battle effect sequence step that follows a target object: x20 =
 * the step's object (map.at(id), one reference taken), then [x19+0x68]->vf(0x10) returns the target as a smart
 * pointer in [x29-0x38] (main+0x953F48). The game does not check it: with a null target it calls FUN_9a7808 /
 * FUN_9abe2c, which read [0+0x108] and then call a null vtable entry (guest pc=0, the game freezes). At 30 FPS the
 * target still exists when the step runs; at uncapped FPS (unlocked-FPS patch) it is already gone ~2.5 s into the
 * Judgment sequence (ee_fc_en13_00), every time. The step runs once ([x19+0xF0] is cleared at main+0x953D70) and
 * places its object relative to the target; with Judgment the target is the creature's new form, whose model loads
 * asynchronously (wall time) while the sequence advances per frame, so at high FPS the step comes first.
 *
 * Hook at main+0x953F4C (return from that blr): when the target is null, set [x19+0xF0] again so the step is
 * retried on the next frame (as at 30 FPS, where the model is ready in time), and leave the step the way its normal
 * end does without the target-dependent work: drop the reference on x20, restore the frame and tail-call
 * main+0x953984(x19, v8), as main+0x954190 does. Skipping the step instead left the creature invisible during the move.
 *
 * The root cause is a gap, not the frame rate itself: the form change step (vtable_71040cc0a8::vf9, the call to the
 * model setter main+0x9E2040 returning to main+0x95AB1C) empties the actor's model ([actor+0x258]) and the new form
 * loads asynchronously (measured 1.6 s at 30 FPS, 0.6 s uncapped), while the sequence keeps running on time. Steps
 * that touch the model in that gap either crash (the one above) or do nothing: vtable_71040cd458::vf9 (one shot,
 * main+0x96D4FC) shows the model through a component of it, and when it runs in the gap (uncapped, ~0.53 s after
 * the change) the creature stays invisible. So the whole sequence waits: while a model emptied by the form change step
 * is still empty, the sequence frame does not advance (the sequence player's vf11, vtable_71040d3338 at
 * main+0x9DBEAC, still updates the active steps). Limit 5 s, then it goes on as before. */
#include <stdarg.h>
#include <stdio.h>
#include <intrin.h>
#include "pla_mod_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const pla_api* api;
static uint64_t g_main;
static volatile long g_hits, g_leaks;

static void logf_(int lvl, const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    api->log(lvl, line);
}

static uint64_t rd64(pla_ctx* ctx, uint64_t va) {
    uint64_t v = 0;
    api->read_mem(ctx, va, &v, 8);
    return v;
}

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    (void)config;
    api = a;
    g_main = api->module_base("main");
    return g_main ? 0 : -1;
}

/* Drops one reference of x20 (atomic decrement of [[x20+0x58]+0xC]) unless it is the last one: freeing needs a
 * guest virtual call (main+0x95435C), so a last reference is left in place (a leak of one effect object). */
static void release_ref(pla_ctx* ctx, uint64_t obj) {
    const uint64_t rc = obj ? rd64(ctx, obj + 0x58) : 0;
    volatile long* p = rc ? (volatile long*)api->host_pointer(rc + 0xC, 4) : NULL;
    long v;
    if (!p) return;
    for (;;) {
        v = *p;
        if (v <= 1) {
            _InterlockedIncrement(&g_leaks);
            return;
        }
        if (_InterlockedCompareExchange(p, v - 1, v) == v) return;
    }
}

/* Actors whose model the form change step emptied, and when (host ms). */
#define MAX_GAPS 4
static volatile uint64_t g_gap_actor[MAX_GAPS];
static volatile uint64_t g_gap_t0[MAX_GAPS];
static volatile long g_paused_frames, g_gaps, g_timeouts;
#define GAP_LIMIT_MS 5000

/* main+0x9E2040 (entry): SetModel(actor = x0, sp<Model>* x1); [x1] = 0 from main+0x95AB1C starts a gap. */
PLA_MOD_EXPORT pla_action btf_setmodel(pla_ctx* ctx) {
    const uint64_t actor = api->get_x(ctx, 0);
    int i;
    if (api->get_x(ctx, 30) - g_main != 0x95AB1C || rd64(ctx, api->get_x(ctx, 1)) != 0) return PLA_CONTINUE;
    for (i = 0; i < MAX_GAPS; ++i)
        if (g_gap_actor[i] == 0 || g_gap_actor[i] == actor) {
            g_gap_t0[i] = GetTickCount64();
            g_gap_actor[i] = actor;
            if (_InterlockedIncrement(&g_gaps) <= 5)
                logf_(PLA_LOG_INFO, "battle_target_fix: form change emptied actor %#llx's model; sequences wait for the new one",
                      (unsigned long long)actor);
            return PLA_CONTINUE;
        }
    return PLA_CONTINUE;
}

/* True while an actor emptied by the form change still has no model (and the limit has not passed). */
static int in_gap(pla_ctx* ctx) {
    int i, any = 0;
    for (i = 0; i < MAX_GAPS; ++i) {
        const uint64_t actor = g_gap_actor[i];
        if (!actor) continue;
        if (rd64(ctx, actor + 0x258) != 0) {
            logf_(PLA_LOG_INFO, "battle_target_fix: actor %#llx has its new model after %llu ms", (unsigned long long)actor,
                  (unsigned long long)(GetTickCount64() - g_gap_t0[i]));
            g_gap_actor[i] = 0;
        } else if (GetTickCount64() - g_gap_t0[i] > GAP_LIMIT_MS) {
            _InterlockedIncrement(&g_timeouts);
            logf_(PLA_LOG_WARNING, "battle_target_fix: actor %#llx still without a model after %u ms, sequences go on",
                  (unsigned long long)actor, GAP_LIMIT_MS);
            g_gap_actor[i] = 0;
        } else {
            any = 1;
        }
    }
    return any;
}

/* main+0x9DBEAC (entry): sequence player vf11, which updates the active steps and then advances the sequence frame
 * (main+0x9DC3D4: if frame < end, frame = min(frame + rate, end), frame at [x0+0xE8], end at +0xEC, rate at +0xF0).
 * During a gap the active steps still have to run (the form change step itself assigns the new model once it has
 * loaded: skipping the whole update never ended the gap), so the frame is moved back by one step before the update
 * and the update's own advance brings it back: no later step starts before the model exists. (A post hook that put
 * the frame back afterwards broke the battle intro: this path does not tolerate the trap return address.) */
PLA_MOD_EXPORT pla_action btf_sequence(pla_ctx* ctx) {
    const uint64_t seq = api->get_x(ctx, 0);
    float f[3];
    if (!in_gap(ctx)) return PLA_CONTINUE;
    if (api->read_mem(ctx, seq + 0xE8, f, sizeof f) != sizeof f || !(f[2] > 0.0f) || !(f[0] < f[1])) return PLA_CONTINUE;
    f[0] -= f[2];
    api->write_mem(ctx, seq + 0xE8, &f[0], 4);
    _InterlockedIncrement(&g_paused_frames);
    return PLA_CONTINUE;
}

/* main+0x953F4C */
PLA_MOD_EXPORT pla_action btf_target(pla_ctx* ctx) {
    const uint64_t fp = api->get_x(ctx, 29), sp = api->get_sp(ctx), x19 = api->get_x(ctx, 19);
    const uint8_t one = 1;
    uint64_t v8[2], d[2];
    long n;
    if (rd64(ctx, fp - 0x38) != 0) return PLA_CONTINUE;
    n = _InterlockedIncrement(&g_hits);
    api->write_mem(ctx, x19 + 0xF0, &one, 1);
    release_ref(ctx, api->get_x(ctx, 20));
    api->get_v(ctx, 8, v8);
    api->set_v(ctx, 0, v8[0], v8[1]);
    api->set_x(ctx, 0, x19);
    /* epilogue of vf11 (main+0x954194..0x9541A8) */
    api->read_mem(ctx, sp + 0x60, d, 16);
    api->set_v(ctx, 9, d[0], 0);
    api->set_v(ctx, 8, d[1], 0);
    api->set_x(ctx, 29, rd64(ctx, sp + 0x90));
    api->set_x(ctx, 30, rd64(ctx, sp + 0x98));
    api->set_x(ctx, 20, rd64(ctx, sp + 0x80));
    api->set_x(ctx, 19, rd64(ctx, sp + 0x88));
    api->set_x(ctx, 22, rd64(ctx, sp + 0x70));
    api->set_x(ctx, 21, rd64(ctx, sp + 0x78));
    api->set_sp(ctx, sp + 0xA0);
    api->set_pc(ctx, g_main + 0x953984);
    if (n <= 5) logf_(PLA_LOG_INFO, "battle_target_fix: effect step %#llx without target yet, retried next frame (#%ld)",
                      (unsigned long long)x19, n);
    return PLA_REDIRECT;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    if (api && (g_hits || g_gaps))
        logf_(PLA_LOG_INFO, "battle_target_fix: %ld form change gap(s), %ld sequence frame(s) held, %ld timeout(s); %ld step "
              "update(s) deferred for a missing target, %ld last reference(s) kept", g_gaps, g_paused_frames, g_timeouts,
              g_hits, g_leaks);
}
