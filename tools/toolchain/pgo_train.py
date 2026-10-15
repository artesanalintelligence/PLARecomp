#!/usr/bin/env python3
"""PGO profile for the recompiled modules: instrumented builds, merge, and the manifest the launcher checks.

The release ships one sparse clang profile (build/pgo/<launcher profile>.profdata) with a manifest
(<launcher profile>.pgo.json) that pins what it was trained on: the exporter binary, the emitter options and the game
build. The launcher's compile phase uses it only when all three match (tools/launcher/launcher.py pgo_choice). So the
profile MUST be retrained whenever the exporter or the emitter options change. Developer machine only: instrumented
images link the VS CRT and LLVM's profile runtime (VS Build Tools + LLVM 23), which the portable toolchain lacks.

Retraining (docs/port/LAUNCHER.md §8.12):
  1. A normal build of the current export (launcher compile phase or build_modules.py --export <pkg> --out <build>).
  2. Instrumented images, at most ~24 big units per image (Windows refuses images >= 2 GiB, error 193); main in
     batches by hotness, the other modules whole:
       pgo_train.py instrument --export <pkg> --build <build> --module main --units-file hot-1-24.txt --out <dir>\\a
       pgo_train.py instrument --export <pkg> --build <build> --module sdk --module subsdk0 --module subsdk1 --out <dir>\\c
     Each <out>\\img holds the instrumented recompiled_*.dll: copy them over a game folder's DLLs.
  3. Training: run that game folder on representative scenes with LLVM_PROFILE_FILE=<dir>\\<scene>.profraw (and
     optionally RECOMP_PGO_DUMP_S=20): every instrumented image writes <file>.<image>.0/.1 while it runs.
  4. pgo_train.py merge --out <dir>\\all.profdata <newest dump of every image and scene...>
  5. pgo_train.py manifest --profdata <dir>\\all.profdata --exporter <fork>\\build\\aot_export\\aot_export.exe
       --out-dir D:\\PLARecomp\\pgo --coverage "main hot 1-96, sdk, subsdk0, subsdk1; town + field"
     then tools/installer/make_release.py ships it (it refuses a profile whose manifest does not match).
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
LLVM = Path(os.environ.get("LLVM_DIR", r"C:\Program Files\LLVM"))


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def subset_module(src: Path, out: Path, units: list[str]) -> Path:
    """Module dir with only these generated units (hard links; the CMakeLists source list rewritten)."""
    (out / "src").mkdir(parents=True, exist_ok=True)
    for p in src.iterdir():
        if p.is_file() and p.suffix in (".h", ".c") and not (out / p.name).exists():
            os.link(p, out / p.name)
    for u in units:
        if not (out / "src" / u).exists():
            os.link(src / "src" / u, out / "src" / u)
    cm = (src / "CMakeLists.txt").read_text(encoding="utf-8")
    cm = re.sub(r"set\(RECOMP_SOURCES\s+.*?\)", "set(RECOMP_SOURCES\n" + "\n".join("src/" + u for u in units) + ")",
                cm, count=1, flags=re.S)
    (out / "CMakeLists.txt").write_text(cm, encoding="utf-8")
    return out


def profile_link_libs() -> list[str]:
    msvc = sorted(glob.glob(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\lib\x64"))
    sdk = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\Lib\10.*"))
    rt = sorted(LLVM.glob("lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib"))
    if not (msvc and sdk and rt):
        raise SystemExit("instrumented images need VS Build Tools (MSVC CRT), the Windows SDK and LLVM's profile runtime")
    # The runtime by full path: a libpath search may find an older clang_rt.profile in the MSVC lib dir first,
    # whose raw profile version does not match the compiler's instrumentation.
    return ["/libpath:" + msvc[-1], "/libpath:" + sdk[-1] + r"\ucrt\x64", "/libpath:" + sdk[-1] + r"\um\x64",
            "/nodefaultlib:libcmt", str(rt[-1]), "msvcrt.lib", "vcruntime.lib", "ucrt.lib", "oldnames.lib", "kernel32.lib"]


def relink(base: Path, inst: Path, out_dll: Path, dump_obj: Path, tc: Path) -> None:
    """The normal build's link.rsp with the instrumented units' objects in place of theirs and the profile runtime
    instead of the portable CRT startup."""
    st = json.loads((inst / "state.json").read_text(encoding="utf-8"))
    rep = {}
    for k, v in st.items():
        if k.startswith("split:"):
            objs = [inst / "obj" / (n + ".obj") for n in v["parts"]]
            if objs and all(o.exists() for o in objs):
                rep[Path(k[6:]).stem] = objs
    for o in (inst / "obj").glob("src_recompiled_*.c.obj"):
        unit = o.name[len("src_"):-len(".c.obj")]
        if o.name in st and unit not in rep:
            rep[unit] = [o]
    tlib = str(tc / "lib").lower()
    new, done = [], set()
    for line in (base / "link.rsp").read_text(encoding="utf-8").splitlines():
        x = line.strip('"')
        m = re.search(r"\\obj\\(?:src_)?(recompiled_[a-z0-9]+(?:_\d+)?)(?:__p\d+\.obj|\.c\.obj)$", x)
        unit = m.group(1) if m else None
        if unit in rep:
            if unit not in done:
                new += [str(o) for o in rep[unit]]
                done.add(unit)
            continue
        if x.startswith("/out:"):
            x = "/out:" + str(out_dll)
        elif x.startswith("/implib:"):
            x = "/implib:" + str(out_dll.with_suffix(".lib"))
        elif x.startswith("/nodefaultlib") or x.startswith("/entry:") or x.lower().endswith("recomp_crt0.obj") \
                or (x.lower().endswith(".lib") and tlib in x.lower()):
            continue
        new.append(x)
    if set(rep) - done:
        raise SystemExit(f"units not in {base / 'link.rsp'}: {sorted(set(rep) - done)}")
    new += profile_link_libs() + [str(dump_obj)]
    out_dll.parent.mkdir(parents=True, exist_ok=True)
    rsp = out_dll.with_suffix(".rsp")
    rsp.write_text("\n".join(f'"{x}"' if " " in x else x for x in new), encoding="utf-8")
    r = subprocess.run([str(tc / "bin/lld-link.exe"), "@" + str(rsp)], capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise SystemExit(f"link of {out_dll.name} failed:\n{(r.stdout + r.stderr)[-2000:]}")
    gb = out_dll.stat().st_size / 2**30
    print(f"{out_dll}: {len(done)} instrumented units, {gb:.2f} GiB" + ("  WARNING: >= 2 GiB, Windows will refuse it" if gb >= 2 else ""))


def cmd_instrument(a) -> int:
    tc = a.toolchain.resolve()
    exefs = next(d for d in a.export.rglob("exefs") if d.is_dir() and d.parent.name == "aot_cache")
    out = a.out.resolve()
    dump_obj = out / "pgo_dump.obj"
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(LLVM / "bin/clang-cl.exe"), "/nologo", "/c", "/O2", "/MD", str(HERE / "pgo_dump.c"), f"/Fo{dump_obj}"],
                   check=True)
    dirs = []
    for m in a.module:
        src = exefs / m
        if a.units_file:  # a batch of this module's units (main)
            units = [Path(l.split(":")[0].strip()).name for l in a.units_file.read_text().splitlines() if l.strip()]
            dirs.append(subset_module(src, out / "src" / m, units))
        else:
            dirs.append(src)
    argv = [sys.executable, str(HERE / "build_modules.py"), "--out", str(out / "obj"), "--toolchain", str(tc), "--no-link",
            "--split-mb", str(a.split_mb), "--unit-flags=-fprofile-instr-generate"]
    for d in dirs:
        argv += ["--module-dir", str(d)]
    if subprocess.run(argv).returncode != 0:
        return 1
    for m, d in zip(a.module, dirs):
        dll = "recompiled_image.dll" if m == "main" else f"recompiled_{m}.dll"
        relink(a.build / m, out / "obj" / d.name, out / "img" / dll, dump_obj, tc)
    return 0


def cmd_merge(a) -> int:
    tool = LLVM / "bin/llvm-profdata.exe"
    r = subprocess.run([str(tool), "merge", "--sparse", "-o", str(a.out)] + [str(p) for p in a.inputs])
    if r.returncode == 0:
        print(f"{a.out}: {a.out.stat().st_size / 2**20:.1f} MB (sparse)")
    return r.returncode


def emitter_key(profile: dict) -> dict:
    """The emitter options as the launcher compares them (tools/launcher/launcher.py pgo_choice)."""
    em = profile["emitter"]
    return {"backend": em["backend"], "shl_imm": bool(em.get("shl_imm")),
            "env": {k: v for k, v in sorted(em["env"].items()) if v not in ("", "0")}}


def build_ids(profile: dict) -> dict:
    return {m: v["build_id"] for m, v in sorted(profile["expected_exefs"].items()) if "build_id" in v}


def cmd_manifest(a) -> int:
    profile = json.loads((REPO / "tools/launcher/profiles" / f"{a.profile}.json").read_text(encoding="utf-8"))
    a.out_dir.mkdir(parents=True, exist_ok=True)
    dst = a.out_dir / f"{a.profile}.profdata"
    if a.profdata.resolve() != dst.resolve():
        shutil.copy2(a.profdata, dst)
    doc = {"format": 1, "profile": a.profile, "profdata": dst.name, "profdata_sha256": sha256(dst),
           "exporter_sha256": sha256(a.exporter), "emitter": emitter_key(profile), "build_ids": build_ids(profile),
           "split_mb": a.split_mb, "coverage": a.coverage, "date": time.strftime("%Y-%m-%d %H:%M"),
           "content": "clang PGO counters of the recompiled code, keyed by generated function names "
                      "(blk_<module>_<address>, runtime helpers); no game code or data"}
    (a.out_dir / f"{a.profile}.pgo.json").write_text(json.dumps(doc, indent=1), encoding="utf-8")
    print(f"{a.out_dir / (a.profile + '.pgo.json')}: profile {doc['profdata_sha256'][:12]}, exporter {doc['exporter_sha256'][:12]}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("instrument", help="instrumented images of some units/modules (training builds)")
    i.add_argument("--export", type=Path, required=True, help="the export folder (export\\pkg) the normal build came from")
    i.add_argument("--build", type=Path, required=True, help="the normal build folder (<module>\\link.rsp + obj)")
    i.add_argument("--module", action="append", required=True, help="module to instrument (repeatable)")
    i.add_argument("--units-file", type=Path, help="only these units of the (single) module, one file name per line")
    i.add_argument("--out", type=Path, required=True)
    i.add_argument("--toolchain", type=Path, default=Path(r"D:\PLARecomp\toolchain-portable\recomp-toolchain"))
    i.add_argument("--split-mb", type=float, default=8.0, help="must equal the install's (launcher --split-mb)")
    m = sub.add_parser("merge", help="merge raw dumps into one sparse profile")
    m.add_argument("--out", type=Path, required=True)
    m.add_argument("inputs", nargs="+", type=Path)
    f = sub.add_parser("manifest", help="install the profile with its manifest (what the launcher checks)")
    f.add_argument("--profdata", type=Path, required=True)
    f.add_argument("--exporter", type=Path, required=True, help="the aot_export.exe the training export was made with")
    f.add_argument("--profile", default="pla-1.1.1", help="launcher profile (emitter options, game build)")
    f.add_argument("--out-dir", type=Path, default=Path(r"D:\PLARecomp\pgo"))
    f.add_argument("--split-mb", type=float, default=8.0)
    f.add_argument("--coverage", default="", help="free text: units/modules and scenes trained")
    a = ap.parse_args()
    if a.cmd == "instrument" and a.units_file and len(a.module) != 1:
        ap.error("--units-file takes exactly one --module")
    return {"instrument": cmd_instrument, "merge": cmd_merge, "manifest": cmd_manifest}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
