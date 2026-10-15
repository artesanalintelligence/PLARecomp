#!/usr/bin/env python3
"""Aggregates hostprof's HOSTPROF_GUEST dump (plahost GuestThread samples) by game function.

  python tools/perf/guest-prof.py <prof-guest.txt> --log <pla.log> [--maps D:/PLARecomp/perf/symxc]
         [--top 40] [--threads GuestThread80,...] [--merge] [--paths N] [--focus <name substring>]

Line format (hostprof): <thread> TAB <host frames module+rva, comma-separated> TAB pc TAB x30 TAB x29 TAB sp TAB <stack values...>
- Self: the host top frame. In a recompiled DLL it is mapped to its block (linker map, blk_<module>_<offset>)
  and then to the game function containing that offset (main: tools/ghidra/out/main_functions_named.tsv).
  Anywhere else (plahost, ntdll: kernel calls, waits, memory helpers) it is reported as "[host <module>]".
- Inclusive: the self function plus every value of the dumped guest stack that is a return address into main
  (the instruction before it is a BL or BLR). EXPERIMENTAL: with the current recompiled code the GuestContext
  that hostprof reads is stale, so this column normally equals the self column (see hostprof.cpp).
"""
import argparse
import bisect
import collections
import csv
import os
import re
import struct

GHIDRA_BASE = 0x7100000000
LOADER = re.compile(r"Loader <Info> (\w+)\s+at 0x([0-9a-f]+) size 0x([0-9a-f]+)")
MAP_LINE = re.compile(r"^\s*[0-9a-f]{4}:[0-9a-f]{8}\s+blk_(\w+?)_([0-9a-f]{16})\s+([0-9a-f]{16})", re.I)
HERE = os.path.dirname(os.path.abspath(__file__))
DLL = {"recompiled_image": "main", "recompiled_sdk": "sdk", "recompiled_subsdk0": "subsdk0",
       "recompiled_subsdk1": "subsdk1"}


def load_functions(path):
    starts, sizes, names = [], [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            starts.append(int(row[0], 16) - GHIDRA_BASE)
            sizes.append(int(row[1] or 0))
            names.append((row[3] or row[2])[:100])
    order = sorted(range(len(starts)), key=lambda i: starts[i])
    return [starts[i] for i in order], [sizes[i] for i in order], [names[i] for i in order]


def load_map(path):
    rvas, offs = [], []
    base = 0x180000000
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
                continue
            m = MAP_LINE.match(line)
            if m:
                rvas.append(int(m.group(3), 16) - base)
                offs.append(int(m.group(2), 16))
    order = sorted(range(len(rvas)), key=lambda i: rvas[i])
    return [rvas[i] for i in order], [offs[i] for i in order]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--log", required=True)
    ap.add_argument("--maps", default="D:/PLARecomp/perf/symxc")
    ap.add_argument("--functions", default=os.path.join(HERE, "..", "ghidra", "out", "main_functions_named.tsv"))
    ap.add_argument("--elf", default=os.path.join(HERE, "..", "..", "data", "main.elf"))
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--threads", default="")
    ap.add_argument("--merge", action="store_true", help="one report for all the selected threads")
    ap.add_argument("--paths", type=int, default=0, help="also print the N most frequent (caller > self) pairs")
    ap.add_argument("--focus", default="", help="only samples whose inclusive set contains this substring")
    a = ap.parse_args()

    mods = {}
    for line in open(a.log, encoding="utf-8", errors="replace"):
        m = LOADER.search(line)
        if m:
            mods[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    starts, sizes, names = load_functions(a.functions)
    maps = {}
    for dll, mod in DLL.items():
        p = os.path.join(a.maps, mod, dll + ".map")
        if os.path.exists(p):
            maps[dll] = load_map(p)
    text = b""
    try:
        from elftools.elf.elffile import ELFFile
        with open(a.elf, "rb") as f:
            seg = [s for s in ELFFile(f).iter_segments() if s["p_type"] == "PT_LOAD"][0]
            text = seg.data()
    except Exception as e:  # noqa: BLE001
        print("no main.elf text (%s): inclusive without the BL check" % e)
    main_base = mods.get("main", (0x80004000, 0))[0]

    def fname(off):
        i = bisect.bisect_right(starts, off) - 1
        if i >= 0 and (sizes[i] == 0 or off < starts[i] + sizes[i] + 4):
            return names[i] or "main+%x" % starts[i]
        return "main+%x" % off

    def is_ret(va):
        off = va - main_base
        if not text or off < 4 or off >= len(text):
            return False
        w = struct.unpack_from("<I", text, off - 4)[0]
        return (w >> 26) == 0x25 or (w & 0xFFFFFC1F) == 0xD63F0000  # BL / BLR

    cache = {}

    def self_of(host):
        if host in cache:
            return cache[host]
        mod, _, rva = host.partition("+")
        r = "[host %s]" % mod
        if mod in maps and rva:
            rv, off = maps[mod]
            i = bisect.bisect_right(rv, int(rva, 16)) - 1
            if i >= 0:
                o = off[i]
                r = fname(o) if DLL[mod] == "main" else "%s+%x" % (DLL[mod], o & ~0xFF)
        cache[host] = r
        return r

    want = set(t for t in a.threads.split(",") if t)
    per = collections.defaultdict(list)
    for line in open(a.dump, encoding="utf-8", errors="replace"):
        if line.startswith("#"):
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) < 2 or (want and p[0] not in want):
            continue
        th = "ALL" if a.merge else p[0]
        frames = p[1].split(",")
        s = self_of(frames[0])
        if s.startswith("[host recompiled_"):  # runtime helper (memory, FP): charge the calling block
            for fr in frames[1:]:
                t = self_of(fr)
                if not t.startswith("[host"):
                    s = t
                    break
        callers = []
        for x in p[6:]:
            va = int(x, 16)
            if is_ret(va):
                f = fname(va - main_base)
                if f != s and f not in callers:
                    callers.append(f)
        if a.focus and a.focus not in s and not any(a.focus in c for c in callers):
            continue
        per[th].append((s, callers))

    for th in sorted(per, key=lambda t: -len(per[t])):
        ch = per[th]
        n = len(ch)
        if n < 20:
            continue
        selfc, incl, pairs = collections.Counter(), collections.Counter(), collections.Counter()
        for s, callers in ch:
            selfc[s] += 1
            incl[s] += 1
            for c in callers:
                incl[c] += 1
            if a.paths:
                pairs["%s > %s" % (callers[0] if callers else "?", s)] += 1
        print("\n== %s: %d busy samples ==" % (th, n))
        print("  -- self --")
        for f, c in selfc.most_common(a.top):
            print("    %5.1f%%  %s" % (100.0 * c / n, f))
        print("  -- inclusive (stack scan) --")
        for f, c in incl.most_common(a.top):
            print("    %5.1f%%  %s" % (100.0 * c / n, f))
        if a.paths:
            print("  -- nearest caller > self --")
            for f, c in pairs.most_common(a.paths):
                print("    %5.1f%%  %s" % (100.0 * c / n, f))


if __name__ == "__main__":
    main()
