/* malloc.h minimal (port's portable toolchain); required by clang's mm_malloc.h. */
#ifndef TC_MALLOC_H
#define TC_MALLOC_H
#include "_tc_crt.h"
_TC_BEGIN
_TC_CRTIMP void* __cdecl _aligned_malloc(size_t n, size_t align);
_TC_CRTIMP void __cdecl _aligned_free(void* p);
#define _alloca(n) __builtin_alloca(n)
_TC_END
#endif
