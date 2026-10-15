// islands: native islands for the hottest game functions (sprint FPS-4, docs/port/PROGRESS-FPS4-ISLANDS.md).
//
// Island 1 (skeleton): main+0x2A697AC, the lazy recursive joint world-matrix update (and its TRS->matrix
// helpers 0x2ACB330 / 0x2ACB6F0), lifted by nlift.py into native C++ (islands_gen.inc). The pre hook runs the
// native version and returns PLA_SKIP; a path the island does not cover (constraint joints, non-finite inputs,
// unexpected FPCR) returns PLA_CONTINUE before writing anything of that joint, so the recompiled original runs.
//
// Modes (mod.json "config", overridable with environment variables):
//   skeleton   (PLA_ISLANDS_SKEL)    1 = native island, 0 = hook off (original only)
//   verify     (PLA_ISLANDS_VERIFY)  1 = shadow check: the island runs on shadow copies of the outputs, the
//                                    original then runs for real, and a post hook compares both byte by byte
//   capture    (PLA_ISLANDS_CAPTURE) N > 0: in verify mode, write every Nth checked call (and every mismatch)
//                                    to islands-captures.bin for the offline replay harness (isl_replay.exe)
//   stats_s    (PLA_ISLANDS_STATS)   seconds between stats lines in the log (0 = off)
//
// Requirements: plahost (GuestContext layout of tools/port/plahost/src/common.h), FM2 guest memory, a CPU
// with FMA3 + SSE4.1. Otherwise the mod stays inert.
//
// Built on the user's machine at install time (tools/islands/build_islands.py: nlift.py on the user's main.elf +
// the portable toolchain), so the lifted game code is never distributed. No C++ standard library (the portable
// toolchain has none), no dynamic initializers and only constant-initialized thread_local variables.
#include <immintrin.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "nl_rt.h"
#include "pla_mod_api.h"

using nl::V;

static const pla_api* api;
uint64_t g_main;
thread_local uint32_t g_fpcr;
static bool g_constraints;
static uint8_t* g_fm;  // FM2 base: host address of guest va = g_fm + va
static bool g_on, g_verify, g_palette, g_loop;
static unsigned g_capture_every;
static unsigned g_stats_s = 10;

// plahost GuestContext (tools/port/plahost/src/common.h): x[0..31] at 0 (x[31] = SP), fpcr at 848, fpsr at 856.
struct HostCtx {
    uint64_t x[32];
};
static inline uint64_t CtxX(pla_ctx* c, int n) { return reinterpret_cast<HostCtx*>(c)->x[n]; }
static inline uint64_t CtxFpcr(pla_ctx* c) { return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(c) + 848); }

// ------------------------------------------------------------------------------------------- memory models
struct MMFast {
    static inline uint8_t* P(uint64_t va) { return g_fm + (va & ((UINT64_C(1) << 39) - 1)); }
    static inline uint8_t* W(uint64_t va) { return P(va); }
};
struct Remap {
    uint64_t lo, hi;
    uint8_t* to;
};
static thread_local Remap t_remap[2];
static thread_local int t_nremap;
static thread_local uint64_t t_stack_lo, t_stack_hi;  // [sp - 64 KiB, sp): the island's own frames
static thread_local bool t_bad_store;
static thread_local uint8_t t_sink[64];
struct MMVerify {
    static inline uint8_t* P(uint64_t va) {
        for (int i = 0; i < t_nremap; ++i)
            if (va >= t_remap[i].lo && va < t_remap[i].hi) return t_remap[i].to + (va - t_remap[i].lo);
        return MMFast::P(va);
    }
    // Stores outside the shadowed outputs and the stack would reach real memory before the original runs:
    // they are diverted and flagged instead (an island path writing anything else fails verification).
    static inline uint8_t* W(uint64_t va) {
        for (int i = 0; i < t_nremap; ++i)
            if (va >= t_remap[i].lo && va < t_remap[i].hi) return t_remap[i].to + (va - t_remap[i].lo);
        if (va >= t_stack_lo && va < t_stack_hi) return MMFast::P(va);
        t_bad_store = true;
        return t_sink;
    }
};
#include "isl_core.h"

// ------------------------------------------------------------------------------------------- skeleton object
// Fields of the skeleton object read by 0x2A697AC (offsets from the disassembly).
enum : uint64_t {
    kParent = 0x100,    // s16 parent joint per joint (<0: root)
    kDirty = 0x138,     // u32 words: world matrix dirty bit per joint
    kSsc = 0x178,       // u32 words: segment-scale-compensation bit per joint
    kLocal = 0x270,     // 0x30 bytes per joint: local TRS (begin; end at +0x278)
    kWorld = 0x2A8,     // 0x40 bytes per joint: world matrix (begin; end at +0x2B0)
    kConstraint = 0x2E0 // s16 constraint index per joint (<0: none)
};

static inline bool Finite16(const uint8_t* p) {  // 4 floats finite
    const __m128i v = _mm_loadu_si128((const __m128i*)p);
    const __m128i e = _mm_and_si128(v, _mm_set1_epi32(0x7f800000));
    return _mm_movemask_epi8(_mm_cmpeq_epi32(e, _mm_set1_epi32(0x7f800000))) == 0;
}

enum Reason { R_OK, R_CONSTRAINT, R_NONFINITE, R_FPCR, R_BAIL, R_COUNT };
static const char* kReason[R_COUNT] = {"ok", "constraint", "nonfinite", "fpcr", "bail"};

// Checks the joints the island would visit (j and its dirty ancestors) before running it.
static Reason Precheck(uint64_t obj, int j) {
    const uint64_t par = ld64<MMFast>(obj + kParent), dirty = ld64<MMFast>(obj + kDirty);
    const uint64_t loc = ld64<MMFast>(obj + kLocal), wm = ld64<MMFast>(obj + kWorld);
    const uint64_t con = ld64<MMFast>(obj + kConstraint);
    for (int guard = 0; guard < 4096; ++guard) {
        if (!g_constraints && (int16_t)ld16<MMFast>(con + 2 * (uint64_t)(int64_t)j) >= 0) return R_CONSTRAINT;
        const uint8_t* l = MMFast::P(loc + 0x30 * (uint64_t)(int64_t)j);
        if (!Finite16(l) || !Finite16(l + 16) || !Finite16(l + 32)) return R_NONFINITE;
        const int p = (int16_t)ld16<MMFast>(par + 2 * (uint64_t)(int64_t)j);
        if (p < 0) return R_OK;
        const uint8_t* pl = MMFast::P(loc + 0x30 * (uint64_t)(int64_t)p);
        if (!Finite16(pl)) return R_NONFINITE;  // parent scale (segment scale compensation)
        const uint32_t w = ld32<MMFast>(dirty + (((uint64_t)(int64_t)p >> 3) & ~UINT64_C(3)));
        if (!(w & (1u << (p & 31)))) {
            const uint8_t* m = MMFast::P(wm + 0x40 * (uint64_t)(int64_t)p);
            return (Finite16(m) && Finite16(m + 16) && Finite16(m + 32) && Finite16(m + 48)) ? R_OK : R_NONFINITE;
        }
        j = p;
    }
    return R_BAIL;
}

// ------------------------------------------------------------------------------------------- stats
struct Counter {  // relaxed atomic counter
    volatile long long v;
    uint64_t fetch_add(uint64_t n) { return (uint64_t)_InterlockedExchangeAdd64(&v, (long long)n); }
    uint64_t load() const { return (uint64_t)v; }
};
static Counter s_calls, s_reason[R_COUNT], s_checked, s_mismatch, s_cycles, s_badstore, s_racy;
static Counter s_pal_calls, s_pal_checked, s_pal_mismatch, s_pal_bad, s_loop_calls, s_loop_deopt, s_decomp_calls;
static Counter s_next_report;

static void Logf(int level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    api->log(level, buf);
}

static void MaybeReport() {
    if (!g_stats_s) return;
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG next = s_next_report.load();
    if (now < next) return;
    if ((ULONGLONG)_InterlockedCompareExchange64(&s_next_report.v, (long long)(now + 1000ull * g_stats_s), (long long)next) != next)
        return;
    if (!next) return;  // first call only arms the timer
    Logf(PLA_LOG_INFO,
         "skel: calls %llu native %llu constraint %llu nonfinite %llu fpcr %llu bail %llu | verify checked %llu "
         "mismatch %llu badstore %llu racy %llu | native cycles/call %.0f | palette calls %llu checked %llu mismatch %llu bad %llu | loop calls %llu deopt %llu | decompose %llu",
         (unsigned long long)s_calls.load(), (unsigned long long)s_reason[R_OK].load(),
         (unsigned long long)s_reason[R_CONSTRAINT].load(), (unsigned long long)s_reason[R_NONFINITE].load(),
         (unsigned long long)s_reason[R_FPCR].load(), (unsigned long long)s_reason[R_BAIL].load(),
         (unsigned long long)s_checked.load(), (unsigned long long)s_mismatch.load(),
         (unsigned long long)s_badstore.load(), (unsigned long long)s_racy.load(),
         s_reason[R_OK].load() ? (double)s_cycles.load() / (double)s_reason[R_OK].load() : 0.0,
         (unsigned long long)s_pal_calls.load(), (unsigned long long)s_pal_checked.load(),
         (unsigned long long)s_pal_mismatch.load(), (unsigned long long)s_pal_bad.load(),
         (unsigned long long)s_loop_calls.load(), (unsigned long long)s_loop_deopt.load(),
         (unsigned long long)s_decomp_calls.load());
}

// ------------------------------------------------------------------------------------------- capture
// Record: "ISL1", u32 kind (0 sample, 1 mismatch), u64 main, obj, sp, fpcr, u32 j, n, then blobs
// (u32 tag, u64 va, u32 size, bytes): object header, parent, constraint, dirty (pre), ssc, local, world (pre),
// world (original result), dirty (original result), world (island result), dirty (island result).
static SRWLOCK s_cap_lock = SRWLOCK_INIT;
static FILE* s_cap;
static Counter s_cap_count;

// Growable byte buffer, reused and never freed (verify / capture modes only).
struct Bytes {
    uint8_t* p = nullptr;
    size_t n = 0, cap = 0;
    void resize(size_t m) {
        if (m > cap) {
            p = (uint8_t*)realloc(p, m);
            cap = m;
        }
        n = m;
    }
    void copy_from(uint64_t va, size_t m) {  // guest memory
        resize(m);
        if (m) memcpy(p, MMFast::P(va), m);
    }
    void assign(const Bytes& o) {
        resize(o.n);
        if (o.n) memcpy(p, o.p, o.n);
    }
    bool equals(const Bytes& o) const { return n == o.n && (!n || memcmp(p, o.p, n) == 0); }
    size_t size() const { return n; }
    uint8_t* data() const { return p; }
};

struct Snapshot {
    uint64_t obj = 0, sp = 0, fpcr = 0;
    int j = 0, n = 0;
    uint64_t wm = 0, dirty = 0;
    size_t wm_size = 0, dirty_size = 0;
    Bytes hdr, par, con, dirty_pre, ssc, loc, wm_pre, wm_isl, dirty_isl;
    int reason = 0;
};
static thread_local Snapshot t_snap;
static thread_local int t_depth;
static thread_local bool t_pending;

static void Blob(FILE* f, uint32_t tag, uint64_t va, const Bytes& b) {
    const uint32_t sz = (uint32_t)b.size();
    fwrite(&tag, 4, 1, f);
    fwrite(&va, 8, 1, f);
    fwrite(&sz, 4, 1, f);
    if (sz) fwrite(b.data(), 1, sz, f);
}
static Bytes s_cap_tmp;  // under s_cap_lock

static void WriteCapture(const Snapshot& s, uint32_t kind, const Bytes& wm_orig, const Bytes& dirty_orig) {
    AcquireSRWLockExclusive(&s_cap_lock);
    if (!s_cap) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* sl = strrchr(path, '\\');
        if (sl) strcpy(sl + 1, "islands-captures.bin");
        const char* env = getenv("PLA_ISLANDS_CAPTURE_FILE");
        s_cap = fopen(env && *env ? env : path, "ab");
    }
    if (s_cap) {
        fwrite("ISL1", 1, 4, s_cap);
        fwrite(&kind, 4, 1, s_cap);
        const uint64_t hdrv[4] = {g_main, s.obj, s.sp, s.fpcr};
        fwrite(hdrv, 8, 4, s_cap);
        const uint32_t jn[2] = {(uint32_t)s.j, (uint32_t)s.n};
        fwrite(jn, 4, 2, s_cap);
        const uint64_t par = ld64<MMFast>(s.obj + kParent), con = ld64<MMFast>(s.obj + kConstraint);
        const uint64_t ssc = ld64<MMFast>(s.obj + kSsc), loc = ld64<MMFast>(s.obj + kLocal);
        Blob(s_cap, 1, s.obj, s.hdr);
        Blob(s_cap, 2, par, s.par);
        Blob(s_cap, 3, con, s.con);
        Blob(s_cap, 4, s.dirty, s.dirty_pre);
        Blob(s_cap, 5, ssc, s.ssc);
        Blob(s_cap, 6, loc, s.loc);
        Blob(s_cap, 7, s.wm, s.wm_pre);
        Blob(s_cap, 8, s.wm, wm_orig);
        Blob(s_cap, 9, s.dirty, dirty_orig);
        Blob(s_cap, 10, s.wm, s.wm_isl);
        Blob(s_cap, 11, s.dirty, s.dirty_isl);
        int nc = 0;  // constraint structs (0xD0 each) and the 0x238 array (0x30 per joint)
        for (int i = 0; i < s.n; ++i) {
            const int c = (int)(int16_t)ld16<MMFast>(con + 2 * (uint64_t)i) + 1;
            if (c > nc) nc = c;
        }
        s_cap_tmp.copy_from(ld64<MMFast>(s.obj + 0x318), (size_t)nc * 0xD0);
        Blob(s_cap, 12, ld64<MMFast>(s.obj + 0x318), s_cap_tmp);
        s_cap_tmp.copy_from(ld64<MMFast>(s.obj + 0x238), (size_t)s.n * 0x30);
        Blob(s_cap, 13, ld64<MMFast>(s.obj + 0x238), s_cap_tmp);
        fflush(s_cap);
    }
    ReleaseSRWLockExclusive(&s_cap_lock);
}

// ------------------------------------------------------------------------------------------- hooks
static inline int RunNative(uint64_t obj, uint64_t j, uint64_t sp, bool verify, uint64_t fpcr) {
    g_fpcr = (uint32_t)fpcr;
    Regs R;
    R.x[0] = obj;
    R.x[1] = j;
    R.sp = sp;
    return verify ? fn_2a697ac<MMVerify>(R) : fn_2a697ac<MMFast>(R);
}

// FPCR.FZ -> MXCSR FTZ|DAZ for the island's duration.
struct FpScope {
    unsigned old;
    explicit FpScope(bool fz) : old(_mm_getcsr()) {
        unsigned m = (old & ~0x6000u) | 0x1F80u;  // RN, all exceptions masked
        m = fz ? (m | 0x8040u) : (m & ~0x8040u);
        _mm_setcsr(m);
    }
    ~FpScope() { _mm_setcsr(old); }
};

extern "C" PLA_MOD_EXPORT pla_action skel_joint_pre(pla_ctx* c) {
    if (!g_on) return PLA_CONTINUE;
    const uint64_t obj = CtxX(c, 0), j = CtxX(c, 1), sp = CtxX(c, 31);
    if (g_verify) {
        if (t_depth++ != 0) return PLA_CONTINUE;  // nested call of the original: post hook pops it
        s_calls.fetch_add(1);
        t_pending = false;
        const uint64_t fpcr = CtxFpcr(c);
        Reason r = (fpcr & ~(UINT64_C(1) << 24)) ? R_FPCR : Precheck(obj, (int16_t)j);
        if (r == R_OK) {
            Snapshot& s = t_snap;
            s.obj = obj; s.sp = sp; s.fpcr = fpcr; s.j = (int)(int16_t)j;
            const uint64_t lb = ld64<MMFast>(obj + kLocal), le = ld64<MMFast>(obj + kLocal + 8);
            s.n = (int)((le - lb) / 0x30);
            s.wm = ld64<MMFast>(obj + kWorld);
            s.wm_size = (size_t)(ld64<MMFast>(obj + kWorld + 8) - s.wm);
            s.dirty = ld64<MMFast>(obj + kDirty);
            const int nw = (int)(s.wm_size / 64);  // the world array can hold more matrices than local TRS
            s.dirty_size = (size_t)(((nw > s.n ? nw : s.n) + 31) / 32) * 4;
            s.dirty_pre.copy_from(s.dirty, s.dirty_size);
            s.wm_pre.copy_from(s.wm, s.wm_size);
            s.wm_isl.assign(s.wm_pre);
            s.dirty_isl.assign(s.dirty_pre);
            t_remap[0] = {s.wm, s.wm + s.wm_size, s.wm_isl.data()};
            t_remap[1] = {s.dirty, s.dirty + s.dirty_size, s.dirty_isl.data()};
            t_nremap = 2;
            t_stack_lo = sp - 0x10000;
            t_stack_hi = sp;
            t_bad_store = false;
            int rc;
            {
                FpScope fp((fpcr >> 24) & 1);
                rc = RunNative(obj, j, sp, true, fpcr);
            }
            t_nremap = 0;
            if (rc == NL_OK && t_bad_store) {
                s_badstore.fetch_add(1);
                rc = NL_BAIL;
            }
            if (rc == NL_OK) {
                t_pending = true;
                {
                    s.hdr.copy_from(obj, 0x480);
                    s.par.copy_from(ld64<MMFast>(obj + kParent), (size_t)s.n * 2);
                    s.con.copy_from(ld64<MMFast>(obj + kConstraint), (size_t)s.n * 2);
                    s.ssc.copy_from(ld64<MMFast>(obj + kSsc), s.dirty_size);
                    s.loc.copy_from(lb, (size_t)s.n * 0x30);
                }
            } else {
                r = R_BAIL;
            }
        }
        s_reason[r].fetch_add(1);
        MaybeReport();
        return PLA_CONTINUE;
    }
    s_calls.fetch_add(1);
    const uint64_t fpcr = CtxFpcr(c);
    if (fpcr & ~(UINT64_C(1) << 24)) {
        s_reason[R_FPCR].fetch_add(1);
        return PLA_CONTINUE;
    }
    const Reason r = Precheck(obj, (int16_t)j);
    if (r != R_OK) {
        s_reason[r].fetch_add(1);
        MaybeReport();
        return PLA_CONTINUE;
    }
    const uint64_t t0 = __rdtsc();
    int rc;
    {
        FpScope fp((fpcr >> 24) & 1);
        rc = RunNative(obj, j, sp, false, fpcr);
    }
    if (rc != NL_OK) {
        s_reason[R_BAIL].fetch_add(1);
        return PLA_CONTINUE;
    }
    s_cycles.fetch_add(__rdtsc() - t0);
    s_reason[R_OK].fetch_add(1);
    MaybeReport();
    return PLA_SKIP;
}

static thread_local Bytes t_post_wm, t_post_dirty, t_post_loc;

extern "C" PLA_MOD_EXPORT void skel_joint_post(pla_ctx*) {
    if (!g_verify) return;
    if (--t_depth != 0 || !t_pending) return;
    t_pending = false;
    const Snapshot& s = t_snap;
    Bytes& wm = t_post_wm;
    Bytes& dirty = t_post_dirty;
    wm.copy_from(s.wm, s.wm_size);
    dirty.copy_from(s.dirty, s.dirty_size);
    // Another thread writing the skeleton's local transforms (and dirty bits) while the original runs makes
    // the comparison meaningless: detected by the local array changing between pre and post (counted apart).
    t_post_loc.copy_from(ld64<MMFast>(s.obj + kLocal), (size_t)s.n * 0x30);
    if (!t_post_loc.equals(s.loc)) {
        s_racy.fetch_add(1);
        return;
    }
    const bool same = wm.equals(s.wm_isl) && dirty.equals(s.dirty_isl);
    const uint64_t k = s_checked.fetch_add(1);
    if (!same) {
        const uint64_t m = s_mismatch.fetch_add(1);
        if (m < 200) WriteCapture(s, 1, wm, dirty);
        if (m < 20) {
            size_t at = 0;
            while (at < wm.size() && at < s.wm_isl.size() && wm.p[at] == s.wm_isl.p[at]) ++at;
            uint32_t wo = 0, wi = 0;
            if (at + 4 <= wm.size() && at + 4 <= s.wm_isl.size()) {
                memcpy(&wo, wm.p + (at & ~(size_t)3), 4);
                memcpy(&wi, s.wm_isl.p + (at & ~(size_t)3), 4);
            }
            Logf(PLA_LOG_WARNING, "skel verify MISMATCH obj %#llx joint %d n %d world entries %zu: first differing "
                 "world byte %zu (entry %zu) original %08x island %08x; dirty %s",
                 (unsigned long long)s.obj, s.j, s.n, wm.size() / 64, at, at / 64, wo, wi,
                 dirty.equals(s.dirty_isl) ? "same" : "differs");
        }
    } else if (g_capture_every && k % g_capture_every == 0 && s_cap_count.fetch_add(1) < 20000) {
        WriteCapture(s, 0, wm, dirty);
    }
}

// ------------------------------------------------------------------------------------------- update-all loop
// Region main+0x2A6BAF4..0x2A6BB60 (inside 0x2A6BA60, lock held): for every dirty joint, the joint island is
// called directly (no hook dispatch per joint); a joint whose precheck fails or whose island bails leaves the
// region into the recompiled 0x2A697AC with the full state (x30 = loop continuation).
template <class MM> static bool skel_guard(Regs& R) {
    const Reason r = Precheck(R.x[0], (int16_t)R.x[1]);
    s_calls.fetch_add(1);
    s_reason[r].fetch_add(1);
    return r == R_OK;
}

extern "C" PLA_MOD_EXPORT pla_action skel_loop_pre(pla_ctx* ctx) {
    if (!g_loop || g_verify) return PLA_CONTINUE;
    const uint64_t fpcr = CtxFpcr(ctx);
    if (fpcr & ~(UINT64_C(1) << 24)) return PLA_CONTINUE;
    GC* c = reinterpret_cast<GC*>(ctx);
    s_loop_calls.fetch_add(1);
    int rc;
    {
        FpScope fp((fpcr >> 24) & 1);
        g_fpcr = (uint32_t)fpcr;
        rc = fn_2a6baf4<MMFast>(c);
    }
    if (rc != NL_EXIT) return PLA_CONTINUE;
    if (c->pc != g_main + 0x2A6BB60) s_loop_deopt.fetch_add(1);
    MaybeReport();
    return PLA_REDIRECT;
}

// ------------------------------------------------------------------------------------------- leaf regions
// Leaf functions lifted as regions: entered at their first instruction with the full context, left at their ret
// (PLA_REDIRECT to x30). main+0x2A3781C: 4x4 matrix -> scale / rotation decomposition.
static bool g_decomp;
extern "C" PLA_MOD_EXPORT pla_action mtx_decompose_pre(pla_ctx* ctx) {
    if (!g_decomp || g_verify) return PLA_CONTINUE;
    const uint64_t fpcr = CtxFpcr(ctx);
    if (fpcr & ~(UINT64_C(1) << 24)) return PLA_CONTINUE;
    s_decomp_calls.fetch_add(1);
    int rc;
    {
        FpScope fp((fpcr >> 24) & 1);
        g_fpcr = (uint32_t)fpcr;
        rc = fn_2a3781c<MMFast>(reinterpret_cast<GC*>(ctx));
    }
    return rc == NL_EXIT ? PLA_REDIRECT : PLA_CONTINUE;
}

// ------------------------------------------------------------------------------------------- skin palette
// Region main+0x29C98BC..0x29C995C (inside 0x29C983C): for i < count (w21): idx = s16 x19[i]; if idx >= 0:
// out(x0)[idx] = B(x20 table)[i] x A(x22 table)[i]. Entered after the palette buffer lookup returned (x0).
static thread_local bool t_pal_pending;
static thread_local uint64_t t_pal_lo, t_pal_hi;
static thread_local Bytes t_pal_shadow, t_pal_real;

static bool PaletteRange(const GC* c, uint64_t& lo, uint64_t& hi) {
    const uint64_t out = c->x[0], idx = c->x[19];
    const unsigned n = (unsigned)(c->x[21] & 0xffff);
    if (!out || (int16_t)n < 1) return false;
    int mx = -1;
    for (unsigned i = 0; i < n; ++i) {
        const int v = (int)(int16_t)ld16<MMFast>(idx + 2 * i);
        if (v > mx) mx = v;
    }
    if (mx < 0) return false;
    lo = out;
    hi = out + 64 * (uint64_t)(mx + 1);
    return true;
}

extern "C" PLA_MOD_EXPORT pla_action palette_pre(pla_ctx* ctx) {
    if (!g_palette) return PLA_CONTINUE;
    GC* c = reinterpret_cast<GC*>(ctx);
    const uint64_t fpcr = CtxFpcr(ctx);
    if (fpcr & ~(UINT64_C(1) << 24)) return PLA_CONTINUE;
    s_pal_calls.fetch_add(1);
    if (g_verify) {
        uint64_t lo, hi;
        t_pal_pending = false;
        if (!PaletteRange(c, lo, hi)) return PLA_CONTINUE;
        GC copy = *c;
        t_pal_shadow.copy_from(lo, (size_t)(hi - lo));
        t_remap[0] = {lo, hi, t_pal_shadow.data()};
        t_nremap = 1;
        t_stack_lo = c->x[31] - 0x10000;
        t_stack_hi = c->x[31];
        t_bad_store = false;
        int rc;
        {
            FpScope fp((fpcr >> 24) & 1);
            g_fpcr = (uint32_t)fpcr;
            rc = fn_29c98bc<MMVerify>(&copy);
        }
        t_nremap = 0;
        if (t_bad_store || rc != NL_EXIT || copy.pc != g_main + 0x29C995C) {
            s_pal_bad.fetch_add(1);
            return PLA_CONTINUE;
        }
        t_pal_lo = lo;
        t_pal_hi = hi;
        t_pal_pending = true;
        return PLA_CONTINUE;
    }
    int rc;
    {
        FpScope fp((fpcr >> 24) & 1);
        g_fpcr = (uint32_t)fpcr;
        rc = fn_29c98bc<MMFast>(c);
    }
    return rc == NL_EXIT ? PLA_REDIRECT : PLA_CONTINUE;
}

// Verify only: the epilogue block of 0x29C983C, reached by the original loop.
extern "C" PLA_MOD_EXPORT pla_action palette_exit_pre(pla_ctx*) {
    if (!t_pal_pending) return PLA_CONTINUE;
    t_pal_pending = false;
    Bytes& real = t_pal_real;
    real.copy_from(t_pal_lo, (size_t)(t_pal_hi - t_pal_lo));
    s_pal_checked.fetch_add(1);
    if (!real.equals(t_pal_shadow)) {
        const uint64_t m = s_pal_mismatch.fetch_add(1);
        if (m < 20) {
            size_t at = 0;
            while (at < real.size() && at < t_pal_shadow.size() && real.p[at] == t_pal_shadow.p[at]) ++at;
            Logf(PLA_LOG_WARNING, "palette verify MISMATCH out %#llx byte %zu", (unsigned long long)t_pal_lo, at);
        }
    }
    return PLA_CONTINUE;
}

// ------------------------------------------------------------------------------------------- init
static bool CfgFlag(const char* json, const char* key, const char* env, bool def) {
    const char* e = getenv(env);
    if (e && *e) return atoi(e) != 0;
    const char* p = json ? strstr(json, key) : nullptr;
    if (!p) return def;
    p = strchr(p + strlen(key), ':');
    if (!p) return def;
    while (*++p == ' ' || *p == '"') {}
    return *p == 't' || *p == '1' || *p == 'o';  // true / 1 / on
}
static unsigned CfgNum(const char* json, const char* key, const char* env, unsigned def) {
    const char* e = getenv(env);
    if (e && *e) return (unsigned)atoi(e);
    const char* p = json ? strstr(json, key) : nullptr;
    if (!p) return def;
    p = strchr(p + strlen(key), ':');
    if (!p) return def;
    while (*++p == ' ' || *p == '"') {}
    return (unsigned)atoi(p);
}

extern "C" PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

extern "C" PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* cfg) {
    api = a;
    g_on = CfgFlag(cfg, "\"skeleton\"", "PLA_ISLANDS_SKEL", true);
    g_verify = CfgFlag(cfg, "\"verify\"", "PLA_ISLANDS_VERIFY", false);
    g_capture_every = CfgNum(cfg, "\"capture\"", "PLA_ISLANDS_CAPTURE", 0);
    g_stats_s = CfgNum(cfg, "\"stats_s\"", "PLA_ISLANDS_STATS", 10);
    g_decomp = CfgFlag(cfg, "\"decompose\"", "PLA_ISLANDS_DECOMPOSE", true);
    g_loop = CfgFlag(cfg, "\"update_all\"", "PLA_ISLANDS_LOOP", true);
    g_palette = CfgFlag(cfg, "\"palette\"", "PLA_ISLANDS_PALETTE", true);
    g_constraints = CfgFlag(cfg, "\"constraints\"", "PLA_ISLANDS_CONSTRAINTS", false);
    int r[4];
    __cpuid(r, 1);
    const bool fma = (r[2] >> 12) & 1, sse41 = (r[2] >> 19) & 1;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\');
    const bool plahost = !PLA_API_HAS(a, guest_free) ? false : (base && _stricmp(base + 1, "PLA.exe") == 0);
    g_main = a->module_base("main");
    uint8_t* hp = g_main ? (uint8_t*)a->host_pointer(g_main, 4096) : nullptr;
    g_fm = hp ? hp - g_main : nullptr;
    if (!fma || !sse41 || !plahost || !g_fm) {
        Logf(PLA_LOG_WARNING, "islands inert (fma %d sse4.1 %d plahost %d fm %p)", fma, sse41, plahost, (void*)g_fm);
        g_on = false;
        g_palette = false;
        g_loop = false;
        g_decomp = false;
    }
    Logf(PLA_LOG_INFO, "islands: skeleton %s%s%s, palette %d, update_all loop %d, capture every %u, stats %u s (main %#llx)", g_on ? "on" : "off",
         g_verify ? " VERIFY" : "", g_constraints ? " +constraints" : "", (int)g_palette, (int)g_loop, g_capture_every, g_stats_s,
         (unsigned long long)g_main);
    return 0;
}
