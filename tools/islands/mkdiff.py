#!/usr/bin/env python3
"""mkdiff.py - builds isl_diff.exe, the offline differential tester of the skeleton island (FPS4-ISLANDS).

The recompiled original (the block functions of the shipped export for main+0x2A697AC and its TRS helpers) and the
native island run on the same randomly generated skeleton objects in one process; world matrices, dirty bits and
every other touched guest page (except the stack) are compared byte by byte.

usage: python mkdiff.py <export main dir (recomp_runtime.h, src/)> <out dir> [--run N]
"""
import os
import re
import subprocess
import sys
from pathlib import Path

CLANG = r"D:\PLARecomp\toolchain-portable\recomp-toolchain\bin\clang-cl.exe"
SYSCLANG = r"C:\Program Files\LLVM\bin\clang-cl.exe"
UNIT_FLAGS = ["/nologo", "/X", "/DWIN32", "/D_WINDOWS", "/W0", "/MD", "/O2", "/Ob2", "/DNDEBUG", "/std:c11",
              "/bigobj", "/fp:precise", "-DSUYU_HOSTED_RECOMP=1", "-DRECOMP_SHARED_MODULE=1", "/c"]
HERE = Path(__file__).resolve().parent
RANGES = [(0x2A697AC, 0x2A6A984), (0x2ACB330, 0x2ACB50C), (0x2ACB6F0, 0x2ACB978), (0x29C98BC, 0x29C995C), (0x2A6BAF4, 0x2A6BB60), (0x8C6B1C, 0x8C7000), (0xBB8D18, 0xBB9F80), (0x2A3781C, 0x2A37A1C)]
BLK = re.compile(r"^void blk_main_([0-9a-f]{16})\(GuestContext\* c\)\{", re.M)


def slice_units(src_dir, out):
    want = {}
    names = []
    for f in sorted(Path(src_dir).glob("recompiled_main_*.c")):
        s = f.read_text(encoding="utf-8", errors="replace")
        hits = [m for m in BLK.finditer(s) if any(a <= int(m.group(1), 16) < b for a, b in RANGES)]
        if not hits:
            continue
        starts = [m.start() for m in BLK.finditer(s)]
        seg = s.find("\nconst struct _recomp_ent _seg_")
        body = []
        for m in hits:
            i = starts.index(m.start())
            end = starts[i + 1] if i + 1 < len(starts) else seg
            body.append(s[m.start():end])
            names.append(int(m.group(1), 16))
        o = out / f"slice_{f.stem}.c"
        o.write_text(s[:starts[0]] + "".join(body), encoding="utf-8", newline="\n")
        want[o] = len(hits)
    return want, names


def main():
    mod, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    units, blocks = slice_units(mod / "src", out)
    print("slices:", {k.name: v for k, v in units.items()}, file=sys.stderr)
    tbl = "".join(f"  {{0x{a:x}ULL, blk_main_{a:016x}}},\n" for a in sorted(blocks))
    decl = "".join(f"void blk_main_{a:016x}(GuestContext* c);\n" for a in sorted(blocks))
    (out / "blocks.inc").write_text('extern "C" {\n' + decl + "}\nstatic const struct { uint64_t pc; BlockFn fn; } "
                                    "kBlocks[] = {\n" + tbl + "};\n", encoding="utf-8")
    rt = (mod / "recomp_runtime.c").read_text(encoding="utf-8", errors="replace")
    i = rt.index("uint64_t recomp_fp_estimate(GuestContext* c,")
    j = rt.index("\n}\n", i) + 3
    (out / "fpest.inc").write_text(rt[i:j], encoding="utf-8")
    env = dict(os.environ)
    env["INCLUDE"] = ""
    tc_inc = r"D:\PLARecomp\toolchain-portable\recomp-toolchain\include"
    objs = []
    procs = []
    for u in units:
        o = u.with_suffix(".obj")
        objs.append(o)
        procs.append(subprocess.Popen([CLANG] + UNIT_FLAGS + ["-I" + str(mod), "-I" + tc_inc, str(u), "/Fo" + str(o)],
                                      env=env))
    for p in procs:
        if p.wait():
            sys.exit("unit compile failed")
    vc = r'"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"'
    cmd = (f'call {vc} >nul 2>&1 && "{SYSCLANG}" /nologo /O2 /MD /EHsc /std:c++20 -D_CRT_SECURE_NO_WARNINGS -msse4.1 '
           f'-mfma -mavx -fno-strict-aliasing -Wno-everything -I"{mod}" -I"{out}" -I"{HERE}" '
           f'-I"{HERE.parent.parent / "recomp" / "mods" / "include"}" '
           f'"{HERE / "isl_diff.cpp"}" ' + " ".join(f'"{o}"' for o in objs) +
           f' /Fe"{out / "isl_diff.exe"}" /link /STACK:16777216 kernel32.lib')
    r = subprocess.run(cmd, shell=True)
    if r.returncode:
        sys.exit("link failed")
    print("built", out / "isl_diff.exe", file=sys.stderr)


if __name__ == "__main__":
    main()
