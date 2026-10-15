"""Utilities shared by the decompilation tools (tools/decomp).

Address conventions: all public addresses are absolute with the image at
0x7100000000 (like data/main_functions.csv and viking). data/main.elf is
linked at 0; `IMAGE_BASE` is the offset.
"""
from __future__ import annotations

import bisect
import csv
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "data"
TOOLS = ROOT / "tools"
DECOMP_TOOLS = TOOLS / "decomp"
CACHE = DECOMP_TOOLS / "cache"            # regenerable, in .gitignore
GHIDRA_OUT = TOOLS / "ghidra" / "out"
DECOMP_OUT = GHIDRA_OUT / "decomp"        # Ghidra pseudo-C per function
MAIN_ELF = DATA / "main.elf"
FUNCTIONS_CSV = DATA / "main_functions.csv"
STATUS_CSV = DATA / "decomp_status.csv"
SYMBOLS_CSV = DATA / "decomp_symbols.csv"
DATA_SYMBOLS_CSV = DATA / "data_symbols.csv"
TRIAGE_CSV = DATA / "triage.csv"
TU_MAP_CSV = DATA / "tu_map.csv"
TRIAGE_OVERRIDES_CSV = DATA / "triage_overrides.csv"   # manual triage corrections
TU_MERGES_CSV = DATA / "tu_merges.csv"                 # tu_map.csv ranges that are a single TU
SRC = ROOT / "src"
INCLUDE = ROOT / "include"
LIB = ROOT / "lib"
IMAGE_BASE = 0x71_0000_0000

sys.path.insert(0, str(TOOLS / "analysis"))

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

# Symbol names that carry the original address: FUN_7100123456, DAT_..., etc.
ADDR_NAME_RE = re.compile(r"(?:FUN|DAT|PTR|LAB|sub|off|unk|vtable|data|func)_(71[0-9a-fA-F]{8})")
GENERIC_NAME_RE = re.compile(r"^(FUN_|vtable_[0-9a-f]+::|thunk_FUN_|_DT_)")


def parse_addr(s: str | int) -> int:
    """Accepts 0x7100123456, 7100123456, 0x123456 (relative to the base) or a FUN_xxx."""
    if isinstance(s, int):
        v = s
    else:
        s = s.strip()
        m = ADDR_NAME_RE.search(s)
        if m and not s.lower().startswith("0x"):
            return int(m.group(1), 16)
        v = int(s, 16) if not s.lower().startswith("0x") else int(s, 0)
    if v < IMAGE_BASE:
        v += IMAGE_BASE
    return v


def fmt(a: int) -> str:
    return f"0x{a:010x}"


@dataclass
class Func:
    addr: int
    size: int
    name: str
    quality: str

    @property
    def end(self) -> int:
        return self.addr + self.size

    @property
    def generic(self) -> bool:
        return not self.name or bool(GENERIC_NAME_RE.match(self.name))


class FunctionIndex:
    """Index of data/main_functions.csv (address -> function, lookup by name)."""

    def __init__(self, path: Path = FUNCTIONS_CSV):
        self.funcs: list[Func] = []
        with open(path, newline="", encoding="utf-8") as f:
            r = csv.reader(f)
            next(r)
            for row in r:
                if len(row) < 4:
                    continue
                self.funcs.append(Func(int(row[0], 16), int(row[2]), row[3], row[1]))
        self.funcs.sort(key=lambda f: f.addr)
        self.starts = [f.addr for f in self.funcs]
        self.by_addr = {f.addr: f for f in self.funcs}
        self._by_name: dict[str, Func] | None = None
        self._demangled: dict[str, str] | None = None

    def at(self, a: int) -> Func | None:
        return self.by_addr.get(a)

    def containing(self, a: int) -> Func | None:
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and self.funcs[i].addr <= a < self.funcs[i].end:
            return self.funcs[i]
        return None

    @property
    def by_name(self) -> dict[str, Func]:
        if self._by_name is None:
            self._by_name = {}
            for f in self.funcs:
                if f.name:
                    self._by_name.setdefault(f.name, f)
        return self._by_name

    def demangled(self, f: Func) -> str:
        """Readable name (uses tools/ghidra/out/main_functions_named.tsv if present)."""
        if self._demangled is None:
            self._demangled = {}
            p = GHIDRA_OUT / "main_functions_named.tsv"
            if p.exists():
                with open(p, encoding="utf-8") as fh:
                    next(fh)
                    for line in fh:
                        parts = line.rstrip("\n").split("\t")
                        if len(parts) >= 4 and parts[3]:
                            self._demangled[parts[2]] = parts[3]
        return self._demangled.get(f.name) or f.name or f"FUN_{f.addr:x}"

    def label(self, a: int) -> str:
        f = self.containing(a)
        if f is None:
            return fmt(a)
        nm = self.demangled(f)
        return nm if f.addr == a else f"{nm}+{a - f.addr:#x}"

    def resolve(self, s: str) -> Func | None:
        """Address, FUN_xxx, mangled name or exact readable name."""
        s = s.strip()
        try:
            a = parse_addr(s)
            f = self.at(a) or self.containing(a)
            if f:
                return f
        except ValueError:
            pass
        f = self.by_name.get(s)
        if f:
            return f
        self.demangled(self.funcs[0])
        for f in self.funcs:
            if self._demangled.get(f.name) == s:
                return f
        return None


@lru_cache(maxsize=1)
def functions() -> FunctionIndex:
    return FunctionIndex()


# ----------------------------------------------------------- triage / TU map
def _read_flexible_csv(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path, newline="", encoding="utf-8") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]   # comments
    sample = "".join(lines[:50])
    delim = "\t" if sample.count("\t") > sample.count(",") else ","
    return list(csv.DictReader(lines, delimiter=delim))


def _pick(row: dict, *keys):
    low = {k.lower().strip(): v for k, v in row.items() if k}
    for k in keys:
        if k in low and low[k] not in (None, ""):
            return low[k]
    return None


@lru_cache(maxsize=1)
def triage() -> dict[int, str]:
    """address -> category (game, library, havok, sdk, stub...). Empty if it does not exist yet."""
    out = {}
    for row in _read_flexible_csv(TRIAGE_CSV):
        a = _pick(row, "address", "addr", "start", "va")
        c = _pick(row, "category", "class", "kind", "type", "classification", "origin")
        if a and c:
            try:
                out[parse_addr(a)] = c.strip()
            except ValueError:
                pass
    if out:
        apply_triage_overrides(out)
    return out


def apply_triage_overrides(out: dict[int, str]):
    """data/triage_overrides.csv: address[,end],category[,reason]. With `end`, all functions in
    [address, end) receive the category."""
    rows = _read_flexible_csv(TRIAGE_OVERRIDES_CSV)
    if not rows:
        return
    idx = None
    for row in rows:
        a, e, c = _pick(row, "address", "addr", "start"), _pick(row, "end"), _pick(row, "category")
        if not a or not c:
            continue
        try:
            lo = parse_addr(a)
            if e:
                hi = parse_addr(e)
                idx = idx or functions()
                i = bisect.bisect_left(idx.starts, lo)
                while i < len(idx.funcs) and idx.funcs[i].addr < hi:
                    out[idx.funcs[i].addr] = c.strip()
                    i += 1
            else:
                out[lo] = c.strip()
        except ValueError:
            continue


def _tu_merges() -> list[tuple[int, int, str | None]]:
    """data/tu_merges.csv: start,end[,name][,reason] -> [start, end) ranges that form a single TU."""
    out = []
    for row in _read_flexible_csv(TU_MERGES_CSV):
        try:
            lo, hi = parse_addr(_pick(row, "start", "address")), parse_addr(_pick(row, "end"))
        except (TypeError, ValueError, AttributeError):
            continue
        out.append((lo, hi, (_pick(row, "name", "tu") or "").strip() or None))
    return out


def _apply_tu_merges(out: dict[int, str]):
    for lo, hi, name in _tu_merges():
        idx = functions()
        i = bisect.bisect_left(idx.starts, lo)
        first = idx.funcs[i].addr if i < len(idx.funcs) else lo
        nm = name or out.get(first) or f"unk/tu_{lo:x}"
        while i < len(idx.funcs) and idx.funcs[i].addr < hi:
            out[idx.funcs[i].addr] = nm
            i += 1


@lru_cache(maxsize=1)
def tu_map() -> dict[int, str]:
    """address -> TU, with the data/tu_merges.csv merges applied."""
    out = _tu_map_raw()
    if out:
        _apply_tu_merges(out)
    return out


def _tu_map_raw() -> dict[int, str]:
    """address -> TU (e.g. 'field/FieldObject'). Empty if data/tu_map.csv does not exist yet."""
    out = {}
    rows = list(_read_flexible_csv(TU_MAP_CSV))
    if rows and _pick(rows[0], "start") and _pick(rows[0], "end") and not _pick(rows[0], "tu", "file", "name"):
        # Format of tools/triage/tu_map.py: start,end ranges -> name "unk/tu_<start>".
        idx = functions()
        for row in rows:
            try:
                lo, hi = parse_addr(_pick(row, "start")), parse_addr(_pick(row, "end"))
            except ValueError:
                continue
            name = f"unk/tu_{lo:x}"
            i = bisect.bisect_left(idx.starts, lo)
            while i < len(idx.funcs) and idx.funcs[i].addr < hi:
                out[idx.funcs[i].addr] = name
                i += 1
        return out
    for row in rows:
        a = _pick(row, "address", "addr", "start", "va")
        t = _pick(row, "tu", "translation_unit", "file", "unit", "path", "name")
        if a and t:
            try:
                out[parse_addr(a)] = t.strip()
            except ValueError:
                pass
    return out


GAME_CATEGORIES = {"game", "juego", "pla", "gamecode", "game_code", "unknown", "desconocido", ""}


def is_decomp_target(f: Func) -> bool:
    """Should this function be decompiled? Uses data/triage.csv if present; otherwise a heuristic."""
    if f.quality == "L":
        return False
    t = triage()
    if t:
        c = t.get(f.addr)
        if c is not None:
            return c.lower() in GAME_CATEGORIES
    return guess_category(f) == "game"


_LIB_PREFIXES = ("nn::", "_ZN2nn", "std::", "_ZNSt", "_ZNKSt", "_ZSt", "grpc", "_ZN4grpc", "google::", "_ZN6google",
                 "hk", "_ZN2hk", "_ZNK2hk", "pia", "_ZN3nn", "flatbuffers", "_ZN11flatbuffers", "lua", "absl",
                 "_ZN4absl", "nlib", "Havok", "_ZN4nlib", "__cxx", "_ZN10__cxxabiv1", "nvn", "_ZN3nvn", "curl",
                 "_ZN3sead", "sead::", "_ZN5Trinity")


def guess_category(f: Func) -> str:
    """Heuristic while data/triage.csv does not exist: 'stub', 'library' or 'game'."""
    if f.size <= 0:
        return "stub"
    n = f.name or ""
    if n.startswith("_DT_") or (f.size == 16 and 0x71032A2370 <= f.addr < 0x71032A5690):
        return "stub"  # .init/.fini and PLT stubs
    if GENERIC_NAME_RE.match(n) or not n:
        return "game"
    if n.startswith(_LIB_PREFIXES):
        return "library"
    # Provisional RTTI names: the class says where it comes from.
    base = n.split("::")[0]
    if base in ("nn", "std", "grpc", "google", "absl", "flatbuffers", "nlib", "pia", "nvn", "sead"):
        return "library"
    if base.startswith("hk"):
        return "library"
    return "game"


def category(f: Func) -> str:
    t = triage()
    if t and f.addr in t:
        return t[f.addr]
    return guess_category(f)


# ------------------------------------------------------------------ locking
class FileLock:
    """Cross-process file lock (exclusive creation). Cleans up orphaned locks (> stale s)."""

    def __init__(self, path: Path, timeout: float = 120.0, stale: float = 300.0):
        self.path = Path(str(path) + ".lock")
        self.timeout = timeout
        self.stale = stale

    def __enter__(self):
        t0 = time.time()
        while True:
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, f"{os.getpid()} {time.time()}\n".encode())
                os.close(fd)
                return self
            except (FileExistsError, PermissionError):
                # On Windows, creating the lock while another process is deleting it ("delete pending")
                # gives PermissionError (WinError 5) instead of FileExistsError: same thing, retry.
                try:
                    if time.time() - self.path.stat().st_mtime > self.stale:
                        self.path.unlink()
                        continue
                except FileNotFoundError:
                    continue
                except PermissionError:
                    pass
                if time.time() - t0 > self.timeout:
                    raise TimeoutError(f"could not acquire {self.path} within {self.timeout} s")
                time.sleep(0.05)

    def __exit__(self, *exc):
        for i in range(200):
            try:
                self.path.unlink()
                return
            except FileNotFoundError:
                return
            except PermissionError:   # antivirus/indexer holding the file open: retry
                time.sleep(0.01 * (1 + i % 10))


REPLACE_TIMEOUT = 90.0     # seconds of os.replace retries before falling back to in-place writing


def _clean_stale_tmps(path: Path, older_than: float = 600.0):
    """Deletes orphaned <file>.tmp<pid>_<thread> files (from processes that died mid-write)."""
    try:
        now = time.time()
        for p in path.parent.glob(path.name + ".tmp*"):
            try:
                if now - p.stat().st_mtime > older_than:
                    p.unlink()
            except OSError:
                pass
    except OSError:
        pass


def atomic_write_text(path: Path, text: str, timeout: float = REPLACE_TIMEOUT):
    """Writes `text` to `path` atomically (temp file + os.replace).

    On Windows, os.replace fails with PermissionError (WinError 5/32) while another process has the
    target open (e.g. another verify.py reading the CSV). It is retried with exponential backoff with
    random jitter for `timeout` seconds; if it still fails, it writes in place
    (truncate + write: Python opens with FILE_SHARE_READ|WRITE, so it works even with
    readers) and the temp file is deleted. For shared files, call with their FileLock held
    (see `locked_write_text`)."""
    import random
    import threading
    path = Path(path)
    tmp = path.with_name(path.name + f".tmp{os.getpid()}_{threading.get_ident()}")
    data = text.encode("utf-8")
    tmp.write_bytes(data)
    t0 = time.time()
    delay = 0.005
    try:
        while True:
            try:
                os.replace(tmp, path)
                return
            except PermissionError:
                if time.time() - t0 > timeout:
                    break
                time.sleep(delay * (0.5 + random.random()))
                delay = min(delay * 1.6, 0.5)
        # last resort: in-place write (under the caller's lock, writers do not compete)
        for _ in range(200):
            try:
                with open(path, "r+b" if path.exists() else "wb") as fh:
                    fh.write(data)
                    fh.truncate()
                return
            except PermissionError:
                time.sleep(0.1 * (0.5 + random.random()))
        raise PermissionError(f"could not replace {path} (another process holds it open)")
    finally:
        try:
            if tmp.exists():
                tmp.unlink()
        except OSError:
            pass
        _clean_stale_tmps(path)


def locked_write_text(path: Path, text: str):
    """atomic_write_text under the file's FileLock (for one-off shared writes)."""
    with FileLock(path):
        atomic_write_text(path, text)


def read_text_shared(path: Path, retries: float = 30.0) -> str | None:
    """Reads a shared file in one go (keeps it open for the minimum time, so as not to block
    other processes' os.replace on Windows) and retries if it is mid-replace.
    Returns None if it does not exist."""
    import random
    t0 = time.time()
    delay = 0.005
    while True:
        try:
            with open(path, "rb") as fh:
                data = fh.read()
            return data.decode("utf-8", errors="replace")
        except FileNotFoundError:
            # during os.replace on Windows the target may briefly "disappear"
            if time.time() - t0 > 0.5:
                return None
        except PermissionError:
            if time.time() - t0 > retries:
                raise
        time.sleep(delay * (0.5 + random.random()))
        delay = min(delay * 1.6, 0.25)


def agent_name(cli: str | None = None) -> str:
    """--agent > PLA_AGENT variable > .pla_agent file (current directory or repo root) > user."""
    if cli:
        return cli
    if os.environ.get("PLA_AGENT"):
        return os.environ["PLA_AGENT"]
    for d in (Path.cwd(), ROOT):
        try:
            t = (d / ".pla_agent").read_text(encoding="utf-8").strip()
            if t:
                return t.split()[0]
        except OSError:
            pass
    return os.environ.get("USERNAME") or os.environ.get("USER") or "anon"


def load_json(p: Path, default=None):
    try:
        return json.loads(Path(p).read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return default
