#!/usr/bin/env python3
"""Partition of the "game" code into probable translation units (TUs).

Input:  data/triage.csv (tools/triage/triage.py) and the Ghidra outputs.
Output: data/tu_map.csv  (start,end,funcs,bytes,hint)  -- end is exclusive.

Note: the alignment of the game functions (4 or 16 bytes) varies WITHIN
a single TU (see docs/decomp/triage.md), so it is not usable as a cut.

Cuts (the linker preserves the TU order and, within each one, clang's
emission order):
  1. Stretch change: any function that is not game (library, PLT)
     cuts. Template instances (libc++, sol2, boost), thunks and
     initializers that fall inside a game stretch stay in the TU.
  2. Static initializer (_INIT_N = _GLOBAL__sub_I_<file>): clang emits it
     at the end of the module, so it closes the TU.
  3. Size: a stretch without cuts of more than MAX_FUNCS functions or MAX_BYTES bytes
     is split at the vtable (class) change closest to the midpoint.

The hint column summarizes the best clue for each TU: source paths (.cpp),
sol2 types, characteristic strings and class names.
"""
from __future__ import annotations

import csv
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import DATA, GHIDRA_OUT  # noqa: E402

MAX_FUNCS = 400
MAX_BYTES = 256 * 1024
MIN_BYTES = 8 * 1024    # smaller pieces are grouped with the next one
SPAN = 300              # maximum distance (in functions) of a must-link interval
GAME_LIKE = {"game", "lib-open:sol2", "lib-open:libc++", "lib-open:boost",
             "compiler-generated"}


def load_rows():
    with open(DATA / "triage.csv", encoding="utf-8") as fh:
        return [(int(r["Address"], 16), int(r["Size"]), r["Category"], r["Name"], r["Source"])
                for r in csv.DictReader(fh)]


def load_strings():
    per = defaultdict(list)
    cpp = re.compile(r"([A-Za-z0-9_]+\.(?:cpp|cc|h|hpp))")
    soltype = re.compile(r"\[T = (?:sol::usertype_metatable<)?([A-Za-z_:][A-Za-z0-9_:]*)")
    with open(GHIDRA_OUT / "strings.tsv", encoding="utf-8", errors="replace") as fh:
        next(fh)
        for line in fh:
            p = line.rstrip("\n").split("\t", 4)
            if len(p) < 5 or not p[3]:
                continue
            s = p[4]
            m = cpp.findall(s)
            hint = None
            if "localrombuild" in s and m:
                hint = ("src", m[-1] if "sol::" not in s else m[0])
            elif "ctti_get_type_name" in s:
                t = soltype.search(s)
                if t:
                    hint = ("sol", t.group(1))
            elif len(s) >= 6 and re.fullmatch(r"[\x20-\x7e]+", s) and int(p[2]) <= 3:
                hint = ("str", s[:40])
            if hint:
                for a in p[3].split(","):
                    per[int(a, 16)].append(hint)
    return per


def make_hint(funcs, strings):
    src, sol, strs, cls = Counter(), Counter(), Counter(), Counter()
    for a, s, c, n, _ in funcs:
        for kind, v in strings.get(a, []):
            {"src": src, "sol": sol, "str": strs}[kind][v] += 1
        m = re.match(r"^(.*)::(thunk\d*_)?vf\d+$", n)
        if m and not m.group(1).startswith("vtable_"):
            cls[m.group(1)] += 1
    parts = []
    if src:
        parts.append("src=" + ",".join(k for k, _ in src.most_common(2)))
    if sol:
        parts.append("sol=" + ",".join(k for k, _ in sol.most_common(2)))
    if cls:
        parts.append("cls=" + ",".join(k[:50] for k, _ in cls.most_common(1)))
    if strs and not src:
        parts.append("str=" + "|".join(k for k, _ in strs.most_common(2)))
    nvt = sum(1 for f in funcs if f[3].startswith("vtable_"))
    if nvt:
        parts.append(f"vfuncs={nvt}")
    if any(f[3].startswith("_INIT_") for f in funcs):
        parts.append("init")
    return "; ".join(parts).replace("\n", " ")


def vt_key(name):
    m = re.match(r"^(.*)::(thunk\d*_)?vf\d+$", name)
    return m.group(1) if m else None


def split_big(chunk):
    """Recursively split a stretch that is too large."""
    nb = sum(f[1] for f in chunk)
    if len(chunk) <= MAX_FUNCS and nb <= MAX_BYTES:
        return [chunk]
    mid = len(chunk) // 2
    best = None
    for d in range(0, len(chunk) // 2):
        for j in (mid - d, mid + d):
            if 1 <= j < len(chunk):
                a, b = vt_key(chunk[j - 1][3]), vt_key(chunk[j][3])
                if (a or b) and a != b:
                    best = j
                    break
        if best:
            break
    if best is None:
        best = mid
    return split_big(chunk[:best]) + split_big(chunk[best:])


def must_link(rows):
    """Coverage of "do not cut here": cov[i] > 0 if the gap between function i-1
    and i falls inside some interval that must go in the same TU:
      * writable data (.data/.bss) used only by nearby functions
        (static variables of the TU);
      * functions called only from nearby functions (static functions);
      * entries of the same vtable without RTTI (class defined in the TU)."""
    import numpy as np
    from common import Image
    from datarefs import adrp_refs
    from evidence import call_graph, load_funcs
    img = Image()
    starts = np.array([r[0] for r in rows])
    cover = np.zeros(len(rows) + 2, dtype=np.int64)

    def add(lo, hi):
        if 0 < hi - lo <= SPAN:
            cover[lo + 1] += 1
            cover[hi + 1] -= 1

    pcs, addrs = adrp_refs(img)
    fi = np.searchsorted(starts, pcs, side="right") - 1
    wr = np.zeros(addrs.size, dtype=bool)
    for lo, hi, fl, _ in img.segs:
        if fl & 2:
            wr |= (addrs >= lo) & (addrs < hi)
    users = defaultdict(set)
    for f, a in zip(fi[wr].tolist(), addrs[wr].tolist()):
        users[a].add(f)
    for us in users.values():
        if len(us) >= 2:
            add(min(us), max(us))
    fs = load_funcs()
    assert len(fs) == len(rows)
    callers, _ = call_graph(fs, img)
    for g, cs in callers.items():
        if len(cs) <= 8:
            s = set(cs) | {g}
            add(min(s), max(s))
    groups = defaultdict(list)
    for i, r in enumerate(rows):
        k = vt_key(r[3])
        if k and k.startswith("vtable_"):
            groups[k].append(i)
    for ids in groups.values():
        add(min(ids), max(ids))
    return np.cumsum(cover)[: len(rows)]


def main():
    rows = load_rows()
    strings = load_strings()
    cov = must_link(rows)

    # 1) hard stretches: between non-game functions and after each _INIT_
    hard = []
    cur = []
    for i, r in enumerate(rows):
        a, s, cat, name, src = r
        if cat not in GAME_LIKE:
            if cur:
                hard.append((cur, "lib"))
            cur = []
            continue
        cur.append(i)
        if name.startswith("_INIT_"):
            hard.append((cur, "init"))
            cur = []
    if cur:
        hard.append((cur, "lib"))

    # 2) within each hard stretch, candidate cuts at gaps without "must-link";
    #    pieces are grouped up to MIN_BYTES and split if they exceed MAX_*
    tus = []
    for ids, why in hard:
        pieces = [[ids[0]]]
        for i in ids[1:]:
            if cov[i] == 0:
                pieces.append([i])
            else:
                pieces[-1].append(i)
        local = []
        chunk = []
        for p in pieces:
            chunk.extend(p)
            if sum(rows[i][1] for i in chunk) >= MIN_BYTES:
                local.append(chunk)
                chunk = []
        if chunk:
            # small remainder at the end of the hard stretch: merged into the previous piece
            if local and sum(rows[i][1] for i in chunk) < MIN_BYTES // 2:
                local[-1] = local[-1] + chunk
            else:
                local.append(chunk)
        for k, c in enumerate(local):
            tus.append(([rows[i] for i in c], why if k == len(local) - 1 else "free"))

    final = []
    for t, why in tus:
        parts = split_big(t)
        for k, p in enumerate(parts):
            final.append((p, why if k == len(parts) - 1 else "size"))

    out = []
    for t, why in final:
        if not any(f[2] == "game" for f in t):
            continue
        start = t[0][0]
        end = t[-1][0] + t[-1][1]
        hint = make_hint(t, strings)
        hint = (hint + "; " if hint else "") + f"cut={why}"
        out.append((start, end, len(t), sum(f[1] for f in t), hint))

    with open(DATA / "tu_map.csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["start", "end", "funcs", "bytes", "hint"])
        for s, e, n, b, h in out:
            w.writerow([f"0x{s:016x}", f"0x{e:016x}", n, b, h])
    nb = sorted(o[3] for o in out)
    nf = sorted(o[2] for o in out)
    print(f"{len(out)} probable TUs with game code; functions {sum(nf)}, bytes {sum(nb)}")
    print(f"median {nf[len(nf)//2]} functions / {nb[len(nb)//2]} bytes; "
          f"p90 {nf[int(len(nf)*.9)]} functions / {nb[int(len(nb)*.9)]} bytes; max {nf[-1]} / {nb[-1]}")
    c = Counter(o[4].rsplit("cut=", 1)[1] for o in out)
    print("cuts:", dict(c))
    write_doc_section(out, nf, nb, c, int((cov == 0).sum()))
    return out


def write_doc_section(out, nf, nb, cuts, free_gaps):
    """Update the TU section at the end of docs/decomp/triage.md."""
    from common import ROOT
    doc = ROOT / "docs" / "decomp" / "triage.md"
    if not doc.exists():
        return
    head = "## Translation units (`data/tu_map.csv`)"
    text = doc.read_text(encoding="utf-8").split(head)[0].rstrip() + "\n\n"
    with_src = sum(1 for o in out if "src=" in o[4])
    with_sol = sum(1 for o in out if "sol=" in o[4])
    text += f"""{head}

Generated by `tools/triage/tu_map.py`: **{len(out)} units** containing game code
({sum(nf)} functions, {sum(nb) / 1048576:.2f} MiB, including interleaved templates and thunks).
Median {nf[len(nf) // 2]} functions / {nb[len(nb) // 2]} bytes; p90 {nf[int(len(nf) * .9)]} functions /
{nb[int(len(nb) * .9)]} bytes; maximum {nf[-1]} functions / {nb[-1]} bytes.

Cuts (`hint` column, field `cut=`): `init` {cuts.get('init', 0)} (end of TU by its
static initializer `_GLOBAL__sub_I_*`), `lib` {cuts.get('lib', 0)} (a library starts),
`free` {cuts.get('free', 0)} and `size` {cuts.get('size', 0)}.

- A `free` cut falls in a gap between functions that no "must-link" interval crosses: `.data`/`.bss`
  data used only by nearby functions (`static` variables), functions called
  only from nearby (`static` functions) and entries of a single vtable without RTTI. There are
  {free_gaps} such gaps in all of `.text`; since many are internal to a TU, the pieces are grouped
  up to ~8 KiB and split above 400 functions or 256 KiB (at a vtable change).
- Therefore each row is a **probable TU or a small group of contiguous TUs**: it serves as a unit
  of distribution (contiguous range, no libraries inside), not as an exact list of .cpp files.
- `hint` summarizes the clues: `src=` source file cited in strings ({with_src} units, mostly
  the Lua bindings `*_glue.cpp`), `sol=` C++ type registered in sol2 ({with_sol}), `cls=` class with
  RTTI, `str=` characteristic strings, `vfuncs=` virtual functions of vtables without RTTI and `init`.
- 16-byte alignment is not used as a cut: it varies within TUs (see above, PGO).
"""
    doc.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
