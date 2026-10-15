#!/usr/bin/env python3
"""Extracts the ExeFS (and optionally the RomFS) of the game (title ID 01001F5010DFA000).

Typical usage (from the repo root):

    python tools/extract/extract.py

Looks in dump/ for any combination of: XCI, NSP, NSZ/XCZ (requires `nsz`),
loose NCAs (+ .tik) or an already extracted ExeFS folder; applies the update on
top of the base game and generates:

    data/exefs/{main,main.npdm,rtld,sdk,subsdk*}
    data/main.nso          copy of exefs/main
    data/main.elf          ELF for Ghidra/IDA/viking (nso2elf.py)
    data/target.json       version, SHA-256, build IDs and segments
    data/romfs/            only with --romfs (needs hactoolnet or hactool)

The ExeFS is decrypted in Python (nxfmt.py). If hactoolnet/hactool is in
tools/bin (see bootstrap.py), it is also used to verify the ExeFS
independently and for the patched RomFS (BKTR).
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nxfmt as nx  # noqa: E402

REPO = HERE.parent.parent
PLA_TITLE_ID = 0x01001F5010DFA000
PLA_TARGET_VERSION = "1.1.1"
PLA_TARGET_VERSION_NUMBER = 262144  # v262144 = 1.1.1 (fourth update)
EXEFS_NAMES = ("main", "main.npdm", "rtld", "sdk") + tuple(f"subsdk{i}" for i in range(10))
WORKDIR_NAME = ".extract-work"


# ---------------------------------------------------------------------------
# Messages
# ---------------------------------------------------------------------------

def info(msg: str) -> None:
    print(f"[extract] {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"[extract] WARNING: {msg}", file=sys.stderr, flush=True)


class Fatal(Exception):
    pass


# ---------------------------------------------------------------------------
# Dump inventory
# ---------------------------------------------------------------------------

@dataclass
class NcaEntry:
    region: nx.FileRegion
    source: Path
    nca: Optional[nx.Nca] = None

    @property
    def content_id(self) -> Optional[str]:
        stem = self.region.name.split(".")[0].lower()
        if len(stem) == 32 and all(c in "0123456789abcdef" for c in stem):
            return stem
        return None


@dataclass
class Inventory:
    ncas: List[NcaEntry] = field(default_factory=list)
    tickets: List[Tuple[nx.FileRegion, Path]] = field(default_factory=list)
    compressed: List[Path] = field(default_factory=list)   # NSZ/XCZ/NCZ
    exefs_dirs: List[Path] = field(default_factory=list)
    unknown: List[Path] = field(default_factory=list)


def scan_dump(dump: Path, workdir: Path) -> Inventory:
    inv = Inventory()
    if not dump.is_dir():
        raise Fatal(f"Folder {dump} does not exist. Create dump/ and copy your XCI/NSP there.")
    for p in sorted(dump.rglob("*")):
        if workdir in p.parents or p == workdir:
            continue
        if p.is_dir():
            if (p / "main").is_file() and (p / "main.npdm").is_file():
                inv.exefs_dirs.append(p)
            continue
        ext = p.suffix.lower()
        try:
            if ext == ".nsp":
                for f in nx.parse_pfs0(nx.FileRegion(p, 0, p.stat().st_size, p.name)):
                    _add_member(inv, f, p)
            elif ext == ".xci":
                for part, files in nx.parse_xci(p).items():
                    if part == "update":
                        continue  # system update, not the game's
                    for f in files:
                        _add_member(inv, f, p)
            elif ext in (".nsz", ".xcz", ".ncz"):
                inv.compressed.append(p)
            elif ext == ".nca":
                inv.ncas.append(NcaEntry(nx.FileRegion(p, 0, p.stat().st_size, p.name), p))
            elif ext == ".tik":
                inv.tickets.append((nx.FileRegion(p, 0, p.stat().st_size, p.name), p))
            elif p.name in EXEFS_NAMES or p.name == ".gitkeep":
                pass
            else:
                inv.unknown.append(p)
        except nx.FormatError as e:
            warn(f"{p.name}: {e}")
    return inv


def _add_member(inv: Inventory, f: nx.FileRegion, src: Path) -> None:
    low = f.name.lower()
    if low.endswith(".nca"):
        inv.ncas.append(NcaEntry(f, src))
    elif low.endswith(".tik"):
        inv.tickets.append((f, src))
    elif low.endswith(".ncz"):
        if src not in inv.compressed:
            inv.compressed.append(src)


def find_nsz() -> Optional[List[str]]:
    # Prefer the module importable by this interpreter (the installer's bundled Python ships it): a stray
    # nsz.exe on PATH or in the user's Python folder may belong to another interpreter without the module.
    try:
        import nsz  # noqa: F401
        import importlib.util
        if importlib.util.find_spec("nsz.__main__"):
            return [sys.executable, "-m", "nsz"]
        # The pip wheel (nsz 5.x) only ships a console script, no __main__.
        return [sys.executable, "-c", "import sys; from nsz import main; sys.argv[0] = 'nsz'; main()"]
    except ImportError:
        pass
    exe = shutil.which("nsz")
    if exe:
        return [exe]
    appdata = os.environ.get("APPDATA")
    if appdata:
        for c in Path(appdata).glob("Python/Python3*/Scripts/nsz.exe"):
            return [str(c)]
    return None


def decompress_nsz(paths: List[Path], dump: Path) -> List[Path]:
    """Decompress NSZ/XCZ/NCZ in dump/ (so next time it is no longer needed)."""
    cmd = find_nsz()
    if not cmd:
        raise Fatal(
            "There are compressed files (NSZ/XCZ/NCZ) in dump/ and 'nsz' was not found.\n"
            "  Install it with:  python -m pip install nsz\n"
            "  or decompress them yourself:  nsz -D -o dump <file.nsz>")
    out = dump if all(p.parent == dump for p in paths) else paths[0].parent
    before = set(out.iterdir())
    produced = []
    for p in paths:
        info(f"decompressing {p.name} with nsz (may take a while)...")
        r = subprocess.run(cmd + ["-D", "-w", "-o", str(out), str(p)])
        if r.returncode != 0:
            raise Fatal(f"nsz failed on {p.name} (code {r.returncode}).")
    for q in out.iterdir():
        if q not in before and q.suffix.lower() in (".nsp", ".xci", ".nca"):
            produced.append(q)
    return produced


# ---------------------------------------------------------------------------
# Keys
# ---------------------------------------------------------------------------

def default_keys_dir() -> Path:
    return Path(os.environ.get("USERPROFILE") or Path.home()) / ".switch"


def load_keys(prod: Optional[Path], title: Optional[Path]) -> nx.KeySet:
    kdir = default_keys_dir()
    prod = prod or kdir / "prod.keys"
    title = title or kdir / "title.keys"
    if not prod.is_file():
        alt = kdir / "keys.txt"
        if alt.is_file():
            prod = alt
        else:
            raise Fatal(
                f"{prod} not found.\n"
                "  Dump the keys from YOUR console with Lockpick_RCM and copy prod.keys to\n"
                f"  {kdir}\\prod.keys  (or give the path with --keys).")
    ks = nx.KeySet.load(prod, title if title.is_file() else None)
    if not ks.has("header_key"):
        raise Fatal(f"{prod} does not contain 'header_key'. The key file is incomplete.")
    return ks


# ---------------------------------------------------------------------------
# Content selection
# ---------------------------------------------------------------------------

@dataclass
class Title:
    cnmt: nx.Cnmt
    meta: NcaEntry
    contents: Dict[int, NcaEntry]  # ctype -> NcaEntry (Program=1, Control=3)


def open_ncas(inv: Inventory, keys: nx.KeySet) -> List[NcaEntry]:
    ok = []
    for e in inv.ncas:
        try:
            e.nca = nx.Nca(e.region, keys)
            ok.append(e)
        except nx.KeyError_ as ex:
            raise Fatal(str(ex))
        except nx.FormatError as ex:
            warn(f"{e.region.location}: {ex}")
    return ok


def read_cnmts(ncas: List[NcaEntry], key_errors: Optional[list] = None) -> List[Title]:
    by_id = {e.content_id: e for e in ncas if e.content_id}
    titles = []
    for e in ncas:
        if e.nca.content_type != 1:
            continue
        try:
            for f in nx.nca_pfs0_files(e.nca):
                if f.name.endswith(".cnmt"):
                    c = nx.parse_cnmt(f.read(0, f.size))
                    contents = {}
                    for r in c.contents:
                        if r.content_id in by_id and r.ctype not in contents:
                            contents[r.ctype] = by_id[r.content_id]
                    titles.append(Title(c, e, contents))
        except (nx.FormatError, nx.KeyError_) as ex:
            warn(f"{e.region.location}: could not read the CNMT: {ex}")
            if key_errors is not None and isinstance(ex, nx.KeyError_):
                key_errors.append(str(ex))
    return titles


def read_nacp(entry: Optional[NcaEntry]) -> Optional[bytes]:
    if not entry:
        return None
    try:
        view = entry.nca.section_view(0)
        f = nx.romfs_find(view, "control.nacp")
        return f.read(0, f.size) if f else None
    except (nx.FormatError, nx.KeyError_) as ex:
        warn(f"could not read control.nacp: {ex}")
        return None


# ---------------------------------------------------------------------------
# External tools (hactoolnet / hactool)
# ---------------------------------------------------------------------------

@dataclass
class Tool:
    kind: str   # "hactoolnet" | "hactool"
    path: Path


def find_tool(pref: str) -> Optional[Tool]:
    if pref == "none":
        return None
    bindir = REPO / "tools" / "bin"
    order = ["hactoolnet", "hactool"] if pref == "auto" else [pref]
    for kind in order:
        for cand in (bindir / f"{kind}.exe", bindir / kind, bindir / kind / f"{kind}.exe"):
            if cand.is_file():
                return Tool(kind, cand)
        w = shutil.which(kind)
        if w:
            return Tool(kind, Path(w))
    if pref not in ("auto",):
        raise Fatal(f"{pref} not found. Run: python tools/extract/bootstrap.py")
    return None


class ToolEnv:
    """Temporary folder with prod.keys + title.keys (only the needed ones, without
    duplicates). Deleted when done. Keys are never printed."""

    def __init__(self, keys: nx.KeySet, rights: List[bytes]):
        self.keys = keys
        self.rights = rights

    def __enter__(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pla-keys-"))
        sw = self.tmp / ".switch"
        sw.mkdir()
        shutil.copyfile(self.keys.prod_path, sw / "prod.keys")
        with open(sw / "title.keys", "w") as f:
            for r in self.rights:
                k = self.keys.titlekeys.get(r)
                if k:
                    f.write(f"{r.hex()} = {k.hex()}\n")
        self.prod = sw / "prod.keys"
        self.title = sw / "title.keys"
        self.env = dict(os.environ, HOME=str(self.tmp), USERPROFILE=str(self.tmp))
        return self

    def __exit__(self, *a):
        shutil.rmtree(self.tmp, ignore_errors=True)


def run_tool(tool: Tool, env: ToolEnv, nca: Path, base: Optional[Path], exefs: Optional[Path],
             romfs: Optional[Path]) -> None:
    if tool.kind == "hactoolnet":
        cmd = [str(tool.path), "-k", str(env.prod), "--titlekeys", str(env.title), "-t", "nca"]
        if base:
            cmd += ["--basenca", str(base)]
        if exefs:
            cmd += ["--exefsdir", str(exefs)]
        if romfs:
            cmd += ["--romfsdir", str(romfs)]
        cmd.append(str(nca))
    else:
        cmd = [str(tool.path), "-k", str(env.prod), "--disablekeywarns", "--suppresskeys", "-t", "nca"]
        if base:
            cmd.append(f"--basenca={base}")
        if exefs:
            cmd.append(f"--exefsdir={exefs}")
        if romfs:
            cmd.append(f"--romfsdir={romfs}")
        cmd.append(str(nca))
    r = subprocess.run(cmd, env=env.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace")
    # The full output is never forwarded: hactool may print keys.
    if r.returncode != 0:
        tail = [l for l in r.stdout.splitlines() if "key" not in l.lower()][-8:]
        raise Fatal(f"{tool.kind} failed (code {r.returncode}):\n    " + "\n    ".join(tail))


def nca_as_file(entry: NcaEntry, workdir: Path) -> Path:
    """Return a path to a .nca on disk (copying it out of the container if needed)."""
    reg = entry.region
    if reg.offset == 0 and reg.path.stat().st_size == reg.size:
        return reg.path
    dest = workdir / "nca" / reg.name
    if dest.is_file() and dest.stat().st_size == reg.size:
        return dest
    info(f"copying {reg.name} ({nx.human(reg.size)}) to {dest.parent} ...")
    reg.copy_to(dest)
    return dest


# ---------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------

def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def npdm_info(data: bytes) -> dict:
    d = {}
    if data[:4] == b"META":
        d["name"] = data[0x20:0x30].split(b"\0")[0].decode("utf-8", "replace")
        aci_off = struct.unpack_from("<I", data, 0x70)[0]
        if data[aci_off:aci_off + 4] == b"ACI0":
            d["program_id"] = f"{struct.unpack_from('<Q', data, aci_off + 0x10)[0]:016X}"
    return d


def write_target_json(out: Path, meta: dict, exefs_dir: Path, elf_path: Optional[Path], elf_opts: dict) -> dict:
    import nso2elf
    exefs = {}
    sdk_versions = set()
    for name in sorted(p.name for p in exefs_dir.iterdir()):
        p = exefs_dir / name
        entry = {"size": p.stat().st_size, "sha256": sha256_file(p)}
        blob = p.read_bytes()
        if blob[:4] == b"NSO0":
            nso = nso2elf.parse_nso(blob)
            entry["build_id"] = nso.build_id
            entry["build_id_short"] = nso.build_id_short
            entry["segments"] = {s.name: {"mem_offset": f"0x{s.mem_offset:X}", "size": s.size,
                                          "file_size": s.file_size, "compressed": s.compressed,
                                          "sha256": hashlib.sha256(s.data).hexdigest()}
                                 for s in nso.segments}
            entry["bss_size"] = nso.bss_size
            if nso.warnings:
                entry["warnings"] = nso.warnings
            sdk_versions.update(nso2elf.find_sdk_versions(nso))
        elif name == "main.npdm":
            entry.update(npdm_info(blob))
        exefs[name] = entry
    doc = dict(meta)
    doc["sdk_versions"] = sorted(sdk_versions)
    doc["exefs"] = exefs
    if elf_path and elf_path.is_file():
        doc["main_elf"] = {"path": str(elf_path.relative_to(REPO)).replace("\\", "/") if REPO in elf_path.parents else str(elf_path),
                           "sha256": sha256_file(elf_path), "size": elf_path.stat().st_size, **elf_opts}
    doc["generated_by"] = "tools/extract/extract.py"
    doc["generated_at"] = _dt.datetime.now().astimezone().isoformat(timespec="seconds")
    (out / "target.json").write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return doc


# ---------------------------------------------------------------------------
# Main program
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Extract ExeFS/RomFS of the game and generate data/main.{nso,elf} and target.json")
    ap.add_argument("--dump", type=Path, default=REPO / "dump", help="folder with XCI/NSP/NCA (default dump/)")
    ap.add_argument("--out", type=Path, default=REPO / "data", help="output folder (default data/)")
    ap.add_argument("--keys", type=Path, help="prod.keys (default ~/.switch/prod.keys)")
    ap.add_argument("--title-keys", type=Path, help="title.keys (default ~/.switch/title.keys)")
    ap.add_argument("--title-id", type=lambda x: int(x, 16), default=PLA_TITLE_ID,
                    help="Base Title ID in hex (default 01001F5010DFA000)")
    ap.add_argument("--expect-version", default=None,
                    help=f"expected display version (default {PLA_TARGET_VERSION} for the game)")
    ap.add_argument("--any-version", action="store_true", help="do not require the expected version")
    ap.add_argument("--allow-base", action="store_true",
                    help="continue without the update (the base game executable is extracted)")
    ap.add_argument("--romfs", action="store_true", help="also extract the RomFS (large; needs hactoolnet/hactool)")
    ap.add_argument("--tool", choices=["auto", "hactoolnet", "hactool", "none"], default="auto",
                    help="external tool to verify/extract RomFS (auto = whichever is in tools/bin)")
    ap.add_argument("--no-verify", action="store_true", help="do not verify the ExeFS with the external tool")
    ap.add_argument("--raw-elf", action="store_true",
                    help="main.elf without filling the .got (bytes identical to the NSO; see README)")
    ap.add_argument("--elf-base", type=lambda x: int(x, 0), default=0, help="ELF base (default 0)")
    ap.add_argument("--keep-work", action="store_true", help="do not delete dump/.extract-work when done")
    ap.add_argument("--list", action="store_true", help="only show what was found in dump/")
    a = ap.parse_args(argv)

    nx.need_crypto()
    dump, out = a.dump.resolve(), a.out.resolve()
    workdir = dump / WORKDIR_NAME
    expect = a.expect_version if a.expect_version is not None else (
        PLA_TARGET_VERSION if a.title_id == PLA_TITLE_ID else None)
    tid_base = a.title_id & ~0xFFF
    tid_patch = tid_base | 0x800

    try:
        inv = scan_dump(dump, workdir)
        if inv.compressed:
            have_nca = {e.content_id for e in inv.ncas if e.content_id}
            if have_nca:
                info("NSZ/XCZ ignored because uncompressed NSP/XCI/NCA are already present")
            else:
                decompress_nsz(inv.compressed, dump)
                inv = scan_dump(dump, workdir)

        if inv.exefs_dirs and not inv.ncas:
            return from_exefs_dir(inv.exefs_dirs[0], out, a)

        if not inv.ncas:
            raise Fatal(
                f"No game found in {dump}.\n"
                "  Copy the XCI or NSP of the base game and the NSP of update 1.1.1 there\n"
                "  (loose NCAs with their .tik, or an exefs/ folder, also work).")

        keys = load_keys(a.keys, a.title_keys)
        info(f"keys: {keys.prod_path}" + (f" + {keys.title_path}" if keys.title_path else ""))
        for reg, src in inv.tickets:
            try:
                t = nx.parse_ticket(reg.read(0, reg.size))
                keys.add_titlekey(t.rights_id, nx.ticket_titlekey(t, keys))
                info(f"ticket {t.rights_id.hex().upper()} ({'personalized' if t.is_personalized else 'common'}) in {src.name}")
            except (nx.FormatError, nx.KeyError_) as ex:
                warn(f"ticket {reg.name}: {ex}")

        ncas = open_ncas(inv, keys)
        titles = read_cnmts(ncas)
        info(f"{len(ncas)} NCAs, {len(titles)} metadata (CNMT):")
        for t in titles:
            info(f"  {nx.tid_str(t.cnmt.title_id)} {t.cnmt.type_name:<12} v{t.cnmt.version:<8} ({t.meta.source.name})")
        if a.list:
            for e in ncas:
                n = e.nca
                info(f"  {e.region.name:44s} {n.type_name:<10} {nx.tid_str(n.program_id)} "
                     f"{nx.human(e.region.size):>10} {'rights-id' if n.has_rights_id else ''} {'patch' if n.is_patch else ''}")
            return 0

        base = next((t for t in titles if t.cnmt.title_id == tid_base and t.cnmt.meta_type == 0x80), None)
        patches = sorted((t for t in titles if t.cnmt.title_id == tid_patch and t.cnmt.meta_type == 0x81),
                         key=lambda t: t.cnmt.version)
        patch = patches[-1] if patches else None

        # Without CNMT (renamed loose NCAs): heuristic by type and patch
        progs = [e for e in ncas if e.nca.content_type == 0 and (e.nca.program_id & ~0xFFF) == tid_base]
        base_prog = base.contents.get(1) if base else next((e for e in progs if not e.nca.is_patch), None)
        patch_prog = patch.contents.get(1) if patch else next((e for e in progs if e.nca.is_patch), None)
        if base and not base_prog:
            warn("the base game CNMT has no program NCA in dump/")
        if patch and not patch_prog:
            raise Fatal(f"Found the CNMT of update v{patch.cnmt.version} but not its program NCA.")

        if not base_prog and not patch_prog:
            raise Fatal(f"No program NCA for title {nx.tid_str(tid_base)} in dump/. "
                        "Is it the right game?")

        nacp_patch = read_nacp(patch.contents.get(3)) if patch else None
        nacp_base = read_nacp(base.contents.get(3)) if base else None
        disp_patch = nx.nacp_display_version(nacp_patch) if nacp_patch else None
        disp_base = nx.nacp_display_version(nacp_base) if nacp_base else None
        game_name = nx.tid_str(tid_base)  # neutral value: the title ID (the title name is not written)

        if not patch_prog:
            msg = (f"Update ({nx.tid_str(tid_patch)}) not found in dump/.\n"
                   f"  The project target is version {expect or 'updated'}: the base game\n"
                   f"  executable ({disp_base or 'v0'}) is different and is no use for matching.\n"
                   "  Dump the update from the console (nxdumptool: 'Dump installed SD card / eMMC content'\n"
                   "  -> the update) and copy the NSP into dump/. To extract the base anyway: --allow-base")
            if not a.allow_base:
                raise Fatal(msg)
            warn(msg.splitlines()[0] + " Continuing with the base game (--allow-base).")
        chosen = patch_prog or base_prog
        version_num = patch.cnmt.version if patch_prog and patch else (base.cnmt.version if base else 0)
        version_disp = disp_patch if patch_prog else disp_base
        if patch_prog and expect and not a.any_version:
            if version_disp and version_disp != expect:
                raise Fatal(f"The update found is {version_disp} (v{version_num}), but {expect} is expected.\n"
                            "  Use the correct update or pass --any-version if you know what you are doing.")
            if not version_disp and a.title_id == PLA_TITLE_ID and version_num != PLA_TARGET_VERSION_NUMBER:
                raise Fatal(f"The update is v{version_num}; 1.1.1 is v{PLA_TARGET_VERSION_NUMBER}. "
                            "Use --any-version to continue.")
        info(f"program: {chosen.region.location} ({'update ' + (version_disp or '') if patch_prog else 'base'})")

        # ---- ExeFS in Python ----
        exefs_dir = out / "exefs"
        if exefs_dir.exists():
            shutil.rmtree(exefs_dir)
        exefs_dir.mkdir(parents=True)
        try:
            files = nx.nca_pfs0_files(chosen.nca, 0)
        except nx.KeyError_ as ex:
            raise Fatal(str(ex))
        for f in files:
            f.copy_to(exefs_dir / f.name)
        info(f"ExeFS -> {exefs_dir} ({', '.join(f.name for f in files)})")
        if not (exefs_dir / "main").is_file():
            raise Fatal("The ExeFS does not contain 'main'.")

        # ---- external tool: verification and RomFS ----
        tool = find_tool(a.tool)
        rights = [e.nca.rights_id for e in (base_prog, patch_prog) if e and e.nca.has_rights_id]
        verified = None
        if tool and not a.no_verify and chosen.region.size < (1 << 30):
            with ToolEnv(keys, rights) as env:
                tmp = Path(tempfile.mkdtemp(prefix="pla-exefs-"))
                try:
                    run_tool(tool, env, nca_as_file(chosen, workdir), None, tmp, None)
                    bad = [p.name for p in tmp.iterdir()
                           if not (exefs_dir / p.name).is_file() or sha256_file(p) != sha256_file(exefs_dir / p.name)]
                    if bad or len(list(tmp.iterdir())) != len(files):
                        raise Fatal(f"{tool.kind} and the Python extractor disagree on: {bad or 'number of files'}")
                    verified = tool.kind
                    info(f"ExeFS verified with {tool.kind}: identical")
                finally:
                    shutil.rmtree(tmp, ignore_errors=True)
        elif not tool:
            info("no hactoolnet/hactool: cross-verification skipped (optional; see bootstrap.py)")

        if a.romfs:
            if not tool:
                raise Fatal("--romfs needs hactoolnet or hactool. Run: python tools/extract/bootstrap.py")
            romfs_dir = out / "romfs"
            if romfs_dir.exists():
                shutil.rmtree(romfs_dir)
            need_base = chosen is patch_prog and chosen.nca.is_patch
            if need_base and not base_prog:
                raise Fatal("The base game is also needed in dump/ for the update's RomFS.")
            with ToolEnv(keys, rights) as env:
                base_file = nca_as_file(base_prog, workdir) if need_base else None
                info(f"extracting RomFS with {tool.kind} (several GB, be patient)...")
                run_tool(tool, env, nca_as_file(chosen, workdir), base_file, None, romfs_dir)
            info(f"RomFS -> {romfs_dir}")

        # ---- main.nso / main.elf / target.json ----
        meta = {
            "game": game_name,
            "title_id": nx.tid_str(tid_base),
            "update_title_id": nx.tid_str(tid_patch) if patch_prog else None,
            "version": {"display": version_disp, "number": version_num,
                        "source": "update" if patch_prog else "base"},
            "base_version": {"display": disp_base, "number": base.cnmt.version} if base else None,
            "program_nca": {"content_id": chosen.content_id, "key_generation": chosen.nca.key_generation,
                            "sdk_addon_version": f"{chosen.nca.sdk_version >> 24}.{(chosen.nca.sdk_version >> 16) & 0xFF}."
                                                 f"{(chosen.nca.sdk_version >> 8) & 0xFF}",
                            "rights_id": chosen.nca.has_rights_id},
            "verified_with": verified,
        }
        return finish(exefs_dir, out, meta, a)
    except Fatal as e:
        print(f"\n[extract] ERROR: {e}\n", file=sys.stderr)
        return 1
    finally:
        if workdir.exists() and not a.keep_work:
            shutil.rmtree(workdir, ignore_errors=True)


def from_exefs_dir(src: Path, out: Path, a) -> int:
    info(f"using the already extracted ExeFS from {src}")
    exefs_dir = out / "exefs"
    if exefs_dir.resolve() != src.resolve():
        if exefs_dir.exists():
            shutil.rmtree(exefs_dir)
        exefs_dir.mkdir(parents=True)
        for p in src.iterdir():
            if p.is_file() and p.name in EXEFS_NAMES:
                shutil.copyfile(p, exefs_dir / p.name)
    warn("the version cannot be checked with a loose ExeFS; make sure it is 1.1.1")
    meta = {"game": None, "title_id": None, "update_title_id": None,
            "version": {"display": None, "number": None, "source": f"exefs:{src.name}"}}
    return finish(exefs_dir, out, meta, a)


def finish(exefs_dir: Path, out: Path, meta: dict, a) -> int:
    import nso2elf
    main_src = exefs_dir / "main"
    nso_path = out / "main.nso"
    shutil.copyfile(main_src, nso_path)
    elf_path = out / "main.elf"
    resolve = not a.raw_elf
    try:
        nso = nso2elf.parse_nso(nso_path.read_bytes())
        mod = nso2elf.analyze(nso)
        elf_path.write_bytes(nso2elf.build_elf(nso, a.elf_base, resolve, mod))
    except nso2elf.NsoError as e:
        raise Fatal(f"main is not a valid NSO: {e}")
    unc_path = out / "main.uncompressed.nso"  # like nx2elf --export-uncompressed (nx-decomp-tools)
    unc_path.write_bytes(nso2elf.build_uncompressed_nso(nso))
    for n in nso.warnings + mod.notes:
        info(f"nso2elf: {n}")
    info(f"{nso_path}, {unc_path.name} and {elf_path.name}")
    doc = write_target_json(out, meta, exefs_dir, elf_path, {"base": f"0x{a.elf_base:X}", "resolve_got": resolve})
    m = doc["exefs"]["main"]
    info("done:")
    info(f"  version  : {meta['version']['display']} (v{meta['version']['number']})")
    info(f"  main     : sha256 {m['sha256']}")
    info(f"  build id : {m['build_id_short']}")
    for k, s in m["segments"].items():
        info(f"  .{k:<7}: {s['mem_offset']:>10} size 0x{s['size']:X}")
    if doc["sdk_versions"]:
        nn = [v for v in doc["sdk_versions"] if "nnSdk" in v] or doc["sdk_versions"][:1]
        info(f"  SDK      : {', '.join(nn)} (+{len(doc['sdk_versions']) - len(nn)} middleware in target.json)")
    info(f"  -> {out / 'target.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
