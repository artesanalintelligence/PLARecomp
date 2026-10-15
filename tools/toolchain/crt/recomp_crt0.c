/* Minimal startup for the recompiled_*.dll built with the portable toolchain (tools/toolchain).
 *
 * Replaces the static part of the MSVC CRT (msvcrt.lib: _DllMainCRTStartup, the /GS cookie,
 * _fltused, __chkstk) that the VS Build Tools build links in. The emitted C has no
 * constructors, atexit or TLS, so this is enough:
 *   - _DllMainCRTStartup: initializes the /GS cookie and disables thread notifications
 *     (the same as the CRT's default DllMain).
 *   - __security_cookie / __security_check_cookie for code compiled with /GS.
 *   - _fltused, referenced by the compiler as soon as there is floating point.
 *   - __chkstk, the stack probe for frames larger than 4 KiB.
 * Compiled with /GS- (the check itself cannot carry a cookie). Written from scratch. */
#include <stdint.h>
#include <intrin.h>

typedef struct HINSTANCE__* HINSTANCE;
__declspec(dllimport) int __stdcall DisableThreadLibraryCalls(HINSTANCE mod);
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(uint64_t* ft);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(int64_t* c);

#define TC_DEFAULT_COOKIE 0x00002B992DDFA232ULL

uintptr_t __security_cookie = TC_DEFAULT_COOKIE;
uintptr_t __security_cookie_complement = ~(uintptr_t)TC_DEFAULT_COOKIE;
int _fltused = 0x9875;

static void tc_init_cookie(void)
{
    uint64_t ft = 0;
    int64_t qpc = 0;
    uintptr_t c;
    if (__security_cookie != TC_DEFAULT_COOKIE) return;
    GetSystemTimeAsFileTime(&ft);
    QueryPerformanceCounter(&qpc);
    c = (uintptr_t)ft ^ ((uintptr_t)GetCurrentThreadId() << 32) ^ GetCurrentProcessId() ^
        ((uintptr_t)qpc << 32) ^ (uintptr_t)qpc ^ (uintptr_t)&c ^ __rdtsc();
    c &= 0x0000FFFFFFFFFFFFULL; /* like the CRT: top 16 bits cleared */
    if (c == TC_DEFAULT_COOKIE || c == 0) c = TC_DEFAULT_COOKIE + 1;
    __security_cookie = c;
    __security_cookie_complement = ~c;
}

__declspec(noinline) void __cdecl __security_check_cookie(uintptr_t cookie)
{
    if (cookie != __security_cookie) __fastfail(2); /* FAST_FAIL_STACK_COOKIE_CHECK_FAILURE */
}

/* CPU level check. A DLL built for x86-64-v2/v3 (build_modules.py --isa) links recomp_isa_guard.obj, whose
 * recomp_isa_check() returns 0 on a CPU without the instructions the code uses: the DLL then refuses to load
 * (LoadLibrary fails with ERROR_DLL_INIT_FAILED, 1114) instead of crashing later on an illegal instruction, e.g.
 * when an install folder is copied to an older PC. Without the guard the alternate name below links a check
 * that always passes. */
int recomp_isa_check(void);
int recomp_isa_check_none(void) { return 1; }
#pragma comment(linker, "/alternatename:recomp_isa_check=recomp_isa_check_none")

int __stdcall _DllMainCRTStartup(HINSTANCE mod, unsigned long reason, void* reserved)
{
    (void)reserved;
    if (reason == 1 /* DLL_PROCESS_ATTACH */) {
        if (!recomp_isa_check()) return 0;
        tc_init_cookie();
        DisableThreadLibraryCalls(mod);
    }
    return 1;
}

/* __chkstk (x64): RAX = bytes the caller is about to reserve. Touches every 4 KiB page below
 * RSP so the guard page grows in order; does not change RSP (the caller does "sub rsp, rax")
 * and preserves all registers. */
__attribute__((naked)) void __chkstk(void)
{
    __asm__ volatile(
        "pushq %rcx\n\t"
        "pushq %rax\n\t"
        "leaq 24(%rsp), %rcx\n\t"      /* caller's RSP before the call */
        "cmpq $0x1000, %rax\n\t"
        "jb 2f\n"
        "1:\n\t"
        "subq $0x1000, %rcx\n\t"
        "testq %rcx, (%rcx)\n\t"
        "subq $0x1000, %rax\n\t"
        "cmpq $0x1000, %rax\n\t"
        "ja 1b\n"
        "2:\n\t"
        "subq %rax, %rcx\n\t"
        "testq %rcx, (%rcx)\n\t"
        "popq %rax\n\t"
        "popq %rcx\n\t"
        "ret");
}
