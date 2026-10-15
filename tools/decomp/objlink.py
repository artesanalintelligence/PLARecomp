"""Compilation of a TU and minimal "linking" of its .o against the original binary.

- compile_tu(): clang with the flags from tools/decomp/compiler.json (by default those of the
  project's CMake) -> .o in build/decomp/.
- ObjFile: sections, symbols and relocations of an AArch64 relocatable ELF.
- link(): places the .o's SHF_ALLOC sections starting at a base, creates its own GOT
  and applies the relocations. Each symbol is resolved with a `resolver`:
  those corresponding to game functions/data go to their ORIGINAL address
  (so our code's outgoing calls go to the same addresses as the original's)
  and the rest (literals, inline helpers) stays local.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import DECOMP_TOOLS, ROOT  # noqa: E402

BUILD_DIR = ROOT / "build" / "decomp"

# Runtime symbols the compiler references implicitly (initializers of globals with a destructor).
# Addresses from the original main 1.1.1, verified against the original binary.
KNOWN_SYMBOLS = {
    "__dso_handle": 0x7100000250,
    "__cxa_atexit": 0x710401A000,
}

DEFAULT_COMPILER = {
    "clang": "9.0.0",
    "triple": "aarch64-none-elf",
    "opt": "-O3",
    "flags": ["-g", "-mcpu=cortex-a57+fp+simd+crypto+crc", "-mno-implicit-float", "-fPIC",
              "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden", "-fno-omit-frame-pointer", "-fno-exceptions", "-fstandalone-debug",
              "-fno-rtti", "-fno-strict-aliasing", "-std=c++17",
              "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-invalid-offsetof",
              "-Wno-unused-command-line-argument"],
    "defines": ["SWITCH", "NNSDK", "MATCHING_HACK_NX_CLANG", "NON_MATCHING"],
    "include_dirs": ["src", "include", "lib"],
    "note": "Default = flags from CMakeLists.txt/ToolchainNX64.cmake. This file may be "
            "overwritten once the exact clang and flags are identified.",
}


def compiler_config() -> dict:
    p = DECOMP_TOOLS / "compiler.json"
    cfg = dict(DEFAULT_COMPILER)
    if p.exists():
        cfg.update(json.loads(p.read_text(encoding="utf-8")))
    return cfg


def clang_dir(cfg: dict) -> Path:
    c = cfg["clang"]
    p = Path(c)
    if p.is_dir():
        return p
    return ROOT / "toolchain" / "cache" / f"clang-{c}"


def compile_cmd(cfg: dict, src: Path, out: Path) -> list[str]:
    cd = clang_dir(cfg)
    exe = cd / "bin" / ("clang++.exe" if os.name == "nt" else "clang++")
    ver = next((cd / "lib" / "clang").iterdir()).name
    cmd = [str(exe), f"--target={cfg['triple']}", cfg["opt"], *cfg["flags"],
           "-nostdinc", "-isystem", str(cd / "libcxx-include"),
           "-isystem", str(cd / "lib" / "clang" / ver / "include"),
           "-isystem", str(ROOT / "toolchain" / "musl" / "include")]
    cmd += [f"-D{d}" for d in cfg["defines"]]
    cmd += [f"-I{ROOT / d}" for d in cfg["include_dirs"]]
    cmd += ["-c", str(src), "-o", str(out)]
    return cmd


def compile_tu(src: Path, extra: list[str] | None = None) -> tuple[Path | None, str, list[str]]:
    """Compiles src -> .o. Returns (path or None on failure, diagnostic output, command)."""
    cfg = compiler_config()
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    rel = src.resolve().relative_to(ROOT).as_posix() if src.resolve().is_relative_to(ROOT) else src.name
    tag = hashlib.sha1(rel.encode()).hexdigest()[:10]
    out = BUILD_DIR / f"{Path(rel).stem}_{tag}_{os.getpid()}.o"
    cmd = compile_cmd(cfg, src, out) + (extra or [])
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT)
    if p.returncode != 0:
        return None, p.stderr + p.stdout, cmd
    return out, p.stderr + p.stdout, cmd


# --------------------------------------------------------------- ELF object
SHF_ALLOC, SHF_EXEC, SHF_WRITE = 2, 4, 1


@dataclass
class Section:
    idx: int
    name: str
    data: bytes
    size: int
    align: int
    flags: int
    nobits: bool
    relocs: list = field(default_factory=list)   # (offset, type, symidx, addend)


@dataclass
class Sym:
    idx: int
    name: str
    shndx: int | None    # None = undefined (or ABS/COMMON handled separately)
    value: int
    size: int
    type: str
    bind: str


class ObjFile:
    def __init__(self, path: Path):
        from elftools.elf.elffile import ELFFile
        from elftools.elf.relocation import RelocationSection
        self.path = path
        with open(path, "rb") as fh:
            elf = ELFFile(fh)
            self.sections: dict[int, Section] = {}
            for i, s in enumerate(elf.iter_sections()):
                if s["sh_flags"] & SHF_ALLOC and s["sh_type"] in ("SHT_PROGBITS", "SHT_NOBITS", "SHT_INIT_ARRAY",
                                                                   "SHT_FINI_ARRAY"):
                    nob = s["sh_type"] == "SHT_NOBITS"
                    self.sections[i] = Section(i, s.name, b"" if nob else s.data(), s["sh_size"],
                                               max(1, s["sh_addralign"]), s["sh_flags"], nob)
            symtab = elf.get_section_by_name(".symtab")
            self.syms: list[Sym] = []
            for j, s in enumerate(symtab.iter_symbols()):
                sh = s["st_shndx"]
                self.syms.append(Sym(j, s.name, sh if isinstance(sh, int) else None, s["st_value"], s["st_size"],
                                     s["st_info"]["type"], s["st_info"]["bind"]))
            for s in elf.iter_sections():
                if isinstance(s, RelocationSection):
                    tgt = s["sh_info"]
                    if tgt in self.sections:
                        for r in s.iter_relocations():
                            self.sections[tgt].relocs.append(
                                (r["r_offset"], r["r_info_type"], r["r_info_sym"], r["r_addend"] if r.is_RELA() else 0))
        # Exclude .eh_frame (not needed for execution).
        self.sections = {k: v for k, v in self.sections.items() if not v.name.startswith(".eh_frame")}

    def functions(self) -> list[Sym]:
        return [s for s in self.syms if s.type == "STT_FUNC" and s.shndx in self.sections]

    def defined(self, name: str) -> Sym | None:
        for s in self.syms:
            if s.name == name and s.shndx in self.sections:
                return s
        return None

    def func_at(self, shndx: int, off: int) -> Sym | None:
        """Named function defined at (section, offset): for the relocations the
        assembler emits against the SECTION symbol (calls to the TU's `static` functions)."""
        if not hasattr(self, "_func_at"):
            self._func_at = {}
            for s in self.syms:
                if s.type == "STT_FUNC" and s.name and s.shndx in self.sections:
                    self._func_at.setdefault((s.shndx, s.value), s)
        return self._func_at.get((shndx, off))


# --------------------------------------------------------------- relocations
R = {"ABS64": 257, "ABS32": 258, "PREL64": 260, "PREL32": 261, "MOVW_UABS_G0": 263, "MOVW_UABS_G0_NC": 264,
     "MOVW_UABS_G1": 265, "MOVW_UABS_G1_NC": 266, "MOVW_UABS_G2": 267, "MOVW_UABS_G2_NC": 268, "MOVW_UABS_G3": 269,
     "LD_PREL_LO19": 273, "ADR_PREL_LO21": 274, "ADR_PREL_PG_HI21": 275, "ADR_PREL_PG_HI21_NC": 276,
     "ADD_ABS_LO12_NC": 277, "LDST8_ABS_LO12_NC": 278, "TSTBR14": 279, "CONDBR19": 280, "JUMP26": 282,
     "CALL26": 283, "LDST16_ABS_LO12_NC": 284, "LDST32_ABS_LO12_NC": 285, "LDST64_ABS_LO12_NC": 286,
     "LDST128_ABS_LO12_NC": 299, "ADR_GOT_PAGE": 311, "LD64_GOT_LO12_NC": 312, "NONE": 0}
GOT_RELOCS = {311, 312}


class LinkError(Exception):
    pass


def _patch(buf: bytearray, off: int, rtype: int, S: int, A: int, P: int):
    def ins():
        return struct.unpack_from("<I", buf, off)[0]

    def put(v):
        struct.pack_into("<I", buf, off, v & 0xFFFFFFFF)
    X = S + A
    if rtype == 0:
        return
    if rtype == 257:
        struct.pack_into("<Q", buf, off, X & 0xFFFFFFFFFFFFFFFF)
    elif rtype == 258:
        struct.pack_into("<I", buf, off, X & 0xFFFFFFFF)
    elif rtype == 260:
        struct.pack_into("<Q", buf, off, (X - P) & 0xFFFFFFFFFFFFFFFF)
    elif rtype == 261:
        struct.pack_into("<I", buf, off, (X - P) & 0xFFFFFFFF)
    elif rtype in (275, 276, 311):
        v = ((X & ~0xFFF) - (P & ~0xFFF)) >> 12
        if rtype != 276 and not -(1 << 20) <= v < (1 << 20):
            raise LinkError(f"ADRP out of range at +{off:#x}")
        i = ins() & ~((3 << 29) | (0x7FFFF << 5))
        put(i | ((v & 3) << 29) | (((v >> 2) & 0x7FFFF) << 5))
    elif rtype == 274:
        v = X - P
        i = ins() & ~((3 << 29) | (0x7FFFF << 5))
        put(i | ((v & 3) << 29) | (((v >> 2) & 0x7FFFF) << 5))
    elif rtype in (277, 278, 284, 285, 286, 299, 312):
        shift = {277: 0, 278: 0, 284: 1, 285: 2, 286: 3, 299: 4, 312: 3}[rtype]
        v = (X & 0xFFF) >> shift
        i = ins() & ~(0xFFF << 10)
        put(i | ((v & 0xFFF) << 10))
    elif rtype in (282, 283):
        v = (X - P) >> 2
        if not -(1 << 25) <= v < (1 << 25):
            raise LinkError(f"BL/B out of range (+-128 MiB) at +{off:#x}")
        put((ins() & 0xFC000000) | (v & 0x3FFFFFF))
    elif rtype in (280, 273):
        v = (X - P) >> 2
        put((ins() & ~(0x7FFFF << 5)) | ((v & 0x7FFFF) << 5))
    elif rtype == 279:
        v = (X - P) >> 2
        put((ins() & ~(0x3FFF << 5)) | ((v & 0x3FFF) << 5))
    elif 263 <= rtype <= 269:
        sh = {263: 0, 264: 0, 265: 16, 266: 16, 267: 32, 268: 32, 269: 48}[rtype]
        v = (X >> sh) & 0xFFFF
        put((ins() & ~(0xFFFF << 5)) | (v << 5))
    else:
        raise LinkError(f"unsupported relocation type {rtype} at +{off:#x}")


@dataclass
class Linked:
    base: int
    blob: bytearray
    sec_addr: dict            # idx -> address
    sym_addr: dict            # name -> local address (defined)
    code_ranges: list         # [(start, end)] of executable sections
    ro_ranges: list           # [(start, end, name)] of read-only sections
    objects: list             # [(start, end, symbol name)] local data objects
    bindings: dict            # name -> address it was linked to (external/original)
    unresolved: list


def link(obj: ObjFile, base: int, resolver, prefer_original=None) -> Linked:
    """`resolver(name) -> int | None` gives a symbol's original address (or None).
    `prefer_original(name) -> bool`: whether a locally defined symbol should still be linked
    to its original address (e.g. another game function defined in the same TU)."""
    addr = base
    sec_addr = {}
    order = sorted(obj.sections.values(), key=lambda s: (0 if s.flags & SHF_EXEC else 1 if not s.flags & SHF_WRITE else 2,
                                                          s.idx))
    for s in order:
        addr = (addr + s.align - 1) & ~(s.align - 1)
        sec_addr[s.idx] = addr
        addr += max(s.size, 0)
    # own GOT
    got_syms = []
    for s in order:
        for off, t, si, a in s.relocs:
            if t in GOT_RELOCS and si not in got_syms:
                got_syms.append(si)
    addr = (addr + 7) & ~7
    got_base = addr
    addr += 8 * len(got_syms)
    total = addr - base
    blob = bytearray(total)
    for s in order:
        if not s.nobits:
            o = sec_addr[s.idx] - base
            blob[o:o + len(s.data)] = s.data
    sym_addr = {}
    for sy in obj.syms:
        if sy.shndx in sec_addr and sy.name:
            sym_addr[sy.name] = sec_addr[sy.shndx] + sy.value
    bindings, unresolved = {}, []

    def value_of(si: int) -> int:
        sy = obj.syms[si]
        if sy.type == "STT_SECTION":
            return sec_addr.get(sy.shndx, 0)
        if sy.shndx in sec_addr:
            if sy.name and prefer_original and prefer_original(sy.name):
                v = resolver(sy.name)
                if v is not None:
                    bindings[sy.name] = v
                    return v
            return sec_addr[sy.shndx] + sy.value
        if sy.name in bindings:
            return bindings[sy.name]
        v = resolver(sy.name)
        if v is None:
            v = KNOWN_SYMBOLS.get(sy.name)
        if v is None:
            if sy.name not in unresolved:
                unresolved.append(sy.name)
            return 0
        bindings[sy.name] = v
        return v

    def section_func_original(si: int, a: int):
        """Relocation against the section symbol of a local (`static`) function with a marker:
        original address of that function (or None)."""
        sy = obj.syms[si]
        if sy.type != "STT_SECTION" or not prefer_original or sy.shndx not in obj.sections:
            return None
        if not obj.sections[sy.shndx].flags & SHF_EXEC:
            return None
        f = obj.func_at(sy.shndx, a)
        if f is None or not prefer_original(f.name):
            return None
        v = resolver(f.name)
        if v is not None:
            bindings[f.name] = v
        return v

    for i, si in enumerate(got_syms):
        struct.pack_into("<Q", blob, got_base - base + 8 * i, value_of(si))
    for s in order:
        sa = sec_addr[s.idx]
        for off, t, si, a in s.relocs:
            P = sa + off
            if t in GOT_RELOCS:
                S, A = got_base + 8 * got_syms.index(si), 0
            else:
                S, A = value_of(si), a
                if obj.syms[si].type == "STT_SECTION":   # static function with marker -> the original
                    v = section_func_original(si, a)
                    if v is not None:
                        S, A = v, 0
            if s.nobits:
                continue
            try:
                _patch(blob, P - base, t, S, A, P)
            except LinkError as e:
                if unresolved:
                    raise LinkError(f"{e}; unresolved symbols: {', '.join(unresolved)}") from None
                raise
    code = [(sec_addr[s.idx], sec_addr[s.idx] + s.size) for s in order if s.flags & SHF_EXEC]
    ro = [(sec_addr[s.idx], sec_addr[s.idx] + s.size, s.name) for s in order
          if not s.flags & SHF_EXEC and not s.flags & SHF_WRITE]
    objs = [(sym_addr[sy.name], sym_addr[sy.name] + sy.size, sy.name) for sy in obj.syms
            if sy.type == "STT_OBJECT" and sy.name in sym_addr]
    return Linked(base, blob, sec_addr, sym_addr, code, ro, objs, bindings, unresolved)


def _orig_target(rtype: int, word: int, P: int):
    """Part of the address that the ORIGINAL instruction `word` (at P) encodes for a relocation:
    ('page', page) for ADRP, ('lo12', low bits) for ADD/LDST LO12, ('abs', addr) for ADR/LDR literal."""
    if rtype in (275, 276):
        if (word & 0x9F000000) != 0x90000000:
            return None
        imm = (((word >> 5) & 0x7FFFF) << 2) | ((word >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        return ("page", (P & ~0xFFF) + (imm << 12))
    if rtype in (277, 278, 284, 285, 286, 299):
        shift = {277: 0, 278: 0, 284: 1, 285: 2, 286: 3, 299: 4}[rtype]
        return ("lo12", (((word >> 10) & 0xFFF) << shift) & 0xFFF)
    if rtype == 274:
        if (word & 0x9F000000) != 0x10000000:
            return None
        imm = (((word >> 5) & 0x7FFFF) << 2) | ((word >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        return ("abs", P + imm)
    if rtype == 273:
        imm = (word >> 5) & 0x7FFFF
        if imm & (1 << 18):
            imm -= 1 << 19
        return ("abs", P + imm * 4)
    return None


def _local_data_bytes(obj: ObjFile, shndx: int, off: int) -> tuple[bytes, bool] | None:
    """Contents of a TU-local datum pointed to by (section, offset): (bytes, is_string)."""
    sec = obj.sections.get(shndx)
    if sec is None or sec.nobits or sec.flags & SHF_WRITE or sec.flags & SHF_EXEC:
        return None
    data = sec.data
    if not 0 <= off < len(data):
        return None
    if sec.name.startswith(".rodata.str"):
        end = data.find(b"\0", off)
        return (data[off:] if end < 0 else data[off:end + 1]), True
    if sec.name.startswith(".rodata.cst"):
        try:
            k = int(sec.name[len(".rodata.cst"):].split(".")[0])
        except ValueError:
            k = 8
        return data[off:off + k], False
    for s in obj.syms:   # named object containing the offset
        if s.type == "STT_OBJECT" and s.shndx == shndx and s.value <= off < s.value + max(s.size, 1):
            return data[off:s.value + s.size], False
    return data[off:], False


def relocate_function_at(obj: ObjFile, sym: Sym, at: int, resolver, got_lookup=None, orig: bytes | None = None,
                         orig_read=None) -> tuple[bytes, list[str]]:
    """Bytes of function `sym` as if it were at address `at` (for the exact match).
    Returns (bytes, problems): local or unresolved symbols are listed as problems.

    With `orig` (the function's original bytes) and `orig_read(addr, n)` (image read), references
    to TU-local read-only data (string literals, constants, `static const` tables)
    are accepted if the original points, in the same instruction, to data with the same
    contents: the original address is used. Calls to `static` functions with a marker (which the
    assembler relocates against the section symbol) are resolved to their original address."""
    sec = obj.sections[sym.shndx]
    start = sym.value
    size = sym.size or (sec.size - start)
    buf = bytearray(sec.data)
    problems = []
    # 1) references to local data: original address reconstructed per group (symbol, addend)
    local_target: dict[tuple[int, int], int | None] = {}
    if orig is not None and orig_read is not None:
        parts: dict[tuple[int, int], dict] = {}
        for off, t, si, a in sec.relocs:
            if not (start <= off < start + size) or off - start + 4 > len(orig):
                continue
            sy = obj.syms[si]
            if not (sy.type == "STT_SECTION" or (sy.shndx in obj.sections and not sy.name)):
                continue
            if sy.shndx in obj.sections and obj.sections[sy.shndx].flags & SHF_EXEC:
                continue
            word = struct.unpack_from("<I", orig, off - start)[0]
            ot = _orig_target(t, word, at + (off - start))
            key = (si, a)
            if ot is None:
                parts.setdefault(key, {})["bad"] = True
                continue
            d = parts.setdefault(key, {})
            k, v = ot
            if k in d and d[k] != v:
                d["bad"] = True
            d[k] = v
        for key, d in parts.items():
            if d.get("bad"):
                continue
            if "abs" in d:
                tgt = d["abs"]
                if "page" in d and (d["page"] != tgt & ~0xFFF or d.get("lo12", tgt & 0xFFF) != tgt & 0xFFF):
                    continue
            elif "page" in d and "lo12" in d:
                tgt = d["page"] | d["lo12"]
            else:
                continue
            si, a = key
            ours = _local_data_bytes(obj, obj.syms[si].shndx, a)
            if not ours or not ours[0]:
                continue
            content, _ = ours
            try:
                if orig_read(tgt, len(content)) == content:
                    local_target[key] = tgt
            except Exception:  # noqa: BLE001
                pass
    for off, t, si, a in sec.relocs:
        if not (start <= off < start + size):
            continue
        sy = obj.syms[si]
        P = at + (off - start)
        name = sy.name
        if sy.type == "STT_SECTION" or (sy.shndx in obj.sections and not name):
            fsym = obj.func_at(sy.shndx, a) if sy.type == "STT_SECTION" and sy.shndx in obj.sections else None
            v = resolver(fsym.name) if fsym is not None else None
            if v is not None:
                S, a = v, 0            # TU static function with marker
            elif (si, a) in local_target and t not in GOT_RELOCS:
                S, a = local_target[(si, a)], 0   # local data with the same contents as the original
            else:
                problems.append(f"+{off - start:#x}: reference to TU-local data ({obj.sections[sy.shndx].name if sy.shndx in obj.sections else '?'})")
                continue
        else:
            v = resolver(name)
            if v is None:
                problems.append(f"+{off - start:#x}: symbol without original address: {name}")
                continue
            else:
                S = v
        if t in GOT_RELOCS:
            slot = got_lookup(S) if (got_lookup and S) else None
            if slot is None:
                problems.append(f"+{off - start:#x}: GOT access to {name}: no original GOT slot with that value")
                continue
            S, a = slot, 0
        try:
            _patch(buf, off, t, S, a, P)
        except LinkError as e:
            problems.append(str(e))
    return bytes(buf[start:start + size]), problems
