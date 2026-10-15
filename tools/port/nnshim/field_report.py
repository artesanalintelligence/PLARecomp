#!/usr/bin/env python3
"""Per-phase summary of a field session (field_drive.py + PERF + frame_dt_probe).

Reads <session>/suyu_log.txt ("PERF game_fps=..." lines every second and "DT frames=... game/real=..."
from frame_dt_probe every 5 s) and <session>/drive-*.txt (phases with their time since start) and gives,
per phase: mean / min / max FPS, and the game time / real time ratio.
Suyu log times count from when the core starts; the run's, from when the script was
launched (a few seconds earlier): --shift corrects that difference (default 0).

  python tools/port/nnshim/field_report.py <session> [<session2> ...]
"""
from __future__ import annotations

import argparse
import re
import statistics
from pathlib import Path

PERF_RE = re.compile(r"^\[\s*([\d.]+)\].*PERF game_fps=([\d.]+)")
DT_RE = re.compile(r"^\[\s*([\d.]+)\].*DT frames=(\d+) fps=([\d.]+) .*game/real=([\d.]+) clamped=(\d+)")
PHASE_RE = re.compile(r" t=\s*([\d.]+) phase (\S+)")


def load(session: Path, shift: float):
    perf, dt = [], []
    for line in (session / "suyu_log.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        if m := PERF_RE.match(line):
            perf.append((float(m.group(1)), float(m.group(2))))
        elif m := DT_RE.match(line):
            dt.append((float(m.group(1)), int(m.group(2)), float(m.group(3)), float(m.group(4)), int(m.group(5))))
    phases = []
    drives = sorted(session.glob("drive-*.txt"))
    if drives:
        for line in drives[0].read_text(encoding="utf-8").splitlines():
            if m := PHASE_RE.search(line):
                phases.append((float(m.group(1)) + shift, m.group(2)))
    return perf, dt, phases


def report(session: Path, shift: float) -> None:
    perf, dt, phases = load(session, shift)
    print(f"== {session}")
    if not phases:
        phases = [(0.0, "all")]
    end = max([p[0] for p in perf] + [phases[-1][0] + 1])
    bounds = [(phases[i][1], phases[i][0], phases[i + 1][0] if i + 1 < len(phases) else end) for i in range(len(phases))]
    print(f"{'phase':<16}{'from':>7}{'to':>7}{'fps_avg':>9}{'fps_min':>9}{'fps_max':>9}{'game/real':>12}{'clamped':>9}")
    for name, a, b in bounds:
        f = [v for t, v in perf if a <= t < b and v > 0]
        d = [x for x in dt if a <= x[0] < b]
        frames = sum(x[1] for x in d)
        # ratio weighted by frames (each DT line is a ~5 s window)
        ratio = sum(x[1] * x[3] for x in d) / frames if frames else float("nan")
        clamped = sum(x[4] for x in d)
        if f:
            print(f"{name:<16}{a:7.0f}{b:7.0f}{statistics.mean(f):9.1f}{min(f):9.1f}{max(f):9.1f}{ratio:12.4f}{clamped:9d}")
        else:
            print(f"{name:<16}{a:7.0f}{b:7.0f}{'-':>9}{'-':>9}{'-':>9}{ratio:12.4f}{clamped:9d}")
    if dt:
        frames = sum(x[1] for x in dt)
        print(f"total DT: {len(dt)} windows, {frames} frames, weighted game/real = "
              f"{sum(x[1] * x[3] for x in dt) / frames:.4f}, clamped = {sum(x[4] for x in dt)}")
    else:
        print("no DT lines (is frame_dt_probe installed?)")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("sessions", nargs="+", type=Path)
    ap.add_argument("--shift", type=float, default=0.0)
    args = ap.parse_args(argv)
    for s in args.sessions:
        report(s, args.shift)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
