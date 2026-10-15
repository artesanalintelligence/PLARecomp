"""Reading and writing of NSO (Switch) files to patch the module before the AOT export.

Header format (0x100 bytes, documented by switchbrew):
  0x00 'NSO0'  0x04 version  0x0C flags (bits 0-2: compressed segment, bits 3-5: check hash)
  0x10/0x20/0x30 text/rodata/data segments: {file_off, mem_off, size} (+ u32 extra)
  0x3C bss_size  0x40 module_id (build id, 0x20)  0x60 compressed sizes (3 x u32)
  0x88 embedded/dynstr/dynsym {off, size}  0xA0 SHA-256 of each decompressed segment

All addresses in this module are "module offsets" (mem_off), the same as in
pchtxt files with `@flag offset_shift 0x100` and as in Ghidra after subtracting the base 0x7100000000.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field
from pathlib import Path

HEADER_SIZE = 0x100
SEG_NAMES = ("text", "rodata", "data")


class NsoError(Exception):
    pass


@dataclass
class Segment:
    file_off: int
    mem_off: int
    size: int
    extra: int          # module_name_offset / module_name_size / bss_size depending on the segment
    file_size: int      # size in the file (compressed or not)
    compressed: bool
    hash: bytes


@dataclass
class Nso:
    header: bytearray
    segs: list[Segment]
    module_name: bytes
    image: bytearray = field(repr=False)   # module memory from 0 to the end of .data

    @property
    def build_id(self) -> str:
        return self.header[0x40:0x60].hex().upper()

    @property
    def build_id_short(self) -> str:
        return self.header[0x40:0x54].hex().upper()

    @property
    def bss_size(self) -> int:
        return self.segs[2].extra

    def seg_of(self, off: int, size: int = 1) -> str | None:
        for name, s in zip(SEG_NAMES, self.segs):
            if s.mem_off <= off and off + size <= s.mem_off + s.size:
                return name
        return None

    def text_gap(self) -> tuple[int, int]:
        """Gap between the end of .text and the start of .rodata (padding, mapped as code)."""
        t, r = self.segs[0], self.segs[1]
        return t.mem_off + t.size, r.mem_off

    def read(self, off: int, size: int) -> bytes:
        end = off + size
        if end <= len(self.image):
            return bytes(self.image[off:end])
        # Outside .data (bss) memory starts as zero.
        data_end = self.segs[2].mem_off + self.segs[2].size
        if off >= data_end and end <= data_end + self.bss_size:
            return bytes(size)
        raise NsoError(f"range 0x{off:x}+0x{size:x} outside the image")

    def write(self, off: int, data: bytes) -> None:
        end = off + len(data)
        if end > len(self.image):
            raise NsoError(f"write at 0x{off:x} outside text/rodata/data (bss cannot be baked)")
        self.image[off:end] = data

    def extend_text(self, new_end: int) -> None:
        """Grows .text to include bytes of the text->rodata gap (code caves)."""
        t, r = self.segs[0], self.segs[1]
        if new_end <= t.mem_off + t.size:
            return
        if new_end > r.mem_off:
            raise NsoError(f".text cannot grow to 0x{new_end:x} (rodata starts at 0x{r.mem_off:x})")
        t.size = new_end - t.mem_off


def _lz4_decompress(data: bytes, size: int) -> bytes:
    import lz4.block
    return lz4.block.decompress(data, uncompressed_size=size)


def _lz4_compress(data: bytes) -> bytes:
    import lz4.block
    return lz4.block.compress(data, mode="high_compression", store_size=False)


def load(path: Path | str) -> Nso:
    raw = Path(path).read_bytes()
    if len(raw) < HEADER_SIZE or raw[:4] != b"NSO0":
        raise NsoError(f"{path}: not an NSO")
    hdr = bytearray(raw[:HEADER_SIZE])
    flags = struct.unpack_from("<I", hdr, 0xC)[0]
    csizes = struct.unpack_from("<3I", hdr, 0x60)
    segs = []
    for i in range(3):
        file_off, mem_off, size, extra = struct.unpack_from("<4I", hdr, 0x10 + i * 0x10)
        segs.append(Segment(file_off, mem_off, size, extra, csizes[i], bool(flags >> i & 1),
                            bytes(hdr[0xA0 + i * 0x20:0xC0 + i * 0x20])))
    name_off, name_size = segs[0].extra, segs[1].extra
    module_name = raw[name_off:name_off + name_size] if name_size else b""
    end = segs[2].mem_off + segs[2].size
    image = bytearray(end)
    for name, s in zip(SEG_NAMES, segs):
        blob = raw[s.file_off:s.file_off + s.file_size]
        data = _lz4_decompress(blob, s.size) if s.compressed else blob
        if len(data) != s.size:
            raise NsoError(f"{path}: segment {name}: {len(data)} bytes, expected {s.size}")
        image[s.mem_off:s.mem_off + s.size] = data
    return Nso(hdr, segs, module_name, image)


def build(nso: Nso, compress: bool = False) -> bytes:
    """Serializes the NSO. Uncompressed by default (faster, and suyu accepts it just the same).

    Recomputes the segment SHA-256 hashes and keeps the build id, module name and
    the embedded/dynstr/dynsym offsets (they are relative to .rodata)."""
    hdr = bytearray(nso.header)
    flags = struct.unpack_from("<I", hdr, 0xC)[0] & ~0x7
    out = bytearray(HEADER_SIZE)
    out += nso.module_name
    for i, s in enumerate(nso.segs):
        data = bytes(nso.image[s.mem_off:s.mem_off + s.size])
        blob = data
        if compress:
            blob = _lz4_compress(data)
            flags |= 1 << i
        s.file_off = len(out)
        s.file_size = len(blob)
        s.hash = hashlib.sha256(data).digest()
        out += blob
        struct.pack_into("<4I", hdr, 0x10 + i * 0x10, s.file_off, s.mem_off, s.size,
                         HEADER_SIZE if i == 0 else (len(nso.module_name) if i == 1 else s.extra))
        struct.pack_into("<I", hdr, 0x60 + i * 4, s.file_size)
        hdr[0xA0 + i * 0x20:0xC0 + i * 0x20] = s.hash
    if not nso.module_name:
        struct.pack_into("<I", hdr, 0x1C, 0)
    struct.pack_into("<I", hdr, 0xC, flags)
    out[:HEADER_SIZE] = hdr
    return bytes(out)
