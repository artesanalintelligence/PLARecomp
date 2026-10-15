/* stdio.h minimal (port's portable toolchain); see _tc_crt.h.
 * As in the UCRT, printf and friends are inline functions over
 * __stdio_common_vfprintf/__stdio_common_vsprintf (local options = 0). */
#ifndef TC_STDIO_H
#define TC_STDIO_H
#include "_tc_crt.h"
#include <stdarg.h>
_TC_BEGIN
typedef struct _iobuf { void* _Placeholder; } FILE;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
_TC_CRTIMP FILE* __cdecl __acrt_iob_func(unsigned idx);
#define stdin  (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
_TC_CRTIMP int __cdecl __stdio_common_vfprintf(unsigned long long opts, FILE* f, const char* fmt, void* loc, va_list ap);
_TC_CRTIMP int __cdecl __stdio_common_vsprintf(unsigned long long opts, char* buf, size_t n, const char* fmt, void* loc, va_list ap);
_TC_CRTIMP FILE* __cdecl fopen(const char* path, const char* mode);
_TC_CRTIMP int __cdecl fclose(FILE* f);
_TC_CRTIMP size_t __cdecl fread(void* p, size_t sz, size_t n, FILE* f);
_TC_CRTIMP size_t __cdecl fwrite(const void* p, size_t sz, size_t n, FILE* f);
_TC_CRTIMP int __cdecl fseek(FILE* f, long off, int whence);
_TC_CRTIMP long __cdecl ftell(FILE* f);
_TC_CRTIMP int __cdecl fflush(FILE* f);
_TC_CRTIMP int __cdecl fputs(const char* s, FILE* f);
_TC_CRTIMP int __cdecl fputc(int c, FILE* f);
_TC_CRTIMP int __cdecl puts(const char* s);
_TC_CRTIMP int __cdecl remove(const char* path);
static __inline int __cdecl vfprintf(FILE* f, const char* fmt, va_list ap)
{ return __stdio_common_vfprintf(0, f, fmt, 0, ap); }
static __inline int __cdecl fprintf(FILE* f, const char* fmt, ...)
{ int r; va_list ap; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
static __inline int __cdecl vprintf(const char* fmt, va_list ap)
{ return vfprintf(stdout, fmt, ap); }
static __inline int __cdecl printf(const char* fmt, ...)
{ int r; va_list ap; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }
/* 2 = standard snprintf behavior (truncates, NUL-terminates and returns the full length). */
static __inline int __cdecl vsnprintf(char* buf, size_t n, const char* fmt, va_list ap)
{ int r = __stdio_common_vsprintf(2, buf, n, fmt, 0, ap); return r < 0 ? -1 : r; }
static __inline int __cdecl snprintf(char* buf, size_t n, const char* fmt, ...)
{ int r; va_list ap; va_start(ap, fmt); r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
_TC_END
#endif
