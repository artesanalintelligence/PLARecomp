"""Reading of Nintendo Switch formats in pure Python (+ pycryptodome).

Covers what is needed to get the ExeFS out of a game without external tools:

* keys: prod.keys / title.keys (Lockpick_RCM / hactool format)
* containers: XCI (HFS0), NSP (PFS0), folders with loose NCAs
* tickets (.tik), common and personalized
* NCA3: header (AES-XTS with Nintendo's tweak), key area, titlekeys,
  AES-CTR sections with PFS0 (ExeFS, CNMT) and unpatched RomFS (control.nacp)
* CNMT and NACP

What it does NOT cover: patched RomFS (BKTR / AesCtrEx). For that hactool
or hactoolnet is used (see bootstrap.py).

No function in this module prints keys.
"""

from __future__ import annotations

import io
import os
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO, Dict, Iterator, List, Optional, Tuple

try:
    from Crypto.Cipher import AES
except ImportError:  # pragma: no cover - reported in the CLI
    AES = None


class FormatError(Exception):
    pass


class KeyError_(Exception):
    """A key is missing or incorrect."""


def need_crypto() -> None:
    if AES is None:
        raise SystemExit(
            "Missing pycryptodome module. Install it with:\n"
            "    python -m pip install -r tools/extract/requirements.txt")


# ---------------------------------------------------------------------------
# Claves
# ---------------------------------------------------------------------------

_KEYLINE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*[=,]\s*([0-9A-Fa-f]+)\s*$")


def parse_keyfile(path: Path) -> Dict[str, bytes]:
    keys: Dict[str, bytes] = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = _KEYLINE.match(line)
            if not m:
                continue
            name, value = m.group(1).lower(), m.group(2)
            if len(value) % 2:
                continue
            keys[name] = bytes.fromhex(value)
    return keys


@dataclass
class KeySet:
    prod: Dict[str, bytes] = field(default_factory=dict)
    titlekeys: Dict[bytes, bytes] = field(default_factory=dict)  # rights_id -> encrypted titlekey
    prod_path: Optional[Path] = None
    title_path: Optional[Path] = None

    @classmethod
    def load(cls, prod_path: Optional[Path], title_path: Optional[Path]) -> "KeySet":
        ks = cls()
        if prod_path and prod_path.is_file():
            ks.prod = parse_keyfile(prod_path)
            ks.prod_path = prod_path
        if title_path and title_path.is_file():
            ks.title_path = title_path
            for name, value in parse_keyfile(title_path).items():
                if len(name) == 32 and len(value) == 16:
                    # repeated title.keys: we keep the first one, it makes no difference
                    ks.titlekeys.setdefault(bytes.fromhex(name), value)
        return ks

    def get(self, name: str) -> bytes:
        v = self.prod.get(name)
        if v is None:
            raise KeyError_(
                f"Missing key '{name}' in {self.prod_path or 'prod.keys'}. "
                "Dump the keys from YOUR console again with Lockpick_RCM "
                "(a version that supports your console's firmware).")
        return v

    def has(self, name: str) -> bool:
        return name in self.prod

    def add_titlekey(self, rights_id: bytes, enc_titlekey: bytes) -> None:
        self.titlekeys[rights_id] = enc_titlekey


# ---------------------------------------------------------------------------
# Cryptography
# ---------------------------------------------------------------------------

def _gf_mul_alpha(t: int) -> int:
    t <<= 1
    if t >> 128:
        t = (t & ((1 << 128) - 1)) ^ 0x87
    return t


def xts_decrypt(key: bytes, data: bytes, tweak: bytes) -> bytes:
    """AES-128-XTS of one data unit. `key` = K1||K2 (32 bytes)."""
    need_crypto()
    if len(key) != 32 or len(data) % 16:
        raise ValueError("XTS: 32-byte key and data in multiples of 16")
    k1 = AES.new(key[:16], AES.MODE_ECB)
    t = int.from_bytes(AES.new(key[16:], AES.MODE_ECB).encrypt(tweak), "little")
    out = bytearray()
    blocks = [data[i:i + 16] for i in range(0, len(data), 16)]
    xored = bytearray()
    tweaks = []
    for b in blocks:
        tb = t.to_bytes(16, "little")
        tweaks.append(tb)
        xored += bytes(x ^ y for x, y in zip(b, tb))
        t = _gf_mul_alpha(t)
    dec = k1.decrypt(bytes(xored))
    for i, tb in enumerate(tweaks):
        out += bytes(x ^ y for x, y in zip(dec[i * 16:i * 16 + 16], tb))
    return bytes(out)


def nintendo_xts_decrypt(key: bytes, data: bytes, first_sector: int, sector_size: int = 0x200) -> bytes:
    """XTS with Nintendo's tweak: sector number in big-endian."""
    out = bytearray()
    for i in range(0, len(data), sector_size):
        sector = first_sector + i // sector_size
        out += xts_decrypt(key, data[i:i + sector_size], sector.to_bytes(16, "big"))
    return bytes(out)


def aes_ecb_decrypt(key: bytes, data: bytes) -> bytes:
    need_crypto()
    return AES.new(key, AES.MODE_ECB).decrypt(data)


# ---------------------------------------------------------------------------
# Access to file regions (without copying)
# ---------------------------------------------------------------------------

@dataclass
class FileRegion:
    """A slice [offset, offset+size) of a file on disk."""
    path: Path
    offset: int
    size: int
    name: str

    def open(self) -> BinaryIO:
        return open(self.path, "rb")

    def read(self, off: int, size: int, f: Optional[BinaryIO] = None) -> bytes:
        if off < 0 or off + size > self.size:
            raise FormatError(f"{self.name}: read out of range ({off:#x}+{size:#x} > {self.size:#x})")
        own = f is None
        if own:
            f = self.open()
        try:
            f.seek(self.offset + off)
            data = f.read(size)
        finally:
            if own:
                f.close()
        if len(data) != size:
            raise FormatError(f"{self.name}: truncated file")
        return data

    def sub(self, off: int, size: int, name: str) -> "FileRegion":
        if off + size > self.size:
            raise FormatError(f"{name}: outside container {self.name}")
        return FileRegion(self.path, self.offset + off, size, name)

    def copy_to(self, dest: Path, chunk: int = 16 << 20) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        with self.open() as f, open(dest, "wb") as o:
            f.seek(self.offset)
            left = self.size
            while left:
                b = f.read(min(chunk, left))
                if not b:
                    raise FormatError(f"{self.name}: truncated file while copying")
                o.write(b)
                left -= len(b)

    @property
    def location(self) -> str:
        if self.offset == 0 and self.path.stat().st_size == self.size:
            return str(self.path)
        return f"{self.path.name}::{self.name}"


class BytesRegion(FileRegion):
    """Memory-backed FileRegion (for tests and already decrypted data)."""

    def __init__(self, data: bytes, name: str = "<mem>"):
        self._data = data
        super().__init__(Path(name), 0, len(data), name)

    def open(self) -> BinaryIO:
        return io.BytesIO(self._data)

    def sub(self, off: int, size: int, name: str) -> "BytesRegion":
        return BytesRegion(self._data[off:off + size], name)


# ---------------------------------------------------------------------------
# PFS0 / HFS0 / XCI
# ---------------------------------------------------------------------------

def parse_pfs0(region: FileRegion) -> List[FileRegion]:
    head = region.read(0, 0x10)
    if head[:4] != b"PFS0":
        raise FormatError(f"{region.name}: not PFS0")
    n, strsize = struct.unpack_from("<II", head, 4)
    table = region.read(0x10, 0x18 * n + strsize)
    strtab = table[0x18 * n:]
    data_start = 0x10 + 0x18 * n + strsize
    out = []
    for i in range(n):
        off, size, name_off = struct.unpack_from("<QQI", table, 0x18 * i)
        nul_end = strtab.find(b"\0", name_off)  # some NSZ do not terminate the last name with \0
        name = strtab[name_off:nul_end if nul_end >= 0 else len(strtab)].decode("utf-8", "replace")
        out.append(region.sub(data_start + off, size, name))
    return out


def parse_hfs0(region: FileRegion) -> List[FileRegion]:
    head = region.read(0, 0x10)
    if head[:4] != b"HFS0":
        raise FormatError(f"{region.name}: not HFS0")
    n, strsize = struct.unpack_from("<II", head, 4)
    table = region.read(0x10, 0x40 * n + strsize)
    strtab = table[0x40 * n:]
    data_start = 0x10 + 0x40 * n + strsize
    out = []
    for i in range(n):
        off, size, name_off = struct.unpack_from("<QQI", table, 0x40 * i)
        nul_end = strtab.find(b"\0", name_off)  # some NSZ do not terminate the last name with \0
        name = strtab[name_off:nul_end if nul_end >= 0 else len(strtab)].decode("utf-8", "replace")
        out.append(region.sub(data_start + off, size, name))
    return out


def parse_xci(path: Path) -> Dict[str, List[FileRegion]]:
    """Return {partition: [files]} (update, normal, secure, logo)."""
    size = path.stat().st_size
    whole = FileRegion(path, 0, size, path.name)
    base = None
    for cand in (0, 0x1000):  # some dumps include the 0x1000 "key area" in front
        if whole.read(cand + 0x100, 4) == b"HEAD":
            base = cand
            break
    if base is None:
        raise FormatError(f"{path.name}: does not look like an XCI (no 'HEAD' at 0x100)")
    hfs_off, hfs_size = struct.unpack("<QQ", whole.read(base + 0x130, 0x10))
    root = whole.sub(base + hfs_off, size - base - hfs_off, "root.hfs0")
    parts = {}
    for p in parse_hfs0(root):
        try:
            parts[p.name] = parse_hfs0(p)
        except FormatError:
            parts[p.name] = []
    return parts


# ---------------------------------------------------------------------------
# Tickets
# ---------------------------------------------------------------------------

_SIG_SIZES = {0x10000: 0x200 + 0x3C, 0x10001: 0x100 + 0x3C, 0x10002: 0x3C + 0x40,
              0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}


@dataclass
class Ticket:
    rights_id: bytes
    titlekey_type: int  # 0 common, 1 personalized
    key_block: bytes    # 0x100
    master_key_rev: int

    @property
    def is_personalized(self) -> bool:
        return self.titlekey_type == 1


def parse_ticket(data: bytes) -> Ticket:
    sig_type = struct.unpack_from("<I", data, 0)[0]
    if sig_type not in _SIG_SIZES:
        raise FormatError(f"ticket with unknown signature type {sig_type:#x}")
    d = 4 + _SIG_SIZES[sig_type]
    return Ticket(rights_id=data[d + 0x160:d + 0x170],
                  titlekey_type=data[d + 0x141],
                  key_block=data[d + 0x40:d + 0x140],
                  master_key_rev=data[d + 0x145])


def ticket_titlekey(tik: Ticket, keys: KeySet) -> bytes:
    """Titlekey (still encrypted with titlekek) from a ticket."""
    if not tik.is_personalized:
        return tik.key_block[:16]
    # Personalized ticket: RSA-OAEP with the console's eTicket key.
    kp = keys.prod.get("eticket_rsa_keypair")
    if not kp or len(kp) < 0x204:
        raise KeyError_(
            "The ticket is personalized (tied to the console) and "
            "'eticket_rsa_keypair' is missing in prod.keys. Dump the NSP again with "
            "nxdumptool enabling 'remove console specific data', or dump "
            "the keys with Lockpick_RCM.")
    from Crypto.PublicKey import RSA
    from Crypto.Cipher import PKCS1_OAEP
    from Crypto.Hash import SHA256
    d = int.from_bytes(kp[0:0x100], "big")
    n = int.from_bytes(kp[0x100:0x200], "big")
    e = int.from_bytes(kp[0x200:0x204], "big")
    rsa = RSA.construct((n, e, d))
    dec = PKCS1_OAEP.new(rsa, hashAlgo=SHA256).decrypt(tik.key_block)
    return dec[:16]


# ---------------------------------------------------------------------------
# NCA
# ---------------------------------------------------------------------------

CONTENT_TYPES = {0: "Program", 1: "Meta", 2: "Control", 3: "Manual", 4: "Data", 5: "PublicData"}
ENC_NONE, ENC_XTS, ENC_CTR, ENC_CTREX, ENC_CTR_SKIP, ENC_CTREX_SKIP = 1, 2, 3, 4, 5, 6
HASH_SHA256, HASH_IVFC = 2, 3


@dataclass
class FsSection:
    index: int
    start: int          # bytes, absolute within the NCA
    end: int
    fs_type: int        # 0 RomFS, 1 PFS0
    hash_type: int
    enc_type: int
    upper_counter: int
    raw_header: bytes

    @property
    def is_patch(self) -> bool:
        return self.enc_type in (ENC_CTREX, ENC_CTREX_SKIP) or any(self.raw_header[0x100:0x140])

    def data_region(self) -> Tuple[int, int]:
        """(offset, size) of the useful content (PFS0 or RomFS) relative to the section."""
        h = self.raw_header[8:0x100]
        if self.hash_type == HASH_SHA256:
            layer_count = struct.unpack_from("<I", h, 0x24)[0]
            off, size = struct.unpack_from("<QQ", h, 0x28 + 0x10 * (layer_count - 1))
            return off, size
        if self.hash_type == HASH_IVFC:
            if h[:4] != b"IVFC":
                raise FormatError("invalid IVFC")
            max_layers = struct.unpack_from("<I", h, 0xC)[0]
            lvl = max_layers - 2  # the last level is the data one
            off, size = struct.unpack_from("<QQ", h, 0x10 + 0x18 * lvl)
            return off, size
        return 0, self.end - self.start


class Nca:
    def __init__(self, region: FileRegion, keys: KeySet):
        need_crypto()
        self.region = region
        self.keys = keys
        raw = region.read(0, 0xC00)
        try:
            header_key = keys.get("header_key")
        except KeyError_:
            raise
        hdr = nintendo_xts_decrypt(header_key, raw, 0)
        magic = hdr[0x200:0x204]
        if magic not in (b"NCA3",):
            if magic in (b"NCA2", b"NCA0"):
                raise FormatError(f"{region.name}: format {magic.decode()} not supported (NCA3 only)")
            raise KeyError_(
                f"{region.name}: the header does not decrypt to 'NCA3'. Either the file is not an NCA, "
                "or 'header_key' in prod.keys is incorrect.")
        self.header = hdr
        self.distribution = hdr[0x204]
        self.content_type = hdr[0x205]
        kg_old, self.kaek_index = hdr[0x206], hdr[0x207]
        self.content_size, self.program_id = struct.unpack_from("<QQ", hdr, 0x208)
        self.sdk_version = struct.unpack_from("<I", hdr, 0x21C)[0]
        kg = max(kg_old, hdr[0x220])
        self.key_generation = kg - 1 if kg > 0 else 0
        self.rights_id = hdr[0x230:0x240]
        self.sections: List[FsSection] = []
        for i in range(4):
            start, end = struct.unpack_from("<II", hdr, 0x240 + 0x10 * i)
            if start == 0 and end == 0:
                continue
            fsh = hdr[0x400 + 0x200 * i:0x600 + 0x200 * i]
            want = hdr[0x280 + 0x20 * i:0x2A0 + 0x20 * i]
            import hashlib
            if hashlib.sha256(fsh).digest() != want:
                raise FormatError(f"{region.name}: header FS hash {i} does not match")
            self.sections.append(FsSection(
                index=i, start=start * 0x200, end=end * 0x200,
                fs_type=fsh[2], hash_type=fsh[3], enc_type=fsh[4],
                upper_counter=struct.unpack_from("<Q", fsh, 0x140)[0],
                raw_header=fsh))
        self._key: Optional[bytes] = None

    # --- info ---
    @property
    def has_rights_id(self) -> bool:
        return any(self.rights_id)

    @property
    def type_name(self) -> str:
        return CONTENT_TYPES.get(self.content_type, f"tipo{self.content_type}")

    @property
    def is_patch(self) -> bool:
        return any(s.is_patch for s in self.sections)

    def section(self, idx: int) -> FsSection:
        for s in self.sections:
            if s.index == idx:
                return s
        raise FormatError(f"{self.region.name}: has no section {idx}")

    # --- claves ---
    def content_key(self) -> bytes:
        if self._key is not None:
            return self._key
        kg = self.key_generation
        if self.has_rights_id:
            enc = self.keys.titlekeys.get(self.rights_id)
            if enc is None:
                raise KeyError_(
                    f"{self.region.name}: missing titlekey for Rights ID "
                    f"{self.rights_id.hex().upper()}. It must come in the NSP's ticket (.tik) "
                    "or in ~/.switch/title.keys.")
            kek = self.keys.get(f"titlekek_{kg:02x}")
            self._key = aes_ecb_decrypt(kek, enc)
        else:
            area = ("application", "ocean", "system")[self.kaek_index] if self.kaek_index < 3 else None
            if area is None:
                raise FormatError(f"key area index desconocido {self.kaek_index}")
            kaek = self.keys.get(f"key_area_key_{area}_{kg:02x}")
            self._key = aes_ecb_decrypt(kaek, self.header[0x300 + 0x20:0x300 + 0x30])
        return self._key

    # --- decrypted reading ---
    def read_section(self, sec: FsSection, off: int, size: int, f: Optional[BinaryIO] = None) -> bytes:
        """Read `size` decrypted bytes from `off` (relative to the start of the section)."""
        if sec.enc_type == ENC_NONE:
            return self.region.read(sec.start + off, size, f)
        if sec.enc_type not in (ENC_CTR, ENC_CTR_SKIP, ENC_CTREX, ENC_CTREX_SKIP):
            raise FormatError(f"section encryption {sec.enc_type} not supported")
        abs_off = sec.start + off
        aligned = abs_off & ~0xF
        pad = abs_off - aligned
        raw = self.region.read(aligned, ((pad + size + 0xF) & ~0xF), f)
        iv = struct.pack(">QQ", sec.upper_counter, aligned >> 4)
        dec = AES.new(self.content_key(), AES.MODE_CTR, nonce=b"", initial_value=iv).decrypt(raw)
        return dec[pad:pad + size]

    def section_view(self, idx: int) -> "NcaSectionView":
        sec = self.section(idx)
        off, size = sec.data_region()
        return NcaSectionView(self, sec, off, size)


class NcaSectionView(FileRegion):
    """Decrypted view of the content (PFS0/RomFS) of an NCA section."""

    def __init__(self, nca: Nca, sec: FsSection, off: int, size: int, name: str = None):
        self.nca, self.sec, self.base = nca, sec, off
        super().__init__(nca.region.path, 0, size, name or f"{nca.region.name}#sec{sec.index}")

    def open(self):
        return self.nca.region.open()

    def read(self, off: int, size: int, f=None) -> bytes:
        if off < 0 or off + size > self.size:
            raise FormatError(f"{self.name}: read out of range")
        return self.nca.read_section(self.sec, self.base + off, size, f)

    def sub(self, off: int, size: int, name: str) -> "NcaSectionView":
        return NcaSectionView(self.nca, self.sec, self.base + off, size, name)

    def copy_to(self, dest: Path, chunk: int = 8 << 20) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        with self.open() as f, open(dest, "wb") as o:
            pos = 0
            while pos < self.size:
                n = min(chunk, self.size - pos)
                o.write(self.read(pos, n, f))
                pos += n

    def read_all(self) -> bytes:
        out = bytearray()
        with self.open() as f:
            pos = 0
            while pos < self.size:
                n = min(8 << 20, self.size - pos)
                out += self.read(pos, n, f)
                pos += n
        return bytes(out)


def nca_pfs0_files(nca: Nca, idx: int = 0) -> List[NcaSectionView]:
    sec = nca.section(idx)
    if sec.fs_type != 1:
        raise FormatError(f"{nca.region.name}: section {idx} is not PFS0")
    return parse_pfs0(nca.section_view(idx))  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# RomFS (minimal reading: files by path, no patches)
# ---------------------------------------------------------------------------

def romfs_find(view: FileRegion, wanted: str) -> Optional[FileRegion]:
    """Look for a file in the root of a RomFS (e.g. 'control.nacp')."""
    hdr = view.read(0, 0x50)
    vals = struct.unpack_from("<10Q", hdr, 0)
    file_meta_off, file_meta_size, data_off = vals[7], vals[8], vals[9]
    meta = view.read(file_meta_off, file_meta_size)
    pos = 0
    while pos + 0x20 <= len(meta):
        parent, sibling, off, size, hnext, name_len = struct.unpack_from("<IIQQII", meta, pos)
        name = meta[pos + 0x20:pos + 0x20 + name_len].decode("utf-8", "replace")
        if parent == 0 and name == wanted:
            return view.sub(data_off + off, size, name)
        pos += 0x20 + ((name_len + 3) & ~3)
    return None


# ---------------------------------------------------------------------------
# CNMT / NACP
# ---------------------------------------------------------------------------

CNMT_TYPES = {0x80: "Application", 0x81: "Patch", 0x82: "AddOnContent", 0x83: "Delta"}


@dataclass
class ContentRecord:
    content_id: str     # lowercase hex
    size: int
    ctype: int          # 0 Meta 1 Program 2 Data 3 Control 4 HtmlDocument 5 LegalInfo 6 DeltaFragment
    id_offset: int


@dataclass
class Cnmt:
    title_id: int
    version: int
    meta_type: int
    contents: List[ContentRecord]

    @property
    def type_name(self) -> str:
        return CNMT_TYPES.get(self.meta_type, hex(self.meta_type))

    @property
    def version_str(self) -> str:
        return format_version(self.version)


def format_version(v: int) -> str:
    return f"v{v}"


def parse_cnmt(data: bytes) -> Cnmt:
    title_id, version = struct.unpack_from("<QI", data, 0)
    meta_type = data[0xC]
    ext_size, content_count = struct.unpack_from("<HH", data, 0xE)
    pos = 0x20 + ext_size
    contents = []
    for _ in range(content_count):
        cid = data[pos + 0x20:pos + 0x30].hex()
        size = int.from_bytes(data[pos + 0x30:pos + 0x35], "little")
        ctype, idoff = data[pos + 0x36], data[pos + 0x37]
        contents.append(ContentRecord(cid, size, ctype, idoff))
        pos += 0x38
    return Cnmt(title_id, version, meta_type, contents)


def nacp_display_version(nacp: bytes) -> str:
    return nacp[0x3060:0x3070].split(b"\0")[0].decode("utf-8", "replace")


def nacp_title(nacp: bytes) -> str:
    # Entry 0 = American English; if empty, the first non-empty one.
    for i in range(16):
        name = nacp[i * 0x300:i * 0x300 + 0x200].split(b"\0")[0]
        if name:
            return name.decode("utf-8", "replace")
    return ""


# ---------------------------------------------------------------------------
# Utilidades
# ---------------------------------------------------------------------------

def tid_str(tid: int) -> str:
    return f"{tid:016X}"


def human(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024
    return str(n)
