"""Scan of the pending game functions: disassembly (capstone), textual template and
body hash with relocations masked (to group duplicates).

  python tools/decomp/autogen/scan.py            # -> tools/decomp/cache/autogen_scan.pkl
  python tools/decomp/autogen/scan.py --hist 40  # histogram of templates of functions <= 64 bytes

Only considers `game` functions (data/triage.csv) that are neither O/F nor reserved by another agent
(this is checked again when reserving with claim.py).
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import pickle
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import CACHE, functions, is_decomp_target  # noqa: E402
from image import load_image  # noqa: E402

SCAN_PKL = CACHE / "autogen_scan.pkl"
SMALL = 1 << 20   # all are disassembled (the template is used for grouping)


def mask_words(words: list[int]) -> list[int]:
    """Masks the relocatable fields (B/BL, ADRP/ADR, LO12 on ADRP registers, LDR literal)."""
    out = []
    page = set()                       # registers that hold an ADRP page
    for w in words:
        m = w
        if (w & 0x7C000000) == 0x14000000:              # B / BL imm26
            m = w & 0xFC000000
        elif (w & 0x1F000000) == 0x10000000:            # ADR / ADRP
            m = w & 0x9F00001F
            if w & 0x80000000:
                page.add(w & 31)
            else:
                page.discard(w & 31)
        elif (w & 0x3B000000) == 0x18000000:            # LDR literal
            m = w & 0xFF00001F
        else:
            rd, rn = w & 31, (w >> 5) & 31
            is_addimm = (w & 0x7F800000) == 0x11000000  # ADD imm (32/64)
            is_ldst = (w & 0x3B000000) == 0x39000000    # LDR/STR unsigned imm
            if (is_addimm or is_ldst) and rn in page:
                m = w & ~(0xFFF << 10)
                if is_addimm:
                    page.discard(rd)
                elif not (w >> 22) & 1:                 # store: does not write Rt
                    pass
                else:
                    page.discard(rd) if rd != rn else page.discard(rd)
            else:
                writes = True
                if (w & 0x0A000000) == 0x08000000 and not (w >> 22) & 1:
                    writes = False                       # store
                if (w & 0x7C000000) == 0x14000000 or (w & 0xFE000000) == 0x54000000:
                    writes = False
                if writes and rd in page:
                    page.discard(rd)
        out.append(m)
    return out


_NUM = re.compile(r"#-?0x[0-9a-f]+|#-?\d+(?:\.\d+(?:e[-+]?\d+)?)?")


def template(insns) -> str:
    """Textual template: mnemonic + operands with immediates and branch targets substituted."""
    parts = []
    for i in insns:
        op = i.op_str
        if i.mnemonic in ("b", "bl", "adrp", "adr") or i.mnemonic.startswith("b."):
            op = re.sub(r"#0x[0-9a-f]+", "T", op)
        else:
            op = _NUM.sub("N", op)
        parts.append(f"{i.mnemonic} {op}".strip())
    return "; ".join(parts)


def pending(funcs, rows=None):
    """Pending, free game functions (no O/F quality and no live reservation by another agent)."""
    import status as st
    from claim import is_free
    rows = st.read() if rows is None else rows
    me = None
    out = []
    for f in funcs.funcs:
        if f.size <= 0 or not is_decomp_target(f):
            continue
        r = rows.get(f.addr)
        if r is not None:
            if r.get("quality") in ("O", "F"):
                continue
            if r.get("quality") == "W" and r.get("agent") not in ("autogen", ""):
                continue   # another agent already has code written for it
            if not is_free(r, me or "autogen", 24.0):
                continue
        out.append(f)
    return out


def scan():
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    im = load_image()
    funcs = functions()
    res = {}
    for f in pending(funcs):
        raw = im.read(f.addr, f.size)
        words = list(struct.unpack(f"<{f.size // 4}I", raw[: f.size // 4 * 4]))
        h = hashlib.sha1(struct.pack(f"<{len(words)}I", *mask_words(words))).hexdigest()[:16]
        tpl = None
        if f.size <= SMALL:
            insns = list(cs.disasm(raw, f.addr))
            if len(insns) * 4 == f.size:
                tpl = template(insns)
        res[f.addr] = (f.size, h, tpl)
    SCAN_PKL.write_bytes(pickle.dumps(res))
    return res


def load_scan():
    return pickle.loads(SCAN_PKL.read_bytes())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hist", type=int, default=0, help="show the N most frequent templates")
    ap.add_argument("--reuse", action="store_true", help="do not rescan (use the cache)")
    a = ap.parse_args(argv)
    res = load_scan() if a.reuse and SCAN_PKL.exists() else scan()
    n = len(res)
    by_hash = collections.Counter(h for _, h, _ in res.values())
    dup_funcs = sum(c for c in by_hash.values() if c > 1)
    print(f"{n} pending functions; {len(by_hash)} distinct bodies; {dup_funcs} in clusters of >1")
    if a.hist:
        c = collections.Counter(t for _, _, t in res.values() if t)
        for t, k in c.most_common(a.hist):
            print(f"{k:>7}  {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
