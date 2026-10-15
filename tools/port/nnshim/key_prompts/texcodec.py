"""Minimal Switch texture codec for the button prompts (BNTX in SARC/GFLXPACK).

- Tegra X1 block-linear deswizzle/swizzle (64x8-byte GOBs, block height 2^n GOBs).
- BC1/BC3/BC4/BC5: decode to RGBA and encode from RGBA (simple encoder: min/max
  per block; good enough for flat 16-84 px prompts).
- SARC and BNTX reading (names, format, size, mip 0 data).

Uses only the standard library and numpy. Contains no game data.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

# BNTX formats used by the prompts (type << 8 | variant).
FMT_BC1, FMT_BC3, FMT_BC4, FMT_BC5 = 0x1A01, 0x1C01, 0x1D01, 0x1E01
BLOCK_BYTES = {FMT_BC1: 8, FMT_BC3: 16, FMT_BC4: 8, FMT_BC5: 16}


# ---------------------------------------------------------------- containers
def sarc_entries(d: bytes):
    """[(name, start, end)] with absolute offsets inside the SARC."""
    if d[:4] != b"SARC":
        raise ValueError("not a SARC")
    hl, _bom, _size, dataoff = struct.unpack_from("<HHII", d, 4)
    o = hl
    if d[o:o + 4] != b"SFAT":
        raise ValueError("SFAT")
    _, n, _ = struct.unpack_from("<HHI", d, o + 4)
    ents = [struct.unpack_from("<IIII", d, o + 12 + 16 * i) for i in range(n)]
    so = o + 12 + 16 * n
    nt = so + 8
    out = []
    for _h, attr, s, e in ents:
        name = ""
        if attr & 0x01000000:
            no = nt + (attr & 0xFFFF) * 4
            name = d[no:d.index(b"\0", no)].decode("utf-8", "replace")
        out.append((name, dataoff + s, dataoff + e))
    return out


@dataclass
class Tex:
    name: str
    fmt: int
    w: int
    h: int
    mips: int
    bh_log2: int
    comp: bytes          # BNTX channel selector (R,G,B,A): 0 zero, 1 one, 2 R, 3 G, 4 B, 5 A
    data_off: int        # mip 0 offset relative to the start of the BNTX
    size: int            # mip 0 bytes (computed)
    image_size: int      # bytes of all mips (BRTI)


def bntx_textures(d: bytes, base: int = 0) -> list[Tex]:
    """Textures of a BNTX starting at d[base:]."""
    if d[base:base + 4] != b"BNTX":
        raise ValueError("not a BNTX")
    cnt, = struct.unpack_from("<I", d, base + 0x24)
    infop, = struct.unpack_from("<Q", d, base + 0x28)
    out = []
    for i in range(cnt):
        o, = struct.unpack_from("<Q", d, base + infop + 8 * i)
        o += base
        if d[o:o + 4] != b"BRTI":
            raise ValueError("BRTI")
        _flags, _dim, _tile, _swz, mips, _ms = struct.unpack_from("<BBHHHH", d, o + 0x10)
        fmt, _acc, w, h, _depth, _arr, layout, _l2 = struct.unpack_from("<IIiiiiii", d, o + 0x1C)
        isize, = struct.unpack_from("<I", d, o + 0x50)
        comp = d[o + 0x58:o + 0x5C]
        nameo, _parent, ptrs = struct.unpack_from("<QQQ", d, o + 0x60)
        n, = struct.unpack_from("<H", d, base + nameo)
        name = d[base + nameo + 2:base + nameo + 2 + n].decode("utf-8", "replace")
        m0, = struct.unpack_from("<Q", d, base + ptrs)
        bh = layout & 7
        size = surface_size(fmt, w, h, bh) if fmt in BLOCK_BYTES else 0
        out.append(Tex(name, fmt, w, h, mips, bh, comp, m0, size, isize))
    return out


# ---------------------------------------------------------------- block-linear
def _dims(fmt, w, h):
    return (w + 3) // 4, (h + 3) // 4, BLOCK_BYTES[fmt]


def surface_size(fmt: int, w: int, h: int, bh_log2: int) -> int:
    bw, bhh, bpp = _dims(fmt, w, h)
    gobs_w = (bw * bpp + 63) // 64
    rows = 8 << bh_log2
    return gobs_w * ((bhh + rows - 1) // rows) * 512 * (1 << bh_log2)


def _addr_table(bw: int, bhh: int, bpp: int, bh_log2: int) -> np.ndarray:
    """Byte offset of each block (y, x) in the block-linear surface."""
    bhb = 1 << bh_log2
    gobs_w = (bw * bpp + 63) // 64
    y = np.arange(bhh)[:, None]
    x = np.arange(bw)[None, :] * bpp
    gob = (y // (8 * bhb)) * 512 * bhb * gobs_w + (x // 64) * 512 * bhb + ((y % (8 * bhb)) // 8) * 512
    return gob + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)


def deswizzle(fmt, w, h, bh_log2, data: bytes) -> np.ndarray:
    bw, bhh, bpp = _dims(fmt, w, h)
    a = np.frombuffer(data, np.uint8)
    t = _addr_table(bw, bhh, bpp, bh_log2)
    idx = t[..., None] + np.arange(bpp)
    return a[idx]                                   # (bhh, bw, bpp)


def swizzle(fmt, w, h, bh_log2, blocks: np.ndarray, size: int) -> bytes:
    bw, bhh, bpp = _dims(fmt, w, h)
    out = np.zeros(size, np.uint8)
    t = _addr_table(bw, bhh, bpp, bh_log2)
    out[t[..., None] + np.arange(bpp)] = blocks
    return out.tobytes()


# ---------------------------------------------------------------- BCn
def _bc4_dec(b: np.ndarray) -> np.ndarray:
    """b: (..., 8) -> (..., 16) values 0..255."""
    r0 = b[..., 0].astype(np.int32)
    r1 = b[..., 1].astype(np.int32)
    bits = np.zeros(b.shape[:-1], np.uint64)
    for k in range(6):
        bits |= b[..., 2 + k].astype(np.uint64) << np.uint64(8 * k)
    idx = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64) for i in range(16)], -1)
    pal = np.zeros(b.shape[:-1] + (8,), np.int32)
    pal[..., 0] = r0
    pal[..., 1] = r1
    big = (r0 > r1)[..., None]
    i8 = np.arange(1, 7)
    p8 = ((7 - i8) * r0[..., None] + i8 * r1[..., None]) // 7
    i6 = np.arange(1, 5)
    p6 = ((5 - i6) * r0[..., None] + i6 * r1[..., None]) // 5
    p6 = np.concatenate([p6, np.zeros_like(p6[..., :1]), np.full_like(p6[..., :1], 255)], -1)
    pal[..., 2:] = np.where(big, p8, p6)
    return np.take_along_axis(pal, idx, -1)


def _bc4_enc(v: np.ndarray) -> np.ndarray:
    """v: (..., 16) 0..255 -> (..., 8) (8-level mode, r0 >= r1)."""
    v = v.astype(np.int32)
    r0 = v.max(-1)
    r1 = v.min(-1)
    same = r0 == r1
    pal = np.zeros(v.shape[:-1] + (8,), np.int32)
    pal[..., 0] = r0
    pal[..., 1] = r1
    i8 = np.arange(1, 7)
    pal[..., 2:] = ((7 - i8) * r0[..., None] + i8 * r1[..., None]) // 7
    d = np.abs(v[..., :, None] - pal[..., None, :])
    idx = d.argmin(-1)
    idx = np.where(same[..., None], 0, idx)
    bits = np.zeros(v.shape[:-1], np.uint64)
    for i in range(16):
        bits |= idx[..., i].astype(np.uint64) << np.uint64(3 * i)
    out = np.zeros(v.shape[:-1] + (8,), np.uint8)
    out[..., 0] = r0
    out[..., 1] = r1
    for k in range(6):
        out[..., 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def _565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def _bc1_pal(c0, c1):
    e0 = np.stack(_565(c0), -1)
    e1 = np.stack(_565(c1), -1)
    return np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], -2)   # 4-color mode


def _bc1_dec(b: np.ndarray) -> np.ndarray:
    c0 = b[..., 0].astype(np.int32) | (b[..., 1].astype(np.int32) << 8)
    c1 = b[..., 2].astype(np.int32) | (b[..., 3].astype(np.int32) << 8)
    bits = b[..., 4].astype(np.int64) | (b[..., 5].astype(np.int64) << 8) | (b[..., 6].astype(np.int64) << 16) | (
        b[..., 7].astype(np.int64) << 24)
    pal = _bc1_pal(c0, c1)
    idx = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], -1)
    return np.take_along_axis(pal, idx[..., None].repeat(3, -1), -2)


def _bc1_enc(rgb: np.ndarray) -> np.ndarray:
    """rgb: (..., 16, 3) -> (..., 8), 4-color mode (c0 > c1)."""
    rgb = rgb.astype(np.int32)
    lum = rgb @ np.array([2, 4, 1])
    i_hi = lum.argmax(-1)
    i_lo = lum.argmin(-1)
    hi = np.take_along_axis(rgb, i_hi[..., None, None].repeat(3, -1), -2)[..., 0, :]
    lo = np.take_along_axis(rgb, i_lo[..., None, None].repeat(3, -1), -2)[..., 0, :]

    def pack(c):
        return (((c[..., 0] * 31 + 127) // 255) << 11) | (((c[..., 1] * 63 + 127) // 255) << 5) | (
            (c[..., 2] * 31 + 127) // 255)

    c0 = pack(hi)
    c1 = pack(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    pal = _bc1_pal(c0, c1)
    d = ((rgb[..., :, None, :] - pal[..., None, :, :]) ** 2).sum(-1)
    idx = d.argmin(-1)
    idx = np.where((c0 == c1)[..., None], 0, idx)
    bits = np.zeros(rgb.shape[:-2], np.int64)
    for i in range(16):
        bits |= idx[..., i].astype(np.int64) << (2 * i)
    out = np.zeros(rgb.shape[:-2] + (8,), np.uint8)
    out[..., 0] = c0 & 0xFF
    out[..., 1] = c0 >> 8
    out[..., 2] = c1 & 0xFF
    out[..., 3] = c1 >> 8
    for k in range(4):
        out[..., 4 + k] = (bits >> (8 * k)) & 0xFF
    return out


def _blocks_to_image(px: np.ndarray, w: int, h: int) -> np.ndarray:
    """(bhh, bw, 16, C) -> (h, w, C)."""
    bhh, bw = px.shape[:2]
    c = px.shape[-1]
    img = px.reshape(bhh, bw, 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(bhh * 4, bw * 4, c)
    return img[:h, :w]


def _image_to_blocks(img: np.ndarray) -> np.ndarray:
    h, w, c = img.shape
    bh, bw = (h + 3) // 4, (w + 3) // 4
    pad = np.zeros((bh * 4, bw * 4, c), img.dtype)
    pad[:h, :w] = img
    pad[h:, :w] = img[h - 1:h, :]                 # repeat the edge
    pad[:, w:] = pad[:, w - 1:w]
    return pad.reshape(bh, 4, bw, 4, c).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, c)


def decode(tex: Tex, data: bytes) -> np.ndarray:
    """Swizzled mip 0 -> (h, w, 4) uint8 image with the format's RAW channels
    (BC4: R in RGB; BC5: R, G; BC1/BC3: RGBA)."""
    blk = deswizzle(tex.fmt, tex.w, tex.h, tex.bh_log2, data)
    if tex.fmt == FMT_BC4:
        r = _bc4_dec(blk)
        px = np.stack([r, r, r, np.full_like(r, 255)], -1)
    elif tex.fmt == FMT_BC5:
        r = _bc4_dec(blk[..., :8])
        g = _bc4_dec(blk[..., 8:])
        px = np.stack([r, g, np.zeros_like(r), np.full_like(r, 255)], -1)
    elif tex.fmt == FMT_BC3:
        a = _bc4_dec(blk[..., :8])
        rgb = _bc1_dec(blk[..., 8:])
        px = np.concatenate([rgb, a[..., None]], -1)
    elif tex.fmt == FMT_BC1:
        rgb = _bc1_dec(blk)
        px = np.concatenate([rgb, np.full(rgb.shape[:-1] + (1,), 255)], -1)
    else:
        raise ValueError(hex(tex.fmt))
    return _blocks_to_image(px.astype(np.uint8), tex.w, tex.h)


def encode(tex: Tex, img: np.ndarray) -> bytes:
    """(h, w, 4) image with RAW channels (see decode) -> swizzled mip 0 of tex.size bytes."""
    blk = _image_to_blocks(img.astype(np.int32))
    if tex.fmt == FMT_BC4:
        b = _bc4_enc(blk[..., 0])
    elif tex.fmt == FMT_BC5:
        b = np.concatenate([_bc4_enc(blk[..., 0]), _bc4_enc(blk[..., 1])], -1)
    elif tex.fmt == FMT_BC3:
        b = np.concatenate([_bc4_enc(blk[..., 3]), _bc1_enc(blk[..., :3])], -1)
    elif tex.fmt == FMT_BC1:
        b = _bc1_enc(blk[..., :3])
    else:
        raise ValueError(hex(tex.fmt))
    return swizzle(tex.fmt, tex.w, tex.h, tex.bh_log2, b, tex.size)


def to_visible(tex: Tex, raw: np.ndarray) -> np.ndarray:
    """Applies the BNTX channel selector: raw channels -> RGBA as the shader sees it."""
    src = {0: np.zeros_like(raw[..., 0]), 1: np.full_like(raw[..., 0], 255), 2: raw[..., 0], 3: raw[..., 1],
           4: raw[..., 2], 5: raw[..., 3]}
    return np.stack([src.get(c, raw[..., 0]) for c in tex.comp], -1)
