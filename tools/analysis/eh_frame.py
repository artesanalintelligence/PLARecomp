#!/usr/bin/env python3
"""Lists the FDEs of .eh_frame as (start, size): the most reliable function
boundaries of a binary without symbols.

Locating .eh_frame (in order):
  1. .eh_frame / .eh_frame_hdr sections, if the ELF has section headers;
  2. PT_GNU_EH_FRAME segment -> .eh_frame_hdr -> eh_frame_ptr;
  3. NSO MOD0 header (start of .text + 4) -> eh_frame_hdr_start;
  4. search for the typical .eh_frame_hdr signature (01 1b 03 3b) in the data.

Usage:
    python tools/analysis/eh_frame.py data/main.elf --csv tools/ghidra/out/eh_frame_functions.csv
"""
from __future__ import annotations

import argparse
import csv
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfimage import ElfImage, add_common_args  # noqa: E402

# DWARF EH pointer encodings (DW_EH_PE_*)
PE_ABSPTR, PE_ULEB128, PE_UDATA2, PE_UDATA4, PE_UDATA8 = 0x00, 0x01, 0x02, 0x03, 0x04
PE_SLEB128, PE_SDATA2, PE_SDATA4, PE_SDATA8 = 0x09, 0x0A, 0x0B, 0x0C
PE_PCREL, PE_TEXTREL, PE_DATAREL, PE_FUNCREL, PE_ALIGNED = 0x10, 0x20, 0x30, 0x40, 0x50
PE_INDIRECT, PE_OMIT = 0x80, 0xFF


@dataclass
class FDE:
    start: int      # va
    size: int
    fde_va: int
    cie_va: int
    lsda: int | None = None   # va of the LSDA (.gcc_except_table), if any


class EhError(Exception):
    pass


def uleb(img: ElfImage, va: int) -> tuple[int, int]:
    result = shift = 0
    p = va
    while True:
        b = img.u8(p)
        p += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, p - va


def sleb(img: ElfImage, va: int) -> tuple[int, int]:
    result = shift = 0
    p = va
    while True:
        b = img.u8(p)
        p += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                result -= 1 << shift
            return result, p - va


def read_encoded(img: ElfImage, va: int, enc: int, datarel_base: int | None = None,
                 apply: bool = True) -> tuple[int, int]:
    """Read an encoded pointer. Returns (value, bytes consumed)."""
    if enc == PE_OMIT:
        return 0, 0
    fmt = enc & 0x0F
    app = enc & 0x70
    if app == PE_ALIGNED:
        aligned = (va + 7) & ~7
        return img.u64(aligned), aligned - va + 8
    if fmt == PE_ABSPTR:
        val, n = img.u64(va), 8
        p = img.pointer_at(va)
        if p is not None and p.target is not None:
            val = p.target
    elif fmt == PE_UDATA2:
        val, n = int.from_bytes(img.read(va, 2), "little"), 2
    elif fmt == PE_UDATA4:
        val, n = img.u32(va), 4
    elif fmt == PE_UDATA8:
        val, n = img.u64(va), 8
    elif fmt == PE_SDATA2:
        val, n = int.from_bytes(img.read(va, 2), "little", signed=True), 2
    elif fmt == PE_SDATA4:
        val, n = img.s32(va), 4
    elif fmt == PE_SDATA8:
        val, n = img.s64(va), 8
    elif fmt == PE_ULEB128:
        val, n = uleb(img, va)
    elif fmt == PE_SLEB128:
        val, n = sleb(img, va)
    else:
        raise EhError(f"unsupported pointer encoding {enc:#x} at {va:#x}")
    if apply and val:
        if app == PE_PCREL:
            val += va
        elif app == PE_DATAREL:
            if datarel_base is None:
                raise EhError("DW_EH_PE_datarel without base")
            val += datarel_base
        elif app in (PE_TEXTREL, PE_FUNCREL):
            raise EhError(f"unsupported pointer application {enc:#x}")
        val &= (1 << 64) - 1
    if apply and enc & PE_INDIRECT and val:
        val = img.u64(val)
    return val, n


# ----------------------------------------------------------------- .eh_frame_hdr
def parse_eh_frame_hdr(img: ElfImage, hdr: int):
    """Return (eh_frame_va, [(pc, fde_va), ...])."""
    version = img.u8(hdr)
    if version != 1:
        raise EhError(f".eh_frame_hdr at {hdr:#x}: version {version}")
    ptr_enc, cnt_enc, tbl_enc = img.u8(hdr + 1), img.u8(hdr + 2), img.u8(hdr + 3)
    p = hdr + 4
    eh_frame, n = read_encoded(img, p, ptr_enc, hdr)
    p += n
    table = []
    if cnt_enc != PE_OMIT and tbl_enc != PE_OMIT:
        count, n = read_encoded(img, p, cnt_enc, hdr)
        p += n
        for _ in range(count):
            pc, n = read_encoded(img, p, tbl_enc, hdr)
            p += n
            fde, n = read_encoded(img, p, tbl_enc, hdr)
            p += n
            table.append((pc, fde))
    return eh_frame, table


def _hdr_plausible(img: ElfImage, hdr: int) -> bool:
    try:
        eh, table = parse_eh_frame_hdr(img, hdr)
    except (EhError, IndexError):
        return False
    if not img.is_mapped(eh) or not table:
        return False
    sample = table[:: max(1, len(table) // 64)]
    if not all(img.is_exec(pc) and img.is_mapped(f) for pc, f in sample):
        return False
    return all(a[0] <= b[0] for a, b in zip(table, table[1:]))


def find_mod0(img: ElfImage) -> int | None:
    """Locate the MOD0 header of a Switch module (image start + 4)."""
    start = img.segments[0].vaddr
    try:
        off = img.u32(start + 4)
        mod0 = start + off
        if img.read(mod0, 4) == b"MOD0":
            return mod0
    except IndexError:
        pass
    return None


def locate(img: ElfImage) -> dict:
    """Return {'eh_frame': va, 'eh_frame_end': va|None, 'hdr': va|None, 'method': str}."""
    sec = img.section(".eh_frame")
    hsec = img.section(".eh_frame_hdr")
    if sec is not None and sec["sh_addr"]:
        hdr = hsec["sh_addr"] if hsec is not None else None
        return {"eh_frame": sec["sh_addr"], "eh_frame_end": sec["sh_addr"] + sec["sh_size"],
                "hdr": hdr, "method": "section .eh_frame"}
    candidates = []
    if hsec is not None and hsec["sh_addr"]:
        candidates.append((hsec["sh_addr"], "section .eh_frame_hdr"))
    ph = img.phdr("PT_GNU_EH_FRAME")
    if ph is not None:
        candidates.append((ph["p_vaddr"], "PT_GNU_EH_FRAME"))
    mod0 = find_mod0(img)
    if mod0 is not None:
        candidates.append((mod0 + img.s32(mod0 + 0x10), "MOD0"))
    for hdr, how in candidates:
        if _hdr_plausible(img, hdr):
            eh, _ = parse_eh_frame_hdr(img, hdr)
            return {"eh_frame": eh, "eh_frame_end": None, "hdr": hdr, "method": how}
    # Last resort: search for the signature 01 1b 03 3b (pcrel|sdata4, udata4, datarel|sdata4)
    sig = b"\x01\x1b\x03\x3b"
    for seg in img.data_segments + img.exec_segments:
        blob = img.read(seg.vaddr, seg.filesz) if seg.filesz else b""
        i = blob.find(sig)
        while i >= 0:
            hdr = seg.vaddr + i
            if hdr % 4 == 0 and _hdr_plausible(img, hdr):
                eh, _ = parse_eh_frame_hdr(img, hdr)
                return {"eh_frame": eh, "eh_frame_end": None, "hdr": hdr, "method": "firma"}
            i = blob.find(sig, i + 1)
    raise EhError(".eh_frame not found")


# --------------------------------------------------------------------- .eh_frame
def parse_cie(img: ElfImage, cie: int, hdr: int | None) -> dict:
    length = img.u32(cie)
    p = cie + 4
    if length == 0xFFFFFFFF:
        length = img.u64(p)
        p += 8
        idsize = 8
    else:
        idsize = 4
    end = p + length
    cie_id = img.u64(p) if idsize == 8 else img.u32(p)
    if cie_id != 0:
        raise EhError(f"{cie:#x} is not a CIE")
    p += idsize
    version = img.u8(p)
    p += 1
    aug = img.cstring(p, 64) or b""
    p += len(aug) + 1
    aug = aug.decode("ascii", "replace")
    if "eh" in aug:
        p += 8
    _, n = uleb(img, p); p += n           # code_alignment_factor
    _, n = sleb(img, p); p += n           # data_alignment_factor
    if version == 1:
        p += 1
    else:
        _, n = uleb(img, p); p += n       # return_address_register
    info = {"fde_enc": PE_ABSPTR, "lsda_enc": PE_OMIT, "aug": aug, "end": end}
    if aug.startswith("z"):
        _, n = uleb(img, p); p += n
        for c in aug[1:]:
            if c == "R":
                info["fde_enc"] = img.u8(p); p += 1
            elif c == "L":
                info["lsda_enc"] = img.u8(p); p += 1
            elif c == "P":
                penc = img.u8(p); p += 1
                _, n = read_encoded(img, p, penc, hdr, apply=False); p += n
            elif c in "SBG":
                pass
            else:
                break  # unknown augmentation: the rest is skipped with aug_len
    return info


def iter_fdes(img: ElfImage, eh_frame: int, end: int | None, hdr: int | None):
    seg = img.segment_of(eh_frame)
    limit = end if end is not None else seg.end
    cies: dict[int, dict] = {}
    p = eh_frame
    while p + 4 <= limit:
        length = img.u32(p)
        if length == 0:
            break  # terminador
        hdr_size = 4
        if length == 0xFFFFFFFF:
            length = img.u64(p + 4)
            hdr_size = 12
        idfield = p + hdr_size
        nxt = idfield + length
        if nxt > limit:
            break
        cie_ptr = img.u64(idfield) if hdr_size == 12 else img.u32(idfield)
        if cie_ptr != 0:  # FDE
            cie = idfield - cie_ptr
            if cie not in cies:
                cies[cie] = parse_cie(img, cie, hdr)
            enc = cies[cie]["fde_enc"]
            q = idfield + (8 if hdr_size == 12 else 4)
            start, n = read_encoded(img, q, enc, hdr)
            size, n2 = read_encoded(img, q + n, enc & 0x0F, hdr, apply=False)
            lsda = None
            c = cies[cie]
            if c["aug"].startswith("z") and c["lsda_enc"] != PE_OMIT:
                r = q + n + n2
                _, n3 = uleb(img, r)
                try:
                    lsda, _ = read_encoded(img, r + n3, c["lsda_enc"], hdr)
                except (EhError, IndexError):
                    lsda = None
                lsda = lsda or None
            if size:
                yield FDE(start, size, p, cie, lsda)
        p = nxt


def parse_lsda(img: ElfImage, lsda: int, func_start: int) -> list[int]:
    """Landing pads of a GCC/clang LSDA (.gcc_except_table format)."""
    p = lsda
    lpstart_enc = img.u8(p); p += 1
    lpstart = func_start
    if lpstart_enc != PE_OMIT:
        lpstart, n = read_encoded(img, p, lpstart_enc); p += n
    ttype_enc = img.u8(p); p += 1
    if ttype_enc != PE_OMIT:
        _, n = uleb(img, p); p += n
    cs_enc = img.u8(p); p += 1
    cs_len, n = uleb(img, p); p += n
    end = p + cs_len
    pads = []
    while p < end:
        _, n = read_encoded(img, p, cs_enc, apply=False); p += n      # start
        _, n = read_encoded(img, p, cs_enc, apply=False); p += n      # longitud
        lp, n = read_encoded(img, p, cs_enc, apply=False); p += n     # landing pad
        _, n = uleb(img, p); p += n                                   # action
        if lp:
            pads.append(lpstart + lp)
    return pads


def landing_pads(img: ElfImage, fdes: list[FDE]) -> tuple[list[int], int]:
    """All landing pads (unwinder targets). Returns (pads, errors)."""
    out, errors = set(), 0
    for f in fdes:
        if f.lsda is None:
            continue
        try:
            for x in parse_lsda(img, f.lsda, f.start):
                if f.start <= x < f.start + f.size:
                    out.add(x)
                else:
                    errors += 1
        except (EhError, IndexError):
            errors += 1
    return sorted(out), errors


def collect(img: ElfImage) -> tuple[list[FDE], dict]:
    loc = locate(img)
    fdes = list(iter_fdes(img, loc["eh_frame"], loc["eh_frame_end"], loc["hdr"]))
    if loc["hdr"] is not None:
        try:
            _, table = parse_eh_frame_hdr(img, loc["hdr"])
            loc["hdr_fde_count"] = len(table)
        except (EhError, IndexError):
            pass
    loc["fde_count"] = len(fdes)
    # Deduplicate by start and sort
    by_start: dict[int, FDE] = {}
    for f in fdes:
        if f.start not in by_start or f.size > by_start[f.start].size:
            by_start[f.start] = f
    return sorted(by_start.values(), key=lambda f: f.start), loc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--csv", help="write start,size (absolute addresses) to this CSV")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)
    img = ElfImage(args.elf, args.base)
    fdes, loc = collect(img)
    outside = sum(1 for f in fdes if not img.is_exec(f.start))
    print(f"eh_frame at {img.to_abs(loc['eh_frame']):#x} (via {loc['method']}); "
          f"{len(fdes)} unique FDEs" + (f", hdr table: {loc.get('hdr_fde_count')}" if "hdr_fde_count" in loc else "")
          + (f"; {outside} outside executable code" if outside else ""), file=sys.stderr)
    if args.csv:
        Path(args.csv).parent.mkdir(parents=True, exist_ok=True)
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["Address", "Size"])
            for x in fdes:
                w.writerow([f"0x{img.to_abs(x.start):016x}", x.size])
    elif not args.quiet:
        for x in fdes:
            print(f"0x{img.to_abs(x.start):016x} {x.size:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
