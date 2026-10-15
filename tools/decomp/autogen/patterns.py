"""Patterns of the automatic generator (tools/decomp/autogen).

Each pattern associates one or more disassembly *templates* (those of scan.template: mnemonic
and operands with immediates replaced by N and branch/ADRP targets by T) with an
emitter that writes the equivalent C++ from the concrete values of the function.

An emitter receives a `Match` (N and T values in template order) and a `Decls` (the
external declarations the file needs) and returns an `Fn`, or None if the values do not
fit (e.g. a call that is not to the expected function). There can be several C++ variants
per pattern: if the first does not verify, run.py tries the next one.
"""
from __future__ import annotations

import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import INCLUDE  # noqa: E402

# ------------------------------------------------------------------ template values
_NUM = re.compile(r"#(-?0x[0-9a-f]+|-?\d+(?:\.\d+(?:e[-+]?\d+)?)?)")


def _num(s: str):
    if "." in s or "e" in s and not s.startswith(("0x", "-0x")):
        return float(s)
    return int(s, 0)


_MEM = re.compile(r"^(\w+), \[(\w+)(?:, #(-?0x[0-9a-f]+|-?\d+))?\]!?$")
GOT_RANGES = ((0x7104270878, 0x7104270878 + 0x1998), (0x7104272210, 0x7104272210 + 0x172B0))


@dataclass
class Match:
    addr: int
    size: int
    insns: list          # capstone CsInsn
    n: list              # immediates (int or float) in order of appearance
    t: list              # targets of B/BL/B.cond/ADRP/ADR in order of appearance
    tpl: str

    @staticmethod
    def build(addr, size, insns, tpl) -> "Match":
        n, t = [], []
        for i in insns:
            if i.mnemonic in ("b", "bl", "adrp", "adr") or i.mnemonic.startswith("b."):
                t.extend(int(x, 16) for x in re.findall(r"#(0x[0-9a-f]+)", i.op_str))
            else:
                n.extend(_num(x) for x in _NUM.findall(i.op_str))
        return Match(addr, size, insns, n, t, tpl)

    def ops(self, i: int) -> list:
        """Immediates of instruction i (for B/BL/ADRP/ADR/B.cond: its target)."""
        ins = self.insns[i]
        if ins.mnemonic in ("b", "bl", "adrp", "adr") or ins.mnemonic.startswith("b."):
            return [int(x, 16) for x in re.findall(r"#(0x[0-9a-f]+)", ins.op_str)]
        return [_num(x) for x in _NUM.findall(ins.op_str)]

    def op(self, i: int, k: int = 0, default=0):
        v = self.ops(i)
        return v[k] if k < len(v) else default

    def target(self, i: int) -> int:
        return self.ops(i)[0]

    def adrp_addr(self, i: int) -> int:
        """Address formed by ADRP at instruction i + LO12 of instruction i+1."""
        return self.target(i) + self.op(i + 1, 0 if self.insns[i + 1].mnemonic == "add" else -1)

    def analyze(self):
        """Data references in order of appearance, and BL calls.
        self.refs: [(symbol address, via_got)] without repeats; self.calls: [target of each BL]."""
        if hasattr(self, "refs"):
            return self
        from image import load_image
        im = load_image()
        page: dict[str, int] = {}
        refs: list[tuple[int, bool]] = []
        seen = set()

        def add(a, got):
            if a not in seen:
                seen.add(a)
                refs.append((a, got))
        calls = []
        for ins in self.insns:
            mn, op = ins.mnemonic, ins.op_str
            regs = [r.strip() for r in op.split(",")]
            if mn == "adrp":
                page[regs[0]] = int(regs[1][1:], 16)
                continue
            if mn == "bl":
                calls.append(int(op[1:], 16))
            mm = _MEM.match(op)
            if mn == "add" and len(regs) == 3 and regs[1] in page and regs[2].startswith("#"):
                add(page[regs[1]] + _num(regs[2][1:]), False)
                page.pop(regs[0], None)
                continue
            if mm and mm.group(2) in page:
                a = page[mm.group(2)] + (_num(mm.group(3)) if mm.group(3) else 0)
                if mn == "ldr" and mm.group(1).startswith("x") and any(s <= a < e for s, e in GOT_RANGES):
                    add(im.u64(a), True)
                else:
                    add(a, False)
                if mn.startswith("ld") and mm.group(1) == mm.group(2):
                    page.pop(mm.group(1), None)
                continue
            if regs and regs[0] in page and not mn.startswith(("st", "b", "c", "t")):
                page.pop(regs[0], None)
        self.refs, self.calls = refs, calls
        return self

    def got_value(self, i: int) -> int:
        """Value of the GOT entry read with ADRP (i) + LDR (i+1)."""
        from image import load_image
        slot = self.target(i) + self.ops(i + 1)[-1]
        return load_image().u64(slot)


# ------------------------------------------------------------------ nombres conocidos
def _lua_api() -> dict[int, tuple[str, str, str]]:
    """address -> (name, return, parameters) from include/auto/Lua.h."""
    out = {}
    p = INCLUDE / "auto" / "Lua.h"
    if not p.exists():
        return out
    lines = p.read_text(encoding="utf-8").splitlines()
    for k, line in enumerate(lines[:-1]):
        m = re.match(r"// 0x(71[0-9a-f]{8})$", line.strip())
        if m:
            d = re.match(r"(.+?)\b(\w+)\((.*)\);$", lines[k + 1].strip())
            if d:
                out[int(m.group(1), 16)] = (d.group(2), d.group(1).strip(), d.group(3))
    return out


LUA = _lua_api()
LUA_BY_NAME = {v[0]: k for k, v in LUA.items()}
_KNOWN: dict[int, str] | None = None


def known_name(a: int) -> str | None:
    """Real (non-generic) name of a function, from data/triage.csv (Lua, PLT imports...)."""
    global _KNOWN
    if _KNOWN is None:
        import csv
        from common import TRIAGE_CSV
        _KNOWN = {}
        with open(TRIAGE_CSV, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                nm = r.get("Name") or ""
                if nm and not nm.startswith(("FUN_", "vtable_", "thunk_")):
                    _KNOWN[int(r["Address"], 16)] = nm
        try:
            from image import load_image
            im = load_image()
            for pa, pn in im.plt.items():
                _KNOWN.setdefault(pa, pn)
        except Exception:  # noqa: BLE001
            pass
    return _KNOWN.get(a)


# ------------------------------------------------------------------ file declarations
class Decls:
    """External declarations (FUN_ functions and DAT_ data) used by a generated file.
    If the same address is declared with two different signatures, the second gets a suffix
    (FUN_7100123456_1): the name still carries the address, so verify.py resolves it."""

    def __init__(self):
        self.lines: dict[str, str] = {}
        self._fn: dict[tuple, str] = {}
        self._count: dict[str, int] = {}

    def _unique(self, base: str, key: tuple, text_fn) -> str:
        if key in self._fn:
            return self._fn[key]
        k = self._count.get(base, 0)
        self._count[base] = k + 1
        name = base if k == 0 else f"{base}_{k}"
        self._fn[key] = name
        self.lines[name] = text_fn(name)
        return name

    def fn(self, a: int, ret: str = "void", params: str = "", noreturn: bool = False, got: bool = False,
           raw: bool = False) -> str:
        """Name to call function `a` with that signature (got=True: default visibility,
        its address is taken from the GOT as in the original; raw=True: always FUN_<addr>, even if
        it is part of the Lua API, for generic signatures such as those of the linear translator)."""
        if a in LUA and not raw:
            nm, r, p = LUA[a]
            return nm
        kn = known_name(a)
        com = f"  // {kn}" if kn else ""
        attr = "[[noreturn]] " if noreturn else ""
        vis = '__attribute__((visibility("default"))) ' if got else ""
        return self._unique(f"FUN_{a:x}", ("f", a, ret, params, noreturn, got),
                            lambda n: f'{attr}extern "C" {vis}{ret} {n}({params});{com}')

    def import_fn(self, a: int, ret: str = "void", params: str = "") -> str:
        """Function imported from the sdk module whose address is taken from the GOT (e.g. a destructor
        passed to __cxa_atexit). It is named FUN_<fictitious address of the import> (the one given by
        tools/decomp/image.py): verify.py resolves it by name and finds its GOT entry.
        (An assembler label with the mangled name would clash with the inline definitions of
        libc++, e.g. ~basic_string.)"""
        from image import load_image
        mangled = load_image().imports[a]
        kn = known_name(a) or mangled
        return self._unique(f"FUN_{a:x}", ("i", a, ret, params),
                            lambda n: f'extern "C" __attribute__((visibility("default"))) {ret} {n}({params});'
                                      f'  // import {kn}')

    def data(self, a: int, ctype: str = "u8", array: bool = True, comment: str = "", got: bool = False) -> str:
        """Global datum. got=True: default visibility (the original reads it through the GOT)."""
        com = f"  // {comment}" if comment else ""
        suf = "[]" if array else ""
        vis = '__attribute__((visibility("default"))) ' if got else ""
        return self._unique(f"DAT_{a:x}", ("d", a, ctype, array, got),
                            lambda n: f'extern "C" {vis}{ctype} {n}{suf};{com}')

    def render(self) -> list[str]:
        return [self.lines[k] for k in sorted(self.lines)]

    @staticmethod
    def is_lua(a: int, name: str) -> bool:
        return LUA_BY_NAME.get(name) == a


# ------------------------------------------------------------------ emitted function
@dataclass
class Fn:
    ret: str
    name: str            # prefix; _<address> is appended
    params: str
    body: list[str]
    attrs: str = ""      # p. ej. "[[noreturn]]"
    note: str = ""       # comment above the function (optional)

    def render(self, addr: int) -> str:
        out = []
        if self.note:
            out.append(f"// {self.note}")
        out.append(f"// 0x{addr:010x}")
        pre = f"{self.attrs} " if self.attrs else ""
        if self.body:
            out.append(f"{pre}{self.ret} {self.name}_{addr:x}({self.params}) {{")
            out.extend("    " + b for b in self.body)
            out.append("}")
        else:
            out.append(f"{pre}{self.ret} {self.name}_{addr:x}({self.params}) {{}}")
        return "\n".join(out)


@dataclass
class Pattern:
    key: str
    tpls: list[str]
    emit: object                     # (Match, Decls, variante) -> Fn | None
    variants: int = 1
    desc: str = ""
    rx: re.Pattern | None = None     # alternative: regular expression over the template
    probe: bool = False              # classify by running the emitter (for very broad rx)
    includes: tuple = ()             # additional headers needed by the emitted C++


PATTERNS: list[Pattern] = []
BY_TPL: dict[str, Pattern] = {}
BY_RX: list[Pattern] = []


def pattern(key: str, *tpls: str, variants: int = 1, desc: str = "", rx: str | None = None, probe: bool = False,
            like: tuple = (), includes: tuple = ()):
    """tpls: exact templates; rx: regular expression; like: example addresses whose template is used."""
    if like:
        tpls = tpls + tuple(template_of(a) for a in like)

    def deco(fn):
        p = Pattern(key, list(tpls), fn, variants, desc, re.compile(rx) if rx else None, probe, tuple(includes))
        PATTERNS.append(p)
        for t in tpls:
            assert t not in BY_TPL, f"plantilla repetida: {t}"
            BY_TPL[t] = p
        if rx:
            BY_RX.append(p)
        return fn
    return deco


def template_of(a: int) -> str:
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
    from common import functions
    from image import load_image
    import scan
    f = functions().at(a)
    insns = list(Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(load_image().read(a, f.size), a))
    return scan.template(insns)


def find_patterns(tpl: str) -> list:
    """All candidate patterns (exact first, then the regular expressions in order)."""
    out = []
    p = BY_TPL.get(tpl)
    if p is not None:
        out.append(p)
    out.extend(p for p in BY_RX if p.rx.fullmatch(tpl))
    return out


def find_pattern(tpl: str) -> Pattern | None:
    p = BY_TPL.get(tpl)
    if p is not None:
        return p
    for p in BY_RX:
        if p.rx.fullmatch(tpl):
            return p
    return None


# ------------------------------------------------------------------ emission utilities
def hx(v: int) -> str:
    return f"{v:#x}" if v >= 0 else f"-{-v:#x}"


def s32(v: int) -> int:
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def s64(v: int) -> int:
    v &= (1 << 64) - 1
    return v - (1 << 64) if v >> 63 else v


def lua_index(v: int) -> str:
    v = s32(v)
    if v == -1001000:
        return "LUA_REGISTRYINDEX"
    if -1001256 <= v < -1001000:
        return f"lua_upvalueindex({-1001000 - v})"
    return str(v)


def cstr_at(a: int, maxlen: int = 120) -> str | None:
    """Printable C string at `a` (for comments), or None."""
    from image import load_image
    im = load_image()
    s = im.cstring(a, 4096)
    if s is None or not s:
        return None
    try:
        t = s.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if not all(c.isprintable() for c in t):
        return None
    t = t.replace("*/", "* /")
    return t if len(t) <= maxlen else t[:maxlen] + "..."


def strcomment(a: int) -> str:
    s = cstr_at(a)
    return f'"{s}"' if s is not None else ""


def mov_imm(insns_vals: list[tuple[int, int]]) -> int:
    """Value of a mov/movk sequence: [(imm16, shift)]."""
    v = 0
    for imm, sh in insns_vals:
        v = (v & ~(0xFFFF << sh)) | (imm << sh)
    return v


def is_lua(a: int, name: str) -> bool:
    return LUA_BY_NAME.get(name) == a


# Memory access types by mnemonic/register (read).
LOAD_TYPES = {("ldr", "w"): "u32", ("ldr", "x"): "u64", ("ldrb", "w"): "u8", ("ldrh", "w"): "u16",
              ("ldrsb", "w"): "s8", ("ldrsh", "w"): "s16", ("ldrsw", "x"): "s32", ("ldrsb", "x"): "s8",
              ("ldrsh", "x"): "s16", ("ldr", "s"): "f32", ("ldr", "d"): "f64", ("ldr", "q"): "Vec128"}
STORE_TYPES = {("str", "w"): "u32", ("str", "x"): "u64", ("strb", "w"): "u8", ("strh", "w"): "u16",
               ("str", "s"): "f32", ("str", "d"): "f64"}
RET_OF = {"u32": "u32", "u64": "u64", "u8": "u8", "u16": "u16", "s8": "s32", "s16": "s32", "s32": "s64",
          "f32": "f32", "f64": "f64"}


from patterns_lib import *  # noqa: E402,F401,F403  (registers the patterns)
from patterns_lib2 import *  # noqa: E402,F401,F403
from patterns_lib3 import *  # noqa: E402,F401,F403
from lifter import *  # noqa: E402,F401,F403  (last: generic fallback pattern)
