"""In-memory image of data/main.elf loaded at 0x7100000000 with relocations applied.

Computed once and stored in tools/decomp/cache/image_<key>.bin (+ .json), so
verify.py and ctx.py start in milliseconds. Imports (symbols of the `sdk`
module) get fake addresses at IMPORT_BASE, one per symbol, so that calls and
accesses through the GOT are deterministic and identifiable.
"""
from __future__ import annotations

import json
import struct
import sys
from functools import lru_cache
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import CACHE, IMAGE_BASE, MAIN_ELF  # noqa: E402

IMPORT_BASE = 0x71_0500_0000      # fake addresses of the imports (0x40 bytes each)
IMPORT_STRIDE = 0x40
R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027


class Image:
    def __init__(self, blob: bytearray, meta: dict):
        self.blob = blob                      # bytes from IMAGE_BASE to the end of .bss
        self.meta = meta
        self.base = IMAGE_BASE
        self.size = len(blob)
        self.segments = [(int(s["start"], 16), int(s["end"], 16), s["perm"]) for s in meta["segments"]]
        self.imports = {int(k, 16): v for k, v in meta["imports"].items()}       # fake addr -> name
        self.import_addr = {v: k for k, v in self.imports.items()}
        self.got = {int(k, 16): v for k, v in meta["got"].items()}               # slot -> imported name
        self.plt = {int(k, 16): v for k, v in meta["plt"].items()}               # stub -> imported name
        self.plt_by_name = {}
        for a, n in sorted(self.plt.items()):
            self.plt_by_name.setdefault(n, a)
        self.dynsyms = {k: int(v, 16) for k, v in meta["dynsyms"].items()}       # defined symbols
        self.text = next((s, e) for s, e, p in self.segments if "x" in p)
        self.rodata = [(s, e) for s, e, p in self.segments if p == "r"]

    # ------------------------------------------------------------- reading
    def contains(self, a: int, n: int = 1) -> bool:
        return self.base <= a and a + n <= self.base + self.size

    def read(self, a: int, n: int) -> bytes:
        o = a - self.base
        return bytes(self.blob[o:o + n])

    def u32(self, a: int) -> int:
        return struct.unpack_from("<I", self.blob, a - self.base)[0]

    def u64(self, a: int) -> int:
        return struct.unpack_from("<Q", self.blob, a - self.base)[0]

    def is_code(self, a: int) -> bool:
        return self.text[0] <= a < self.text[1]

    def is_readonly(self, a: int) -> bool:
        return any(s <= a < e for s, e in self.rodata) or self.is_code(a)

    def cstring(self, a: int, maxlen: int = 256) -> bytes | None:
        if not self.contains(a):
            return None
        o = a - self.base
        end = self.blob.find(b"\0", o, o + maxlen)
        if end < 0:
            return None
        return bytes(self.blob[o:end])


def _key() -> str:
    st = MAIN_ELF.stat()
    return f"{st.st_size:x}_{int(st.st_mtime):x}"


def build_image() -> tuple[bytearray, dict]:
    from elftools.elf.elffile import ELFFile

    data = MAIN_ELF.read_bytes()
    import io
    elf = ELFFile(io.BytesIO(data))
    loads = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
    end = max(s["p_vaddr"] + s["p_memsz"] for s in loads)
    end = (end + 0xFFF) & ~0xFFF
    blob = bytearray(end)
    segs = []
    for s in loads:
        va, fs, ms, off = s["p_vaddr"], s["p_filesz"], s["p_memsz"], s["p_offset"]
        blob[va:va + fs] = data[off:off + fs]
        fl = s["p_flags"]
        perm = ("r" if fl & 4 else "") + ("w" if fl & 2 else "") + ("x" if fl & 1 else "")
        segs.append({"start": f"{IMAGE_BASE + va:x}", "end": f"{IMAGE_BASE + ((va + ms + 0xFFF) & ~0xFFF):x}",
                     "perm": perm})

    dynsym = elf.get_section_by_name(".dynsym")
    syms = list(dynsym.iter_symbols())
    imports: dict[str, int] = {}
    dynsyms: dict[str, str] = {}
    for s in syms:
        if not s.name:
            continue
        if s["st_shndx"] == "SHN_UNDEF":
            if s.name not in imports:
                imports[s.name] = IMPORT_BASE + len(imports) * IMPORT_STRIDE
        else:
            dynsyms[s.name] = f"{IMAGE_BASE + s['st_value']:x}"

    dt = np.dtype([("off", "<u8"), ("info", "<u8"), ("add", "<i8")])
    got: dict[str, str] = {}
    view = np.frombuffer(blob, dtype=np.uint8)
    for secname in (".rela.dyn", ".rela.plt"):
        sec = elf.get_section_by_name(secname)
        if sec is None:
            continue
        rel = np.frombuffer(sec.data(), dtype=dt)
        typ = (rel["info"] & 0xFFFFFFFF).astype(np.int64)
        symi = (rel["info"] >> 32).astype(np.int64)
        vals = np.zeros(len(rel), dtype=np.uint64)
        isrel = typ == R_RELATIVE
        vals[isrel] = (rel["add"][isrel] + IMAGE_BASE).astype(np.uint64)
        for i in np.nonzero(~isrel)[0]:
            t = int(typ[i])
            if t not in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT):
                continue
            s = syms[int(symi[i])]
            add = int(rel["add"][i])
            if s["st_shndx"] == "SHN_UNDEF":
                v = imports.get(s.name, IMPORT_BASE) + add
                got[f"{IMAGE_BASE + int(rel['off'][i]):x}"] = s.name
            else:
                v = IMAGE_BASE + s["st_value"] + add
            vals[i] = v
        ok = (typ == R_RELATIVE) | (typ == R_ABS64) | (typ == R_GLOB_DAT) | (typ == R_JUMP_SLOT)
        offs = rel["off"][ok].astype(np.int64)
        v8 = vals[ok]
        for k in range(8):
            view[offs + k] = ((v8 >> np.uint64(8 * k)) & np.uint64(0xFF)).astype(np.uint8)

    # PLT stubs -> imported name (adrp x16; ldr x17,[x16,#o]; add x16,..; br x17)
    plt: dict[str, str] = {}
    pltsec = elf.get_section_by_name(".plt")
    if pltsec is not None:
        a0, n = pltsec["sh_addr"], pltsec["sh_size"]
        for pc in range(a0, a0 + n - 12, 4):
            ad, ld = struct.unpack_from("<II", blob, pc)
            if (ad & 0x9F00001F) == 0x90000010 and (ld & 0xFFC003FF) == 0xF9400211 and \
                    struct.unpack_from("<I", blob, pc + 12)[0] == 0xD61F0220:
                imm = (((ad >> 5) & 0x7FFFF) << 2) | ((ad >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                slot = (pc & ~0xFFF) + (imm << 12) + ((ld >> 10) & 0xFFF) * 8
                nm = got.get(f"{IMAGE_BASE + slot:x}")
                if nm:
                    plt[f"{IMAGE_BASE + pc:x}"] = nm
    meta = {"key": _key(), "base": f"{IMAGE_BASE:x}", "size": len(blob), "segments": segs,
            "imports": {f"{v:x}": k for k, v in imports.items()}, "got": got, "plt": plt, "dynsyms": dynsyms}
    return blob, meta


@lru_cache(maxsize=1)
def load_image() -> Image:
    import os
    from common import FileLock
    CACHE.mkdir(parents=True, exist_ok=True)
    key = _key()
    binp, jsp = CACHE / f"image_{key}.bin", CACHE / f"image_{key}.json"
    if not (binp.exists() and jsp.exists()):
        with FileLock(CACHE / "image_build"):
            if not (binp.exists() and jsp.exists()):
                blob, meta = build_image()
                tmp = CACHE / f"image_{key}.{os.getpid()}.bintmp"
                tmp.write_bytes(blob)
                os.replace(tmp, binp)
                tmpj = CACHE / f"image_{key}.{os.getpid()}.jsontmp"
                tmpj.write_text(json.dumps(meta), encoding="utf-8")
                os.replace(tmpj, jsp)   # the .json is published last
                return Image(blob, meta)
    meta = json.loads(jsp.read_text(encoding="utf-8"))
    return Image(bytearray(binp.read_bytes()), meta)

if __name__ == "__main__":
    import time
    t = time.time()
    im = load_image()
    print(f"image {im.size:#x} bytes, {len(im.imports)} imports, {len(im.plt)} PLT stubs, "
          f"{len(im.got)} import GOT slots, {time.time() - t:.2f} s")
