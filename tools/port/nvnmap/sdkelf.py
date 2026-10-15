"""Minimal helpers to read build/port/sdk.elf (vaddr <-> file offset, relocs, ADRP xrefs)."""
from __future__ import annotations
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[3]
SDK_ELF = ROOT / "build" / "port" / "sdk.elf"


class Img:
    def __init__(self, path: Path = SDK_ELF):
        self.data = path.read_bytes()
        self.elf = ELFFile(open(path, "rb"))
        self.segs = [(s["p_vaddr"], s["p_offset"], s["p_filesz"], s["p_memsz"])
                     for s in self.elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        self.rel = {}
        for r in self.elf.get_section_by_name(".rela.dyn").iter_relocations():
            if r["r_info_type"] == 1027:  # R_AARCH64_RELATIVE
                self.rel[r["r_offset"]] = r["r_addend"]
        t = self.elf.get_section_by_name(".text")
        self.text_lo, self.text_hi = t["sh_addr"], t["sh_addr"] + t["sh_size"]

    def v2o(self, v: int) -> int:
        for va, o, fs, ms in self.segs:
            if va <= v < va + fs:
                return v - va + o
        raise KeyError(hex(v))

    def o2v(self, o: int) -> int:
        for va, fo, fs, ms in self.segs:
            if fo <= o < fo + fs:
                return o - fo + va
        raise KeyError(hex(o))

    def u32(self, v: int) -> int:
        return struct.unpack_from("<I", self.data, self.v2o(v))[0]

    def u64(self, v: int) -> int:
        return struct.unpack_from("<Q", self.data, self.v2o(v))[0]

    def ptr(self, v: int) -> int:
        """Pointer in .data with its RELATIVE reloc applied (base 0)."""
        return self.rel.get(v, self.u64(v))

    def cstr(self, v: int) -> str:
        o = self.v2o(v)
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")

    def adrp_refs(self, target_lo: int, target_hi: int):
        """ADRP pairs (+ the following immediate ADD/LDR on the same register) whose target falls in
        [target_lo, target_hi). Returns [(pc_adrp, target)]."""
        out = []
        base = self.v2o(self.text_lo)
        n = (self.text_hi - self.text_lo) // 4
        words = struct.unpack_from(f"<{n}I", self.data, base)
        for i, w in enumerate(words):
            if (w & 0x9F000000) != 0x90000000:
                continue
            rd = w & 31
            immlo = (w >> 29) & 3
            immhi = (w >> 5) & 0x7FFFF
            imm = ((immhi << 2) | immlo) << 12
            if imm & (1 << 32):
                imm -= 1 << 33
            pc = self.text_lo + 4 * i
            page = (pc & ~0xFFF) + imm
            if not (target_lo & ~0xFFF) <= page <= target_hi:
                continue
            for j in range(i + 1, min(i + 8, n)):
                w2 = words[j]
                if (w2 & 0xFF800000) == 0x91000000 and ((w2 >> 5) & 31) == rd:  # ADD imm
                    dst = page + ((w2 >> 10) & 0xFFF)
                    if target_lo <= dst < target_hi:
                        out.append((pc, dst))
                    break
                if (w2 & 0x3B000000) == 0x39000000 and ((w2 >> 5) & 31) == rd:  # LDR/STR uimm
                    size = w2 >> 30
                    dst = page + (((w2 >> 10) & 0xFFF) << size)
                    if target_lo <= dst < target_hi:
                        out.append((pc, dst))
                    break
        return out
