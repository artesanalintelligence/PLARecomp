"""Minimal ELF64 reader for the tests (independent of nso2elf.py)."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional


@dataclass
class Shdr:
    index: int
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    align: int
    entsize: int


@dataclass
class Phdr:
    type: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    align: int


@dataclass
class Sym:
    name: str
    value: int
    size: int
    info: int
    other: int
    shndx: int

    @property
    def type(self) -> int:
        return self.info & 0xF

    @property
    def bind(self) -> int:
        return self.info >> 4


class Elf:
    def __init__(self, data: bytes):
        assert data[:4] == b"\x7fELF" and data[4] == 2, "not ELF64"
        self.data = data
        (self.type, self.machine, _v, self.entry, phoff, shoff, _f, _eh, phentsize, phnum,
         shentsize, shnum, shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
        self.phdrs = []
        for i in range(phnum):
            t, fl, off, va, _pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", data, phoff + phentsize * i)
            self.phdrs.append(Phdr(t, fl, off, va, fs, ms, al))
        raw = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + shentsize * i) for i in range(shnum)]
        stroff = raw[shstrndx][4] if shnum else 0
        self.shdrs: List[Shdr] = []
        for i, r in enumerate(raw):
            nm_end = data.index(b"\0", stroff + r[0])
            name = data[stroff + r[0]:nm_end].decode()
            self.shdrs.append(Shdr(i, name, r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]))

    @classmethod
    def load(cls, path: Path) -> "Elf":
        return cls(Path(path).read_bytes())

    def section(self, name: str) -> Optional[Shdr]:
        return next((s for s in self.shdrs if s.name == name), None)

    def read(self, addr: int, size: int) -> bytes:
        for p in self.phdrs:
            if p.type == 1 and p.vaddr <= addr and addr + size <= p.vaddr + p.filesz:
                o = p.offset + addr - p.vaddr
                return self.data[o:o + size]
        raise ValueError(f"{addr:#x}+{size:#x} is not in any PT_LOAD")

    def section_bytes(self, s: Shdr) -> bytes:
        return self.data[s.offset:s.offset + s.size]

    def dynsyms(self) -> List[Sym]:
        ds = next(s for s in self.shdrs if s.type == 11)
        strtab = self.shdrs[ds.link]
        out = []
        for i in range(ds.size // 24):
            n, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.data, ds.offset + 24 * i)
            so = strtab.offset + n
            name = self.data[so:self.data.index(b"\0", so)].decode()
            out.append(Sym(name, value, size, info, other, shndx))
        return out

    def dynamic(self) -> Dict[int, int]:
        p = next(p for p in self.phdrs if p.type == 2)
        d = {}
        for k in range(p.filesz // 16):
            tag, val = struct.unpack_from("<qQ", self.data, p.offset + 16 * k)
            if tag == 0:
                break
            d.setdefault(tag, val)
        return d

    def vaddr_to_offset(self, addr: int) -> int:
        for p in self.phdrs:
            if p.type == 1 and p.vaddr <= addr < p.vaddr + p.filesz:
                return p.offset + addr - p.vaddr
        raise ValueError(hex(addr))
