"""Functional decomp progress: % of functions and bytes by quality (O/F/W/U) and by category.

  python tools/decomp/progress.py              # summary (target = game functions) + categories
  python tools/decomp/progress.py --agents     # also, per agent
  python tools/decomp/progress.py --json

Sources: data/main_functions.csv (universe), data/triage.csv (categories; heuristic if missing)
and data/decomp_status.csv (qualities).
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import category, functions, is_decomp_target, triage  # noqa: E402
import status as st  # noqa: E402

QUALS = ["O", "F", "W", "U"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--agents", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    funcs = functions()
    rows = st.read()
    tgt_n = defaultdict(int)
    tgt_b = defaultdict(int)
    cat_n = defaultdict(lambda: defaultdict(int))
    cat_b = defaultdict(lambda: defaultdict(int))
    agents = defaultdict(lambda: defaultdict(int))
    for f in funcs.funcs:
        if f.size <= 0:
            continue
        q = rows.get(f.addr, {}).get("quality") or "U"
        q = q if q in QUALS else "U"
        c = category(f)
        cat_n[c][q] += 1
        cat_b[c][q] += f.size
        if is_decomp_target(f):
            tgt_n[q] += 1
            tgt_b[q] += f.size
        ag = rows.get(f.addr, {}).get("agent")
        if ag:
            agents[ag][q] += 1
    claimed = sum(1 for r in rows.values() if r.get("claimed_at"))
    out = {"target": {"functions": dict(tgt_n), "bytes": dict(tgt_b)},
           "categories": {c: {"functions": dict(cat_n[c]), "bytes": dict(cat_b[c])} for c in cat_n},
           "agents": {k: dict(v) for k, v in agents.items()}, "claimed": claimed,
           "triage": "data/triage.csv" if triage() else "heuristic"}
    if a.json:
        print(json.dumps(out, indent=1, ensure_ascii=False))
        return 0
    N, B = sum(tgt_n.values()), sum(tgt_b.values())

    def pct(x, t):
        return 100.0 * x / t if t else 0.0
    print(f"Target (game functions per {out['triage']}): {N} functions, {B / 1e6:.2f} MB of code")
    print(f"{'quality':<14}{'functions':>12}{'%':>9}{'bytes':>14}{'%':>9}")
    for q, lab in zip(QUALS, ["O matching", "F functional", "W written", "U pending"]):
        print(f"{lab:<14}{tgt_n[q]:>12}{pct(tgt_n[q], N):>8.3f}%{tgt_b[q]:>14}{pct(tgt_b[q], B):>8.3f}%")
    done_n, done_b = tgt_n["O"] + tgt_n["F"], tgt_b["O"] + tgt_b["F"]
    print(f"{'DONE (O+F)':<14}{done_n:>12}{pct(done_n, N):>8.3f}%{done_b:>14}{pct(done_b, B):>8.3f}%")
    print(f"active claims: {claimed}")
    print(f"\nBy category:\n{'category':<26}{'functions':>10}{'bytes':>12}{'O':>7}{'F':>7}{'W':>7}{'done bytes':>13}")
    for c in sorted(cat_n, key=lambda c: -sum(cat_b[c].values())):
        n, b = sum(cat_n[c].values()), sum(cat_b[c].values())
        d = cat_b[c]["O"] + cat_b[c]["F"]
        print(f"{c[:25]:<26}{n:>10}{b:>12}{cat_n[c]['O']:>7}{cat_n[c]['F']:>7}{cat_n[c]['W']:>7}{pct(d, b):>12.2f}%")
    if a.agents:
        print(f"\nBy agent:\n{'agent':<20}{'O':>6}{'F':>6}{'W':>6}{'U (claimed)':>16}")
        for ag, d in sorted(agents.items()):
            print(f"{ag:<20}{d['O']:>6}{d['F']:>6}{d['W']:>6}{d['U']:>16}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
