#!/usr/bin/env python3
"""Prepares PLA.exe for the dist: the game icon (from YOUR dump) embedded in the exe.

1. Reads the icon from the game's control NCA (control RomFS, icon_AmericanEnglish.dat,
   JPEG 256x256) with the tools/extract tools and your keys (~/.switch).
2. Converts it to .ico (256, 128, 64, 48, 32, 16) with Pillow.
3. Replaces icon group 1 of PLA.exe (BeginUpdateResourceW / UpdateResourceW),
   the same way suyu's exporter does (game_export.cpp). The window uses that resource
   (EmuWindow_SDL2::SetWindowIcon with g_native_export_mode).

Usage:
  python recomp/scripts/make-pla-exe.py --exe <root>\\dist\\PLA-PC-v0\\PLA.exe
         [--ico output.ico] [--dump dump/] [--icon-from file.png|.jpg|.ico]

None of this is committed: the icon comes from the user's dump and stays only in their dist.
"""
from __future__ import annotations

import argparse
import ctypes
import io
import struct
import sys
from ctypes import wintypes
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "extract"))

PLA_TID_BASE = 0x01001F5010DFA000


def icon_from_dump(dump: Path) -> bytes:
    import extract as ex  # noqa: E402
    import nxfmt as nx  # noqa: E402

    nx.need_crypto()
    inv = ex.scan_dump(dump, dump / ex.WORKDIR_NAME)
    keys = ex.load_keys(None, None)
    for reg, _src in inv.tickets:
        try:
            t = nx.parse_ticket(reg.read(0, reg.size))
            keys.add_titlekey(t.rights_id, nx.ticket_titlekey(t, keys))
        except (nx.FormatError, nx.KeyError_):
            pass
    titles = ex.read_cnmts(ex.open_ncas(inv, keys))
    # The update carries its own control NCA; otherwise the base game's one is used.
    order = sorted(titles, key=lambda t: (t.cnmt.title_id & ~0xFFF) != PLA_TID_BASE or t.cnmt.meta_type != 0x81)
    for t in order:
        if (t.cnmt.title_id & ~0xFFF) != PLA_TID_BASE:
            continue
        ctrl = t.contents.get(3)
        if not ctrl:
            continue
        view = ctrl.nca.section_view(0)
        for name in ("icon_AmericanEnglish.dat", "icon_LatinAmericanSpanish.dat",
                     "icon_Spanish.dat", "icon_Japanese.dat"):
            f = nx.romfs_find(view, name)
            if f:
                return f.read(0, f.size)
    raise SystemExit("The icon was not found in the dump's control NCA.")


def make_ico(image_bytes: bytes) -> bytes:
    from PIL import Image

    img = Image.open(io.BytesIO(image_bytes)).convert("RGBA")
    out = io.BytesIO()
    img.save(out, format="ICO", sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
    return out.getvalue()


def parse_ico(ico: bytes):
    _res, typ, count = struct.unpack_from("<HHH", ico, 0)
    if typ != 1:
        raise SystemExit("The file is not an .ico")
    entries = []
    for i in range(count):
        w, h, colors, _r, planes, bpp, size, off = struct.unpack_from("<BBBBHHII", ico, 6 + i * 16)
        entries.append((w, h, colors, planes, bpp, ico[off:off + size]))
    return entries


RT_ICON = 3
RT_GROUP_ICON = 14

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.BeginUpdateResourceW.restype = wintypes.HANDLE
k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
k32.UpdateResourceW.restype = wintypes.BOOL
k32.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD,
                                ctypes.c_void_p, wintypes.DWORD]
k32.EndUpdateResourceW.restype = wintypes.BOOL
k32.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
k32.LoadLibraryExW.restype = wintypes.HMODULE
k32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
k32.FreeLibrary.argtypes = [wintypes.HMODULE]

ENUMLANG = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p,
                              wintypes.WORD, ctypes.c_void_p)
ENUMNAME = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p,
                              ctypes.c_void_p)
k32.EnumResourceNamesW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ENUMNAME, ctypes.c_void_p]
k32.EnumResourceLanguagesW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, ENUMLANG,
                                       ctypes.c_void_p]


def existing(exe: Path, rtype: int):
    """[(id, lang)] of the numeric resources of that type."""
    found = []
    h = k32.LoadLibraryExW(str(exe), None, 0x2 | 0x20)  # AS_DATAFILE | AS_IMAGE_RESOURCE
    if not h:
        return found

    @ENUMLANG
    def on_lang(_m, _t, name, lang, _p):
        found.append((name, lang))
        return True

    names = []

    @ENUMNAME
    def on_name(_m, _t, name, _p):
        if name is not None and name < 0x10000:
            names.append(name)
        return True

    k32.EnumResourceNamesW(h, rtype, on_name, None)
    for n in names:
        k32.EnumResourceLanguagesW(h, rtype, n, on_lang, None)
    k32.FreeLibrary(h)
    return found


def embed(exe: Path, ico: bytes) -> None:
    entries = parse_ico(ico)
    old_icons = existing(exe, RT_ICON)
    old_groups = existing(exe, RT_GROUP_ICON)
    lang = old_groups[0][1] if old_groups else 0
    h = k32.BeginUpdateResourceW(str(exe), False)
    if not h:
        raise SystemExit(f"BeginUpdateResourceW failed ({ctypes.get_last_error()})")
    ok = True
    for rid, rlang in old_icons:
        ok &= bool(k32.UpdateResourceW(h, RT_ICON, rid, rlang, None, 0))
    for rid, rlang in old_groups:
        if rid == 1:
            ok &= bool(k32.UpdateResourceW(h, RT_GROUP_ICON, rid, rlang, None, 0))
    grp = struct.pack("<HHH", 0, 1, len(entries))
    keep = []
    for i, (w, hgt, colors, planes, bpp, data) in enumerate(entries, start=1):
        buf = ctypes.create_string_buffer(data, len(data))
        keep.append(buf)
        ok &= bool(k32.UpdateResourceW(h, RT_ICON, i, lang, buf, len(data)))
        grp += struct.pack("<BBBBHHIH", w, hgt, colors, 0, planes, bpp, len(data), i)
    gbuf = ctypes.create_string_buffer(grp, len(grp))
    ok &= bool(k32.UpdateResourceW(h, RT_GROUP_ICON, 1, lang, gbuf, len(grp)))
    if not k32.EndUpdateResourceW(h, not ok) or not ok:
        raise SystemExit(f"Could not update the icon of {exe} ({ctypes.get_last_error()})")
    print(f"icon embedded in {exe}: {len(entries)} sizes (previously {len(old_icons)} images)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--ico", type=Path, help="also save the .ico (e.g. next to the exe)")
    ap.add_argument("--dump", type=Path, default=REPO / "dump")
    ap.add_argument("--icon-from", type=Path, help="use this image instead of reading the dump")
    a = ap.parse_args()
    if a.icon_from:
        raw = a.icon_from.read_bytes()
        ico = raw if a.icon_from.suffix.lower() == ".ico" else make_ico(raw)
    else:
        ico = make_ico(icon_from_dump(a.dump))
    if a.ico:
        a.ico.write_bytes(ico)
        print(f"icon saved to {a.ico}")
    embed(a.exe, ico)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
