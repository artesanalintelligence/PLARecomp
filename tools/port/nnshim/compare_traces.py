#!/usr/bin/env python3
"""Compares two runs of the nn shim (A/B test): the HLE one (families in "observe") and the
native one. Reads the trace (trace_path) and the statistics (stats_path) of each.

  python tools/port/nnshim/compare_traces.py <dir_A> <dir_B>
      each directory contains trace.txt and stats.json (written by run-ab.ps1)

Reports:
  - RomFS files opened: how many, how many distinct, those that only appear on one side and
    whether the order of the first N opens matches (the game loads on several threads: the
    total order may vary a little; the set should not);
  - different results for the same path (e.g. PathNotFound on one side and success on the other);
  - byte validation (observe side): reads compared with the host RomFS and mismatches;
  - per-adapter counters of each side.
Exits with 0 if the file sets match and there were no byte mismatches.
"""
from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path

OPEN_RE = re.compile(r"^\d+ open (\S+) m=(\d+) r=(0x[0-9a-f]+|0)$")


def load(d: Path):
    opens = []
    trace = d / "trace.txt"
    if trace.exists():
        for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
            m = OPEN_RE.match(line)
            if m:
                opens.append((m.group(1), int(m.group(2)), int(m.group(3), 16)))
    stats = {}
    if (d / "stats.json").exists():
        stats = json.loads((d / "stats.json").read_text(encoding="utf-8"))
    return opens, stats


def main(argv=None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 2:
        print(__doc__)
        return 2
    a_dir, b_dir = Path(argv[0]), Path(argv[1])
    a_opens, a_stats = load(a_dir)
    b_opens, b_stats = load(b_dir)

    def rom(opens):
        return [o for o in opens if not o[0].startswith(("save:", "SaveData", "save_"))]

    ar, br = rom(a_opens), rom(b_opens)
    a_set, b_set = {p for p, _, _ in ar}, {p for p, _, _ in br}
    print(f"A ({a_dir.name}): {len(ar)} opens, {len(a_set)} distinct files")
    print(f"B ({b_dir.name}): {len(br)} opens, {len(b_set)} distinct files")
    common = a_set & b_set
    only_a, only_b = sorted(a_set - b_set), sorted(b_set - a_set)
    print(f"common: {len(common)}; only in A: {len(only_a)}; only in B: {len(only_b)}")
    for p in only_a[:20]:
        print(f"  only A: {p}")
    for p in only_b[:20]:
        print(f"  only B: {p}")
    # Results per path.
    ra = {p: r for p, _, r in ar}
    rb = {p: r for p, _, r in br}
    diff_res = [(p, ra[p], rb[p]) for p in sorted(common) if ra[p] != rb[p]]
    print(f"different results for the same path: {len(diff_res)}")
    for p, x, y in diff_res[:20]:
        print(f"  {p}: A={x:#x} B={y:#x}")
    # Order of the first opens (sequence of paths, without repeats).
    def first_unique(opens, n):
        seen, out = set(), []
        for p, _, _ in opens:
            if p not in seen:
                seen.add(p)
                out.append(p)
            if len(out) >= n:
                break
        return out
    n = min(len(a_set), len(b_set), 200)
    fa, fb = first_unique(ar, n), first_unique(br, n)
    same_prefix = 0
    for x, y in zip(fa, fb):
        if x != y:
            break
        same_prefix += 1
    print(f"order: the first {same_prefix} of {n} opens match in the same order")
    # Repeated opens (how many times each file): same counts on both sides.
    ca, cb = Counter(p for p, _, _ in ar), Counter(p for p, _, _ in br)
    diff_cnt = [(p, ca[p], cb[p]) for p in common if ca[p] != cb[p]]
    print(f"files opened a different number of times: {len(diff_cnt)}")

    for name, st in (("A", a_stats), ("B", b_stats)):
        c = st.get("counters", {})
        if c:
            print(f"counters {name}: " + ", ".join(f"{k}={v}" for k, v in sorted(c.items())))
    va = a_stats.get("counters", {})
    mism = va.get("fs.validate.mismatch", 0)
    if va.get("fs.validate.reads"):
        print(f"byte validation (A, HLE vs. host RomFS): {va.get('fs.validate.reads')} reads, "
              f"{va.get('fs.validate.bytes', 0)} bytes, {mism} mismatched, {va.get('fs.validate.unchecked', 0)} unchecked")
    ok = not only_a and not only_b and not mism and not diff_res
    print("RESULT:", "EQUAL" if ok else "DIFFERENCES")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
