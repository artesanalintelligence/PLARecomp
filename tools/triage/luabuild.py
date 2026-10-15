"""Compiles Lua 5.3.x (official source from lua.org) with a candidate clang and
returns its functions in link order (alphabetical TUs, as in main).

Sources: tools/triage/cache/src/lua-5.3.x (downloaded from https://www.lua.org/ftp/).
Objects: tools/triage/cache/build/<key>/ (in .gitignore)."""
from __future__ import annotations

import hashlib
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from common import CACHE, ROOT
from objfile import read_object

SRC = CACHE / "src"
BUILD = CACHE / "build"
CLANGS = ROOT / "toolchain" / "cache"
MUSL = ROOT / "toolchain" / "musl" / "include"

# The TUs of the library (lua.c and luac.c are the executables; they are left out).
LUA_TUS = ["lapi", "lauxlib", "lbaselib", "lbitlib", "lcode", "lcorolib", "lctype",
           "ldblib", "ldebug", "ldo", "ldump", "lfunc", "lgc", "linit", "liolib",
           "llex", "lmathlib", "lmem", "loadlib", "lobject", "lopcodes", "loslib",
           "lparser", "lstate", "lstring", "lstrlib", "ltable", "ltablib", "ltm",
           "lundump", "lutf8lib", "lvm", "lzio"]

BASE_FLAGS = ["-ffunction-sections", "-fdata-sections", "-c", "-nostdinc"]
DEFAULT_DEFINES = ["-DLUA_COMPAT_5_2", "-DLUA_USE_POSIX"]


def clang_path(ver: str) -> Path:
    return CLANGS / f"clang-{ver}" / "bin" / "clang.exe"


def build(lua_ver: str, clang_ver: str, flags: list[str], defines=None, jobs: int = 4):
    defines = DEFAULT_DEFINES if defines is None else defines
    clang = clang_path(clang_ver)
    resinc = next((CLANGS / f"clang-{clang_ver}" / "lib" / "clang").iterdir()) / "include"
    key = hashlib.sha1(repr((lua_ver, clang_ver, flags, defines)).encode()).hexdigest()[:12]
    out = BUILD / f"lua{lua_ver}-clang{clang_ver}-{key}"
    out.mkdir(parents=True, exist_ok=True)
    (out / "cmd.txt").write_text(" ".join([str(clang)] + flags + defines + BASE_FLAGS))
    src = SRC / f"lua-{lua_ver}" / "src"

    def one(tu):
        o = out / f"{tu}.o"
        c = src / f"{tu}.c"
        if not c.exists():
            return None
        if not o.exists():
            cmd = [str(clang), "-x", "c", *flags, *defines, *BASE_FLAGS,
                   "-isystem", str(resinc), "-isystem", str(MUSL), str(c), "-o", str(o)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                raise RuntimeError(f"fallo compilando {tu}: {r.stderr[:2000]}")
        return o

    with ThreadPoolExecutor(jobs) as ex:
        objs = list(ex.map(one, LUA_TUS))
    funcs = []
    for tu, o in zip(LUA_TUS, objs):
        if o is None:
            continue
        for fn in read_object(o):
            fn.tu = tu
            funcs.append(fn)
    return funcs, out
