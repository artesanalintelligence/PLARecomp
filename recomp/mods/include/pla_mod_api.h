/* SPDX-FileCopyrightText: Copyright 2026 PLA recomp contributors
 * SPDX-License-Identifier: MIT
 *
 * pla_mod_api.h - native mods for statically recompiled Switch titles.
 *
 * A mod is a host DLL plus a mod.json that maps guest function addresses
 * (module + offset) to functions the DLL exports. When a recompiled image is
 * loaded, the runtime (core/arm/recomp/recomp_mods.cpp) rewrites that image's
 * block index so the guest address dispatches into the mod first. Nothing is
 * re-exported or recompiled: enabling or disabling a mod only needs a restart
 * of the game.
 *
 * Hook model (the AOT counterpart of RECOMP_HOOK / RECOMP_HOOK_RETURN /
 * RECOMP_PATCH in N64Recomp):
 *   pre  (pla_pre_fn)  runs at the function entry, with the arguments in
 *                      X0-X7 / V0-V7. It may change them and returns
 *                        PLA_CONTINUE  run the original function (the
 *                                      trampoline is its recompiled code),
 *                        PLA_SKIP      do not run it: return to the caller
 *                                      (X30) with whatever the hook left in
 *                                      X0/V0 - a full replacement,
 *                        PLA_REDIRECT  continue at the PC the hook set with
 *                                      set_pc (X30 untouched, so a guest
 *                                      function jumped to returns to the
 *                                      original caller).
 *   post (pla_post_fn) runs when the original returns, with its results in
 *                      X0 / V0. pla_call_data() keeps up to four values from
 *                      the pre hook of the same call (e.g. an out-pointer
 *                      argument, which X0 no longer holds after the call).
 *
 * The original cannot be called synchronously from inside a hook: a guest
 * function may stop at any instruction for a supervisor call (IPC, mutexes,
 * sleeps) that the HLE kernel services after the CPU backend returns, which
 * a host stack frame of the mod cannot survive. pre + PLA_CONTINUE + post is
 * that call, split at the point where the guest runs.
 *
 * Hooks run on guest CPU threads, possibly several at once: keep them
 * thread-safe and short. A pla_ctx is valid only during the call it was
 * passed to.
 */
#ifndef PLA_MOD_API_H
#define PLA_MOD_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change. The runtime refuses a mod whose
 * pla_mod_api_version() differs; pla_api.size grows for compatible additions. */
#define PLA_MOD_API_VERSION 1u

#if defined(_WIN32)
#define PLA_MOD_EXPORT __declspec(dllexport)
#else
#define PLA_MOD_EXPORT __attribute__((visibility("default")))
#endif

typedef struct pla_ctx pla_ctx; /* opaque: the guest CPU context of the calling thread */

typedef enum pla_action {
    PLA_CONTINUE = 0,
    PLA_SKIP = 1,
    PLA_REDIRECT = 2
} pla_action;

typedef pla_action (*pla_pre_fn)(pla_ctx* ctx);
typedef void (*pla_post_fn)(pla_ctx* ctx);

enum { PLA_LOG_TRACE = 0, PLA_LOG_DEBUG, PLA_LOG_INFO, PLA_LOG_WARNING, PLA_LOG_ERROR };

/* Register numbers for get_x/set_x: 0-30 are X0-X30 (X29 = FP, X30 = LR). */
#define PLA_REG_FP 29u
#define PLA_REG_LR 30u

typedef struct pla_api {
    uint32_t version; /* PLA_MOD_API_VERSION */
    uint32_t size;    /* sizeof(pla_api) on the runtime side */

    /* General registers. n = 0..30; anything else reads 0 and is not written. */
    uint64_t (*get_x)(pla_ctx* ctx, unsigned n);
    void (*set_x)(pla_ctx* ctx, unsigned n, uint64_t value);
    uint64_t (*get_sp)(pla_ctx* ctx);
    void (*set_sp)(pla_ctx* ctx, uint64_t value);
    uint64_t (*get_pc)(pla_ctx* ctx);
    void (*set_pc)(pla_ctx* ctx, uint64_t value); /* only meaningful with PLA_REDIRECT */

    /* SIMD/FP registers V0-V31 as two 64-bit halves (lo = bits 0-63). */
    void (*get_v)(pla_ctx* ctx, unsigned n, uint64_t out[2]);
    void (*set_v)(pla_ctx* ctx, unsigned n, uint64_t lo, uint64_t hi);

    /* Guest virtual memory. Return the number of bytes copied (short on an
     * unmapped page). Do not write guest code: the code guard would send the
     * changed blocks to the JIT, or stop a strict run. */
    size_t (*read_mem)(pla_ctx* ctx, uint64_t va, void* dst, size_t size);
    size_t (*write_mem)(pla_ctx* ctx, uint64_t va, const void* src, size_t size);

    /* Load address of a module by its export name ("main", "sdk", "subsdk0",
     * "rtld"), or 0. Guest addresses are module base + offset. */
    uint64_t (*module_base)(const char* module);

    /* Four values that live from the pre hook to the post hook of one call. */
    uint64_t* (*call_data)(pla_ctx* ctx);

    /* Writes one line to suyu's log, prefixed with the mod id. */
    void (*log)(int level, const char* message);

    /* ---- extension 1.1 (check with PLA_API_HAS(api, event_create)) ----------
     *
     * Kernel events. A pla_event is an opaque id (0 = none) for an HLE KEvent.
     * The runtime does all kernel work on one host thread of its own, so these
     * are safe from hooks (guest CPU threads) and from any host thread (an
     * audio callback). Calls that return a value wait for that thread
     * (microseconds); signal and clear are queued and return at once, in order.
     *
     *   event_create         new event owned by the runtime (like an HLE service's)
     *   event_guest_handle   adds its readable end to the game's handle table and
     *                        returns the handle (what an SDK function hands the game
     *                        for os::AttachReadableHandleToSystemEvent); the game owns
     *                        and closes it. 0 on failure. Each call is a new handle.
     *   event_from_guest_handle  an event the game already has (readable or writable
     *                        handle), so native code can signal or wait on it
     *   event_signal/clear   0 = queued, -1 = unknown event
     *   event_wait           host threads only (never a hook: it would stall a guest
     *                        core). 1 = signalled, 0 = timeout, -1 = error.
     *                        timeout_ns < 0 waits forever. Does not clear.
     *   event_destroy        drops the runtime's reference (the game's handles stay valid)
     */
    uint64_t (*event_create)(const char* name);
    uint32_t (*event_guest_handle)(uint64_t event);
    uint64_t (*event_from_guest_handle)(uint32_t handle);
    int (*event_signal)(uint64_t event);
    int (*event_clear)(uint64_t event);
    int (*event_wait)(uint64_t event, int64_t timeout_ns);
    void (*event_destroy)(uint64_t event);

    /* Host address of guest memory [va, va + size), or NULL when the range is
     * not mapped or not contiguous on the host. host_pointer uses the fastmem
     * arena (any mapped range is contiguous there); host_backing_pointer the
     * physical backing (one big host allocation: contiguous only where the
     * guest's physical pages are). Valid until the guest unmaps the range.
     * Plain loads and stores through them are what read_mem/write_mem do. */
    void* (*host_pointer)(uint64_t va, size_t size);
    void* (*host_backing_pointer)(uint64_t va, size_t size);

    /* The game window (HWND on Windows) for a native renderer, or NULL. */
    void* (*window_handle)(void);

    /* The guest's CNTPCT_EL0 (19.2 MHz), as os::GetSystemTick sees it. */
    uint64_t (*guest_ticks)(void);

    /* ---- extension 1.2 (check with PLA_API_HAS) ----
     * A native renderer that presents on its own (and skips the game's
     * vi/nvnflinger queue) calls this once per presented frame: it feeds the
     * frame counter (window title FPS, PERF samples, the loading splash). */
    void (*frame_presented)(void);

    /* Guest memory owned by a mod: maps `size` bytes (rounded up to 4 KiB) of new,
     * zeroed, read-write memory into the game's address space (outside its heap,
     * in the stack region, as the main thread stack is) and returns the guest
     * address, or 0. `host` (optional) receives host_pointer() of the range.
     * For data the game must read through a guest pointer (shared fonts...).
     * Not from inside a hook. guest_free unmaps it (0 = ok). */
    uint64_t (*guest_alloc)(size_t size, void** host);
    int (*guest_free)(uint64_t va, size_t size);
} pla_api;

/* True when the runtime's pla_api has `member` (compatible additions grow `size`). */
#define PLA_API_HAS(api, member) \
    ((api)->size >= offsetof(pla_api, member) + sizeof(((pla_api*)0)->member))

/* ---- what a mod DLL exports ------------------------------------------------
 *
 *   PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }   required
 *   PLA_MOD_EXPORT int  pla_mod_init(const pla_api* api, const char* config_json);    optional
 *        config_json is the "config" object of mod.json ("{}" when absent).
 *        Return 0 to load; anything else leaves the mod out.
 *   PLA_MOD_EXPORT void pla_mod_shutdown(void);                                       optional
 *   PLA_MOD_EXPORT pla_action <pre name>(pla_ctx* ctx);    named in mod.json "pre"
 *   PLA_MOD_EXPORT void       <post name>(pla_ctx* ctx);   named in mod.json "post"
 */

/* ---- convenience helpers (header only) ---------------------------------- */

static inline float pla_get_s(const pla_api* api, pla_ctx* ctx, unsigned n) {
    uint64_t v[2];
    uint32_t bits;
    float f;
    api->get_v(ctx, n, v);
    bits = (uint32_t)v[0];
    memcpy(&f, &bits, 4);
    return f;
}

static inline double pla_get_d(const pla_api* api, pla_ctx* ctx, unsigned n) {
    uint64_t v[2];
    double d;
    api->get_v(ctx, n, v);
    memcpy(&d, &v[0], 8);
    return d;
}

/* Writing S/D zeroes the rest of the vector register, as a scalar FP write does. */
static inline void pla_set_s(const pla_api* api, pla_ctx* ctx, unsigned n, float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    api->set_v(ctx, n, bits, 0);
}

static inline void pla_set_d(const pla_api* api, pla_ctx* ctx, unsigned n, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    api->set_v(ctx, n, bits, 0);
}

static inline uint32_t pla_read_u32(const pla_api* api, pla_ctx* ctx, uint64_t va) {
    uint32_t v = 0;
    api->read_mem(ctx, va, &v, 4);
    return v;
}

static inline uint64_t pla_read_u64(const pla_api* api, pla_ctx* ctx, uint64_t va) {
    uint64_t v = 0;
    api->read_mem(ctx, va, &v, 8);
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* PLA_MOD_API_H */
