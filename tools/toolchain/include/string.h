/* string.h minimal (port's portable toolchain); see _tc_crt.h. */
#ifndef TC_STRING_H
#define TC_STRING_H
#include "_tc_crt.h"
_TC_BEGIN
/* As in the MSVC CRT: memcpy, memset, memcmp, strlen and strcmp have no dllimport
   (they are intrinsics; when emitted as calls they resolve through the import library). */
void* __cdecl memcpy(void* dst, const void* src, size_t n);
void* __cdecl memset(void* dst, int c, size_t n);
int __cdecl memcmp(const void* a, const void* b, size_t n);
size_t __cdecl strlen(const char* s);
int __cdecl strcmp(const char* a, const char* b);
_TC_CRTIMP void* __cdecl memmove(void* dst, const void* src, size_t n);
_TC_CRTIMP void* __cdecl memchr(const void* s, int c, size_t n);
_TC_CRTIMP int __cdecl strncmp(const char* a, const char* b, size_t n);
_TC_CRTIMP char* __cdecl strchr(const char* s, int c);
_TC_CRTIMP char* __cdecl strrchr(const char* s, int c);
_TC_CRTIMP char* __cdecl strstr(const char* h, const char* n);
_TC_CRTIMP char* __cdecl strcpy(char* d, const char* s);
_TC_CRTIMP char* __cdecl strncpy(char* d, const char* s, size_t n);
_TC_CRTIMP char* __cdecl strcat(char* d, const char* s);
_TC_CRTIMP int __cdecl _stricmp(const char* a, const char* b);
_TC_END
#endif
