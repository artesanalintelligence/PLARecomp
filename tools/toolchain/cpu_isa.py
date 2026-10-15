#!/usr/bin/env python3
"""x86-64 micro-architecture level of this CPU (x86-64-v1/v2/v3/v4), from CPUID and XGETBV.

The recompiled modules are compiled on the player's own machine, so they can target its CPU. The build uses
the level as clang's -march (TOOLCHAIN.md §8). Standard library only: a few bytes of machine code run through
ctypes (VirtualAlloc + a function pointer), so no compiler or extra package is needed.

    python cpu_isa.py            -> prints e.g. "x86-64-v3" and the missing features of the next level
"""
from __future__ import annotations

import ctypes
import sys

# cpuid(leaf, subleaf) -> writes eax, ebx, ecx, edx to out[0..3]. Microsoft x64 ABI: rcx = leaf, rdx = subleaf,
# r8 = out. rbx is callee-saved, so it is kept in r9 around cpuid.
_CPUID = bytes([
    0x49, 0x89, 0xD9,              # mov r9, rbx
    0x89, 0xC8,                    # mov eax, ecx
    0x89, 0xD1,                    # mov ecx, edx
    0x0F, 0xA2,                    # cpuid
    0x41, 0x89, 0x00,              # mov [r8], eax
    0x41, 0x89, 0x58, 0x04,        # mov [r8+4], ebx
    0x41, 0x89, 0x48, 0x08,        # mov [r8+8], ecx
    0x41, 0x89, 0x50, 0x0C,        # mov [r8+12], edx
    0x4C, 0x89, 0xCB,              # mov rbx, r9
    0xC3,                          # ret
])
# xgetbv(0) -> rax = XCR0 (low 32 bits are enough: SSE/AVX/AVX-512 state bits).
_XGETBV = bytes([0x31, 0xC9, 0x0F, 0x01, 0xD0, 0xC3])  # xor ecx,ecx; xgetbv; ret

_mem = None


def _code(blob: bytes, restype, argtypes):
    global _mem
    k32 = ctypes.windll.kernel32
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
    if _mem is None:
        _mem = k32.VirtualAlloc(None, 4096, 0x3000, 0x40)  # MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE
        if not _mem:
            raise OSError("VirtualAlloc failed")
        _code.off = 0
    addr = _mem + _code.off
    ctypes.memmove(addr, blob, len(blob))
    _code.off += (len(blob) + 15) & ~15
    return ctypes.CFUNCTYPE(restype, *argtypes)(addr)


_cpuid_fn = None
_xgetbv_fn = None


def cpuid(leaf: int, sub: int = 0) -> tuple[int, int, int, int]:
    global _cpuid_fn
    if _cpuid_fn is None:
        _cpuid_fn = _code(_CPUID, None, [ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)])
    out = (ctypes.c_uint32 * 4)()
    _cpuid_fn(leaf, sub, out)
    return out[0], out[1], out[2], out[3]


def xcr0() -> int:
    global _xgetbv_fn
    if _xgetbv_fn is None:
        _xgetbv_fn = _code(_XGETBV, ctypes.c_uint64, [])
    return int(_xgetbv_fn())


def features() -> dict[str, bool]:
    f: dict[str, bool] = {}
    max_leaf = cpuid(0)[0]
    _, _, c1, d1 = cpuid(1)
    b7 = c7 = 0
    if max_leaf >= 7:
        _, b7, c7, _ = cpuid(7, 0)
    max_ext = cpuid(0x80000000)[0]
    c81 = cpuid(0x80000001)[2] if max_ext >= 0x80000001 else 0
    bit = lambda r, n: bool((r >> n) & 1)  # noqa: E731
    osxsave = bit(c1, 27)
    x = xcr0() if osxsave else 0
    ymm_os = (x & 0x6) == 0x6            # XMM + YMM state enabled by the OS
    zmm_os = ymm_os and (x & 0xE0) == 0xE0
    f.update(cx8=bit(d1, 8), cmov=bit(d1, 15), sse2=bit(d1, 26),
             cx16=bit(c1, 13), popcnt=bit(c1, 23), sse3=bit(c1, 0), ssse3=bit(c1, 9),
             sse41=bit(c1, 19), sse42=bit(c1, 20), lahf=bit(c81, 0),
             avx=bit(c1, 28) and ymm_os, fma=bit(c1, 12) and ymm_os, f16c=bit(c1, 29) and ymm_os,
             movbe=bit(c1, 22), avx2=bit(b7, 5) and ymm_os, bmi1=bit(b7, 3), bmi2=bit(b7, 8),
             lzcnt=bit(c81, 5), xsave=bit(c1, 26) and osxsave,
             avx512f=bit(b7, 16) and zmm_os, avx512bw=bit(b7, 30) and zmm_os, avx512cd=bit(b7, 28) and zmm_os,
             avx512dq=bit(b7, 17) and zmm_os, avx512vl=bit(b7, 31) and zmm_os)
    _ = c7
    return f


LEVELS = [
    ("x86-64-v2", ("cx16", "lahf", "popcnt", "sse3", "sse41", "sse42", "ssse3")),
    ("x86-64-v3", ("avx", "avx2", "bmi1", "bmi2", "f16c", "fma", "lzcnt", "movbe", "xsave")),
    ("x86-64-v4", ("avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl")),
]


def level(f: dict[str, bool] | None = None) -> tuple[str, list[str]]:
    """-> (highest level fully supported, features missing for the next one)."""
    f = f if f is not None else features()
    got = "x86-64"
    for name, need in LEVELS:
        missing = [n for n in need if not f.get(n)]
        if missing:
            return got, missing
        got = name
    return got, []


if __name__ == "__main__":
    try:
        lv, miss = level()
    except Exception as e:  # pragma: no cover
        print(f"x86-64 (detection failed: {e!r})")
        sys.exit(0)
    print(lv + (f"  (next level missing: {', '.join(miss)})" if miss else ""))
