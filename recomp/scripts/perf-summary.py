"""Summarizes perf-run.ps1 runs: average FPS per window, system load and, if there is a probe, SVCs.

    python recomp/scripts/perf-summary.py D:/PLARecomp/perf/runs/*R1* D:/PLARecomp/perf/runs/*R2*

Windows (seconds since suyu-cmd started, a-mash script with wall-clock TAS at 60 Hz):
  dialog = 120-220 s (intro scene on the beach with the professor, dialog boxes)
  beach  = 300-590 s (free control on the prologue beach, character standing still)
"""
import csv
import glob
import os
import re
import statistics
import sys

WINDOWS = {"dialog": (120, 220), "beach": (300, 590)}


def load_perf(run):
    rows = []
    p = os.path.join(run, "perf.csv")
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            rows.append((float(r["t"]), float(r["game_fps"]), float(r["frametime_ms"]) if r["frametime_ms"] not in ("nan", "-nan") else 0.0))
    return rows


def load_load(run):
    rows = []
    p = os.path.join(run, "load.csv")
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            rows.append((float(r["t"]), float(r["cpu_total_pct"]), float(r["suyu_cpu_pct"])))
    return rows


def probe(run, lo, hi):
    """Average ArbitrateLock/s, average us and total wait per second in the window."""
    log = os.path.join(run, "suyu_log.txt")
    if not os.path.exists(log):
        return None
    pat = re.compile(r"^\[\s*([\d.]+)\].*PROBE svcs:(.*)$")
    acc = {}
    n = 0
    for line in open(log, encoding="utf-8", errors="replace"):
        m = pat.match(line)
        if not m or not (lo <= float(m.group(1)) <= hi):
            continue
        n += 1
        for name, rate, us in re.findall(r" (\w+)=(\d+)/s avg ([\d.]+)us;", m.group(2)):
            a = acc.setdefault(name, [0.0, 0.0])
            a[0] += float(rate)
            a[1] += float(rate) * float(us)
    if not n:
        return None
    out = {}
    for name, (rate, weighted) in acc.items():
        r = rate / n
        out[name] = (r, weighted / rate if rate else 0.0, weighted / n / 1000.0)
    return out


def main():
    runs = []
    for a in sys.argv[1:]:
        runs += sorted(glob.glob(a))
    for run in runs:
        perf = load_perf(run)
        load = load_load(run)
        print(f"== {os.path.basename(run)}")
        for wname, (lo, hi) in WINDOWS.items():
            f = [x[1] for x in perf if lo <= x[0] <= hi]
            l = [x[1] for x in load if lo <= x[0] <= hi]
            s = [x[2] for x in load if lo <= x[0] <= hi]
            if not f:
                print(f"   {wname:8s}: no samples")
                continue
            p5 = sorted(f)[max(0, len(f) // 20)]
            print(f"   {wname:8s}: FPS mean {statistics.mean(f):6.1f}  median {statistics.median(f):6.1f}  p5 {p5:6.1f}"
                  f"  (n={len(f)})  system load {statistics.mean(l) if l else 0:4.0f}%  suyu {statistics.mean(s) if s else 0:4.1f}%")
            pr = probe(run, lo, hi)
            if pr:
                top = sorted(pr.items(), key=lambda kv: -kv[1][2])[:5]
                print("             SVC (calls/s, average us, ms of wait per s): " +
                      "; ".join(f"{k} {v[0]:.0f}/s {v[1]:.1f}us {v[2]:.0f}ms" for k, v in top))


if __name__ == "__main__":
    main()
