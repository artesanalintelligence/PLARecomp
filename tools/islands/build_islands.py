#!/usr/bin/env python3
"""Builds the islands mod (recomp/mods/islands) on the user's machine: nlift.py lifts the selected functions of
the user's own main.elf into islands_gen.inc, and the portable toolchain (tools/toolchain: clang-cl, lld-link,
llvm-objdump, our headers and import libraries) compiles islands.cpp into islands.dll. The lifted code is a
translation of the game's code, so it is generated here and never shipped.

  python tools/islands/build_islands.py --elf <work>/data/main.elf --toolchain <recomp-toolchain> --out <mod dir>
      [--work <build dir>]

Writes <out>/islands.dll and <out>/mod.json (the repo's mod.json; config values already saved in an existing
<out>/mod.json are kept by the launcher, not here).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SRC = REPO / "recomp/mods/islands"
API_INCLUDE = REPO / "recomp/mods/include"
SOURCES = ["islands.cpp", "isl_core.h", "nl_rt.h"]
# Same code generation as the VS build of tools/islands/build.cmd; /Zc:tlsGuards-: every thread_local is
# constant-initialized, so no on-demand TLS initialization (which needs the CRT) is emitted.
CFLAGS = ["/nologo", "/X", "/O2", "/MD", "/std:c++20", "/EHs-c-", "/GR-", "/Zc:tlsGuards-", "-msse4.1", "-mfma", "-mavx",
          "-fno-strict-aliasing", "-Wno-unused-label", "-Wno-parentheses-equality"]


def run(cmd: list[str], env=None) -> None:
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env)
    if r.returncode != 0:
        raise SystemExit(f"failed ({r.returncode}): {' '.join(cmd)}\n{(r.stdout + r.stderr)[-3000:]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=Path, required=True, help="main.elf extracted from the user's dump (tools/extract)")
    ap.add_argument("--toolchain", type=Path, required=True, help="portable toolchain folder (recomp-toolchain)")
    ap.add_argument("--out", type=Path, required=True, help="mod folder to write islands.dll and mod.json into")
    ap.add_argument("--work", type=Path, help="build folder (default: <out>/../_islands_build)")
    a = ap.parse_args()
    tc = a.toolchain
    for req in ("bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-objdump.exe", "lib/recomp_crt0.obj",
                "lib/recomp_tls.obj", "include/windows.h"):
        if not (tc / req).is_file():
            raise SystemExit(f"portable toolchain incomplete: missing {tc / req}")
    if not a.elf.is_file():
        raise SystemExit(f"missing {a.elf}")
    work = a.work or a.out.parent / "_islands_build"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    # Sources and the generated include side by side (isl_core.h includes "islands_gen.inc" from its own folder).
    for s in SOURCES:
        shutil.copy2(SRC / s, work / s)
    env = dict(os.environ, NLIFT_OBJDUMP=str(tc / "bin/llvm-objdump.exe"))
    run([sys.executable, str(HERE / "nlift.py"), str(a.elf), str(work / "islands_gen.inc")], env)
    env = dict(os.environ, INCLUDE="")  # keep clang-cl away from an installed VS / Windows SDK
    run([str(tc / "bin/clang-cl.exe"), *CFLAGS, f"/I{tc / 'include'}", f"/I{API_INCLUDE}", "/c",
         str(work / "islands.cpp"), f"/Fo{work / 'islands.obj'}"], env)
    a.out.mkdir(parents=True, exist_ok=True)
    dll = work / "islands.dll"
    run([str(tc / "bin/lld-link.exe"), "/nologo", "/dll", "/machine:x64", "/INCREMENTAL:NO", "/Brepro", "/nodefaultlib",
         "/entry:_DllMainCRTStartup", f"/out:{dll}", f"/implib:{work / 'islands.lib'}",
         str(tc / "lib/recomp_crt0.obj"), str(tc / "lib/recomp_tls.obj"), str(work / "islands.obj"),
         *sorted(str(p) for p in (tc / "lib").glob("*.lib"))])
    shutil.copy2(dll, a.out / "islands.dll")
    shutil.copy2(SRC / "mod.json", a.out / "mod.json")
    print(f"islands.dll {dll.stat().st_size // 1024} KB -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
