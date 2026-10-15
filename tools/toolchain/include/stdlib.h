/* stdlib.h minimal (port's portable toolchain); see _tc_crt.h. */
#ifndef TC_STDLIB_H
#define TC_STDLIB_H
#include "_tc_crt.h"
_TC_BEGIN
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
_TC_CRTIMP void* __cdecl malloc(size_t n);
_TC_CRTIMP void* __cdecl calloc(size_t n, size_t sz);
_TC_CRTIMP void* __cdecl realloc(void* p, size_t n);
_TC_CRTIMP void __cdecl free(void* p);
_TC_CRTIMP __declspec(noreturn) void __cdecl abort(void);
_TC_CRTIMP __declspec(noreturn) void __cdecl exit(int code);
_TC_CRTIMP char* __cdecl getenv(const char* name);
_TC_CRTIMP int __cdecl atoi(const char* s);
_TC_CRTIMP long long __cdecl strtoll(const char* s, char** end, int base);
_TC_CRTIMP unsigned long long __cdecl strtoull(const char* s, char** end, int base);
_TC_END
#endif
