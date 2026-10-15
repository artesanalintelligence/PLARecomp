#!/usr/bin/env python3
"""Merges the function sources into data/main_functions.csv (viking format).

Function start sources (from most to least reliable for the size):
  FUNC symbols (.dynsym/.symtab) > .eh_frame FDEs > Ghidra functions >
  vtable entries and code pointers from data (size by gap).

Names (from highest to lowest priority):
  1. name already present in the existing CSV, if not provisional (human work);
  2. known symbol (mangled, as viking expects it);
  3. non-generic Ghidra name (not FUN_/thunk_FUN_/LAB_...);
  4. provisional RTTI name: `Class::vfN` (primary vtable) or
     `Class::thunkO_vfN` (secondary sub-vtable with offset_to_top -O; usually
     non-virtual or virtual thunks); for vtables without RTTI,
     `vtable_<addr>::vfN`;
  5. `FUN_<address>` (or empty with --unnamed empty).

Formato (open-ead/nx-decomp-tools, viking):
  Address,Quality,Size,Name
  0x0000007100001234,U,000052,_ZN4game4Base6updateEf
  * viking reads the CSV without quotes: names cannot contain commas
    (in provisional names they are replaced by ';').
  * Functions cannot overlap and names cannot repeat.
  * Quality is kept from the existing CSV (O, m, M, W, L, U); new ones get U.

It also writes <out-dir>/main_functions_named.tsv with the demangled names and the sources.

Usage:
    python tools/analysis/merge_functions.py data/main.elf \
        [--ghidra tools/ghidra/out/ghidra_functions.csv] [--rtti tools/ghidra/out/rtti.json] \
        [-o data/main_functions.csv]
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfimage import ElfImage, add_common_args  # noqa: E402
from demangle import demangle_many  # noqa: E402
import eh_frame  # noqa: E402
import rtti as rtti_mod  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
HEADER = ["Address", "Quality", "Size", "Name"]
GENERIC_GHIDRA = re.compile(r"^(FUN|thunk_FUN|LAB|SUB|DAT|switchD|caseD|UNK|EXT_FUN)_[0-9a-fA-F]+")
PROVISIONAL = re.compile(r"^(FUN_[0-9a-fA-F]+|.*::(thunk\d+_)?vf\d+)$")
MAPPING_SYM = re.compile(r"^\$[xd](\..*)?$")
NOP = 0xD503201F


def csv_safe(name: str) -> str:
    return name.replace(", ", ";").replace(",", ";").replace("\n", " ")


def read_viking_csv(path: Path) -> dict[int, tuple[str, int, str]]:
    out = {}
    with open(path, newline="", encoding="utf-8") as f:
        r = csv.reader(f)
        head = next(r, None)
        if head != HEADER:
            raise SystemExit(f"{path}: cabecera inesperada {head}")
        for row in r:
            if len(row) != 4:
                continue
            out[int(row[0], 16)] = (row[1], int(row[2]), row[3])
    return out


def rtti_names(img: ElfImage, rtti_res: dict) -> dict[int, str]:
    """Provisional RTTI names; base classes are processed first."""
    classes = rtti_res["classes"]
    by_ti = {c["typeinfo"]: n for n, c in classes.items()}
    order, seen = [], set()

    def visit(n):
        if n in seen or n not in classes:
            return
        seen.add(n)
        for b in classes[n]["bases"]:
            visit(by_ti.get(b["typeinfo"], b["name"]))
        order.append(n)

    for n in classes:
        visit(n)
    names: dict[int, str] = {}
    for n in order:
        c = classes[n]
        cname = csv_safe(n)
        for sv in c.get("vtables", []):
            ott = sv["offset_to_top"]
            for i, e in enumerate(sv["entries"]):
                if not e or not e.startswith("0x"):
                    continue
                va = img.to_va(int(e, 16))
                if va in names or not img.is_exec(va):
                    continue
                names[va] = f"{cname}::vf{i}" if ott == 0 else f"{cname}::thunk{-ott}_vf{i}"
    # Vtables without RTTI (-fno-rtti): vtable_<addr>::vfN
    for av in rtti_res.get("vtables_without_rtti", []):
        for sv in av["vtables"]:
            ott = sv["offset_to_top"]
            for i, e in enumerate(sv["entries"]):
                if not e or not e.startswith("0x"):
                    continue
                va = img.to_va(int(e, 16))
                if va in names or not img.is_exec(va):
                    continue
                names[va] = f"{av['name']}::vf{i}" if ott == 0 else f"{av['name']}::thunk{-ott}_vf{i}"
    return names


def trim_padding(img: ElfImage, start: int, size: int) -> int:
    end = start + size
    while end - 4 > start:
        try:
            w = int.from_bytes(img.read(end - 4, 4), "little")
        except IndexError:
            break
        if w not in (0, NOP):
            break
        end -= 4
    return end - start


def merge(img: ElfImage, rtti_res: dict | None, ghidra: dict | None, existing: dict | None,
          unnamed: str = "fun", extra_starts=None):
    starts: dict[int, dict] = defaultdict(lambda: {"src": set(), "size": {}, "names": {}})

    def add(va, src, size=None, name=None):
        if not img.is_exec(va):
            return
        e = starts[va]
        e["src"].add(src)
        if size:
            e["size"].setdefault(src, size)
        if name:
            e["names"].setdefault(src, name)

    # Symbols
    for s in img.defined_symbols("STT_FUNC"):
        if MAPPING_SYM.match(s.name):
            continue
        prev = starts[s.va]["names"].get("symbol") if s.va in starts else None
        # prefer mangled and global symbols
        if prev is None or (s.name.startswith("_Z") and not prev.startswith("_Z")):
            starts[s.va]["names"]["symbol"] = s.name
        add(s.va, "symbol", s.size or None)
    # eh_frame
    try:
        fdes, _ = eh_frame.collect(img)
    except eh_frame.EhError as e:
        print(f"[warning] eh_frame: {e}", file=sys.stderr)
        fdes = []
    for f in fdes:
        add(f.start, "eh_frame", f.size)
    # Ghidra
    for a, (_, size, name) in (ghidra or {}).items():
        va = img.to_va(a)
        add(va, "ghidra", size or None,
            name if name and not GENERIC_GHIDRA.match(name) and not PROVISIONAL.match(name) else None)
    # RTTI
    rnames = rtti_names(img, rtti_res) if rtti_res else {}
    for va, n in rnames.items():
        add(va, "vtable", name=n)
    # PLT stubs: named after the imported symbol
    for va, name in img.plt_stubs().items():
        add(va, "plt", 16, name)
    for va, src in (extra_starts or []):
        add(va, src)
    # Existentes
    for a, (q, size, name) in (existing or {}).items():
        add(img.to_va(a), "existing", size or None, name or None)

    # Sizes
    order = sorted(starts)
    funcs = []
    stats = defaultdict(int)
    for i, va in enumerate(order):
        e = starts[va]
        nxt = order[i + 1] if i + 1 < len(order) else None
        rng = next((r for r in img.code_ranges if r[0] <= va < r[1]), None)
        limit = min(nxt if nxt is not None else rng[1], rng[1])
        size = None
        for src in ("symbol", "eh_frame", "existing", "plt", "ghidra"):
            if src in e["size"]:
                size = e["size"][src]
                stats[f"size_{src}"] += 1
                break
        if size is None:
            size = trim_padding(img, va, limit - va)
            stats["size_gap"] += 1
        if va + size > limit:
            stats["truncated_overlap"] += 1
            size = limit - va
        if size <= 0:
            stats["dropped_empty"] += 1
            continue
        funcs.append((va, size, e))

    # Names
    used = set()
    rows = []
    for va, size, e in funcs:
        names = e["names"]
        ex = names.get("existing")
        name = None
        for cand in (ex if ex and not PROVISIONAL.match(ex) else None,
                     names.get("symbol"), names.get("plt"), names.get("ghidra"), names.get("vtable")):
            if cand and csv_safe(cand) not in used:
                name = csv_safe(cand)
                break
        if name is None:
            name = f"FUN_{img.to_abs(va):x}" if unnamed == "fun" else ""
        if name:
            used.add(name)
        a = img.to_abs(va)
        q = existing[a][0] if existing and a in existing else "U"
        rows.append((a, q, size, name, sorted(e["src"])))
    return rows, dict(stats)


def write_csv(rows, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write(",".join(HEADER) + "\n")
        for a, q, size, name, _ in rows:
            f.write(f"0x{a:016x},{q},{size:06d},{name}\n")


def write_named(rows, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    dem = demangle_many([r[3] for r in rows if r[3].startswith("_Z")])
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("address\tsize\tname\tdemangled\tsources\n")
        for a, q, size, name, src in rows:
            f.write(f"0x{a:016x}\t{size}\t{name}\t{dem.get(name) or ''}\t{','.join(src)}\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--ghidra", help="CSV exported by Ghidra (default tools/ghidra/out/ghidra_functions.csv if it exists)")
    ap.add_argument("--rtti", help="rtti.json (computed if not given)")
    ap.add_argument("--existing", help="existing CSV whose Quality/names are kept (default: the output one)")
    ap.add_argument("-o", "--output", default=str(REPO / "data" / "main_functions.csv"))
    ap.add_argument("--out-dir", default=str(REPO / "tools" / "ghidra" / "out"))
    ap.add_argument("--unnamed", choices=["fun", "empty"], default="fun",
                    help="name for unnamed functions: FUN_<addr> (default) or empty")
    ap.add_argument("--with-funcptrs", action="store_true",
                    help="also add as starts the targets of code pointers found in data")
    args = ap.parse_args(argv)

    img = ElfImage(args.elf, args.base)
    ghidra_path = Path(args.ghidra) if args.ghidra else REPO / "tools" / "ghidra" / "out" / "ghidra_functions.csv"
    ghidra = read_viking_csv(ghidra_path) if ghidra_path.exists() else None
    if args.ghidra and ghidra is None:
        raise SystemExit(f"{ghidra_path} does not exist")
    rtti_res = (json.loads(Path(args.rtti).read_text(encoding="utf-8")) if args.rtti
                else rtti_mod.analyze(img))
    existing_path = Path(args.existing) if args.existing else Path(args.output)
    existing = read_viking_csv(existing_path) if existing_path.exists() else None
    extra = []
    if args.with_funcptrs:
        for va, p in img.pointer_slots():
            if p.target is not None and img.is_exec(p.target):
                extra.append((p.target, "funcptr"))
    rows, stats = merge(img, rtti_res, ghidra, existing, args.unnamed, extra)
    write_csv(rows, Path(args.output))
    write_named(rows, Path(args.out_dir) / "main_functions_named.tsv")
    named = sum(1 for r in rows if r[3] and not r[3].startswith("FUN_"))
    print(f"{len(rows)} functions ({named} with non-generic name) -> {args.output}", file=sys.stderr)
    print("  " + ", ".join(f"{k}={v}" for k, v in sorted(stats.items())), file=sys.stderr)
    src_count = defaultdict(int)
    for r in rows:
        for s in r[4]:
            src_count[s] += 1
    print("  fuentes: " + ", ".join(f"{k}={v}" for k, v in sorted(src_count.items())), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
