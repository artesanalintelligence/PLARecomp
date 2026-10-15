#!/usr/bin/env python3
"""Builds the test ELFs with devkitA64 (aarch64-none-elf-g++).

Variants (all -fPIC -frtti, linked as a shared object at base 0):
  default   default visibility: typeinfo/vtables referenced with
            R_AARCH64_ABS64 against symbols; __cxxabiv1 imported.
  hidden    -fvisibility=hidden: internal references via R_AARCH64_RELATIVE,
            __cxxabiv1 vtables imported (like the Switch main, where
            libc++ lives in the sdk module).
  static    hidden + libsupc++ statically linked with --exclude-libs,ALL:
            the __cxxabiv1 vtables are local and nameless once the
            symbols are stripped (tests the heuristic classification).
  nortti    hidden + -fno-rtti: vtables without typeinfo (like the game's
            own code, where only the libraries have RTTI).
  eh        hidden + -fexceptions: LSDA with landing pads (fixtures/exceptions.cpp).
For each variant <v>.elf (with .symtab, serves as ground truth) and
<v>.stripped.elf (without .symtab or extra section headers) are generated.

Usage: python tools/analysis/tests/build_fixtures.py [output_directory]
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "fixtures" / "hierarchy.cpp"
SWITCH_SRC = HERE / "fixtures" / "switches.cpp"
CLANG_JT_SRC = HERE / "fixtures" / "clang_jumptables.s"
EH_SRC = HERE / "fixtures" / "exceptions.cpp"

COMMON = ["-O2", "-fPIC", "-frtti", "-fno-exceptions", "-funwind-tables", "-shared",
          "-Wl,--eh-frame-hdr", "-Wl,--build-id=none"]
VARIANTS = {
    "default": ["-nostdlib"],
    "hidden": ["-nostdlib", "-fvisibility=hidden"],
    "static": ["-nostartfiles", "-fvisibility=hidden", "-Wl,--exclude-libs,ALL", "-Wl,-Bstatic", "-lsupc++"],
    "nortti": ["-nostdlib", "-fvisibility=hidden", "-fno-rtti"],
    "eh": ["-nostdlib", "-fvisibility=hidden", "-fexceptions"],
}


def find_tool(name: str) -> str | None:
    exe = name + (".exe" if os.name == "nt" else "")
    for root in (os.environ.get("DEVKITPRO"), "C:/devkitPro", "/opt/devkitpro"):
        if root:
            p = Path(root) / "devkitA64" / "bin" / exe
            if p.exists():
                return str(p)
    return shutil.which(name)


def build(outdir: Path) -> dict[str, Path]:
    gxx = find_tool("aarch64-none-elf-g++")
    strip = find_tool("aarch64-none-elf-strip")
    if not gxx or not strip:
        raise FileNotFoundError("aarch64-none-elf-g++ not found (devkitA64)")
    outdir.mkdir(parents=True, exist_ok=True)
    out = {}
    for name, extra in VARIANTS.items():
        elf = outdir / f"{name}.elf"
        srcs = [str(SRC), str(SWITCH_SRC), str(CLANG_JT_SRC), str(EH_SRC)]
        # Libraries must come after the sources
        flags = [f for f in extra if not f.startswith("-l") and not f.startswith("-Wl,-B")]
        libs = [f for f in extra if f.startswith("-l") or f.startswith("-Wl,-B")]
        cmd = [gxx, *COMMON, *flags, "-o", str(elf), *srcs, *libs]
        subprocess.run(cmd, check=True)
        stripped = outdir / f"{name}.stripped.elf"
        subprocess.run([strip, "--strip-all", "-o", str(stripped), str(elf)], check=True)
        out[name] = elf
        out[name + ".stripped"] = stripped
    return out


if __name__ == "__main__":
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "build"
    for k, v in build(d).items():
        print(f"{k:16s} {v}")
