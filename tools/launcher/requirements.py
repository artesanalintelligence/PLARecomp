#!/usr/bin/env python3
"""Checks the launcher requirements on this machine (read-only, fast).

Each check returns a Req: ok=True (met), False (missing, blocks the phases that need it)
or None (warning). None of this installs or downloads anything; the messages say how to
get what is missing.
"""
from __future__ import annotations

import ctypes
import importlib.util
import os
import platform
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

GB = 1 << 30


@dataclass
class Req:
    name: str
    ok: Optional[bool]
    detail: str
    phases: str = ""         # phases that need it ("" = all)
    help: str = ""


def _run(argv: List[str], timeout: float = 20) -> str:
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                           encoding="utf-8", errors="replace")
        return (r.stdout or "") + (r.stderr or "")
    except (OSError, subprocess.TimeoutExpired):
        return ""


def free_space(path: Path) -> int:
    p = path
    while not p.exists() and p.parent != p:
        p = p.parent
    return shutil.disk_usage(p).free


def ram_total() -> int:
    class MS(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("avail", ctypes.c_ulonglong), ("tpf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong),
                    ("tv", ctypes.c_ulonglong), ("av", ctypes.c_ulonglong), ("ave", ctypes.c_ulonglong)]
    m = MS()
    m.len = ctypes.sizeof(MS)
    try:
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.total
    except (AttributeError, OSError):
        return 0


def vs_vcvars() -> Optional[Path]:
    if os.environ.get("MK8R_VCVARS") and Path(os.environ["MK8R_VCVARS"]).is_file():
        return Path(os.environ["MK8R_VCVARS"])
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return None
    out = _run([str(vswhere), "-version", "[17.0,18.0)", "-products", "*", "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]).strip()
    if not out:
        return None
    p = Path(out.splitlines()[0]) / "VC/Auxiliary/Build/vcvars64.bat"
    return p if p.is_file() else None


def clang_cl() -> Optional[Path]:
    for c in (Path(r"C:\Program Files\LLVM\bin\clang-cl.exe"), shutil.which("clang-cl")):
        if c and Path(c).is_file():
            return Path(c)
    return None


def cmake_kitware() -> Optional[Path]:
    c = Path(r"C:\Program Files\CMake\bin\cmake.exe")
    if c.is_file():
        return c
    w = shutil.which("cmake")
    return Path(w) if w and "msys" not in w.lower() and "devkitpro" not in w.lower() else None


def check(profile: dict, work: Path, dest: Path, fork: Optional[Path], keys: Optional[Path],
          needs_nsz: bool = False, space_gb: float = 0.0, toolchain: str = "vs",
          toolchain_dir: Optional[Path] = None, exporter: Optional[Path] = None,
          game_exe: Optional[Path] = None) -> List[Req]:
    R: List[Req] = []
    repo = Path(__file__).resolve().parents[2]

    # System
    ok_os = platform.system() == "Windows" and platform.machine().endswith("64")
    R.append(Req("Windows x64", ok_os, f"{platform.system()} {platform.release()} {platform.machine()}"))
    ram = ram_total()
    jobs = max(1, ram // (8 * GB)) if ram else 1
    R.append(Req("RAM", None if ram < 24 * GB else True,
                 f"{ram / GB:.0f} GB -> {jobs} parallel compilations of main (each clang-cl of main uses ~5.4 GB)",
                 "compile", "With 16 GB, compiling main goes from ~45 min to ~1.5-2 h (1-2 jobs)."))

    # Python
    v = sys.version_info
    R.append(Req("Python >= 3.10", v >= (3, 10), f"{sys.executable} {v.major}.{v.minor}.{v.micro}"))
    for mod, phase, hlp in (("lz4", "", "python tools/extract/bootstrap.py"),
                            ("Crypto", "", "python tools/extract/bootstrap.py (pycryptodome)"),
                            ("zstandard", "identify/prepare/export (NSZ)", "python -m pip install nsz"),
                            ("PIL", "assemble (icon)", "python -m pip install pillow")):
        ok = importlib.util.find_spec(mod) is not None
        R.append(Req(f"module {mod}", ok if phase == "" else (ok or None), "present" if ok else "missing", phase, hlp))
    if needs_nsz:
        sys.path.insert(0, str(repo / "tools" / "extract"))
        import extract as ex  # noqa: E402
        cmd = ex.find_nsz()
        R.append(Req("nsz (decompress NSZ/XCZ)", bool(cmd), " ".join(cmd) if cmd else "missing",
                     "prepare", "python -m pip install nsz"))

    # Keys (only that they exist and have header_key; they are never shown)
    kdir = Path(os.environ.get("USERPROFILE") or Path.home()) / ".switch"
    prod = keys or kdir / "prod.keys"
    has = prod.is_file() and re.search(r"(?m)^\s*header_key\s*=", prod.read_text(errors="replace"))
    R.append(Req("user prod.keys", bool(has), str(prod) if prod.is_file() else f"{prod} does not exist",
                 "identify/extract", "Dump the keys of YOUR console with Lockpick_RCM."))

    # Toolchain that compiles the generated C
    if toolchain == "portable":
        tc = toolchain_dir or Path(os.environ.get("PLA_RECOMP_ROOT", r"D:\PLARecomp")) / "toolchain-portable/recomp-toolchain"
        parts = [tc / "bin/clang-cl.exe", tc / "bin/lld-link.exe", tc / "build_modules.py"]
        missing = [p.name for p in parts if not p.is_file()]
        R.append(Req("portable toolchain (clang-cl + lld-link)", not missing,
                     str(tc) if not missing else f"{tc}: missing {', '.join(missing)}", "compile",
                     "Extract recomp-toolchain.tar.xz (docs/port/TOOLCHAIN.md) or use --toolchain vs."))
        return _check_rest(R, profile, work, dest, fork, space_gb, repo, exporter, game_exe)
    if toolchain == "custom":
        R.append(Req("compilation", None, "custom command (--build-cmd): not checked", "compile"))
        return _check_rest(R, profile, work, dest, fork, space_gb, repo, exporter, game_exe)
    vc = vs_vcvars()
    R.append(Req("VS 2022 Build Tools (MSVC + Windows SDK)", bool(vc), str(vc or "not found (vswhere)"),
                 "compile/platform", "Install 'Build Tools for Visual Studio 2022' with the 'Desktop development with C++' workload."))
    sdk = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10/Lib"
    vers = sorted(p.name for p in sdk.iterdir()) if sdk.is_dir() else []
    R.append(Req("Windows 10/11 SDK", bool(vers), vers[-1] if vers else "not found", "compile"))
    cl = clang_cl()
    clv = re.search(r"clang version (\S+)", _run([str(cl), "--version"])) if cl else None
    R.append(Req("clang-cl (LLVM)", bool(cl), f"{cl} {clv.group(1) if clv else ''}".strip() if cl else "not found",
                 "compile", "Install LLVM for Windows (https://github.com/llvm/llvm-project/releases), 17 or newer."))
    cm = cmake_kitware()
    first = shutil.which("cmake") or ""
    R.append(Req("CMake (Kitware)", bool(cm), str(cm or "not found"), "compile",
                 "Install CMake from cmake.org. The devkitPro/MSYS2 one does not work with MSVC."))
    if first and ("msys" in first.lower() or "devkitpro" in first.lower()):
        R.append(Req("CMake on PATH", None, f"the first one is {first}; the launcher puts the Kitware one first (env.ps1)", "compile"))
    nj = shutil.which("ninja") or (vc and next((vc.parents[3] / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja").glob("ninja.exe"), None))
    R.append(Req("Ninja", bool(nj), str(nj or "not found"), "compile", "Comes with VS Build Tools (CMake component) or from ninja-build.org."))
    return _check_rest(R, profile, work, dest, fork, space_gb, repo, exporter, game_exe)


def _check_rest(R: List[Req], profile: dict, work: Path, dest: Path, fork: Optional[Path],
                space_gb: float, repo: Path, exporter: Optional[Path] = None,
                game_exe: Optional[Path] = None) -> List[Req]:
    # Running the game
    vk = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/vulkan-1.dll"
    R.append(Req("Vulkan (vulkan-1.dll)", vk.is_file(), str(vk) if vk.is_file() else "not found", "play",
                 "Update the GPU driver (it ships the Vulkan loader)."))

    # Platform (own binaries: the install-time exporter, the game's PLA.exe, nnshim, nvnvk)
    if fork and not exporter:
        exporter = fork / "build/aot_export/aot_export.exe"
    for name, p, phase, hlp in (
            ("exporter", exporter, "export",
             "Build it (fork tools/recomp_reemit/build_aot_export.cmd; GUI fallback: build-suyu-pla.ps1) "
             "or use a platform package."),
            (f"game {profile['dist']['exe']} (plahost)", game_exe, "assemble",
             "Build it (tools/port/plahost/build.ps1) or use a platform package.")):
        if p:
            R.append(Req(f"platform: {name}", p.is_file(), str(p) if p.is_file() else f"{p} does not exist", phase, hlp))
    for d in ("nnshim", "nvnvk"):
        p = repo / "build/port" / d / f"{d}.dll"
        R.append(Req(f"platform: {d}.dll", p.is_file() or None, str(p) if p.is_file() else f"{p} does not exist", "platform"))
    ht = repo / "tools/bin"
    has_ht = any(ht.glob("hactool*.exe")) if ht.is_dir() else False
    R.append(Req("hactool (ExeFS cross-check)", has_ht or None, "present" if has_ht else "missing (optional)",
                 "extract", "python tools/extract/bootstrap.py"))

    # Disk space
    vols = {}
    for p in (work, dest):
        root = Path(os.path.splitdrive(str(p.resolve()))[0] + "\\")
        vols[root] = free_space(p)
    for root, free in vols.items():
        need = space_gb if len(vols) == 1 or root == Path(os.path.splitdrive(str(work.resolve()))[0] + "\\") else 3
        R.append(Req(f"free space on {root}", free / GB >= need or (None if free / GB >= need * 0.7 else False),
                     f"{free / GB:.1f} GB free; the plan needs ~{need:.0f} GB", "",
                     "Free some space or use --work on another drive (and --clean at the end)."))

    # Coordination with this machine's test queue (if it exists)
    locks = Path(os.environ.get("PLA_RECOMP_ROOT", r"D:\PLARecomp")) / "locks"
    if locks.is_dir():
        held = [p.name for p in locks.iterdir() if p.name in ("heavy", "game")]
        R.append(Req("queue locks", None if held else True,
                     f"held: {', '.join(held)}" if held else "free", "export/compile",
                     "Another heavy build or the game is running: wait or coordinate."))
    procs = _run(["tasklist", "/FO", "CSV", "/NH"]).lower()
    running = [n for n in ("suyu.exe", "pla.exe", "suyu-cmd.exe") if f'"{n}"' in procs]
    if running:
        R.append(Req("game processes", None, f"running: {', '.join(running)}", "export/compile",
                     "Do not export or compile main with the game open (virtual memory)."))
    return R


def report(reqs: List[Req]) -> str:
    sym = {True: "OK     ", False: "MISSING", None: "WARNING"}
    L = []
    for r in reqs:
        f = f" [{r.phases}]" if r.phases else ""
        L.append(f"  {sym[r.ok]} {r.name}{f}: {r.detail}")
        if r.ok is not True and r.help:
            L.append(f"           -> {r.help}")
    return "\n".join(L)
