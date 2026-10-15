#!/usr/bin/env python3
"""Converts nvnvk's raw render target dumps (option "dump_rts") to PNG.

Each file is named rt-<sec>s-<gpu>-<w>x<h>-f<VkFormat>.bin (level 0, layer 0; depth targets contain
only the depth aspect). Floating-point formats are clamped to [0, 1]; depth is normalized between
its min and max so something is visible.

  python tools/port/nvnvk/tools/rt_dump.py <folder> [--out <folder>] [--sec 70] [--montage]
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

import numpy as np
from PIL import Image

NAME = re.compile(r"rt-(\d+)s-([0-9a-f]+)-(\d+)x(\d+)-f(\d+)\.bin$")

# VkFormat -> (description, bytes->float image HxWx(1..4) function)
def _u8(n):
    return lambda b, w, h: np.frombuffer(b, np.uint8).reshape(h, w, n) / 255.0


def _f16(n):
    return lambda b, w, h: np.frombuffer(b, np.float16).reshape(h, w, n).astype(np.float32)


def _f32(n):
    return lambda b, w, h: np.frombuffer(b, np.float32).reshape(h, w, n)


def _unorm16(n):
    return lambda b, w, h: np.frombuffer(b, np.uint16).reshape(h, w, n) / 65535.0


def _b10g11r11(b, w, h):
    v = np.frombuffer(b, np.uint32).reshape(h, w)

    def uf(bits, mbits):
        e = (bits >> mbits) & 0x1F
        m = bits & ((1 << mbits) - 1)
        return np.where(e == 0, m / (1 << mbits) * 2.0**-14, (1 + m / (1 << mbits)) * 2.0 ** (e.astype(np.float64) - 15))

    r = uf(v & 0x7FF, 6)
    g = uf((v >> 11) & 0x7FF, 6)
    bl = uf((v >> 22) & 0x3FF, 5)
    return np.stack([r, g, bl], -1).astype(np.float32)


def _a2b10g10r10(b, w, h):
    v = np.frombuffer(b, np.uint32).reshape(h, w)
    return np.stack([(v & 0x3FF), (v >> 10) & 0x3FF, (v >> 20) & 0x3FF], -1) / 1023.0


def _d24(b, w, h):
    v = np.frombuffer(b, np.uint32).reshape(h, w) & 0xFFFFFF
    return (v / float(0xFFFFFF))[..., None]


FORMATS = {
    9: ("R8_UNORM", _u8(1)), 16: ("R8G8_UNORM", _u8(2)), 37: ("R8G8B8A8_UNORM", _u8(4)),
    43: ("R8G8B8A8_SRGB", _u8(4)), 44: ("B8G8R8A8_UNORM", _u8(4)), 50: ("B8G8R8A8_SRGB", _u8(4)),
    64: ("A2B10G10R10_UNORM", _a2b10g10r10), 70: ("R16_UNORM", _unorm16(1)), 76: ("R16_SFLOAT", _f16(1)),
    77: ("R16G16_UNORM", _unorm16(2)), 83: ("R16G16_SFLOAT", _f16(2)), 91: ("R16G16B16A16_UNORM", _unorm16(4)),
    97: ("R16G16B16A16_SFLOAT", _f16(4)), 100: ("R32_SFLOAT", _f32(1)), 103: ("R32G32_SFLOAT", _f32(2)),
    109: ("R32G32B32A32_SFLOAT", _f32(4)), 122: ("B10G11R11_UFLOAT", _b10g11r11), 124: ("D16_UNORM", _unorm16(1)),
    126: ("D32_SFLOAT", _f32(1)), 129: ("D24_UNORM_S8_UINT", _d24), 130: ("D32_SFLOAT_S8_UINT", _f32(1)),
}
DEPTH = {124, 126, 129, 130}


def to_rgb(img: np.ndarray, depth: bool) -> np.ndarray:
    if depth:
        d = img[..., 0]
        lo, hi = float(np.nanmin(d)), float(np.nanmax(d))
        d = (d - lo) / (hi - lo) if hi > lo else d * 0
        img = d[..., None]
    c = img.shape[-1]
    if c == 1:
        rgb = np.repeat(img, 3, -1)
    elif c == 2:
        rgb = np.concatenate([img, np.zeros_like(img[..., :1])], -1)
    else:
        rgb = img[..., :3]
    rgb = np.nan_to_num(rgb, nan=1.0, posinf=1.0, neginf=0.0)
    return (np.clip(rgb, 0, 1) * 255).astype(np.uint8)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--out", default="")
    ap.add_argument("--sec", type=int, default=-1)
    ap.add_argument("--montage", action="store_true", help="also a contact sheet of thumbnails")
    a = ap.parse_args(argv)
    src = Path(a.folder)
    out = Path(a.out) if a.out else src / "rt-png"
    out.mkdir(parents=True, exist_ok=True)
    thumbs = []
    for f in sorted(src.glob("rt-*.bin")):
        m = NAME.search(f.name)
        if not m:
            continue
        sec, gpu, w, h, fmt = int(m[1]), m[2], int(m[3]), int(m[4]), int(m[5])
        if a.sec >= 0 and sec != a.sec:
            continue
        desc, conv = FORMATS.get(fmt, (f"fmt{fmt}", _u8(4)))
        data = f.read_bytes()
        try:
            img = conv(data, w, h)
        except ValueError as e:
            print(f"{f.name}: {e}")
            continue
        rgb = to_rgb(img, fmt in DEPTH)
        p = out / f"{f.stem}-{desc}.png"
        Image.fromarray(rgb).save(p)
        mean = img.reshape(-1, img.shape[-1]).mean(0)
        print(f"{p.name}  mean={np.round(mean, 3).tolist()}")
        t = Image.fromarray(rgb)
        t.thumbnail((320, 180))
        thumbs.append((f"{gpu} {w}x{h} {desc}", t))
    if a.montage and thumbs:
        from PIL import ImageDraw
        cols = 6
        rows = (len(thumbs) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * 320, rows * 200), (40, 40, 40))
        d = ImageDraw.Draw(sheet)
        for i, (label, t) in enumerate(thumbs):
            x, y = (i % cols) * 320, (i // cols) * 200
            sheet.paste(t, (x, y))
            d.text((x + 2, y + 182), label[:52], fill=(255, 255, 0))
        sheet.save(out / "montage.png")
        print(f"sheet: {out / 'montage.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
