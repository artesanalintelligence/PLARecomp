// Builds XZ Embedded (third_party/xz-embedded, 0BSD) as one translation unit with the options the component
// archives need: CRC64 integrity checks and the x86 BCJ filter (tools/toolchain/pack.py). XZ_DEC_ANY_CHECK lets
// other check types through unverified; the whole blob is SHA-256 verified before it is unpacked anyway.
// SPDX-License-Identifier: MIT
#define XZ_USE_CRC64
#define XZ_DEC_X86
#define XZ_DEC_ANY_CHECK

#ifdef _MSC_VER
#pragma warning(disable : 4244 4267)  // upstream size_t/uint64_t -> uint32_t narrowing (bounded by design)
#endif

#include "xz_crc32.c"
#include "xz_crc64.c"
#include "xz_dec_stream.c"
#include "xz_dec_lzma2.c"
#include "xz_dec_bcj.c"
