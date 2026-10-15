#!/usr/bin/env python3
"""Converts all_code_targets.json (static analysis of main) to the extra-roots
format of the suyu exporter (SUYU_AOT_EXTRA_ROOTS).

game_export.cpp reads <SUYU_AOT_EXTRA_ROOTS>/<module>.roots: one hexadecimal offset
per line, relative to the start of the module's .text (it adds text_vaddr). In
PLA's main, .text starts at module offset 0, so the JSON offsets ("relative to
module start") are used as-is; offsets outside .text or not 4-aligned are still
filtered out.

  python make-roots.py --json tools/ghidra/out/all_code_targets.json \
      --out <root>/roots/pla-1.1.1 [--text-size 0x...] [--kinds function,block]
"""

import argparse
import json
import pathlib
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--module", default="main")
    ap.add_argument("--text-offset", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--text-size", type=lambda s: int(s, 0), default=0,
                    help="size of .text; 0 = do not filter by size")
    ap.add_argument("--sources", default="",
                    help="list of sources to include (eh_frame,jumptable,...); empty = all")
    a = ap.parse_args()

    data = json.loads(pathlib.Path(a.json).read_text(encoding="utf-8"))
    wanted = set(filter(None, a.sources.split(",")))
    offs = set()
    skipped = 0
    for t in data["targets"]:
        if wanted and not wanted.intersection(t.get("src", [])):
            continue
        off = int(t["off"]) - a.text_offset
        if off < 0 or off & 3 or (a.text_size and off >= a.text_size):
            skipped += 1
            continue
        offs.add(off)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{a.module}.roots"
    path.write_text("".join(f"{o:x}\n" for o in sorted(offs)), encoding="ascii")
    print(f"{path}: {len(offs)} roots ({skipped} discarded)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
