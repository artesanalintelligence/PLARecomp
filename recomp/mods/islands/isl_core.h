// isl_core.h - shared by the islands mod and the offline replay harness: guest memory accessors on a memory
// model MM (MM::P(va) = host address), the call interface of lifted functions, and the generated islands.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "nl_rt.h"
using nl::V;
extern uint64_t g_main;
extern thread_local uint32_t g_fpcr;  // guest FPCR of the running island (FRSQRTE)
template <class MM> static inline uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, MM::P(a), 8); return v; }
template <class MM> static inline uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, MM::P(a), 4); return v; }
template <class MM> static inline uint16_t ld16(uint64_t a) { uint16_t v; memcpy(&v, MM::P(a), 2); return v; }
template <class MM> static inline uint8_t ld8(uint64_t a) { return *MM::P(a); }
template <class MM> static inline void st64(uint64_t a, uint64_t v) { memcpy(MM::W(a), &v, 8); }
template <class MM> static inline void st32(uint64_t a, uint64_t v) { uint32_t w = (uint32_t)v; memcpy(MM::W(a), &w, 4); }
template <class MM> static inline void st16(uint64_t a, uint64_t v) { uint16_t w = (uint16_t)v; memcpy(MM::W(a), &w, 2); }
template <class MM> static inline void st8(uint64_t a, uint64_t v) { *MM::W(a) = (uint8_t)v; }
template <class MM> static inline V ldv(uint64_t a, int sz) {
    const uint8_t* p = MM::P(a);
    switch (sz) {
    case 16: return _mm_loadu_si128((const __m128i*)p);
    case 8: return _mm_loadl_epi64((const __m128i*)p);
    case 4: { uint32_t w; memcpy(&w, p, 4); return _mm_cvtsi32_si128((int)w); }
    default: { uint16_t w; memcpy(&w, p, 2); return _mm_cvtsi32_si128(w); }
    }
}
template <class MM> static inline void stv(uint64_t a, V v, int sz) {
    uint8_t* p = MM::W(a);
    switch (sz) {
    case 16: _mm_storeu_si128((__m128i*)p, v); break;
    case 8: _mm_storel_epi64((__m128i*)p, v); break;
    case 4: { uint32_t w = (uint32_t)_mm_cvtsi128_si32(v); memcpy(p, &w, 4); break; }
    default: { uint16_t w = (uint16_t)_mm_cvtsi128_si32(v); memcpy(p, &w, 2); break; }
    }
}

// Prefix of plahost's GuestContext (tools/port/plahost/src/common.h): regions read and write it directly.
struct GC {
    uint64_t x[32];  // x[31] = SP
    uint64_t pc;
    uint8_t n, z, c, v;
    uint8_t pad0[44];
    uint64_t vreg[32][2];
};
static_assert(offsetof(GC, pc) == 256, "GuestContext.pc");
static_assert(offsetof(GC, vreg) == 312, "GuestContext.vreg");
struct Regs {
    uint64_t x[8];
    V v[8];
    uint64_t sp;
};
enum { NL_OK = 0, NL_BAIL = 1, NL_EXIT = 2 };
#define NL_INLINE static inline __attribute__((always_inline))
#define NL_NOINLINE static __attribute__((noinline))

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#endif
#ifndef NL_TRACE
#define NL_TRACE(pc) ((void)0)
#endif
template <class MM> static bool skel_guard(Regs& R);  // defined by each user of the islands
#include "islands_gen.inc"
