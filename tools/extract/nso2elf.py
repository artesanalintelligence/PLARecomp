#!/usr/bin/env python3
"""Converts a Nintendo Switch NSO (or uncompressed NSO) into an AArch64 ELF64.

Pure Python + lz4. Inspired by nx2elf (shuffle2) and the switchbrew
documentation (NSO, MOD0). Designed so the ELF can serve as `data/main.elf`
for nx-decomp-tools/viking (`tools/check`), and for Ghidra/IDA/objdump.

What it does:
  * decompresses the segments (LZ4 block) and validates sizes and SHA-256
  * locates MOD0 -> .dynamic, .bss, .eh_frame_hdr
  * rebuilds the sections: .text .rodata .data .bss .dynamic .dynsym
    .dynstr .rela.dyn .rela.plt .relr.dyn .plt .got .got.plt .hash
    .gnu.hash .init_array .fini_array .init .fini .note.gnu.build-id
    .eh_frame_hdr .eh_frame (+ gaps such as .text.N/.rodata.N/.data.N)
  * places the sections at the indices pointed to by the st_shndx of
    .dynsym (like nx2elf), so readelf/objdump/Ghidra show the
    section of each symbol correctly
  * program headers: 3 x PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME, PT_NOTE

File layout: file_offset = (vaddr - base) + 0x1000, so that
p_offset ≡ p_vaddr (mod 0x1000) and the ELF is also loadable by
standard tools. Base 0 by default (what viking expects).

Usage:
  python nso2elf.py main.nso main.elf [--base 0x7100000000] [--resolve-got]
                    [--export-uncompressed main.uncompressed.nso] [--info]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import lz4.block as lz4block
except ImportError:  # pragma: no cover
    lz4block = None

# --------------------------------------------------------------------------
# ELF constants
# --------------------------------------------------------------------------
EM_AARCH64 = 183
ET_DYN = 3
PT_LOAD, PT_DYNAMIC, PT_NOTE, PT_GNU_EH_FRAME = 1, 2, 4, 0x6474E550
PF_X, PF_W, PF_R = 1, 2, 4
SHT_NULL, SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_HASH, SHT_DYNAMIC, SHT_NOTE, SHT_NOBITS = 0, 1, 2, 3, 4, 5, 6, 7, 8
SHT_DYNSYM, SHT_INIT_ARRAY, SHT_FINI_ARRAY, SHT_RELR, SHT_GNU_HASH = 11, 14, 15, 19, 0x6FFFFFF6
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR, SHF_INFO_LINK = 1, 2, 4, 0x40
SHN_UNDEF, SHN_LORESERVE, SHN_ABS = 0, 0xFF00, 0xFFF1
STB_LOCAL = 0
STT_TLS = 6

DT = dict(NULL=0, NEEDED=1, PLTRELSZ=2, PLTGOT=3, HASH=4, STRTAB=5, SYMTAB=6, RELA=7, RELASZ=8,
          RELAENT=9, STRSZ=10, SYMENT=11, INIT=12, FINI=13, SONAME=14, RPATH=15, SYMBOLIC=16,
          REL=17, RELSZ=18, RELENT=19, PLTREL=20, DEBUG=21, TEXTREL=22, JMPREL=23, BIND_NOW=24,
          INIT_ARRAY=25, FINI_ARRAY=26, INIT_ARRAYSZ=27, FINI_ARRAYSZ=28, RUNPATH=29, FLAGS=30,
          PREINIT_ARRAY=32, PREINIT_ARRAYSZ=33, RELRSZ=35, RELR=36, RELRENT=37,
          GNU_HASH=0x6FFFFEF5, VERSYM=0x6FFFFFF0, RELACOUNT=0x6FFFFFF9, RELCOUNT=0x6FFFFFFA,
          FLAGS_1=0x6FFFFFFB, VERDEF=0x6FFFFFFC, VERNEED=0x6FFFFFFE)
DT_NAME = {v: k for k, v in DT.items()}
# Tags whose value is an address (rebased with --base)
DT_PTR_TAGS = {DT[k] for k in ("PLTGOT", "HASH", "STRTAB", "SYMTAB", "RELA", "INIT", "FINI", "REL",
                               "JMPREL", "INIT_ARRAY", "FINI_ARRAY", "PREINIT_ARRAY", "RELR",
                               "GNU_HASH", "VERSYM", "VERDEF", "VERNEED", "DEBUG")}

R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027

SEG_NAMES = ("text", "rodata", "data")
HDR_PAD = 0x1000  # file offset = vaddr - base + HDR_PAD


class NsoError(Exception):
    pass


def align_up(x: int, a: int) -> int:
    return (x + a - 1) & ~(a - 1)


# --------------------------------------------------------------------------
# NSO
# --------------------------------------------------------------------------

@dataclass
class Segment:
    name: str
    file_offset: int
    mem_offset: int
    size: int            # decompressed size
    file_size: int       # size in the file (compressed or not)
    compressed: bool
    check_hash: bool
    sha256: bytes        # expected hash (header)
    data: bytes = b""

    @property
    def end(self) -> int:
        return self.mem_offset + self.size


@dataclass
class Nso:
    version: int
    flags: int
    segments: List[Segment]
    bss_size: int
    module_id: bytes
    module_name: bytes
    api_info: Tuple[int, int]
    dynstr: Tuple[int, int]
    dynsym: Tuple[int, int]
    raw_header: bytes
    warnings: List[str] = field(default_factory=list)

    @property
    def text(self) -> Segment:
        return self.segments[0]

    @property
    def rodata(self) -> Segment:
        return self.segments[1]

    @property
    def data(self) -> Segment:
        return self.segments[2]

    @property
    def build_id(self) -> str:
        return self.module_id.hex().upper()

    @property
    def build_id_short(self) -> str:
        return self.module_id.rstrip(b"\0").hex().upper()

    def image(self) -> bytearray:
        """In-memory image (without .bss) from 0 to the end of .data."""
        img = bytearray(self.data.end)
        for s in self.segments:
            img[s.mem_offset:s.end] = s.data
        return img


def parse_nso(blob: bytes, strict: bool = True) -> Nso:
    if len(blob) < 0x100 or blob[:4] != b"NSO0":
        raise NsoError("not an NSO (missing 'NSO0' signature)")
    version, _res, flags = struct.unpack_from("<III", blob, 4)
    segs: List[Segment] = []
    extra = []
    for i in range(3):
        foff, moff, size, x = struct.unpack_from("<IIII", blob, 0x10 + 0x10 * i)
        extra.append(x)
        fsize = struct.unpack_from("<I", blob, 0x60 + 4 * i)[0]
        sha = blob[0xA0 + 0x20 * i:0xC0 + 0x20 * i]
        segs.append(Segment(SEG_NAMES[i], foff, moff, size, fsize,
                            bool(flags & (1 << i)), bool(flags & (8 << i)), sha))
    module_name_off, module_name_size, bss_size = extra
    module_id = blob[0x40:0x60]
    api_info = struct.unpack_from("<II", blob, 0x88)
    dynstr = struct.unpack_from("<II", blob, 0x90)
    dynsym = struct.unpack_from("<II", blob, 0x98)
    nso = Nso(version, flags, segs, bss_size, module_id,
              blob[module_name_off:module_name_off + module_name_size],
              api_info, dynstr, dynsym, blob[:0x100])

    for s in segs:
        raw = blob[s.file_offset:s.file_offset + s.file_size]
        if len(raw) != s.file_size:
            raise NsoError(f"segment {s.name}: truncated file")
        if s.compressed:
            if lz4block is None:
                raise NsoError("missing lz4 module: python -m pip install lz4")
            try:
                data = lz4block.decompress(raw, uncompressed_size=s.size)
            except Exception as e:  # lz4.block.LZ4BlockError
                raise NsoError(f"segmento {s.name}: error LZ4: {e}")
        else:
            data = raw
        if len(data) != s.size:
            raise NsoError(f"segment {s.name}: size {len(data):#x}, expected {s.size:#x}")
        s.data = bytes(data)
        digest = hashlib.sha256(s.data).digest()
        if digest != s.sha256:
            msg = f"segment {s.name}: SHA-256 does not match the header"
            if s.check_hash and strict:
                raise NsoError(msg)
            nso.warnings.append(msg)
    # consistency of the memory layout
    if not (segs[0].mem_offset == 0 and segs[0].end <= segs[1].mem_offset and segs[1].end <= segs[2].mem_offset):
        nso.warnings.append("segments are not in the usual order text < rodata < data")
    return nso


def build_uncompressed_nso(nso: Nso) -> bytes:
    """Equivalent NSO without compression (like `nx2elf --export-uncompressed` / hactool --uncompressed)."""
    hdr = bytearray(nso.raw_header)
    flags = nso.flags & ~0x7
    struct.pack_into("<I", hdr, 0xC, flags)
    out = bytearray()
    pos = 0x100
    # module name right after the header, as Nintendo does
    name = nso.module_name
    struct.pack_into("<I", hdr, 0x1C, pos)
    pos += len(name)
    body = bytearray(name)
    for i, s in enumerate(nso.segments):
        struct.pack_into("<I", hdr, 0x10 + 0x10 * i, pos)
        struct.pack_into("<I", hdr, 0x60 + 4 * i, s.size)
        body += s.data
        pos += s.size
    out += hdr + body
    return bytes(out)


# --------------------------------------------------------------------------
# Module analysis (MOD0, dynamic, symbols)
# --------------------------------------------------------------------------

@dataclass
class Section:
    name: str
    sh_type: int
    flags: int
    addr: int            # relative to the base (image offset)
    size: int
    align: int = 1
    entsize: int = 0
    link_name: str = ""
    info: int = 0
    info_name: str = ""  # sh_info points to another section (rela.plt -> .plt)
    index: int = 0

    @property
    def end(self) -> int:
        return self.addr + self.size


@dataclass
class Module:
    nso: Nso
    image: bytearray
    mod0: int
    dynamic: int
    bss_start: int
    bss_end: int
    eh_hdr: Tuple[int, int]
    module_object: int
    dyn: List[Tuple[int, int]]
    dyninfo: Dict[str, int]
    syms: List[Tuple[int, int, int, int, int, int]]  # name, info, other, shndx, value, size
    sections: List[Section] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)

    def u32(self, off: int) -> int:
        return struct.unpack_from("<I", self.image, off)[0]

    def u64(self, off: int) -> int:
        return struct.unpack_from("<Q", self.image, off)[0]

    def segment_of(self, addr: int) -> Optional[int]:
        for i, s in enumerate(self.nso.segments):
            if s.mem_offset <= addr < s.end:
                return i
        if self.nso.data.end <= addr < max(self.bss_end, self.nso.data.end + self.nso.bss_size):
            return 3  # bss
        return None

    def sym_name(self, name_off: int) -> str:
        base = self.dyninfo.get("STRTAB", 0)
        end = self.image.index(b"\0", base + name_off)
        return self.image[base + name_off:end].decode("utf-8", "replace")

    def relr_addrs(self) -> List[int]:
        """Addresses affected by DT_RELR (packed relative relocations)."""
        off, size = self.dyninfo.get("RELR"), self.dyninfo.get("RELRSZ", 0)
        out: List[int] = []
        if not off or not size:
            return out
        where = 0
        for p in range(off, off + size, 8):
            e = self.u64(p)
            if not e & 1:
                out.append(e)
                where = e + 8
            else:
                bits = e >> 1
                for i in range(63):
                    if bits & (1 << i):
                        out.append(where + 8 * i)
                where += 63 * 8
        return out

    def relocs(self, key_off: str, key_size: str) -> List[Tuple[int, int, int, int]]:
        off, size = self.dyninfo.get(key_off), self.dyninfo.get(key_size, 0)
        if not off or not size:
            return []
        out = []
        for p in range(off, off + size, 24):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", self.image, p)
            out.append((r_offset, r_info >> 32, r_info & 0xFFFFFFFF, r_addend))
        return out


def _valid_dt(tag: int) -> bool:
    return 0 <= tag <= 0x40 or 0x6000000D <= tag <= 0x7FFFFFFF


def analyze(nso: Nso) -> Module:
    img = nso.image()
    if len(img) < 8:
        raise NsoError("image too small")
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if mod0 + 0x1C > len(img) or img[mod0:mod0 + 4] != b"MOD0":
        raise NsoError(f"MOD0 not found (offset {mod0:#x})")
    rel = struct.unpack_from("<6i", img, mod0 + 4)
    dynamic, bss_start, bss_end, eh_start, eh_end, modobj = (mod0 + r for r in rel)
    # dynamic table
    dyn = []
    p = dynamic
    while p + 16 <= len(img):
        tag, val = struct.unpack_from("<qQ", img, p)
        if not _valid_dt(tag):
            # GNU ld with --spare-dynamic-tags=0 (libnx) leaves no DT_NULL at the
            # end: what follows is already .got (GOT[0] = &_DYNAMIC).
            break
        dyn.append((tag, val))
        p += 16
        if tag == 0:
            break
    info: Dict[str, int] = {}
    for tag, val in dyn:
        name = DT_NAME.get(tag)
        if name and name not in info:
            info[name] = val
    mod = Module(nso, img, mod0, dynamic, bss_start, bss_end, (eh_start, eh_end), modobj, dyn, info, [])
    # dynamic symbols
    symtab = info.get("SYMTAB")
    nsyms = 0
    if nso.dynsym[1]:
        nsyms = nso.dynsym[1] // 24
        if symtab is not None and symtab != nso.rodata.mem_offset + nso.dynsym[0]:
            mod.notes.append("DT_SYMTAB does not match the NSO header; DT_SYMTAB is used")
    elif info.get("HASH"):
        nsyms = mod.u32(info["HASH"] + 4)
    if symtab is not None:
        for i in range(nsyms):
            mod.syms.append(struct.unpack_from("<IBBHQQ", img, symtab + 24 * i))
    build_sections(mod)
    return mod


def _find_plt(mod: Module) -> Optional[Tuple[int, int]]:
    """Search for the AArch64 PLT0 (stp x16, x30, [sp,#-16]!; adrp; ldr; add; br x17)."""
    pltrelsz = mod.dyninfo.get("PLTRELSZ", 0)
    if not pltrelsz:
        return None
    text = mod.nso.text.data
    needle = struct.pack("<I", 0xA9BF7BF0)
    start = 0
    while True:
        i = text.find(needle, start)
        if i < 0:
            return None
        if i % 4 == 0 and i + 32 <= len(text):
            w = struct.unpack_from("<8I", text, i)
            if ((w[1] & 0x9F00001F) == 0x90000010 and  # adrp x16, ...
                    (w[2] & 0xFFC003FF) == 0xF9400211 and  # ldr x17, [x16, #...]
                    (w[3] & 0xFFC003FF) == 0x91000210 and  # add x16, x16, #...
                    w[4] == 0xD61F0220):                   # br x17
                n = pltrelsz // 24
                return mod.nso.text.mem_offset + i, 0x20 + 0x10 * n
        start = i + 4


def _eh_frame_size(img: bytearray, start: int, limit: int) -> int:
    """Walk CIE/FDE from `start` to the terminator (length 0) or `limit`."""
    p = start
    while p + 4 <= limit:
        length = struct.unpack_from("<I", img, p)[0]
        if length == 0:
            return p + 4 - start
        if length == 0xFFFFFFFF:
            if p + 12 > limit:
                break
            length = struct.unpack_from("<Q", img, p + 4)[0]
            p += 12 + length
        else:
            p += 4 + length
    return min(p, limit) - start


def _read_encoded(img: bytearray, p: int, enc: int) -> Tuple[Optional[int], int]:
    """Read a DW_EH_PE value; returns (absolute value, new position)."""
    if enc == 0xFF:
        return None, p
    fmt = enc & 0x0F
    app = enc & 0x70
    sizes = {0x00: ("<Q", 8), 0x02: ("<H", 2), 0x03: ("<I", 4), 0x04: ("<Q", 8),
             0x0A: ("<h", 2), 0x0B: ("<i", 4), 0x0C: ("<q", 8)}
    if fmt not in sizes:
        return None, p
    f, n = sizes[fmt]
    v = struct.unpack_from(f, img, p)[0]
    if app == 0x10:  # pcrel
        v += p
    elif app not in (0x00, 0x30):  # datarel (0x30) relative to eh_frame_hdr: handled by the caller
        return None, p + n
    return v, p + n


def build_sections(mod: Module) -> None:
    nso, img, info = mod.nso, mod.image, mod.dyninfo
    S: List[Section] = []

    def add(*a, **k):
        s = Section(*a, **k)
        if s.size > 0:
            S.append(s)
        return s

    # --- dynamic structures ---
    ndyn = len(mod.dyn)
    add(".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, mod.dynamic, ndyn * 16, 8, 16, ".dynstr")
    if "STRTAB" in info:
        add(".dynstr", SHT_STRTAB, SHF_ALLOC, info["STRTAB"], info.get("STRSZ", nso.dynstr[1]), 1)
    if "SYMTAB" in info and mod.syms:
        last_local = max((i for i, s in enumerate(mod.syms) if (s[1] >> 4) == STB_LOCAL), default=0)
        add(".dynsym", SHT_DYNSYM, SHF_ALLOC, info["SYMTAB"], len(mod.syms) * 24, 8, 24, ".dynstr",
            info=last_local + 1)
    if "RELA" in info and info.get("RELASZ"):
        add(".rela.dyn", SHT_RELA, SHF_ALLOC, info["RELA"], info["RELASZ"], 8, 24, ".dynsym")
    if "RELR" in info and info.get("RELRSZ"):
        add(".relr.dyn", SHT_RELR, SHF_ALLOC, info["RELR"], info["RELRSZ"], 8, 8)
    if "HASH" in info:
        nb, nc = struct.unpack_from("<II", img, info["HASH"])
        add(".hash", SHT_HASH, SHF_ALLOC, info["HASH"], 8 + 4 * (nb + nc), 8, 4, ".dynsym")
    if "GNU_HASH" in info:
        gh = info["GNU_HASH"]
        nb, symndx, maskwords, _ = struct.unpack_from("<IIII", img, gh)
        size = 16 + 8 * maskwords + 4 * nb + 4 * max(0, len(mod.syms) - symndx)
        add(".gnu.hash", SHT_GNU_HASH, SHF_ALLOC, gh, size, 8, 0, ".dynsym")

    # --- PLT / GOT ---
    plt = _find_plt(mod)
    plt_sec = None
    if plt:
        plt_sec = add(".plt", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, plt[0], plt[1], 16, 16)
    jmprel = mod.relocs("JMPREL", "PLTRELSZ")
    if jmprel:
        add(".rela.plt", SHT_RELA, SHF_ALLOC | (SHF_INFO_LINK if plt_sec else 0), info["JMPREL"],
            info["PLTRELSZ"], 8, 24, ".dynsym", info_name=".plt" if plt_sec else "")
    jump_end = max((r[0] + 8 for r in jmprel if r[2] == R_AARCH64_JUMP_SLOT), default=0)
    if jump_end and info.get("PLTGOT"):
        add(".got.plt", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, info["PLTGOT"], jump_end - info["PLTGOT"], 8, 8)
    # --- init/fini arrays (before .got: they serve as a bound) ---
    if info.get("INIT_ARRAY") and info.get("INIT_ARRAYSZ"):
        add(".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE, info["INIT_ARRAY"], info["INIT_ARRAYSZ"], 8, 8)
    if info.get("FINI_ARRAY") and info.get("FINI_ARRAYSZ"):
        add(".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE, info["FINI_ARRAY"], info["FINI_ARRAYSZ"], 8, 8)

    # --- .got ---
    # nx2elf looks for &_DYNAMIC after the JUMP_SLOTs; with lld (Nintendo
    # binaries) that entry does not exist and nx2elf does not create .got. Here we start from
    # the GLOB_DAT relocations and extend the region over the contiguous
    # slots with relocations (or with &_DYNAMIC, typical of GNU ld), without
    # going past the next known section.
    rela = mod.relocs("RELA", "RELASZ")
    glob = sorted(r[0] for r in rela if r[2] == R_AARCH64_GLOB_DAT)
    if glob:
        slots = {r[0] for r in rela if r[2] in (R_AARCH64_GLOB_DAT, R_AARCH64_RELATIVE, R_AARCH64_ABS64)}
        slots.update(mod.relr_addrs())
        lo_bound = max([s.end for s in S if s.end <= glob[0]] + [nso.data.mem_offset if glob[0] >= nso.data.mem_offset else nso.rodata.mem_offset])
        hi_bound = min([s.addr for s in S if s.addr > glob[-1]] + [nso.data.end])
        dyn_ptr = mod.dynamic
        # GNU ld style: _GLOBAL_OFFSET_TABLE_[0] = &_DYNAMIC and .got extends
        # up to the next section (normally .got.plt).
        gnu_start = None
        p = (glob[0] - 8) & ~7
        while p >= lo_bound and glob[0] - p <= 0x80000:
            if mod.u64(p) == dyn_ptr:
                gnu_start = p
                break
            p -= 8
        if gnu_start is not None:
            start, end = gnu_start, hi_bound
        else:
            # lld style (Nintendo): without &_DYNAMIC; extended over the
            # contiguous slots that have a relocation.
            start = glob[0]
            while start - 8 >= lo_bound and start - 8 in slots:
                start -= 8
            end = glob[-1] + 8
            while end + 8 <= hi_bound and end in slots:
                end += 8
        add(".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, start, end - start, 8, 8)
    # .init/.fini: up to the first "ret" or "b <...>" (like nx2elf, but accepting both)
    def _ends(w: int) -> bool:
        return w == 0xD65F03C0 or (w & 0xFC000000) == 0x14000000
    for key, name, pred in (("INIT", ".init", _ends), ("FINI", ".fini", _ends)):
        a = info.get(key)
        if a and nso.text.mem_offset <= a < nso.text.end:
            for k in range(0x100):
                if a + 4 * k + 4 > nso.text.end:
                    break
                if pred(mod.u32(a + 4 * k)):
                    add(name, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, a, 4 * (k + 1), 4)
                    break

    # --- build id ---
    for seg in (nso.rodata, nso.text, nso.data):
        for dsz in (0x14, 0x10, 0x20):
            needle = struct.pack("<III", 4, dsz, 3) + b"GNU\0"
            i = seg.data.find(needle)
            if i >= 0 and i % 4 == 0:
                add(".note.gnu.build-id", SHT_NOTE, SHF_ALLOC, seg.mem_offset + i, 16 + dsz, 4)
                break
        else:
            continue
        break

    # --- eh_frame ---
    eh_start, eh_end = mod.eh_hdr
    if eh_end > eh_start and img[eh_start] == 1 and eh_start + 8 <= len(img):
        ptr_enc = img[eh_start + 1]
        frame, _ = _read_encoded(img, eh_start + 4, ptr_enc)
        if (ptr_enc & 0x70) == 0x30:  # datarel: relativo a eh_frame_hdr
            raw, _ = _read_encoded(img, eh_start + 4, ptr_enc & 0x0F)
            frame = eh_start + raw if raw is not None else None
        if frame is not None and 0 <= frame < len(img):
            seg = mod.segment_of(frame)
            limit = nso.segments[seg].end if seg is not None and seg < 3 else len(img)
            hdr_size = eh_end - eh_start
            if eh_start < frame:
                hdr_size = min(align_up(hdr_size, 4), frame - eh_start)
            add(".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC, eh_start, hdr_size, 4)
            add(".eh_frame", SHT_PROGBITS, SHF_ALLOC, frame, _eh_frame_size(img, frame, limit), 8)
        else:
            add(".eh_frame_hdr", SHT_PROGBITS, SHF_ALLOC, eh_start, eh_end - eh_start, 4)
    elif eh_end == eh_start and 0 < eh_start + 8 <= len(img):
        # Without .eh_frame_hdr (e.g. homebrew with libnx): MOD0 points to the start
        # of .eh_frame. Accepted if there is a valid CIE there (id 0, version 1/3).
        length, cie_id = struct.unpack_from("<II", img, eh_start)
        if 8 <= length < 0x1000 and cie_id == 0 and img[eh_start + 8] in (1, 3):
            seg = mod.segment_of(eh_start)
            limit = nso.segments[seg].end if seg is not None and seg < 3 else len(img)
            add(".eh_frame", SHT_PROGBITS, SHF_ALLOC, eh_start, _eh_frame_size(img, eh_start, limit), 8)

    # --- broad sections: the largest gap of each segment ---
    seg_flags = (SHF_ALLOC | SHF_EXECINSTR, SHF_ALLOC, SHF_ALLOC | SHF_WRITE)
    seg_names = (".text", ".rodata", ".data")
    broad: List[Section] = []
    for i, seg in enumerate(nso.segments):
        lo, hi = seg.mem_offset, seg.end
        if i == 2:
            hi = min(hi, max(mod.bss_start, lo)) if lo < mod.bss_start < hi else hi
        subs = sorted((s.addr, s.end) for s in S if s.addr < hi and s.end > lo)
        gaps = []
        cur = lo
        for a, b in subs:
            if a > cur:
                gaps.append((cur, a))
            cur = max(cur, b)
        if hi > cur:
            gaps.append((cur, hi))
        if not gaps:
            continue
        # Page-alignment padding at the start of the gap (e.g. between
        # .got.plt and a .data aligned to 0x1000): it is not part of the section.
        trimmed = []
        for a, b in gaps:
            pa = align_up(a, 0x1000)
            if a < pa < b and not any(img[a:pa]):
                trimmed.append((a, pa))
                a = pa
            trimmed.append((a, b))
        gaps = trimmed
        main = max(gaps, key=lambda g: g[1] - g[0])
        n = 0
        for a, b in gaps:
            if (a, b) == main:
                name = seg_names[i]
            else:
                # small gaps (alignment padding) do not deserve a section
                if b - a < 0x20 and not any(img[a:b]):
                    continue
                n += 1
                name = f"{seg_names[i]}.{n}"
            al = 0x10 if i == 0 else 8
            while al > 1 and a % al:
                al //= 2
            broad.append(Section(name, SHT_PROGBITS, seg_flags[i], a, b - a, al))
    S.extend(broad)

    # --- .bss ---
    # MOD0 gives the exact range; the NSO header rounds it up to a page (the
    # "end" symbol is usually there), so it is covered up to the larger of the two.
    bss_lo = mod.bss_start if mod.bss_end > mod.bss_start else nso.data.end
    bss_hi = max(mod.bss_end, nso.data.end + nso.bss_size)
    if bss_hi > bss_lo:
        S.append(Section(".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, bss_lo, bss_hi - bss_lo, 8))

    S.sort(key=lambda s: (s.addr, s.name))
    mod.sections = S
    _assign_indices(mod)


def _assign_indices(mod: Module) -> None:
    """Put each section at the index used by the st_shndx of .dynsym, if possible."""
    S = mod.sections
    wanted: Dict[int, int] = {}  # shndx -> address of its first defined symbol
    for name, info, other, shndx, value, size in mod.syms:
        if shndx == SHN_UNDEF or shndx >= SHN_LORESERVE or (info & 0xF) == STT_TLS:
            continue  # TLS symbols have offsets within the TLS block, not addresses
        wanted.setdefault(shndx, value)
    taken: Dict[int, Section] = {}
    for shndx, addr in sorted(wanted.items()):
        cands = [s for s in S if s.index == 0 and (s.addr <= addr < s.end or
                                                   (s.size == 0 and s.addr == addr) or
                                                   (s.sh_type == SHT_NOBITS and addr == s.end))]
        if not cands:
            # symbol at the exact end of a section (e.g. "end", "__bss_end")
            cands = [s for s in S if s.index == 0 and s.end == addr]
        if not cands:
            mod.notes.append(f"st_shndx {shndx}: no section for {addr:#x}")
            continue
        # the smallest one that contains it
        sec = min(cands, key=lambda s: s.size)
        sec.index = shndx
        taken[shndx] = sec
    nxt = 1
    for s in S:
        if s.index:
            continue
        while nxt in taken:
            nxt += 1
        s.index = nxt
        taken[nxt] = s
        nxt += 1


# --------------------------------------------------------------------------
# ELF writing
# --------------------------------------------------------------------------

def build_elf(nso: Nso, base: int = 0, resolve_got: bool = False, mod: Optional[Module] = None) -> bytes:
    mod = mod or analyze(nso)
    img = bytearray(mod.image)
    info = mod.dyninfo

    # --- optional rebase of the dynamic structures ---
    if base:
        for k, (tag, val) in enumerate(mod.dyn):
            if tag in DT_PTR_TAGS:
                struct.pack_into("<Q", img, mod.dynamic + 16 * k + 8, val + base)
        if "SYMTAB" in info:
            for i, s in enumerate(mod.syms):
                if s[3] != SHN_UNDEF and s[3] < SHN_LORESERVE:
                    struct.pack_into("<Q", img, info["SYMTAB"] + 24 * i + 8, s[4] + base)
        for koff, ksz in (("RELA", "RELASZ"), ("JMPREL", "PLTRELSZ")):
            if info.get(koff) and info.get(ksz):
                for p in range(info[koff], info[koff] + info[ksz], 24):
                    r_off, r_info, add_ = struct.unpack_from("<QQq", img, p)
                    if (r_info & 0xFFFFFFFF) == R_AARCH64_RELATIVE:
                        add_ += base
                    struct.pack_into("<QQq", img, p, r_off + base, r_info, add_)

    # --- fill the GOT with the targets of its relocations (optional) ---
    if resolve_got:
        got = [s for s in mod.sections if s.name in (".got",)]
        if got:
            g = got[0]
            filled = 0
            for r_off, sym, typ, add_ in mod.relocs("RELA", "RELASZ"):
                if not (g.addr <= r_off < g.end):
                    continue
                if typ == R_AARCH64_RELATIVE:
                    val = base + add_
                elif typ in (R_AARCH64_GLOB_DAT, R_AARCH64_ABS64) and sym < len(mod.syms) and \
                        mod.syms[sym][3] != SHN_UNDEF:
                    val = base + mod.syms[sym][4] + add_
                else:
                    continue
                struct.pack_into("<Q", img, r_off, val)
                filled += 1
            mod.notes.append(f"--resolve-got: {filled} .got entries written")

    data_seg = nso.data
    mem_end = max(data_seg.end + nso.bss_size, mod.bss_end)
    eh = next((s for s in mod.sections if s.name == ".eh_frame_hdr"), None)
    note = next((s for s in mod.sections if s.name == ".note.gnu.build-id"), None)

    # --- phdrs ---
    phdrs = []
    for i, seg in enumerate(nso.segments):
        flags = (PF_R | PF_X, PF_R, PF_R | PF_W)[i]
        memsz = seg.size if i < 2 else mem_end - seg.mem_offset
        phdrs.append((PT_LOAD, flags, seg.mem_offset + HDR_PAD, base + seg.mem_offset, seg.size, memsz, 0x1000))
    phdrs.append((PT_DYNAMIC, PF_R | PF_W, mod.dynamic + HDR_PAD, base + mod.dynamic, len(mod.dyn) * 16,
                  len(mod.dyn) * 16, 8))
    if eh:
        phdrs.append((PT_GNU_EH_FRAME, PF_R, eh.addr + HDR_PAD, base + eh.addr, eh.size, eh.size, 4))
    if note:
        phdrs.append((PT_NOTE, PF_R, note.addr + HDR_PAD, base + note.addr, note.size, note.size, 4))
    if 64 + 56 * len(phdrs) > HDR_PAD:
        raise NsoError("too many program headers")

    # --- shstrtab and shdrs ---
    nshdr = max(s.index for s in mod.sections) + 2  # +1 conteo, +1 .shstrtab
    shstr_index = nshdr - 1
    by_name = {s.name: s.index for s in mod.sections}
    shstrtab = bytearray(b"\0")
    name_off = {}
    for nm in [s.name for s in mod.sections] + [".shstrtab"]:
        if nm not in name_off:
            name_off[nm] = len(shstrtab)
            shstrtab += nm.encode() + b"\0"

    file_data_end = HDR_PAD + data_seg.end
    shstr_off = file_data_end
    shoff = align_up(shstr_off + len(shstrtab), 8)
    total = shoff + 64 * nshdr

    out = bytearray(total)
    # ELF header
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    struct.pack_into("<16sHHIQQQIHHHHHH", out, 0, ident, ET_DYN, EM_AARCH64, 1, base + nso.text.mem_offset,
                     64, shoff, 0, 64, 56, len(phdrs), 64, nshdr, shstr_index)
    for k, ph in enumerate(phdrs):
        p_type, p_flags, p_off, p_vaddr, p_filesz, p_memsz, p_align = ph
        struct.pack_into("<IIQQQQQQ", out, 64 + 56 * k, p_type, p_flags, p_off, p_vaddr, p_vaddr,
                         p_filesz, p_memsz, p_align)
    # segment contents
    for seg in nso.segments:
        out[HDR_PAD + seg.mem_offset:HDR_PAD + seg.end] = img[seg.mem_offset:seg.end]
    out[shstr_off:shstr_off + len(shstrtab)] = shstrtab

    for s in mod.sections:
        link = by_name.get(s.link_name, 0) if s.link_name else 0
        info_v = by_name.get(s.info_name, 0) if s.info_name else s.info
        sh_off = s.addr + HDR_PAD if s.sh_type != SHT_NOBITS else min(s.addr, data_seg.end) + HDR_PAD
        struct.pack_into("<IIQQQQIIQQ", out, shoff + 64 * s.index, name_off[s.name], s.sh_type, s.flags,
                         base + s.addr, sh_off, s.size, link, info_v, s.align, s.entsize)
    struct.pack_into("<IIQQQQIIQQ", out, shoff + 64 * shstr_index, name_off[".shstrtab"], SHT_STRTAB, 0,
                     0, shstr_off, len(shstrtab), 0, 0, 1, 0)
    return bytes(out)


# --------------------------------------------------------------------------
# Information
# --------------------------------------------------------------------------

SDK_MARKERS = (b"NintendoSdk_nnSdk-", b"SDK MW+Nintendo+NintendoSdk_nnSdk-")


def find_sdk_versions(nso: Nso) -> List[str]:
    import re
    found = set()
    for s in nso.segments:
        for m in re.finditer(rb"SDK MW\+[^\x00]{1,120}", s.data):
            found.add(m.group(0).decode("ascii", "replace"))
    return sorted(found)


def nso_info(nso: Nso, mod: Optional[Module] = None) -> dict:
    d = {
        "build_id": nso.build_id,
        "build_id_short": nso.build_id_short,
        "flags": nso.flags,
        "segments": {s.name: {"mem_offset": s.mem_offset, "size": s.size, "file_size": s.file_size,
                              "compressed": s.compressed, "sha256": hashlib.sha256(s.data).hexdigest()}
                     for s in nso.segments},
        "bss_size": nso.bss_size,
        "module_name": nso.module_name.rstrip(b"\0").decode("utf-8", "replace"),
        "warnings": list(nso.warnings),
    }
    if mod:
        d["mod0"] = {"offset": mod.mod0, "dynamic": mod.dynamic, "bss_start": mod.bss_start,
                     "bss_end": mod.bss_end, "eh_frame_hdr": list(mod.eh_hdr),
                     "module_object": mod.module_object}
        d["dynsym_count"] = len(mod.syms)
        d["imports"] = sum(1 for s in mod.syms if s[3] == SHN_UNDEF and s[0])
        d["sections"] = [{"index": s.index, "name": s.name, "addr": s.addr, "size": s.size}
                         for s in sorted(mod.sections, key=lambda s: s.index)]
        d["notes"] = list(mod.notes)
    return d


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="NSO -> ELF64 AArch64 (pure Python).")
    ap.add_argument("nso", type=Path)
    ap.add_argument("elf", type=Path, nargs="?")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0,
                    help="ELF base address (default 0, what viking expects)")
    ap.add_argument("--resolve-got", action="store_true",
                    help="write into .got the target of its RELATIVE/GLOB_DAT relocations")
    ap.add_argument("--export-uncompressed", type=Path, metavar="NSO",
                    help="also write an uncompressed NSO")
    ap.add_argument("--no-strict", action="store_true", help="do not abort if a hash does not match")
    ap.add_argument("--info", action="store_true", help="show the information as JSON")
    a = ap.parse_args(argv)
    try:
        nso = parse_nso(a.nso.read_bytes(), strict=not a.no_strict)
        mod = analyze(nso)
        if a.elf:
            a.elf.write_bytes(build_elf(nso, a.base, a.resolve_got, mod))
            print(f"ELF written to {a.elf}")
        if a.export_uncompressed:
            a.export_uncompressed.write_bytes(build_uncompressed_nso(nso))
        for w in nso.warnings + mod.notes:
            print(f"warning: {w}", file=sys.stderr)
        if a.info or not a.elf:
            print(json.dumps(nso_info(nso, mod), indent=2))
    except NsoError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
