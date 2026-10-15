"""Atomic work claims (functions or TUs) so several agents do not step on each other.

  python tools/decomp/claim.py --agent A1 --count 10              # 10 free contiguous game functions
  python tools/decomp/claim.py --agent A1 --tu                    # one whole TU (data/tu_map.csv)
  python tools/decomp/claim.py --agent A1 --count 20 --max-size 120 --order size   # smallest first
  python tools/decomp/claim.py --agent A1 --addr 0x7100123456 0x7100123480        # specific ones
  python tools/decomp/claim.py --agent A1 --mine                  # my claims
  python tools/decomp/claim.py --agent A1 --release --addr 0x...  # release (or --all)
  python tools/decomp/claim.py --agent A1 --header include/ui/Example.h   # claim a header to edit it
  python tools/decomp/claim.py --agent A1 --release --header include/ui/Example.h

--addr also honors the category (default: game functions; --category for another);
--any claims the addresses even if they are not in that category.
Header claims live in data/header_claims.csv (same lock and same expiry).

Everything happens inside the data/decomp_status.csv lock. A claim expires after 24 h
(--expire-hours) unless renewed (claiming the same function again renews it).
The agent comes from --agent or the PLA_AGENT environment variable.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (DATA, ROOT, FileLock, agent_name, atomic_write_text, category, fmt, functions,  # noqa: E402
                    is_decomp_target, parse_addr, read_text_shared, tu_map)
import status as st  # noqa: E402


def _age_hours(ts: str) -> float:
    if not ts:
        return 1e9
    try:
        t = time.mktime(time.strptime(ts[:19], "%Y-%m-%dT%H:%M:%S"))
    except ValueError:
        return 1e9
    return (time.time() - t) / 3600


def is_free(row: dict | None, agent: str, expire: float) -> bool:
    if row is None:
        return True
    if row.get("quality") in ("O", "F"):
        return False
    owner = row.get("agent", "")
    if row.get("claimed_at") and owner and owner != agent and _age_hours(row["claimed_at"]) < expire:
        return False
    return True


HEADER_CLAIMS_CSV = DATA / "header_claims.csv"
HEADER_FIELDS = ["path", "agent", "claimed_at", "note"]


def _norm_header(h: str) -> str:
    p = Path(h)
    try:
        p = (p if p.is_absolute() else (Path.cwd() / p)).resolve().relative_to(ROOT)
    except ValueError:
        pass
    return p.as_posix()


def read_header_claims() -> dict[str, dict]:
    import csv
    import io
    text = read_text_shared(HEADER_CLAIMS_CSV) if HEADER_CLAIMS_CSV.exists() else None
    out = {}
    for r in csv.DictReader(io.StringIO(text or "", newline="")):
        if r.get("path"):
            out[r["path"]] = {k: r.get(k, "") or "" for k in HEADER_FIELDS}
    return out


def _write_header_claims(rows: dict[str, dict]):
    import csv
    import io
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=HEADER_FIELDS, lineterminator="\n", extrasaction="ignore")
    w.writeheader()
    for k in sorted(rows):
        w.writerow({f: str(rows[k].get(f, "")).replace(",", ";") for f in HEADER_FIELDS})
    atomic_write_text(HEADER_CLAIMS_CSV, buf.getvalue())


def header_cmd(a, agent: str) -> int:
    """Header claims (include/...): prevent two agents from editing the same file at once."""
    with FileLock(HEADER_CLAIMS_CSV):
        rows = read_header_claims()
        if a.mine:
            mine = {k: r for k, r in rows.items() if r["agent"] == agent and _age_hours(r["claimed_at"]) < a.expire_hours}
            for k, r in sorted(mine.items()):
                print(f"{k}  (since {r['claimed_at']})")
            print(f"{len(mine)} headers of {agent}")
            return 0
        targets = [_norm_header(h) for h in a.header] if a.header else []
        if a.release:
            n = 0
            for k in list(rows):
                if rows[k]["agent"] == agent and (a.all or k in targets):
                    del rows[k]
                    n += 1
            _write_header_claims(rows)
            print(f"released {n} headers of {agent}")
            return 0
        rc = 0
        now = st.now()
        for k in targets:
            r = rows.get(k)
            if r and r["agent"] != agent and _age_hours(r["claimed_at"]) < a.expire_hours:
                print(f"{k} is already claimed by {r['agent']} (since {r['claimed_at']})", file=sys.stderr)
                rc = 1
                continue
            rows[k] = {"path": k, "agent": agent, "claimed_at": now, "note": ""}
            print(f"claimed {k} for {agent}")
        _write_header_claims(rows)
        return rc


def suggested_file(addr: int) -> str:
    tm = tu_map()
    if tm and addr in tm:
        t = tm[addr]
        return t if t.endswith(".cpp") else f"src/{t}.cpp"
    return (f"src/<subsystem>/<Name>.cpp or src/unk/tu_{addr:x}.cpp "
            f"(without data/tu_map.csv: one TU = contiguous functions; see docs/decomp/CONTRIBUTING.md)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--agent")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--tu", nargs="?", const="", default=None, help="claim a whole TU (optional: its name)")
    ap.add_argument("--addr", nargs="+")
    ap.add_argument("--category", default=None, help="category from data/triage.csv (default: game)")
    ap.add_argument("--any", action="store_true", help="with --addr: claim even if they are not in the category")
    ap.add_argument("--header", nargs="+", help="claim (or with --release, release) headers to edit them")
    ap.add_argument("--min-size", type=int, default=1)
    ap.add_argument("--max-size", type=int, default=1 << 30)
    ap.add_argument("--order", choices=["address", "size"], default="address")
    ap.add_argument("--start", help="search starting from this address")
    ap.add_argument("--expire-hours", type=float, default=24.0)
    ap.add_argument("--mine", action="store_true")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--all", action="store_true", help="with --release: all my claims")
    a = ap.parse_args(argv)
    agent = agent_name(a.agent)
    if a.header or (a.mine and a.header is not None):
        return header_cmd(a, agent)
    funcs = functions()

    if a.mine:
        rows = st.read()
        mine = [(k, r) for k, r in sorted(rows.items()) if r.get("agent") == agent and
                (r.get("claimed_at") or r.get("quality") in ("W",))]
        for k, r in mine:
            f = funcs.at(k)
            print(f"{fmt(k)} {r['quality']} {f.size if f else '?':>6} {funcs.demangled(f) if f else ''}  {r.get('notes', '')}")
        print(f"{len(mine)} functions of {agent}")
        hs = [k for k, r in read_header_claims().items()
              if r["agent"] == agent and _age_hours(r["claimed_at"]) < a.expire_hours]
        if hs:
            print("claimed headers: " + ", ".join(sorted(hs)))
        return 0

    if a.release:
        with st.transaction() as rows:
            n = 0
            targets = {parse_addr(x) for x in a.addr} if a.addr else None
            for k, r in rows.items():
                if r.get("agent") == agent and r.get("claimed_at") and (a.all or (targets and k in targets)):
                    r["claimed_at"] = ""
                    r["updated_at"] = st.now()
                    n += 1
        print(f"released {n} claims of {agent}")
        if a.all and HEADER_CLAIMS_CSV.exists():
            a.header = []
            header_cmd(a, agent)
        return 0

    def wanted(f) -> bool:
        if f.size < a.min_size or f.size > a.max_size:
            return False
        if a.category:
            return category(f).lower() == a.category.lower()
        return is_decomp_target(f)

    claimed = []
    with st.transaction() as rows:
        if a.addr:
            for x in a.addr:
                f = funcs.resolve(x)
                if f is None:
                    print(f"{x} does not exist", file=sys.stderr)
                    continue
                if not is_free(rows.get(f.addr), agent, a.expire_hours):
                    r = rows[f.addr]
                    print(f"{fmt(f.addr)} is already taken by {r.get('agent')} ({r.get('quality')})", file=sys.stderr)
                    continue
                if not a.any and not (category(f).lower() == a.category.lower() if a.category
                                      else is_decomp_target(f)):
                    print(f"{fmt(f.addr)} is of category '{category(f)}', not "
                          f"'{a.category or 'game'}': not claimed (use --any to force it)", file=sys.stderr)
                    continue
                claimed.append(f)
        elif a.tu is not None:
            tm = tu_map()
            if not tm:
                print("data/tu_map.csv does not exist yet: use --count (contiguous ranges)", file=sys.stderr)
                return 2
            by_tu: dict[str, list] = {}
            for addr, t in tm.items():
                f = funcs.at(addr)
                if f is not None:
                    by_tu.setdefault(t, []).append(f)
            names = [a.tu] if a.tu else sorted(by_tu, key=lambda t: min(f.addr for f in by_tu[t]))
            for t in names:
                # already-done ones (O/F, e.g. generated by tools/decomp/autogen) do not prevent claiming the TU
                fs = [f for f in by_tu.get(t, []) if wanted(f) and
                      rows.get(f.addr, {}).get("quality", "U") not in ("O", "F")]
                if fs and all(is_free(rows.get(f.addr), agent, a.expire_hours) for f in fs) and \
                        any(rows.get(f.addr, {}).get("quality", "U") not in ("O", "F") for f in fs):
                    claimed = fs
                    break
        else:
            start = parse_addr(a.start) if a.start else 0
            cands = [f for f in funcs.funcs if f.addr >= start and wanted(f)]
            if a.order == "size":
                cands = [f for f in cands if is_free(rows.get(f.addr), agent, a.expire_hours)]
                cands.sort(key=lambda f: (f.size, f.addr))
                claimed = cands[: a.count]
            else:
                # first free contiguous run (in address order of the target functions)
                run = []
                for f in cands:
                    if rows.get(f.addr, {}).get("quality", "U") in ("O", "F"):
                        continue   # already done: does not break the run (autogen marks scattered functions everywhere)
                    if is_free(rows.get(f.addr), agent, a.expire_hours):
                        run.append(f)
                        if len(run) >= a.count:
                            break
                    elif run:
                        run = []
                claimed = run
        now = st.now()
        for f in claimed:
            r = rows.setdefault(f.addr, {k: "" for k in st.FIELDS})
            r["quality"] = r.get("quality") or "U"
            r["size"] = str(f.size)
            r["agent"] = agent
            r["claimed_at"] = now
            r["updated_at"] = now
    if not claimed:
        print("no free work with those filters")
        return 1
    tot = sum(f.size for f in claimed)
    print(f"claimed {len(claimed)} functions ({tot} bytes) for {agent}:")
    for f in claimed:
        print(f"  {fmt(f.addr)} {f.size:>6}  {funcs.demangled(f)}  [{category(f)}]")
    print(f"suggested file: {suggested_file(claimed[0].addr)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
