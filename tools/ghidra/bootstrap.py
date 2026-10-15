#!/usr/bin/env python3
"""Downloads a portable JDK 21 (Eclipse Temurin) and Ghidra into tools/bin/.

It does not touch the system Java: everything stays inside tools/bin/ and the paths are
recorded in tools/bin/toolpaths.json, which analyze.py and the other scripts read.

Usage:
    python tools/ghidra/bootstrap.py                    # latest stable Ghidra
    python tools/ghidra/bootstrap.py --ghidra-version 12.1.2 --switch-loader yes
    python tools/ghidra/bootstrap.py --check            # only show the status

Switch loader (Adubbz/Ghidra-Switch-Loader): Ghidra extensions only
load if they were built for the EXACT Ghidra version. With
--switch-loader auto (default) it is installed only if a loader release
exists for the chosen version; otherwise the native ELF loader is used with
data/main.elf (see README.md, section "Decision on the Switch loader").
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import sys
import urllib.request
import zipfile
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "tools" / "bin"
CACHE = BIN / "cache"
TOOLPATHS = BIN / "toolpaths.json"

GH_API = "https://api.github.com/repos"
UA = {"User-Agent": "PLARecomp-bootstrap"}


def http_json(url: str):
    req = urllib.request.Request(url, headers=UA)
    if "api.github.com" in url:
        req.add_header("Accept", "application/vnd.github+json")
        token = os.environ.get("GITHUB_TOKEN")
        if token:
            req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def download(url: str, dest: Path, sha256: str | None = None) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and sha256 and file_sha256(dest) == sha256.lower():
        print(f"[cache] {dest.name}")
        return dest
    tmp = dest.with_suffix(dest.suffix + ".part")
    print(f"[download] {url}")
    req = urllib.request.Request(url, headers={"User-Agent": UA["User-Agent"]})
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        last = -1
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            if total:
                pct = done * 100 // total
                if pct // 10 != last:
                    last = pct // 10
                    print(f"  {pct:3d}% ({done >> 20} / {total >> 20} MiB)", flush=True)
    if sha256:
        got = file_sha256(tmp)
        if got != sha256.lower():
            tmp.unlink()
            raise SystemExit(f"Wrong SHA-256 for {dest.name}: {got} != {sha256}")
        print(f"  SHA-256 OK ({sha256[:16]}...)")
    tmp.replace(dest)
    return dest


def file_sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def extract(archive: Path, dest_parent: Path) -> Path:
    """Extract and return the single root directory of the archive."""
    dest_parent.mkdir(parents=True, exist_ok=True)
    print(f"[extract] {archive.name} -> {dest_parent}")
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            roots = {n.split("/", 1)[0] for n in z.namelist()}
            z.extractall(dest_parent)
            # zipfile does not preserve the executable bit (matters on Linux/macOS)
            for info in z.infolist():
                mode = (info.external_attr >> 16) & 0o777
                if mode & 0o111:
                    p = dest_parent / info.filename
                    if p.is_file():
                        p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    else:
        with tarfile.open(archive) as t:
            roots = {n.split("/", 1)[0] for n in t.getnames()}
            t.extractall(dest_parent)
    if len(roots) != 1:
        raise SystemExit(f"{archive.name}: expected a single root directory, found {roots}")
    return dest_parent / roots.pop()


# --------------------------------------------------------------------------- JDK
def jdk_platform():
    sysname = platform.system().lower()
    os_name = {"windows": "windows", "linux": "linux", "darwin": "mac"}[sysname]
    mach = platform.machine().lower()
    arch = "aarch64" if mach in ("arm64", "aarch64") else "x64"
    return os_name, arch


def install_jdk(major: int) -> Path:
    os_name, arch = jdk_platform()
    api = (f"https://api.adoptium.net/v3/assets/latest/{major}/hotspot"
           f"?architecture={arch}&image_type=jdk&os={os_name}&vendor=eclipse")
    assets = http_json(api)
    if not assets:
        raise SystemExit(f"Adoptium returned no JDK {major} for {os_name}/{arch}")
    pkg = assets[0]["binary"]["package"]
    release = assets[0]["release_name"]
    target = BIN / release
    if (target / "bin" / java_exe_name()).exists():
        print(f"[ok] JDK {release} already installed")
        return target
    archive = download(pkg["link"], CACHE / pkg["name"], pkg.get("checksum"))
    root = extract(archive, BIN)
    if root != target:
        if target.exists():
            shutil.rmtree(target)
        root.rename(target)
    if os_name == "mac" and (target / "Contents" / "Home").exists():
        return target / "Contents" / "Home"
    return target


def java_exe_name() -> str:
    return "java.exe" if os.name == "nt" else "java"


# ------------------------------------------------------------------------ Ghidra
def ghidra_release(version: str | None):
    if version:
        rel = http_json(f"{GH_API}/NationalSecurityAgency/ghidra/releases/tags/Ghidra_{version}_build")
    else:
        rel = http_json(f"{GH_API}/NationalSecurityAgency/ghidra/releases/latest")
    tag = rel["tag_name"]
    ver = re.match(r"Ghidra_(.+)_build", tag).group(1)
    asset = next(a for a in rel["assets"] if a["name"].endswith(".zip"))
    m = re.search(r"SHA-256:\s*`?([0-9a-fA-F]{64})", rel.get("body") or "")
    return ver, asset, (m.group(1) if m else None)


def install_ghidra(version: str | None) -> tuple[Path, str]:
    ver, asset, sha = ghidra_release(version)
    # ghidra_12.1.4_PUBLIC_20260921.zip -> ghidra_12.1.4_PUBLIC
    dirname = re.sub(r"_\d{8}\.zip$", "", asset["name"])
    target = BIN / dirname
    if (target / "support").is_dir():
        print(f"[ok] Ghidra {ver} already installed in {target}")
        return target, ver
    archive = download(asset["browser_download_url"], CACHE / asset["name"], sha)
    root = extract(archive, BIN)
    if root != target:
        root.rename(target)
    return target, ver


def install_switch_loader(ghidra_dir: Path, ghidra_ver: str, mode: str) -> dict:
    """Install Adubbz/Ghidra-Switch-Loader if there is a build for ghidra_ver."""
    if mode == "no":
        return {"installed": False, "reason": "disabled with --switch-loader no"}
    rels = http_json(f"{GH_API}/Adubbz/Ghidra-Switch-Loader/releases?per_page=30")
    wanted = f"Ghidra_{ghidra_ver}.zip"
    found = None
    available = set()
    for r in rels:
        for a in r["assets"]:
            m = re.search(r"Ghidra_([\d.]+)\.zip$", a["name"])
            if m:
                available.add(m.group(1))
            if a["name"].endswith(wanted) and found is None:
                found = (r, a)
    if not found:
        reason = (f"there is no Switch loader build for Ghidra {ghidra_ver} "
                  f"(available builds: {', '.join(sorted(available, key=_vkey, reverse=True)[:5])}); "
                  "the native ELF loader is used with data/main.elf")
        if mode == "yes":
            raise SystemExit("ERROR: " + reason +
                             ". Use --ghidra-version with one of those versions.")
        print("[switch-loader] " + reason)
        return {"installed": False, "reason": reason}
    rel, asset = found
    archive = download(asset["browser_download_url"], CACHE / asset["name"])
    ext_dir = ghidra_dir / "Ghidra" / "Extensions"
    root = extract(archive, ext_dir)
    print(f"[switch-loader] installed {asset['name']} in {root}")
    return {"installed": True, "release": rel["tag_name"], "asset": asset["name"], "path": str(root)}


def _vkey(v: str):
    return tuple(int(x) for x in v.split("."))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghidra-version", help="e.g. 12.1.4 (default: latest stable)")
    ap.add_argument("--jdk", type=int, default=21, help="JDK major version (default 21)")
    ap.add_argument("--switch-loader", choices=["auto", "yes", "no"], default="auto")
    ap.add_argument("--keep-archives", action="store_true",
                    help="keep the .zip files in tools/bin/cache (by default they are deleted to save disk)")
    ap.add_argument("--check", action="store_true", help="only show toolpaths.json")
    args = ap.parse_args(argv)

    if args.check:
        if TOOLPATHS.exists():
            print(TOOLPATHS.read_text(encoding="utf-8"))
            return 0
        print("No installation; run bootstrap.py without --check")
        return 1

    BIN.mkdir(parents=True, exist_ok=True)
    java_home = install_jdk(args.jdk)
    ghidra_dir, ghidra_ver = install_ghidra(args.ghidra_version)
    sl = install_switch_loader(ghidra_dir, ghidra_ver, args.switch_loader)

    info = {
        "java_home": str(java_home),
        "ghidra_dir": str(ghidra_dir),
        "ghidra_version": ghidra_ver,
        "switch_loader": sl,
    }
    TOOLPATHS.write_text(json.dumps(info, indent=2), encoding="utf-8")
    print(f"[ok] paths saved in {TOOLPATHS}")
    if not args.keep_archives and CACHE.exists():
        shutil.rmtree(CACHE, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
