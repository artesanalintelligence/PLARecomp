#!/usr/bin/env python3
"""Identifies the user's dump without extracting it: format, titles, version and build id.

Reuses tools/extract (nxfmt + extract) for PFS0/HFS0/NCA/tickets/CNMT/NACP. The only own
code reads NCZ (the compressed NCAs of an NSZ/XCZ) in memory, so the update's build id can be
obtained without decompressing the base game NSZ (6 GB). It never writes anything to disk nor
shows keys.

Standalone use (the launcher calls it the same way):
  python tools/launcher/identify.py <file or folder> [...] [--profile pla-1.1.1] [--json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "extract"))

import nxfmt as nx  # noqa: E402
import extract as ex  # noqa: E402

NCZ_BODY = 0x4000                 # the first 0x4000 bytes of the NCA are stored as-is (encrypted)
NCZ_MEMORY_LIMIT = 512 << 20      # largest decompressed body that is read into memory


def utf8_console() -> None:
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def load_profile(name: str) -> dict:
    p = Path(name)
    if not p.is_file():
        p = HERE / "profiles" / f"{name}.json"
    return json.loads(p.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# Format
# ---------------------------------------------------------------------------

def detect_format(p: Path) -> str:
    if p.is_dir():
        return "exefs-dir" if (p / "main").is_file() and (p / "main.npdm").is_file() else "folder"
    try:
        with open(p, "rb") as f:
            head = f.read(0x4010)
    except OSError:
        return "unreadable"
    ext = p.suffix.lower()
    if head[:4] == b"PFS0":
        return "nsz" if ext == ".nsz" else "nsp"
    if head[0x100:0x104] == b"HEAD":
        return "xcz" if ext == ".xcz" else "xci"
    if head[NCZ_BODY:NCZ_BODY + 8] == b"NCZSECTN":
        return "ncz"
    if ext == ".nca":
        return "nca"
    if ext == ".tik":
        return "tik"
    return "unknown"


# ---------------------------------------------------------------------------
# NCZ (nsz format): original NCA header + decrypted sections compressed with zstd
# ---------------------------------------------------------------------------

class NczBody:
    """Decompressed body (from 0x4000) of an NCZ, loaded into memory on demand."""

    def __init__(self, region: nx.FileRegion):
        self.region = region
        magic, count = struct.unpack("<8sQ", region.read(NCZ_BODY, 0x10))
        if magic != b"NCZSECTN":
            raise nx.FormatError(f"{region.name}: not an NCZ")
        self.pos = NCZ_BODY + 0x10 + 0x40 * count
        secs = [struct.unpack_from("<QQ", region.read(NCZ_BODY + 0x10 + 0x40 * i, 0x10)) for i in range(count)]
        self.size = max((o + s for o, s in secs), default=NCZ_BODY) - NCZ_BODY
        self._data: Optional[bytes] = None

    def data(self) -> bytes:
        if self._data is not None:
            return self._data
        if self.size > NCZ_MEMORY_LIMIT:
            raise nx.FormatError(f"{self.region.name}: NCZ of {nx.human(self.size)}; too large to read "
                                 "into memory (it is identified after decompressing)")
        try:
            import zstandard
        except ImportError:
            raise nx.FormatError("the 'zstandard' module is missing (python -m pip install nsz)")
        r = self.region
        rest = r.read(self.pos, r.size - self.pos)
        if rest[:8] == b"NCZBLOCK":
            _v, _t, _u, exp, n = struct.unpack_from("<BBBBI", rest, 8)
            total = struct.unpack_from("<Q", rest, 0x10)[0]
            sizes = struct.unpack_from(f"<{n}I", rest, 0x18)
            off, out, bs = 0x18 + 4 * n, bytearray(), 1 << exp
            d = zstandard.ZstdDecompressor()
            for i, cs in enumerate(sizes):
                want = min(bs, total - i * bs)
                blk = rest[off:off + cs]
                out += blk if cs >= want else d.decompress(blk, max_output_size=want)
                off += cs
            self._data = bytes(out)
        else:
            with zstandard.ZstdDecompressor().stream_reader(rest) as s:
                self._data = s.read(self.size + (1 << 20))
        return self._data


class NczNca(nx.Nca):
    """nx.Nca whose sections are read from the (already decrypted) NCZ body instead of being decrypted."""

    def __init__(self, region: nx.FileRegion, keys: nx.KeySet):
        super().__init__(region, keys)
        self.body = NczBody(region)

    def read_section(self, sec, off, size, f=None):
        a = sec.start + off
        if a < NCZ_BODY:
            return super().read_section(sec, off, size, f)
        d = self.body.data()
        a -= NCZ_BODY
        if a + size > len(d):
            raise nx.FormatError(f"{self.region.name}: read outside the NCZ body")
        return d[a:a + size]


# ---------------------------------------------------------------------------
# Inventory
# ---------------------------------------------------------------------------

@dataclass
class InputFile:
    path: Path
    format: str
    size: int
    ncz_members: int = 0


@dataclass
class Inventory:
    files: List[InputFile] = field(default_factory=list)
    ncas: List[ex.NcaEntry] = field(default_factory=list)
    ncz: set = field(default_factory=set)           # ids of the NcaEntry objects that are NCZ
    tickets: list = field(default_factory=list)
    exefs_dirs: List[Path] = field(default_factory=list)


def _members(inv: Inventory, src: InputFile, regions) -> None:
    for f in regions:
        low = f.name.lower()
        if low.endswith(".nca"):
            inv.ncas.append(ex.NcaEntry(f, src.path))
        elif low.endswith(".ncz"):
            e = ex.NcaEntry(f, src.path)
            inv.ncas.append(e)
            inv.ncz.add(id(e))
            src.ncz_members += 1
        elif low.endswith(".tik"):
            inv.tickets.append((f, src.path))


def take_inventory(inputs: List[Path]) -> Inventory:
    inv = Inventory()
    paths: List[Path] = []
    for e in inputs:
        if e.is_dir() and detect_format(e) != "exefs-dir":
            paths += sorted(p for p in e.rglob("*") if p.is_file() and ex.WORKDIR_NAME not in p.parts)
            paths += sorted(p for p in e.rglob("*") if p.is_dir() and detect_format(p) == "exefs-dir")
        else:
            paths.append(e)
    for p in paths:
        fmt = detect_format(p)
        a = InputFile(p, fmt, p.stat().st_size if p.is_file() else 0)
        try:
            if fmt in ("nsp", "nsz"):
                _members(inv, a, nx.parse_pfs0(nx.FileRegion(p, 0, a.size, p.name)))
            elif fmt in ("xci", "xcz"):
                for part, files in nx.parse_xci(p).items():
                    if part != "update":
                        _members(inv, a, files)
            elif fmt == "nca":
                inv.ncas.append(ex.NcaEntry(nx.FileRegion(p, 0, a.size, p.name), p))
            elif fmt == "ncz":
                e = ex.NcaEntry(nx.FileRegion(p, 0, a.size, p.name), p)
                inv.ncas.append(e)
                inv.ncz.add(id(e))
            elif fmt == "tik":
                inv.tickets.append((nx.FileRegion(p, 0, a.size, p.name), p))
            elif fmt == "exefs-dir":
                inv.exefs_dirs.append(p)
            else:
                continue
        except nx.FormatError as err:
            a.format += f" (error: {err})"
        inv.files.append(a)
    return inv


# ---------------------------------------------------------------------------
# ExeFS: build id and hashes
# ---------------------------------------------------------------------------

def nso_build_id(header: bytes) -> Optional[str]:
    if header[:4] != b"NSO0":
        return None
    return header[0x40:0x54].hex().upper()   # 20 significant bytes (the rest is padding)


def exefs_from_nca(nca: nx.Nca, with_hash: bool) -> Dict[str, dict]:
    out = {}
    for f in nx.nca_pfs0_files(nca, 0):
        d = {"size": f.size}
        if f.size >= 0x100:
            d["build_id"] = nso_build_id(f.read(0, 0x100))
        if with_hash:
            h = hashlib.sha256()
            with f.open() as fh:
                pos = 0
                while pos < f.size:
                    n = min(8 << 20, f.size - pos)
                    h.update(f.read(pos, n, fh))
                    pos += n
            d["sha256"] = h.hexdigest()
        out[f.name] = d
    return out


def exefs_from_folder(d: Path) -> Dict[str, dict]:
    out = {}
    for p in sorted(d.iterdir()):
        if p.is_file() and p.name in ex.EXEFS_NAMES:
            data = p.read_bytes()
            out[p.name] = {"size": len(data), "build_id": nso_build_id(data[:0x100]),
                           "sha256": hashlib.sha256(data).hexdigest()}
    return out


def compare_exefs(exefs: Dict[str, dict], profile: dict) -> List[str]:
    """List of differences against the profile (empty = match)."""
    diff = []
    for name, exp in profile.get("expected_exefs", {}).items():
        got = exefs.get(name)
        if not got:
            diff.append(f"{name} is missing")
            continue
        if exp.get("build_id") and got.get("build_id") and got["build_id"][:40] != exp["build_id"]:
            diff.append(f"{name}: build id {got['build_id'][:16]}... (expected {exp['build_id'][:16]}...)")
        if exp.get("sha256") and got.get("sha256") and got["sha256"] != exp["sha256"]:
            diff.append(f"{name}: different sha256")
    return diff


# ---------------------------------------------------------------------------
# Full identification
# ---------------------------------------------------------------------------

def identify(inputs: List[Path], profile: dict, prod: Optional[Path] = None,
             title: Optional[Path] = None, with_hash: bool = True) -> dict:
    nx.need_crypto()
    r: dict = {"profile": profile["id"], "files": [], "titles": [], "warnings": [], "state": "unknown"}
    inv = take_inventory(inputs)
    r["files"] = [{"path": str(a.path), "format": a.format, "size": a.size,
                   "ncz": a.ncz_members} for a in inv.files]
    if not inv.ncas and inv.exefs_dirs:
        exefs = exefs_from_folder(inv.exefs_dirs[0])
        r.update(source="exefs-dir", exefs=exefs, differences=compare_exefs(exefs, profile))
        r["state"] = "supported" if not r["differences"] else "unsupported"
        r["warnings"].append("Loose ExeFS: enough for the recompilation, but the RomFS has to be provided separately")
        return r
    if not inv.ncas:
        r["state"] = "empty"
        r["warnings"].append("No NSP/NSZ/XCI/XCZ/NCA was found in the input.")
        return r

    try:
        keys = ex.load_keys(prod, title)
    except ex.Fatal as e:
        r["state"] = "no keys"
        r["warnings"].append(str(e))
        return r
    for reg, _src in inv.tickets:
        try:
            t = nx.parse_ticket(reg.read(0, reg.size))
            keys.add_titlekey(t.rights_id, nx.ticket_titlekey(t, keys))
        except (nx.FormatError, nx.KeyError_) as e:
            r["warnings"].append(f"ticket {reg.name}: {e}")
    opened = []
    for e in inv.ncas:
        try:
            e.nca = NczNca(e.region, keys) if id(e) in inv.ncz else nx.Nca(e.region, keys)
            opened.append(e)
        except (nx.FormatError, nx.KeyError_) as err:
            r["warnings"].append(f"{e.region.location}: {err}")
    key_errors: list = []
    titles = ex.read_cnmts(opened, key_errors)
    tid = int(profile["title_id"], 16)
    tid_upd = int(profile["update_title_id"], 16)
    base = next((t for t in titles if t.cnmt.title_id == tid and t.cnmt.meta_type == 0x80), None)
    upds = sorted((t for t in titles if t.cnmt.title_id == tid_upd and t.cnmt.meta_type == 0x81),
                  key=lambda t: t.cnmt.version)
    upd = upds[-1] if upds else None
    for t in titles:
        r["titles"].append({"title_id": nx.tid_str(t.cnmt.title_id), "type": t.cnmt.type_name,
                            "version": t.cnmt.version, "file": str(t.meta.source)})

    def nacp_version(t):
        if not t or 3 not in t.contents:
            return None, None
        try:
            n = ex.read_nacp(t.contents[3])
        except (nx.FormatError, nx.KeyError_):
            n = None
        return (nx.nacp_display_version(n), nx.nacp_title(n)) if n else (None, None)

    vb, name_b = nacp_version(base)
    vu, name_u = nacp_version(upd)
    r["game"] = name_u or name_b
    r["base"] = None if not base else {
        "version": base.cnmt.version, "display": vb, "file": str(base.meta.source),
        "program": bool(base.contents.get(1)),
        "program_compressed": bool(base.contents.get(1)) and id(base.contents[1]) in inv.ncz,
        "program_size": base.contents[1].region.size if base.contents.get(1) else 0}
    r["update"] = None if not upd else {
        "version": upd.cnmt.version, "display": vu, "file": str(upd.meta.source),
        "program": bool(upd.contents.get(1)),
        "program_compressed": bool(upd.contents.get(1)) and id(upd.contents[1]) in inv.ncz}

    if key_errors and (not base or not upd):
        import re
        names = sorted(set(re.findall(r"Missing key '([^']+)'", " ".join(key_errors))))
        r["state"] = "no keys"
        r["warnings"].append(
            "Your prod.keys cannot decrypt this dump" + (f" (missing {', '.join(names)})" if names else "") +
            ": the keys come from a console on an older firmware than the game needs. Dump the keys again from "
            "your own console with an up-to-date Lockpick_RCM and select the new prod.keys.")
        return r
    if not base:
        r["warnings"].append(f"The base game ({profile['title_id']}) is missing: it is needed for the RomFS.")
    if not upd:
        r["state"] = "incomplete"
        r["warnings"].append(f"The update {profile['version']['display']} ({profile['update_title_id']}) is missing.")
        return r
    if upd.cnmt.version != profile["version"]["number"]:
        r["state"] = "unsupported"
        r["warnings"].append(f"The update is v{upd.cnmt.version} ({vu}); the profile supports "
                             f"v{profile['version']['number']} ({profile['version']['display']}).")
        return r
    prog = upd.contents.get(1)
    if not prog:
        r["state"] = "incomplete"
        r["warnings"].append("The update CNMT does not have its program NCA in the input.")
        return r
    try:
        exefs = exefs_from_nca(prog.nca, with_hash)
    except (nx.FormatError, nx.KeyError_) as err:
        r["state"] = "pending"
        r["warnings"].append(f"Could not read the update ExeFS: {err}")
        return r
    r["source"] = "update"
    r["exefs"] = exefs
    r["differences"] = compare_exefs(exefs, profile)
    r["state"] = "supported" if not r["differences"] and base else (
        "incomplete" if not r["differences"] else "unsupported")
    return r


def report(r: dict) -> str:
    L = [f"Profile: {r['profile']}  ->  state: {r['state'].upper()}"]
    for a in r["files"]:
        extra = f", {a['ncz']} compressed NCAs" if a["ncz"] else ""
        L.append(f"  [{a['format']}] {a['path']} ({nx.human(a['size'])}{extra})")
    if r.get("game"):
        L.append(f"  game: {r['game']}")
    for t in r.get("titles", []):
        L.append(f"  title {t['title_id']} {t['type']:<12} v{t['version']}")
    for k in ("base", "update"):
        v = r.get(k)
        if v:
            comp = " (compressed program: needs decompressing)" if v["program_compressed"] else ""
            L.append(f"  {k}: v{v['version']} {v['display'] or ''}{comp}")
    if r.get("exefs"):
        m = r["exefs"].get("main", {})
        L.append(f"  main build id: {(m.get('build_id') or '?')[:40]}")
        L.append(f"  ExeFS: {len(r['exefs'])} files; hashes " +
                 ("MATCH the profile" if not r.get("differences") else "DO NOT match: " + "; ".join(r["differences"])))
    for a in r.get("warnings", []):
        L.append(f"  warning: {a}")
    return "\n".join(L)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Identifies a dump (NSP/NSZ/XCI/XCZ/NCA/ExeFS) without extracting it.")
    ap.add_argument("inputs", nargs="+", type=Path)
    ap.add_argument("--profile", default="pla-1.1.1")
    ap.add_argument("--keys", type=Path)
    ap.add_argument("--title-keys", type=Path)
    ap.add_argument("--no-hash", action="store_true", help="build id only, without the ExeFS SHA-256")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    utf8_console()
    r = identify(a.inputs, load_profile(a.profile), a.keys, a.title_keys, not a.no_hash)
    print(json.dumps(r, indent=2, ensure_ascii=False) if a.json else report(r))
    return 0 if r["state"] == "supported" else 2


if __name__ == "__main__":
    sys.exit(main())
