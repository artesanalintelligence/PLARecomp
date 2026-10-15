#!/usr/bin/env python3
"""Gathers all known code addresses ("discovery roots") for the
static recompiler and exports the function pointers.

Sources:
  eh_frame      start of each FDE (function or function fragment)
  symbol        defined FUNC symbols (.dynsym and, if present, .symtab)
  entry         ELF entry point, DT_INIT and DT_FINI
  init_array    entries of DT_INIT_ARRAY / DT_FINI_ARRAY / DT_PREINIT_ARRAY
  vtable        vtable entries recovered by rtti.py
  reloc         other data slots (R_AARCH64_RELATIVE/ABS64/GLOB_DAT or
                raw pointers) that point to executable code
  jumptable     switch jump table targets (blocks, not functions)
  landing_pad   exception landing pads (LSDA in .gcc_except_table), blocks
  plt           PLT stubs (also carry "import": imported symbol)

Outputs:
  funcptrs.json          each data slot that contains a pointer to code
                         (slot, target, origin and relocation type)
  all_code_targets.json  simple format for the recompiler:
      {"module": "main", "base": "0x7100000000", "link_base": "0x0",
       "image_size": N, "offsets_are": "relative to module start",
       "counts": {...},
       "targets": [{"off": 4660, "kind": "function", "src": ["eh_frame", "vtable"],
                    "size": 32}, ...]}
    `off` is relative to the module start (va - link_base). kind is
    "function" (probable function start) or "block" (internal target, e.g.
    of a jump table). `size` only appears if it comes from eh_frame or a symbol.

Usage:
    python tools/analysis/code_targets.py data/main.elf --out-dir tools/ghidra/out
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfimage import ElfImage, add_common_args  # noqa: E402
import eh_frame  # noqa: E402
import jumptables  # noqa: E402
import rtti  # noqa: E402

FUNCTION_SOURCES = ("eh_frame", "symbol", "entry", "init_array", "vtable", "reloc")


def dynamic_tags(img: ElfImage) -> dict[str, int]:
    out = {}
    for seg in img.elf.iter_segments():
        if seg["p_type"] == "PT_DYNAMIC":
            try:
                for t in seg.iter_tags():
                    out.setdefault(t.entry.d_tag, t.entry.d_val)
            except Exception:
                pass
    return out


def array_slots(img: ElfImage, tags: dict, arr: str, sz: str) -> list[int]:
    if arr not in tags or sz not in tags:
        return []
    return list(range(tags[arr], tags[arr] + tags[sz], 8))


def collect(img: ElfImage, rtti_res: dict | None = None, jt_res: list | None = None,
            fdes=None, verbose: bool = True):
    def log(msg):
        if verbose:
            print(msg, file=sys.stderr)

    if fdes is None:
        try:
            fdes, _ = eh_frame.collect(img)
        except eh_frame.EhError as e:
            log(f"[warning] eh_frame: {e}")
            fdes = []
    log(f"eh_frame: {len(fdes)} FDE")
    if rtti_res is None:
        rtti_res = rtti.analyze(img)
    log(f"rtti: {rtti_res['stats']['classes']} classes")
    if jt_res is None:
        jt_res, _ = jumptables.find_jumptables(img, [(f.start, f.size) for f in fdes])
    log(f"jump tables: {len(jt_res)}")

    targets: dict[int, dict] = {}

    def add(va: int, src: str, size: int | None = None, kind: str = "function"):
        if not img.is_exec(va):
            return
        t = targets.setdefault(va, {"src": [], "kind": kind})
        if src not in t["src"]:
            t["src"].append(src)
        if kind == "function":
            t["kind"] = "function"
        if size and "size" not in t:
            t["size"] = size

    for f in fdes:
        add(f.start, "eh_frame", f.size)
    for s in img.defined_symbols("STT_FUNC"):
        add(s.va, "symbol", s.size or None)

    tags = dynamic_tags(img)
    if img.elf["e_entry"]:
        add(img.elf["e_entry"], "entry")
    for t in ("DT_INIT", "DT_FINI"):
        if t in tags:
            add(tags[t], "entry")
    init_slots = set()
    for arr, sz in (("DT_INIT_ARRAY", "DT_INIT_ARRAYSZ"), ("DT_FINI_ARRAY", "DT_FINI_ARRAYSZ"),
                    ("DT_PREINIT_ARRAY", "DT_PREINIT_ARRAYSZ")):
        init_slots.update(array_slots(img, tags, arr, sz))

    # Vtable slots (address point + i*8) according to rtti.json
    vt_slots: dict[int, str] = {}
    for cname, c in rtti_res["classes"].items():
        subs = list(c.get("vtables", []))
        for cv in c.get("construction_vtables", []):
            subs.extend(cv.get("vtables", []))
        for sv in subs:
            ap = img.to_va(int(sv["address_point"], 16))
            for i in range(len(sv["entries"])):
                vt_slots[ap + 8 * i] = cname

    for av in rtti_res.get("vtables_without_rtti", []):
        for sv in av["vtables"]:
            ap = img.to_va(int(sv["address_point"], 16))
            for i in range(len(sv["entries"])):
                vt_slots.setdefault(ap + 8 * i, av["name"])

    funcptrs = []
    for va, p in img.pointer_slots():
        if p.target is None or not img.is_exec(p.target):
            continue
        if va in vt_slots:
            origin = "vtable"
        elif va in init_slots:
            origin = "init_array"
        else:
            origin = "reloc"
        funcptrs.append({"slot": va, "target": p.target, "origin": origin, "reloc": p.kind,
                         **({"class": vt_slots[va]} if origin == "vtable" else {})})
        add(p.target, origin)

    for t in jt_res:
        for x in t["targets"]:
            add(x, "jumptable", kind="block")

    plt = img.plt_stubs()
    for va, name in plt.items():
        add(va, "plt", 16)
        targets[va]["import"] = name
    log(f"stubs PLT: {len(plt)}")

    pads, pad_err = eh_frame.landing_pads(img, fdes)
    for x in pads:
        add(x, "landing_pad", kind="block")
    log(f"landing pads (LSDA): {len(pads)}" + (f", {pad_err} outside their function or unreadable" if pad_err else ""))

    return targets, funcptrs, jt_res, rtti_res, fdes


def write_outputs(img: ElfImage, targets, funcptrs, out_dir: Path, module: str = "main"):
    out_dir.mkdir(parents=True, exist_ok=True)
    rel = lambda va: va - img.link_base  # noqa: E731
    counts = defaultdict(int)
    for t in targets.values():
        for s in t["src"]:
            counts[s] += 1
    counts["total"] = len(targets)
    counts["functions"] = sum(1 for t in targets.values() if t["kind"] == "function")
    counts["blocks_only"] = counts["total"] - counts["functions"]
    image_size = max(s.end for s in img.segments) - img.link_base
    doc = {
        "module": module,
        "elf": str(img.path),
        "base": f"0x{img.base:x}",
        "link_base": f"0x{img.link_base:x}",
        "image_size": image_size,
        "offsets_are": "relative to module start (va - link_base); absolute = base + off",
        "counts": dict(counts),
        "targets": [{"off": rel(va), **t} for va, t in sorted(targets.items())],
    }
    (out_dir / "all_code_targets.json").write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
    fp = {
        "module": module, "base": f"0x{img.base:x}", "link_base": f"0x{img.link_base:x}",
        "offsets_are": "relative to module start",
        "counts": {o: sum(1 for f in funcptrs if f["origin"] == o) for o in ("vtable", "init_array", "reloc")},
        "funcptrs": [{**f, "slot": rel(f["slot"]), "target": rel(f["target"])} for f in funcptrs],
    }
    (out_dir / "funcptrs.json").write_text(json.dumps(fp, separators=(",", ":")), encoding="utf-8")
    return doc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--out-dir", default="tools/ghidra/out")
    ap.add_argument("--rtti", help="already computed rtti.json (otherwise it is computed and saved in out-dir)")
    ap.add_argument("--module", default="main")
    args = ap.parse_args(argv)
    img = ElfImage(args.elf, args.base)
    out = Path(args.out_dir)
    rtti_res = json.loads(Path(args.rtti).read_text(encoding="utf-8")) if args.rtti else None
    targets, funcptrs, jt, rtti_res, fdes = collect(img, rtti_res)
    out.mkdir(parents=True, exist_ok=True)
    if not args.rtti:
        (out / "rtti.json").write_text(json.dumps(rtti_res, indent=1, ensure_ascii=False), encoding="utf-8")
    A = img.to_abs
    (out / "jumptables.json").write_text(json.dumps({
        "base": f"0x{img.base:x}",
        "tables": [{**t, "br": f"0x{A(t['br']):x}", "table": f"0x{A(t['table']):x}",
                    "anchor": None if t["anchor"] is None else f"0x{A(t['anchor']):x}",
                    "targets": [f"0x{A(x):x}" for x in t["targets"]]} for t in jt]}, indent=1), encoding="utf-8")
    doc = write_outputs(img, targets, funcptrs, out, args.module)
    c = doc["counts"]
    print("all_code_targets: " + ", ".join(f"{k}={v}" for k, v in sorted(c.items())), file=sys.stderr)
    print(f"funcptrs: {len(funcptrs)} data slots point to code -> {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
