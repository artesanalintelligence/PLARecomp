#!/usr/bin/env python3
"""Detects switch jump tables (clang and GCC) and exports all their targets.

Supported patterns (backward slicing from each `br Xn`):
  clang >= 9, compressed tables (AArch64CompressJumpTables):
      adrp xT, table ; add xT, xT, :lo12:table
      adr  xA, anchor
      ldrb/ldrh wE, [xT, xI(, lsl #1)]
      add  xD, xA, xE, lsl #2
      br   xD
  classic clang (32-bit entries relative to the table):
      ldrsw xE, [xT, xI, lsl #2] ; add xD, xT, xE ; br xD
  GCC:
      ldrb/ldrh wE, [xT, wI, uxtw(#1)] ; adr xA, anchor ; add xD, xA, wE, sxtb/sxth #2 ; br xD
  absolute tables (64-bit pointers, e.g. non-PIC code):
      ldr xD, [xT, xI, lsl #3] ; br xD

The number of entries comes from the preceding `cmp wI, #N` + `b.hi/b.ls/b.hs/b.lo`. If
it is not found, entries are read while the targets fall inside the
function (eh_frame bounds) and bounded=false is set.

Usage:
    python tools/analysis/jumptables.py data/main.elf -o tools/ghidra/out/jumptables.json
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfimage import ElfImage, add_common_args  # noqa: E402

import capstone  # noqa: E402

_ARCH = getattr(capstone, "CS_ARCH_ARM64", None)
if _ARCH is None:
    _ARCH = capstone.CS_ARCH_AARCH64

WINDOW = 40          # instructions backward from the br
MAX_ENTRIES = 8192
MAX_UNBOUNDED = 512  # cap on entries if the bound is not found
MAX_FUNC_SCAN = 64 * 1024  # function bytes to disassemble if the table base is far away

_REG = re.compile(r"^[xw](\d+)$")
_IMM = re.compile(r"^#(-?(?:0x[0-9a-f]+|\d+))$")


def regnum(tok: str) -> int | None:
    tok = tok.strip()
    if tok in ("xzr", "wzr"):
        return 31
    m = _REG.match(tok)
    return int(m.group(1)) if m else None


def imm(tok: str) -> int | None:
    m = _IMM.match(tok.strip())
    return int(m.group(1), 0) if m else None


def split_ops(op_str: str) -> list[str]:
    """Split operands respecting brackets: 'w1, [x2, w0, uxtw]' -> ['w1', '[x2, w0, uxtw]']."""
    out, depth, cur = [], 0, ""
    for ch in op_str:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip().rstrip("!"))
    return out


class Insn:
    __slots__ = ("addr", "mn", "ops")

    def __init__(self, addr, mn, op_str):
        self.addr = addr
        self.mn = mn
        self.ops = split_ops(op_str)

    def dest(self) -> int | None:
        if not self.ops or self.mn.startswith(("st", "b", "cmp", "cmn", "tst", "cb", "tb", "ret", "nop", "prfm")):
            return None
        return regnum(self.ops[0])

    def __repr__(self):
        return f"{self.addr:#x}: {self.mn} {', '.join(self.ops)}"


LOAD_SIZES = {"ldrb": (1, False), "ldrsb": (1, True), "ldrh": (2, False), "ldrsh": (2, True),
              "ldrsw": (4, True)}
EXT_SIGNED = {"sxtb": (1, True), "sxth": (2, True), "sxtw": (4, True),
              "uxtb": (1, False), "uxth": (2, False), "uxtw": (4, False)}


class JumpTableFinder:
    def __init__(self, img: ElfImage, func_bounds: list[tuple[int, int]] | None = None):
        self.img = img
        self.md = capstone.Cs(_ARCH, capstone.CS_MODE_ARM)
        self.md.skipdata = True
        self.bounds = sorted(func_bounds or [])
        self._bstarts = [b[0] for b in self.bounds]
        self.unresolved: list[int] = []

    def func_of(self, va: int) -> tuple[int, int] | None:
        i = bisect.bisect_right(self._bstarts, va) - 1
        if i >= 0 and self.bounds[i][0] <= va < self.bounds[i][0] + self.bounds[i][1]:
            return self.bounds[i]
        return None

    def find_brs(self):
        """Search for `br Xn` (0xD61F0000 | n<<5) in the executable segments."""
        for seg in self.img.exec_segments:
            blob = self.img.read(seg.vaddr, seg.filesz)
            for m in re.finditer(rb"[\x00-\xff][\x00-\x03]\x1f\xd6", blob, re.S):
                off = m.start()
                if off % 4:
                    continue
                w = int.from_bytes(blob[off:off + 4], "little")
                if w & 0xFFFFFC1F == 0xD61F0000:
                    yield seg.vaddr + off, (w >> 5) & 31

    def window(self, br: int, full: bool = False) -> list[Insn]:
        f = self.func_of(br)
        seg_lo = self.img.segment_of(br).vaddr
        if full:
            lo = max(f[0] if f else br - 4 * WINDOW, br - MAX_FUNC_SCAN, seg_lo)
        else:
            lo = max(br - 4 * WINDOW, f[0] if f else 0, seg_lo)
        code = self.img.read(lo, br + 4 - lo)
        return [Insn(i.address, i.mnemonic, i.op_str) for i in self.md.disasm(code, lo)]

    @staticmethod
    def find_def(insns: list[Insn], idx: int, reg: int, cross: bool = False) -> int | None:
        """Index of the last instruction before idx that writes to reg.

        With cross=True it does not stop at block ends (linear search
        over the whole function, for table bases computed outside a loop)."""
        for j in range(idx - 1, -1, -1):
            ins = insns[j]
            if not cross and (ins.mn in ("ret", "b") or ins.mn == "br"):
                return None  # end of basic block going backward
            if ins.dest() == reg:
                return j
            # ldp/ldr with post/pre-index or pairs: the second destination
            if ins.mn in ("ldp", "ldpsw") and len(ins.ops) > 1 and regnum(ins.ops[1]) == reg:
                return j
        return None

    def resolve_addr(self, insns: list[Insn], idx: int, reg: int, cross: bool = False) -> int | None:
        """Value of a register built with adrp(+add) or adr."""
        j = self.find_def(insns, idx, reg, cross)
        if j is None:
            return None
        ins = insns[j]
        if ins.mn == "adr":
            return imm(ins.ops[1])
        if ins.mn == "adrp":
            return imm(ins.ops[1])
        if ins.mn == "add" and len(ins.ops) == 3 and regnum(ins.ops[1]) is not None:
            k = imm(ins.ops[2])
            if k is None:
                return None
            base = self.resolve_addr(insns, j, regnum(ins.ops[1]), cross)
            return None if base is None else base + k
        if ins.mn == "mov" and len(ins.ops) == 2 and regnum(ins.ops[1]) is not None:
            return self.resolve_addr(insns, j, regnum(ins.ops[1]), cross)
        return None

    def resolve_far(self, br: int, at_addr: int, reg: int) -> int | None:
        """Like resolve_addr, but searching the whole function (base outside the window)."""
        full = self.window(br, full=True)
        idx = next((i for i, x in enumerate(full) if x.addr == at_addr), None)
        if idx is None:
            return None
        return self.resolve_addr(full, idx, reg, cross=True)

    @staticmethod
    def parse_mem(op: str):
        """'[x9, x8, lsl #2]' -> (base, index, ext, shift)."""
        if not op.startswith("["):
            return None
        parts = [p.strip() for p in op.strip("[]!").split(",")]
        if len(parts) < 2:
            return None
        base, index = regnum(parts[0]), regnum(parts[1])
        if base is None or index is None:
            return None
        ext, shift = None, 0
        if len(parts) > 2:
            m = re.match(r"(lsl|[su]xt[bhwx])(?:\s+#(\d+))?", parts[2])
            if m:
                ext = m.group(1)
                shift = int(m.group(2) or 0)
        return base, index, ext, shift

    def find_bound(self, insns: list[Insn], idx: int, index_reg: int) -> int | None:
        """Search for cmp wI, #N followed by b.<cond> and return the number of entries."""
        regs = {index_reg}
        for j in range(idx - 1, -1, -1):
            ins = insns[j]
            if ins.mn in ("cmp", "subs") and ins.ops:
                r = regnum(ins.ops[0] if ins.mn == "cmp" else ins.ops[1])
                n = imm(ins.ops[-1])
                if r in regs and n is not None:
                    # condition of the first b.cond after the cmp
                    for k in range(j + 1, min(j + 4, len(insns))):
                        mn = insns[k].mn
                        if mn in ("b.hi", "b.ls"):
                            return n + 1
                        if mn in ("b.hs", "b.lo", "b.cs", "b.cc"):
                            return n
                    return n + 1
            d = ins.dest()
            # index bounded by mask: and wI, wS, #(2^k - 1) / ubfx wI, wS, #lsb, #k
            if d in regs and ins.mn == "and" and len(ins.ops) == 3:
                m = imm(ins.ops[2])
                if m is not None and m > 0 and (m & (m + 1)) == 0:
                    return m + 1
            if d in regs and ins.mn == "ubfx" and len(ins.ops) == 4:
                width = imm(ins.ops[3])
                if width is not None and width <= 13:
                    return 1 << width
            if d in regs and ins.mn in ("uxtb",):
                return 256
            # follow copies of the index (mov wI, wJ / and / ubfx)
            if d in regs and ins.mn in ("mov", "uxtw", "sxtw", "uxth", "and") and len(ins.ops) >= 2:
                src = regnum(ins.ops[1])
                if src is not None:
                    regs.add(src)
        return None

    def analyze_br(self, br: int, reg: int) -> dict | None:
        insns = self.window(br)
        if not insns or insns[-1].addr != br:
            return None
        idx = len(insns) - 1
        j = self.find_def(insns, idx, reg)
        if j is None:
            return None
        d = insns[j]
        # ---- absolute table: ldr xD, [xT, xI, lsl #3]
        if d.mn == "ldr" and len(d.ops) == 2 and d.ops[0].startswith("x"):
            mem = self.parse_mem(d.ops[1])
            if not mem:
                return None
            base, index, _, shift = mem
            table = self.resolve_addr(insns, j, base)
            if table is None:
                table = self.resolve_far(br, d.addr, base)
            if table is None or shift != 3:
                return None
            return self._emit(br, insns, j, index, table, None, 8, False, 0, absolute=True)
        if d.mn != "add" or len(d.ops) < 3:
            return None
        ra, rb = regnum(d.ops[1]), regnum(d.ops[2])
        if ra is None or rb is None:
            return None
        shift = 0
        ext = None
        if len(d.ops) > 3:
            m = re.match(r"(lsl|[su]xt[bhwx])(?:\s+#(\d+))?", d.ops[3])
            if m:
                ext, shift = m.group(1), int(m.group(2) or 0)
        # which of the two operands comes from an indexed load?
        for load_reg, anchor_reg in ((rb, ra), (ra, rb)):
            lj = self.find_def(insns, j, load_reg)
            if lj is None:
                continue
            ld = insns[lj]
            if ld.mn not in LOAD_SIZES and not (ld.mn == "ldr" and ld.ops[0].startswith("w")):
                continue
            mem = self.parse_mem(ld.ops[1]) if len(ld.ops) == 2 else None
            if not mem:
                continue
            base, index, _, _ = mem
            size, signed = LOAD_SIZES.get(ld.mn, (4, False))
            if ext in EXT_SIGNED:
                esize, esigned = EXT_SIGNED[ext]
                if esize <= size:
                    size, signed = esize, esigned
            table = self.resolve_addr(insns, lj, base)
            if table is None:
                table = self.resolve_far(br, ld.addr, base)
            if table is None:
                continue
            if anchor_reg == base and self.find_def(insns, j, anchor_reg) == self.find_def(insns, lj, base):
                anchor = table
            else:
                anchor = self.resolve_addr(insns, j, anchor_reg)
                if anchor is None:
                    anchor = self.resolve_far(br, d.addr, anchor_reg)
            if anchor is None:
                continue
            return self._emit(br, insns, lj, index, table, anchor, size, signed, shift)
        return None

    def _emit(self, br, insns, load_idx, index_reg, table, anchor, size, signed, shift, absolute=False):
        img = self.img
        count = self.find_bound(insns, load_idx, index_reg)
        bounded = count is not None
        f = self.func_of(br)
        if count is None:
            count = MAX_UNBOUNDED
        targets = []
        for i in range(min(count, MAX_ENTRIES)):
            va = table + i * size
            try:
                if absolute:
                    p = img.pointer_at(va)
                    t = p.target if p is not None else None
                else:
                    e = int.from_bytes(img.read(va, size), "little", signed=signed)
                    t = anchor + (e << shift)
            except IndexError:
                t = None
            if t is None or not img.is_exec(t) or t % 4:
                if bounded:
                    return None  # inconsistent table: discard
                break
            if not bounded and f and not f[0] <= t < f[0] + f[1]:
                break
            targets.append(t)
        if not targets:
            return None
        return {"br": br, "table": table, "anchor": anchor, "entry_size": size, "signed": signed,
                "shift": shift, "count": len(targets), "bounded": bounded, "targets": targets,
                "absolute": absolute}

    def run(self) -> list[dict]:
        out = []
        for br, reg in self.find_brs():
            try:
                r = self.analyze_br(br, reg)
            except (IndexError, ValueError):
                r = None
            if r:
                out.append(r)
            else:
                self.unresolved.append(br)
        # Unbounded tables: they cannot invade the next table (they are usually contiguous).
        starts = sorted({t["table"] for t in out})
        for t in out:
            if t["bounded"]:
                continue
            i = bisect.bisect_right(starts, t["table"])
            if i < len(starts):
                room = (starts[i] - t["table"]) // t["entry_size"]
                if room < len(t["targets"]):
                    t["targets"] = t["targets"][:max(room, 1)]
                    t["count"] = len(t["targets"])
        return out


def find_jumptables(img: ElfImage, func_bounds=None) -> tuple[list[dict], list[int]]:
    if func_bounds is None:
        try:
            import eh_frame
            fdes, _ = eh_frame.collect(img)
            func_bounds = [(f.start, f.size) for f in fdes]
        except Exception:
            func_bounds = []
    jf = JumpTableFinder(img, func_bounds)
    return jf.run(), jf.unresolved


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("-o", "--output", help="output JSON")
    args = ap.parse_args(argv)
    img = ElfImage(args.elf, args.base)
    tables, unresolved = find_jumptables(img)
    A = img.to_abs
    res = {
        "elf": str(img.path), "base": f"0x{img.base:x}",
        "stats": {"tables": len(tables), "targets": sum(t["count"] for t in tables),
                  "unique_targets": len({x for t in tables for x in t["targets"]}),
                  "unbounded": sum(1 for t in tables if not t["bounded"]),
                  "indirect_br_unresolved": len(unresolved)},
        "tables": [{**t, "br": f"0x{A(t['br']):x}", "table": f"0x{A(t['table']):x}",
                    "anchor": None if t["anchor"] is None else f"0x{A(t['anchor']):x}",
                    "targets": [f"0x{A(x):x}" for x in t["targets"]]} for t in tables],
    }
    st = res["stats"]
    print(f"jump tables: {st['tables']} tables, {st['unique_targets']} unique targets, "
          f"{st['unbounded']} unbounded, {st['indirect_br_unresolved']} unresolved indirect brs "
          "(calls through pointers, PLT, etc.)", file=sys.stderr)
    text = json.dumps(res, indent=1)
    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
