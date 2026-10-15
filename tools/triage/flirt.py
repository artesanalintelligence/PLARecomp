#!/usr/bin/env python3
"""FLIRT-style signatures: compiles the source of an open library with the
identified compiler (clang 9.0.0 + flags from docs/research/07-compiler-id.md)
and searches ALL of main for identical functions with the relocations masked.

Output: tools/triage/out/flirt_<lib>.csv (address,size,name,lib,obj)

Usage:
    python tools/triage/flirt.py zlib
    python tools/triage/flirt.py lua        # Lua 5.3.4 (cross-check)
"""
from __future__ import annotations

import csv
import hashlib
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import CACHE, RELOC_MASK, Image, load_functions, words  # noqa: E402
from lua_compiler_id import BEST_CLANG, BEST_FLAGS  # noqa: E402
from luabuild import CLANGS, MUSL  # noqa: E402
from objfile import read_object  # noqa: E402

OUT = Path(__file__).resolve().parent / "out"

LIBS = {
    "zlib": {
        "src": CACHE / "src" / "zlib-1.2.11",
        "files": ["adler32", "compress", "crc32", "deflate", "infback", "inffast",
                  "inflate", "inftrees", "trees", "uncompr", "zutil"],
        "defines": ["-DHAVE_UNISTD_H"],
        "label": "lib-open:zlib",
    },
    "lua": {
        "src": CACHE / "src" / "lua-5.3.4" / "src",
        "files": None,   # all the .c files except lua.c and luac.c
        "defines": ["-DLUA_COMPAT_5_2", "-DLUA_USE_POSIX"],
        "label": "lib-open:lua",
    },
}


def build(lib: str, flags=BEST_FLAGS, clang=BEST_CLANG):
    cfg = LIBS[lib]
    src = cfg["src"]
    files = cfg["files"] or sorted(p.stem for p in src.glob("*.c") if p.stem not in ("lua", "luac"))
    key = hashlib.sha1(repr((lib, clang, flags, cfg["defines"])).encode()).hexdigest()[:10]
    out = CACHE / "build" / f"flirt-{lib}-{key}"
    out.mkdir(parents=True, exist_ok=True)
    cc = CLANGS / f"clang-{clang}" / "bin" / "clang.exe"
    resinc = next((CLANGS / f"clang-{clang}" / "lib" / "clang").iterdir()) / "include"

    def one(stem):
        o = out / f"{stem}.o"
        if not o.exists():
            cmd = [str(cc), "-x", "c", *flags, *cfg["defines"], "-ffunction-sections",
                   "-fdata-sections", "-c", "-nostdinc", "-isystem", str(resinc),
                   "-isystem", str(MUSL), "-I", str(src), str(src / f"{stem}.c"), "-o", str(o)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                print(f"warning: {stem}.c does not compile: {r.stderr[:300]}", file=sys.stderr)
                return []
        return read_object(o)

    with ThreadPoolExecutor(4) as ex:
        return [f for fl in ex.map(one, files) for f in fl]


def match(lib: str, min_size: int = 16):
    img = Image()
    funcs = load_functions()
    by_size = defaultdict(list)
    for a, s, _ in funcs:
        by_size[s].append(a)
    objs = build(lib)
    hits = []
    for f in objs:
        if f.size < min_size:
            continue
        mask = {off // 4: RELOC_MASK.get(t, 0) for off, (t, _s) in f.relocs.items()}
        mine = f.masked()
        found = []
        for a in by_size.get(f.size, []):
            tw = words(img.read(a, f.size))
            if all((tw[k] & ~mask.get(k, 0)) == mine[k] for k in range(len(mine))):
                found.append(a)
        # a signature that appears in many places identifies nothing
        if 1 <= len(found) <= 2:
            for a in found:
                hits.append((a, f.size, f.name, lib, f.obj))
    OUT.mkdir(exist_ok=True)
    with open(OUT / f"flirt_{lib}.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["address", "size", "name", "lib", "obj"])
        for a, s, n, l, o in sorted(hits):
            w.writerow([f"0x{a:016x}", s, n, l, o])
    print(f"{lib}: {len(hits)} exact matches out of {sum(1 for f in objs if f.size >= min_size)} "
          f"compiled functions (>= {min_size} bytes)")
    return hits


if __name__ == "__main__":
    for lib in sys.argv[1:] or ["zlib"]:
        match(lib)
