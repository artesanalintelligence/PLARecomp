/* Minimal headers of the port's portable toolchain (tools/toolchain).
 *
 * They replace the MSVC CRT (UCRT/vcruntime) and the Windows SDK ONLY for compiling the C
 * emitted by the recompiler (recomp_runtime.c/.h, recomp_export.c and the src units). They
 * declare just what that C (and the C++ of the islands mod) uses, with the same ABI as the dynamic CRT (/MD): UCRT functions
 * are imported from api-ms-win-crt-*.dll and memcpy/memset/memcmp from vcruntime140.dll,
 * exactly like the official build. Written from scratch from the documented ABI. */
#ifndef TC_CRT_H
#define TC_CRT_H
#include <stddef.h>
#define _TC_CRTIMP __declspec(dllimport)
/* C linkage when a header is included from C++ (mods such as islands; the emitted C is plain C). */
#ifdef __cplusplus
#define _TC_BEGIN extern "C" {
#define _TC_END }
#else
#define _TC_BEGIN
#define _TC_END
#endif
#endif
