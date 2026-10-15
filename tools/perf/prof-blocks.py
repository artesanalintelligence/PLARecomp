#!/usr/bin/env python3
"""Cross-references the raw hostprof dump (HOSTPROF_RAW) with the linker maps of the recompiled
DLLs (lld-link /MAP) and with the game's function list.

Answers: which game functions (by recompiled block -> guest function), which runtime
helpers (memory, guard, dispatch) and which parts of the host consume the CPU of the CPUCore threads.

  python tools/perf/prof-blocks.py <raw.txt> [--maps D:/PLARecomp/perf/sym] [--top 40]

Maps are looked up as <maps>/<module>/<dll>.map: main -> recompiled_image.map, etc.
"""
import argparse
import bisect
import collections
import csv
import os
import re
import sys

MAP_LINE = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(\S+)", re.I)
DLL_TO_DIR = {"recompiled_image": "main", "recompiled_sdk": "sdk", "recompiled_subsdk0": "subsdk0",
              "recompiled_subsdk1": "subsdk1", "recompiled_rtld": "rtld"}
GUEST_BASE = 0x7100000000


def load_map(path):
    syms = {}
    base = 0x180000000
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
                continue
            m = MAP_LINE.match(line)
            if m:
                va = int(m.group(2), 16)
                if va >= base:
                    syms.setdefault(va - base, m.group(1))
    keys = sorted(syms)
    return keys, [syms[k] for k in keys]


def load_functions(path):
    starts, names = [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            a = int(row[0], 16) - GUEST_BASE
            starts.append(a)
            names.append((row[3] or row[2])[:90])
    order = sorted(range(len(starts)), key=lambda i: starts[i])
    return [starts[i] for i in order], [names[i] for i in order]


def category(mod, sym):
    if mod.startswith("recompiled_"):
        if sym.startswith("blk_"):
            return "blocks (game code)"
        if re.match(r"recomp_(load|store|ldp|stp|ld|st)|recomp_fm|recomp_host_ptr|recomp_gg_store", sym):
            return "memory helpers"
        if sym.startswith(("recomp_image_run_slice", "recomp_lookup", "recomp_gg_miss", "recomp_code_guard")):
            return "dispatch/guard in the image"
        return "other image runtime"
    if mod.startswith("suyu-cmd"):
        return "suyu host (dispatcher, HLE kernel, services)"
    if mod == "?":
        return "no module"
    return "system (" + mod + ")"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--maps", default="D:/PLARecomp/perf/sym")
    ap.add_argument("--functions", default=os.path.join(os.path.dirname(__file__), "..", "ghidra", "out",
                                                         "main_functions_named.tsv"))
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--threads", default="CPUCore")
    # List of recompiled_image symbols with samples (blocks and helpers, most to least), for
    # lld-link /order:@<file>: gathers the hot code into few pages.
    ap.add_argument("--order-out", help="writes the list of hot main symbols for /order")
    a = ap.parse_args()

    maps = {}
    for dll, d in DLL_TO_DIR.items():
        p = os.path.join(a.maps, d, dll + ".map")
        if os.path.exists(p):
            maps[dll] = load_map(p)
    fstarts, fnames = load_functions(a.functions) if os.path.exists(a.functions) else ([], [])

    def sym_of(mod, rva):
        if mod not in maps:
            return None
        keys, names = maps[mod]
        i = bisect.bisect_right(keys, rva) - 1
        return names[i] if i >= 0 else None

    total = 0
    by_cat = collections.Counter()
    by_sym = collections.Counter()
    by_mod = collections.Counter()
    by_guest_fn = collections.Counter()
    by_caller_of_helper = collections.Counter()
    by_thread = collections.Counter()
    hot_image = collections.Counter()
    with open(a.raw, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2 or not parts[0].startswith(a.threads):
                continue
            total += 1
            by_thread[parts[0]] += 1
            mod, _, off = parts[1].rpartition("+0x")
            rva = int(off, 16)
            sym = sym_of(mod, rva) or (mod + "+?")
            by_mod[mod] += 1
            by_cat[category(mod, sym)] += 1
            by_sym[mod + "!" + sym] += 1
            # All frames of the main image on the stack (the block, the helper and its caller).
            for fr in parts[1:4]:
                fmod, _, foff = fr.rpartition("+0x")
                if fmod == "recompiled_image" and foff:
                    fsym = sym_of(fmod, int(foff, 16))
                    if fsym and (fsym.startswith("blk_main_") or fsym.startswith("recomp_")):
                        hot_image[fsym] += 1
            # Guest function: that of the block at the top or, if the top is a helper, that of the caller.
            gsym = sym if sym.startswith("blk_main_") else None
            if gsym is None and mod.startswith("recompiled_") and len(parts) > 2:
                cmod, _, coff = parts[2].rpartition("+0x")
                csym = sym_of(cmod, int(coff, 16)) or ""
                if csym.startswith("blk_"):
                    by_caller_of_helper[sym + " <- " + csym] += 1
                if csym.startswith("blk_main_"):
                    gsym = csym
            if gsym and fstarts:
                ga = int(gsym.rsplit("_", 1)[1], 16)
                i = bisect.bisect_right(fstarts, ga) - 1
                by_guest_fn[("0x%x %s" % (fstarts[i] + GUEST_BASE, fnames[i])) if i >= 0 else "?"] += 1

    if a.order_out:
        with open(a.order_out, "w", encoding="ascii") as o:
            for k, _ in hot_image.most_common():
                o.write(k + "\n")
        print("order: %d recompiled_image symbols in %s" % (len(hot_image), a.order_out))

    def dump(title, c, n):
        print("\n-- %s --" % title)
        for k, v in c.most_common(n):
            print("  %5.1f%%  %s" % (100.0 * v / max(1, total), k))

    print("samples %s: %d (%s)" % (a.threads, total, ", ".join("%s=%d" % kv for kv in sorted(by_thread.items()))))
    dump("by category", by_cat, 12)
    dump("by module", by_mod, 10)
    dump("by symbol (self)", by_sym, a.top)
    dump("by game function (main; block or helper caller)", by_guest_fn, a.top)
    dump("helper <- block that calls it", by_caller_of_helper, 20)


if __name__ == "__main__":
    sys.exit(main())
