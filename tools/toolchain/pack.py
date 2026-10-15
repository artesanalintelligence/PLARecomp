#!/usr/bin/env python3
"""Packs (and unpacks) the portable toolchain as a .tar.xz with the x86 BCJ filter.

The BCJ filter turns the relative addresses of x86 call/jmp instructions into absolute ones
before LZMA2, which compresses clang-cl.exe and lld-link.exe much better. Only the Python
standard library is used (lzma + tarfile), so the launcher can unpack it without 7-Zip or
anything installed.

    python pack.py create <root>\\toolchain-portable\\recomp-toolchain out.tar.xz
    python pack.py extract out.tar.xz D:\\dest
"""
import lzma
import sys
import tarfile
import time
from pathlib import Path

FILTERS = [
    {"id": lzma.FILTER_X86},
    {"id": lzma.FILTER_LZMA2, "preset": 9 | lzma.PRESET_EXTREME, "dict_size": 192 * 2 ** 20},
]


def create(folder: Path, out: Path) -> None:
    t = time.time()
    with lzma.open(out, "wb", format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, filters=FILTERS) as f:
        with tarfile.open(fileobj=f, mode="w|", format=tarfile.PAX_FORMAT) as tar:
            tar.add(folder, arcname=folder.name)
    print(f"{out}: {out.stat().st_size / 2 ** 20:.1f} MB in {time.time() - t:.0f} s")


def extract(archive: Path, dest: Path) -> None:
    t = time.time()
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:xz") as tar:
        members = tar.getmembers()
        for m in members:  # nothing outside the destination folder, no links
            p = (dest / m.name).resolve()
            if not str(p).startswith(str(dest.resolve())) or m.issym() or m.islnk():
                raise SystemExit(f"member not allowed in the archive: {m.name}")
        if hasattr(tarfile, "data_filter"):
            tar.extractall(dest, members=members, filter="data")
        else:
            tar.extractall(dest, members=members)
    print(f"extracted into {dest} in {time.time() - t:.1f} s")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("create", "extract"):
        raise SystemExit(__doc__)
    (create if sys.argv[1] == "create" else extract)(Path(sys.argv[2]), Path(sys.argv[3]))
