"""Exports Ghidra pseudo-C to tools/ghidra/out/decomp/<dir>.c (batch or on demand).

Uses a private COPY of the project (tools/ghidra/projects/decomp_copy) so as not to
lock the main project, which other tools use (analyze.py, probes).

  python tools/decomp/ghidra_decomp.py --all [--threads 2]      # full batch, resumable
  python tools/decomp/ghidra_decomp.py --addr 0x7100123456 ...  # only those (starts Ghidra: ~1-3 min)
  python tools/decomp/ghidra_decomp.py --status                  # batch progress
  python tools/decomp/ghidra_decomp.py --stop                    # asks the server to finish

While a batch runs, `ctx.py` leaves requests in out/decomp/_requests/ and the batch
serves them first (in seconds). Batch order: game functions first
(data/triage.csv if present; otherwise a heuristic), then the rest.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (DECOMP_OUT, ROOT, TOOLS, functions, is_decomp_target, load_json, parse_addr)  # noqa: E402

GHIDRA = TOOLS / "ghidra"
MAIN_PROJ = GHIDRA / "projects"
COPY_PROJ = GHIDRA / "projects" / "decomp_copy"
SCRIPTS = GHIDRA / "scripts"
HEARTBEAT = DECOMP_OUT / "_server.json"
REQ_DIR = DECOMP_OUT / "_requests"


def toolpaths() -> dict:
    return json.loads((TOOLS / "bin" / "toolpaths.json").read_text(encoding="utf-8"))


def server_alive(max_age: float = 30.0) -> dict | None:
    hb = load_json(HEARTBEAT)
    if hb and time.time() - hb.get("time", 0) < max_age:
        return hb
    return None


def request(addrs: list[int], wait: float = 0.0) -> bool:
    """Queues requests for the active server; waits up to `wait` s for the .c files to exist."""
    REQ_DIR.mkdir(parents=True, exist_ok=True)
    p = REQ_DIR / f"{os.getpid()}_{time.time_ns()}.req"
    tmp = p.with_suffix(".tmp")
    tmp.write_text("\n".join(f"{a:x}" for a in addrs) + "\n", encoding="utf-8")
    os.replace(tmp, p)
    t0 = time.time()
    while time.time() - t0 < wait:
        if all((DECOMP_OUT / f"{a:x}.c").exists() or (DECOMP_OUT / f"{a:x}.err").exists() for a in addrs):
            return True
        time.sleep(0.5)
    return all((DECOMP_OUT / f"{a:x}.c").exists() for a in addrs)


def ensure_copy(refresh: bool = False):
    src_gpr = MAIN_PROJ / "main.gpr"
    if refresh and COPY_PROJ.exists():
        shutil.rmtree(COPY_PROJ)
    if not (COPY_PROJ / "main.gpr").exists():
        COPY_PROJ.mkdir(parents=True, exist_ok=True)
        print(f"[ghidra_decomp] copying the project to {COPY_PROJ} ...")
        shutil.copy2(src_gpr, COPY_PROJ / "main.gpr")
        shutil.copytree(MAIN_PROJ / "main.rep", COPY_PROJ / "main.rep")


def write_order(path: Path):
    idx = functions()
    first, rest = [], []
    for f in idx.funcs:
        if f.size <= 0:
            continue
        (first if is_decomp_target(f) else rest).append(f.addr)
    # First the moderately sized game ones (those that will be decompiled first).
    first.sort(key=lambda a: (idx.at(a).size > 2000, a))
    path.write_text("\n".join(f"{a:x}" for a in first + rest) + "\n", encoding="utf-8")
    return len(first), len(rest)


def run_headless(order_arg: str, threads: int, timeout: int, serve: bool, maxmem: str, background: bool):
    tp = toolpaths()
    gdir = Path(tp["ghidra_dir"])
    env = dict(os.environ)
    env["JAVA_HOME"] = tp["java_home"]
    env["PATH"] = str(Path(tp["java_home"]) / "bin") + os.pathsep + env.get("PATH", "")
    env["GHIDRA_HEADLESS_MAXMEM"] = maxmem
    DECOMP_OUT.mkdir(parents=True, exist_ok=True)
    cmd = (["cmd", "/c", str(gdir / "support" / "analyzeHeadless.bat")] if os.name == "nt"
           else [str(gdir / "support" / "analyzeHeadless")])
    cmd += [str(COPY_PROJ), "main", "-process", "main.elf", "-noanalysis", "-readOnly",
            "-scriptPath", str(SCRIPTS),
            "-postScript", "ExportDecompiled.java", str(DECOMP_OUT), order_arg, str(threads), str(timeout)]
    if serve:
        cmd.append("serve")
    log = DECOMP_OUT.parent / "decomp_headless.log"
    cmd += ["-log", str(log), "-scriptlog", str(DECOMP_OUT.parent / "decomp_scripts.log")]
    print("[ghidra_decomp]", " ".join(cmd))
    if background:
        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NEW_PROCESS_GROUP | 0x00004000  # BELOW_NORMAL_PRIORITY_CLASS
        with open(DECOMP_OUT.parent / "decomp_stdout.log", "ab") as out:
            p = subprocess.Popen(cmd, env=env, stdout=out, stderr=subprocess.STDOUT, cwd=ROOT, creationflags=flags)
        print(f"[ghidra_decomp] launched in the background (pid {p.pid}); progress: --status")
        return 0
    return subprocess.call(cmd, env=env, cwd=ROOT)


def status():
    n_c = n_err = 0
    if DECOMP_OUT.exists():
        with os.scandir(DECOMP_OUT) as it:
            for e in it:
                if e.name.endswith(".c"):
                    n_c += 1
                elif e.name.endswith(".err"):
                    n_err += 1
    total = len(functions().funcs)
    hb = server_alive()
    print(f"cached pseudo-C: {n_c} / {total} functions ({100 * n_c / max(total, 1):.1f} %), errors: {n_err}")
    print("server:", json.dumps(hb) if hb else "no active batch")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--all", action="store_true", help="all functions (priority order)")
    g.add_argument("--addr", nargs="+", help="specific addresses or names")
    g.add_argument("--serve", action="store_true", help="only serve requests (no batch)")
    g.add_argument("--status", action="store_true")
    g.add_argument("--stop", action="store_true")
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--timeout", type=int, default=60, help="seconds per function")
    ap.add_argument("--maxmem", default="12G")
    ap.add_argument("--background", action="store_true", help="do not wait (batch in the background)")
    ap.add_argument("--keep-serving", action="store_true", help="with --all: keep serving requests when done")
    ap.add_argument("--refresh-copy", action="store_true", help="copy the main project again")
    a = ap.parse_args(argv)

    if a.status:
        status()
        return 0
    if a.stop:
        DECOMP_OUT.mkdir(parents=True, exist_ok=True)
        (DECOMP_OUT / "_stop").write_text("stop\n")
        print("server stop requested")
        return 0
    hb = server_alive()
    if a.addr:
        idx = functions()
        addrs = []
        for s in a.addr:
            f = idx.resolve(s)
            addrs.append(f.addr if f else parse_addr(s))
        if hb:
            ok = request(addrs, wait=300)
            print("done" if ok else "the server did not respond in time")
            return 0 if ok else 1
        ensure_copy(a.refresh_copy)
        lst = DECOMP_OUT.parent / f"decomp_req_{os.getpid()}.txt"
        DECOMP_OUT.mkdir(parents=True, exist_ok=True)
        lst.write_text("\n".join(f"{x:x}" for x in addrs) + "\n")
        rc = run_headless(str(lst), min(a.threads, len(addrs)), a.timeout, False, a.maxmem, False)
        lst.unlink(missing_ok=True)
        return rc
    if hb:
        print(f"a server is already active: {hb}")
        return 1
    ensure_copy(a.refresh_copy)
    if a.serve:
        return run_headless("none", a.threads, a.timeout, True, a.maxmem, a.background)
    order = DECOMP_OUT.parent / "decomp_order.txt"
    DECOMP_OUT.mkdir(parents=True, exist_ok=True)
    n1, n2 = write_order(order)
    print(f"[ghidra_decomp] order: {n1} game functions first, {n2} after")
    return run_headless(str(order), a.threads, a.timeout, a.keep_serving, a.maxmem, a.background)


if __name__ == "__main__":
    sys.exit(main())
