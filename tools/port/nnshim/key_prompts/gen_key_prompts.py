#!/usr/bin/env python3
"""Generates key_prompts.bin: the game's button icons redrawn with the user's keys and mouse buttons.

The game draws button prompts (A, B, ZR, sticks...) with BNTX textures from its layouts
(button_icon_A_00^t, ...), not with a font. This script:
  1. finds those textures in the user's extracted RomFS (bin/appli/**.arc, uncompressed SARC) and in
     the BNTX files the nnshim dumped from the GFLXPACKs (--dump, option "ui.dump_dir"; those are Oodle);
  2. reads the controls file (keyboard_map, mouse.buttons) to know which key or mouse button acts as
     each Switch button;
  3. draws, for each icon, a key (or a mouse) of the same size and encodes it in the same format
     (BC4/BC5/BC3, block-linear), and
  4. writes key_prompts.bin (loaded by the nnshim, src/abi_ui.cpp) and, with --preview, PNG images.

The output contains data derived from the user's copy of the game: it stays on their PC (it is never
published or committed).

Usage:
  python tools/port/nnshim/key_prompts/gen_key_prompts.py --romfs D:/PLARecomp/romfs \
      --controls D:/PLARecomp/nnshim-v11/controls.json --out D:/PLARecomp/nnshim-v11/key_prompts.bin \
      [--dump D:/PLARecomp/nnshim-v11/ui-dump] [--preview <folder>]
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import texcodec as tc  # noqa: E402

SS = 4   # supersampling while drawing

# ---------------------------------------------------------------- controls file
DEFAULT_KEYS = {
    "A": "RETURN", "B": "BACK", "X": "LSHIFT", "Y": "SPACE", "L": "Q", "R": "E", "ZL": "1", "ZR": "3",
    "Plus": "TAB", "Minus": "M", "StickL": "Z", "StickR": "C", "Up": "UP", "Down": "DOWN",
    "Left": "LEFT", "Right": "RIGHT", "LStickUp": "W", "LStickDown": "S", "LStickLeft": "A",
    "LStickRight": "D", "RStickUp": "I", "RStickDown": "K", "RStickLeft": "J", "RStickRight": "L",
}
DEFAULT_MOUSE = {"left": "ZR", "right": "ZL", "middle": "", "x1": "", "x2": "", "wheel_up": "L", "wheel_down": "R"}

# Key labels (overridable with "ui.labels" in the controls file). The short labels of the navigation keys
# use the short English key-cap names (Spc, Home, End, PgUp, PgDn, Del).
LABELS = {
    "RETURN": "\u21b5", "ENTER": "\u21b5", "BACK": "\u232b", "BACKSPACE": "\u232b", "SPACE": "Spc",
    "LSHIFT": "\u21e7", "RSHIFT": "\u21e7", "SHIFT": "\u21e7", "TAB": "Tab", "ESCAPE": "Esc", "ESC": "Esc",
    "LCTRL": "Ctrl", "RCTRL": "Ctrl", "CONTROL": "Ctrl", "LALT": "Alt", "RALT": "Alt", "UP": "\u2191",
    "DOWN": "\u2193", "LEFT": "\u2190", "RIGHT": "\u2192", "HOME": "Home", "END": "End", "PAGEUP": "PgUp",
    "PAGEDOWN": "PgDn", "INSERT": "Ins", "DELETE": "Del", "COMMA": ",", "PERIOD": ".", "MINUS": "-",
    "PLUS": "+",
}


def load_controls(path: str | None) -> dict:
    # A missing controls.json falls back to the legacy name controles.json in the same folder.
    if path and not os.path.exists(path) and os.path.basename(path) == "controls.json":
        path = os.path.join(os.path.dirname(path), "controles.json")
    if not path or not os.path.exists(path):
        return {}
    text = open(path, encoding="utf-8-sig").read()
    # Strip // comments outside strings.
    out, i, ins = [], 0, False
    while i < len(text):
        c = text[i]
        if ins:
            out.append(c)
            if c == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                ins = False
        elif c == '"':
            ins = True
            out.append(c)
        elif text.startswith("//", i):
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return json.loads("".join(out))


class Bindings:
    """What to draw for each Switch button."""

    def __init__(self, ctl: dict):
        km = dict(DEFAULT_KEYS)
        km.update(ctl.get("keyboard_map", {}) or {})
        self.keys = {b: [k.strip() for k in str(v).split(",") if k.strip()] for b, v in km.items()}
        mouse = ctl.get("mouse", {}) or {}
        en = mouse.get("enabled", "auto")
        self.mouse_on = not (en is False or str(en).lower() in ("off", "false", "0"))
        mb = dict(DEFAULT_MOUSE)
        mb.update(mouse.get("buttons", {}) or {})
        self.mouse = {}   # Switch button -> "left" | "right" | "middle"
        ui = ctl.get("ui", {}) or {}
        # By default, buttons that are also on the mouse (ZR = left click...) are labeled with their key:
        # in menus the clicks are A/B ("mouse_menus"), so a mouse on the inventory's ZL/ZR tab would be
        # confusing. "prompt_mouse": true draws the mouse.
        if self.mouse_on and ui.get("prompt_mouse", False):
            for phys in ("left", "right", "middle"):
                for b in re.split(r"[+,| ]", str(mb.get(phys, ""))):
                    if b and b not in self.mouse:
                        self.mouse[b] = phys
        self.labels = dict(LABELS)
        self.labels.update((ctl.get("ui", {}) or {}).get("labels", {}) or {})

    def label(self, key: str) -> str:
        k = key.upper()
        if k in self.labels:
            return self.labels[k]
        if k.startswith("NUMPAD"):
            return "N" + k[6:]
        if k.startswith("VK_"):
            return "?"
        return key.upper() if len(key) <= 3 else key[:3].capitalize()

    def what(self, button: str):
        """('mouse', 'left') or ('key', 'Q') or None."""
        if button in self.mouse:
            return ("mouse", self.mouse[button])
        ks = self.keys.get(button) or []
        return ("key", self.label(ks[0])) if ks else None

    def stick(self, which: str):
        if which == "R" and self.mouse_on:
            return ("mouse", "move")
        p = "LStick" if which == "L" else "RStick"
        ks = [self.keys.get(p + d, [""])[0] for d in ("Up", "Left", "Down", "Right")]
        if all(ks):
            return ("cluster", [self.label(k) for k in ks])
        return None


# ---------------------------------------------------------------- texture classification
# Name (without the ^x suffix) -> (kind, value). Anything not listed here (shadows "_ds", outlines, ABXY
# quarters, bases) is left as in the game.
def classify(name: str):
    base = name.split("^")[0]
    if re.search(r"_ds|outline|quarter|shrinked|_base|keyguid_", base):
        return None
    m = re.fullmatch(r"(?:button_icon|btn_hud)_(A|B|X|Y|L|R|ZL|ZR|zl|zr|plus|minus)_(?:\d\d|round_\d\d|l_\d\d)", base)
    if m:
        b = {"plus": "Plus", "minus": "Minus", "zl": "ZL", "zr": "ZR"}.get(m.group(1), m.group(1))
        return ("button", b)
    m = re.fullmatch(r"button_icon_stick_(up|down|ud)_r_\d\d", base)
    if m:
        return ("rstick_dir", m.group(1))
    m = re.fullmatch(r"button_icon_stick_(l|r)_\d\d", base)
    if m:
        return ("stick", m.group(1).upper())
    m = re.fullmatch(r"button_icon_stick_push_(l|r)_\d\d", base)
    if m:
        return ("button", "StickL" if m.group(1) == "l" else "StickR")
    m = re.fullmatch(r"button_icon_stick_lr_(l|r)_\d\d", base)
    if m:
        return ("stick_lr", m.group(1).upper())
    m = re.fullmatch(r"button_icon_direct_(\d\d)", base)
    if m:
        return ("button", "Left" if m.group(1) == "00" else "Right")
    return None


# ---------------------------------------------------------------- drawing
FONT_DIRS = [os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts")]


def font(size: int, symbol: bool = False):
    names = ["seguisym.ttf", "segoeuib.ttf", "arialbd.ttf"] if symbol else ["segoeuib.ttf", "arialbd.ttf", "seguisym.ttf"]
    for d in FONT_DIRS:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def is_symbol(text: str) -> bool:
    return any(ord(c) > 0x2000 for c in text)


def fit_text(draw: ImageDraw.ImageDraw, text: str, box_w: int, box_h: int):
    """Largest font (and stroke width) with which the text fits in the box."""
    sym = is_symbol(text)
    size = int(box_h * (1.3 if sym else 0.9))
    while size > 6:
        f = font(size, sym)
        sw = max(1, size // (20 if sym else 30))   # Segoe UI Symbol glyphs are thin: thicken them
        l, t, r, b = draw.textbbox((0, 0), text, font=f, stroke_width=sw)
        if r - l <= box_w and b - t <= box_h:
            return f, sw, (l, t, r, b)
        size -= 2
    f = font(6, sym)
    return f, 1, draw.textbbox((0, 0), text, font=f, stroke_width=1)


def draw_text_center(draw, text, cx, cy, box_w, box_h, fill):
    f, sw, (l, t, r, b) = fit_text(draw, text, box_w, box_h)
    draw.text((cx - (l + r) / 2, cy - (t + b) / 2), text, font=f, fill=fill, stroke_width=sw, stroke_fill=fill)


def keycap(draw, x0, y0, x1, y1, text, radius):
    """Key: white body with a gray bottom lip (relief) and a black legend. Image in (lum, alpha)."""
    h = y1 - y0
    lip = max(2, int(h * 0.10))
    draw.rounded_rectangle((x0, y0, x1, y1), radius=radius, fill=(150, 255))
    draw.rounded_rectangle((x0, y0, x1, y1 - lip), radius=radius, fill=(255, 255))
    pad = max(2, int(min(x1 - x0, h) * 0.09))
    draw_text_center(draw, text, (x0 + x1) / 2, (y0 + y1 - lip) / 2, x1 - x0 - 2 * pad, h - lip - 2 * pad, (0, 255))


def mouse_icon(draw, x0, y0, x1, y1, which):
    """Mouse: white body with a black outline; the used button in black. which: left|right|middle|move."""
    w, h = x1 - x0, y1 - y0
    mw = min(w, h * 0.70)
    cx = (x0 + x1) / 2
    bx0, bx1 = cx - mw / 2, cx + mw / 2
    by0, by1 = y0 + h * 0.04, y1 - h * 0.04
    r = mw / 2
    lw = max(SS, int(mw * 0.08))
    draw.rounded_rectangle((bx0, by0, bx1, by1), radius=r, fill=(0, 255))
    draw.rounded_rectangle((bx0 + lw, by0 + lw, bx1 - lw, by1 - lw), radius=r - lw, fill=(255, 255))
    split = by0 + (by1 - by0) * 0.45
    draw.rectangle((bx0 + lw, split - lw / 2, bx1 - lw, split + lw / 2), fill=(0, 255))   # horizontal line
    draw.rectangle((cx - lw / 2, by0 + lw, cx + lw / 2, split), fill=(0, 255))           # center line
    if which == "left":
        draw.pieslice((bx0 + lw, by0 + lw, bx1 - lw, by0 + 2 * r - lw), 180, 270, fill=(0, 255))
        draw.rectangle((bx0 + lw, by0 + r, cx, split), fill=(0, 255))
    elif which == "right":
        draw.pieslice((bx0 + lw, by0 + lw, bx1 - lw, by0 + 2 * r - lw), 270, 360, fill=(0, 255))
        draw.rectangle((cx, by0 + r, bx1 - lw, split), fill=(0, 255))
    elif which == "middle":
        ww = mw * 0.22
        draw.rounded_rectangle((cx - ww / 2, by0 + h * 0.12, cx + ww / 2, split - h * 0.04), radius=ww / 2, fill=(0, 255))
    elif which == "move":
        # arrows on both sides of the body
        a = (x1 - x0 - mw) / 2 * 0.9
        my = (by0 + by1) / 2
        if a > 2 * SS:
            draw.polygon([(x0 + 1, my), (x0 + a, my - a * 0.8), (x0 + a, my + a * 0.8)], fill=(0, 255))
            draw.polygon([(x1 - 1, my), (x1 - a, my - a * 0.8), (x1 - a, my + a * 0.8)], fill=(0, 255))


def cluster(draw, x0, y0, x1, y1, labels):
    """Four keys in a cross (W / A S D)."""
    w, h = x1 - x0, y1 - y0
    k = min(w / 3.0, h / 2.0) * 0.96
    g = k * 0.04
    cx = (x0 + x1) / 2
    top = (y0 + y1) / 2 - k
    up, left, down, right = labels
    keycap(draw, cx - k / 2 + g, top + g, cx + k / 2 - g, top + k - g, up, k * 0.18)
    for i, t in enumerate((left, down, right)):
        kx = cx - 1.5 * k + i * k
        keycap(draw, kx + g, top + k + g, kx + k - g, top + 2 * k - g, t, k * 0.18)


def is_mask(tex: tc.Tex) -> bool:
    """Legend only (e.g. button_icon_B_01^s): white + alpha in R; the layout paints it over a disc."""
    return tex.fmt == tc.FMT_BC4 and bytes(tex.comp) == b"\x01\x01\x01\x02"


def render_mask(tex: tc.Tex, what) -> np.ndarray:
    """White legend on transparent, centered in the texture (the game draws the disc behind it)."""
    W, H = tex.w * SS, tex.h * SS
    m = 0.20
    x0, y0, x1, y1 = W * m, H * m, W * (1 - m), H * (1 - m)
    if what[0] == "key":
        img = Image.new("LA", (W, H), (255, 0))
        draw_text_center(ImageDraw.Draw(img), what[1], W / 2, H / 2, x1 - x0, y1 - y0, (255, 255))
    else:
        # Mouse or key cross: drawn normally, and the dark parts are kept as the legend.
        tmp = Image.new("LA", (W, H), (255, 0))
        d = ImageDraw.Draw(tmp)
        if what[0] == "mouse":
            mouse_icon(d, x0, y0, x1, y1, what[1])
        else:
            cluster(d, x0, y0, x1, y1, what[1])
        a = np.asarray(tmp)
        out = np.zeros_like(a)
        out[..., 0] = 255
        out[..., 1] = np.where(a[..., 0] < 128, a[..., 1], 0)
        img = Image.fromarray(out, "LA")
    return np.asarray(img.resize((tex.w, tex.h), Image.LANCZOS))


def render(tex: tc.Tex, orig_raw: np.ndarray, kind, what) -> np.ndarray | None:
    """(lum, alpha) image of the new icon, at texture size; None = keep the original."""
    if what is None:
        return None
    if is_mask(tex):
        return render_mask(tex, what)
    vis = tc.to_visible(tex, orig_raw)
    alpha = vis[..., 3]
    ys, xs = np.nonzero(alpha > 96)
    if not len(xs):
        return None
    bx0, by0, bx1, by1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
    W, H = tex.w * SS, tex.h * SS
    img = Image.new("LA", (W, H), (255, 0))
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = bx0 * SS, by0 * SS, bx1 * SS, by1 * SS
    # Round icons (A/B/X/Y, sticks): a key slightly wider than tall does not fit; the whole box is used.
    if what is None:
        return None
    if what[0] == "mouse":
        mouse_icon(d, x0, y0, x1, y1, what[1])
    elif what[0] == "cluster":
        cluster(d, x0, y0, x1, y1, what[1])
    else:
        keycap(d, x0, y0, x1, y1, what[1], radius=min(x1 - x0, y1 - y0) * 0.22)
    small = img.resize((tex.w, tex.h), Image.LANCZOS)
    return np.asarray(small)   # (h, w, 2): lum, alpha


def to_raw(tex: tc.Tex, la: np.ndarray, orig_raw: np.ndarray) -> np.ndarray:
    """(lum, alpha) -> raw format channels according to the texture's channel selector."""
    lum, a = la[..., 0].astype(np.int32), la[..., 1].astype(np.int32)
    raw = orig_raw.astype(np.int32).copy()
    comp = list(tex.comp)
    # comp = sources of the visible (R, G, B, A): 2 = raw channel R, 3 = G, 4 = B, 5 = A.
    want = {0: lum, 1: lum, 2: lum, 3: a}   # visible -> value
    for vis_i, src in enumerate(comp):
        if 2 <= src <= 5:
            raw[..., src - 2] = want[vis_i]
    if tex.fmt == tc.FMT_BC4 and comp[3] == 2 and comp[0] == 1:   # mask (white + alpha in R)
        raw[..., 0] = np.where(lum < 128, 0, a)                   # the legend as a hole
    return np.clip(raw, 0, 255).astype(np.uint8)


# ---------------------------------------------------------------- texture sources
def iter_romfs_arcs(romfs: str):
    """(path, bytes) of the .arc files in bin/appli, from the extracted folder or romfs.bin (same order)."""
    if os.path.isfile(romfs):
        from romfs_bin import RomfsBin
        img = RomfsBin(romfs)
        try:
            for p in img.walk("bin/appli", ".arc"):
                yield os.path.join(romfs, p), img.read(p)
        finally:
            img.close()
        return
    for p in glob.glob(os.path.join(romfs, "bin", "appli", "**", "*.arc"), recursive=True):
        yield p, open(p, "rb").read()


def iter_sources(romfs: str | None, dump: str | None):
    if romfs:
        for p, d in iter_romfs_arcs(romfs):
            if d[:4] != b"SARC":
                continue
            for n, s, e in tc.sarc_entries(d):
                if n.endswith(".bntx"):
                    yield p, d, s
    if dump:
        for p in glob.glob(os.path.join(dump, "*.bntx")):
            yield p, open(p, "rb").read(), 0


def collect(romfs, dump):
    seen = {}
    for src, d, base in iter_sources(romfs, dump):
        try:
            texs = tc.bntx_textures(d, base)
        except Exception:
            continue
        for t in texs:
            if t.fmt not in tc.BLOCK_BYTES or not classify(t.name):
                continue
            key = (t.name, t.fmt, t.w, t.h, t.size)
            if key not in seen:
                seen[key] = (t, d[base + t.data_off: base + t.data_off + t.size], src)
    return seen


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--romfs", required=True, help="romfs.bin image (exefs/romfs.bin of the game folder) or an extracted RomFS folder")
    ap.add_argument("--dump", default="", help="folder with BNTX files dumped by the nnshim (ui.dump_dir)")
    ap.add_argument("--controls", default="", help="controls file (controls.json; default: the built-in controls)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--preview", default="", help="folder for before/after PNG images")
    args = ap.parse_args(argv)

    b = Bindings(load_controls(args.controls))
    found = collect(args.romfs, args.dump or None)
    entries = []
    sheet = []
    for (name, fmt, w, h, size), (t, data, src) in sorted(found.items()):
        kind = classify(name)
        if kind[0] == "rstick_dir":
            ks = {"up": ["RStickUp"], "down": ["RStickDown"], "ud": ["RStickUp", "RStickDown"]}[kind[1]]
            what = ("key", " ".join(b.label(b.keys.get(k, ["?"])[0]) for k in ks))
        else:
            what = b.stick(kind[1]) if kind[0] == "stick" else (
                ("mouse", "move") if kind[0] == "stick_lr" and kind[1] == "R" and b.mouse_on else
                (("key", b.label(b.keys.get("LStickLeft", ["A"])[0]) + " " + b.label(b.keys.get("LStickRight", ["D"])[0]))
                 if kind[0] == "stick_lr" else b.what(kind[1])))
        raw = tc.decode(t, data)
        la = render(t, raw, kind, what)
        if la is None:
            continue
        new_raw = to_raw(t, la, raw)
        enc = tc.encode(t, new_raw)
        assert len(enc) == t.size
        entries.append((name, fmt, w, h, enc))
        if args.preview:
            before = tc.to_visible(t, raw)
            after = tc.to_visible(t, tc.decode(t, enc))
            sheet.append((name, before, after))
    with open(args.out, "wb") as f:
        f.write(b"PLATECL1" + struct.pack("<I", len(entries)))   # pack magic (format v1, read by ui::Pack)
        for name, fmt, w, h, data in entries:
            nb = name.encode("utf-8")
            f.write(struct.pack("<H", len(nb)) + nb + struct.pack("<IIII", fmt, w, h, len(data)) + data)
    print(f"{args.out}: {len(entries)} prompts from {len(found)} candidates")
    if args.preview and sheet:
        os.makedirs(args.preview, exist_ok=True)
        z = 3
        cw = max(x[1].shape[1] for x in sheet) * z * 2 + 30
        ch = max(x[1].shape[0] for x in sheet) * z + 26
        cols = 4
        rows = (len(sheet) + cols - 1) // cols
        img = Image.new("RGBA", (cols * cw, rows * ch), (52, 70, 60, 255))
        dr = ImageDraw.Draw(img)
        small = font(11)
        for i, (name, before, after) in enumerate(sheet):
            x, y = (i % cols) * cw, (i // cols) * ch
            for j, arr in enumerate((before, after)):
                im = Image.fromarray(arr.astype(np.uint8), "RGBA").resize((arr.shape[1] * z, arr.shape[0] * z), Image.NEAREST)
                img.alpha_composite(im, (x + 6 + j * (arr.shape[1] * z + 10), y + 2))
            dr.text((x + 4, y + ch - 20), name.split("^")[0].replace("button_icon_", "")[:34], font=small, fill=(255, 255, 255, 255))
        img.save(os.path.join(args.preview, "key_prompts_before_after.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
