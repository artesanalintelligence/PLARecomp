#!/usr/bin/env python3
"""Approximate arity of the driver's NVN functions (own analysis of the sdk code).

For each function in nvn_sdk_map.csv it walks the code (reachable blocks, instruction budget;
direct `b` branches are followed, `bl` calls to internal functions are not) and records which
argument registers (X0-X7 / W0-W7, V0-V7 / S / D) are read before being written and which are
used as the base of a memory access (= pointer). It is a conservative approximation: a register
that is only forwarded to a `bl` counts as read.

Output: adds/updates the `xargs`, `ptrargs`, `vargs`, `tail` columns of
docs/port/data/nvn_sdk_map.csv (xargs = number of X read, ptrargs = bit mask of those used
as pointers, vargs = number of V read, tail = 'vcall' if it ends in an indirect jump with
the arguments untouched).
"""
from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

import capstone
from capstone import arm64_const as A

sys.path.insert(0, str(Path(__file__).parent))
from sdkelf import Img, ROOT  # noqa: E402

MAP = ROOT / "docs" / "port" / "data" / "nvn_sdk_map.csv"

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True


def reg_index(cs_reg: int):
    """('x', n) / ('v', n) for argument registers, otherwise None."""
    name = md.reg_name(cs_reg)
    m = re.fullmatch(r"([xw])([0-7])", name)
    if m:
        return ("x", int(m.group(2)))
    m = re.fullmatch(r"([bhsdqv])([0-7])", name)
    if m:
        return ("v", int(m.group(2)))
    return None


def analyze(im: Img, entry: int, budget: int = 400):
    read_x, read_v, ptr = set(), set(), set()
    tail = ""
    # per-path state: set of written registers; depth-first walk with copies
    work = [(entry, frozenset())]
    visited = set()
    steps = 0
    while work and steps < budget:
        pc, written = work.pop()
        if (pc, written) in visited:
            continue
        visited.add((pc, written))
        w = set(written)
        while steps < budget:
            steps += 1
            try:
                ins = next(md.disasm(im.data[im.v2o(pc):im.v2o(pc) + 4], pc))
            except StopIteration:
                break
            regs_r, regs_w = ins.regs_access()
            # memory bases
            for op in ins.operands:
                if op.type == A.ARM64_OP_MEM and op.mem.base:
                    r = reg_index(op.mem.base)
                    if r and r[0] == "x" and ("x", r[1]) not in w:
                        ptr.add(r[1])
            for r in regs_r:
                ri = reg_index(r)
                if ri and ri not in w:
                    (read_x if ri[0] == "x" else read_v).add(ri[1])
            mn = ins.mnemonic
            if mn == "bl":
                # the callee may read any argument: already-written ones do not count
                for k in range(8):
                    w.add(("x", k))
                    w.add(("v", k))
                pc += 4
                continue
            for r in regs_w:
                ri = reg_index(r)
                if ri:
                    w.add(ri)
            if mn == "ret":
                break
            if mn in ("br", "blr") and mn == "br":
                if not tail:
                    tail = "vcall"
                break
            if mn == "b":
                work.append((ins.operands[0].imm, frozenset(w)))
                break
            if mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz"):
                tgt = ins.operands[-1].imm
                work.append((tgt, frozenset(w)))
            pc += 4
    nx = max(read_x) + 1 if read_x else 0
    nv = max(read_v) + 1 if read_v else 0
    mask = sum(1 << k for k in ptr if k < 8)
    return nx, mask, nv, tail


def main() -> int:
    im = Img()
    rows = list(csv.DictReader(open(MAP, newline="")))
    for r in rows:
        nx, mask, nv, tail = analyze(im, int(r["sdk_offset"], 16))
        r["xargs"], r["ptrargs"], r["vargs"], r["tail"] = nx, f"0x{mask:x}", nv, tail
    fields = list(rows[0].keys())
    with open(MAP, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=fields)
        wr.writeheader()
        wr.writerows(rows)
    print(f"{len(rows)} functions analyzed -> {MAP}")
    for nm in ("nvnCommandBufferDrawArrays", "nvnDeviceGetTextureHandle", "nvnProgramSetShaders",
               "nvnCommandBufferSetRenderTargets", "nvnCommandBufferSetDepthRange",
               "nvnMemoryPoolGetBufferAddress", "nvnCommandBufferClearColor", "nvnTextureInitialize"):
        r = next(x for x in rows if x["name"] == nm)
        print(f"  {nm}: xargs={r['xargs']} ptr={r['ptrargs']} vargs={r['vargs']} {r['tail']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
