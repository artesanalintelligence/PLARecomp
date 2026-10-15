#!/usr/bin/env python3
"""Compares the COFF objects of two builds (official VS build vs portable), ignoring only the
COFF header timestamp. Used to check that the portable toolchain emits exactly the same code
for the units of the emitted C.

    python compare_objects.py <official_obj_dir> <portable_obj_dir>

Objects are paired by source name (CMake: src/x.c.obj; portable: src_x.c.obj).
"""
import sys
from pathlib import Path

BIGOBJ_CLSID = bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8")


def normalized(p: Path) -> bytes:
    b = bytearray(p.read_bytes())
    if len(b) >= 56 and b[0:2] == b"\x00\x00" and b[2:4] == b"\xff\xff" and b[12:28] == BIGOBJ_CLSID:
        b[8:12] = b"\0\0\0\0"   # ANON_OBJECT_HEADER_BIGOBJ.TimeDateStamp
    else:
        b[4:8] = b"\0\0\0\0"    # IMAGE_FILE_HEADER.TimeDateStamp
    return bytes(b)


def key(p: Path) -> str:
    n = p.name
    return n[4:] if n.startswith("src_") else n


def main() -> int:
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    oa = {key(p): p for p in a.rglob("*.obj")}
    ob = {key(p): p for p in b.rglob("*.obj")}
    same = diff = 0
    for k in sorted(set(oa) & set(ob)):
        if normalized(oa[k]) == normalized(ob[k]):
            same += 1
        else:
            diff += 1
            print(f"DIFFERENT {k}: {oa[k].stat().st_size} / {ob[k].stat().st_size} bytes")
    unpaired = sorted(set(oa) ^ set(ob))
    print(f"identical {same}, different {diff}, unpaired {len(unpaired)} {unpaired[:6]}")
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
