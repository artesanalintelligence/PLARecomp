#!/usr/bin/env python3
"""Prepares the extraction tools in tools/bin/ (ignored by git).

    python tools/extract/bootstrap.py              # pip + hactool (+ hactoolnet if obtainable)
    python tools/extract/bootstrap.py --hactoolnet-zip C:\\path\\hactoolnet-0.19.0-win.zip

What it installs:
  1. Python modules from requirements.txt (lz4, pycryptodome, pytest).
  2. hactool 1.4.0 (SciresM) for Windows, with a pinned SHA-256.
  3. hactoolnet (LibHac, Thealexbarney), if a source is available.

Note (2026-10): the Thealexbarney/LibHac repository is no longer on GitHub and its
releases return 404, so hactoolnet cannot be downloaded
automatically. If you have the zip of a release (e.g. hactoolnet-0.19.0-win.zip)
pass it with --hactoolnet-zip or with the HACTOOLNET_URL variable. It is not
essential: extract.py decrypts the ExeFS in Python and uses hactool for
cross-verification and for the RomFS.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BIN = REPO / "tools" / "bin"

HACTOOL = {
    "version": "1.4.0",
    "url": "https://github.com/SciresM/hactool/releases/download/1.4.0/hactool-1.4.0-win.zip",
    "sha256": "36e9a221c8a7949c86ada9388eb703c90663aedfe9f65b6032429614c5e1abe8",
    "member": "hactool.exe",
}

# Known hactoolnet sources (tried in order). No pinned hash because
# none is accessible today to compute it: it is verified by running it.
HACTOOLNET_URLS = [
    "https://github.com/Thealexbarney/LibHac/releases/download/v0.19.0/hactoolnet-0.19.0-win.zip",
    "https://github.com/Thealexbarney/LibHac/releases/download/v0.18.0/hactoolnet-0.18.0-win.zip",
]


def log(msg: str) -> None:
    print(f"[bootstrap] {msg}", flush=True)


def download(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "pla-decomp-bootstrap"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def run_quiet(exe: Path) -> str:
    """Run the tool without keys (empty HOME) and return its output."""
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ, HOME=home, USERPROFILE=home)
        r = subprocess.run([str(exe)], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace", timeout=60)
        return r.stdout


def install_hactool(force: bool) -> bool:
    dest = BIN / "hactool.exe"
    if dest.is_file() and not force:
        log(f"hactool is already in {dest}")
    else:
        log(f"downloading hactool {HACTOOL['version']} ...")
        try:
            blob = download(HACTOOL["url"])
        except Exception as e:
            log(f"ERROR downloading hactool: {e}")
            return False
        digest = hashlib.sha256(blob).hexdigest()
        if digest != HACTOOL["sha256"]:
            log(f"ERROR: unexpected SHA-256 for hactool ({digest})")
            return False
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            dest.write_bytes(z.read(HACTOOL["member"]))
        log(f"hactool -> {dest} (zip sha256 verified)")
    out = run_quiet(dest)
    if "hactool" not in out or "SciresM" not in out:
        log("ERROR: hactool does not run correctly")
        return False
    log("hactool verified (it runs)")
    return True


def install_hactoolnet(src: str | None, force: bool) -> bool:
    dest_dir = BIN / "hactoolnet"
    exe = dest_dir / "hactoolnet.exe"
    if exe.is_file() and not force:
        log(f"hactoolnet is already in {exe}")
        return verify_hactoolnet(exe)
    sources = [src] if src else []
    env_url = os.environ.get("HACTOOLNET_URL")
    if env_url:
        sources.append(env_url)
    sources += HACTOOLNET_URLS
    for s in sources:
        try:
            if Path(s).is_file():
                blob = Path(s).read_bytes()
            else:
                log(f"probando {s} ...")
                blob = download(s)
        except Exception as e:
            log(f"  not available ({e})")
            continue
        try:
            with zipfile.ZipFile(io.BytesIO(blob)) as z:
                if dest_dir.exists():
                    shutil.rmtree(dest_dir)
                z.extractall(dest_dir)
        except zipfile.BadZipFile:
            if blob[:2] == b"MZ":
                dest_dir.mkdir(parents=True, exist_ok=True)
                exe.write_bytes(blob)
            else:
                log("  not a zip or an .exe")
                continue
        found = next(iter(dest_dir.rglob("hactoolnet.exe")), None)
        if not found:
            log("  the zip does not contain hactoolnet.exe")
            continue
        if found != exe:
            shutil.move(str(found), exe)
        log(f"hactoolnet -> {exe} (sha256 {hashlib.sha256(exe.read_bytes()).hexdigest()})")
        return verify_hactoolnet(exe)
    log("hactoolnet not available (LibHac was removed from GitHub). hactool will be used; that is enough.")
    return False


def verify_hactoolnet(exe: Path) -> bool:
    try:
        out = run_quiet(exe)
    except Exception as e:
        log(f"ERROR: hactoolnet does not run: {e}")
        return False
    if "hactoolnet" not in out.lower() and "usage" not in out.lower():
        log("ERROR: hactoolnet does not respond as expected")
        return False
    log("hactoolnet verified (it runs)")
    return True


def pip_install() -> bool:
    req = HERE / "requirements.txt"
    log(f"pip install -r {req.relative_to(REPO)}")
    r = subprocess.run([sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "-q", "-r", str(req)])
    return r.returncode == 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Download and verify hactool/hactoolnet in tools/bin/")
    ap.add_argument("--hactoolnet-zip", help="local zip or .exe (or URL) of hactoolnet")
    ap.add_argument("--no-pip", action="store_true", help="do not install the Python modules")
    ap.add_argument("--force", action="store_true", help="download again even if they already exist")
    a = ap.parse_args(argv)

    BIN.mkdir(parents=True, exist_ok=True)
    ok = True
    if not a.no_pip:
        ok &= pip_install()
    if os.name != "nt":
        log("warning: the pinned binaries are for Windows; on Linux/macOS build hactool or use your distro's")
    ok &= install_hactool(a.force)
    have_net = install_hactoolnet(a.hactoolnet_zip, a.force)
    log("summary: pip=%s hactool=%s hactoolnet=%s" % (
        "ok" if not a.no_pip else "skipped", "ok" if (BIN / "hactool.exe").is_file() else "NO",
        "ok" if have_net else "no (optional)"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
