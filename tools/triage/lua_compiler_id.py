#!/usr/bin/env python3
"""Compiler identification using Lua 5.3 (statically linked in main).

1. Compiles official Lua 5.3.x with a reference configuration and aligns its
   functions (link order) with those of main in the Lua range to obtain
   the name -> address map (tools/triage/out/lua_map.csv).
2. For each candidate (Lua version, clang, flags) it recompiles and compares
   function by function with masked relocations: exact matches,
   exact bytes and size-weighted mean similarity.

Usage:
    python tools/triage/lua_compiler_id.py map            # generates the map
    python tools/triage/lua_compiler_id.py matrix --set versions|clangs|best-per-clang|ablation
    python tools/triage/lua_compiler_id.py exact          # list of identical functions
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import Image, load_functions  # noqa: E402
from fnmatch_util import align, compare  # noqa: E402
from luabuild import LUA_TUS, build  # noqa: E402

OUT = Path(__file__).resolve().parent / "out"
LUA_START, LUA_END = 0x710004FD20, 0x71000791E4

# Link order observed in main: alphabetical, except lcode and llex, which go
# after lparser.
_base = [t for t in LUA_TUS if t not in ("lcode", "llex")]
_k = _base.index("lparser")
LINK_ORDER = _base[: _k + 1] + ["lcode", "llex"] + _base[_k + 1:]

MCPU = "-mcpu=cortex-a57+fp+simd+crypto+crc"
REF_FLAGS = ["--target=aarch64-none-elf", MCPU, "-O3", "-fPIC", "-mno-implicit-float",
             "-fno-common", "-fno-short-enums"]


def targets():
    return [f for f in load_functions() if LUA_START <= f[0] < LUA_END]


def make_map(lua_ver="5.3.4", clang="9.0.0", flags=None):
    flags = flags or BEST_FLAGS
    img = Image()
    T = targets()
    funcs, _ = build(lua_ver, clang, flags)
    funcs.sort(key=lambda f: LINK_ORDER.index(f.tu))
    pairs, tb = align(funcs, T, img)
    OUT.mkdir(exist_ok=True)
    rows = []
    for i, j, s in pairs:
        f = funcs[i]
        rows.append({"address": f"0x{T[j][0]:016x}", "size": T[j][1], "name": f.name,
                     "tu": f.tu + ".c", "align_score": f"{s:.3f}"})
    with open(OUT / "lua_map.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    print(f"{len(rows)} Lua functions mapped out of {len(T)} in the range; "
          f"{len(funcs) - len(rows)} compiled functions without a match (removed by the linker or modified)")
    return rows


def load_map(min_score=0.3):
    with open(OUT / "lua_map.csv", encoding="utf-8") as fh:
        return [r for r in csv.DictReader(fh) if float(r["align_score"]) >= min_score]


def score(lua_ver, clang, flags, defines=None, img=None, mp=None, detail=False):
    img = img or Image()
    mp = mp or load_map()
    funcs, outdir = build(lua_ver, clang, flags, defines)
    byname = {}
    for f in funcs:
        byname.setdefault(f.name, f)
    n = ex = exb = tot = 0
    simw = 0.0
    det = []
    for r in mp:
        f = byname.get(r["name"])
        if f is None:
            continue
        a, s = int(r["address"], 16), int(r["size"])
        e, sim = compare(f, img.read(a, s))
        n += 1
        tot += s
        ex += e
        exb += s if e else 0
        simw += sim * s
        det.append((r["name"], r["address"], s, f.size, e, round(sim, 3)))
    res = {"lua": lua_ver, "clang": clang, "flags": " ".join(flags),
           "defines": " ".join(defines) if defines else "default",
           "funcs": n, "exact": ex, "exact_pct": round(100 * ex / max(n, 1), 1),
           "exact_bytes_pct": round(100 * exb / max(tot, 1), 1),
           "similarity": round(100 * simw / max(tot, 1), 1)}
    if detail:
        res["detail"] = det
    return res


# Winning configuration (see docs/research/07-compiler-id.md)
BEST_CLANG = "9.0.0"
BEST_LUA = "5.3.4"
BEST_FLAGS = ["--target=aarch64-none-elf", MCPU, "-O3", "-fPIC", "-mno-implicit-float",
              "-fno-common", "-fno-short-enums", "-fno-strict-aliasing", "-funwind-tables", "-g"]


def _swap(flags, old, new):
    out = [f for f in flags if f != old]
    if new:
        out += new.split()
    return out


def flagset(name):
    T = "--target=aarch64-none-elf"
    common = ["-fPIC", "-mno-implicit-float", "-fno-common", "-fno-short-enums"]
    clangs = ["8.0.0", "9.0.0", "10.0.0", "11.0.0", "12.0.0", "13.0.0"]
    if name == "versions":
        return [(v, BEST_CLANG, BEST_FLAGS) for v in
                ["5.3.0", "5.3.1", "5.3.2", "5.3.3", "5.3.4", "5.3.5", "5.3.6"]]
    if name == "clangs":
        # "starting" flags (those of botw/SMO/SSBU), without the two key ones
        out = []
        for c in clangs:
            for o in ["-O2", "-O3", "-Os"]:
                out.append((BEST_LUA, c, [T, MCPU, o, *common]))
        return out
    if name == "best-per-clang":
        out = []
        for c in clangs:
            for o in ["-O2", "-O3"]:
                out.append((BEST_LUA, c, _swap(BEST_FLAGS, "-O3", o)))
        return out
    if name == "ablation":
        B = BEST_FLAGS
        return [(BEST_LUA, BEST_CLANG, f) for f in [
            B,
            _swap(B, "-funwind-tables", None),
            _swap(B, "-fno-strict-aliasing", None),
            _swap(_swap(B, "-funwind-tables", None), "-fno-strict-aliasing", None),
            _swap(B, "-funwind-tables", "-fasynchronous-unwind-tables"),
            _swap(B, "-funwind-tables", "-fexceptions"),
            _swap(B, MCPU, None),
            _swap(B, MCPU, "-mcpu=cortex-a53"),
            _swap(B, MCPU, "-mcpu=cortex-a57"),
            _swap(B, "-fPIC", None),
            _swap(B, "-fPIC", "-fno-pic"),
            _swap(B, "-O3", "-O2"),
            _swap(B, "-O3", "-Os"),
            _swap(B, "--target=aarch64-none-elf", "--target=aarch64-linux-gnu"),
            _swap(B, "-mno-implicit-float", None),
            B + ["-fstack-protector-strong"],
            B + ["-fno-omit-frame-pointer"],
            B + ["-fomit-frame-pointer"],
            B + ["-fno-slp-vectorize"],
            B + ["-fno-vectorize"],
            B + ["-ffast-math"],
            _swap(B, "-g", None),
            _swap(_swap(B, "-g", None), "-funwind-tables", None),
        ]]
    raise SystemExit(f"conjunto desconocido {name}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("map")
    m.add_argument("--lua", default="5.3.4")
    m.add_argument("--clang", default="9.0.0")
    x = sub.add_parser("matrix")
    x.add_argument("--set", default="versions")
    x.add_argument("--lua", default="5.3.4")
    x.add_argument("--json", default=None)
    sub.add_parser("exact", help="write out/lua_exact.json with the winning configuration")
    one = sub.add_parser("one")
    one.add_argument("--lua", default="5.3.6")
    one.add_argument("--clang", default="11.0.0")
    one.add_argument("--flags", default=" ".join(REF_FLAGS))
    one.add_argument("--defines", default=None)
    one.add_argument("--detail", action="store_true")
    a = ap.parse_args()
    if a.cmd == "map":
        make_map(a.lua, a.clang)
    elif a.cmd == "exact":
        r = score(BEST_LUA, BEST_CLANG, BEST_FLAGS, detail=True)
        names = [d[0] for d in r["detail"] if d[4]]
        (OUT / "lua_exact.json").write_text(json.dumps(sorted(names), indent=0))
        print(f"{len(names)} of {r['funcs']} identical functions with clang {BEST_CLANG} {' '.join(BEST_FLAGS)}")
    elif a.cmd == "one":
        d = a.defines.split() if a.defines is not None else None
        r = score(a.lua, a.clang, a.flags.split(), d, detail=a.detail)
        det = r.pop("detail", None)
        print(json.dumps(r))
        if det:
            for row in det:
                print(*row)
    else:
        img = Image()
        mp = load_map()
        res = []
        for lua, clang, flags in flagset(a.set):
            lua = a.lua if lua == "@LUA" else lua
            r = score(lua, clang, flags, img=img, mp=mp)
            print(json.dumps(r), flush=True)
            res.append(r)
        if a.json:
            OUT.mkdir(exist_ok=True)
            Path(a.json).write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
