# SPDX-License-Identifier: MIT
"""Walks the game's RomFS (loose files and GFLXPACK entries) and:

  * finds every BNSH container (loose or embedded, e.g. in VFXB/ptcl);
  * extracts each unique NVN stage code (control + data) to an output directory;
  * counts the texture formats of every BRTI (loose or embedded BNTX).

GFLXPACK entries use compression 3 (Oodle); they are decompressed with `pyooz`
(free reimplementation of Kraken/Mermaid/Selkie, `pip install pyooz`).

Usage:
  python romfs_scan.py --romfs D:\\PLARecomp\\romfs --out build\\port\\nvnshader\\extract
Output: <out>/shaders/<hash>.{ctrl,data}, <out>/manifest.json, <out>/textures.json,
        <out>/programs.txt (unique stage combinations per variation)
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import bnsh  # noqa: E402

try:
    import ooz
except ImportError:  # pragma: no cover
    ooz = None

SURF = {
    0x01: "R4G4", 0x02: "R8", 0x03: "R4G4B4A4", 0x04: "A4B4G4R4", 0x05: "R5G5B5A1",
    0x06: "A1B5G5R5", 0x07: "R5G6B5", 0x08: "B5G6R5", 0x09: "R8G8", 0x0A: "R16",
    0x0B: "R8G8B8A8", 0x0C: "B8G8R8A8", 0x0D: "R9G9B9E5", 0x0E: "R10G10B10A2",
    0x0F: "R11G11B10", 0x10: "B10G11R11", 0x11: "R10G11B11", 0x12: "R16G16", 0x13: "R24G8",
    0x14: "R32", 0x15: "R16G16B16A16", 0x16: "R32G8X24", 0x17: "R32G32", 0x18: "R32G32B32",
    0x19: "R32G32B32A32", 0x1A: "BC1", 0x1B: "BC2", 0x1C: "BC3", 0x1D: "BC4", 0x1E: "BC5",
    0x1F: "BC6H", 0x20: "BC7", 0x21: "EAC_R11", 0x22: "EAC_R11G11", 0x23: "ETC1",
    0x24: "ETC2", 0x25: "ETC2_MASK", 0x26: "ETC2_ALPHA", 0x27: "PVRTC1_2BPP",
    0x28: "PVRTC1_4BPP", 0x29: "PVRTC1_ALPHA_2BPP", 0x2A: "PVRTC1_ALPHA_4BPP",
    0x2B: "PVRTC2_ALPHA_2BPP", 0x2C: "PVRTC2_ALPHA_4BPP", 0x2D: "ASTC_4x4",
    0x2E: "ASTC_5x4", 0x2F: "ASTC_5x5", 0x30: "ASTC_6x5", 0x31: "ASTC_6x6",
    0x32: "ASTC_8x5", 0x33: "ASTC_8x6", 0x34: "ASTC_8x8", 0x35: "ASTC_10x5",
    0x36: "ASTC_10x6", 0x37: "ASTC_10x8", 0x38: "ASTC_10x10", 0x39: "ASTC_12x10",
    0x3A: "ASTC_12x12", 0x3B: "B5G5R5A1",
}
CHAN = {1: "UNORM", 2: "SNORM", 3: "UINT", 4: "SINT", 5: "FLOAT", 6: "SRGB", 7: "DEPTH",
        8: "USCALED", 9: "SSCALED", 0xA: "UFLOAT"}


def fmt_name(v: int) -> str:
    return f"{SURF.get(v >> 8, hex(v >> 8))}_{CHAN.get(v & 0xFF, hex(v & 0xFF))}"


def iter_gfpak(d: bytes):
    fc = struct.unpack_from("<I", d, 0x10)[0]
    fio = struct.unpack_from("<Q", d, 0x18)[0]
    for i in range(fc):
        _lvl, ct, dsz, csz, _pad, off = struct.unpack_from("<HHIIIQ", d, fio + i * 0x18)
        raw = d[off:off + csz]
        if ct == 0:
            yield i, raw
        elif ct == 3:
            if ooz is None:
                raise SystemExit("pyooz is required to decompress GFLXPACK (pip install pyooz)")
            yield i, ooz.decompress(raw, dsz)
        else:
            yield i, None  # other methods (zlib/lz4) do not occur in game version 1.1.1


def scan_textures(blob: bytes, counter, origin, tex_samples):
    pos = 0
    while True:
        i = blob.find(b"BRTI", pos)
        if i < 0:
            return
        pos = i + 4
        if i + 0x34 > len(blob):
            return
        fmt = struct.unpack_from("<I", blob, i + 0x1C)[0]
        dim = blob[i + 0x11]
        w, h = struct.unpack_from("<II", blob, i + 0x24)
        if (fmt >> 8) not in SURF or (fmt & 0xFF) not in CHAN or not (0 < w <= 16384 and 0 < h <= 16384):
            continue
        counter[fmt] += 1
        if fmt not in tex_samples:
            tex_samples[fmt] = f"{origin} ({w}x{h}, dim {dim})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", default=r"D:\PLARecomp\romfs")
    ap.add_argument("--out", default=r"build\port\nvnshader\extract")
    ap.add_argument("--no-gfpak", action="store_true")
    ap.add_argument("--extra", nargs="*", default=[],
                    help="extra files (e.g. data/main.elf: built-in ui2d/font shaders)")
    args = ap.parse_args()

    out_sh = os.path.join(args.out, "shaders")
    os.makedirs(out_sh, exist_ok=True)
    shaders = {}       # hash -> dict
    programs = {}      # (vs, fs, cs) -> number of variations using it
    containers = []    # list of BNSH containers
    tex = collections.Counter()
    tex_samples = {}
    tex_by_kind = collections.Counter()
    t0 = time.time()
    nfiles = nentries = 0

    def handle(blob: bytes, origin: str):
        before = sum(tex.values())
        scan_textures(blob, tex, origin, tex_samples)
        if sum(tex.values()) != before:
            tex_by_kind[origin.rsplit(".", 1)[-1].split(":")[0]] += sum(tex.values()) - before
        for off, sub in bnsh.find_all(blob):
            try:
                b = bnsh.parse(sub)
            except Exception as e:  # noqa: BLE001
                containers.append({"origin": origin, "offset": off, "error": str(e)})
                continue
            stages = collections.Counter()
            for v in b.variations:
                key = tuple(v.stages[st].code_hash() if st in v.stages else ""
                            for st in ("vertex", "fragment", "compute"))
                programs[key] = programs.get(key, 0) + 1
                for st, c in v.stages.items():
                    stages[st] += 1
                    h = c.code_hash()
                    rec = shaders.get(h)
                    if rec is None:
                        rec = shaders[h] = {"stage": st, "data_size": len(c.data),
                                            "control_size": len(c.control), "refs": []}
                        with open(os.path.join(out_sh, h + ".data"), "wb") as f:
                            f.write(c.data)
                        with open(os.path.join(out_sh, h + ".ctrl"), "wb") as f:
                            f.write(c.control)
                    if len(rec["refs"]) < 8:
                        rec["refs"].append(f"{origin}@{off:#x}:{b.name}#{v.index}")
                    rec["nrefs"] = rec.get("nrefs", 0) + 1
            containers.append({"origin": origin, "offset": off, "name": b.name,
                               "variations": len(b.variations), "stages": dict(stages)})

    for root, _dirs, files in os.walk(args.romfs):
        for fn in files:
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, args.romfs).replace("\\", "/")
            with open(p, "rb") as f:
                d = f.read()
            nfiles += 1
            if d[:8] == b"GFLXPACK":
                if args.no_gfpak:
                    continue
                for idx, blob in iter_gfpak(d):
                    nentries += 1
                    if blob is not None:
                        handle(blob, f"{rel}:{idx}")
            else:
                handle(d, rel)
            if nfiles % 2000 == 0:
                print(f"  {nfiles} files, {nentries} entries, {len(shaders)} shaders, "
                      f"{time.time() - t0:.0f} s", flush=True)

    for p in args.extra:
        with open(p, "rb") as f:
            handle(f.read(), "exefs/" + os.path.basename(p))
        nfiles += 1

    st = collections.Counter(r["stage"] for r in shaders.values())
    by_origin = collections.Counter()
    for c in containers:
        if "name" in c:
            by_origin[c["origin"].split("/")[0] + "/" + c["origin"].rsplit(".", 1)[-1].split(":")[0]] += 1
    manifest = {
        "romfs": args.romfs, "files": nfiles, "gfpak_entries": nentries,
        "containers": len(containers),
        "variations": sum(c.get("variations", 0) for c in containers),
        "stage_refs": dict(collections.Counter(
            {k: v for c in containers for k, v in c.get("stages", {}).items()})),
        "unique_codes": len(shaders), "unique_by_stage": dict(st),
        "container_kinds": dict(by_origin),
        "container_list": containers, "shaders": shaders,
    }
    # stage_refs summed correctly
    sr = collections.Counter()
    for c in containers:
        sr.update(c.get("stages", {}))
    manifest["stage_refs"] = dict(sr)
    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    with open(os.path.join(args.out, "programs.txt"), "w", encoding="utf-8") as f:
        f.write("# vs fs cs variations (- = stage absent)\n")
        for (vs, fs, cs), n in sorted(programs.items()):
            f.write(f"{vs or '-'} {fs or '-'} {cs or '-'} {n}\n")
    textures = {"total": sum(tex.values()),
                "by_format": {fmt_name(k): v for k, v in tex.most_common()},
                "by_container_ext": dict(tex_by_kind),
                "examples": {fmt_name(k): v for k, v in tex_samples.items()}}
    astc = sum(v for k, v in tex.items() if 0x2D <= (k >> 8) <= 0x3A)
    textures["astc"] = astc
    with open(os.path.join(args.out, "textures.json"), "w", encoding="utf-8") as f:
        json.dump(textures, f, indent=1)
    print(f"files {nfiles}, gfpak entries {nentries}, BNSH containers {len(containers)}, "
          f"variations {manifest['variations']}, unique codes {len(shaders)} {dict(st)}, "
          f"unique programs {len(programs)}")
    print(f"BRTI textures {textures['total']}, ASTC {astc}")
    for k, v in tex.most_common():
        print(f"  {fmt_name(k):24s} {v}")
    print(f"{time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
