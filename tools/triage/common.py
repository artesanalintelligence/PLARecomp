"""Shared triage utilities: main.elf image, function list,
disassembly and AArch64 instruction normalization.

Address convention: same as viking/botw, the image is placed at
0x7100000000 (main.elf is linked at 0).
"""
from __future__ import annotations

import csv
import struct
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "data"
GHIDRA_OUT = ROOT / "tools" / "ghidra" / "out"
CACHE = Path(__file__).resolve().parent / "cache"
BASE = 0x7100000000


class Image:
    """Minimal view of the PT_LOADs of an ELF (read-only)."""

    def __init__(self, path: Path = DATA / "main.elf", base: int = BASE):
        raw = path.read_bytes()
        self.base = base
        (e_phoff,) = struct.unpack_from("<Q", raw, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", raw, 0x36)
        self.segs = []
        for i in range(e_phnum):
            p_type, p_flags, p_off, p_va, _pa, p_fsz, p_msz, _al = struct.unpack_from(
                "<IIQQQQQQ", raw, e_phoff + i * e_phentsize)
            if p_type == 1:  # PT_LOAD
                data = raw[p_off:p_off + p_fsz] + b"\0" * (p_msz - p_fsz)
                self.segs.append((p_va + base, p_va + base + p_msz, p_flags, data))
        self.segs.sort()

    def read(self, va: int, n: int) -> bytes:
        for lo, hi, _f, data in self.segs:
            if lo <= va and va + n <= hi:
                return data[va - lo: va - lo + n]
        raise ValueError(f"unmapped address {va:#x}+{n}")

    def is_exec(self, va: int) -> bool:
        return any(lo <= va < hi and (f & 1) for lo, hi, f, _ in self.segs)

    def text_range(self):
        for lo, hi, f, _ in self.segs:
            if f & 1:
                return lo, hi
        raise RuntimeError("no executable segment")


def load_functions(path: Path = DATA / "main_functions.csv"):
    """Sorted list of (addr, size, name) from the viking CSV."""
    out = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            out.append((int(r["Address"], 16), int(r["Size"]), r["Name"]))
    out.sort()
    return out


# --------------------------------------------------------------------------
# Instruction normalization (masking of relocatable fields)
# --------------------------------------------------------------------------
MASK_IMM26 = 0x03FFFFFF          # B / BL
MASK_ADRP = 0x60FFFFE0           # ADR / ADRP: immlo + immhi
MASK_IMM12 = 0x003FFC00          # ADD imm / LDR/STR unsigned offset
MASK_IMM19 = 0x00FFFFE0          # LDR literal / B.cond / CBZ

# AArch64 relocation types (ELF) -> mask of the affected field
RELOC_MASK = {
    282: MASK_IMM26,  # R_AARCH64_JUMP26
    283: MASK_IMM26,  # R_AARCH64_CALL26
    274: MASK_ADRP,   # ADR_PREL_LO21
    275: MASK_ADRP,   # ADR_PREL_PG_HI21
    276: MASK_ADRP,   # ADR_PREL_PG_HI21_NC
    311: MASK_ADRP,   # ADR_GOT_PAGE
    277: MASK_IMM12,  # ADD_ABS_LO12_NC
    278: MASK_IMM12,  # LDST8_ABS_LO12_NC
    284: MASK_IMM12,  # LDST16
    285: MASK_IMM12,  # LDST32
    286: MASK_IMM12,  # LDST64
    299: MASK_IMM12,  # LDST128
    312: MASK_IMM12,  # LD64_GOT_LO12_NC
    273: MASK_IMM19,  # LD_PREL_LO19
    280: MASK_IMM19,  # CONDBR19
    279: 0x0007FFE0,  # TSTBR14
}


def words(b: bytes):
    return list(struct.unpack(f"<{len(b) // 4}I", b[: len(b) // 4 * 4]))


def generic_mask(w: int) -> int:
    """Heuristic mask for already linked code (without relocations):
    masks the immediates of BL/B, ADRP/ADR and LDR literal. The LO12 of
    ADD/LDR are handled separately (they depend on the previous ADRP)."""
    if (w & 0x7C000000) == 0x14000000:      # B / BL
        return MASK_IMM26
    if (w & 0x1F000000) == 0x10000000:      # ADR / ADRP
        return MASK_ADRP
    return 0


def is_adrp(w: int) -> bool:
    return (w & 0x9F000000) == 0x90000000


def adrp_rd(w: int) -> int:
    return w & 0x1F


def lo12_user(w: int):
    """If w is ADD (imm) or LDR/STR (unsigned imm), return the base register."""
    if (w & 0x7F000000) == 0x11000000:      # ADD (immediate), 32/64 bits
        return (w >> 5) & 0x1F
    if (w & 0x3B000000) == 0x39000000:      # LDR/STR (unsigned immediate)
        return (w >> 5) & 0x1F
    return None


def normalize_linked(ws):
    """Normalize linked code: masks B/BL that leave the function,
    ADRP and the LO12 of the ADD/LDR/STR that uses the register of a previous ADRP."""
    n = len(ws)
    out = list(ws)
    pending = {}
    for i, w in enumerate(ws):
        if (w & 0x7C000000) == 0x14000000:
            imm = w & MASK_IMM26
            if imm & 0x2000000:
                imm -= 0x4000000
            tgt = i + imm
            if not (0 <= tgt < n) or (w & 0x80000000):  # BL always, B if it leaves
                out[i] = w & ~MASK_IMM26
            continue
        if is_adrp(w):
            out[i] = w & ~MASK_ADRP
            pending[adrp_rd(w)] = i
            continue
        base = lo12_user(w)
        if base is not None and base in pending:
            out[i] = w & ~MASK_IMM12
    return out


@lru_cache(maxsize=1)
def capstone_md():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    return md


def disasm(b: bytes, addr: int = 0):
    md = capstone_md()
    return [(i.address, i.mnemonic, i.op_str) for i in md.disasm(b, addr)]
