#!/usr/bin/env python3
"""Generates the nn shim's mod.json (format of recomp/MODDING.md).

Cross-references src/shim_table.inc (which adapters are NATIVE and where they hook) with
generated/plt_imports.inc (PLT offsets in main and of the definition in sdk, written by
tools/port/inventory.py) and generated/sdk_symbols.inc (internal SDK functions,
gen_sdk_symbols.py).

Usage:
  python tools/port/nnshim/gen_mod_json.py [--out build/port/nnshim/mod.json]
      [--romfs-dir D:/PLA/romfs | --romfs-bin auto|D:/PLA/exefs/romfs.bin] [--save-root D:/PLA/save] [--input auto|xinput|sdl2|keyboard|none]
      [--only hid,fs,time,...]          hooked families (default: all NATIVE ones)
      [--families fs=observe,time=off]  mode of each family in "config" (native/observe/off)
      [--fs-validate]                   adds the fs "post" hooks to validate reads against the HLE
      [--probe-file F]                  adds "nnshim_probe" (caller probe) at each line
                                        "<module> <offset> [comment]" of F (unless a hook is already there)
      [--fps-fixes]                     adds the game fixes for unlocked FPS (src/abi_fps.cpp)
      [--ui]                            key and mouse prompts in menus (src/abi_ui.cpp)
      [--validate]                      --fs-validate + the time/settings/oe/account "post" hooks (service A/B)
      [--set key=value ...]             extra "config" values (dotted key: hid.remote_port=7001)
      [--trace PATH] [--stats PATH]

Families (column 1 of plt_imports.inc): hid, fs, time, settings, applet (oe), account, err,
swkbd, font (pl).
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SHIM_RE = re.compile(r'^SHIM\((\w+),\s*(\w+),\s*"([^"]+)",\s*(\w+)\)', re.M)
IMP_RE = re.compile(r'^NN_IMPORT\((\w+), (0x[0-9a-f]+), (\w+), (0x[0-9a-f]+), (\d+), "([^"]+)"\)', re.M)
SDK_RE = re.compile(r'^NN_SDKSYM\((\w+), (\w+), (0x[0-9a-f]+), "([^"]+)"\)', re.M)
ALIASES = {"oe": "applet", "pl": "font"}
# "post" hooks for the fs A/B validation (observe mode).
FS_POSTS = {"nnshim_fs_MountRom": "nnshim_fs_MountRom_post", "nnshim_fs_MountRomCache": "nnshim_fs_MountRom_post",
            "nnshim_fs_OpenFile": "nnshim_fs_OpenFile_post", "nnshim_fs_ReadFile": "nnshim_fs_ReadFile_post",
            "nnshim_fs_ReadFileOut": "nnshim_fs_ReadFileOut_post"}
# Game fixes for unlocked FPS (src/abi_fps.cpp): hooks in main's code.
FPS_FIXES = [
    ("main", 0x1837d0, "nnshim_fps_VfxCalcRate",
     "vfx: frameRate = speed * (int)(dt/16.67 ms) -> not truncated (frozen effects > 60 FPS)"),
    ("main", 0xc9a9b4, "nnshim_fps_PadRepeatScale",
     "input: repeat delay/interval in 30 FPS frames -> scaled by dt (menus at > 30 FPS)"),
    ("main", 0x9dbeac, "nnshim_fps_SeqStep",
     "battle sequences (.wbin): frame += speed per call (30 FPS) -> speed scaled by real time"),
    ("main", 0x2adb28c, "nnshim_fps_ThrowDrag",
     "thrown balls: per-frame air drag and lift (vf17@0x1a7191c) -> scaled by dt (slow fall > 30 FPS)"),
    ("main", 0x1059a78, "nnshim_fps_AiFrames",
     "AI behavior tree: frames since the last evaluation (waits, turns, gauges) -> 30 FPS frames of real time"),
    ("main", 0x1212d60, "nnshim_fps_MapCursor",
     "map cursor: pos += stick * speed per frame (vf17@0x120f1f4) -> scaled by dt"),
    ("main", 0x124a394, "nnshim_fps_MapCursor",
     "map cursor (map top): pos += stick * speed per frame (vf17@0x1245db8) -> scaled by dt"),
    ("main", 0x2549fa0, "nnshim_fps_ClimbStop",
     "climbing mount: stop easing frame counter +0xd04 += 1 per frame -> 30 FPS frames of real time; animation rate from the per-frame displacement -> per 30 FPS frame"),
    ("main", 0x199b800, "nnshim_fps_ClimbMove",
     "climbing mount: per-frame displacement dir * speed / 30 (FUN_71025540e8) -> scaled by dt"),
] + [("main", off, "nnshim_fps_ScriptFrames",
      f"scripts: AppConfig.{name} (Lua) of a '*_frame' key (30 FPS frames) -> scaled by FPS / 30")
     for off, name in ((0xd9e8dc, "GetIntValue"), (0xd9edcc, "GetIntValue (method)"),
                       (0xd9f2d0, "GetFloatValue"), (0xd9f78c, "GetFloatValue (method)"))]
FPS_POSTS = {"nnshim_fps_SeqStep": "nnshim_fps_SeqStep_post", "nnshim_fps_AiFrames": "nnshim_fps_AiFrames_post",
             "nnshim_fps_ClimbStop": "nnshim_fps_ClimbStop_post", "nnshim_fps_ScriptFrames": "nnshim_fps_ScriptFrames_post"}
# Keyboard and mouse UI (src/abi_ui.cpp): key prompts in the GFLXPACKs and menu state.
UI_HOOKS = [
    ("main", 0x2975d5c, "nnshim_ui_GfpakLoad",
     "GFPack::LoadFile(pack, idx, dst): post = replace button prompts in the decompressed BNTX"),
    ("main", 0xc392d4, "nnshim_ui_MenuTick", "once per frame with a pause menu open (menu state)"),
    ("main", 0x2505ff8, "nnshim_ui_FieldTick", "once per frame with the player in control (menu state)"),
    ("main", 0x1363b58, "nnshim_ui_FrameTick", "once per frame, always (menu state)"),
    ("main", 0x1353e0, "nnshim_ui_LayoutDraw",
     "nn::ui2d::Layout::Draw: visible panes and their on-screen rectangles (mouse pointer)"),
    ("main", 0x2708dd8, "nnshim_ui_WidgetTick",
     "UI kit button update (vf16): registry of live buttons and their highlight (mouse pointer)"),
]
UI_POSTS = {"nnshim_ui_GfpakLoad": "nnshim_ui_GfpakLoad_post"}
# "post" hooks for the service A/B validation (observe mode): <adapter>_post compares the
# HLE's answer with the shim's (src/validate.hpp; val.* counters).
SVC_POSTS = {fn: fn + "_post" for fn in (
    "nnshim_time_UserGetCurrentTime", "nnshim_time_NetworkGetCurrentTime", "nnshim_time_ToCalendarTime",
    "nnshim_time_ToCalendarTimeInUtc", "nnshim_time_ToPosixTime", "nnshim_time_GetDeviceLocationName",
    "nnshim_time_CreateWithStandardSystemClock",
    "nnshim_settings_LanguageCodeMake", "nnshim_settings_LanguageEqCode", "nnshim_settings_CodeEqLanguage",
    "nnshim_oe_GetDesiredLanguage", "nnshim_oe_GetOperationMode", "nnshim_oe_GetPerformanceMode",
    "nnshim_oe_GetCurrentFocusState",
    "nnshim_account_TryOpenPreselectedUser", "nnshim_account_GetUserId", "nnshim_account_GetNickname",
    "nnshim_account_GetUserExistence", "nnshim_account_IsNetworkServiceAccountAvailable",
    "nnshim_aoc_CountAddOnContent", "nnshim_aoc_ListAddOnContent", "nnshim_nifm_IsNetworkAvailable",
    "nnshim_friends_GetFriendList", "nnshim_prepo_PlayReportSave",
    "nnshim_hid_GetNpadStyleSet", "nnshim_hid_GetNpadState_FullKey", "nnshim_hid_GetNpadState_Handheld")}


def parse_value(v: str):
    if v in ("true", "false"):
        return v == "true"
    try:
        return int(v, 0)
    except ValueError:
        pass
    try:
        return float(v)
    except ValueError:
        return v


def set_path(d: dict, key: str, value) -> None:
    parts = key.split(".")
    for p in parts[:-1]:
        d = d.setdefault(p, {})
    d[parts[-1]] = value


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "port" / "nnshim" / "mod.json")
    ap.add_argument("--romfs-dir", default="")
    ap.add_argument("--romfs-bin", default="", help="romfs.bin image ('auto' = <PLA.exe folder>/exefs/romfs.bin)")
    ap.add_argument("--save-root", default="")
    ap.add_argument("--input", default="auto")
    ap.add_argument("--only", default="", help="families to include (default: all NATIVE ones)")
    ap.add_argument("--families", default="", help="modes: fs=observe,time=off,...")
    ap.add_argument("--fs-validate", action="store_true")
    ap.add_argument("--validate", action="store_true", help="--fs-validate + service A/B")
    ap.add_argument("--probe-file", type=Path, default=None)
    ap.add_argument("--fps-fixes", action="store_true")
    ap.add_argument("--ui", action="store_true", help="key and mouse prompts in menus (src/abi_ui.cpp)")
    ap.add_argument("--trace", default="")
    ap.add_argument("--stats", default="")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    args = ap.parse_args(argv)

    imports: dict[str, list[tuple[str, int, str, int]]] = {}
    for fam, plt, mod, off, _cs, mangled in IMP_RE.findall((HERE / "generated" / "plt_imports.inc").read_text()):
        imports.setdefault(mangled, []).append((fam, int(plt, 16), mod, int(off, 16)))
    sdk_syms = {}
    sdk_inc = HERE / "generated" / "sdk_symbols.inc"
    if sdk_inc.exists():
        for fam, mod, off, mangled in SDK_RE.findall(sdk_inc.read_text(encoding="utf-8")):
            sdk_syms[mangled] = (fam, mod, int(off, 16))

    only = {ALIASES.get(x, x) for x in args.only.split(",") if x}
    hooks, missing, seen = [], [], set()
    counts: dict[str, int] = {}
    # shim_table.inc and the tables of other families (audio_table.inc).
    tables = [HERE / "src" / "shim_table.inc"] + sorted(p for p in (HERE / "src").glob("*_table.inc")
                                                         if p.name != "shim_table.inc")
    rows_all = [r for t in tables for r in SHIM_RE.findall(t.read_text(encoding="utf-8"))]
    for site, status, mangled, fn in rows_all:
        if status != "NATIVE":
            continue
        if site == "SDKX":
            if mangled not in sdk_syms:
                missing.append(mangled)
                continue
            fam, mod, off = sdk_syms[mangled]
            targets = [(mod, off, " [sdk internal]")]
        else:
            rows = imports.get(mangled)
            if not rows:
                missing.append(mangled)
                continue
            fam = rows[0][0]
            if site == "SDK" and rows[0][3]:
                targets = [(rows[0][2], rows[0][3], "")]
            else:
                targets = [("main", plt, " [PLT]") for _f, plt, _m, _o in rows if plt]
        if only and fam not in only:
            continue
        for mod, off, tag in targets:
            if (mod, off) in seen:   # two symbols with the same definition (D1/D2)
                continue
            seen.add((mod, off))
            h = {"module": mod, "offset": f"0x{off:x}", "pre": fn, "comment": mangled + tag}
            if (args.fs_validate or args.validate) and fn in FS_POSTS:
                h["post"] = FS_POSTS[fn]
            if args.validate and fn in SVC_POSTS:
                h["post"] = SVC_POSTS[fn]
            hooks.append(h)
            counts[fam] = counts.get(fam, 0) + 1
    if args.fps_fixes:
        for mod, off, fn, why in FPS_FIXES:
            if (mod, off) not in seen:
                seen.add((mod, off))
                h = {"module": mod, "offset": f"0x{off:x}", "pre": fn, "comment": why}
                if fn in FPS_POSTS:
                    h["post"] = FPS_POSTS[fn]
                hooks.append(h)
                counts["fps"] = counts.get("fps", 0) + 1
    if args.ui:
        for mod, off, fn, why in UI_HOOKS:
            if (mod, off) not in seen:
                seen.add((mod, off))
                h = {"module": mod, "offset": f"0x{off:x}", "pre": fn, "comment": why}
                if fn in UI_POSTS:
                    h["post"] = UI_POSTS[fn]
                hooks.append(h)
                counts["ui"] = counts.get("ui", 0) + 1
    if args.probe_file:
        nprobe = 0
        for line in args.probe_file.read_text(encoding="utf-8").splitlines():
            parts = line.split(None, 2)
            if len(parts) < 2 or line.lstrip().startswith("#"):
                continue
            mod, off = parts[0], int(parts[1], 16)
            if (mod, off) in seen:
                continue
            seen.add((mod, off))
            hooks.append({"module": mod, "offset": f"0x{off:x}", "pre": "nnshim_probe",
                          "comment": "probe: " + (parts[2] if len(parts) > 2 else "")})
            nprobe += 1
        counts["probe"] = nprobe
    if missing:
        raise SystemExit(f"symbols without an entry in plt_imports.inc/sdk_symbols.inc: {missing}")

    config: dict = {"input_backend": args.input, "positional_face_buttons": True,
                    "romfs_dir": args.romfs_dir, "save_root": args.save_root}
    if args.romfs_bin:
        config["romfs_bin"] = args.romfs_bin
    if args.families:
        fams = {}
        for item in args.families.split(","):
            k, _, v = item.partition("=")
            fams[ALIASES.get(k, k)] = parse_value(v) if v in ("true", "false") else v
        config["families"] = fams
    if args.trace:
        config["trace_path"] = args.trace
    if args.stats:
        config["stats_path"] = args.stats
    for kv in args.set:
        k, _, v = kv.partition("=")
        set_path(config, k, parse_value(v))

    mod = {
        "id": "nnshim",
        "name": "native nn shim (platform v1)",
        "version": "0.2.0",
        "title_id": "01001F5010DFA000",
        "build_id": "AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E",
        "library": "nnshim.dll",
        "enabled": True,
        "hooks": hooks,
        "config": config,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(mod, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{args.out}: {len(hooks)} hooks ({', '.join(f'{k} {v}' for k, v in sorted(counts.items()))})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
