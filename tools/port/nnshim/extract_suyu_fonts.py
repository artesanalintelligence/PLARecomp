#!/usr/bin/env python3
"""Extracts the free replacement fonts shipped with suyu (src/core/file_sys/system_archive/data/
font_*.cpp, TrueType byte arrays, GPL) into a folder for the nnshim's "font.dir".

Writes one .ttf for each of the 7 system fonts, with the names the shim expects
(the same as the firmware .bfttf files, with a .ttf extension); nintendo_ext and nintendo_ext2 are the
same one, as in the HLE. If the user has the firmware, the original .bfttf files can go there instead
(the shim prefers the .bfttf when both exist).

Usage:
  python tools/port/nnshim/extract_suyu_fonts.py [--suyu <root>/mk8-recomp/third_party/suyu] [--out <root>/fonts]
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

SLOTS = [
    ("font_standard.cpp", "nintendo_udsg-r_std_003"),
    ("font_chinese_simplified.cpp", "nintendo_udsg-r_org_zh-cn_003"),
    ("font_extended_chinese_simplified.cpp", "nintendo_udsg-r_ext_zh-cn_003"),
    ("font_chinese_traditional.cpp", "nintendo_udjxh-db_zh-tw_003"),
    ("font_korean.cpp", "nintendo_udsg-r_ko_003"),
    ("font_nintendo_extended.cpp", "nintendo_ext_003"),
    ("font_nintendo_extended.cpp", "nintendo_ext2_003"),
]


def parse_array(path: Path) -> bytes:
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"std::array<unsigned char,\s*(\d+)>\s*\w+\s*\{\{(.*?)\}\}", text, re.S)
    if not m:
        raise SystemExit(f"{path}: array not found")
    size = int(m.group(1))
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if len(data) != size:
        raise SystemExit(f"{path}: {len(data)} bytes, expected {size}")
    return data


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--suyu", type=Path, default=Path("D:/PLARecomp/mk8-recomp/third_party/suyu"))
    ap.add_argument("--out", type=Path, default=Path("D:/PLARecomp/fonts"))
    args = ap.parse_args(argv)
    src = args.suyu / "src" / "core" / "file_sys" / "system_archive" / "data"
    args.out.mkdir(parents=True, exist_ok=True)
    cache: dict[str, bytes] = {}
    for cpp, name in SLOTS:
        if cpp not in cache:
            cache[cpp] = parse_array(src / cpp)
        data = cache[cpp]
        if data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
            print(f"warning: {cpp} does not start like a TrueType/OpenType font")
        (args.out / f"{name}.ttf").write_bytes(data)
        print(f"{name}.ttf  {len(data)} bytes  (from {cpp})")
    (args.out / "README.txt").write_text(
        "Free replacement fonts from suyu (GPL), extracted with tools/port/nnshim/extract_suyu_fonts.py.\n"
        "For the nnshim: \"font\": {\"dir\": \"<this folder>\"}. They can be replaced by the .bfttf files of\n"
        "your own firmware (same name, .bfttf extension).\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
