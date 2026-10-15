#!/usr/bin/env python3
"""Runs FramerateProbe.java on the already analyzed Ghidra project (read-only).

Usage:
    python tools/ghidra/probe.py <output.txt> dec:0x2783048 xref:0x397e8d0 dis:0x2aa278c:40
    python tools/ghidra/probe.py <output.txt> @commands.txt

Addresses are offsets into main (the 0x7100000000 image base is added automatically).
It neither reanalyzes nor saves the project (-noanalysis -readOnly).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    tp = json.loads((REPO / "tools" / "bin" / "toolpaths.json").read_text(encoding="utf-8"))
    ghidra = Path(tp["ghidra_dir"])
    env = dict(os.environ, JAVA_HOME=tp["java_home"], GHIDRA_HEADLESS_MAXMEM="16G")
    out = Path(sys.argv[1]).resolve()
    args = [a if not a.startswith("@") else "@" + str(Path(a[1:]).resolve()) for a in sys.argv[2:]]
    headless = ghidra / "support" / ("analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")
    cmd = (["cmd", "/c", str(headless)] if os.name == "nt" else [str(headless)]) + [
        str(HERE / "projects"), "main", "-process", "main.elf", "-noanalysis", "-readOnly",
        "-scriptPath", str(HERE / "scripts"),
        "-postScript", "FramerateProbe.java", str(out), *args,
    ]
    rc = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL if "-q" in os.environ.get("PROBE_FLAGS", "") else None).returncode
    print(f"[probe] code {rc} -> {out}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
