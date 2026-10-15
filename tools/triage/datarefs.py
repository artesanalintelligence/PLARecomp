"""Data references (ADRP + ADD/LDR/STR) per function, vectorized with numpy.

Returns, for each function, the absolute addresses it materializes with the
ADRP + LO12 pair. Useful to find private data of a TU (static
variables): if an address is only used by functions close to each other, those
functions are in the same TU.
"""
from __future__ import annotations

import numpy as np

LOOKAHEAD = 8


def adrp_refs(img, lo=None, hi=None):
    tlo, thi = img.text_range()
    lo = lo or tlo
    hi = hi or thi
    seg = next(s for s in img.segs if s[0] == tlo)
    arr = np.frombuffer(seg[3][lo - tlo: (hi - tlo) // 4 * 4], dtype="<u4").astype(np.int64)
    n = arr.size
    pcs = lo + 4 * np.arange(n, dtype=np.int64)
    is_adrp = (arr & 0x9F000000) == 0x90000000
    idx = np.nonzero(is_adrp)[0]
    w = arr[idx]
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = (pcs[idx] & ~0xFFF) + (imm << 12)
    rd = w & 0x1F
    found = np.zeros(idx.size, dtype=bool)
    addr = np.zeros(idx.size, dtype=np.int64)
    for k in range(1, LOOKAHEAD + 1):
        j = np.minimum(idx + k, n - 1)
        u = arr[j]
        base = (u >> 5) & 0x1F
        is_add = (u & 0x7F000000) == 0x11000000          # ADD imm (32/64)
        is_ldst = (u & 0x3B000000) == 0x39000000          # LDR/STR unsigned imm
        hit = ~found & (base == rd) & (is_add | is_ldst)
        imm12 = (u >> 10) & 0xFFF
        sh = (u >> 22) & 1
        size = (u >> 30) & 3
        v = (u >> 26) & 1
        opc = (u >> 22) & 3
        scale = np.where((v == 1) & (opc >= 2) & (size == 0), 4, size)
        off = np.where(is_add, np.where(sh == 1, imm12 << 12, imm12), imm12 << scale)
        addr = np.where(hit, page + off, addr)
        found |= hit
        # if the instruction redefines the register before it is used, it is lost (approx.)
    return pcs[idx][found], addr[found]
