/* setjmp.h minimal (port's portable toolchain): clang's intrin.h includes it only for
 * the jmp_buf type. On x64 it is 16 elements of 16 bytes aligned to 16 (256 bytes). */
#ifndef TC_SETJMP_H
#define TC_SETJMP_H
#include "_tc_crt.h"
_TC_BEGIN
typedef struct __declspec(align(16)) _TC_JBFLOAT128 { unsigned long long Part[2]; } _TC_JBFLOAT128;
typedef _TC_JBFLOAT128 jmp_buf[16];
_TC_END
#endif
