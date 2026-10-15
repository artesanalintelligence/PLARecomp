/* io.h minimal (port's portable toolchain); see _tc_crt.h. */
#ifndef TC_IO_H
#define TC_IO_H
#include "_tc_crt.h"
_TC_BEGIN
_TC_CRTIMP int __cdecl _access(const char* path, int mode);
_TC_END
#endif
