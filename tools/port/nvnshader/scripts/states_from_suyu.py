# SPDX-License-Identifier: MIT
"""Turns each pipeline in suyu's cache (`vulkan.bin`) into state lines for
`nvnshader_tool translate --states`: the real state the game drew with (attribute types,
topology, XFB, dual source, alpha test, texture hints...). Used to translate and validate the
game's shaders with the true state rather than the default one.

  python states_from_suyu.py <vulkan.bin> <extract_dir> <output.txt> [--xfb-explicit]

--xfb-explicit also writes the XFB layout as suyu computes it (xfbx=...), to compare
against the one nvnshader reads from the control section.
"""
from __future__ import annotations

import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import suyu_cache  # noqa: E402
from hints_from_suyu import integer_formats  # noqa: E402

# Offsets within FixedPipelineState (1056-byte graphics key, version 18):
# raw1 +0, raw2 +4, alpha_test_ref +24, point_size +28, attributes[32] +112,
# xfb_state.layouts[4] +432, xfb_state.varyings[4][128] +480.
ATTR_LETTER = {1: "F", 2: "F", 7: "F", 3: "S", 4: "U", 5: "u", 6: "s"}  # Maxwell VertexAttribute::Type
TOPO = {0: "points", 1: "lines", 2: "lines", 3: "lines"}
# Packed Maxwell ComparisonOp (FixedPipelineState::PackComparisonOp) -> Shader::CompareFunction
# PackComparisonOp: Never 0, Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7
VECTORS = {28} | set(range(32, 160, 4)) | {160, 164} | set(range(192, 224, 4))


def xfb_varyings(fps: bytes):
    """Replica of VideoCommon::MakeTransformFeedbackVaryings."""
    out = {}
    for b in range(4):
        stream, count, stride = struct.unpack_from("<III", fps, 432 + 12 * b)
        locs = fps[480 + 128 * b:480 + 128 * (b + 1)]
        off = 0
        while off < count:
            attr = locs[off]
            base_off = off
            comps = 1
            if (attr & ~3) in VECTORS:
                while off + 1 < count and locs[off + 1] // 4 == attr // 4:
                    off += 1
                    comps += 1
            out[attr] = (b, stride, base_off * 4, comps)
            off += 1
    return out


def main():
    cache_path, extract, out_path = sys.argv[1:4]
    explicit = "--xfb-explicit" in sys.argv
    ints = integer_formats()
    idx = collections.defaultdict(list)
    for p in glob.glob(os.path.join(extract, "shaders", "*.data")):
        d = open(p, "rb").read()
        off = 0x30 if d[:4] == bytes.fromhex("78563412") else 0
        idx[d[off:off + 0x60]].append((os.path.basename(p)[:-5], d[off:]))
    _v, _g, recs = suyu_cache.load(cache_path)
    lines = []
    seen = set()
    for n, r in enumerate(recs):
        fps = r.key[48:]
        for e in r.envs:
            h = None
            for hh, body in idx.get(e.code[:0x60], []):
                m = min(len(body), len(e.code)) - 8
                if body[:m] == e.code[:m]:
                    h = hh
                    break
            if h is None:
                continue
            kv = []
            # texture hints (as in hints_from_suyu.py)
            by_value = collections.defaultdict(list)
            for (b, o), v in e.cbuf_values.items():
                if b != 1:
                    by_value[v].append((b, o))
            tex = []
            for handle in set(e.texture_types) | set(e.texture_formats):
                slots = by_value.get(handle) or []
                if len(slots) == 1:
                    b, o = slots[0]
                    t = e.texture_types.get(handle, 2)
                    i = 1 if e.texture_formats.get(handle) in ints else 0
                    tex.append(f"{b}:{o}:{t}:{i}")
            if tex:
                kv.append("tex=" + ",".join(sorted(tex)))
            if e.stage == 5:
                line = f"{h} {' '.join(kv)} tag=p{n}"
            else:
                raw1, raw2 = struct.unpack_from("<II", fps, 0)
                topo = TOPO.get((raw1 >> 24) & 0xF, "triangles")
                kv.append(f"topo={topo}")
                if topo == "points":
                    kv.append(f"psize={struct.unpack_from('<f', fps, 28)[0]:g}")
                kv.append(f"ndc={(raw1 >> 7) & 1}")
                kv.append(f"ynegate={(raw2 >> 11) & 1}")
                kv.append(f"vt={e.viewport_transform_state}")
                if e.stage == 0:
                    letters = []
                    for i in range(32):
                        a = struct.unpack_from("<I", fps, 112 + 4 * i)[0]
                        letters.append(ATTR_LETTER.get((a >> 20) & 7, "F") if a & 1 else "-")
                    kv.append("attribs=" + "".join(letters))
                    if (raw1 >> 6) & 1:
                        kv.append("xfb=1")
                        if explicit:
                            xv = xfb_varyings(fps)
                            kv.append("xfbx=" + ",".join(f"{a}:{b}:{s}:{o}:{c}" for a, (b, s, o, c) in sorted(xv.items())))
                if e.stage == 4:
                    kv.append(f"dual={(raw1 >> 21) & 1}")
                    kv.append(f"earlyz={(raw2 >> 4) & 1}")
                    alpha = (raw2 >> 1) & 7
                    kv.append(f"alpha={alpha}")
                    if alpha != 7:
                        kv.append(f"aref={struct.unpack_from('<f', fps, 24)[0]:g}")
                line = f"{h} {' '.join(kv)} tag=p{n}"
            key = line.split(" tag=")[0]
            if key in seen:
                continue
            seen.add(key)
            lines.append(line)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# generated by states_from_suyu.py\n")
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} distinct states from {sum(len(r.envs) for r in recs)} stages in {len(recs)} pipelines")


if __name__ == "__main__":
    main()
