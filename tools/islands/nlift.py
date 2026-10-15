#!/usr/bin/env python3
"""nlift.py - lifts selected AArch64 guest functions into native C++ islands (FPS4-ISLANDS).

Every guest instruction becomes a statement on C++ locals (x registers uint64_t, vector registers __m128i,
flags as ints), branches become gotos, calls between lifted functions become C++ calls, so clang keeps the
guest registers in host registers across the whole function (no guest context, no block dispatch, no per-access
translation: guest memory is MM::P(va) = FM2 base + va). Loads from read-only, relocation-free .rodata through
an adrp page are folded into constants at generation time.

Exactness: each instruction keeps its AArch64 semantics (nl_rt.h). A path that reaches an instruction or call
the lifter does not support returns NL_BAIL; the generator proves (forward dataflow) that no non-stack store of
the same invocation can precede such a point, so the caller can run the recompiled original instead with
identical results (stores of recursive calls are complete, correct updates and are allowed).

usage: python nlift.py <main.elf> <out.inc>
env NLIFT_OBJDUMP: llvm-objdump to use (default: the LLVM install; build_islands.py passes the portable toolchain's)
"""
import os
import re
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile

OBJDUMP = os.environ.get("NLIFT_OBJDUMP") or r"C:\Program Files\LLVM\bin\llvm-objdump.exe"

# Functions to lift: start, end (exclusive), argument registers, result registers, inline.
FUNCS = [
    # allow_after_store: recursive calls that follow stores to the joint's own world matrix rows only (constraint
    # types >= 3); a re-run of the original rewrites those rows before reading them, so a bail there is safe.
    dict(start=0x2A697AC, end=0x2A6A984, args=["x0", "x1"], rets=[], inline=False, name="skel_joint_update", guard="skel_guard",
         allow_after_store=[0x2A6A834, 0x2A6A8FC]),
    # Quaternion helpers of the constraint paths (slerp-like 0x8C6B1C, 0xBB8D18): leaf code + sqrtf.
    dict(start=0x8C6B1C, end=None, args=["x0", "x1", "v0"], rets=["v0"], inline=False, name="quat_slerp"),
    dict(start=0xBB8D18, end=None, args=["x0"], rets=["v0"], inline=False, name="quat_op"),
    dict(start=0x2ACB330, end=0x2ACB50C, args=["x0"], rets=["v0", "v1", "v2", "v3"], inline=True, name="trs_to_mtx"),
    dict(start=0x2ACB6F0, end=0x2ACB978, args=["x0", "x1"], rets=["v0", "v1", "v2", "v3"], inline=True,
         name="trs_to_mtx_ssc"),
    # Region: skin palette loop of main+0x29C983C (out[idx[i]] = A[i] x B[i]), entered at the block after the
    # buffer lookup's indirect call returns, left at the epilogue.
    dict(start=0x29C98BC, end=0x29C995C, kind="region", args=[], rets=[], inline=False, name="skin_palette_loop"),
    # Region: the per-joint loop of the skeleton "update all" (main+0x2A6BA60) after its lock is taken: calls the
    # joint island directly instead of one hook dispatch per dirty joint; left before the skinning virtual call.
    # Leaf function as a region (entered at its first instruction, left at its ret: full register fidelity):
    # 4x4 matrix -> scale/rotation decomposition (sqrt, rsqrt estimate/steps, TBL shuffles).
    dict(start=0x2A3781C, end=None, kind="region", args=[], rets=[], inline=False, name="mtx_decompose"),
    dict(start=0x2A6BAF4, end=0x2A6BB60, kind="region", args=[], rets=[], inline=False, name="skel_update_all_loop"),
]


# Calls replaced by their (exact) body: sqrtf@plt -> the guest libm's `fsqrt s0, s0; ret`.
INLINE_CALLS = {0x32A3090: "v0 = nl::fsqrts_arm(v0); /* sqrtf */"}


class LiftError(Exception):
    pass


# ----------------------------------------------------------------------------------------------- ELF
class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.elf = ELFFile(self.f)
        self.ro = []  # (start, end) of read-only constant ranges
        for s in self.elf.iter_sections():
            if s.name.startswith(".rodata"):
                self.ro.append((s["sh_addr"], s["sh_addr"] + s["sh_size"], s))
        self.rel = set()
        for s in self.elf.iter_sections():
            if s.name.startswith(".rela"):
                for r in s.iter_relocations():
                    off = r["r_offset"]
                    for (a, b, _) in self.ro:
                        if a <= off < b:
                            self.rel.add(off & ~7)

    def const(self, addr, size):
        for (a, b, s) in self.ro:
            if a <= addr and addr + size <= b:
                for q in range(addr & ~7, addr + size, 8):
                    if q in self.rel:
                        return None
                data = s.data()
                return data[addr - a: addr - a + size]
        return None


# ----------------------------------------------------------------------------------------------- disasm
LINE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")


def disasm(elf, start, end):
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", f"--start-address={start:#x}",
                          f"--stop-address={end:#x}", elf], capture_output=True, text=True, check=True).stdout
    insns = []
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        ops = re.sub(r"\s*//.*$", "", m.group(3))
        ops = re.sub(r"\s*<[^>]*>", "", ops).strip()
        insns.append((int(m.group(1), 16), m.group(2), ops))
    return insns


def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm(s):
    s = s.strip().lstrip("#")
    return int(s, 0)


# ----------------------------------------------------------------------------------------------- operands
XR = re.compile(r"^(x|w)(\d+|zr)$")
VR = re.compile(r"^v(\d+)\.(16b|8b|4s|2s|2d|1d|8h|4h)$")
VE = re.compile(r"^v(\d+)\.(s|d|b|h)\[(\d+)\]$")
FR = re.compile(r"^(q|d|s|h|b)(\d+)$")


class Fn:
    def __init__(self, cfg, img, funcs_by_addr):
        self.cfg = cfg
        self.img = img
        self.funcs = funcs_by_addr
        if cfg.get("end") is None:  # auto extent: everything reachable from the entry inside a window
            win = disasm(sys.argv[1], cfg["start"], cfg["start"] + 0x2000)
            at = {a: i for i, (a, _, _) in enumerate(win)}
            seen, work = set(), [cfg["start"]]
            while work:
                a = work.pop()
                if a in seen or a not in at:
                    continue
                seen.add(a)
                _, mn, ops = win[at[a]]
                if mn in ("ret", "br"):
                    continue
                if mn == "b" or mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz"):
                    work.append(imm(split_ops(ops)[-1]))
                    if mn == "b":
                        continue
                work.append(a + 4)
            cfg["end"] = max(seen) + 4
        self.insns = disasm(sys.argv[1], cfg["start"], cfg["end"])
        self.addrs = {a for a, _, _ in self.insns}
        self.used_x = set()
        self.used_v = set()
        self.lines = []
        self.targets = set()
        self.bail_points = []  # instruction addresses that may return NL_BAIL
        self.page = {}  # register -> known adrp page (basic-block local)
        self.region = cfg.get("kind") == "region"

    # -- register access
    def xr(self, tok):
        m = XR.match(tok)
        if tok == "sp" or tok == "wsp":
            return "sp", (64 if tok == "sp" else 32)
        if not m:
            raise LiftError("not an x/w register: " + tok)
        if m.group(2) == "zr":
            return "0", (64 if m.group(1) == "x" else 32)
        n = int(m.group(2))
        self.used_x.add(n)
        return f"x{n}", (64 if m.group(1) == "x" else 32)

    def rd(self, tok):  # value expression of a source register
        r, w = self.xr(tok)
        if r == "0":
            return "UINT64_C(0)"
        return r if w == 64 else f"(uint64_t)(uint32_t){r}"

    def entry_value(self, tok):
        """True if the register still holds its value from function entry (prologue spills)."""
        if FR.match(tok):
            return f"v{FR.match(tok).group(2)}" not in self.written
        r = self.xr(tok)[0]
        return r not in self.written and r not in ("0",)

    def wr(self, tok, expr):  # assignment of a destination register
        r, w = self.xr(tok)
        self.written.add(r)
        if r == "0":
            return f"(void)({expr});"
        self.page.pop(r, None)
        if w == 64:
            return f"{r} = (uint64_t)({expr});"
        return f"{r} = (uint64_t)(uint32_t)({expr});"

    def vreg(self, tok):
        m = VR.match(tok)
        if m:
            n = int(m.group(1)); self.used_v.add(n)
            return f"v{n}", m.group(2), None
        m = VE.match(tok)
        if m:
            n = int(m.group(1)); self.used_v.add(n)
            return f"v{n}", m.group(2), int(m.group(3))
        m = FR.match(tok)
        if m:
            n = int(m.group(2)); self.used_v.add(n)
            return f"v{n}", m.group(1), None
        raise LiftError("not a vector register: " + tok)

    # -- addressing
    def addr(self, ops, idx):
        """Returns (address expr, writeback statement or None, base reg, base page const or None, offset)."""
        a = ops[idx]
        if not a.startswith("["):
            raise LiftError("bad address " + a)
        pre = a.endswith("!")
        inner = a.strip("[]!").strip().rstrip("]")
        parts = [p.strip() for p in inner.split(",")]
        base = parts[0]
        breg = self.xr(base)[0]
        bexp = breg if breg != "0" else "UINT64_C(0)"
        off = 0
        offexp = None
        if len(parts) >= 2:
            if parts[1].startswith("#"):
                off = imm(parts[1])
            else:
                rexp = self.rd(parts[1])
                if len(parts) == 3:
                    mm = re.match(r"(lsl|uxtw|sxtw)\s*#?(\d*)", parts[2])
                    kind, sh = mm.group(1), int(mm.group(2) or 0)
                    if kind == "uxtw":
                        rexp = f"(uint64_t)(uint32_t){rexp}"
                    elif kind == "sxtw":
                        rexp = f"(uint64_t)(int64_t)(int32_t){rexp}"
                    if sh:
                        rexp = f"({rexp} << {sh})"
                offexp = rexp
        post = None
        if len(ops) > idx + 1:  # post-index
            post = imm(ops[idx + 1])
        ea = bexp
        if offexp:
            ea = f"({bexp} + {offexp})"
        elif off and post is None:
            ea = f"({bexp} + (uint64_t){off})"
        wb = None
        if pre:
            wb = f"{breg} = {ea};"
        elif post is not None:
            wb = f"{breg} = {bexp} + (uint64_t){post};"
        page = self.page.get(breg) if not offexp else None
        return ea, wb, breg, page, (off if post is None else 0)

    # -- emission helpers
    def emit(self, s):
        m = re.match(r"\s*([xv]\d+|sp) = ", s)
        if m and hasattr(self, "written"):
            self.written.add(m.group(1))
        self.lines.append("    " + s)

    def bail(self, a, why):
        self.bail_points.append(a)
        self.emit(f"return NL_BAIL; /* {why} */")

    # -- one instruction
    def lift(self, a, mn, ops_s):
        ops = split_ops(ops_s)
        e = self.emit
        if a in self.targets:
            self.page = {}
        # ---------------- branches
        if mn == "b" or mn.startswith("b."):
            t = imm(ops[0])
            e(self.jump(a, t, None if mn == "b" else self.cond(mn[2:])))
            return
        if mn in ("cbz", "cbnz"):
            t = imm(ops[1]); v = self.rd(ops[0]); op = "==" if mn == "cbz" else "!="
            e(self.jump(a, t, f"{v} {op} 0"))
            return
        if mn in ("tbz", "tbnz"):
            t = imm(ops[2]); v = self.rd(ops[0]); b = imm(ops[1]); op = "==" if mn == "tbz" else "!="
            e(self.jump(a, t, f"(({v} >> {b}) & 1) {op} 0"))
            return
        if mn == "ret" and self.region:
            self.used_x.add(30)
            e("exit_pc = x30; goto L_exit;")
            return
        if mn == "ret":
            for r in self.cfg["rets"]:
                e(f"R.{r[0]}[{r[1:]}] = {r};")
            e("return NL_OK;")
            return
        if mn == "bl":
            t = imm(ops[0])
            if t in INLINE_CALLS:
                e(INLINE_CALLS[t])
                return
            f = self.funcs.get(t)
            if not f and self.region:  # deopt: continue in the recompiled callee with the full state
                self.used_x.add(30)
                e(f"x30 = g_main + UINT64_C({a + 4:#x}); exit_pc = g_main + UINT64_C({t:#x}); goto L_exit;")
                return
            if not f:
                self.bail(a, f"call {t:#x}")
                return
            self.call_sites.append(a)
            self.used_x.add(30)
            e("{")
            e("  Regs Rc;")
            for r in f["args"]:
                kind, n = r[0], int(r[1:])
                (self.used_x if kind == "x" else self.used_v).add(n)
                e(f"  Rc.{kind}[{n}] = {r};")
            e(f"  Rc.sp = sp; x30 = UINT64_C({a + 4:#x}) + g_main;")
            if self.region:  # the lifted callee bailed before any store of its own: run the recompiled callee
                g = f.get("guard")
                if g:  # the island's precondition (checked by the hook for calls that come through the guest)
                    e(f"  if (!{g}<MM>(Rc)) {{ exit_pc = g_main + UINT64_C({t:#x}); goto L_exit; }}")
                e(f"  if (fn_{t:x}<MM>(Rc) != NL_OK) {{ exit_pc = g_main + UINT64_C({t:#x}); goto L_exit; }}")
            else:
                e(f"  if (fn_{t:x}<MM>(Rc) != NL_OK) return NL_BAIL;")
            for r in f["rets"]:
                kind, n = r[0], int(r[1:])
                (self.used_x if kind == "x" else self.used_v).add(n)
                e(f"  {r} = Rc.{kind}[{n}];")
            e("}")
            return
        # ---------------- integer
        if mn == "adrp":
            r = self.xr(ops[0])[0]
            p = imm(ops[1])
            e(f"{r} = g_main + UINT64_C({p:#x});")
            self.page[r] = p
            return
        if mn == "mov":
            d, s = ops
            if d.startswith("v") or s.startswith("v") or FR.match(d):
                return self.vmov(d, s)
            if s.startswith("#"):
                e(self.wr(d, f"UINT64_C({imm(s) & 0xFFFFFFFFFFFFFFFF:#x})"))
            else:
                e(self.wr(d, self.rd(s)))
            return
        if mn == "movk":
            sh = 0
            if len(ops) > 2:
                sh = int(re.match(r"lsl\s*#(\d+)", ops[2]).group(1))
            r = self.rd(ops[0])
            e(self.wr(ops[0], f"({r} & ~(UINT64_C(0xffff) << {sh})) | (UINT64_C({imm(ops[1]):#x}) << {sh})"))
            return
        if mn in ("sxth", "sxtb", "sxtw", "uxth", "uxtb"):
            t = {"sxth": "int16_t", "sxtb": "int8_t", "sxtw": "int32_t", "uxth": "uint16_t", "uxtb": "uint8_t"}[mn]
            e(self.wr(ops[0], f"(uint64_t)(int64_t)({t}){self.rd(ops[1])}"))
            return
        if mn in ("add", "sub", "and", "orr", "eor", "bic", "adds", "subs", "ands") and not ops[0].startswith("v"):
            return self.alu(a, mn, ops)
        if mn in ("lsl", "lsr", "asr"):
            d, n, m = ops
            w = self.xr(d)[1]
            v = self.rd(n)
            if m.startswith("#"):
                s = imm(m)
            else:
                s = f"({self.rd(m)} & {w - 1})"
            if mn == "lsl":
                e(self.wr(d, f"{v} << {s}"))
            elif mn == "lsr":
                e(self.wr(d, f"{v} >> {s}"))
            else:
                t = "int64_t" if w == 64 else "int32_t"
                e(self.wr(d, f"(uint64_t)(({t}){v} >> {s})"))
            return
        if mn in ("ubfx", "sbfx"):
            d, n, lsb, wid = ops
            lsb, wid = imm(lsb), imm(wid)
            w = self.xr(d)[1]
            if mn == "ubfx":
                e(self.wr(d, f"({self.rd(n)} >> {lsb}) & UINT64_C({(1 << wid) - 1:#x})"))
            else:
                e(self.wr(d, f"(uint64_t)(((int64_t)({self.rd(n)} << {64 - lsb - wid})) >> {64 - wid})"))
            return
        if mn in ("madd", "msub"):
            d, n, m, acc = ops
            op = "+" if mn == "madd" else "-"
            e(self.wr(d, f"{self.rd(acc)} {op} {self.rd(n)} * {self.rd(m)}"))
            return
        if mn == "mul":
            d, n, m = ops
            e(self.wr(d, f"{self.rd(n)} * {self.rd(m)}"))
            return
        if mn in ("cmp", "cmn", "tst"):
            return self.flags(mn, ops)
        # ---------------- loads / stores
        if mn in ("ldr", "ldur", "ldrh", "ldurh", "ldrsh", "ldrb", "ldrsb", "ldrsw", "ldp", "str", "stur", "strh",
                  "strb", "stp", "ld1r"):
            return self.mem(a, mn, ops)
        # ---------------- SIMD / FP
        return self.simd(a, mn, ops)

    def jump(self, a, t, cond):
        if self.cfg["start"] <= t < self.cfg["end"]:
            g = f"goto L_{t:x};"
        elif self.region:
            g = f"{{ exit_pc = g_main + UINT64_C({t:#x}); goto L_exit; }}"
        else:
            raise LiftError(f"{a:#x}: branch out of the function ({t:#x})")
        return g if cond is None else f"if ({cond}) {g}"

    def cond(self, c):
        return {"eq": "Z", "ne": "!Z", "lt": "(N != Vf)", "ge": "(N == Vf)", "gt": "(!Z && N == Vf)",
                "le": "(Z || N != Vf)", "hi": "(C && !Z)", "ls": "(!C || Z)", "hs": "C", "cs": "C", "lo": "!C",
                "cc": "!C", "mi": "N", "pl": "!N", "vs": "Vf", "vc": "!Vf"}[c]

    def operand2(self, ops, w):
        """Second operand of an ALU op: register (optionally shifted/extended) or immediate."""
        src = ops[0]
        if src.startswith("#"):
            v = f"UINT64_C({imm(src) & 0xFFFFFFFFFFFFFFFF:#x})"
            if len(ops) > 1:
                sh = int(re.match(r"lsl\s*#(\d+)", ops[1]).group(1))
                v = f"({v} << {sh})"
            return v
        v = self.rd(src)
        if len(ops) > 1:
            m = re.match(r"(lsl|lsr|asr|uxth|uxtb|uxtw|sxth|sxtb|sxtw)\s*#?(\d*)", ops[1])
            k, sh = m.group(1), int(m.group(2) or 0)
            if k == "lsl":
                v = f"({v} << {sh})"
            elif k == "lsr":
                v = f"({v} >> {sh})"
            elif k == "asr":
                v = f"(uint64_t)(({'int64_t' if w == 64 else 'int32_t'}){v} >> {sh})"
            else:
                t = {"uxth": "uint16_t", "uxtb": "uint8_t", "uxtw": "uint32_t", "sxth": "int16_t", "sxtb": "int8_t",
                     "sxtw": "int32_t"}[k]
                v = f"(uint64_t)(int64_t)({t}){v}"
                if sh:
                    v = f"({v} << {sh})"
        return v

    def alu(self, a, mn, ops):
        d = ops[0]
        w = self.xr(d)[1]
        n = self.rd(ops[1])
        m = self.operand2(ops[2:], w)
        base = mn.rstrip("s") if mn in ("adds", "subs", "ands") else mn
        expr = {"add": f"{n} + {m}", "sub": f"{n} - {m}", "and": f"{n} & {m}", "orr": f"{n} | {m}",
                "eor": f"{n} ^ {m}", "bic": f"{n} & ~{m}"}[base]
        if self.xr(d)[0] == "sp":
            self.emit(f"sp = {expr};")
            return
        if mn in ("adds", "subs", "ands"):
            self.flags({"adds": "cmn", "subs": "cmp", "ands": "tst"}[mn], ops[1:])
        self.emit(self.wr(d, expr))

    def flags(self, mn, ops):
        w = self.xr(ops[0])[1]
        n = self.rd(ops[0])
        m = self.operand2(ops[1:], w)
        e = self.emit
        if w == 32:
            n, m = f"(uint32_t)({n})", f"(uint32_t)({m})"
            T, S, BITS = "uint32_t", "int32_t", 31
        else:
            T, S, BITS = "uint64_t", "int64_t", 63
        if mn == "tst":
            e(f"{{ {T} r_ = {n} & {m}; N = (int)(r_ >> {BITS}); Z = r_ == 0; C = 0; Vf = 0; }}")
        elif mn == "cmp":
            e(f"{{ {T} a_ = {n}, b_ = {m}, r_ = a_ - b_; N = (int)(r_ >> {BITS}); Z = r_ == 0; C = a_ >= b_; "
              f"Vf = (int)((({S})((a_ ^ b_) & (a_ ^ r_))) < 0); }}")
        else:  # cmn
            e(f"{{ {T} a_ = {n}, b_ = {m}, r_ = a_ + b_; N = (int)(r_ >> {BITS}); Z = r_ == 0; C = r_ < a_; "
              f"Vf = (int)((({S})(~(a_ ^ b_) & (a_ ^ r_))) < 0); }}")

    def mem(self, a, mn, ops):
        e = self.emit
        if mn == "ld1r":
            v, arr, idx = self.vreg(ops[0].strip("{} ").strip())
            ea, wb, breg, page, off = self.addr(ops, 1)
            sz = 4 if arr in ("4s", "2s") else 8
            c = self.const_at(page, off, sz)
            val = c if c else f"ld{sz * 8}<MM>({ea})"
            if arr == "4s":
                e(f"{v} = nl::dupw4((uint32_t)({val}));")
            else:
                raise LiftError(f"{a:#x}: ld1r {arr}")
            if wb:
                e(wb)
            return
        pair = mn in ("ldp", "stp")
        nregs = 2 if pair else 1
        regs = ops[:nregs]
        ea, wb, breg, page, off = self.addr(ops, nregs)
        store = mn.startswith("st")
        if store and breg != "sp":
            self.global_stores.append(a)
        is_vec = bool(FR.match(regs[0]))
        if is_vec:
            kind = FR.match(regs[0]).group(1)
            sz = {"q": 16, "d": 8, "s": 4, "h": 2, "b": 1}[kind]
        else:
            r, w = self.xr(regs[0])
            sz = {"ldr": w // 8, "ldur": w // 8, "str": w // 8, "stur": w // 8, "ldp": w // 8, "stp": w // 8,
                  "ldrh": 2, "ldurh": 2, "ldrsh": 2, "strh": 2, "ldrb": 1, "ldrsb": 1, "strb": 1, "ldrsw": 4}[mn]
        for i, rt in enumerate(regs):
            eai = ea if i == 0 else f"({ea} + {sz * i})"
            if store and self.in_entry and breg == "sp" and self.entry_value(rt):
                e(f"/* spill of entry value {rt} elided */")
                continue
            if store:
                if is_vec:
                    v = self.vreg(rt)[0]
                    e(f"stv<MM>({eai}, {v}, {sz});")
                else:
                    e(f"st{sz * 8}<MM>({eai}, {self.rd(rt)});")
            else:
                if is_vec:
                    v = self.vreg(rt)[0]
                    c = self.const_at(page, off + sz * i, sz)
                    if c:
                        e(f"{v} = {c}; /* const */")
                    else:
                        e(f"{v} = ldv<MM>({eai}, {sz});")
                else:
                    val = f"ld{sz * 8}<MM>({eai})"
                    if mn == "ldrsh":
                        val = f"(uint64_t)(int64_t)(int16_t){val}"
                    elif mn == "ldrsb":
                        val = f"(uint64_t)(int64_t)(int8_t){val}"
                    elif mn == "ldrsw":
                        val = f"(uint64_t)(int64_t)(int32_t){val}"
                    e(self.wr(rt, val))
        if wb:
            if not store and not is_vec and any(self.xr(rt)[0] == breg for rt in regs):
                raise LiftError(f"{a:#x}: writeback load into its base register")
            e(wb)

    def const_at(self, page, off, sz):
        if page is None:
            return None
        b = self.img.const(page + off, sz)
        if b is None:
            return None
        b = b + bytes(16 - len(b))
        lo, hi = struct.unpack("<QQ", b)
        if sz <= 8:
            return f"nl::vconst(UINT64_C({lo:#x}), 0)"
        return f"nl::vconst(UINT64_C({lo:#x}), UINT64_C({hi:#x}))"

    def vmov(self, d, s):
        e = self.emit
        if XR.match(d):  # umov
            vs, as_, ids = self.vreg(s)
            e(self.wr(d, f"nl::lane32({vs}, {ids})" if as_ == "s" else f"nl::lane64({vs}, {ids})"))
            return
        vd, ad, idd = self.vreg(d)
        if s.startswith("#"):
            raise LiftError("mov imm to vector")
        if XR.match(s):
            raise LiftError("mov x->v")
        vs, as_, ids = self.vreg(s)
        if idd is not None:  # ins
            if ad == "s":
                e(f"{vd} = nl::ins_s({vd}, {idd}, {vs}, {ids});")
            elif ad == "d":
                e(f"{vd} = nl::ins_d({vd}, {idd}, {vs}, {ids});")
            else:
                raise LiftError("ins " + ad)
        elif ad in ("s", "d") and ids is not None:  # mov sD, vN.s[j]  (dup scalar)
            if ad == "s":
                e(f"{vd} = nl::scal_s({vs}, {ids});")
            else:
                e(f"{vd} = nl::vconst(nl::lane64({vs}, {ids}), 0);")
        elif ad == "16b":
            e(f"{vd} = {vs};")
        elif ad == "8b":
            e(f"{vd} = nl::lo64({vs});")
        else:
            raise LiftError(f"mov {d}, {s}")

    def simd(self, a, mn, ops):
        e = self.emit
        V = self.vreg

        def arr(tok):
            return V(tok)[1]

        if mn == "tbl":
            d = V(ops[0])
            tabs = [t.strip() for t in ops[1].strip("{} ").split(",")]
            idx = V(ops[2])
            w = "8" if d[1] == "8b" else "16"
            if len(tabs) == 1:
                e(f"{d[0]} = nl::tbl1_{w}({V(tabs[0])[0]}, {idx[0]});")
            elif len(tabs) == 2:
                e(f"{d[0]} = nl::tbl2_{w}({V(tabs[0])[0]}, {V(tabs[1])[0]}, {idx[0]});")
            else:
                raise LiftError("tbl with >2 tables")
            return
        if mn == "ext":
            d, n, m, k = V(ops[0]), V(ops[1]), V(ops[2]), imm(ops[3])
            e(f"{d[0]} = nl::ext{16 if d[1] == '16b' else 8}<{k}>({n[0]}, {m[0]});")
            return
        if mn in ("and", "orr", "eor", "bic", "bsl", "mvn"):
            d = V(ops[0])
            if mn == "mvn":
                r = f"nl::vmvn({V(ops[1])[0]})"
            elif mn == "bsl":
                r = f"nl::vbsl({d[0]}, {V(ops[1])[0]}, {V(ops[2])[0]})"
            else:
                r = f"nl::v{mn}({V(ops[1])[0]}, {V(ops[2])[0]})"
            e(f"{d[0]} = {r if d[1] == '16b' else 'nl::lo64(' + r + ')'};")
            return
        if mn == "dup":
            d = V(ops[0])
            if XR.match(ops[1]):
                if d[1] not in ("4s", "2s"):
                    raise LiftError("dup gpr " + d[1])
                r_ = f"nl::dupw4((uint32_t){self.rd(ops[1])})"
                e(f"{d[0]} = {r_ if d[1] == '4s' else 'nl::lo64(' + r_ + ')'};")
                return
            s = V(ops[1])
            e(f"{d[0]} = nl::dup{4 if d[1] == '4s' else 2}({s[0]}, {s[2]});")
            return
        if mn in ("zip1", "zip2"):
            d = V(ops[0])
            e(f"{d[0]} = nl::{mn}_{d[1]}({V(ops[1])[0]}, {V(ops[2])[0]});")
            return
        if mn == "movi":
            d = V(ops[0])
            val = imm(ops[1])
            if len(ops) > 2:
                val <<= int(re.match(r"lsl\s*#(\d+)", ops[2]).group(1))
            if d[1] in ("2d", "d"):
                if val not in (0,):
                    raise LiftError("movi 2d nonzero")
                e(f"{d[0]} = nl::vzero();")
            elif d[1] == "4s":
                e(f"{d[0]} = nl::dupw4(UINT32_C({val:#x}));")
            elif d[1] == "2s":
                e(f"{d[0]} = nl::lo64(nl::dupw4(UINT32_C({val:#x})));")
            else:
                raise LiftError("movi " + d[1])
            return
        if mn == "fmov":
            d = V(ops[0]) if not XR.match(ops[0]) else None
            if d is None:
                s_ = V(ops[1])
                if s_[1] != "s":
                    raise LiftError("fmov gpr <- " + s_[1])
                e(self.wr(ops[0], f"nl::lane32({s_[0]}, 0)"))
                return
            if ops[1].startswith("#"):
                bits = struct.unpack("<I", struct.pack("<f", float(ops[1].lstrip("#"))))[0]
                if d[1] == "4s":
                    e(f"{d[0]} = nl::dupw4(UINT32_C({bits:#x}));")
                elif d[1] == "2s":
                    e(f"{d[0]} = nl::lo64(nl::dupw4(UINT32_C({bits:#x})));")
                elif d[1] == "s":
                    e(f"{d[0]} = nl::fromw(UINT32_C({bits:#x}));")
                else:
                    raise LiftError("fmov imm " + d[1])
            elif XR.match(ops[1]):
                if d[1] != "s":
                    raise LiftError("fmov gpr " + d[1])
                e(f"{d[0]} = nl::fromw((uint32_t){self.rd(ops[1])});")
            else:
                s = V(ops[1])
                if d[1] == "s":
                    e(f"{d[0]} = nl::lo32({s[0]});")
                else:
                    raise LiftError("fmov reg " + d[1])
            return
        # scalar FP <-> flags / gpr
        if mn == "fcmp" or mn == "fcmpe":
            a_ = V(ops[0])
            b_ = "nl::vzero()" if ops[1].startswith("#") else V(ops[1])[0]
            if a_[1] != "s":
                raise LiftError("fcmp " + a_[1])
            e(f"nl::fcmp_s({a_[0]}, {b_}, N, Z, C, Vf);")
            return
        if mn == "fcsel":
            d, n_, m_ = V(ops[0]), V(ops[1]), V(ops[2])
            e(f"{d[0]} = nl::lo32(({self.cond(ops[3])}) ? {n_[0]} : {m_[0]});")
            return
        if mn == "fnmul":
            d, n_, m_ = V(ops[0]), V(ops[1]), V(ops[2])
            if d[1] != "s":
                raise LiftError("fnmul " + d[1])
            e(f"{d[0]} = nl::fnegs(nl::fmuls({n_[0]}, {m_[0]}));")
            return
        if mn == "fmov" and XR.match(ops[0]):
            s_ = V(ops[1])
            if s_[1] != "s":
                raise LiftError("fmov gpr <- " + s_[1])
            e(self.wr(ops[0], f"nl::lane32({s_[0]}, 0)"))
            return
        if mn == "fcvtzs" and XR.match(ops[0]):
            s_ = V(ops[1])
            if s_[1] != "s" or self.xr(ops[0])[1] != 32:
                raise LiftError("fcvtzs gpr form")
            e(self.wr(ops[0], f"nl::lane32(nl::fcvtzs4({s_[0]}), 0)"))
            return
        if mn == "scvtf" and XR.match(ops[1]):
            d = V(ops[0])
            if d[1] != "s" or self.xr(ops[1])[1] != 32:
                raise LiftError("scvtf gpr form")
            e(f"{d[0]} = nl::lo32(nl::scvtf4(nl::dupw4((uint32_t){self.rd(ops[1])})));")
            return
        # FP arithmetic
        if mn in ("fadd", "fsub", "fmul", "fdiv", "fmla", "fmls", "fabs", "fneg", "fsqrt", "fcmgt", "fcmge", "fcmeq",
                  "faddp", "frsqrts", "frsqrte", "scvtf", "fcvtzs"):
            d = V(ops[0])
            form = d[1]
            srcs = []
            for t in ops[1:]:
                if t.startswith("#"):
                    srcs.append(("nl::vzero()", None, None))
                else:
                    srcs.append(V(t))

            def val(s, broadcast):
                if s[2] is not None and broadcast:  # by element
                    return f"nl::dup4({s[0]}, {s[2]})"
                return s[0]

            vec = form in ("4s", "2s")
            if form == "s":  # scalar
                xs = [s[0] if s[2] is None else f"nl::dup4({s[0]}, {s[2]})" for s in srcs]
                f = {"fadd": "fadds", "fsub": "fsubs", "fmul": "fmuls", "fdiv": "fdivs", "fabs": "fabss",
                     "fneg": "fnegs", "fsqrt": "fsqrts_arm"}.get(mn)
                if not f:
                    raise LiftError(f"{a:#x}: scalar {mn}")
                e(f"{d[0]} = nl::{f}({', '.join(xs)});")
                return
            if not vec:
                raise LiftError(f"{a:#x}: {mn} {form}")
            w = "4" if form == "4s" else "2"
            xs = [val(s, True) for s in srcs]
            if mn in ("fmla", "fmls"):
                r = f"nl::{mn}4({d[0]}, {xs[0]}, {xs[1]})"
            elif mn == "faddp":
                r = f"nl::faddp{w}({xs[0]}, {xs[1]})"
                e(f"{d[0]} = {r};")
                return
            elif mn in ("fabs", "fneg", "fsqrt", "scvtf", "fcvtzs"):
                r = f"nl::{mn}4({xs[0]})"
            elif mn == "frsqrte":
                r = f"nl::frsqrte4({xs[0]}, g_fpcr)"
            else:
                r = f"nl::{mn}4({xs[0]}, {xs[1]})"
            e(f"{d[0]} = {r if w == '4' else 'nl::lo64(' + r + ')'};")
            return
        # unsupported: bail
        self.bail(a, f"unsupported {mn} {', '.join(ops)}")

    # -- whole function
    def run(self):
        for a, mn, ops in self.insns:
            if mn in ("b", "cbz", "cbnz") or mn.startswith("b."):
                try:
                    t = imm(split_ops(ops)[-1])
                    if self.cfg["start"] <= t < self.cfg["end"]:
                        self.targets.add(t)
                except ValueError:
                    pass
            if mn in ("tbz", "tbnz"):
                self.targets.add(imm(split_ops(ops)[2]))
        self.call_sites = []
        self.global_stores = []
        self.per_insn = {}
        self.written = set()
        self.in_entry = True
        for a, mn, ops in self.insns:
            if a in self.targets:
                self.in_entry = False
                self.lines.append(f"  L_{a:x}: NL_TRACE(0x{a:x});")
            start = len(self.lines)
            self.lift(a, mn, ops)
            self.per_insn[a] = (mn, ops)
            if mn in ("b", "bl", "ret", "cbz", "cbnz", "tbz", "tbnz", "br", "blr") or mn.startswith("b."):
                self.in_entry = False
        self.check_bails()
        return self.render()

    def succ(self, i):
        a, mn, ops = self.insns[i]
        nxt = self.insns[i + 1][0] if i + 1 < len(self.insns) else None
        if mn == "ret" or a in self.bail_set:
            return []
        if mn == "b":
            return [imm(ops)]
        if mn.startswith("b.") or mn in ("cbz", "cbnz", "tbz", "tbnz"):
            return [imm(split_ops(ops)[-1]), nxt]
        return [nxt] if nxt is not None else []

    def check_bails(self):
        """Forward dataflow: may a non-stack store of this invocation precede each bail point?"""
        self.bail_set = set(self.bail_points)
        idx = {a: i for i, (a, _, _) in enumerate(self.insns)}
        stored = {a: False for a, _, _ in self.insns}
        work = [self.insns[0][0]]
        seen = {self.insns[0][0]: False}
        gs = set(self.global_stores)
        state_in = {self.insns[0][0]: False}
        while work:
            a = work.pop()
            s = state_in[a]
            i = idx[a]
            out = s or (a in gs)
            for t in self.succ(i):
                if t is None or t not in idx:
                    continue
                if t not in state_in or (out and not state_in[t]):
                    state_in[t] = out or state_in.get(t, False)
                    work.append(t)
        bad = [a for a in self.bail_points + self.call_sites if state_in.get(a, False)
               and a not in self.cfg.get("allow_after_store", [])]
        self.dead = [a for a, _, _ in self.insns if a not in state_in]
        unreachable = [a for a in self.bail_points if a not in state_in]
        if bad:
            raise LiftError(f"{self.cfg['start']:#x}: bail/call after a global store at " +
                            ", ".join(f"{b:#x}" for b in bad))
        self.unreachable_bails = unreachable

    def render(self):
        c = self.cfg
        out = []
        inl = "NL_INLINE" if c["inline"] else "NL_NOINLINE"
        out.append(f"// {c['name']}: guest main+{c['start']:#x}..{c['end']:#x} ({len(self.insns)} instructions, "
                   f"{len(self.bail_points)} bail points)")
        if self.region:
            return self.render_region(out)
        out.append(f"template <class MM> {inl} int fn_{c['start']:x}(Regs& R) {{")
        for n in sorted(self.used_x):
            init = f"R.x[{n}]" if f"x{n}" in c["args"] else "0"
            out.append(f"    uint64_t x{n} = {init};")
        out.append("    uint64_t sp = R.sp;")
        for n in sorted(self.used_v):
            init = f"R.v[{n}]" if f"v{n}" in c["args"] else "nl::vzero()"
            out.append(f"    V v{n} = {init};")
        out.append("    int N = 0, Z = 0, C = 0, Vf = 0;")
        out.append("    (void)N; (void)Z; (void)C; (void)Vf;")
        out.extend(self.lines)
        out.append("}")
        return "\n".join(out)


def render_region(self, out):
    """Region: entered by a hook at a block inside a guest function, with the thread's full register state in the
    guest context; every exit (branch out of the range, ret, unsupported call) writes the registers it used back
    and leaves the guest pc to continue at."""
    c = self.cfg
    out.append(f"template <class MM> NL_NOINLINE int fn_{c['start']:x}(GC* c) {{")
    for n in sorted(self.used_x):
        out.append(f"    uint64_t x{n} = c->x[{n}];")
    out.append("    uint64_t sp = c->x[31];")
    for n in sorted(self.used_v):
        out.append(f"    V v{n} = _mm_loadu_si128((const V*)c->vreg[{n}]);")
    out.append("    int N = c->n, Z = c->z, C = c->c, Vf = c->v;")
    out.append("    uint64_t exit_pc = 0;")
    out.extend(self.lines)
    out.append("  L_exit:")
    for n in sorted(self.used_x):
        out.append(f"    c->x[{n}] = x{n};")
    out.append("    c->x[31] = sp;")
    for n in sorted(self.used_v):
        out.append(f"    _mm_storeu_si128((V*)c->vreg[{n}], v{n});")
    out.append("    c->n = (uint8_t)N; c->z = (uint8_t)Z; c->c = (uint8_t)C; c->v = (uint8_t)Vf;")
    out.append("    c->pc = exit_pc;")
    out.append("    return NL_EXIT;")
    out.append("}")
    return "\n".join(out)


Fn.render_region = render_region


def main():
    img = Image(sys.argv[1])
    funcs = {f["start"]: f for f in FUNCS}
    parts = ["// Generated by nlift.py from the game's main module. Do not edit.", ""]
    for f in reversed(FUNCS):  # callees first
        if f.get("kind") == "region":
            parts.append(f"template <class MM> NL_NOINLINE int fn_{f['start']:x}(GC* c);")
        else:
            parts.append(f"template <class MM> {'NL_INLINE' if f['inline'] else 'NL_NOINLINE'} int fn_{f['start']:x}(Regs& R);")
    parts.append("")
    for f in reversed(FUNCS):
        fn = Fn(f, img, funcs)
        parts.append(fn.run())
        parts.append("")
        print(f"{f['name']}: {len(fn.insns)} insns, bail points {len(fn.bail_points)}, calls {len(fn.call_sites)}, "
              f"global stores {len(fn.global_stores)}, unreachable insns {len(fn.dead)}", file=sys.stderr)
    open(sys.argv[2], "w", newline="\n").write("\n".join(parts))


if __name__ == "__main__":
    main()
