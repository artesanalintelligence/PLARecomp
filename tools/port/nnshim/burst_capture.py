#!/usr/bin/env python3
"""Capture bursts of the game window (PrintWindow with PW_RENDERFULLCONTENT, works even when another
window covers it), to see animations frame by frame without the 1 s minimum of suyu's captures.

  python tools/port/nnshim/burst_capture.py --out <dir> --start 95 --seconds 20 --hz 8 [--title PLARecomp]

--start: seconds to wait from script start (launch it together with the game). Saves
<dir>/b-<t_ms>.jpg (640x360) with t_ms = milliseconds since script start.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import time
from pathlib import Path

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()
PW_RENDERFULLCONTENT = 2


def find_window(part: str):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _l):
        if user32.IsWindowVisible(h):
            buf = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(h, buf, 512)
            if part in buf.value and "PowerShell" not in buf.value:
                found.append(h)
                return False
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


def grab(hwnd) -> Image.Image | None:
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT | 1)   # 1 = client area only
    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--hz", type=float, default=8.0)
    ap.add_argument("--title", default="PLARecomp", help="substring of the game window title (PLA.exe)")
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()
    time.sleep(max(0.0, args.start))
    hwnd = None
    end = time.monotonic() + args.seconds
    n = 0
    while time.monotonic() < end:
        tick = time.monotonic()
        if not hwnd:
            hwnd = find_window(args.title)
        if hwnd:
            im = grab(hwnd)
            if im:
                im.resize((640, 360)).save(args.out / f"b-{int((tick - t0) * 1000):07d}.jpg", quality=80)
                n += 1
        time.sleep(max(0.0, 1.0 / args.hz - (time.monotonic() - tick)))
    print(f"{n} captures in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
