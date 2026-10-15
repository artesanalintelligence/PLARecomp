"""Reading of AArch64 relocatable ELF objects (.o) compiled with
-ffunction-sections: returns the functions in the order of their sections
(the order in which the linker will place them) with their bytes and relocations."""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

from common import RELOC_MASK, words


@dataclass
class ObjFunc:
    name: str
    obj: str
    section: str
    data: bytes
    # offset (bytes) -> (relocation type, name of the target symbol)
    relocs: dict = field(default_factory=dict)
    binding: str = "GLOBAL"
    align: int = 4

    @property
    def size(self) -> int:
        return len(self.data)

    def masked(self):
        ws = words(self.data)
        out = list(ws)
        for off, (rtype, _sym) in self.relocs.items():
            m = RELOC_MASK.get(rtype)
            if m is not None and off // 4 < len(out):
                out[off // 4] &= ~m
        return out


def read_object(path: Path) -> list[ObjFunc]:
    funcs: list[ObjFunc] = []
    with open(path, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        syms = list(symtab.iter_symbols())
        by_sec: dict[int, list] = {}
        for s in syms:
            if s["st_info"]["type"] == "STT_FUNC" and isinstance(s["st_shndx"], int):
                by_sec.setdefault(s["st_shndx"], []).append(s)
        relsecs = {}
        for sec in elf.iter_sections():
            if isinstance(sec, RelocationSection):
                relsecs[sec["sh_info"]] = sec
        for idx, sec in enumerate(elf.iter_sections()):
            if idx not in by_sec:
                continue
            data = sec.data()
            rels = []
            if idx in relsecs:
                for r in relsecs[idx].iter_relocations():
                    sym = syms[r["r_info_sym"]]
                    name = sym.name
                    if not name and isinstance(sym["st_shndx"], int):
                        name = elf.get_section(sym["st_shndx"]).name
                    rels.append((r["r_offset"], r["r_info_type"], name))
            fs = sorted(by_sec[idx], key=lambda s: s["st_value"])
            for i, s in enumerate(fs):
                start = s["st_value"]
                size = s["st_size"] or ((fs[i + 1]["st_value"] if i + 1 < len(fs) else len(data)) - start)
                rel = {o - start: (t, n) for o, t, n in rels if start <= o < start + size}
                funcs.append(ObjFunc(s.name, path.name, sec.name, data[start:start + size], rel,
                                     s["st_info"]["bind"].replace("STB_", ""), sec["sh_addralign"]))
    return funcs
