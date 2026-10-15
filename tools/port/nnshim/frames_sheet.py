#!/usr/bin/env python3
"""Contact sheet of a session's captures (to review many captures at low resolution).

  python tools/port/nnshim/frames_sheet.py <session> --from 400 --to 520 --step 5 --out sheet.png
  python tools/port/nnshim/frames_sheet.py <session> --last 1 --width 640 --out last.png

Each thumbnail carries its capture number. Without --from/--to: the last --last N.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw


def frames(session: Path) -> list[tuple[int, Path]]:
    out = []
    for p in (session / "frames").glob("frame-*.png"):
        m = re.match(r"frame-(\d+)\.png$", p.name)
        if m:
            out.append((int(m.group(1)), p))
    return sorted(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--from", dest="start", type=int)
    ap.add_argument("--to", dest="end", type=int)
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--last", type=int, default=0)
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--width", type=int, default=320, help="width of each thumbnail")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    fr = frames(Path(a.session))
    if not fr:
        print("no captures")
        return 1
    if a.start is not None or a.end is not None:
        lo = a.start if a.start is not None else fr[0][0]
        hi = a.end if a.end is not None else fr[-1][0]
        sel = [f for f in fr if lo <= f[0] <= hi and (f[0] - lo) % a.step == 0]
    else:
        n = a.last or 1
        # the most recent ones; the last one may be half written: skipped if it does not open
        sel = fr[-(n * a.step + 1):][::a.step][-n:]
    imgs = []
    for idx, p in sel:
        try:
            im = Image.open(p).convert("RGB")
            im.load()
        except Exception:
            continue
        h = round(im.height * a.width / im.width)
        im = im.resize((a.width, h), Image.BILINEAR)
        ImageDraw.Draw(im).text((4, 2), str(idx), fill=(255, 0, 255))
        imgs.append(im)
    if not imgs:
        print("no readable capture")
        return 1
    cols = min(a.cols, len(imgs))
    rows = (len(imgs) + cols - 1) // cols
    w, h = imgs[0].size
    sheet = Image.new("RGB", (cols * w, rows * h))
    for i, im in enumerate(imgs):
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    sheet.save(a.out)
    print(f"{a.out}: {len(imgs)} captures ({sel[0][0]}..{sel[-1][0]}), last available {fr[-1][0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
