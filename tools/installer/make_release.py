#!/usr/bin/env python3
"""Builds the PLARecomp release: ONE self-contained PLARecompSetup.exe (setup program + all components).

Components (each a .tar.xz with LZMA2 + x86 BCJ, packed with tools/toolchain/pack.py as-is):
  python     official Python embeddable package from python.org + the modules the phases need (wheels from PyPI)
  scripts    the subset of this repo that the install phases run (tools/launcher, tools/extract, recomp/scripts...)
  platform   prebuilt platform binaries: the game's PLA.exe (plahost, tools/port/plahost: no emulator), nnshim, nvnvk,
             SDL2, prebuilt recomp mods; the headless exporter (aot_export.exe from the fork: install time only, never
             in the game folder; with --gui-exporter also the fork's suyu.exe + Qt as a fallback), the free fonts
             sources; third-party license texts in LICENSES/
  toolchain  the portable compiler package (tools/toolchain, docs/port/TOOLCHAIN.md)
  pgo        the trained PGO profile (build/pgo: sparse clang .profdata = execution counters of the recompiled code,
             keyed by generated function names, + the manifest the launcher checks); --no-pgo leaves it out

Nothing derived from the game goes in: no keys, dumps, generated C, recompiled DLLs, shader caches or assets.

Output (outside the repo, default D:\\PLARecomp\\release):
  PLARecompSetup.exe          full release (default): bare setup .exe + payload (components + index + footer)
  light\\PLARecompSetup.exe    with --light: index only; the components next to it are the download fallback
  light\\*.tar.xz, components.json
  plarecomp-exporter-source-<release>.tar.xz   GPL source of the shipped exporter (release asset next to the .exe)
  release.json                sizes and hashes

  python tools/installer/make_release.py [--out D:\\PLARecomp\\release] [--reuse] [--light] [--fast]
      [--url-base https://github.com/<owner>/PLARecomp/releases/download/<tag>/]
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import io
import json
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
PY_VERSION = "3.12.10"
PY_URL = f"https://www.python.org/ftp/python/{PY_VERSION}/python-{PY_VERSION}-embed-amd64.zip"
PY_SHA256 = "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3"  # python.org file, checked at download
PY_MODULES = ["lz4", "pycryptodome", "zstandard", "nsz", "Pillow", "numpy", "pyelftools", "capstone", "itanium_demangler"]
MAGIC = b"PLARSETU"
URL_PENDING = "https://github.com/OWNER/PLARecomp/releases/download/PENDING/"  # placeholder until a release page exists

# Repo files the install phases run (relative to the repo root). Directories are copied without __pycache__/tests.
SCRIPTS = [
    "tools/launcher/launcher.py", "tools/launcher/identify.py", "tools/launcher/requirements.py", "tools/launcher/profiles",
    "tools/extract/extract.py", "tools/extract/nso2elf.py", "tools/extract/nxfmt.py", "tools/extract/romfs_bktr.py",
    "tools/analysis/code_targets.py", "tools/analysis/coderefs.py", "tools/analysis/demangle.py", "tools/analysis/eh_frame.py",
    "tools/analysis/elfimage.py", "tools/analysis/jumptables.py", "tools/analysis/rtti.py",
    "tools/patch/apply_patches.py", "tools/patch/nsoimg.py",
    "tools/toolchain/build_modules.py", "tools/toolchain/split_units.py", "tools/toolchain/cpu_isa.py", "tools/toolchain/pack.py",
    "tools/port/nnshim/extract_suyu_fonts.py", "tools/port/nnshim/key_prompts",
    "recomp/scripts/env.ps1", "recomp/scripts/setup-portable-suyu.ps1", "recomp/scripts/stage-suyu-userdata.py",
    "recomp/scripts/export-pla.ps1", "recomp/scripts/check-baked-patches.py", "recomp/scripts/build-modules.ps1",
    "recomp/scripts/build-mod.ps1", "recomp/scripts/make-dist.ps1", "recomp/scripts/make-roots.py", "recomp/scripts/make-pla-exe.py",
    "recomp/patches/unlocked-fps", "recomp/mods/research_level_fix", "recomp/mods/battle_target_fix", "recomp/mods/include", "recomp/dist/README.txt",
    # islands is generated from the user's main.elf and compiled at install time (launcher.py build_local_mod):
    # its sources only, file by file (a local islands_gen.inc holds lifted game code and must never ship).
    "tools/islands/nlift.py", "tools/islands/build_islands.py", "recomp/mods/islands/islands.cpp",
    "recomp/mods/islands/isl_core.h", "recomp/mods/islands/nl_rt.h", "recomp/mods/islands/mod.json",
]
# Prebuilt recomp mods shipped in the platform package (build/mods-recomp/<id>): id -> folder under --mods-build with
# the DLL; the mod.json (hooks + default config) comes from recomp/mods/<id>/mod.json.
PREBUILT_MODS = {"research_level_fix": "research_level_fix", "battle_target_fix": "battle_target_fix",
                 "engine_options": "engine_options-en", "render_res": "render_res-en"}
FORK_SCRIPTS = ["install-nsp.py", "nsp-extract.py", "mcp-call.py", "mcp.py"]
FONT_SOURCES = ["font_standard.cpp", "font_chinese_simplified.cpp", "font_extended_chinese_simplified.cpp",
                "font_chinese_traditional.cpp", "font_korean.cpp", "font_nintendo_extended.cpp"]
SKIP_EXT = {".pdb", ".map", ".ilk", ".exp", ".lib", ".obj", ".pyc"}


def log(s: str) -> None:
    print(s, flush=True)


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def tree_size(d: Path) -> int:
    return sum(p.stat().st_size for p in d.rglob("*") if p.is_file())


def copy(src: Path, dst: Path) -> None:
    if src.is_dir():
        for p in src.rglob("*"):
            if p.is_file() and "__pycache__" not in p.parts and "tests" not in p.relative_to(src).parts and p.suffix not in SKIP_EXT:
                (dst / p.relative_to(src)).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dst / p.relative_to(src))
    else:
        if not src.is_file():
            raise SystemExit(f"missing input: {src}")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def packer():
    spec = importlib.util.spec_from_file_location("pack", REPO / "tools/toolchain/pack.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    if FAST:
        m.FILTERS = [{"id": lzma.FILTER_X86}, {"id": lzma.FILTER_LZMA2, "preset": 6}]
    return m


# ---------------------------------------------------------------- components
def stage_python(work: Path, cache: Path) -> Path:
    z = cache / Path(PY_URL).name
    if not z.is_file():
        log(f"downloading {PY_URL}")
        urllib.request.urlretrieve(PY_URL, z)
    if sha256(z) != PY_SHA256:
        raise SystemExit(f"{z}: SHA-256 does not match the pinned python.org hash")
    d = work / "python"
    shutil.rmtree(d, ignore_errors=True)
    with zipfile.ZipFile(z) as f:
        f.extractall(d)
    ver = "".join(PY_VERSION.split(".")[:2])
    # ._pth: isolated (no registry/PYTHONPATH), with site-packages and site enabled.
    (d / f"python{ver}._pth").write_text(f"python{ver}.zip\n.\nLib\\site-packages\nimport site\n", encoding="ascii")
    sp = d / "Lib/site-packages"
    sp.mkdir(parents=True)
    log("installing modules: " + ", ".join(PY_MODULES))
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--disable-pip-version-check", "--target", str(sp),
                    "--platform", "win_amd64", "--python-version", ver[0] + "." + ver[1:], "--implementation", "cp",
                    "--abi", "cp" + ver, "--only-binary=:all:", *PY_MODULES], check=True)
    for junk in ("bin", "benchmarks", "Crypto/SelfTest"):
        shutil.rmtree(sp / junk, ignore_errors=True)
    for p in list(sp.rglob("__pycache__")):
        shutil.rmtree(p, ignore_errors=True)
    # With a ._pth file Python does not add the script's folder to sys.path; the scripts import their siblings.
    (sp / "_plarecomp_script_dir.pth").write_text(
        "import sys, os, site; a = sys.argv[0] if sys.argv else ''; "
        "(a and a not in ('-c', '-m') and os.path.isfile(a)) and sys.path.insert(0, os.path.dirname(os.path.abspath(a))); "
        "u = site.getusersitepackages(); u in sys.path and sys.path.remove(u)\n", encoding="ascii")
    # "python -m nsz" (tools/extract/extract.py find_nsz) needs a __main__; the wheel only has a console script.
    (sp / "nsz/__main__.py").write_text("from nsz import main\n\nmain()\n", encoding="ascii")
    return d


# Names that must never be in a component: game material, keys, recompiler/lifter output (derived from the game).
FORBIDDEN_NAMES = ("islands_gen.inc", "prod.keys", "title.keys", "main.elf", "romfs.bin", "plt_imports.inc", "sdk_symbols.inc",
                   "nvn_funcs.inc", "nvn_formats.inc")
FORBIDDEN_SUFFIXES = (".nso", ".nca", ".nsp", ".nsz", ".xci", ".xcz", ".npdm")


def check_staged(cid: str, d: Path) -> None:
    bad = [p for p in d.rglob("*") if p.is_file() and (p.name.lower() in FORBIDDEN_NAMES or p.suffix.lower() in FORBIDDEN_SUFFIXES
                                                      or p.name.lower().startswith("recompiled_"))]
    if bad:
        raise SystemExit(f"component {cid}: refusing to package game-derived files: " + ", ".join(str(p) for p in bad[:10]))


def stage_scripts(work: Path) -> Path:
    d = work / "scripts" / "port"
    shutil.rmtree(d.parent, ignore_errors=True)
    for rel in SCRIPTS:
        copy(REPO / rel, d / rel)
    return d


def stage_nvnvk_template(work: Path, out: Path) -> None:
    """nvnvk's hook list (mod.json without config), generated here once with gen_tables.py from the local SDK map.
    The map itself never ships; the launcher fills the config at install time (launcher.py nvnvk_mod_json)."""
    tmp = work / "nvnvk-template"
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run([sys.executable, str(REPO / "tools/port/nvnvk/gen_tables.py"), "--mod", "nvnvk", "--mod-json", str(tmp)],
                   check=True, capture_output=True)
    doc = json.loads((tmp / "mod.json").read_text(encoding="utf-8"))
    doc["config"] = {}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    log(f"  nvnvk template: {len(doc['hooks'])} hooks")


# The fork's runtime frontends are not shipped: the game runs on plahost. The exporter is aot_export.exe (headless,
# fork tools/recomp_reemit/build_aot_export.cmd); the fork's suyu.exe (Qt) only with --gui-exporter (fallback).
FORK_BIN_EXCLUDE = {"pla.exe", "suyu-cmd.exe"}
AOT_EXPORT = "build/aot_export/aot_export.exe"


def stage_platform(work: Path, fork: Path, nnshim: Path, nvnvk: Path, mods_build: Path, plahost: Path,
                   gui_exporter: bool = False) -> Path:
    d = work / "platform" / "port"
    shutil.rmtree(d.parent, ignore_errors=True)
    copy(fork / AOT_EXPORT, d / "mk8-recomp" / AOT_EXPORT)
    if gui_exporter:
        copy(fork / "build/suyu/bin", d / "mk8-recomp/build/suyu/bin")
        for p in (d / "mk8-recomp/build/suyu/bin").iterdir():
            if p.name.lower() in FORK_BIN_EXCLUDE:
                p.unlink()
        for s in FORK_SCRIPTS:
            copy(fork / "scripts" / s, d / "mk8-recomp/scripts" / s)
    copy(plahost, d / "build/port/plahost/PLA.exe")
    fonts = "third_party/suyu/src/core/file_sys/system_archive/data"
    for s in FONT_SOURCES:
        copy(fork / fonts / s, d / "mk8-recomp" / fonts / s)
    copy(nnshim, d / "build/port/nnshim/nnshim.dll")
    sdl = sorted((Path.home() / ".nuget/packages/ultz.native.sdl").glob("*/runtimes/win-x64/native/SDL2.dll"), reverse=True)
    if sdl:
        copy(sdl[0], d / "build/port/nnshim/SDL2.dll")
    copy(nvnvk, d / "build/port/nvnvk/nvnvk.dll")
    stage_nvnvk_template(work, d / "build/port/nvnvk/mod.template.json")
    # nnshim: hook template (from generated/*.inc, which never ships) + default controls file.
    subprocess.run([sys.executable, str(REPO / "tools/port/nnshim/nnshim_template.py"), "--out",
                    str(d / "build/port/nnshim/mod.template.json")], check=True, capture_output=True)
    copy(REPO / "tools/port/nnshim/controls.default.json", d / "build/port/nnshim/controls.default.json")
    log("  nnshim template + default controls")
    for m, folder in PREBUILT_MODS.items():
        meta = json.loads((REPO / "recomp/mods" / m / "mod.json").read_text(encoding="utf-8"))
        copy(REPO / "recomp/mods" / m / "mod.json", d / "build/mods-recomp" / m / "mod.json")
        copy(mods_build / folder / meta["library"], d / "build/mods-recomp" / m / meta["library"])
    lic = d / "LICENSES"
    copy(fork / "third_party/suyu/LICENSE.txt", lic / "suyu-GPL-3.0-or-later.txt")
    copy(fork / "third_party/suyu/LICENSES", lic / "spdx")
    copy(REPO / "tools/port/nvnvk/third_party/imgui/LICENSE.txt", lic / "DearImGui-MIT.txt")
    copy(REPO / "tools/port/nvnvk/third_party/fonts/lato/OFL.txt", lic / "Lato-OFL-1.1.txt")
    copy(REPO / "tools/port/nvnshader/third_party/shader_recompiler/PROVENANCE.txt",
         lic / "nvnvk-shader-recompiler-PROVENANCE.txt")
    (lic / "NOTICE.txt").write_text(
        "PLARecomp platform package: third-party software and licenses\n\n"
        "build/port/plahost/PLA.exe  the game's executable: own code of this project, GPL-3.0-or-later (no emulator)\n"
        "mk8-recomp/build/aot_export install-time headless exporter from the suyu fork (tools/recomp_reemit/aot_export.cpp\n"
        "                            + the fork's AArch64-to-C emitter): GPL-3.0-or-later (suyu-GPL-3.0-or-later.txt);\n"
        "                            source: plarecomp-exporter-source-<version>.tar.xz, published next to\n"
        "                            PLARecompSetup.exe on the same release page\n"
        + ("mk8-recomp/build/suyu/bin   fallback GUI exporter from the suyu fork: GPL-3.0-or-later\n"
           "  Qt6*.dll, plugins/        Qt 6: LGPL-3.0 (spdx/LGPL-3.0-*.txt)\n"
           "  libssl/libcrypto          OpenSSL 3: Apache-2.0 (spdx/Apache-2.0.txt)\n"
           "  dxcompiler/dxil           DirectX Shader Compiler: University of Illinois/NCSA and Microsoft redistribution terms\n"
           if gui_exporter else "") +
        "mk8-recomp/third_party/...  font sources from suyu (free substitute fonts), GPL\n"
        "build/port/nnshim           own code of this project (see the repository license)\n"
        "build/port/nvnvk            own code of this project; includes the shader recompiler of the suyu project\n"
        "                            (yuzu continuation), GPL-3.0-or-later (suyu-GPL-3.0-or-later.txt;\n"
        "                            nvnvk-shader-recompiler-PROVENANCE.txt)\n"
        "build/port/nnshim/SDL2.dll  SDL 2: Zlib (spdx/Zlib.txt)\n"
        "build/mods-recomp           own code of this project\n"
        "Setup program               Dear ImGui (DearImGui-MIT.txt), Lato fonts (Lato-OFL-1.1.txt)\n"
        "Python component            PSF-2.0 (python/LICENSE.txt) and the modules' own licenses (python/Lib/site-packages/*.dist-info)\n"
        "Toolchain component         LLVM: Apache-2.0 WITH LLVM-exception (recomp-toolchain/LICENSES)\n", encoding="utf-8")
    return d


def stage_pgo(work: Path, pgo_dir: Path, fork: Path, profile_id: str = "pla-1.1.1") -> Path:
    """The PGO profile (build/pgo): a sparse clang profile of the recompiled code + its manifest. Refuses a profile
    trained with another exporter, other emitter options or another game build (the launcher would ignore it)."""
    spec = importlib.util.spec_from_file_location("pgo_train", REPO / "tools/toolchain/pgo_train.py")
    pt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pt)
    prof, man_p = pgo_dir / f"{profile_id}.profdata", pgo_dir / f"{profile_id}.pgo.json"
    if not prof.is_file() or not man_p.is_file():
        raise SystemExit(f"no PGO profile in {pgo_dir} ({prof.name} + {man_p.name}); train one "
                         "(tools/toolchain/pgo_train.py) or pass --no-pgo")
    man = json.loads(man_p.read_text(encoding="utf-8"))
    profile = json.loads((REPO / "tools/launcher/profiles" / f"{profile_id}.json").read_text(encoding="utf-8"))
    bad = [what for what, ok in (
        ("exporter", man.get("exporter_sha256") == sha256(fork / AOT_EXPORT)),
        ("emitter options", man.get("emitter") == pt.emitter_key(profile)),
        ("game build", man.get("build_ids") == pt.build_ids(profile)),
        ("profile file", man.get("profdata_sha256") == sha256(prof))) if not ok]
    if bad:
        raise SystemExit(f"the PGO profile in {pgo_dir} does not match this release ({', '.join(bad)}): retrain it "
                         "(tools/toolchain/pgo_train.py, docs/port/LAUNCHER.md §8.12) or pass --no-pgo")
    d = work / "pgo" / "port"
    shutil.rmtree(d.parent, ignore_errors=True)
    copy(prof, d / "build/pgo" / prof.name)
    copy(man_p, d / "build/pgo" / man_p.name)
    log(f"  PGO profile {man['profdata_sha256'][:12]} ({prof.stat().st_size / 2**20:.1f} MB): {man.get('coverage', '')}")
    return d


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True,
                          encoding="utf-8").stdout


BIN_EXT = {".exe", ".dll", ".lib", ".pdb", ".obj", ".exp"}


def write_exporter_source(fork: Path, out: Path, release: str) -> None:
    """GPL corresponding source of the shipped exporter (aot_export.exe, and the fork's suyu.exe with --gui-exporter):
    the fork's tracked files and the suyu submodule's tracked files AS THEY ARE ON DISK (local changes included),
    plus SOURCE.txt with the commits. Prebuilt binaries tracked upstream (.exe/.dll/...) are not source and stay out. Unmodified externals are listed with their commit and upstream URL; an
    external with local changes is included as well. Published next to the setup .exe as a release asset."""
    suyu = fork / "third_party/suyu"
    members: list[tuple[Path, str]] = []

    def add_tracked(repo: Path, prefix: str, skip: set[str]) -> None:
        for rel in git(repo, "ls-files", "-z").split("\0"):
            if rel and rel not in skip and Path(rel).suffix.lower() not in BIN_EXT and (repo / rel).is_file():
                members.append((repo / rel, prefix + rel))

    subs = {}
    for line in git(suyu, "submodule", "status").splitlines():
        parts = line[1:].split()
        subs[parts[1]] = (line[0], parts[0])
    dirty = {l[3:].strip() for l in git(suyu, "status", "--porcelain", "--ignore-submodules=none").splitlines()
             if l[3:].strip() in subs}
    add_tracked(fork, "", {"third_party/suyu"})
    add_tracked(suyu, "third_party/suyu/", set(subs))
    for path in sorted(dirty | {p for p, (flag, _) in subs.items() if flag == "+"}):
        add_tracked(suyu / path, f"third_party/suyu/{path}/", set())
    urls = {}
    for line in git(suyu, "config", "-f", ".gitmodules", "--get-regexp", r"submodule\..*\.url").splitlines():
        key, url = line.split(None, 1)
        urls[git(suyu, "config", "-f", ".gitmodules", key.replace(".url", ".path")).strip()] = url
    head = git(fork, "rev-parse", "HEAD").strip()
    suyu_head = git(suyu, "rev-parse", "HEAD").strip()
    changed = git(suyu, "status", "--porcelain").count("\n")
    lines = [f"PLARecomp {release}: corresponding source of the exporter (GPL-3.0-or-later)", "",
             f"fork        commit {head} ({git(fork, 'remote', 'get-url', 'origin').strip()}) + PLARecomp changes",
             f"suyu        commit {suyu_head} + {changed} locally changed paths (included as they are)",
             "build       third_party/suyu/tools/recomp_reemit/build_aot_export.cmd", "", "externals (unmodified unless included):"]
    for p, (flag, sha) in sorted(subs.items()):
        lines.append(f"  {p:<40} {sha}  {urls.get(p, '')}{'  [included]' if p in dirty or flag == '+' else ''}")
    out.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out, "w:xz", preset=6 if FAST else 9) as tf:
        for src, arc in members:
            tf.add(src, f"exporter-source/{arc}", recursive=False)
        data = ("\n".join(lines) + "\n").encode("utf-8")
        ti = tarfile.TarInfo("exporter-source/SOURCE.txt")
        ti.size, ti.mtime = len(data), int(time.time())
        tf.addfile(ti, io.BytesIO(data))
    log(f"  exporter source: {len(members)} files, {out.stat().st_size / 2**20:.1f} MB ({out.name})")


def pack(staged: Path, out: Path, reuse: bool) -> None:
    if reuse and out.is_file():
        log(f"reusing {out.name}")
        return
    t = time.time()
    packer().create(staged, out)
    log(f"  packed {out.name}: {out.stat().st_size / 2**20:.1f} MB in {time.time() - t:.0f} s")


# ---------------------------------------------------------------- assembly
def build_exe(work: Path) -> Path:
    out = work / "build"
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / "build.ps1"), "-Out", str(out)], check=True)
    return out / "PLARecompSetup.exe"


def write_exe(bare: Path, dst: Path, comps: list, embed: bool, url_base: str, release: str) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(bare, dst)
    with open(dst, "r+b") as f:
        f.seek(0, 2)
        items = []
        for c in comps:
            item = {k: v for k, v in c.items() if k != "path"}
            if embed:
                item["offset"] = f.tell()
                with open(c["path"], "rb") as s:
                    shutil.copyfileobj(s, f, 1 << 20)
            items.append(item)
        index = json.dumps({"format": 1, "release": release, "url_base": url_base, "light": not embed, "components": items},
                           indent=1).encode("utf-8")
        ioff = f.tell()
        f.write(index)
        f.write(MAGIC + struct.pack("<IIQQ", 1, 0, ioff, len(index)) + MAGIC)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path(r"D:\PLARecomp\release"))
    ap.add_argument("--release", default="0.1.0-dev")
    ap.add_argument("--url-base", default=URL_PENDING, help="download fallback base URL (default: a PENDING placeholder)")
    ap.add_argument("--fork", type=Path, default=Path(r"D:\PLARecomp\mk8-recomp"))
    ap.add_argument("--nnshim", type=Path, default=REPO / "build/port/nnshim-rel/nnshim.dll")
    ap.add_argument("--nvnvk", type=Path, default=REPO / "build/port/nvnvk-rel/nvnvk.dll")
    ap.add_argument("--plahost", type=Path, default=REPO / "build/port/plahost/PLA.exe",
                    help="the game's PLA.exe (tools/port/plahost/build.ps1)")
    ap.add_argument("--mods-build", type=Path, default=Path(r"D:\PLARecomp\mods-build"))
    ap.add_argument("--toolchain", type=Path, default=Path(r"D:\PLARecomp\toolchain-portable\recomp-toolchain"))
    ap.add_argument("--gui-exporter", action="store_true",
                    help="also ship the fork's GUI exporter (suyu.exe + Qt) for launcher.py --exporter gui (fallback)")
    ap.add_argument("--pgo-dir", type=Path, default=Path(r"D:\PLARecomp\pgo"),
                    help="trained PGO profile + manifest (tools/toolchain/pgo_train.py manifest)")
    ap.add_argument("--no-pgo", action="store_true", help="ship without the PGO profile (installs compile without PGO)")
    ap.add_argument("--reuse", action="store_true", help="reuse component archives that already exist")
    ap.add_argument("--light", action="store_true", help="also build the light .exe (index only) and its component files")
    ap.add_argument("--fast", action="store_true", help="LZMA preset 6 instead of 9e (bigger, much faster; for tests)")
    a = ap.parse_args()
    global FAST
    FAST = a.fast
    work, cache, comp_out = a.out / "work", a.out / "cache", a.out / "components"
    for d in (work, cache, comp_out):
        d.mkdir(parents=True, exist_ok=True)

    plan = [
        ("python", f"Python {PY_VERSION} (embeddable)", PY_VERSION, "PSF-2.0 + module licenses", lambda: stage_python(work, cache)),
        ("scripts", "Setup scripts", a.release, "project license", lambda: stage_scripts(work)),
        ("platform", "Platform (game executable, renderer, exporter)", a.release, "GPL-3.0 / LGPL-3.0 / own (see LICENSES)",
         lambda: stage_platform(work, a.fork, a.nnshim, a.nvnvk, a.mods_build, a.plahost, a.gui_exporter)),
        ("toolchain", "Portable compiler (clang-cl + lld-link)", "23.1.2", "Apache-2.0 WITH LLVM-exception", lambda: a.toolchain),
    ]
    if not a.no_pgo:
        plan.append(("pgo", "PGO profile (execution counters of the recompiled code)", a.release, "project license",
                     lambda: stage_pgo(work, a.pgo_dir, a.fork)))
    comps = []
    for cid, name, ver, lic, stage in plan:
        out = comp_out / f"plarecomp-{cid}-{ver}.tar.xz"
        log(f"== {cid}")
        staged = stage() if not (a.reuse and out.is_file()) else None
        if staged is not None:
            check_staged(cid, staged)
        pack(staged, out, a.reuse)
        meta_p = out.with_suffix(".json")
        if staged is not None:
            meta = {"root": staged.name, "unpacked": tree_size(staged)}
            meta_p.write_text(json.dumps(meta), encoding="utf-8")
        meta = json.loads(meta_p.read_text(encoding="utf-8"))
        comps.append({"id": cid, "name": name, "version": ver, "file": out.name, "size": out.stat().st_size, "sha256": sha256(out),
                      "root": meta["root"], "unpacked": meta["unpacked"], "license": lic, "path": out})
    log("== exporter source (GPL)")
    src_out = a.out / f"plarecomp-exporter-source-{a.release}.tar.xz"
    if not (a.reuse and src_out.is_file()):
        write_exporter_source(a.fork, src_out, a.release)
    log("== setup program")
    bare = build_exe(work)
    full = a.out / "PLARecompSetup.exe"
    write_exe(bare, full, comps, True, a.url_base, a.release)
    report = {"release": a.release, "exe": str(full), "exe_bytes": full.stat().st_size, "exe_sha256": sha256(full),
              "bare_exe_bytes": bare.stat().st_size,
              "exporter_source": {"file": src_out.name, "bytes": src_out.stat().st_size, "sha256": sha256(src_out)},
              "components": [{k: v for k, v in c.items() if k != "path"} for c in comps]}
    if a.light:
        ld = a.out / "light"
        ld.mkdir(exist_ok=True)
        write_exe(bare, ld / "PLARecompSetup.exe", comps, False, a.url_base, a.release)
        for c in comps:
            shutil.copy2(c["path"], ld / c["file"])
        (ld / "components.json").write_text(json.dumps([{k: v for k, v in c.items() if k != "path"} for c in comps], indent=1), encoding="utf-8")
        report["light_exe_bytes"] = (ld / "PLARecompSetup.exe").stat().st_size
    (a.out / "release.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    log(f"\n{full}: {full.stat().st_size / 2**20:.1f} MB (setup {bare.stat().st_size / 2**20:.1f} MB + components "
        f"{sum(c['size'] for c in comps) / 2**20:.1f} MB); GitHub Releases limit: 2 GiB per asset")
    for c in comps:
        log(f"  {c['id']:<10} {c['size'] / 2**20:7.1f} MB packed, {c['unpacked'] / 2**20:7.1f} MB unpacked  {c['sha256'][:16]}…")
    return 0


FAST = False
if __name__ == "__main__":
    sys.exit(main())
