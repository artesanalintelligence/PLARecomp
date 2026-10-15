/* math.h minimal (port's portable toolchain); see _tc_crt.h.
 * The emitted C mostly uses __builtin_*; these declarations only exist in case a unit
 * calls a function by name. */
#ifndef TC_MATH_H
#define TC_MATH_H
#include "_tc_crt.h"
_TC_BEGIN
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#ifndef INFINITY
#define INFINITY  __builtin_inff()
#endif
#ifndef NAN
#define NAN       __builtin_nanf("")
#endif
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
/* As in the UCRT: the classic double functions have no dllimport (they are intrinsics);
   the C99 ones and the x64 float variants do; fabsf is inline (ucrtbase does not export it on x64). */
#define TC_D(n) double __cdecl n(double);
#define TC_I(n) _TC_CRTIMP double __cdecl n(double); _TC_CRTIMP float __cdecl n##f(float);
TC_D(fabs) TC_D(sqrt) TC_D(floor) TC_D(ceil) TC_D(exp) TC_D(log)
TC_D(sin) TC_D(cos) TC_D(tan) TC_D(atan)
_TC_CRTIMP float __cdecl sqrtf(float); _TC_CRTIMP float __cdecl floorf(float);
_TC_CRTIMP float __cdecl ceilf(float); _TC_CRTIMP float __cdecl expf(float);
_TC_CRTIMP float __cdecl logf(float); _TC_CRTIMP float __cdecl sinf(float);
_TC_CRTIMP float __cdecl cosf(float); _TC_CRTIMP float __cdecl tanf(float);
_TC_CRTIMP float __cdecl atanf(float);
TC_I(trunc) TC_I(round) TC_I(rint) TC_I(nearbyint)
#undef TC_D
#undef TC_I
static __inline float __cdecl fabsf(float x) { return (float)fabs(x); }
double __cdecl fmod(double, double);
double __cdecl pow(double, double);
double __cdecl ldexp(double, int);
double __cdecl frexp(double, int*);
_TC_CRTIMP float __cdecl fmodf(float, float);
_TC_CRTIMP float __cdecl powf(float, float);
_TC_CRTIMP double __cdecl fma(double, double, double);
_TC_CRTIMP float __cdecl fmaf(float, float, float);
_TC_CRTIMP double __cdecl fmin(double, double);
_TC_CRTIMP float __cdecl fminf(float, float);
_TC_CRTIMP double __cdecl fmax(double, double);
_TC_CRTIMP float __cdecl fmaxf(float, float);
_TC_CRTIMP double __cdecl copysign(double, double);
_TC_CRTIMP float __cdecl copysignf(float, float);
_TC_END
#endif
