/* Implicit TLS for DLLs built with the portable toolchain that use thread_local / __declspec(thread)
 * (e.g. the islands mod, tools/islands/build_islands.py). Replaces the TLS support of the MSVC CRT (tlssup.obj in
 * msvcrt.lib): the TLS directory that the loader reads, the bounds of the .tls template and the TLS index.
 * Only variables with constant initializers are supported (no dynamic TLS initializers, no thread_local
 * destructors: that would need the CRT's TLS callbacks). The recompiled_*.dll do not link it.
 * Written from scratch from the documented PE format (IMAGE_TLS_DIRECTORY64). Compiled with /GS- /Zl. */
#include <stdint.h>

typedef struct {
    uint64_t StartAddressOfRawData;
    uint64_t EndAddressOfRawData;
    uint64_t AddressOfIndex;
    uint64_t AddressOfCallBacks;
    uint32_t SizeOfZeroFill;
    uint32_t Characteristics;
} tc_tls_directory;

/* The linker sorts the sections by name: .tls (start) < .tls$ (the variables) < .tls$ZZZ (end). */
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;

/* Empty, null-terminated list of TLS callbacks (.CRT$XLA < .CRT$XLB.. < .CRT$XLZ). */
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLZ", long, read)
__declspec(allocate(".CRT$XLA")) const void* __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) const void* __xl_z = 0;

unsigned long _tls_index = 0;

/* lld-link fills the TLS data directory of the image from the symbol _tls_used. */
#pragma section(".rdata$T", long, read)
__declspec(allocate(".rdata$T")) const tc_tls_directory _tls_used = {
    (uint64_t)&_tls_start, (uint64_t)&_tls_end, (uint64_t)&_tls_index, (uint64_t)(&__xl_a + 1), 0, 0};
