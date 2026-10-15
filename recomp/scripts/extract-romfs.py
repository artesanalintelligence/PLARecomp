#!/usr/bin/env python3
"""Extracts a RomFS image (romfs.bin of the AOT export, with the update already applied) to a folder.

The suyu exporter leaves the effective RomFS (base + update) in <pkg>/exefs/romfs.bin,
unencrypted: 0x50-byte header, directory and file tables, and data. No keys or hactool
are needed.

    python recomp/scripts/extract-romfs.py D:/PLARecomp/exports/pla-v0/pkg/exefs/romfs.bin D:/PLARecomp/romfs
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

NONE = 0xFFFFFFFF


def main() -> int:
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    with src.open("rb") as f:
        hdr = struct.unpack("<10Q", f.read(0x50))
        if hdr[0] != 0x50:
            raise SystemExit(f"{src}: unexpected RomFS header (size {hdr[0]:#x})")
        dir_meta_off, dir_meta_size = hdr[3], hdr[4]
        file_meta_off, file_meta_size = hdr[7], hdr[8]
        data_off = hdr[9]
        f.seek(dir_meta_off)
        dirs = f.read(dir_meta_size)
        f.seek(file_meta_off)
        files = f.read(file_meta_size)

        def name(table: bytes, off: int, fixed: int) -> str:
            n = struct.unpack_from("<I", table, off + fixed - 4)[0]
            return table[off + fixed: off + fixed + n].decode("utf-8")

        count = total = 0

        def walk_dir(off: int, path: Path) -> None:
            nonlocal count, total
            _parent, _sib, child, first_file, _hash, _n = struct.unpack_from("<6I", dirs, off)
            path.mkdir(parents=True, exist_ok=True)
            fo = first_file
            while fo != NONE:
                _p, sib, doff, size, _h, _nl = struct.unpack_from("<IIQQII", files, fo)
                fname = name(files, fo, 0x20)
                f.seek(data_off + doff)
                with (path / fname).open("wb") as w:
                    left = size
                    while left:
                        chunk = f.read(min(left, 8 << 20))
                        if not chunk:
                            raise SystemExit(f"{fname}: truncated romfs")
                        w.write(chunk)
                        left -= len(chunk)
                count += 1
                total += size
                fo = sib
            co = child
            while co != NONE:
                cname = name(dirs, co, 0x18)
                walk_dir(co, path / cname)
                co = struct.unpack_from("<I", dirs, co + 4)[0]

        walk_dir(0, out)
    print(f"{count} files, {total / 2**30:.2f} GiB -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
