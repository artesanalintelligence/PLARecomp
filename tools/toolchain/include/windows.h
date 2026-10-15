/* windows.h minimal (port's portable toolchain); see _tc_crt.h.
 * Only the types, constants and kernel32 functions used by the emitted C (and the islands mod). Interlocked*
 * and MemoryBarrier map to the intrinsics, as winnt.h does on x64. */
#ifndef TC_WINDOWS_H
#define TC_WINDOWS_H
#include "_tc_crt.h"
#include <intrin.h>
_TC_BEGIN
#define WINAPI __stdcall
#define DECLSPEC_IMPORT __declspec(dllimport)
#define WINBASEAPI DECLSPEC_IMPORT
#define TRUE 1
#define FALSE 0
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long ULONG_PTR, SIZE_T, DWORD_PTR;
typedef long long LONG_PTR;
typedef void* LPVOID;
typedef void* PVOID;
typedef void* HANDLE;
typedef const char* LPCSTR;
typedef char* LPSTR;
typedef struct HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;
typedef long long (WINAPI* FARPROC)(void);
#define GET_MODULE_HANDLE_EX_FLAG_PIN                0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define MAX_PATH 260
typedef struct _RTL_SRWLOCK { PVOID Ptr; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT {0}
WINBASEAPI HANDLE WINAPI GetCurrentProcess(void);
WINBASEAPI DWORD WINAPI GetCurrentProcessId(void);
WINBASEAPI DWORD WINAPI GetCurrentThreadId(void);
WINBASEAPI BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* mod);
WINBASEAPI HMODULE WINAPI GetModuleHandleA(LPCSTR name);
WINBASEAPI FARPROC WINAPI GetProcAddress(HMODULE mod, LPCSTR name);
WINBASEAPI void WINAPI Sleep(DWORD ms);
WINBASEAPI BOOL WINAPI DisableThreadLibraryCalls(HMODULE mod);
WINBASEAPI BOOL WINAPI QueryPerformanceCounter(LONGLONG* count);
WINBASEAPI BOOL WINAPI QueryPerformanceFrequency(LONGLONG* freq);
WINBASEAPI void WINAPI OutputDebugStringA(LPCSTR s);
WINBASEAPI DWORD WINAPI GetModuleFileNameA(HMODULE mod, LPSTR path, DWORD size);
WINBASEAPI ULONGLONG WINAPI GetTickCount64(void);
WINBASEAPI void WINAPI AcquireSRWLockExclusive(PSRWLOCK lock);
WINBASEAPI void WINAPI ReleaseSRWLockExclusive(PSRWLOCK lock);
#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchange _InterlockedExchange
#define InterlockedExchangeAdd _InterlockedExchangeAdd
#define InterlockedCompareExchange _InterlockedCompareExchange
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#define InterlockedExchange64 _InterlockedExchange64
#define InterlockedCompareExchangePointer _InterlockedCompareExchangePointer
#define MemoryBarrier __faststorefence
#define YieldProcessor _mm_pause
_TC_END
#endif
