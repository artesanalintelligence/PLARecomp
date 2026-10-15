#!/usr/bin/env python3
"""Applies JSON patch lists (pla-patchlist-v1) to a copy of the main NSO.

Used to "bake" patches (60 FPS, resolution, etc.) into the executable BEFORE the
AOT export: the suyu exporter (game_export.cpp) recompiles the ExeFS bytes that
PatchManager::PatchExeFS gives it (update + LayeredExeFS), but it does not apply the .ips/.pchtxt files,
which suyu only applies when loading the NSO into memory (PatchManager::PatchNSO, loader/nso.cpp).
A `main` already patched inside a mod folder (`load/<TID>/<mod>/exefs/main`) is
seen by both the exporter and JIT/Hybrid execution.

Accepted inputs: an NSO (compressed or not) or an exefs directory (all files are copied
and `main` is patched). The output is an uncompressed NSO (or LZ4 with --compress)
with recomputed SHA-256 hashes and the same build id.

If a patch falls in the padding between .text and .rodata (code cave), the .text segment
is grown in the header so that the AOT exporter translates it as well.

Examples:
    python tools/patch/apply_patches.py -p recomp/patches/60fps/pla-111-60fps-fixed.json \\
        --in data/exefs/main --out <root>/patched/main
    python tools/patch/apply_patches.py -p recomp/patches/60fps/pla-111-60fps-fixed.json \\
        --in data/exefs --out <root>/patched/exefs
    python tools/patch/apply_patches.py -p recomp/patches/60fps/pla-111-60fps-fixed.json \\
        --in data/exefs/main --suyu-mod "60FPS baked"      # -> %APPDATA%/suyu/load/<TID>/60FPS baked/exefs/main
    python tools/patch/apply_patches.py -p <lista.json> --in data/exefs/main --check   # verify only
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import nsoimg  # noqa: E402


def load_list(path: Path) -> dict:
    doc = json.loads(path.read_text(encoding="utf-8"))
    if doc.get("format") != "pla-patchlist-v1":
        raise SystemExit(f"{path}: unknown format {doc.get('format')!r}")
    for p in doc["patches"]:
        p["_off"] = int(p["offset"], 16)
        p["_new"] = bytes.fromhex(p["patched"])
        p["_old"] = bytes.fromhex(p["original"])
        if len(p["_old"]) != len(p["_new"]):
            raise SystemExit(f"{path}: {p['offset']}: original and patched have different sizes")
    return doc


def apply(nso: nsoimg.Nso, docs: list[tuple[Path, dict]], force: bool, check_only: bool,
          extend_text: bool) -> tuple[int, int]:
    applied = already = 0
    gap_lo, gap_hi = nso.text_gap()
    need_text_end = 0
    # Detect overlaps between different lists.
    owner: dict[int, str] = {}
    for path, doc in docs:
        bid = doc.get("build_id", "").upper()
        if bid and not nso.build_id.startswith(bid):
            raise SystemExit(f"{path}: build id {bid} != {nso.build_id_short} of the NSO")
        if doc.get("module", "main") != "main":
            raise SystemExit(f"{path}: only main patches are supported")
        for p in doc["patches"]:
            off, new, old = p["_off"], p["_new"], p["_old"]
            for a in range(off, off + len(new)):
                if a in owner and owner[a] != path.name:
                    raise SystemExit(f"main+0x{a:x}: touched by both {owner[a]} and {path.name}")
                owner[a] = path.name
            in_gap = gap_lo <= off < gap_hi
            seg = nso.seg_of(off, len(new))
            if seg is None and not in_gap:
                raise SystemExit(f"{path.name}: main+0x{off:x} is not in text/rodata/data or the cave "
                                 "(patches to .bss cannot be baked into the NSO)")
            if in_gap:
                if off + len(new) > gap_hi:
                    raise SystemExit(f"main+0x{off:x}: the cave overlaps .rodata")
                need_text_end = max(need_text_end, off + len(new))
            cur = nso.read(off, len(new))
            if cur == new:
                already += 1
                continue
            if cur != old and not force:
                raise SystemExit(f"{path.name}: main+0x{off:x}: current bytes {cur.hex()} do not match the "
                                 f"original {old.hex()} (use --force to apply anyway)")
            if not check_only:
                if in_gap and off + len(new) > len(nso.image):
                    nso.image.extend(bytes(off + len(new) - len(nso.image)))
                nso.write(off, new)
            applied += 1
    if need_text_end and not check_only:
        if extend_text:
            new_end = (need_text_end + 0xFF) & ~0xFF
            new_end = min(new_end, gap_hi)
            old_size = nso.segs[0].size
            nso.extend_text(new_end)
            print(f"  .text: 0x{old_size:x} -> 0x{nso.segs[0].size:x} bytes (includes the cave up to 0x{need_text_end:x})")
        else:
            print("  warning: there are patches in the cave but --no-extend-text leaves .text unchanged: the AOT export will not "
                  "translate them (in Hybrid they will go to the JIT) and suyu will NOT load them from the NSO")
    return applied, already


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--patches", action="append", required=True, help="JSON list (can be repeated)")
    ap.add_argument("--in", dest="inp", required=True, help="main NSO or exefs directory")
    ap.add_argument("--out", help="output NSO or directory")
    ap.add_argument("--suyu-mod", help="mod name: writes %%APPDATA%%/suyu/load/<TID>/<mod>/exefs/main")
    ap.add_argument("--title-id", default="01001F5010DFA000")
    ap.add_argument("--compress", action="store_true", help="compress segments with LZ4")
    ap.add_argument("--force", action="store_true", help="apply even if the original bytes do not match")
    ap.add_argument("--check", action="store_true", help="verify only, do not write")
    ap.add_argument("--no-extend-text", action="store_true", help="do not grow .text to include the cave")
    args = ap.parse_args()

    docs = [(Path(p), load_list(Path(p))) for p in args.patches]
    src = Path(args.inp)
    src_main = src / "main" if src.is_dir() else src
    nso = nsoimg.load(src_main)
    print(f"{src_main}: build id {nso.build_id_short}, .text 0x{nso.segs[0].size:x}")
    applied, already = apply(nso, docs, args.force, args.check, not args.no_extend_text)
    print(f"  {applied} patches applied, {already} were already applied")
    if args.check:
        return 0
    if not args.out and not args.suyu_mod:
        raise SystemExit("--out or --suyu-mod is missing")
    blob = nsoimg.build(nso, compress=args.compress)

    targets: list[Path] = []
    if args.out:
        out = Path(args.out)
        if src.is_dir():
            out.mkdir(parents=True, exist_ok=True)
            for f in src.iterdir():
                if f.is_file() and f.name != "main":
                    shutil.copy2(f, out / f.name)
            targets.append(out / "main")
        else:
            out.parent.mkdir(parents=True, exist_ok=True)
            targets.append(out)
    if args.suyu_mod:
        appdata = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
        targets.append(appdata / "suyu" / "load" / args.title_id / args.suyu_mod / "exefs" / "main")
    for t in targets:
        t.parent.mkdir(parents=True, exist_ok=True)
        t.write_bytes(blob)
        print(f"  -> {t} ({len(blob):,} bytes)")
    # Check: reread and verify that the patches are present.
    chk = nsoimg.load(targets[0])
    for _, doc in docs:
        for p in doc["patches"]:
            if chk.read(p["_off"], len(p["_new"])) != p["_new"]:
                raise SystemExit(f"verification failed at main+0x{p['_off']:x}")
    print("  verified: the output NSO contains all the patches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
