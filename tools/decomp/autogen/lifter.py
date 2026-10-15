"""Generic translator from linear code (no conditional branches) to C++, for functions that fit
no specific pattern.

Covers leaf functions and wrappers with calls (BL/BLR) and tail calls (B): loads and
stores with offset, integer arithmetic, comparisons with CSEL/CSET, scalar floating point,
globals (ADRP) and local variables on the stack. Each register is tracked as a C++
expression; loads and calls are materialized into temporaries at their point in the program
to preserve ordering with respect to stores.

The arguments of each direct call are the registers that the target actually reads
(emu.ArityOracle, the same analysis verify.py uses); for indirect calls x0..xk are passed up to
the last argument register written. The result is always verified with verify.py: if the
translation is not equivalent, the function is not marked.

If any unsupported instruction appears, the translator returns None (the function is left
for another pattern or for manual work).
"""
from __future__ import annotations

import re
import struct

from patterns import GOT_RANGES, Fn, hx, pattern

_ORACLE = None


def oracle():
    global _ORACLE
    if _ORACLE is None:
        from common import functions
        from emu import ArityOracle
        from image import load_image
        _ORACLE = ArityOracle(load_image(), functions())
    return _ORACLE


class Unsupported(Exception):
    pass


_MEMOP = re.compile(r"^\[(\w+)(?:, (#-?0x[0-9a-f]+|#-?\d+|\w+)(?:, (lsl|sxtw|uxtw|sxtx) #(\d+)|, (sxtw|uxtw))?)?\](!)?"
                    r"(?:, #(-?0x[0-9a-f]+|-?\d+))?$")
_COND = {"eq": "==", "ne": "!=", "hs": ">=", "cs": ">=", "lo": "<", "cc": "<", "hi": ">", "ls": "<=",
         "ge": ">=", "lt": "<", "gt": ">", "le": "<="}
_SIGNED = {"ge", "lt", "gt", "le"}
_INV = {"eq": "ne", "ne": "eq", "hs": "lo", "lo": "hs", "cs": "cc", "cc": "cs", "hi": "ls", "ls": "hi",
        "ge": "lt", "lt": "ge", "gt": "le", "le": "gt"}
_CALLEE_SAVED = {f"x{i}" for i in range(19, 31)} | {"x29", "x30", "fp", "lr"} | {f"d{i}" for i in range(8, 16)}


def _imm(s: str) -> int:
    s = s.strip().lstrip("#")
    return int(s, 0)


class Val:
    """Value of a register: C++ expression and type ('u64' for GPR, 'f32'/'f64' for FP)."""
    __slots__ = ("e", "t")

    def __init__(self, e, t="u64"):
        self.e, self.t = e, t


class Lifter:
    def __init__(self, m, d):
        self.m, self.d = m, d
        self.x: dict[int, Val | None] = {}      # 0..30
        self.v: dict[int, Val | None] = {}      # 0..31 (scalar)
        self.xparams: dict[int, int] = {}       # index -> width read (32/64)
        self.vparams: dict[int, str] = {}       # index -> f32/f64
        self.body: list[str] = []
        self.ntmp = 0
        self.flags = None                       # (kind, a, b, bits): 'cmp' | 'tst' | 'fcmp'
        self.last_x0 = None                      # 'x' | 'v' depending on the last result written
        self.wrote: set = set()                 # registers written ('x', i) / ('v', i)
        self.local_size = 0
        self.uses_stack = False
        self.sp = 0                             # current sp offset from entry
        self.fp = None                          # x29 offset from entry
        self.min_sp = 0

    # ------------------------------------------------------------- utilities
    def tmp(self, ctype, expr, prefix="t"):
        self.ntmp += 1
        n = f"{prefix}{self.ntmp}"
        self.body.append(f"const {ctype} {n} = {expr};")
        return n

    def reg(self, name: str) -> tuple[str, int, int]:
        """('x'|'v', index, bits) of a register name."""
        if name in ("xzr", "wzr"):
            return ("z", 0, 64 if name == "xzr" else 32)
        if name == "sp":
            return ("sp", 31, 64)
        if name in ("fp",):
            return ("x", 29, 64)
        if name in ("lr",):
            return ("x", 30, 64)
        k = name[0]
        idx = int(name[1:])
        if k == "x":
            return ("x", idx, 64)
        if k == "w":
            return ("x", idx, 32)
        if k in "sdbhq":
            return ("v", idx, {"s": 32, "d": 64, "b": 8, "h": 16, "q": 128}[k])
        raise Unsupported(name)

    def rx(self, name: str) -> str:
        """u64 expression of the integer register (w: the low 32 bits, as u64)."""
        k, i, bits = self.reg(name)
        if k == "z":
            return "0"
        if k == "sp":
            raise Unsupported("sp as a value")
        if i in (29, 30):
            raise Unsupported("fp/lr as a value")
        if i not in self.x:
            if i > 7 and i != 8:
                raise Unsupported(f"read of {name} before it is defined")
            if i == 8:
                # indirect result (sret) of this function: x8 is read on entry (see assemble)
                self.shared["sret_in"] = True
                self.x[8] = Val("a8")
                return "a8" if bits == 64 else "u64(u32(a8))"
            self.xparams[i] = max(self.xparams.get(i, 0), bits)
            self.x[i] = Val(f"a{i}")
        val = self.x[i]
        if val is None:
            raise Unsupported(f"{name} clobbered by a call")
        if bits == 32:
            if re.fullmatch(r"0x[0-9a-f]+|\d+", val.e):
                return hx(int(val.e, 0) & 0xFFFFFFFF)
            return f"u64(u32({val.e}))" if not val.e.startswith("u64(u32(") else val.e
        return val.e

    def rxs(self, name: str) -> tuple[str, int]:
        k, i, bits = self.reg(name)
        return self.rx(name), bits

    def wx(self, name: str, expr: str, materialize=False):
        k, i, bits = self.reg(name)
        if k == "z":
            return
        if k != "x" or i in (29, 30):
            raise Unsupported(f"write to {name}")
        if bits == 32:
            if re.fullmatch(r"0x[0-9a-f]+|\d+", expr):
                expr = hx(int(expr, 0) & 0xFFFFFFFF)
            elif not (expr.startswith("u64(u32(") or expr.startswith("u64((") and " ? 1 : 0)" in expr):
                expr = f"u64(u32({expr}))"
        if materialize or len(expr) > 60:
            expr = self.tmp("u64", expr)
        self.x[i] = Val(expr)
        self.wrote.add(("x", i))
        if i == 0:
            self.last_x0 = "x"
            self.void_call = None

    def rv(self, name: str) -> str:
        k, i, bits = self.reg(name)
        if k != "v" or bits not in (32, 64):
            raise Unsupported(name)
        t = "f32" if bits == 32 else "f64"
        if i not in self.v:
            if i > 7:
                raise Unsupported(f"read of {name} before it is defined")
            if self.vparams.get(i, t) != t:
                raise Unsupported("v read with two widths")
            self.vparams[i] = t
            self.v[i] = Val(f"{'s' if bits == 32 else 'd'}{i}", t)
        val = self.v[i]
        if val is None:
            raise Unsupported(f"{name} clobbered")
        if val.t != t:
            raise Unsupported("different FP width")
        return val.e

    def wv(self, name: str, expr: str, materialize=False):
        k, i, bits = self.reg(name)
        if k != "v" or bits not in (32, 64):
            raise Unsupported(name)
        t = "f32" if bits == 32 else "f64"
        if materialize or len(expr) > 60:
            expr = self.tmp(t, expr, "f")
        self.v[i] = Val(expr, t)
        self.wrote.add(("v", i))
        if i == 0:
            self.last_x0 = "v"

    # ------------------------------------------------------------- memory
    def addr(self, ops: str, ins) -> tuple[str, int | None, str | None]:
        """Address expression and write-back effect (pre/post-index).
        Returns (expr, local_off or None, base_reg_writeback)."""
        mm = _MEMOP.match(ops)
        if not mm:
            raise Unsupported(f"addressing mode {ops}")
        base, idx, ext1, sh1, ext2, pre, post = mm.groups()
        off = 0
        index_expr = None
        if idx is not None:
            if idx.startswith("#"):
                off = _imm(idx)
            else:
                ie = self.rx(idx)
                if ext1 == "sxtw" or ext2 == "sxtw":
                    ie = f"u64(s64(s32(u32({ie}))))"
                elif ext1 == "uxtw" or ext2 == "uxtw":
                    ie = f"u64(u32({ie}))"
                if sh1:
                    ie = f"({ie} << {sh1})"
                index_expr = ie
        wb = None
        if pre:
            wb = ("pre", off)
        elif post is not None:
            wb = ("post", _imm(post))
            off = 0
        if base in ("sp", "x29", "fp"):
            if index_expr is not None:
                raise Unsupported("index on the stack")
            cur = self.sp if base == "sp" else self.fp
            if cur is None:
                raise Unsupported("x29 without a frame")
            return None, cur + off, (base, wb)
        be = self.rx(base)
        e = be
        if index_expr is not None:
            e = f"{e} + {index_expr}"
        if off:
            e = f"{e} + {hx(off)}" if off > 0 else f"{e} - {hx(-off)}"
        if wb:
            return e, None, (base, wb)
        return e, None, None

    def apply_wb(self, wbinfo, ops):
        if not wbinfo:
            return
        base, wb = wbinfo
        if wb is None:
            return
        kind, delta = wb
        if base in ("sp",):
            self.sp += delta
            if self.sp < self.min_frame:
                raise Unsupported("sp below the frame")
            return
        if base in ("x29", "fp"):
            raise Unsupported("x29 writeback")
        be = self.rx(base)
        self.wx(base, f"{be} + {hx(delta)}" if delta >= 0 else f"{be} - {hx(-delta)}")

    def local(self, off: int, ctype: str) -> str:
        self.uses_stack = True
        return f"*reinterpret_cast<{ctype}*>(stack + {off - self.min_frame})"

    # ------------------------------------------------------------- comparisons
    def cond_expr(self, cond: str) -> str:
        if self.flags is None:
            raise Unsupported("condition without flags")
        kind, a, b, bits = self.flags
        if kind == "fcmp":
            fm = {"eq": "{a} == {b}", "ne": "{a} != {b}", "gt": "{a} > {b}", "ge": "{a} >= {b}",
                  "mi": "{a} < {b}", "ls": "{a} <= {b}", "lt": "!({a} >= {b})", "le": "!({a} > {b})",
                  "hi": "!({a} <= {b})", "pl": "!({a} < {b})", "hs": "!({a} < {b})", "lo": "{a} < {b}",
                  "cc": "{a} < {b}", "cs": "!({a} < {b})"}
            if cond not in fm:
                raise Unsupported(f"fcmp {cond}")
            return "(" + fm[cond].format(a=a, b=b) + ")"
        if kind == "tst":
            if cond == "eq":
                return f"(({a} & {b}) == 0)"
            if cond == "ne":
                return f"(({a} & {b}) != 0)"
            if cond in ("mi", "lt"):
                st = "s32" if bits == 32 else "s64"
                return f"({st}({a} & {b}) < 0)"
            if cond in ("pl", "ge"):
                st = "s32" if bits == 32 else "s64"
                return f"({st}({a} & {b}) >= 0)"
            raise Unsupported(f"tst {cond}")
        if cond not in _COND:
            raise Unsupported(f"cond {cond}")
        if kind == "cmn":
            b = f"(0 - {b})"
        if bits == 32:
            ty = "s32" if cond in _SIGNED else "u32"
        else:
            ty = "s64" if cond in _SIGNED else "u64"
        if kind == "cmn" and cond not in ("eq", "ne"):
            raise Unsupported("cmn with ordering")
        def c(x):
            if ty == "u64" or (re.fullmatch(r"0x[0-9a-f]+|\d+", x) and int(x, 0) < 0x80000000):
                return x
            return f"{ty}({x})"
        return f"({c(a)} {_COND[cond]} {c(b)})"

    # ------------------------------------------------------------- operands
    def op2(self, parts: list[str], bits: int) -> str:
        """Second operand: immediate, register or shifted/extended register."""
        p = parts[0]
        if p.startswith("#"):
            v = _imm(p)
            if len(parts) > 1 and parts[1].startswith("lsl"):
                v <<= _imm(parts[1].split()[1])
            return hx(v & ((1 << bits) - 1)) if v >= 0 else hx(v & ((1 << bits) - 1))
        e = self.rx(p)
        if len(parts) > 1:
            sh = parts[1].split()
            kind = sh[0]
            amt = _imm(sh[1]) if len(sh) > 1 else 0
            if kind == "lsl":
                e = f"({e} << {amt})"
            elif kind == "lsr":
                e = f"({e} >> {amt})" if bits == 64 else f"u64(u32({e}) >> {amt})"
            elif kind == "asr":
                e = f"u64(s64({e}) >> {amt})" if bits == 64 else f"u64(u32(s32(u32({e})) >> {amt}))"
            elif kind == "sxtw":
                e = f"u64(s64(s32(u32({e}))) << {amt})"
            elif kind == "uxtw":
                e = f"(u64(u32({e})) << {amt})"
            elif kind == "sxth":
                e = f"u64(s64(s16(u16({e}))) << {amt})"
            elif kind == "sxtb":
                e = f"u64(s64(s8(u8({e}))) << {amt})"
            elif kind == "uxtb":
                e = f"(u64(u8({e})) << {amt})"
            elif kind == "uxth":
                e = f"(u64(u16({e})) << {amt})"
            else:
                raise Unsupported(kind)
        return e

    # ------------------------------------------------------------- translation
    def run(self):
        m = self.m
        insns = m.insns
        n = len(insns)
        # 1) prologue: saving of callee-saved registers / frame
        self.frame_slots = set()
        self.pending_zero = []
        i = 0
        while i < n:
            ins = insns[i]
            mn, ops = ins.mnemonic, ins.op_str
            parts = [p.strip() for p in ops.split(",")]
            if mn == "sub" and parts[0] == "sp" and parts[1] == "sp":
                self.sp -= _imm(parts[2])
            elif mn in ("stp", "str") and "[sp" in ops and all(p in _CALLEE_SAVED or p in ("x29", "x30")
                                                                for p in parts[:2 if mn == "stp" else 1]):
                e, off, wbi = self.addr(", ".join(parts[2 if mn == "stp" else 1:]), ins)
                if wbi and wbi[1] and wbi[1][0] == "pre":
                    self.sp += wbi[1][1]
                    off = self.sp
                for k in range(2 if mn == "stp" else 1):
                    self.frame_slots.add(off + 8 * k)
            elif (mn == "stp" and "[sp" in ops and "!" not in ops and "], #" not in ops
                  and sorted([parts[0] in ("xzr",), parts[1] in ("xzr",)]) == [False, True]
                  and (parts[0] if parts[1] == "xzr" else parts[1]) in _CALLEE_SAVED):
                # stp xzr, x19, [sp, #8]: saves x19 and zeroes a local variable
                e, off, wbi = self.addr(", ".join(parts[2:]), ins)
                kz = 0 if parts[0] == "xzr" else 1
                self.frame_slots.add(off + 8 * (1 - kz))
                self.pending_zero.append(off + 8 * kz)
            elif mn == "mov" and parts[0] == "x29" and parts[1] == "sp":
                self.fp = self.sp
            elif mn == "add" and parts[0] == "x29" and parts[1] == "sp":
                self.fp = self.sp + _imm(parts[2])
            else:
                break
            i += 1
        self.min_sp = self.sp
        self.min_frame = self.sp
        self.frame_size = -self.sp
        for off in self.pending_zero:
            self.body.append(f"{self.local(off, 'u64')} = 0;")
        # 2) body: walk over paths (forward conditional branches = if with its full path)
        self.idx_of = {ins.address: k for k, ins in enumerate(insns)}
        self.shared = {"forks": 0, "kinds": [], "steps": 0}
        self.visited = set()
        lines = self.walk(i)
        return self.assemble(lines)

    # ------------------------------------------------------------- paths
    MAX_FORKS = 20
    MAX_LINES = 80

    def fork(self):
        """Copy of the state to follow the path of the taken branch (own body)."""
        import copy
        self.materialize_void_call()
        c = copy.copy(self)
        c.x, c.v = dict(self.x), dict(self.v)
        c.xparams, c.vparams = dict(self.xparams), dict(self.vparams)
        c.wrote = set(self.wrote)
        c.visited = set(self.visited)
        c.body = []
        c.tail, c.ret, c.void_call = None, False, None
        return c

    def join_child(self, c):
        for k, b in c.xparams.items():
            self.xparams[k] = max(self.xparams.get(k, 0), b)
        for k, t in c.vparams.items():
            if self.vparams.get(k, t) != t:
                raise Unsupported("v read with two widths")
            self.vparams[k] = t
        self.ntmp = c.ntmp
        self.uses_stack = self.uses_stack or c.uses_stack

    def materialize_void_call(self):
        """If x0 is still the result of the last 'void' call, declare it as u64 and
        save the result (a later path may return it)."""
        vc = getattr(self, "void_call", None)
        if vc is None or self.x.get(0, 1) is not None:
            return
        k, t, cparams, cvals = vc
        f = self.d.fn(t, "u64", ", ".join(cparams), raw=True)
        self.ntmp += 1
        r = f"r{self.ntmp}"
        self.body[k] = f"[[maybe_unused]] const u64 {r} = {f}({', '.join(cvals)});"
        self.x[0] = Val(r)
        self.void_call = None

    def branch_cond(self, ins) -> str:
        mn, ops = ins.mnemonic, ins.op_str
        parts = [q.strip() for q in ops.split(",")]
        if mn in ("cbz", "cbnz"):
            e = self.rx(parts[0])
            return f"({e} {'==' if mn == 'cbz' else '!='} 0)"
        if mn in ("tbz", "tbnz"):
            e = self.rx(parts[0])
            bit = _imm(parts[1])
            return f"((({e} >> {bit}) & 1) {'==' if mn == 'tbz' else '!='} 0)"
        if mn.startswith("b."):
            return self.cond_expr(mn[2:])
        raise Unsupported(mn)

    def noreturn_target(self, t: int, i: int) -> bool:
        from emu import is_noreturn_import
        from image import load_image
        im = load_image()
        name = im.plt.get(t) or im.imports.get(t)
        return is_noreturn_import(name) or i == len(self.m.insns) - 1

    def walk(self, i: int) -> list:
        insns = self.m.insns
        n = len(insns)
        while True:
            if i >= n:
                raise Unsupported("path without an end")
            ins = insns[i]
            mn, ops = ins.mnemonic, ins.op_str
            self._cur = i
            if mn in ("ldaxr", "ldxr"):
                i = self.atomic_rmw(i)
                continue
            # Backward branches: allowed if they do not form a loop (the path does not repeat an instruction);
            # the target code is duplicated in each path.
            if i in self.visited:
                raise Unsupported("loop")
            self.visited.add(i)
            self.shared["steps"] += 1
            if self.shared["steps"] > 400:
                raise Unsupported("too many instructions walked")
            if mn in ("cbz", "cbnz", "tbz", "tbnz") or mn.startswith("b."):
                t = int(ops.split("#")[-1], 16)
                if t not in self.idx_of:
                    raise Unsupported("conditional branch outside the function")
                self.shared["forks"] += 1
                if self.shared["forks"] > self.MAX_FORKS:
                    raise Unsupported("too many paths")
                cond = self.branch_cond(ins)
                c = self.fork()
                sub = c.walk(self.idx_of[t])
                self.join_child(c)
                self.body.append(f"if {cond} {{")
                self.body.extend("    " + b for b in sub)
                self.body.append("}")
                i += 1
                continue
            if mn == "b":
                t = int(ops[1:], 16)
                if self.m.addr <= t < self.m.addr + self.m.size:
                    if t not in self.idx_of:
                        raise Unsupported("branch into the middle of an instruction")
                    i = self.idx_of[t]
                    continue
                self.call(t, tail=True)
                return self.end_path()
            if mn == "ret":
                self.ret = True
                return self.end_path()
            if mn == "br":
                self.call_indirect(ops, tail=True)
                return self.end_path()
            if mn == "bl":
                t = int(ops[1:], 16)
                if self.noreturn_target(t, i):
                    k = -1
                    for j in range(8):
                        if self.x.get(j) is not None and (("x", j) in self.wrote or j in self.xparams):
                            k = j
                    vals = [self.rx(f"x{j}") for j in range(k + 1)]
                    f = self.d.fn(t, "void", ", ".join(["u64"] * (k + 1)), noreturn=True, raw=True)
                    self.body.append(f"{f}({', '.join(vals)});")
                    self.shared["kinds"].append("noreturn")
                    return list(self.body)
            self.step(ins, False)
            i += 1

    def atomic_rmw(self, i: int) -> int:
        """ldaxr/ldxr r,[a]; add|sub|subs r,r,#k; stlxr|stxr st,r,[a]; cbnz st, <ldaxr>
        = __atomic_{add,sub}_fetch. Returns the index of the next instruction."""
        ins = self.m.insns[i:i + 4]
        if len(ins) < 4:
            raise Unsupported("standalone ldaxr")
        l, op, stx, br = ins
        lp = [q.strip() for q in l.op_str.split(",", 1)]
        opp = [q.strip() for q in op.op_str.split(",")]
        sp_ = [q.strip() for q in stx.op_str.split(",", 2)]
        bp = [q.strip() for q in br.op_str.split(",")]
        r, mem = lp[0], lp[1]
        if (op.mnemonic not in ("add", "sub", "subs", "adds") or len(opp) != 3 or opp[0] != r or opp[1] != r
                or not opp[2].startswith("#") or stx.mnemonic not in ("stlxr", "stxr") or sp_[1] != r
                or sp_[2] != mem or br.mnemonic != "cbnz" or bp[0] != sp_[0]
                or int(bp[1].lstrip("#"), 16) != l.address):
            raise Unsupported("ldaxr without the atomic add pattern")
        k = _imm(opp[2])
        bits = 64 if r.startswith("x") else 32
        ut = "u64" if bits == 64 else "u32"
        acq, rel = l.mnemonic == "ldaxr", stx.mnemonic == "stlxr"
        order = {(True, True): "__ATOMIC_ACQ_REL", (True, False): "__ATOMIC_ACQUIRE",
                 (False, True): "__ATOMIC_RELEASE", (False, False): "__ATOMIC_RELAXED"}[(acq, rel)]
        e, off, wbi = self.addr(mem, None)
        if e is None or wbi:
            raise Unsupported("atomic on the stack")
        fn = "__atomic_add_fetch" if op.mnemonic.startswith("add") else "__atomic_sub_fetch"
        t = self.tmp("u64", f"u64({fn}(reinterpret_cast<{ut}*>({e}), {ut}({hx(k)}), {order}))")
        self.wx(r, t)
        self.wx(sp_[0], "0")
        if op.mnemonic == "subs":
            self.flags = ("cmp", f"{t} + {hx(k)}", hx(k), bits)
        elif op.mnemonic == "adds":
            self.flags = ("cmn", f"{t} - {hx(k)}", hx(k), bits)
        for j in range(i, i + 4):
            self.visited.add(j)
        return i + 4

    def end_path(self) -> list:
        if self.sp != 0:
            raise Unsupported("unbalanced sp")
        lines, kind = self.ret_stmt()
        self.shared["kinds"].append(kind)
        return list(self.body) + lines

    def step(self, ins, last):
        mn, ops = ins.mnemonic, ins.op_str
        parts = [p.strip() for p in re.split(r",(?![^\[]*\])", ops)] if ops else []
        # epilogue: restoring callee-saved registers and adjusting sp
        if mn in ("ldp", "ldr") and ops and "[sp" in ops and all(p in _CALLEE_SAVED for p in
                                                                 parts[:2 if mn == "ldp" else 1]):
            mem = ", ".join(parts[2 if mn == "ldp" else 1:])
            e, off, wbi = self.addr(mem, ins)
            regs = parts[:2 if mn == "ldp" else 1]
            slots = [off is not None and off + 8 * k in self.frame_slots for k in range(len(regs))]
            if not any(slots) and not wbi[1]:
                # normal load of a local variable into a callee-saved register
                h = getattr(self, "i_" + mn)
                return h(parts, ins)
            if not all(slots):
                raise Unsupported("callee-saved load outside its slot")
            for r in regs:
                if r not in ("x29", "x30"):
                    k, idx, bits = self.reg(r)
                    (self.v if k == "v" else self.x).pop(idx, None)
            self.apply_wb(wbi, mem)
            return
        if mn == "add" and parts[:2] == ["sp", "sp"]:
            self.sp += _imm(parts[2])
            return
        # late prologue (shrink-wrapping: the frame is created after an early exit)
        if mn == "sub" and parts[:2] == ["sp", "sp"]:
            if self.uses_stack:
                raise Unsupported("late frame with locals")
            self.sp -= _imm(parts[2])
            self.min_frame = min(self.min_frame, self.sp)
            self.shared["min_frame"] = min(self.shared.get("min_frame", 0), self.min_frame)
            return
        if mn in ("stp", "str") and ops and "[sp" in ops:
            regs = parts[:2 if mn == "stp" else 1]
            if all(r in _CALLEE_SAVED and (r in ("x29", "x30", "fp", "lr")
                                           or int(r[1:]) not in (self.v if r[0] == "d" else self.x))
                   for r in regs):
                if self.uses_stack:
                    raise Unsupported("late frame with locals")
                e, off, wbi = self.addr(", ".join(parts[len(regs):]), ins)
                if wbi and wbi[1] and wbi[1][0] == "pre":
                    self.sp += wbi[1][1]
                    off = self.sp
                elif wbi and wbi[1]:
                    raise Unsupported("post-index in the prologue")
                self.min_frame = min(self.min_frame, self.sp)
                self.shared["min_frame"] = min(self.shared.get("min_frame", 0), self.min_frame)
                for k in range(len(regs)):
                    self.frame_slots.add(off + 8 * k)
                return
        if mn in ("mov", "add") and parts[:2] == ["x29", "sp"]:
            self.fp = self.sp + (_imm(parts[2]) if mn == "add" else 0)
            return
        if mn == "nop":
            return
        if mn == "ret":
            if not last:
                raise Unsupported("ret in the middle")
            self.ret = True
            return
        if mn == "b":
            t = int(ops[1:], 16)
            if not last or self.m.addr <= t < self.m.addr + self.m.size:
                raise Unsupported("internal branch")
            self.call(t, tail=True)
            return
        if mn == "bl":
            self.call(int(ops[1:], 16), tail=False)
            return
        if mn in ("blr", "br"):
            # Indirect calls: it is not known which registers the target reads (it could use arguments
            # that the function merely passes through) and verify.py only compares x0 for unknown targets.
            # Therefore x0..x7 and d0..d7 are ALWAYS passed as they are (those the function did not touch
            # are parameters being passed through): the target sees the same registers as in the original.
            self.call_indirect(ops, tail=(mn == "br"))
            return
        h = getattr(self, "i_" + mn.replace(".", "_"), None)
        if h is None:
            raise Unsupported(mn)
        h(parts, ins)

    # --- moves
    def i_mov(self, p, ins):
        d, s = p[0], p[1]
        k, i, bits = self.reg(d)
        if k == "v":
            raise Unsupported("vector mov")
        if s.startswith("#"):
            v = _imm(s) & ((1 << bits) - 1)
            self.wx(d, hx(v))
        elif s == "sp":
            off = self.sp
            self.uses_stack = True
            self.wx(d, f"reinterpret_cast<u64>(stack + {off - self.min_frame})")
        else:
            self.wx(d, self.rx(s))

    def i_movz(self, p, ins):
        k, i, bits = self.reg(p[0])
        v = _imm(p[1])
        if len(p) > 2:
            v <<= _imm(p[2].split()[1])
        self.wx(p[0], hx(v & ((1 << bits) - 1)))

    def i_movn(self, p, ins):
        k, i, bits = self.reg(p[0])
        v = _imm(p[1])
        if len(p) > 2:
            v <<= _imm(p[2].split()[1])
        self.wx(p[0], hx(~v & ((1 << bits) - 1)))

    def i_movk(self, p, ins):
        k, i, bits = self.reg(p[0])
        v = _imm(p[1])
        sh = _imm(p[2].split()[1]) if len(p) > 2 else 0
        cur = self.rx(p[0])
        if re.fullmatch(r"0x[0-9a-f]+|\d+", cur):
            nv = (int(cur, 0) & ~(0xFFFF << sh)) | (v << sh)
            self.wx(p[0], hx(nv & ((1 << bits) - 1)))
        else:
            self.wx(p[0], f"(({cur}) & {hx(~(0xFFFF << sh) & ((1 << bits) - 1))}) | {hx(v << sh)}")

    def i_adrp(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.x[i] = Val(f"PAGE:{_imm(p[1])}")
        self.wrote.add(("x", i))

    def _page(self, name):
        k, i, bits = self.reg(name)
        v = self.x.get(i)
        if v is not None and v.e.startswith("PAGE:"):
            return int(v.e[5:])
        return None

    def gaddr(self, a: int, got: bool = False) -> str:
        """u64 expression of the address of a global (or of the function/datum pointed to by the GOT)."""
        from image import load_image
        from common import functions
        im = load_image()
        if got:
            tgt = im.u64(a)
            if tgt in im.imports:
                n = self.d.import_fn(tgt, "void", "")
                return f"reinterpret_cast<u64>(&{n})"
            f = functions().at(tgt)
            if f is not None:
                n = self.d.fn(tgt, "void", "", got=True)
                return f"reinterpret_cast<u64>(&{n})"
            n = self.d.data(tgt, "u8", True, got=True)
            return f"reinterpret_cast<u64>({n})"
        if a == 0x71042f1798:   # already declared in include/auto/Opaque.h (default memory resource)
            return "reinterpret_cast<u64>(&DAT_71042f1798)"
        from common import functions as fs
        f = fs().at(a)
        if f is not None:
            n = self.d.fn(a, "void", "")
            return f"reinterpret_cast<u64>(&{n})"
        n = self.d.data(a, "u8", True)
        return f"reinterpret_cast<u64>({n})"

    # --- arithmetic
    def _bin(self, p, op, signed_shift=False):
        k, i, bits = self.reg(p[0])
        a = self.rx(p[1])
        b = self.op2(p[2:], bits)
        self.wx(p[0], f"{a} {op} {b}")

    def i_add(self, p, ins):
        page = self._page(p[1]) if len(p) >= 3 else None
        if page is not None and p[2].startswith("#"):
            k, i, bits = self.reg(p[0])
            self.x[i] = Val(self.gaddr(page + _imm(p[2])))
            self.wrote.add(("x", i))
            return
        if p[1] == "sp" or (p[1] in ("x29", "fp")):
            base = self.sp if p[1] == "sp" else self.fp
            if base is None or not p[2].startswith("#"):
                raise Unsupported("add sp")
            self.uses_stack = True
            self.wx(p[0], f"reinterpret_cast<u64>(stack + {base + _imm(p[2]) - self.min_frame})")
            return
        self._bin(p, "+")

    def i_sub(self, p, ins):
        if p[1] in ("sp", "x29", "fp"):
            base = self.sp if p[1] == "sp" else self.fp
            if base is None or not p[2].startswith("#"):
                raise Unsupported("sub sp")
            self.uses_stack = True
            self.wx(p[0], f"reinterpret_cast<u64>(stack + {base - _imm(p[2]) - self.min_frame})")
            return
        self._bin(p, "-")

    def i_and(self, p, ins):
        self._bin(p, "&")

    def i_orr(self, p, ins):
        if p[1] in ("wzr", "xzr"):
            k, i, bits = self.reg(p[0])
            self.wx(p[0], self.op2(p[2:], bits))
            return
        self._bin(p, "|")

    def i_eor(self, p, ins):
        self._bin(p, "^")

    def i_mul(self, p, ins):
        self._bin(p, "*")

    def i_madd(self, p, ins):
        self.wx(p[0], f"{self.rx(p[3])} + {self.rx(p[1])} * {self.rx(p[2])}")

    def i_msub(self, p, ins):
        self.wx(p[0], f"{self.rx(p[3])} - {self.rx(p[1])} * {self.rx(p[2])}")

    def i_neg(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.wx(p[0], f"0 - {self.op2(p[1:], bits)}")

    def i_mvn(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.wx(p[0], f"~{self.op2(p[1:], bits)}")

    def i_lsl(self, p, ins):
        k, i, bits = self.reg(p[0])
        if not p[2].startswith("#"):
            return self._shift_reg(p, "lsl")
        self.wx(p[0], f"({self.rx(p[1])} << {_imm(p[2])})")

    def i_lsr(self, p, ins):
        k, i, bits = self.reg(p[0])
        if not p[2].startswith("#"):
            return self._shift_reg(p, "lsr")
        self.wx(p[0], f"({self.rx(p[1])} >> {_imm(p[2])})")

    def i_asr(self, p, ins):
        k, i, bits = self.reg(p[0])
        if not p[2].startswith("#"):
            return self._shift_reg(p, "asr")
        st = "s64" if bits == 64 else "s32"
        ut = "u64" if bits == 64 else "u32"
        self.wx(p[0], f"u64({ut}({st}({ut}({self.rx(p[1])})) >> {_imm(p[2])}))")

    def i_ubfx(self, p, ins):
        lsb, w = _imm(p[2]), _imm(p[3])
        self.wx(p[0], f"(({self.rx(p[1])} >> {lsb}) & {hx((1 << w) - 1)})")

    def i_sxtw(self, p, ins):
        self.wx(p[0], f"u64(s64(s32(u32({self.rx(p[1])}))))")

    def i_sxth(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.wx(p[0], f"u64(s64(s16(u16({self.rx(p[1])}))))")

    def i_sxtb(self, p, ins):
        self.wx(p[0], f"u64(s64(s8(u8({self.rx(p[1])}))))")

    def i_uxtb(self, p, ins):
        self.wx(p[0], f"u64(u8({self.rx(p[1])}))")

    def i_uxth(self, p, ins):
        self.wx(p[0], f"u64(u16({self.rx(p[1])}))")

    # --- comparisons
    def i_cmp(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.flags = ("cmp", self.rx(p[0]), self.op2(p[1:], bits), bits)

    def i_cmn(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.flags = ("cmn", self.rx(p[0]), self.op2(p[1:], bits), bits)

    def i_tst(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.flags = ("tst", self.rx(p[0]), self.op2(p[1:], bits), bits)

    def i_cset(self, p, ins):
        self.wx(p[0], f"u64({self.cond_expr(p[1])} ? 1 : 0)")

    def i_csetm(self, p, ins):
        k, i, bits = self.reg(p[0])
        self.wx(p[0], f"({self.cond_expr(p[1])} ? {hx((1 << bits) - 1)} : 0)")

    def i_csel(self, p, ins):
        self.wx(p[0], f"({self.cond_expr(p[3])} ? {self.rx(p[1])} : {self.rx(p[2])})")

    def i_csinc(self, p, ins):
        self.wx(p[0], f"({self.cond_expr(p[3])} ? {self.rx(p[1])} : {self.rx(p[2])} + 1)")

    def i_csinv(self, p, ins):
        self.wx(p[0], f"({self.cond_expr(p[3])} ? {self.rx(p[1])} : ~{self.rx(p[2])})")

    def i_csneg(self, p, ins):
        self.wx(p[0], f"({self.cond_expr(p[3])} ? {self.rx(p[1])} : 0 - {self.rx(p[2])})")

    def i_cinc(self, p, ins):
        self.wx(p[0], f"({self.cond_expr(p[2])} ? {self.rx(p[1])} + 1 : {self.rx(p[1])})")

    def i_cneg(self, p, ins):
        self.wx(p[0], f"({self.cond_expr(p[2])} ? 0 - {self.rx(p[1])} : {self.rx(p[1])})")

    def i_smaddl(self, p, ins):
        self.wx(p[0], f"{self.rx(p[3])} + u64(s64(s32(u32({self.rx(p[1])}))) * s64(s32(u32({self.rx(p[2])}))))")

    def i_umaddl(self, p, ins):
        self.wx(p[0], f"{self.rx(p[3])} + u64(u32({self.rx(p[1])})) * u64(u32({self.rx(p[2])}))")

    def i_smsubl(self, p, ins):
        self.wx(p[0], f"{self.rx(p[3])} - u64(s64(s32(u32({self.rx(p[1])}))) * s64(s32(u32({self.rx(p[2])}))))")

    def i_smull(self, p, ins):
        self.wx(p[0], f"u64(s64(s32(u32({self.rx(p[1])}))) * s64(s32(u32({self.rx(p[2])}))))")

    def i_umull(self, p, ins):
        self.wx(p[0], f"u64(u32({self.rx(p[1])})) * u64(u32({self.rx(p[2])}))")

    def i_subs(self, p, ins):
        k, i, bits = self.reg(p[0])
        a = self.rx(p[1])
        b = self.op2(p[2:], bits)
        self.flags = ("cmp", a, b, bits)
        self.wx(p[0], f"{a} - {b}", materialize=True)

    def i_adds(self, p, ins):
        k, i, bits = self.reg(p[0])
        a = self.rx(p[1])
        b = self.op2(p[2:], bits)
        self.flags = ("cmn", a, b, bits)
        self.wx(p[0], f"{a} + {b}", materialize=True)

    def i_ands(self, p, ins):
        k, i, bits = self.reg(p[0])
        a = self.rx(p[1])
        b = self.op2(p[2:], bits)
        self.flags = ("tst", a, b, bits)
        self.wx(p[0], f"{a} & {b}", materialize=True)

    def _shift_reg(self, p, kind):
        k, i, bits = self.reg(p[0])
        a = self.rx(p[1])
        n = f"({self.rx(p[2])} & {bits - 1})"
        ut, st = ("u64", "s64") if bits == 64 else ("u32", "s32")
        if kind == "lsl":
            self.wx(p[0], f"({a} << {n})")
        elif kind == "lsr":
            self.wx(p[0], f"u64({ut}({a}) >> {n})")
        else:
            self.wx(p[0], f"u64({ut}({st}({ut}({a})) >> {n}))")

    def i_mrs(self, p, ins):
        if p[1] != "tpidr_el0":
            raise Unsupported("mrs")
        self.wx(p[0], "reinterpret_cast<u64>(__builtin_thread_pointer())", materialize=True)

    def i_adr(self, p, ins):
        self.wx(p[0], self.gaddr(_imm(p[1])))

    def i_ldar(self, p, ins):
        self._acq(p, None)

    def i_ldarb(self, p, ins):
        self._acq(p, "u8")

    def i_ldarh(self, p, ins):
        self._acq(p, "u16")

    def _acq(self, p, ctype):
        k, i, bits = self.reg(p[0])
        if k != "x":
            raise Unsupported("ldar FP")
        ctype = ctype or ("u64" if bits == 64 else "u32")
        mem = ", ".join(p[1:])
        ga = self._page_mem(mem)
        if ga is not None:
            e = self.gaddr(ga)
        else:
            e, off, wbi = self.addr(mem, None)
            if e is None or wbi:
                raise Unsupported("ldar on the stack")
        self.wx(p[0], f"lda<{ctype}>({e})", materialize=True)

    def i_stlr(self, p, ins):
        self._rel(p, None)

    def i_stlrb(self, p, ins):
        self._rel(p, "u8")

    def i_stlrh(self, p, ins):
        self._rel(p, "u16")

    def _rel(self, p, ctype):
        k, i, bits = self.reg(p[0])
        if k not in ("x", "z"):
            raise Unsupported("stlr FP")
        ctype = ctype or ("u64" if bits == 64 else "u32")
        val = "0" if k == "z" else self.rx(p[0])
        mem = ", ".join(p[1:])
        ga = self._page_mem(mem)
        if ga is not None:
            e = self.gaddr(ga)
        else:
            e, off, wbi = self.addr(mem, None)
            if e is None or wbi:
                raise Unsupported("stlr on the stack")
        self.body.append(f"stl<{ctype}>({e}, {ctype}({val}));")

    # --- memory
    def _load(self, dst, ctype, mem, ins, signed_to=None):
        e, off, wbi = self.addr(mem, ins)
        if e is None:
            if off in self.frame_slots:
                raise Unsupported("read of a callee-saved slot")
            src = self.local(off, ctype)
        else:
            src = f"ld<{ctype}>({e})"
        k, i, bits = self.reg(dst)
        if k == "v":
            ft = "f32" if bits == 32 else "f64" if bits == 64 else None
            if ft is None or ctype != ft:
                raise Unsupported("FP load")
            self.wv(dst, src, materialize=True)
        else:
            if signed_to:
                src = f"u64({signed_to}({src}))"
            self.wx(dst, src, materialize=True)
        self.apply_wb(wbi, mem)

    def _page_mem(self, mem):
        mm = _MEMOP.match(mem)
        if mm and mm.group(2) and mm.group(2).startswith("#") and not mm.group(6) and mm.group(7) is None:
            pg = self._page(mm.group(1))
            if pg is not None:
                return pg + _imm(mm.group(2))
        return None

    def _ldr(self, p, ins, ctype=None, signed=None):
        dst = p[0]
        mem = ", ".join(p[1:])
        ga = self._page_mem(mem)
        k, i, bits = self.reg(dst)
        if ctype is None:
            ctype = {("x", 64): "u64", ("x", 32): "u32", ("v", 32): "f32", ("v", 64): "f64"}.get((k, bits))
            if ctype is None:
                raise Unsupported("ldr q/b/h")
        if ga is not None:
            if ctype == "u64" and any(s <= ga < e for s, e in GOT_RANGES):
                self.wx(dst, self.gaddr(ga, got=True))
                return
            base = self.gaddr(ga)
            src = f"ld<{ctype}>({base})"
            if k == "v":
                self.wv(dst, src, materialize=True)
            else:
                self.wx(dst, f"u64({signed}({src}))" if signed else src, materialize=True)
            return
        self._load(dst, ctype, mem, ins, signed)

    def i_ldr(self, p, ins):
        self._ldr(p, ins)

    def i_ldur(self, p, ins):
        self._ldr(p, ins)

    def i_ldrb(self, p, ins):
        self._ldr(p, ins, "u8")

    def i_ldurb(self, p, ins):
        self._ldr(p, ins, "u8")

    def i_ldrh(self, p, ins):
        self._ldr(p, ins, "u16")

    def i_ldurh(self, p, ins):
        self._ldr(p, ins, "u16")

    def i_ldrsw(self, p, ins):
        self._ldr(p, ins, "s32", "s64")

    def i_ldursw(self, p, ins):
        self._ldr(p, ins, "s32", "s64")

    def i_ldrsb(self, p, ins):
        k, i, bits = self.reg(p[0])
        self._ldr(p, ins, "s8", "s64" if bits == 64 else "s32")

    def i_ldrsh(self, p, ins):
        k, i, bits = self.reg(p[0])
        self._ldr(p, ins, "s16", "s64" if bits == 64 else "s32")

    def i_ldursh(self, p, ins):
        self.i_ldrsh(p, ins)

    def i_ldursb(self, p, ins):
        self.i_ldrsb(p, ins)

    def i_ldp(self, p, ins):
        k, i, bits = self.reg(p[0])
        sz = bits // 8
        mem = ", ".join(p[2:])
        mm = _MEMOP.match(mem)
        if not mm:
            raise Unsupported(mem)
        base, idx = mm.group(1), mm.group(2)
        off = _imm(idx) if idx else 0
        post = mm.group(7)
        pre = mm.group(6)
        if mm.group(3) or mm.group(5) or (idx and not idx.startswith("#")):
            raise Unsupported("ldp index")
        o0 = 0 if post is not None else off
        m1 = f"[{base}, #{o0}]" if o0 else f"[{base}]"
        m2 = f"[{base}, #{o0 + sz}]"
        if base in (p[0], p[1]) and base not in ("sp",):
            raise Unsupported("ldp over its base")
        self._ldr([p[0], m1], ins)
        self._ldr([p[1], m2], ins)
        if pre or post is not None:
            delta = off if pre else _imm(post)
            if base == "sp":
                self.sp += delta
            else:
                self.wx(base, f"{self.rx(base)} + {hx(delta)}" if delta >= 0 else f"{self.rx(base)} - {hx(-delta)}")

    def _store(self, src, ctype, mem, ins):
        ga = self._page_mem(mem)
        k, i, bits = self.reg(src)
        if k == "v":
            val = self.rv(src)
        elif k == "z":
            val = "0"
        else:
            val = self.rx(src)
        cast = f"{ctype}({val})" if k != "v" else val
        if ga is not None:
            self.body.append(f"st<{ctype}>({self.gaddr(ga)}, {cast});")
            return
        e, off, wbi = self.addr(mem, ins)
        if e is None:
            if off in self.frame_slots:
                raise Unsupported("write to a callee-saved slot")
            self.body.append(f"{self.local(off, ctype)} = {cast};")
        else:
            self.body.append(f"st<{ctype}>({e}, {cast});")
        self.apply_wb(wbi, mem)

    def _str(self, p, ins, ctype=None):
        k, i, bits = self.reg(p[0])
        if ctype is None:
            ctype = {("x", 64): "u64", ("x", 32): "u32", ("z", 64): "u64", ("z", 32): "u32", ("v", 32): "f32",
                     ("v", 64): "f64"}.get((k, bits))
            if ctype is None:
                raise Unsupported("str q")
        self._store(p[0], ctype, ", ".join(p[1:]), ins)

    def i_str(self, p, ins):
        self._str(p, ins)

    def i_stur(self, p, ins):
        self._str(p, ins)

    def i_strb(self, p, ins):
        self._str(p, ins, "u8")

    def i_sturb(self, p, ins):
        self._str(p, ins, "u8")

    def i_strh(self, p, ins):
        self._str(p, ins, "u16")

    def i_sturh(self, p, ins):
        self._str(p, ins, "u16")

    def i_stp(self, p, ins):
        k, i, bits = self.reg(p[0])
        sz = bits // 8
        mem = ", ".join(p[2:])
        mm = _MEMOP.match(mem)
        if not mm or mm.group(3) or mm.group(5):
            raise Unsupported(mem)
        base, idx = mm.group(1), mm.group(2)
        off = _imm(idx) if idx else 0
        post, pre = mm.group(7), mm.group(6)
        o0 = 0 if post is not None else off
        m1 = f"[{base}, #{o0}]" if o0 else f"[{base}]"
        m2 = f"[{base}, #{o0 + sz}]"
        self._str([p[0], m1], ins)
        self._str([p[1], m2], ins)
        if pre or post is not None:
            delta = off if pre else _imm(post)
            if base == "sp":
                self.sp += delta
                self.min_sp = min(self.min_sp, self.sp)
            else:
                self.wx(base, f"{self.rx(base)} + {hx(delta)}" if delta >= 0 else f"{self.rx(base)} - {hx(-delta)}")

    # --- scalar floating point
    def _ftype(self, r):
        k, i, bits = self.reg(r)
        if k != "v" or bits not in (32, 64):
            raise Unsupported(r)
        return "f32" if bits == 32 else "f64"

    def i_fmov(self, p, ins):
        d, s = p[0], p[1]
        kd = self.reg(d)[0]
        if s.startswith("#"):
            t = self._ftype(d)
            v = float(s[1:])
            self.wv(d, f"{v!r}f" if t == "f32" else repr(v))
            return
        ks = self.reg(s)[0] if s not in ("wzr", "xzr") else "z"
        if kd == "v" and ks == "v":
            self.wv(d, self.rv(s))
        elif kd == "v":
            t = self._ftype(d)
            src = "0" if ks == "z" else self.rx(s)
            it = "u32" if t == "f32" else "u64"
            self.wv(d, f"bitCast<{t}>({it}({src}))")
        else:
            t = self._ftype(s)
            it = "u32" if t == "f32" else "u64"
            self.wx(d, f"u64(bitCast<{it}>({self.rv(s)}))")

    def _f3(self, p, op):
        self._ftype(p[0])
        self.wv(p[0], f"{self.rv(p[1])} {op} {self.rv(p[2])}")

    def i_fadd(self, p, ins):
        self._f3(p, "+")

    def i_fsub(self, p, ins):
        self._f3(p, "-")

    def i_fmul(self, p, ins):
        if "[" in p[2] or "." in p[0]:
            raise Unsupported("fmul by element")
        self._f3(p, "*")

    def i_fdiv(self, p, ins):
        self._f3(p, "/")

    def i_fneg(self, p, ins):
        self.wv(p[0], f"-{self.rv(p[1])}")

    def i_fabs(self, p, ins):
        t = self._ftype(p[0])
        self.wv(p[0], f"__builtin_fabs{'f' if t == 'f32' else ''}({self.rv(p[1])})")

    def i_fsqrt(self, p, ins):
        t = self._ftype(p[0])
        self.wv(p[0], f"__builtin_sqrt{'f' if t == 'f32' else ''}({self.rv(p[1])})")

    # fmadd/fmsub: __builtin_fma ends up as a call to fmaf (unresolved at link time): they are not translated.

    def i_fcvt(self, p, ins):
        t = self._ftype(p[0])
        self.wv(p[0], f"{t}({self.rv(p[1])})")

    def i_scvtf(self, p, ins):
        t = self._ftype(p[0])
        if len(p) > 2:
            raise Unsupported("fixed-point scvtf")
        k, i, bits = self.reg(p[1])
        st = "s64" if bits == 64 else "s32"
        self.wv(p[0], f"{t}({st}({self.rx(p[1])}))")

    def i_ucvtf(self, p, ins):
        t = self._ftype(p[0])
        if len(p) > 2:
            raise Unsupported("fixed-point ucvtf")
        k, i, bits = self.reg(p[1])
        ut = "u64" if bits == 64 else "u32"
        self.wv(p[0], f"{t}({ut}({self.rx(p[1])}))")

    def i_fcvtzs(self, p, ins):
        if len(p) > 2:
            raise Unsupported("fixed-point fcvtzs")
        k, i, bits = self.reg(p[0])
        st = "s64" if bits == 64 else "s32"
        self.wx(p[0], f"{st}({self.rv(p[1])})")

    def i_fcvtzu(self, p, ins):
        if len(p) > 2:
            raise Unsupported("fixed-point fcvtzu")
        k, i, bits = self.reg(p[0])
        ut = "u64" if bits == 64 else "u32"
        self.wx(p[0], f"{ut}({self.rv(p[1])})")

    def i_fcmp(self, p, ins):
        a = self.rv(p[0])
        if p[1].startswith("#"):
            t = self._ftype(p[0])
            b = "0.0f" if t == "f32" else "0.0"
        else:
            b = self.rv(p[1])
        self.flags = ("fcmp", a, b, 0)

    def i_fcsel(self, p, ins):
        self.wv(p[0], f"({self.cond_expr(p[3])} ? {self.rv(p[1])} : {self.rv(p[2])})")

    def i_fmax(self, p, ins):
        raise Unsupported("fmax")

    # ------------------------------------------------------------- calls
    def _args_for(self, spec):
        args = []
        if spec is None:
            raise Unsupported("unknown arity")
        xs = {k: b for k, b in spec.get("x", {}).items() if k != 8}
        vs = spec.get("v", {})
        nx = max(xs) + 1 if xs else 0
        params, vals = [], []
        for i in range(nx):
            if i in xs:
                e = self.rx(f"x{i}")
            else:
                e = "0"
            params.append(f"u64 a{i}")
            vals.append(e)
        nv = max(vs) + 1 if vs else 0
        for i in range(nv):
            bits = vs.get(i, 64)
            if bits not in (32, 64):
                raise Unsupported("vector argument")
            t = "f32" if bits == 32 else "f64"
            if i in vs:
                vi = self.v.get(i)
                if vi is None and i in self.v:
                    raise Unsupported("v clobbered")
                e = self.rv(f"{'s' if bits == 32 else 'd'}{i}")
            else:
                e = "0"
            params.append(f"{t} v{i}")
            vals.append(e)
        return params, vals

    def _clobber(self):
        for i in range(0, 19):
            self.x[i] = None
        for i in range(0, 8):
            self.v[i] = None
        for i in range(16, 32):
            self.v[i] = None
        self.flags = None

    def _result_kind(self):
        """What does the code following the call read: x0, s0/d0 or nothing?"""
        idx = self._cur + 1
        rd_x = rd_v = None
        for ins in self.m.insns[idx:]:
            try:
                rd, wr = ins.regs_access()
            except Exception:  # noqa: BLE001
                raise Unsupported("regs_access")
            names_r = {ins.reg_name(r) for r in rd}
            names_w = {ins.reg_name(r) for r in wr}
            for n in names_r:
                if n in ("x0", "w0") and rd_x is None:
                    rd_x = True
                if n in ("s0", "d0", "q0", "v0", "b0", "h0") and rd_v is None:
                    rd_v = n[0]
            for n in names_w:
                if n in ("x0", "w0") and rd_x is None:
                    rd_x = False
                if n in ("s0", "d0", "q0", "v0") and rd_v is None:
                    rd_v = False
            if ins.mnemonic in ("bl", "blr", "b", "cbz", "cbnz", "tbz", "tbnz") or ins.mnemonic.startswith("b."):
                break
            if ins.mnemonic in ("ret",):
                if self.ret_needs is not None:
                    pass
                break
        return rd_x, rd_v

    def call(self, t, tail):
        spec = oracle().args_of(t)
        if spec is None:
            # unknown arity: as in indirect calls, x0..x7 and d0..d7 are passed as they are
            xs, vs = self._all_args()
            spec = {"x": {}, "v": {}}
            params = [f"u64 a{i}" for i in range(8)] + [f"f64 v{i}" for i in range(8)]
            vals = xs + vs
        else:
            params, vals = self._args_for(spec)
        if 8 in spec.get("x", {}):
            # the target writes its result to *x8: placement new with guaranteed elision (C++17)
            # makes x8 exactly that address
            if tail:
                raise Unsupported("tail call with x8")
            dst = self.rx("x8")
            rx, rv = self._result_kind()
            if rx or rv:
                raise Unsupported("x0 is read after a call with x8")
            f = self.d.fn(t, "SretBuf", ", ".join(params), raw=True)
            self.body.append(f"new (reinterpret_cast<void*>({dst})) SretBuf({f}({', '.join(vals)}));")
            self._clobber()
            self.last_x0 = "call"
            return
        if tail:
            f = self.d.fn(t, "u64", ", ".join(params), raw=True)
            self.tail = f"{f}({', '.join(vals)})"
            return
        rx, rv = self._result_kind()
        if rx is None and not rv:
            rx = True
        if rx and rv:
            raise Unsupported("result in both x0 and v0")
        if rv in ("s", "d"):
            rt = "f32" if rv == "s" else "f64"
            f = self.d.fn(t, rt, ", ".join(params), raw=True)
            self._clobber()
            r = self.tmp(rt, f"{f}({', '.join(vals)})", "r")
            self.v[0] = Val(r, rt)
            self.wrote.add(("v", 0))
            self.last_x0 = "v"
        elif rx:
            f = self.d.fn(t, "u64", ", ".join(params), raw=True)
            self._clobber()
            r = self.tmp("u64", f"{f}({', '.join(vals)})", "r")
            self.x[0] = Val(r)
            self.wrote.add(("x", 0))
            self.last_x0 = "x"
        else:
            f = self.d.fn(t, "void", ", ".join(params), raw=True)
            self.body.append(f"{f}({', '.join(vals)});")
            self._clobber()
            self.last_x0 = "call"
            self.void_call = (len(self.body) - 1, t, params, vals)

    def _all_args(self):
        """x0..x7 and d0..d7 as they are (untouched = parameter; clobbered = 0)."""
        xs, vs = [], []
        for i in range(8):
            if i in self.x and self.x[i] is None:
                xs.append("0")
            elif i in self.x and self.x[i].e.startswith("PAGE:"):
                raise Unsupported("ADRP page as an argument")
            else:
                xs.append(self.rx(f"x{i}"))
        for i in range(8):
            if i in self.v and self.v[i] is None:
                vs.append("0.0")
            elif i in self.v:
                val = self.v[i]
                vs.append(val.e if val.t == "f64" else f"bitCast<f64>(u64(bitCast<u32>({val.e})))")
            else:
                vs.append(self.rv(f"d{i}"))
        return xs, vs

    def call_indirect(self, reg, tail):
        fe = self.rx(reg)
        xs, vs = self._all_args()
        sig = ", ".join(["u64"] * 8 + ["f64"] * 8)
        args = ", ".join(xs + vs)
        if tail:
            self.tail = f"reinterpret_cast<u64 (*)({sig})>({fe})({args})"
            return
        rx, rv = self._result_kind()
        if rx and rv:
            raise Unsupported("result in both x0 and v0")
        if rv in ("s", "d"):
            rt = "f32" if rv == "s" else "f64"
            r = self.tmp(rt, f"reinterpret_cast<{rt} (*)({sig})>({fe})({args})", "r")
            self._clobber()
            self.v[0] = Val(r, rt)
            self.wrote.add(("v", 0))
            self.last_x0 = "v"
            return
        if rv:
            raise Unsupported("vector result of an indirect call")
        x8 = self.x.get(8)
        if not rx and x8 is not None and reg not in ("x8", "w8") and not x8.e.startswith("PAGE:"):
            # x8 defined: may be the indirect result of the target; it is passed as in the original
            self.body.append(f"new (reinterpret_cast<void*>({x8.e})) SretBuf("
                             f"reinterpret_cast<SretBuf (*)({sig})>({fe})({args}));")
            self._clobber()
            self.last_x0 = "call"
            return
        r = self.tmp("u64", f"reinterpret_cast<u64 (*)({sig})>({fe})({args})", "r")
        self._clobber()
        self.x[0] = Val(r)
        self.wrote.add(("x", 0))
        self.last_x0 = "x"

    # ------------------------------------------------------------- end
    def ret_stmt(self):
        """Return statement of the current path and its type."""
        if getattr(self, "tail", None):
            return [f"return {self.tail};"], "u64"
        if not getattr(self, "ret", False):
            raise Unsupported("no end")
        vc = getattr(self, "void_call", None)
        if self.x.get(0, 1) is None and vc is not None and ("v", 0) not in self.wrote:
            # x0 on return = result of the last call: returned as is
            k, t, cparams, cvals = vc
            f = self.d.fn(t, "u64", ", ".join(cparams), raw=True)
            self.ntmp += 1
            self.body[k] = f"const u64 r{self.ntmp} = {f}({', '.join(cvals)});"
            return [f"return r{self.ntmp};"], "u64"
        if ("x", 0) not in self.wrote and ("v", 0) not in self.wrote:
            # x0 unchanged: the function lets it pass through (e.g. returns this)
            return [f"return {self.rx('x0')};"], "u64"
        if self.last_x0 == "x" or ("x", 0) in self.wrote:
            if self.x.get(0) is None:
                raise Unsupported("x0 undefined on return")
            if ("v", 0) in self.wrote:
                raise Unsupported("x0 and v0 both written")
            return [f"return {self.x[0].e};"], "u64"
        v0 = self.v.get(0)
        if v0 is None:
            raise Unsupported("v0 undefined")
        return [f"return {v0.e};"], v0.t

    def assemble(self, body):
        kinds = set(self.shared["kinds"]) - {"noreturn"}
        if len(kinds) > 1:
            raise Unsupported(f"different returns {sorted(kinds)}")
        ret = kinds.pop() if kinds else "void"
        if self.shared.get("sret_in"):
            body.insert(0, 'u64 a8;')
            body.insert(1, 'asm volatile("mov %0, x8" : "=r"(a8));  // indirect result (sret) received in x8')
        if self.uses_stack:
            mf = min(self.min_frame, self.shared.get("min_frame", 0))
            size = max(16, (-mf + 15) // 16 * 16)
            body.insert(0, f"alignas(16) u8 stack[{size}];")
        if len(body) > self.MAX_LINES:
            raise Unsupported("too long")
        for b in body:
            if "PAGE:" in b:
                raise Unsupported("stray ADRP page")
        params = []
        nx = max(self.xparams) + 1 if self.xparams else 0
        for i in range(nx):
            params.append(f"u64 a{i}")
        nv = max(self.vparams) + 1 if self.vparams else 0
        for i in range(nv):
            t = self.vparams.get(i, "f64")
            params.append(f"{t} {'s' if t == 'f32' else 'd'}{i}")
        return Fn(ret, "fn", ", ".join(params), body)


def lift(m, d, max_lines: int = 80):
    lf = Lifter(m, d)
    lf.MAX_LINES = max_lines
    lf.ret_needs = None
    lf.frame_slots = set()
    try:
        return lf.run()
    except (Unsupported, KeyError, ValueError, IndexError):
        return None


_BRANCHY = re.compile(r"(?:^|; )(?:cbz|cbnz|tbz|tbnz|b\.\w+) ")


@pattern("lift", rx=r"(?!.*(?:^|; )(?:cbz|cbnz|tbz|tbnz|b\.\w+) ).*", probe=True,
         desc="generic translation of linear code (no conditional branches)")
def _lift(m, d, v):
    if len(m.insns) > 64:
        return None
    if any(i.mnemonic == "b" and m.addr <= int(i.op_str[1:], 16) < m.addr + m.size for i in m.insns):
        return None
    return lift(m, d)


LIFT_BR_MAX_INSNS = 96
LIFT_BR_MAX_LINES = 120


@pattern("lift_br", rx=r"(?!.*(?:casal?|ldadd|swp)).*(?:^|; )(?:cbz|cbnz|tbz|tbnz|b\.\w+) .*",
         probe=True, desc="generic translation with forward conditional branches (if per path, no loops)")
def _lift_br(m, d, v):
    if len(m.insns) > LIFT_BR_MAX_INSNS:
        return None
    return lift(m, d, max_lines=LIFT_BR_MAX_LINES)
