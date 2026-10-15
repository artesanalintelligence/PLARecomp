#!/usr/bin/env python3
"""Analyzes an AArch64 ELF with headless Ghidra and exports functions, RTTI and strings.

Steps:
  1. tools/analysis/rtti.py  -> <out>/rtti.json   (pure Python, no Ghidra)
  2. analyzeHeadless: imports the ELF with the native ELF loader
     (processor AARCH64:LE:64:v8A, image base 0x7100000000 by default),
     runs the automatic analysis and then the tools/ghidra/scripts scripts:
        ApplyRttiNames.java      names of classes, typeinfo, vtables and Class::vfN
        ExportFunctionsCsv.java  <out>/ghidra_functions.csv (viking format) + .tsv
        ExportStringsXrefs.java  <out>/strings.tsv, <out>/imports.tsv
  3. (optional, --merge) tools/analysis/merge_functions.py -> data/main_functions.csv

Usage:
    python tools/ghidra/analyze.py data/main.elf                 # full analysis
    python tools/ghidra/analyze.py data/main.elf --reuse         # scripts only, existing project
    python tools/ghidra/analyze.py data/main.elf --merge         # and merge into data/main_functions.csv

Requires tools/ghidra/bootstrap.py to have been run (JDK 21 + Ghidra in tools/bin/).
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import functools

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
print = functools.partial(print, flush=True)  # noqa: A001  (correct ordering with Ghidra's output)
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TOOLPATHS = REPO / "tools" / "bin" / "toolpaths.json"
PROCESSOR = "AARCH64:LE:64:v8A"
DEFAULT_BASE = 0x71_0000_0000


def load_toolpaths() -> dict:
    if not TOOLPATHS.exists():
        raise SystemExit("tools/bin/toolpaths.json not found: run "
                         "python tools/ghidra/bootstrap.py first")
    return json.loads(TOOLPATHS.read_text(encoding="utf-8"))


def headless_cmd(ghidra_dir: Path) -> list[str]:
    if os.name == "nt":
        return ["cmd", "/c", str(ghidra_dir / "support" / "analyzeHeadless.bat")]
    return [str(ghidra_dir / "support" / "analyzeHeadless")]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", help="ELF to analyze (e.g. data/main.elf)")
    ap.add_argument("--base", default=f"{DEFAULT_BASE:#x}", help="image base (default 0x7100000000)")
    ap.add_argument("--project-dir", default=str(HERE / "projects"))
    ap.add_argument("--project-name", default=None, help="default: the ELF name without extension")
    ap.add_argument("--out-dir", default=str(HERE / "out"))
    ap.add_argument("--functions-csv", default=None,
                    help="Ghidra functions CSV (default <out>/ghidra_functions.csv)")
    ap.add_argument("--unnamed", choices=["empty", "fun"], default="empty",
                    help="name in the Ghidra CSV for FUN_xxx: empty (viking/botw) or FUN_xxx")
    ap.add_argument("--reuse", action="store_true",
                    help="do not reimport: run the scripts on the existing project (-process -noanalysis)")
    ap.add_argument("--no-rtti", action="store_true", help="do not run rtti.py or ApplyRttiNames")
    ap.add_argument("--maxmem", default=None, help="Java heap for Ghidra (e.g. 16G; default 4G, or 16G if the ELF > 16 MiB)")
    ap.add_argument("--timeout", type=int, default=0, help="-analysisTimeoutPerFile in seconds (0 = no limit)")
    ap.add_argument("--merge", action="store_true", help="run merge_functions.py at the end")
    ap.add_argument("--dry-run", action="store_true", help="only show the command")
    args = ap.parse_args(argv)

    elf = Path(args.elf).resolve()
    if not elf.exists():
        raise SystemExit(f"{elf} does not exist")
    out = Path(args.out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    proj_dir = Path(args.project_dir).resolve()
    proj_dir.mkdir(parents=True, exist_ok=True)
    proj_name = args.project_name or elf.stem
    funcs_csv = Path(args.functions_csv).resolve() if args.functions_csv else out / "ghidra_functions.csv"
    base = int(str(args.base), 0)

    tp = load_toolpaths()
    ghidra_dir = Path(tp["ghidra_dir"])
    env = os.environ.copy()
    env["JAVA_HOME"] = tp["java_home"]
    env["PATH"] = str(Path(tp["java_home"]) / "bin") + os.pathsep + env.get("PATH", "")
    maxmem = args.maxmem or ("16G" if elf.stat().st_size > 16 << 20 else "4G")
    env["GHIDRA_HEADLESS_MAXMEM"] = maxmem

    # 1. RTTI in pure Python
    rtti_json = out / "rtti.json"
    if not args.no_rtti:
        sys.path.insert(0, str(REPO / "tools" / "analysis"))
        from elfimage import ElfImage  # noqa: E402
        import rtti  # noqa: E402
        t0 = time.time()
        res = rtti.analyze(ElfImage(elf, base))
        rtti_json.write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")
        st = res["stats"]
        print(f"[rtti] {st['classes']} classes, {st['with_vtable']} with vtable, "
              f"{st['vtables_without_rtti']} vtables without RTTI ({time.time() - t0:.1f}s) -> {rtti_json}")

    # 2. Ghidra headless
    cmd = headless_cmd(ghidra_dir) + [str(proj_dir), proj_name]
    if args.reuse:
        cmd += ["-process", elf.name, "-noanalysis"]
    else:
        cmd += ["-import", str(elf), "-overwrite",
                "-processor", PROCESSOR,
                "-loader", "ElfLoader",
                "-loader-imagebase", f"{base:x}"]
        if args.timeout:
            cmd += ["-analysisTimeoutPerFile", str(args.timeout)]
    cmd += ["-scriptPath", str(HERE / "scripts")]
    if not args.no_rtti:
        cmd += ["-postScript", "ApplyRttiNames.java", str(rtti_json)]
    cmd += ["-postScript", "ExportFunctionsCsv.java", str(funcs_csv),
            str(funcs_csv.with_suffix(".tsv")), args.unnamed]
    cmd += ["-postScript", "ExportStringsXrefs.java", str(out)]
    cmd += ["-log", str(out / "ghidra_headless.log"), "-scriptlog", str(out / "ghidra_scripts.log")]
    print("[ghidra] " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    print(f"[ghidra] JAVA_HOME={env['JAVA_HOME']}  GHIDRA_HEADLESS_MAXMEM={maxmem}")
    if args.dry_run:
        return 0
    t0 = time.time()
    rc = subprocess.run(cmd, env=env).returncode
    print(f"[ghidra] finished with code {rc} in {time.time() - t0:.0f}s")
    if rc != 0:
        return rc
    if not funcs_csv.exists():
        print(f"[ghidra] ERROR: {funcs_csv} was not generated; check {out / 'ghidra_scripts.log'}")
        return 1

    # 3. Optional merge
    if args.merge:
        merge = [sys.executable, str(REPO / "tools" / "analysis" / "merge_functions.py"), str(elf),
                 "--base", hex(base), "--ghidra", str(funcs_csv), "--out-dir", str(out)]
        if not args.no_rtti:
            merge += ["--rtti", str(rtti_json)]
        rc = subprocess.run(merge).returncode
    return rc


if __name__ == "__main__":
    sys.exit(main())
