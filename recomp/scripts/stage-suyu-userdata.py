#!/usr/bin/env python3
"""Prepares suyu's user folder for PLA (keys + update in the NAND).

It does three things, all local and without decrypting anything:

1. Copies prod.keys (default ~/.switch/prod.keys) to <suyu>/keys/prod.keys.
2. Reads the tickets (.tik) of every given NSP and adds the pair
   rights_id = title_key to <suyu>/keys/title.keys (only "common" tickets; a
   personalized ticket would need the console's RSA key and is skipped).
   It also merges ~/.switch's title.keys if it exists. It never prints keys.
3. With --install-update, copies the NCAs of the update NSP to suyu's virtual NAND
   (registered/), like "Install Files to NAND", using install-nsp.py from the
   mk8-recomp repo. The base NSP is NOT installed: the exporter and the boot read
   it directly from its path.

Usage:
  python stage-suyu-userdata.py --mk8r <root>\\mk8-recomp \\
      --nsp <base.nsp> --nsp <update.nsp> --install-update <update.nsp>
"""

import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import sys


def suyu_dir() -> pathlib.Path:
    return pathlib.Path(os.environ["APPDATA"]) / "suyu"


def parse_pfs0(path):
    with open(path, "rb") as f:
        magic, count, strtab_size, _ = struct.unpack("<4sIII", f.read(0x10))
        if magic != b"PFS0":
            raise ValueError(f"{path}: not a PFS0 container ({magic!r})")
        entries = [struct.unpack("<QQII", f.read(0x18)) for _ in range(count)]
        strtab = f.read(strtab_size)
        data_start = 0x10 + count * 0x18 + strtab_size
        out = []
        for offset, size, name_off, _ in entries:
            end = strtab.find(b"\0", name_off)
            out.append((strtab[name_off:end].decode("utf-8"), data_start + offset, size))
        return out


# Signature size (padding included) by the ticket's signature type.
SIG_SIZES = {
    0x010000: 0x200 + 0x3C, 0x010001: 0x100 + 0x3C, 0x010002: 0x3C + 0x40,
    0x010003: 0x200 + 0x3C, 0x010004: 0x100 + 0x3C, 0x010005: 0x3C + 0x40,
}


def ticket_keys(nsp):
    keys = {}
    with open(nsp, "rb") as f:
        for name, off, size in parse_pfs0(nsp):
            if not name.endswith(".tik"):
                continue
            f.seek(off)
            tik = f.read(size)
            sig_type = struct.unpack_from("<I", tik, 0)[0]
            base = 4 + SIG_SIZES.get(sig_type, 0x13C)
            key_block = tik[base + 0x40: base + 0x40 + 0x100]
            key_type = tik[base + 0x141]
            rights_id = tik[base + 0x160: base + 0x170].hex()
            if key_type != 0:
                print(f"  {name}: personalized ticket (type {key_type}), skipped")
                continue
            keys[rights_id] = key_block[:16].hex()
            print(f"  {name}: rights_id {rights_id} -> title.keys (key not shown)")
    return keys


def read_keyfile(path):
    out = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip().lower()] = v.strip()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prod-keys", default=str(pathlib.Path.home() / ".switch" / "prod.keys"))
    ap.add_argument("--title-keys", default=str(pathlib.Path.home() / ".switch" / "title.keys"))
    ap.add_argument("--nsp", action="append", default=[])
    ap.add_argument("--install-update")
    ap.add_argument("--mk8r", default=r"D:\PLARecomp\mk8-recomp")
    a = ap.parse_args()

    keys_dir = suyu_dir() / "keys"
    keys_dir.mkdir(parents=True, exist_ok=True)

    src = pathlib.Path(a.prod_keys)
    if not src.exists():
        print(f"{src} does not exist")
        return 1
    shutil.copyfile(src, keys_dir / "prod.keys")
    print(f"prod.keys -> {keys_dir / 'prod.keys'}")

    merged = read_keyfile(keys_dir / "title.keys")
    merged.update(read_keyfile(pathlib.Path(a.title_keys)))
    for nsp in a.nsp:
        print(f"tickets of {pathlib.Path(nsp).name}:")
        merged.update(ticket_keys(nsp))
    (keys_dir / "title.keys").write_text(
        "".join(f"{k} = {v}\n" for k, v in sorted(merged.items())), encoding="ascii")
    print(f"title.keys: {len(merged)} entries -> {keys_dir / 'title.keys'}")

    if a.install_update:
        script = pathlib.Path(a.mk8r) / "scripts" / "install-nsp.py"
        r = subprocess.run([sys.executable, str(script), a.install_update])
        if r.returncode:
            return r.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
