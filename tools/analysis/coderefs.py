#!/usr/bin/env python3
"""Address references built in code (ADRP + ADD/LDR), vectorized with numpy.

Returns the set of addresses that the code materializes with
    adrp xN, page ; add xD, xN, #lo12        (address)
    adrp xN, page ; ldr xD/wD, [xN, #lo12]   (load from that address)
looking for the second instruction up to 4 positions after the ADRP. It does
not check whether xN is overwritten in between: it is a fast approximation (some
false positives) meant to confirm vtables and to seed data.

Usage: python tools/analysis/coderefs.py data/main.elf [-o refs.json]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfimage import ElfImage, add_common_args  # noqa: E402

LOOKAHEAD = 4


def adrp_refs(img: ElfImage) -> dict[str, np.ndarray]:
    """{'add': addresses (va) formed with ADD, 'ldr': addresses read with LDR}."""
    adds, ldrs = [], []
    for lo, hi in img.code_ranges:
        lo4 = (lo + 3) & ~3
        n = (hi - lo4) // 4
        if n <= 0:
            continue
        w = np.frombuffer(img.read(lo4, n * 4), dtype="<u4").astype(np.int64)
        pc = lo4 + 4 * np.arange(n, dtype=np.int64)
        is_adrp = (w & 0x9F000000) == 0x90000000
        idx = np.nonzero(is_adrp)[0]
        if not len(idx):
            continue
        aw = w[idx]
        rd = aw & 31
        imm = (((aw >> 5) & 0x7FFFF) << 2) | ((aw >> 29) & 3)
        imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
        page = (pc[idx] & ~0xFFF) + (imm << 12)
        for k in range(1, LOOKAHEAD + 1):
            j = idx + k
            ok = j < n
            jj, pg, r = j[ok], page[ok], rd[ok]
            bw = w[jj]
            rn = (bw >> 5) & 31
            same = rn == r
            # ADD (immediate, 64-bit): sf=1 op=0 S=0 100010 sh imm12
            is_add = ((bw & 0xFF800000) == 0x91000000) & same
            off = (bw >> 10) & 0xFFF
            adds.append(pg[is_add] + off[is_add])
            # LDR (unsigned immediate) 64-bit and 32-bit
            is_ldr64 = ((bw & 0xFFC00000) == 0xF9400000) & same
            is_ldr32 = ((bw & 0xFFC00000) == 0xB9400000) & same
            ldrs.append(pg[is_ldr64] + off[is_ldr64] * 8)
            ldrs.append(pg[is_ldr32] + off[is_ldr32] * 4)
    cat = lambda xs: np.unique(np.concatenate(xs)) if xs else np.zeros(0, dtype=np.int64)  # noqa: E731
    return {"add": cat(adds), "ldr": cat(ldrs)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("-o", "--output")
    args = ap.parse_args(argv)
    img = ElfImage(args.elf, args.base)
    refs = adrp_refs(img)
    print(f"ADRP+ADD: {len(refs['add'])} unique addresses; ADRP+LDR: {len(refs['ldr'])}", file=sys.stderr)
    if args.output:
        Path(args.output).write_text(json.dumps({
            "base": f"0x{img.base:x}",
            "add": [f"0x{img.to_abs(int(x)):x}" for x in refs["add"]],
            "ldr": [f"0x{img.to_abs(int(x)):x}" for x in refs["ldr"]]}), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
