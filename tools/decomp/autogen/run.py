"""Automatic C++ generator for trivial or repetitive functions, with verification.

  python tools/decomp/autogen/run.py hist [--unmatched 40]     # histogram of patterns (pending and free)
  python tools/decomp/autogen/run.py gen --patterns getter,setter [--limit 50] [--jobs 4] [--chunk 400]
  python tools/decomp/autogen/run.py gen --all

Flow of `gen` per pattern:
  1. scan.py: pending (U) and free game functions; template of each one.
  2. Reserves with claim.py (agent `autogen`) those that fit the pattern, BEFORE generating.
  3. Writes src/auto/<pattern>_<first>_<last>.cpp (up to --chunk functions per file).
  4. verify.py --file ... --no-status --json; if a function fails, tries the next emitter
     variant and, if none are left, removes it from the file. If the file does not compile,
     removes the functions on the lines with errors.
  5. Writes to data/decomp_status.csv (one transaction per file) only the O/F ones, registers their
     symbols and releases the reservation of those that failed.
At most --jobs verifications in parallel (4 by default).
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import csv
import io
import json
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
from common import ROOT, SYMBOLS_CSV, FileLock, atomic_write_text, fmt, functions  # noqa: E402
import status as st  # noqa: E402
import scan as sc  # noqa: E402
from patterns import PATTERNS, Decls, Match, find_pattern, find_patterns  # noqa: E402

AGENT = "autogen"
OUT_DIR = ROOT / "src" / "auto"
VERIFY = ROOT / "tools" / "decomp" / "verify.py"
LOG = ROOT / "tools" / "decomp" / "cache" / "autogen_log.jsonl"
VERIFY_TIMEOUT = 1200   # s per verify.py call (a loop that never ends in Unicorn must not hang the batch)
_log_lock = threading.Lock()


def log(rec: dict):
    with _log_lock:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")


# ------------------------------------------------------------------ classification
def classify(res: dict, skip_failed: bool = True) -> dict[str, list[int]]:
    """Pattern of each function. Patterns with `probe` (broad regular expressions) are only
    assigned if the emitter accepts the function (dry run with empty declarations).
    skip_failed: if the function already failed with a pattern (note `autogen <pattern>: ...`), the
    next one is tried (e.g. the generic translator)."""
    out = collections.defaultdict(list)
    cs = None
    notes = {}
    if skip_failed:
        notes = {x: r.get("notes", "") for x, r in st.read().items() if r.get("notes", "").startswith("autogen ")}
    for a, (size, h, tpl) in res.items():
        if not tpl:
            continue
        for p in find_patterns(tpl):
            if notes.get(a, "").startswith(f"autogen {p.key}:"):
                continue
            if p.probe:
                if cs is None:
                    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
                    from image import load_image
                    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
                    cs.detail = True
                    im = load_image()
                insns = list(cs.disasm(im.read(a, size), a))
                try:
                    ok = p.emit(Match.build(a, size, insns, tpl), Decls(), 0) is not None
                except Exception:  # noqa: BLE001
                    ok = False
                if not ok:
                    continue
            out[p.key].append(a)
            break
    for k in out:
        out[k].sort()
    return out


def cmd_hist(a):
    res = sc.scan() if not a.reuse else sc.load_scan()
    cl = classify(res)
    matched = {x for v in cl.values() for x in v}
    tot_n = len(res)
    tot_b = sum(s for s, _, _ in res.values())
    print(f"pending and free: {tot_n} functions, {tot_b} bytes")
    print(f"{'pattern':<22}{'functions':>10}{'bytes':>10}")
    for p in PATTERNS:
        xs = cl.get(p.key, [])
        print(f"{p.key:<22}{len(xs):>10}{sum(res[x][0] for x in xs):>10}  {p.desc}")
    print(f"{'TOTAL':<22}{len(matched):>10}{sum(res[x][0] for x in matched):>10}")
    if a.unmatched:
        c = collections.Counter(t for x, (s, h, t) in res.items() if t and x not in matched)
        sz = {t: s for s, h, t in res.values()}
        print("\nmost frequent templates without a pattern:")
        for t, k in c.most_common(a.unmatched):
            print(f"{k:>7} sz={sz[t]:<5} {t[:300]}")
    return 0


# ------------------------------------------------------------------ reservations
def claim_addrs(addrs: list[int]) -> set[int]:
    import claim
    for i in range(0, len(addrs), 2000):
        part = addrs[i:i + 2000]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            claim.main(["--agent", AGENT, "--addr", *[fmt(x) for x in part]])
    rows = st.read()
    return {x for x in addrs if rows.get(x, {}).get("agent") == AGENT and rows[x].get("claimed_at")
            and rows[x].get("quality") not in ("O", "F")}


# ------------------------------------------------------------------ generation
class Chunk:
    def __init__(self, pat, addrs: list[int], res: dict, cs):
        self.pat = pat
        self.addrs = addrs
        self.variant = {x: 0 for x in addrs}
        self.matches = {}
        funcs = functions()
        from image import load_image
        im = load_image()
        for x in addrs:
            f = funcs.at(x)
            insns = list(cs.disasm(im.read(x, f.size), x))
            self.matches[x] = Match.build(x, f.size, insns, res[x][2])
        self.path = OUT_DIR / f"{pat.key}_{addrs[0]:x}_{addrs[-1]:x}.cpp"
        k = 1
        while self.path.exists():   # do not overwrite an already generated file with the same range
            self.path = OUT_DIR / f"{pat.key}_{addrs[0]:x}_{addrs[-1]:x}_{k}.cpp"
            k += 1
        self.rel = self.path.relative_to(ROOT).as_posix()
        self.alive = list(addrs)
        self.line_of: dict[int, tuple[int, int]] = {}
        self.dropped: dict[int, str] = {}

    def render(self) -> str:
        d = Decls()
        self.line_of = {}
        self.text_of: dict[int, str] = {}
        bodies = []
        keep = []
        for x in self.alive:
            try:
                fn = self.pat.emit(self.matches[x], d, self.variant[x])
            except Exception as e:  # noqa: BLE001
                fn = None
                self.dropped[x] = f"emisor: {e!r}"[:150]
            if fn is None:
                self.dropped.setdefault(x, "the emitter does not accept these values")
                continue
            bodies.append((x, fn.render(x)))
            self.text_of[x] = bodies[-1][1]
            keep.append(x)
        self.alive = keep
        head = [f"// Generated automatically by tools/decomp/autogen (pattern `{self.pat.key}`: {self.pat.desc}).",
                "// Each function is verified with tools/decomp/verify.py (O or F). If the real type of",
                "// one of them is identified, move it to its TU and delete it from here.",
                '#include <new>', '#include <string>', "", '#include "types.h"', "", '#include "auto/Lua.h"',
                '#include "auto/Opaque.h"'] + [f'#include "{h}"' for h in self.pat.includes] + [""]
        decl = d.render()
        lines = head + ["namespace pla::autogen {", ""] + decl + ([""] if decl else [])
        for x, text in bodies:
            start = len(lines) + 1
            tl = text.split("\n")
            lines.extend(tl)
            self.line_of[x] = (start, start + len(tl) - 1)
            lines.append("")
        lines.append("}  // namespace pla::autogen")
        self.decl_lines = (len(head) + 1, len(head) + len(decl))
        self.lines = lines
        return "\n".join(lines) + "\n"

    def write(self):
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        atomic_write_text(self.path, self.render())

    def fn_at_line(self, ln: int):
        for x, (s, e) in self.line_of.items():
            if s <= ln <= e:
                return x
        return None

    def users_of_line(self, ln: int) -> set[int]:
        """Functions that use the name declared on line `ln` (error in a declaration:
        e.g. two incompatible signatures of the same known function). This way the whole file
        is not lost because of one declaration."""
        if not (1 <= ln <= len(getattr(self, "lines", []))):
            return set()
        names = re.findall(r"\b((?:FUN|DAT)_[0-9a-f]+(?:_\d+)?)\b", self.lines[ln - 1])
        out = set()
        for nm in names:
            rx = re.compile(r"\b" + re.escape(nm) + r"\b")
            out |= {x for x, t in self.text_of.items() if x in self.alive and rx.search(t)}
        return out


def run_verify(rel: str, targets: list[int] | None = None) -> tuple[int, list, str]:
    cmd = [sys.executable, str(VERIFY), "--file", rel, "--no-status", "--json", "--agent", AGENT]
    if targets:
        cmd += [fmt(x) for x in targets]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT,
                           timeout=VERIFY_TIMEOUT)
    except subprocess.TimeoutExpired:
        return 3, [], "verify.py exceeded the time limit"
    try:
        j = json.loads(p.stdout) if p.stdout.strip() else []
    except json.JSONDecodeError:
        j = []
    return p.returncode, j, p.stderr


ERR_RE = re.compile(r"^(?:.*[/\\])?([\w.-]+\.cpp):(\d+):\d+: error:", re.M)


def process_chunk(ch: Chunk, max_rounds: int = 12) -> dict:
    results: dict[int, dict] = {}
    pending = None           # None = verify everything
    for rnd in range(max_rounds):
        ch.write()
        if not ch.alive:
            break
        rc, js, err = run_verify(ch.rel, pending)
        if rc == 2 and not js:
            bad = set()
            for m in ERR_RE.finditer(err):
                if m.group(1) != ch.path.name:
                    continue
                x = ch.fn_at_line(int(m.group(2)))
                if x is not None:
                    bad.add(x)
                else:
                    bad |= ch.users_of_line(int(m.group(2)))
            if not bad:
                log({"file": ch.rel, "error": "does not compile (error outside the functions)", "diag": err[-2000:]})
                for x in ch.alive:
                    ch.dropped[x] = "no compila"
                ch.alive = []
                break
            for x in bad:
                ch.variant[x] += 1
                if ch.variant[x] >= ch.pat.variants and x in ch.alive:
                    ch.dropped[x] = "no compila"
                    ch.alive.remove(x)
            pending = [x for x in ch.alive if x not in results]
            continue
        fails = []
        for r in js:
            x = int(r["addr"], 16)
            if r["quality"] in ("O", "F"):
                results[x] = r
            else:
                fails.append((x, r))
        for x, r in fails:
            ch.variant[x] += 1
            if ch.variant[x] >= ch.pat.variants:
                ch.dropped[x] = ("; ".join(r.get("msgs", [])[:2]) or r.get("exact", "falla"))[:200]
                ch.alive.remove(x)
                results.pop(x, None)
        # functions that verify.py did not return (e.g. the symbol does not appear)
        seen = {int(r["addr"], 16) for r in js}
        if not js and err.strip():
            log({"file": ch.rel, "verify_stderr": err[-3000:]})
        for x in list(pending or ch.alive):
            if x not in seen and x in ch.alive:
                ch.dropped[x] = "verify.py returned no result"
                ch.alive.remove(x)
        if not fails and all(x in results for x in ch.alive):
            break
        pending = [x for x in ch.alive if x not in results]
    # final file: only those that pass (those left without a result after the rounds, with a note)
    for x in ch.alive:
        if x not in results:
            ch.dropped.setdefault(x, "no result after the rounds")
    ch.alive = [x for x in ch.alive if x in results]
    ch.write()
    if not ch.alive:
        ch.path.unlink(missing_ok=True)
    elif len(ch.dropped) > 0:
        # final check of the definitive file (must pass entirely)
        rc, js, err = run_verify(ch.rel)
        ok = {int(r["addr"], 16): r for r in js if r["quality"] in ("O", "F")}
        for x in list(ch.alive):
            if x not in ok:
                ch.dropped[x] = "fails in the final verification"
                ch.alive.remove(x)
            else:
                results[x] = ok[x]
        if len(ok) != len(js) or rc == 2:
            ch.write()
            if not ch.alive:
                ch.path.unlink(missing_ok=True)
    apply_results(ch, {x: results[x] for x in ch.alive})
    q = collections.Counter(results[x]["quality"] for x in ch.alive)
    return {"file": ch.rel, "pattern": ch.pat.key, "ok": len(ch.alive), "O": q["O"], "F": q["F"],
            "dropped": len(ch.dropped), "drop_reasons": collections.Counter(v[:80] for v in ch.dropped.values())}


def apply_results(ch: Chunk, ok: dict[int, dict]):
    from demangle import demangle_many
    funcs = functions()
    dem = demangle_many([r["symbol"] for r in ok.values() if r.get("symbol")]) if ok else {}
    with st.transaction() as rows:
        now = st.now()
        for x, r in ok.items():
            row = rows.setdefault(x, {k: "" for k in st.FIELDS})
            if row.get("agent") not in (AGENT, "") and row.get("claimed_at"):
                continue   # (should not happen: reserved by another)
            row.update({"quality": r["quality"], "size": str(funcs.at(x).size), "agent": AGENT, "claimed_at": "",
                        "updated_at": now, "file": ch.rel, "symbol": r.get("symbol", ""),
                        "coverage": r.get("coverage", ""), "cases": r.get("cases", ""), "notes": ""})
        for x, why in ch.dropped.items():
            row = rows.get(x)
            if row and row.get("agent") == AGENT and row.get("quality") not in ("O", "F"):
                row["claimed_at"] = ""
                row["updated_at"] = now
                row["notes"] = f"autogen {ch.pat.key}: {why}"[:200]
    apply_results_symbols(ch, ok)


def apply_results_symbols(ch, ok: dict[int, dict]):
    from demangle import demangle_many
    dem = demangle_many([r["symbol"] for r in ok.values() if r.get("symbol")]) if ok else {}
    if ok:
        with FileLock(SYMBOLS_CSV):
            srows = {}
            if SYMBOLS_CSV.exists():
                with open(SYMBOLS_CSV, newline="", encoding="utf-8") as f:
                    for row in csv.DictReader(f):
                        srows[row["address"]] = row
            for x, r in ok.items():
                if r.get("symbol"):
                    srows[fmt(x)] = {"address": fmt(x), "symbol": r["symbol"],
                                     "demangled": (dem.get(r["symbol"]) or r["symbol"]).replace(",", ";"),
                                     "file": ch.rel}
            buf = io.StringIO()
            w = csv.DictWriter(buf, fieldnames=["address", "symbol", "demangled", "file"], lineterminator="\n")
            w.writeheader()
            for k in sorted(srows):
                w.writerow(srows[k])
            atomic_write_text(SYMBOLS_CSV, buf.getvalue())


def cmd_resync(a):
    """Registers in data/decomp_symbols.csv the symbols of the autogen O/F rows that are missing and
    releases the autogen reservations that have no file (not generated)."""
    rows = st.read()
    mine = {x: r for x, r in rows.items() if r.get("agent") == AGENT}
    done = {x: r for x, r in mine.items() if r.get("quality") in ("O", "F") and r.get("symbol")}
    have = set()
    if SYMBOLS_CSV.exists():
        with open(SYMBOLS_CSV, newline="", encoding="utf-8") as f:
            have = {int(r["address"], 16) for r in csv.DictReader(f)}
    miss = {x: r for x, r in done.items() if x not in have}
    by_file = collections.defaultdict(dict)
    for x, r in miss.items():
        by_file[r["file"]][x] = {"symbol": r["symbol"]}

    class _C:  # noqa: N801
        pass
    for fl, ok in by_file.items():
        c = _C()
        c.rel = fl
        c.dropped = {}
        c.pat = type("P", (), {"key": "resync"})
        apply_results_symbols(c, ok)
    print(f"symbols added: {len(miss)}")
    if a.release:
        with st.transaction() as rows:
            n = 0
            for x, r in rows.items():
                if r.get("agent") == AGENT and r.get("claimed_at") and r.get("quality") not in ("O", "F"):
                    r["claimed_at"] = ""
                    r["updated_at"] = st.now()
                    n += 1
        print(f"reservations released: {n}")
    return 0


def cmd_gen(a):
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True
    res = sc.scan()
    cl = classify(res, skip_failed=not a.retry)
    keys = [p.key for p in PATTERNS] if a.all else a.patterns.split(",")
    bykey = {p.key: p for p in PATTERNS}
    chunks = []
    for k in keys:
        p = bykey[k]
        addrs = cl.get(k, [])
        if not a.retry:   # do not repeat those that already failed with this pattern
            rows = st.read()
            addrs = [x for x in addrs if not rows.get(x, {}).get("notes", "").startswith(f"autogen {k}:")]
        if a.limit:
            addrs = addrs[: a.limit]
        if not addrs:
            continue
        got = sorted(claim_addrs(addrs))
        print(f"{k}: {len(addrs)} candidates, {len(got)} reserved", flush=True)
        for i in range(0, len(got), a.chunk):
            chunks.append(Chunk(p, got[i:i + a.chunk], res, cs))
    t0 = time.time()
    tot = collections.Counter()
    def safe(ch):
        try:
            return process_chunk(ch)
        except Exception as e:  # noqa: BLE001
            import traceback
            log({"file": ch.rel, "exception": traceback.format_exc()[-1500:]})
            # undo: delete the file and release the reservations that did not end up O/F
            rows = st.read()
            if not any(rows.get(x, {}).get("file") == ch.rel and rows[x].get("quality") in ("O", "F")
                       for x in ch.addrs):
                ch.path.unlink(missing_ok=True)
            with st.transaction() as rows:
                for x in ch.addrs:
                    r = rows.get(x)
                    if r and r.get("agent") == AGENT and r.get("quality") not in ("O", "F"):
                        r["claimed_at"] = ""
                        r["notes"] = f"autogen {ch.pat.key}: exception"
            return {"file": ch.rel, "pattern": ch.pat.key, "ok": 0, "O": 0, "F": 0, "dropped": 0,
                    "drop_reasons": collections.Counter({f"EXCEPTION {e!r}"[:80]: 1})}

    single = threading.Lock()

    def throttled(ch):
        # if another agent is running the game (lock), only one verification at a time
        if a.game_lock and Path(a.game_lock).exists():
            with single:
                return safe(ch)
        return safe(ch)

    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for r in ex.map(throttled, chunks):
            tot["ok"] += r["ok"]
            tot["O"] += r["O"]
            tot["F"] += r["F"]
            tot["dropped"] += r["dropped"]
            log({k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in r.items()})
            print(f"{r['file']}: {r['ok']} ok (O {r['O']}, F {r['F']}), {r['dropped']} removed "
                  f"{dict(r['drop_reasons'].most_common(3)) if r['dropped'] else ''}  [{time.time() - t0:.0f}s]",
                  flush=True)
    print(f"TOTAL: {tot['ok']} ok (O {tot['O']}, F {tot['F']}), {tot['dropped']} removed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("hist")
    h.add_argument("--unmatched", type=int, default=0)
    h.add_argument("--reuse", action="store_true")
    g = sub.add_parser("gen")
    g.add_argument("--patterns", default="")
    g.add_argument("--all", action="store_true")
    g.add_argument("--limit", type=int, default=0)
    g.add_argument("--chunk", type=int, default=400)
    g.add_argument("--jobs", type=int, default=4)
    g.add_argument("--game-lock", default="D:/PLARecomp/locks/game",
                   help="if this file exists, only one verification at a time ('' = do not check)")
    g.add_argument("--retry", action="store_true", help="retry those that already failed with the same pattern")
    r = sub.add_parser("resync")
    r.add_argument("--release", action="store_true", help="release the autogen reservations without generating")
    a = ap.parse_args(argv)
    return {"hist": cmd_hist, "gen": cmd_gen, "resync": cmd_resync}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
