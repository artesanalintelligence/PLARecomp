"""Per-function decompilation status: data/decomp_status.csv (source of truth of the functional decomp).

It only holds the functions someone touched (claimed or with code); the rest are U.
Columns:
  address     0x7100123456
  size        original size in bytes
  quality     U (pending) | W (written, unverified or failing) | F (functional, verified) | O (matching)
  agent       who has it claimed or verified it
  claimed_at  ISO-8601 of the claim (empty if not claimed)
  updated_at  ISO-8601 of the last change
  file        src/ file with the definition
  symbol      mangled symbol of our definition
  coverage    % of the original's instructions executed during verification
  cases       test cases that passed / total
  notes       reason for the last failure, etc. (no commas)

Every write goes through `update()` / `transaction()`, which take data/decomp_status.csv.lock.
"""
from __future__ import annotations

import csv
import io
import sys
import time
from contextlib import contextmanager
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import STATUS_CSV, SYMBOLS_CSV, FileLock, atomic_write_text, fmt, read_text_shared  # noqa: E402

FIELDS = ["address", "size", "quality", "agent", "claimed_at", "updated_at", "file", "symbol", "coverage",
          "cases", "notes"]
RANK = {"U": 0, "W": 1, "F": 2, "O": 3}


def now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def read(path: Path = STATUS_CSV) -> dict[int, dict]:
    # The file is read in one go (read_text_shared) and parsed in memory: keeping it open
    # while parsing blocks the writers' os.replace on Windows (WinError 5).
    text = read_text_shared(path) if path.exists() else None
    if not text:
        return {}
    out = {}
    for row in csv.DictReader(io.StringIO(text, newline="")):
        try:
            out[int(row["address"], 16)] = {k: row.get(k, "") or "" for k in FIELDS}
        except (KeyError, ValueError, TypeError):
            continue
    return out


def write(rows: dict[int, dict], path: Path = STATUS_CSV):
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=FIELDS, lineterminator="\n")
    w.writeheader()
    for a in sorted(rows):
        r = rows[a]
        r["address"] = fmt(a)
        w.writerow({k: str(r.get(k, "")).replace(",", ";").replace("\n", " ") for k in FIELDS})
    atomic_write_text(path, buf.getvalue())


@contextmanager
def transaction(path: Path = STATUS_CSV):
    with FileLock(path):
        rows = read(path)
        yield rows
        write(rows, path)


def update_many(updates):
    """Several updates [(addr, size, {field: value})] in a single transaction (one
    lock and one CSV write). Fields with value None are left untouched."""
    out = []
    updates = list(updates)
    if not updates:
        return out
    with transaction() as rows:
        for addr, size, fields in updates:
            r = rows.setdefault(addr, {k: "" for k in FIELDS})
            if not r.get("quality"):
                r["quality"] = "U"
            if size is not None:
                r["size"] = str(size)
            for k, v in fields.items():
                if k in FIELDS and v is not None:
                    r[k] = str(v)
            r["updated_at"] = now()
            out.append(dict(r))
    return out


def update(addr: int, size: int | None = None, **fields):
    with transaction() as rows:
        r = rows.setdefault(addr, {k: "" for k in FIELDS})
        if not r.get("quality"):
            r["quality"] = "U"
        if size is not None:
            r["size"] = str(size)
        for k, v in fields.items():
            if k in FIELDS and v is not None:
                r[k] = str(v)
        r["updated_at"] = now()
        return dict(r)


def register_symbol(addr: int, symbol: str, demangled: str, file: str):
    """data/decomp_symbols.csv: symbols of our definitions (to sync with viking)."""
    register_symbols([(addr, symbol, demangled, file)])


def register_symbols(items):
    """Like register_symbol, but several entries (addr, symbol, demangled, file) in a single write."""
    items = list(items)
    if not items:
        return
    with FileLock(SYMBOLS_CSV):
        rows = {}
        text = read_text_shared(SYMBOLS_CSV) if SYMBOLS_CSV.exists() else None
        for row in csv.DictReader(io.StringIO(text or "", newline="")):
            if row.get("address"):
                rows[row["address"]] = row
        for addr, symbol, demangled, file in items:
            rows[fmt(addr)] = {"address": fmt(addr), "symbol": symbol, "demangled": demangled.replace(",", ";"),
                               "file": file}
        buf = io.StringIO()
        w = csv.DictWriter(buf, fieldnames=["address", "symbol", "demangled", "file"], lineterminator="\n",
                           extrasaction="ignore")
        w.writeheader()
        for k in sorted(rows):
            w.writerow(rows[k])
        atomic_write_text(SYMBOLS_CSV, buf.getvalue())


_SYM_CACHE: tuple | None = None


def symbols_by_name() -> dict[str, int]:
    # Cache keyed by (size, mtime): verify.py asks for it for every function and, on Windows, keeping the CSV
    # open so often prevents other processes from replacing it (os.replace -> WinError 5).
    global _SYM_CACHE
    try:
        stt = SYMBOLS_CSV.stat()
    except FileNotFoundError:
        return {}
    key = (stt.st_size, stt.st_mtime_ns)
    if _SYM_CACHE is not None and _SYM_CACHE[0] == key:
        return dict(_SYM_CACHE[1])
    d = {}
    for r in csv.DictReader(io.StringIO(read_text_shared(SYMBOLS_CSV) or "", newline="")):
        try:
            if r.get("symbol"):
                d[r["symbol"]] = int(r["address"], 16)
        except (KeyError, ValueError, TypeError):
            pass
    _SYM_CACHE = (key, d)
    return dict(d)
