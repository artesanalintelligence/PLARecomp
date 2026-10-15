/* direct.h minimal (port's portable toolchain); see _tc_crt.h. */
#ifndef TC_DIRECT_H
#define TC_DIRECT_H
#include "_tc_crt.h"
_TC_BEGIN
_TC_CRTIMP int __cdecl _mkdir(const char* path);
_TC_END
#endif
