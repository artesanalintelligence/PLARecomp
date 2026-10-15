"""View of an AArch64 ELF as an image loaded in memory (without Ghidra).

Designed for the main.elf produced by tools/extract/nso2elf.py, but it works
with any AArch64 ELF64 (ET_DYN/PIE or ET_EXEC), with or without section
headers: relocations and dynamic symbols are also read from the
PT_DYNAMIC segment.

Addresses:
  * Internally everything is handled in ELF virtual addresses ("va").
  * Outwards, absolute addresses are published: va + delta, where by
    default the image is placed at 0x7100000000 if the ELF is linked at 0
    (convention of botw/OdysseyDecomp and of viking).
"""
from __future__ import annotations

import bisect
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from elftools.elf.constants import P_FLAGS, SH_FLAGS
from elftools.elf.dynamic import DynamicSegment
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

DEFAULT_BASE = 0x71_0000_0000

# Windows consoles: avoid errors/mojibake when printing accents.
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

# AArch64 relocation types that produce a 64-bit pointer.
R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027
_PTR_RELOCS = {R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE}
_BR_X17 = bytes.fromhex("20021fd6")
_RNAME = {R_AARCH64_ABS64: "ABS64", R_AARCH64_GLOB_DAT: "GLOB_DAT",
          R_AARCH64_JUMP_SLOT: "JUMP_SLOT", R_AARCH64_RELATIVE: "RELATIVE"}


@dataclass(frozen=True)
class Segment:
    vaddr: int
    memsz: int
    offset: int
    filesz: int
    flags: int

    @property
    def end(self) -> int:
        return self.vaddr + self.memsz

    @property
    def exec(self) -> bool:
        return bool(self.flags & P_FLAGS.PF_X)

    @property
    def write(self) -> bool:
        return bool(self.flags & P_FLAGS.PF_W)


@dataclass(frozen=True)
class Symbol:
    name: str
    va: int
    size: int
    type: str      # STT_FUNC, STT_OBJECT, ...
    bind: str      # STB_GLOBAL, STB_LOCAL, ...
    defined: bool
    dynamic: bool


@dataclass(frozen=True)
class Pointer:
    """Contents of a pointer slot: local target or imported symbol."""
    target: int | None      # va of the target (None if it is an import)
    import_name: str | None
    addend: int = 0
    kind: str = "raw"       # RELATIVE, ABS64, GLOB_DAT, JUMP_SLOT or raw


def parse_int(v) -> int:
    if isinstance(v, int):
        return v
    return int(str(v), 0)


class ElfImage:
    def __init__(self, path: str | Path, base: str | int | None = "auto"):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        import io
        self.elf = ELFFile(io.BytesIO(self.data))
        if self.elf["e_machine"] != "EM_AARCH64":
            raise ValueError(f"{path}: is not AArch64 ({self.elf['e_machine']})")
        self.segments = sorted(
            (Segment(s["p_vaddr"], s["p_memsz"], s["p_offset"], s["p_filesz"], s["p_flags"])
             for s in self.elf.iter_segments() if s["p_type"] == "PT_LOAD" and s["p_memsz"]),
            key=lambda s: s.vaddr)
        if not self.segments:
            raise ValueError(f"{path}: no PT_LOAD segments")
        self._seg_starts = [s.vaddr for s in self.segments]
        # Code ranges: SHF_EXECINSTR sections if present (in a normal ELF
        # .rodata may share an R+X segment with .text); otherwise PF_X segments.
        code = []
        if self.elf.num_sections():
            for sec in self.elf.iter_sections():
                if sec["sh_flags"] & SH_FLAGS.SHF_EXECINSTR and sec["sh_flags"] & SH_FLAGS.SHF_ALLOC and sec["sh_size"]:
                    code.append((sec["sh_addr"], sec["sh_addr"] + sec["sh_size"]))
        if not code:
            code = [(s.vaddr, s.end) for s in self.segments if s.exec]
        self.code_ranges = sorted(code)
        self._code_starts = [c[0] for c in self.code_ranges]
        self.link_base = self.segments[0].vaddr
        if base in (None, "auto"):
            self.base = DEFAULT_BASE if self.link_base < 0x1_0000_0000 else self.link_base
        else:
            self.base = parse_int(base)
        self.delta = self.base - self.link_base

        self.symbols: list[Symbol] = []
        self.pointers: dict[int, Pointer] = {}
        self.num_relative = 0
        self._load_symbols_and_relocs()
        # If there is no R_AARCH64_RELATIVE (non-PIE ELF or relocations already
        # applied and discarded) the pointers are read as-is from memory.
        self.raw_pointers = self.num_relative == 0
        self._ptr_sorted: list[int] | None = None

    # ------------------------------------------------------------ addresses
    def to_abs(self, va: int) -> int:
        return va + self.delta

    def to_va(self, absaddr: int) -> int:
        return absaddr - self.delta

    def segment_of(self, va: int) -> Segment | None:
        i = bisect.bisect_right(self._seg_starts, va) - 1
        if i >= 0 and self.segments[i].vaddr <= va < self.segments[i].end:
            return self.segments[i]
        return None

    def is_mapped(self, va: int) -> bool:
        return self.segment_of(va) is not None

    def is_exec(self, va: int) -> bool:
        """Is va in executable code (SHF_EXECINSTR section or PF_X segment)?"""
        i = bisect.bisect_right(self._code_starts, va) - 1
        return i >= 0 and self.code_ranges[i][0] <= va < self.code_ranges[i][1]

    def is_data(self, va: int) -> bool:
        """Is va mapped and outside code?"""
        return self.is_mapped(va) and not self.is_exec(va)

    @property
    def exec_segments(self) -> list[Segment]:
        return [s for s in self.segments if s.exec]

    @property
    def data_segments(self) -> list[Segment]:
        return [s for s in self.segments if not s.exec]

    # ---------------------------------------------------------------- lectura
    def read(self, va: int, n: int) -> bytes:
        s = self.segment_of(va)
        if s is None or va + n > s.end:
            raise IndexError(f"read outside the image: {va:#x}+{n}")
        rel = va - s.vaddr
        out = bytearray(n)
        avail = max(0, min(n, s.filesz - rel))
        if avail:
            out[:avail] = self.data[s.offset + rel: s.offset + rel + avail]
        return bytes(out)  # the rest (bss) stays zero

    def u8(self, va):  return self.read(va, 1)[0]
    def u32(self, va): return struct.unpack("<I", self.read(va, 4))[0]
    def s32(self, va): return struct.unpack("<i", self.read(va, 4))[0]
    def u64(self, va): return struct.unpack("<Q", self.read(va, 8))[0]
    def s64(self, va): return struct.unpack("<q", self.read(va, 8))[0]

    def cstring(self, va: int, maxlen: int = 4096) -> bytes | None:
        s = self.segment_of(va)
        if s is None:
            return None
        end = min(s.end, va + maxlen)
        try:
            blob = self.read(va, end - va)
        except IndexError:
            return None
        z = blob.find(b"\0")
        return blob[:z] if z >= 0 else None

    # --------------------------------------------------------------- punteros
    def pointer_at(self, va: int) -> Pointer | None:
        """Return the pointer stored at `va` (via relocation or raw value)."""
        p = self.pointers.get(va)
        if p is not None:
            return p
        if self.raw_pointers:
            try:
                v = self.u64(va)
            except IndexError:
                return None
            if v and self.is_mapped(v):
                return Pointer(v, None)
        return None

    def has_reloc(self, va: int) -> bool:
        return va in self.pointers

    def pointer_slots(self):
        """Iterate (va, Pointer) over all pointer slots in data segments."""
        if not self.raw_pointers:
            if self._ptr_sorted is None:
                self._ptr_sorted = sorted(self.pointers)
            for va in self._ptr_sorted:
                if self.is_data(va):
                    yield va, self.pointers[va]
            return
        for seg in self.data_segments:
            start = (seg.vaddr + 7) & ~7
            blob = self.read(seg.vaddr, seg.memsz)
            for va in range(start, seg.end - 7, 8):
                o = va - seg.vaddr
                v = int.from_bytes(blob[o:o + 8], "little")
                if v and self.is_mapped(v):
                    yield va, Pointer(v, None)

    # ------------------------------------------------- symbols and relocations
    def _load_symbols_and_relocs(self):
        elf = self.elf
        dynseg = next((s for s in elf.iter_segments() if isinstance(s, DynamicSegment)), None)
        have_sections = elf.num_sections() > 0

        dynsyms = []
        dsec = elf.get_section_by_name(".dynsym") if have_sections else None
        if dsec is not None:
            dynsyms = list(dsec.iter_symbols())
        elif dynseg is not None:
            try:
                dynsyms = list(dynseg.iter_symbols())
            except Exception:
                dynsyms = []
        for sym in dynsyms:
            self._add_symbol(sym, dynamic=True)
        ssec = elf.get_section_by_name(".symtab") if have_sections else None
        if ssec is not None:
            for sym in ssec.iter_symbols():
                self._add_symbol(sym, dynamic=False)

        tables = []
        if have_sections:
            for sec in elf.iter_sections():
                if isinstance(sec, RelocationSection) and sec["sh_flags"] & SH_FLAGS.SHF_ALLOC:
                    tables.append(sec)
        if not tables and dynseg is not None:
            try:
                for _, t in dynseg.get_relocation_tables().items():
                    tables.append(t)
            except Exception:
                pass
        for table in tables:
            for r in table.iter_relocations():
                rtype = r["r_info_type"]
                if rtype not in _PTR_RELOCS:
                    continue
                kind = _RNAME[rtype]
                off = r["r_offset"]
                addend = r["r_addend"] if r.is_RELA() else 0
                if rtype == R_AARCH64_RELATIVE:
                    if not r.is_RELA():
                        addend = self.u64(off)
                    self.pointers[off] = Pointer(addend, None, 0, "RELATIVE")
                    self.num_relative += 1
                    continue
                symidx = r["r_info_sym"]
                if symidx == 0 or symidx >= len(dynsyms):
                    if not r.is_RELA():
                        addend = self.u64(off)
                    self.pointers[off] = Pointer(addend, None, 0, kind) if addend else Pointer(None, None, 0, kind)
                    continue
                sym = dynsyms[symidx]
                if sym["st_shndx"] != "SHN_UNDEF" and sym["st_value"]:
                    self.pointers[off] = Pointer(sym["st_value"] + addend, None, 0, kind)
                else:
                    self.pointers[off] = Pointer(None, sym.name, addend, kind)

        self.sym_by_name: dict[str, Symbol] = {}
        for s in self.symbols:
            if s.defined and (s.name not in self.sym_by_name or not self.sym_by_name[s.name].defined):
                self.sym_by_name[s.name] = s
            self.sym_by_name.setdefault(s.name, s)

    def _add_symbol(self, sym, dynamic: bool):
        name = sym.name
        if not name:
            return
        info = sym["st_info"]
        defined = sym["st_shndx"] != "SHN_UNDEF"
        if info["type"] in ("STT_FILE", "STT_SECTION"):
            return
        self.symbols.append(Symbol(name, sym["st_value"], sym["st_size"], info["type"],
                                   info["bind"], defined, dynamic))

    def defined_symbols(self, kind: str | None = None):
        seen = set()
        for s in self.symbols:
            if not s.defined or (kind and s.type != kind):
                continue
            key = (s.name, s.va)
            if key in seen:
                continue
            seen.add(key)
            yield s

    def symbols_at(self) -> dict[int, list[Symbol]]:
        out: dict[int, list[Symbol]] = {}
        for s in self.defined_symbols():
            out.setdefault(s.va, []).append(s)
        return out

    def plt_stubs(self) -> dict[int, str]:
        """PLT stubs -> imported name.

        Recognizes the standard AArch64 sequence
            adrp x16, page ; ldr x17, [x16, #off] ; add x16, x16, #off ; br x17
        and resolves the GOT slot (JUMP_SLOT/GLOB_DAT relocation) to its symbol.
        """
        out: dict[int, str] = {}
        for lo, hi in self.code_ranges:
            lo4 = (lo + 3) & ~3
            blob = self.read(lo4, hi - lo4)
            i = blob.find(_BR_X17)  # br x17
            while i >= 0:
                if i % 4 == 0 and i >= 12:
                    a, l, ad = struct.unpack_from("<III", blob, i - 12)
                    if (a & 0x9F00001F) == 0x90000010 and (l & 0xFFC003FF) == 0xF9400211:
                        pc = lo4 + i - 12
                        imm = (((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 3)
                        if imm & (1 << 20):
                            imm -= 1 << 21
                        slot = (pc & ~0xFFF) + (imm << 12) + ((l >> 10) & 0xFFF) * 8
                        p = self.pointers.get(slot)
                        if p is not None and p.import_name:
                            out[pc] = p.import_name
                i = blob.find(_BR_X17, i + 1)
        return out

    def phdr(self, p_type: str):
        for s in self.elf.iter_segments():
            if s["p_type"] == p_type:
                return s
        return None

    def section(self, name: str):
        if self.elf.num_sections() == 0:
            return None
        return self.elf.get_section_by_name(name)


def add_common_args(ap):
    ap.add_argument("elf", help="AArch64 ELF (e.g. data/main.elf)")
    ap.add_argument("--base", default="auto",
                    help="absolute address of the image (default auto: 0x7100000000 if the ELF is linked at 0)")
