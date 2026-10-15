/* time.h minimal (port's portable toolchain); see _tc_crt.h. */
#ifndef TC_TIME_H
#define TC_TIME_H
#include "_tc_crt.h"
_TC_BEGIN
typedef long clock_t;
typedef long long time_t;
#define CLOCKS_PER_SEC ((clock_t)1000)
_TC_CRTIMP clock_t __cdecl clock(void);
_TC_END
#endif
