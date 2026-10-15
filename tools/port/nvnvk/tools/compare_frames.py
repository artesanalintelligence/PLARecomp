#!/usr/bin/env python3
"""Compares an nvnvk backend capture (PPM) with a suyu one (PNG) or another image.

  python compare_frames.py nvnvk-f000600.ppm suyu.shot-120.png [--out diff.png]

Scales the second image to the first one's size (suyu captures at its window resolution) and
reports the mean absolute error per channel, the PSNR and the percentage of pixels that differ by
more than 16 levels. With --out it saves a side-by-side image plus the difference (amplified x4).
"""
from __future__ import annotations

import argparse
import math
import sys

from PIL import Image, ImageChops


def load(path: str) -> Image.Image:
    return Image.open(path).convert("RGB")


def compare(a: Image.Image, b: Image.Image):
    if b.size != a.size:
        b = b.resize(a.size, Image.BILINEAR)
    diff = ImageChops.difference(a, b)
    px = list(diff.getdata())
    n = len(px)
    mae = sum(sum(p) for p in px) / (3.0 * n)
    mse = sum(p[0] ** 2 + p[1] ** 2 + p[2] ** 2 for p in px) / (3.0 * n)
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
    big = sum(1 for p in px if max(p) > 16) * 100.0 / n
    return b, diff, mae, psnr, big


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ours")
    ap.add_argument("ref")
    ap.add_argument("--out")
    a = ap.parse_args()
    ours, ref = load(a.ours), load(a.ref)
    ref2, diff, mae, psnr, big = compare(ours, ref)
    print(f"{a.ours} vs {a.ref}: {ours.size[0]}x{ours.size[1]}  MAE={mae:.2f}  PSNR={psnr:.2f} dB  "
          f"pixels with |d|>16: {big:.2f} %")
    if a.out:
        w, h = ours.size
        side = Image.new("RGB", (w * 3, h))
        side.paste(ours, (0, 0))
        side.paste(ref2, (w, 0))
        side.paste(diff.point(lambda v: min(255, v * 4)), (2 * w, 0))
        side.save(a.out)
        print(f"-> {a.out} (backend | suyu | difference x4)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
