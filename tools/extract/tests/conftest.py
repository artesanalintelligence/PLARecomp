"""Fixtures: compile the test module with devkitA64 and convert it to NSO with elf2nso."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
EXTRACT = TESTS.parent
sys.path.insert(0, str(EXTRACT))
sys.path.insert(0, str(TESTS))

BUILD = TESTS / "_build"
DEVKITPRO = Path(os.environ.get("DEVKITPRO_WIN") or os.environ.get("DEVKITPRO_PATH") or "C:/devkitPro")
if not DEVKITPRO.is_dir():
    dk = os.environ.get("DEVKITPRO", "")
    # Git Bash/MSYS: /opt/devkitpro -> C:/devkitPro
    DEVKITPRO = Path(dk) if Path(dk).is_dir() else DEVKITPRO

GXX = DEVKITPRO / "devkitA64" / "bin" / "aarch64-none-elf-g++.exe"
if not GXX.is_file():
    GXX = DEVKITPRO / "devkitA64" / "bin" / "aarch64-none-elf-g++"
ELF2NSO = DEVKITPRO / "tools" / "bin" / "elf2nso.exe"
if not ELF2NSO.is_file():
    ELF2NSO = DEVKITPRO / "tools" / "bin" / "elf2nso"

VARIANTS = {
    # libnx default: -z pack-relative-relocs (DT_RELR)
    "relr": [],
    # without RELR: RELATIVE relocations in .rela.dyn, like Nintendo's binaries
    "rela": ["-Wl,-z,nopack-relative-relocs"],
}


def _build(variant: str) -> tuple[Path, Path]:
    out = BUILD / variant
    out.mkdir(parents=True, exist_ok=True)
    elf, nso = out / "testmod.elf", out / "testmod.nso"
    src = TESTS / "fixture" / "testmod.cpp"
    if nso.is_file() and nso.stat().st_mtime > src.stat().st_mtime and \
            nso.stat().st_mtime > Path(__file__).stat().st_mtime:
        return elf, nso
    env = dict(os.environ, DEVKITPRO=str(DEVKITPRO).replace("\\", "/"))
    cmd = [str(GXX), "-O2", "-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE",
           "-frtti", "-fexceptions", f"-specs={DEVKITPRO / 'libnx' / 'switch.specs'}",
           "-Wl,--export-dynamic", "-Wl,--unresolved-symbols=ignore-all", "-Wl,--hash-style=both",
           *VARIANTS[variant], str(src), f"-L{DEVKITPRO / 'libnx' / 'lib'}", "-lnx", "-o", str(elf)]
    subprocess.run(cmd, check=True, env=env)
    subprocess.run([str(ELF2NSO), str(elf), str(nso)], check=True, env=env, stdout=subprocess.DEVNULL)
    return elf, nso


def have_devkit() -> bool:
    return GXX.is_file() and ELF2NSO.is_file() and (DEVKITPRO / "libnx" / "switch.specs").is_file()


@pytest.fixture(scope="session", params=sorted(VARIANTS))
def built(request):
    if not have_devkit():
        pytest.skip("devkitA64/libnx/elf2nso are not installed (C:/devkitPro)")
    return _build(request.param)


@pytest.fixture(scope="session")
def built_rela():
    if not have_devkit():
        pytest.skip("devkitA64/libnx/elf2nso are not installed (C:/devkitPro)")
    return _build("rela")


@pytest.fixture(scope="session")
def built_relr():
    if not have_devkit():
        pytest.skip("devkitA64/libnx/elf2nso are not installed (C:/devkitPro)")
    return _build("relr")
