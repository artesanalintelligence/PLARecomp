"""Builds synthetic NCAs/NSPs/tickets with (random) test keys.

Used to test nxfmt.py and extract.py end to end without the game or the
real keys. Follows the same specification (switchbrew) the reader uses.
"""

from __future__ import annotations

import hashlib
import os
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from Crypto.Cipher import AES


def _gf_mul_alpha(t: int) -> int:
    t <<= 1
    if t >> 128:
        t = (t & ((1 << 128) - 1)) ^ 0x87
    return t


def xts_encrypt(key: bytes, data: bytes, tweak: bytes) -> bytes:
    k1 = AES.new(key[:16], AES.MODE_ECB)
    t = int.from_bytes(AES.new(key[16:], AES.MODE_ECB).encrypt(tweak), "little")
    out = bytearray()
    for i in range(0, len(data), 16):
        tb = t.to_bytes(16, "little")
        x = bytes(a ^ b for a, b in zip(data[i:i + 16], tb))
        out += bytes(a ^ b for a, b in zip(k1.encrypt(x), tb))
        t = _gf_mul_alpha(t)
    return bytes(out)


def nintendo_xts_encrypt(key: bytes, data: bytes) -> bytes:
    return b"".join(xts_encrypt(key, data[i:i + 0x200], (i // 0x200).to_bytes(16, "big"))
                    for i in range(0, len(data), 0x200))


@dataclass
class TestKeys:
    header_key: bytes = field(default_factory=lambda: os.urandom(32))
    kaek: bytes = field(default_factory=lambda: os.urandom(16))      # key_area_key_application_00
    titlekek: bytes = field(default_factory=lambda: os.urandom(16))  # titlekek_00

    def write(self, path: Path) -> None:
        path.write_text(
            f"header_key = {self.header_key.hex()}\n"
            f"key_area_key_application_00 = {self.kaek.hex()}\n"
            f"titlekek_00 = {self.titlekek.hex()}\n")


def pfs0(files: List[Tuple[str, bytes]]) -> bytes:
    strtab = bytearray()
    entries = bytearray()
    data = bytearray()
    for name, blob in files:
        entries += struct.pack("<QQII", len(data), len(blob), len(strtab), 0)
        strtab += name.encode() + b"\0"
        data += blob
    while (0x10 + len(entries) + len(strtab)) % 0x20:
        strtab += b"\0"
    return b"PFS0" + struct.pack("<III", len(files), len(strtab), 0) + bytes(entries) + bytes(strtab) + bytes(data)


def hfs0(files: List[Tuple[str, bytes]]) -> bytes:
    strtab = bytearray()
    entries = bytearray()
    data = bytearray()
    for name, blob in files:
        entries += struct.pack("<QQIIQ", len(data), len(blob), len(strtab), 0x200, 0) + hashlib.sha256(blob[:0x200]).digest()
        strtab += name.encode() + b"\0"
        data += blob
    while (0x10 + len(entries) + len(strtab)) % 0x20:
        strtab += b"\0"
    return b"HFS0" + struct.pack("<III", len(files), len(strtab), 0) + bytes(entries) + bytes(strtab) + bytes(data)


def romfs_single(name: str, blob: bytes) -> bytes:
    """Minimal RomFS with one file in the root."""
    nm = name.encode()
    nm_pad = nm + b"\0" * ((-len(nm)) % 4)
    file_meta = struct.pack("<IIQQII", 0, 0xFFFFFFFF, 0, len(blob), 0xFFFFFFFF, len(nm)) + nm_pad
    dir_meta = struct.pack("<IIIIII", 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0)
    dir_hash = struct.pack("<I", 0)
    file_hash = struct.pack("<I", 0)
    off = 0x200
    data_off = off
    meta_off = align(data_off + len(blob), 4)
    dh_off = meta_off
    dm_off = dh_off + len(dir_hash)
    fh_off = dm_off + len(dir_meta)
    fm_off = fh_off + len(file_hash)
    hdr = struct.pack("<10Q", 0x50, dh_off, len(dir_hash), dm_off, len(dir_meta), fh_off, len(file_hash),
                      fm_off, len(file_meta), data_off)
    out = bytearray(hdr.ljust(off, b"\0"))
    out += blob
    out = out.ljust(meta_off, b"\0")
    out += dir_hash + dir_meta + file_hash + file_meta
    return bytes(out)


def align(x: int, a: int) -> int:
    return (x + a - 1) // a * a


def make_nca(keys: TestKeys, content_type: int, program_id: int,
             sections: List[Tuple[str, bytes]], rights_id: Optional[bytes] = None,
             content_key: Optional[bytes] = None) -> bytes:
    """sections: list of ("pfs0"|"romfs", content). All AES-CTR."""
    content_key = content_key or os.urandom(16)
    body = bytearray()
    hdr = bytearray(0xC00)
    hdr[0x200:0x204] = b"NCA3"
    hdr[0x204] = 0
    hdr[0x205] = content_type
    hdr[0x206] = 0
    hdr[0x207] = 0
    struct.pack_into("<Q", hdr, 0x210, program_id)
    struct.pack_into("<I", hdr, 0x21C, 0x0B040600)
    hdr[0x220] = 0
    if rights_id:
        hdr[0x230:0x240] = rights_id
    else:
        enc_area = AES.new(keys.kaek, AES.MODE_ECB).encrypt(bytes(0x20) + content_key + bytes(0x10))
        hdr[0x300:0x340] = enc_area
    pos = 0xC00
    for i, (kind, payload) in enumerate(sections):
        fsh = bytearray(0x200)
        struct.pack_into("<HBBB", fsh, 0, 2, 1 if kind == "pfs0" else 0, 2 if kind == "pfs0" else 3, 3)
        hashdata = bytearray(0xF8)
        lead = 0x200  # fake "hash table" before the data
        hash_table = b""
        if kind == "pfs0":
            # valid HierarchicalSha256: table with the SHA-256 of each block and master hash
            bs = 0x1000
            hash_table = b"".join(hashlib.sha256(payload[i:i + bs]).digest() for i in range(0, len(payload), bs))
            lead = align(len(hash_table), 0x200)
            hashdata[0:0x20] = hashlib.sha256(hash_table).digest()
            struct.pack_into("<II", hashdata, 0x20, bs, 2)
            struct.pack_into("<QQQQ", hashdata, 0x28, 0, len(hash_table), lead, len(payload))
        else:
            hashdata[0:4] = b"IVFC"
            struct.pack_into("<III", hashdata, 4, 0x20000, 0x20, 7)
            struct.pack_into("<QQI", hashdata, 0x10 + 0x18 * 5, lead, len(payload), 14)
        fsh[8:0x100] = hashdata
        upper = struct.unpack("<Q", os.urandom(8))[0]
        struct.pack_into("<Q", fsh, 0x140, upper)
        plain = (hash_table.ljust(lead, b"\0") + payload).ljust(align(lead + len(payload), 0x200), b"\0")
        iv = struct.pack(">QQ", upper, pos >> 4)
        enc = AES.new(content_key, AES.MODE_CTR, nonce=b"", initial_value=iv).encrypt(plain)
        struct.pack_into("<II", hdr, 0x240 + 0x10 * i, pos // 0x200, (pos + len(plain)) // 0x200)
        hdr[0x400 + 0x200 * i:0x600 + 0x200 * i] = fsh
        hdr[0x280 + 0x20 * i:0x2A0 + 0x20 * i] = hashlib.sha256(fsh).digest()
        body += enc
        pos += len(plain)
    struct.pack_into("<Q", hdr, 0x208, pos)
    return nintendo_xts_encrypt(keys.header_key, bytes(hdr)) + bytes(body)


def make_ticket(keys: TestKeys, rights_id: bytes, content_key: bytes) -> bytes:
    enc = AES.new(keys.titlekek, AES.MODE_ECB).encrypt(content_key)
    t = bytearray(0x2C0)
    struct.pack_into("<I", t, 0, 0x10004)
    d = 0x140
    t[d:d + 0x40] = b"Root-CA00000003-XS00000020".ljust(0x40, b"\0")
    t[d + 0x40:d + 0x50] = enc
    t[d + 0x140] = 2
    t[d + 0x141] = 0
    t[d + 0x160:d + 0x170] = rights_id
    return bytes(t)


def make_cnmt(title_id: int, version: int, meta_type: int, records: List[Tuple[str, int, int]]) -> bytes:
    """records: (content_id_hex, size, type)."""
    ext = struct.pack("<QII", title_id & ~0xFFF if meta_type == 0x81 else title_id | 0x800, 0, 0)[:0x10]
    hdr = struct.pack("<QIBBHHHBBH", title_id, version, meta_type, 0, len(ext), len(records), 0, 0, 0, 0)
    hdr = hdr.ljust(0x20, b"\0")
    recs = b"".join(bytes(0x20) + bytes.fromhex(cid) + size.to_bytes(6, "little") + bytes([ctype, 0])
                    for cid, size, ctype in records)
    return hdr + ext + recs


def make_nacp(name: str, version: str) -> bytes:
    n = bytearray(0x4000)
    n[0:len(name)] = name.encode()
    n[0x3060:0x3060 + len(version)] = version.encode()
    return bytes(n)


def make_npdm(name: str, program_id: int) -> bytes:
    """Minimal but well-formed NPDM (META + ACI0 + ACID with empty FAC/FAH)."""
    aci_off, aci_size = 0x80, 0x80
    acid_off, acid_size = 0x100, 0x300
    b = bytearray(acid_off + acid_size)
    b[0:4] = b"META"
    b[0x20:0x20 + len(name)] = name.encode()
    struct.pack_into("<IIII", b, 0x70, aci_off, aci_size, acid_off, acid_size)
    a = aci_off
    b[a:a + 4] = b"ACI0"
    struct.pack_into("<Q", b, a + 0x10, program_id)
    struct.pack_into("<IIIIII", b, a + 0x20, 0x40, 0x1C, 0x60, 0, 0x60, 0)  # FAH, SAC, KAC
    b[a + 0x40] = 1  # FAH version
    struct.pack_into("<IIII", b, a + 0x4C, 0x1C, 0, 0x1C, 0)
    c = acid_off
    b[c + 0x200:c + 0x204] = b"ACID"
    struct.pack_into("<I", b, c + 0x204, acid_size - 0x100)
    struct.pack_into("<QQ", b, c + 0x210, program_id, program_id)
    struct.pack_into("<IIIIII", b, c + 0x220, 0x240, 0x2C, 0x270, 0, 0x270, 0)  # FAC, SAC, KAC
    b[c + 0x240] = 1  # FAC version
    return bytes(b)


def cid_of(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()[:32]


def make_title_nsp(keys: TestKeys, title_id: int, version: int, display: str, exefs: List[Tuple[str, bytes]],
                   patch: bool, with_ticket: bool = True) -> bytes:
    """NSP with program + control + meta. If patch=True it uses rights ID + ticket (like a real update)."""
    app_id = title_id & ~0xFFF
    meta_id = app_id | 0x800 if patch else app_id
    files = []
    ck = os.urandom(16)
    rights = struct.pack(">Q", meta_id) + bytes(7) + b"\x00" if patch else None
    prog = make_nca(keys, 0, app_id, [("pfs0", pfs0(exefs))], rights_id=rights, content_key=ck)
    ctrl = make_nca(keys, 2, app_id, [("romfs", romfs_single("control.nacp", make_nacp("Test game", display)))])
    recs = [(cid_of(prog), len(prog), 1), (cid_of(ctrl), len(ctrl), 3)]
    kind = 0x81 if patch else 0x80
    cnmt_name = f"{'Patch' if patch else 'Application'}_{meta_id:016x}.cnmt"
    meta = make_nca(keys, 1, meta_id, [("pfs0", pfs0([(cnmt_name, make_cnmt(meta_id, version, kind, recs))]))])
    files += [(cid_of(prog) + ".nca", prog), (cid_of(ctrl) + ".nca", ctrl), (cid_of(meta) + ".cnmt.nca", meta)]
    if patch and with_ticket:
        files.append((rights.hex() + ".tik", make_ticket(keys, rights, ck)))
    return pfs0(files)


def make_xci(nsp_like_files: List[Tuple[str, bytes]]) -> bytes:
    """Minimal XCI: 'HEAD' header at 0x100 and root HFS0 with secure partition."""
    secure = hfs0(nsp_like_files)
    root = hfs0([("update", hfs0([])), ("normal", hfs0([])), ("secure", secure)])
    hdr = bytearray(0xF000)
    hdr[0x100:0x104] = b"HEAD"
    struct.pack_into("<QQ", hdr, 0x130, 0xF000, 0x200)
    return bytes(hdr) + root
