"""Itanium demangling with the best available tool.

Order: GNU/LLVM c++filt (devkitA64, PATH) in batch mode -> itanium_demangler
package (pure Python) -> no demangling.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from functools import lru_cache
from pathlib import Path

_CANDIDATES = ["llvm-cxxfilt", "aarch64-none-elf-c++filt", "c++filt"]


@lru_cache(maxsize=1)
def _cxxfilt() -> str | None:
    env = os.environ.get("CXXFILT")
    if env:
        return env
    for c in _CANDIDATES:
        p = shutil.which(c)
        if p:
            return p
    for root in (os.environ.get("DEVKITPRO"), "C:/devkitPro", "/opt/devkitpro"):
        if root:
            p = Path(root) / "devkitA64" / "bin" / ("aarch64-none-elf-c++filt" + (".exe" if os.name == "nt" else ""))
            if p.exists():
                return str(p)
    return None


_cache: dict[str, str | None] = {}


def demangle_many(names) -> dict[str, str | None]:
    """Return {mangled: demangled or None}. Uses a global cache."""
    todo = sorted({n for n in names if n and n not in _cache})
    if todo:
        tool = _cxxfilt()
        results: list[str | None] | None = None
        if tool:
            try:
                p = subprocess.run([tool], input="\n".join(todo) + "\n", capture_output=True,
                                   text=True, encoding="utf-8", timeout=600)
                lines = p.stdout.splitlines()
                if p.returncode == 0 and len(lines) == len(todo):
                    results = [None if out == inp else out for inp, out in zip(todo, lines)]
            except (OSError, subprocess.SubprocessError):
                results = None
        if results is None:
            results = [_py_demangle(n) for n in todo]
        _cache.update(zip(todo, results))
    return {n: _cache.get(n) for n in names}


def demangle(name: str) -> str | None:
    return demangle_many([name])[name]


def demangle_type(type_name: str) -> str | None:
    """Demangle a type name as it appears in _ZTS (e.g. 'N4game4BaseE')."""
    return demangle_types([type_name])[type_name]


def demangle_types(type_names) -> dict[str, str | None]:
    mangled = {t: "_ZTS" + t for t in type_names}
    res = demangle_many(mangled.values())
    out = {}
    for t, m in mangled.items():
        d = res.get(m)
        prefixes = ("typeinfo name for ",)
        if d:
            for pre in prefixes:
                if d.startswith(pre):
                    d = d[len(pre):]
                    break
            else:
                d = None
        out[t] = d
    return out


def _py_demangle(name: str) -> str | None:
    try:
        import itanium_demangler
    except ImportError:
        return None
    try:
        node = itanium_demangler.parse(name)
    except Exception:
        return None
    if node is None:
        return None
    s = str(node)
    return s.replace("_GLOBAL__N_1", "(anonymous namespace)")
