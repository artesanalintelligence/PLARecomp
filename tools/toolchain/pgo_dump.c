/* PGO training builds only (tools/toolchain/pgo_train.py; docs/port/LAUNCHER.md 8.12). plahost ends with
   TerminateProcess, so the profile runtime's atexit writer never runs. A thread snapshots the counters every
   RECOMP_PGO_DUMP_S seconds (default 20) with the runtime's buffer API and writes them with Win32 calls (no CRT
   stdio), alternating <LLVM_PROFILE_FILE>.<image>.0 / .1 so a kill mid-write leaves the other one intact. The host may also call the export recomp_pgo_dump_v1(path) on its exit path. */
#include <windows.h>
#include <stdint.h>
uint64_t __llvm_profile_get_size_for_buffer(void);
int __llvm_profile_write_buffer(char*);
static wchar_t g_name[2][600];
static DWORD g_period = 20;
static SRWLOCK g_lock = SRWLOCK_INIT;

__declspec(dllexport) int recomp_pgo_dump_v1(const wchar_t* path) {
    AcquireSRWLockExclusive(&g_lock);
    int rc = 0;
    uint64_t n = __llvm_profile_get_size_for_buffer();
    char* b = n ? (char*)VirtualAlloc(0, (SIZE_T)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : 0;
    if (!b) rc = -1;
    else if (__llvm_profile_write_buffer(b)) rc = -2;
    else {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (h == INVALID_HANDLE_VALUE) rc = -3;
        else {
            uint64_t off = 0;
            while (off < n) {
                DWORD chunk = (DWORD)((n - off) > (1u << 30) ? (1u << 30) : (n - off)), w = 0;
                if (!WriteFile(h, b + off, chunk, &w, 0) || w != chunk) { rc = -4; break; }
                off += chunk;
            }
            CloseHandle(h);
        }
    }
    if (b) VirtualFree(b, 0, MEM_RELEASE);
    ReleaseSRWLockExclusive(&g_lock);
    return rc;
}

static DWORD WINAPI dump_loop(void* p) {
    unsigned i = 0;
    (void)p;
    for (;;) {
        Sleep(g_period * 1000);
        recomp_pgo_dump_v1(g_name[i & 1]);
        ++i;
    }
}

static void start(void);
static void start(void) {
    wchar_t b[32], mod[MAX_PATH];
    HMODULE self = 0;
    DWORD n = GetEnvironmentVariableW(L"LLVM_PROFILE_FILE", g_name[0], 400);
    if (n == 0 || n >= 400) return;
    /* one profile per instrumented image: <LLVM_PROFILE_FILE>.<image base name>.0/.1 */
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&start, &self)
        && GetModuleFileNameW(self, mod, MAX_PATH)) {
        wchar_t* base = mod;
        for (wchar_t* c = mod; *c; ++c) if (*c == L'\\' || *c == L'/') base = c + 1;
        for (wchar_t* c = base; *c && *c != L'.' && n < 560; ++c) { if (c == base) g_name[0][n++] = L'.'; g_name[0][n++] = *c; }
        g_name[0][n] = 0;
    }
    for (DWORD k = 0; k <= n; ++k) g_name[1][k] = g_name[0][k];
    g_name[0][n] = L'.'; g_name[0][n + 1] = L'0'; g_name[0][n + 2] = 0;
    g_name[1][n] = L'.'; g_name[1][n + 1] = L'1'; g_name[1][n + 2] = 0;
    if (GetEnvironmentVariableW(L"RECOMP_PGO_DUMP_S", b, 32)) {
        DWORD v = 0;
        for (wchar_t* c = b; *c >= L'0' && *c <= L'9'; ++c) v = v * 10 + (DWORD)(*c - L'0');
        if (v) g_period = v;
    }
    CreateThread(0, 0, dump_loop, 0, 0, 0);
}
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) __attribute__((used)) void (*recomp_pgo_dump_start)(void) = start;
#pragma comment(linker, "/include:recomp_pgo_dump_start")

