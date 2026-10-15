/* recomp_isa_check() for DLLs built with build_modules.py --isa x86-64-v2/v3 (see recomp_crt0.c).
 * Compiled WITHOUT -march (only -DRECOMP_ISA_LEVEL=2 or 3), so the check itself runs on any x86-64 CPU.
 * Same feature sets as cpu_isa.py / the x86-64 psABI levels. Written from scratch. */
#include <intrin.h>

#ifndef RECOMP_ISA_LEVEL
#define RECOMP_ISA_LEVEL 1
#endif

static int bit(int r, int n) { return (int)(((unsigned)r >> n) & 1u); }

int recomp_isa_check(void)
{
    int r1[4], r7[4] = {0, 0, 0, 0}, r81[4] = {0, 0, 0, 0}, r0[4], r80[4];
    __cpuid(r0, 0);
    __cpuid(r1, 1);
    if (r0[0] >= 7) __cpuidex(r7, 7, 0);
    __cpuid(r80, (int)0x80000000);
    if ((unsigned)r80[0] >= 0x80000001u) __cpuid(r81, (int)0x80000001);
#if RECOMP_ISA_LEVEL >= 2
    /* cx16, lahf, popcnt, sse3, sse4.1, sse4.2, ssse3 */
    if (!(bit(r1[2], 13) && bit(r81[2], 0) && bit(r1[2], 23) && bit(r1[2], 0) && bit(r1[2], 19) &&
          bit(r1[2], 20) && bit(r1[2], 9)))
        return 0;
#endif
#if RECOMP_ISA_LEVEL >= 3
    /* avx, avx2, bmi1, bmi2, f16c, fma, lzcnt, movbe, xsave + the OS saving the YMM state */
    if (!(bit(r1[2], 27) && bit(r1[2], 28) && bit(r7[1], 5) && bit(r7[1], 3) && bit(r7[1], 8) &&
          bit(r1[2], 29) && bit(r1[2], 12) && bit(r81[2], 5) && bit(r1[2], 22) && bit(r1[2], 26)))
        return 0;
    if ((_xgetbv(0) & 6) != 6) return 0;
#endif
    return 1;
}
