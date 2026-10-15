#!/usr/bin/env python3
"""Triage of all the functions of main.

Categories (Category column of data/triage.csv):
  game                 the game's own code
  lib-open:<name>      open-source library (Lua, libc++, gRPC, ...)
  lib-closed:<name>    closed library (Havok, Wwise, Pia, NPLN, ...)
  sdk-glue             PLT stubs to sdk/subsdk, runtime symbols, small static nn::
  compiler-generated   static initializers (.init_array) and this-adjustment thunks

Method:
  1. Per-function evidence (tools/triage/evidence.py): RTTI/vtable and PLT
     names, strings (source paths, asserts, type names), Lua 5.3.4 signature
     compiled with the identified compiler, thunk patterns.
  2. Segmentation by contiguity: the linker preserves the TU order, so
     libraries form contiguous stretches. It is solved with Viterbi over the
     function sequence (states = game + each library; switching state
     costs TRANSITION; the evidence adds to its state).
  3. "Overlay" layer: template instances of header-only libraries
     (libc++, sol2, boost) and compiler-generated code are labeled per
     function even if they are inside a game TU.

Usage:
    python tools/triage/triage.py            # writes data/triage.csv and docs/decomp/triage.md
"""
from __future__ import annotations

import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from common import DATA, ROOT, Image  # noqa: E402
from evidence import call_graph, collect, is_thunk, load_funcs  # noqa: E402

# Labels applied function by function (they do not define stretches)
OVERLAY = {"lib-open:lua", "lib-open:libc++", "lib-open:sol2", "lib-open:boost", "lib-open:flatbuffers",
           "lib-open:rapidjson", "compiler-generated", "sdk-glue"}
TRANSITION = 6.0      # cost of switching library between neighboring functions
GAME_PRIOR = 0.02     # preference for "game" of a function without evidence
NORTTI_GAME = 0.5     # function in a vtable without RTTI (the game is compiled with -fno-rtti)


def segment(fs, states, E):
    """Viterbi: E[i, s] = score of function i in state s."""
    n, k = E.shape
    back = np.zeros((n, k), dtype=np.int32)
    score = E[0].copy()
    for i in range(1, n):
        best_prev = int(np.argmax(score))
        stay = score
        switch = score[best_prev] - TRANSITION
        take_switch = switch > stay
        back[i] = np.where(take_switch, best_prev, np.arange(k))
        score = np.where(take_switch, switch, stay) + E[i]
    path = np.zeros(n, dtype=np.int32)
    path[-1] = int(np.argmax(score))
    for i in range(n - 1, 0, -1):
        path[i - 1] = back[i, path[i]]
    return [states[s] for s in path]


def main():
    img = Image()
    fs = load_funcs()
    idx_of, vt_of = collect(fs, img)
    callers, callees = call_graph(fs, img)

    # thunks (compiler-generated)
    for f in fs:
        if f.fixed is None and f.size <= 20:
            try:
                b = img.read(f.addr, f.size)
            except ValueError:
                continue
            if is_thunk(b):
                f.fixed, f.fixed_conf, f.fixed_src = "compiler-generated", "high", "pattern:thunk"

    # vtables without RTTI: the game (and Wwise, Oodle, ...) are compiled with -fno-rtti;
    # it is evidence AGAINST the libraries that do use RTTI.
    nortti = {i for i, groups in vt_of.items()
              if any(g.startswith("vtable_") for g in groups) and not fs[i].votes}

    # propagation through the call graph: if all the callers (BL/B) of a
    # function without evidence belong to the same library, so does the function
    # (libraries do not call game code directly). Outgoing calls
    # count for less (the game also calls the libraries' APIs).
    skip = {None, "lib-open:libc++", "lib-open:sol2", "lib-open:boost",
            "compiler-generated", "sdk-glue", "lib-open:lua"}

    def top(f):
        if f.fixed:
            return f.fixed
        if f.votes:
            return max(f.votes.items(), key=lambda kv: kv[1])[0]
        return None
    # (a) outgoing calls: a single pass and only from direct anchors (without
    # propagating), with at least two distinct targets of the same library
    anchor = [top(f) for f in fs]
    for i, f in enumerate(fs):
        if f.fixed or f.votes or i not in callees:
            continue
        tl = [anchor[c] for c in callees[i] if anchor[c] not in skip]
        if len(tl) >= 2 and len(set(tl)) == 1 and tl[0] != "game":
            f.vote(tl[0], 0.4, "graph:callees")
    # (b) callers: iterative
    def top_strong(f):
        t = top(f)
        if t and not f.fixed and f.why.get(t) == "graph:callees":
            return None          # labels from outgoing calls are not propagated
        return t
    for _round in range(6):
        cur = [top_strong(f) for f in fs]
        changed = 0
        for i, f in enumerate(fs):
            if f.fixed or f.votes or i not in callers:
                continue
            cl = [cur[c] for c in callers[i]
                  if cur[c] not in skip or cur[c] is None]
            labs = set(cl)
            # all the callers (that are not templates/thunks) must have a
            # label and be from the same library; an unlabeled caller blocks
            if len(labs) == 1 and None not in labs:
                lab = labs.pop()
                f.vote(lab, 1.5 if lab != "game" else 0.3, "graph:callers")
                changed += 1
        if not changed:
            break

    # segmentation states
    seg_labels = set()
    for f in fs:
        for lab in f.votes:
            if lab not in OVERLAY:
                seg_labels.add(lab)
        if f.fixed and f.fixed not in OVERLAY:
            seg_labels.add(f.fixed)
    seg_labels.add("game")
    states = sorted(seg_labels)
    si = {s: j for j, s in enumerate(states)}
    E = np.zeros((len(fs), len(states)), dtype=np.float64)
    E[:, si["game"]] = GAME_PRIOR
    rtti_libs = [si[s] for s in states if s.split(":")[-1] in
                 ("havok", "grpc", "protobuf", "pia", "npln", "nintendoware", "re2",
                  "abseil", "webrtc")]
    for i in nortti:
        E[i, rtti_libs] -= NORTTI_GAME
    for i, f in enumerate(fs):
        if f.fixed and f.fixed not in OVERLAY:
            E[i, si[f.fixed]] += 10.0
        for lab, w in f.votes.items():
            if lab in si:
                E[i, si[lab]] += w
            elif lab == "lib-open:sol2":       # sol2 is only instantiated in game TUs
                E[i, si["game"]] += 1.0
    seg = segment(fs, states, E)

    # "sandwich": a short "game" stretch without any game evidence between two
    # stretches of the SAME library is considered part of that library
    runs = []
    for i, lab in enumerate(seg):
        if runs and runs[-1][0] == lab:
            runs[-1][2] = i
        else:
            runs.append([lab, i, i])
    sandwiched = set()
    for k in range(1, len(runs) - 1):
        lab, a, b = runs[k]
        if lab != "game" or runs[k - 1][0] != runs[k + 1][0] or runs[k - 1][0] == "game":
            continue
        size = sum(fs[i].size for i in range(a, b + 1))
        has_game = any("game" in fs[i].votes or "lib-open:sol2" in fs[i].votes
                       for i in range(a, b + 1))
        if size < 256 * 1024 and not has_game:
            for i in range(a, b + 1):
                seg[i] = runs[k - 1][0]
                sandwiched.add(i)

    # -------- final assignment --------
    rows = []
    anchors_by_label = defaultdict(list)
    for i, f in enumerate(fs):
        if f.votes or f.fixed:
            anchors_by_label[seg[i]].append(i)
    for lab in anchors_by_label:
        anchors_by_label[lab] = np.array(anchors_by_label[lab])

    def near_anchor(i, lab, dist=8):
        arr = anchors_by_label.get(lab)
        if arr is None or not len(arr):
            return False
        j = np.searchsorted(arr, i)
        left = arr[j - 1] if j > 0 else -10**9
        right = arr[j] if j < len(arr) else 10**9
        return (i - left) <= dist and (right - i) <= dist

    # size (bytes) of the segmentation stretch each function belongs to
    run_bytes = [0] * len(fs)
    a = 0
    while a < len(fs):
        b = a
        while b + 1 < len(fs) and seg[b + 1] == seg[a]:
            b += 1
        tot = sum(fs[i].size for i in range(a, b + 1))
        for i in range(a, b + 1):
            run_bytes[i] = tot
        a = b + 1

    for i, f in enumerate(fs):
        segl = seg[i]
        own = max(f.votes.items(), key=lambda kv: kv[1])[0] if f.votes else None
        name = f.real_name or f.name
        if f.fixed:
            cat, conf, src = f.fixed, f.fixed_conf, f.fixed_src
        elif own in OVERLAY and own != "sdk-glue":
            cat, conf, src = own, "high" if f.votes[own] >= 3 else "medium", f.why[own]
        elif own == "sdk-glue":
            cat, conf, src = "sdk-glue", "high", f.why[own]
        elif own is not None and own == segl:
            cat, conf, src = segl, "high", f.why[own]
        elif own is not None and own != "game" and f.why.get(own) == "rtti/name" \
                and f.votes[own] >= 3:
            # class name of a library inside a TU of another origin:
            # inline function or header template (e.g. hkReferencedObject in the game)
            cat, conf, src = own, "medium", f"rtti/name (header inline in range {segl})"
        elif own is not None and own != "game":
            # own evidence that contradicts the stretch (e.g. a Havok string
            # used from game code): the stretch wins
            cat, conf, src = segl, "low", f"stretch:{segl} (own evidence: {own})"
        else:
            cat = segl
            if i in sandwiched:
                conf, src = "low", f"sandwich:{segl} (short stretch without evidence between two {segl} stretches)"
            elif segl == "game":
                conf = "medium" if (own == "game" or run_bytes[i] >= 1 << 20) else "low"
                src = f.why.get("game", "contiguity (no library evidence)")
            else:
                conf = "medium" if near_anchor(i, segl) else "low"
                src = f"contiguity:{segl}"
        rows.append((f.addr, f.size, cat, conf, name, src))

    # Lua: the Lua functions without a match in the alignment that ended up inside
    # the Lua range belong to the library (modified by GF or not mapped)
    LUA_LO, LUA_HI = 0x710004FD20, 0x71000791E4
    for k, r in enumerate(rows):
        if LUA_LO <= r[0] < LUA_HI and r[2] != "lib-open:lua":
            rows[k] = (r[0], r[1], "lib-open:lua", "medium", r[4], "contiguous Lua 5.3.4 range")

    # Libraries with exact signatures (flirt): extended to the contiguous stretch of
    # 16-byte-aligned functions that contains them (those libraries were
    # compiled with -mcpu=cortex-a57 without optsize; their neighbors were not)
    k = 0
    while k < len(rows):
        if rows[k][0] % 16:
            k += 1
            continue
        a = k
        while k + 1 < len(rows) and rows[k + 1][0] % 16 == 0:
            k += 1
        b = k
        k += 1
        libs = Counter(rows[i][2] for i in range(a, b + 1)
                       if rows[i][5].startswith("signature:") and rows[i][2] != "lib-open:lua")
        if not libs:
            continue
        lib = libs.most_common(1)[0][0]
        for i in range(a, b + 1):
            r = rows[i]
            if r[2] in (lib, "sdk-glue") or r[5].startswith("signature:"):
                continue
            rows[i] = (r[0], r[1], lib, "medium", r[4],
                       f"16-aligned stretch with exact signatures of {lib}")

    out = DATA / "triage.csv"
    with open(out, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["Address", "Size", "Category", "Confidence", "Name", "Source"])
        for a, s, c, conf, n, src in rows:
            w.writerow([f"0x{a:016x}", s, c, conf, n, src])
    print(f"wrote {out} ({len(rows)} functions)")
    write_summary(rows, seg, fs, callers)


def write_summary(rows, seg, fs, callers):
    from summary import write_markdown
    write_markdown(rows, seg, fs)


if __name__ == "__main__":
    main()
