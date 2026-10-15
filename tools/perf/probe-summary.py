#!/usr/bin/env python3
"""Summarizes the SUYU_PERF_PROBE probe of a perf-run.ps1 run: guest CPU per frame.

Sums, per 5 s window, the time all guest threads spend inside the recompiled code
(ms/s) and divides it by the FPS measured in that window. If the emitter generates
faster code, the guest time per frame goes down even if the FPS do not go up (when the
limit is thread synchronization and not computation).

  python tools/perf/probe-summary.py <run folder> [--start 300] [--end 590]
"""
import argparse
import os
import re
import statistics

THREAD = re.compile(r"^\[\s*([\d.]+)\].*PROBE thread\s+(\d+) \[prio (\d+).*guest\s+([\d.]+) ms/s.*svc\s+([\d.]+) ms/s: (.*)$")
FPS = re.compile(r"^\[\s*([\d.]+)\].*PERF game_fps=([\d.]+)")
ARB = re.compile(r"ArbitrateLock:([\d.]+)ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--start", type=float, default=300)
    ap.add_argument("--end", type=float, default=590)
    a = ap.parse_args()
    windows = {}
    fps = []
    with open(os.path.join(a.run, "suyu_log.txt"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FPS.match(line)
            if m:
                t = float(m.group(1))
                if a.start <= t <= a.end:
                    fps.append(float(m.group(2)))
                continue
            m = THREAD.match(line)
            if m:
                t = round(float(m.group(1)))
                if not (a.start <= t <= a.end):
                    continue
                w = windows.setdefault(t, {"guest": 0.0, "arb": 0.0, "threads": {}})
                g = float(m.group(4))
                w["guest"] += g
                w["threads"][int(m.group(2))] = g
                am = ARB.search(m.group(6))
                if am:
                    w["arb"] += float(am.group(1))
    if not windows or not fps:
        print("no probe or FPS data in the window")
        return 1
    f = statistics.mean(fps)
    guest = statistics.mean(w["guest"] for w in windows.values())
    arb = statistics.mean(w["arb"] for w in windows.values())
    per_thread = {}
    for w in windows.values():
        for tid, g in w["threads"].items():
            per_thread.setdefault(tid, []).append(g)
    top = sorted(((statistics.mean(v), tid) for tid, v in per_thread.items()), reverse=True)[:6]
    print("%s: FPS %.1f | guest %.0f ms/s = %.1f ms/frame | ArbitrateLock %.0f ms/s | windows %d" %
          (os.path.basename(a.run.rstrip("/\\")), f, guest, guest / f, arb, len(windows)))
    print("  threads with most guest (ms/s): " + ", ".join("%d=%.0f" % (tid, g) for g, tid in top))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
