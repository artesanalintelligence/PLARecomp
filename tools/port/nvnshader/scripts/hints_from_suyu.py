# SPDX-License-Identifier: MIT
"""Generates texture hints (type and whether the format is integer) for nvnshader_tool from
suyu's pipeline cache (`vulkan.bin`): what the game had bound when suyu translated each
shader. Output: lines "<hash> <cbuf> <offset> <type> <integer>" (--hints format).

  python hints_from_suyu.py <vulkan.bin> <extract_dir> <output.txt>
"""
from __future__ import annotations

import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import suyu_cache  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SHADER_INFO = os.path.join(HERE, "..", "third_party", "shader_recompiler", "src", "shader_recompiler", "shader_info.h")


def integer_formats() -> set[int]:
    """Shader::TexturePixelFormat indices whose name is integer (_UINT/_SINT, excluding depth)."""
    src = open(SHADER_INFO, encoding="utf-8").read()
    body = src[src.index("enum class TexturePixelFormat"):]
    body = body[body.index("{") + 1:body.index("};")]
    names = [n.strip() for n in body.replace("\n", "").split(",") if n.strip()]
    return {i for i, n in enumerate(names)
            if re.search(r"_(UINT|SINT)$", n) and not n.startswith(("D", "S8_UINT_D"))}


def main():
    cache_path, extract, out_path = sys.argv[1:4]
    ints = integer_formats()
    idx = collections.defaultdict(list)
    for p in glob.glob(os.path.join(extract, "shaders", "*.data")):
        d = open(p, "rb").read()
        off = 0x30 if d[:4] == bytes.fromhex("78563412") else 0
        idx[d[off:off + 0x60]].append((os.path.basename(p)[:-5], d[off:]))
    _ver, _g, recs = suyu_cache.load(cache_path)
    hints = collections.defaultdict(dict)
    stats = collections.Counter()
    for r in recs:
        for e in r.envs:
            match = None
            for h, body in idx.get(e.code[:0x60], []):
                n = min(len(body), len(e.code)) - 8
                if body[:n] == e.code[:n]:
                    match = h
                    break
            if match is None:
                stats["unmatched"] += 1
                continue
            # handle -> (cbuf, offset) from the cbuf reads (banks != 1)
            by_value = collections.defaultdict(list)
            for (b, o), v in e.cbuf_values.items():
                if b != 1:
                    by_value[v].append((b, o))
            keys = set(e.texture_types) | set(e.texture_formats)
            for handle in keys:
                slots = by_value.get(handle) or by_value.get(handle & 0xFFFFF) or []
                if len(slots) != 1:
                    stats["ambiguous handle" if slots else "handle without cbuf"] += 1
                    continue
                b, o = slots[0]
                ttype = e.texture_types.get(handle, 2)
                is_int = 1 if e.texture_formats.get(handle) in ints else 0
                prev = hints[match].get((b, o))
                if prev and prev != (ttype, is_int):
                    stats["conflict between pipelines"] += 1
                hints[match][(b, o)] = (ttype, is_int)
                stats["hints"] += 1
    nondefault = 0
    with open(out_path, "w", encoding="utf-8") as f:
        for h, m in sorted(hints.items()):
            for (b, o), (t, i) in sorted(m.items()):
                f.write(f"{h} {b} {o} {t} {i}\n")
                if t != 2 or i:
                    nondefault += 1
    print(f"{len(hints)} shaders with hints, {nondefault} hints other than (Color2D, non-integer); {dict(stats)}")


if __name__ == "__main__":
    main()
