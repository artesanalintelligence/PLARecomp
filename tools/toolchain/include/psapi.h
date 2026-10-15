/* psapi.h minimal (port's portable toolchain); see windows.h. */
#ifndef TC_PSAPI_H
#define TC_PSAPI_H
#include <windows.h>
#include "_tc_crt.h"
_TC_BEGIN
/* As in the SDK with PSAPI_VERSION 2: no dllimport, resolved in kernel32. */
BOOL WINAPI K32EnumProcessModules(HANDLE process, HMODULE* mods, DWORD cb, DWORD* needed);
#define EnumProcessModules K32EnumProcessModules
_TC_END
#endif
