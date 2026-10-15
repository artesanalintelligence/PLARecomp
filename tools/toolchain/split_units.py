#!/usr/bin/env python3
"""Splits the big generated units of a recompiler export into smaller ones, so the build can run many
more compilers at once (a clang-cl on an 80 MB unit needs ~5.4 GB of RAM; on an 8 MB one, well under
1 GB). See docs/port/TOOLCHAIN.md, "Split units".

A generated unit (src/recompiled_<module>_<n>.c) is, byte for byte:

    header      comment, #includes, `struct _recomp_ent{...};`, in unit 0 `int g_recomp_guard_host_v2=N;`,
                `static uint32_t recomp_gg_seen[COUNT];`
    blocks      COUNT x `void blk_<module>_<pc>(GuestContext* c){ ... }`; block i guards itself with
                `RECOMP_GG_GUARD(_expected,iU);` (its slot in recomp_gg_seen). Blocks never reference
                each other (control flow goes through c->pc and the dispatcher).
    seg table   `const struct _recomp_ent _seg_<module>_<n>[] = {...};` (pc range -> block, sorted) and
                `const unsigned _segn_<module>_<n> = COUNTU;`, both referenced from recompiled_<module>.c

A part keeps the header (the global definitions only in part 0), a contiguous run of blocks with their
guard slots renumbered from 0 and its own `recomp_gg_seen` sized to them; the last part also gets the seg
table, preceded by prototypes of the blocks it points to. Each block compiles to the same code as before
(the slot is a different static array, still at a link-time constant address), the exported tables are
unchanged and the object order (part 0, 1, ...) keeps the blocks in the same order in the DLL.

Anything that does not match this layout exactly is left whole (split_unit returns None).

    python split_units.py <unit.c> <out_dir> [--mb 8]      (manual test)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

FN_START = b"\nvoid blk_"
SEG_START = b"\nconst struct _recomp_ent _seg_"
RE_FN = re.compile(rb"^void (blk_\w+)\(GuestContext\* c\)\{$", re.M)
RE_GUARD = re.compile(rb"RECOMP_GG_GUARD\(_expected,(\d+)U\);")
RE_SEEN = re.compile(rb"^static uint32_t recomp_gg_seen\[(\d+)\];$", re.M)
RE_TAIL = re.compile(rb"\Aconst struct _recomp_ent (_seg_\w+)\[\] = \{\n.*\};\nconst unsigned (_segn_\w+) = (\d+)U;\n\Z", re.S)
# Header lines a part may repeat as-is; global definitions (`int g_x=N;`) only go to part 0.
RE_HDR_SHARED = re.compile(rb"^(|/\*.*\*/|#include [<\"][\w./]+[>\"]|struct _recomp_ent\{[^}]*\};|static uint32_t recomp_gg_seen\[\d+\];)$")
RE_HDR_GLOBAL = re.compile(rb"^int \w+=\d+;$")
VERSION = "2"  # part layout version (goes into the object signatures)

# Header items (file-scope declarations, see header_repeatable) that every part may repeat.
RE_ITEM_PROTO = re.compile(rb"^[A-Za-z_][\w \*]*\b\w+\s*\([^;{}=]*\)\s*;$")
RE_ITEM_STRUCT = re.compile(rb"^struct \w+\s*\{[^{}]*\}\s*;$")
RE_ITEM_SEEN = re.compile(rb"^static uint32_t recomp_gg_seen\[\d+\];$")
RE_PP_DEFINE = re.compile(rb"^#\s*define\s+(\w+)\(([^)]*)\)\s+static\b")


def header_repeatable(header: bytes) -> bool:
    """True if every file-scope item of the unit header can be repeated in each part: preprocessor lines,
    `static` functions (inline or not: a part gets its own private copy, as the blocks call them only within
    their unit), invocations of macros that define static functions, `extern` declarations, prototypes, struct
    definitions, the per-unit `recomp_gg_seen` and `int g_x=N;` globals (kept in part 0 only). Anything else
    (e.g. a static variable other than recomp_gg_seen, which would be duplicated per part) -> False.

    Exports with block chaining / FPXV / FPXOOL (the PERF recipe) carry ~33 KB of such helpers; the plain
    export had only #includes, the struct and the two arrays."""
    # Preprocessor lines (with continuations) out; remember the macros that expand to static functions.
    static_macros: set[bytes] = set()
    code: list[bytes] = []
    lines = header.split(b"\n")
    k = 0
    while k < len(lines):
        ln = lines[k]
        if ln.lstrip().startswith(b"#"):
            full = ln
            while full.endswith(b"\\") and k + 1 < len(lines):
                k += 1
                full = full[:-1] + lines[k]
            m = RE_PP_DEFINE.match(full)
            if m:
                static_macros.add(m.group(1))
        else:
            code.append(ln)
        k += 1
    text = re.sub(rb"/\*.*?\*/", b" ", b"\n".join(code), flags=re.S)
    if b'"' in text or b"'" in text or b"//" in text:
        return False  # keep the scanner simple: the headers seen have no literals or line comments
    if static_macros:
        text = re.sub(rb"\b(%s)\s*\([^()]*\)" % b"|".join(sorted(static_macros)), b" ", text)
    # Split into file-scope items: a definition ends at the '}' that closes it (or at the ';' after it, as in
    # `struct x{...};`), a declaration at ';' at depth 0.
    items, start, depth, i, n = [], 0, 0, 0, len(text)
    while i < n:
        ch = text[i:i + 1]
        if ch == b"{":
            depth += 1
        elif ch == b"}":
            depth -= 1
            if depth < 0:
                return False
            if depth == 0:
                j = i + 1
                while j < n and text[j:j + 1] in b" \t\r\n":
                    j += 1
                if j < n and text[j:j + 1] == b";":
                    i = j
                items.append(text[start:i + 1])
                start = i + 1
        elif ch == b";" and depth == 0:
            items.append(text[start:i + 1])
            start = i + 1
        i += 1
    if depth != 0 or text[start:].strip():
        return False
    globals_ = set()
    for it in items:
        s = b" ".join(it.split())
        if not s:
            continue
        if s.endswith(b"}"):
            if not s.startswith(b"static "):
                return False  # a function with external linkage would be defined once per part
        elif RE_HDR_GLOBAL.match(s):
            globals_.add(s)
        elif s.startswith(b"extern ") or RE_ITEM_STRUCT.match(s) or RE_ITEM_SEEN.match(s):
            continue
        elif RE_ITEM_PROTO.match(s) and not s.startswith(b"static "):
            continue
        else:
            return False
    # split_unit drops the RE_HDR_GLOBAL lines from parts 1..n: they must all be these file-scope globals,
    # never a statement inside a helper's body.
    return {l for l in lines if RE_HDR_GLOBAL.match(l)} == globals_


def _lines(b: bytes) -> list[bytes]:
    return b.split(b"\n")


def split_unit(src: Path, out_dir: Path, target: int) -> list[Path] | None:
    """Writes <stem>__p<k>.c parts into out_dir and returns them, or None if the unit stays whole
    (small, or not in the expected layout). Deterministic: the same input gives the same parts."""
    b = src.read_bytes()
    if len(b) < target * 3 // 2:
        return None
    i = b.find(FN_START)
    j = b.rfind(SEG_START)
    if i < 0 or j < i:
        return None
    header, region, tail = b[:i + 1], b[i + 1:j + 1], b[j + 1:]
    if not RE_TAIL.match(tail) or b"recomp_gg_seen" in region:
        return None
    hlines = _lines(header)
    if hlines and hlines[-1] == b"":
        hlines.pop()
    if any(not (RE_HDR_SHARED.match(l) or RE_HDR_GLOBAL.match(l)) for l in hlines) and not header_repeatable(header):
        return None
    m = RE_SEEN.search(header)
    if not m or len(RE_SEEN.findall(header)) != 1:
        return None
    count = int(m.group(1))
    if int(RE_TAIL.match(tail).group(3)) != count:
        return None

    # Cut points at block starts, every ~target bytes.
    cuts, pos = [0], 0
    while True:
        k = region.find(FN_START, pos + target)
        if k < 0 or len(region) - (k + 1) < target // 3:  # do not leave a tiny last part
            break
        cuts.append(k + 1)
        pos = k + 1
    cuts.append(len(region))
    if len(cuts) < 3:
        return None

    chunks, names, base = [], [], 0
    for a, z in zip(cuts, cuts[1:]):
        ch = region[a:z]
        fns = RE_FN.findall(ch)
        n = [0]

        def renum(mm, base=base):
            idx = int(mm.group(1))
            if idx != base + n[0]:
                raise ValueError("guard slots are not sequential")
            n[0] += 1
            return b"RECOMP_GG_GUARD(_expected,%dU);" % (idx - base)

        try:
            ch2 = RE_GUARD.sub(renum, ch)
        except ValueError:
            return None
        if n[0] != len(fns) or not fns:
            return None
        chunks.append(ch2)
        names.append(fns)
        base += n[0]
    if base != count:
        return None

    out_dir.mkdir(parents=True, exist_ok=True)
    parts = []
    last = len(chunks) - 1
    for k, ch in enumerate(chunks):
        hdr = []
        for l in hlines:
            if RE_HDR_GLOBAL.match(l) and k > 0:
                continue
            if RE_SEEN.match(l):
                l = b"static uint32_t recomp_gg_seen[%d];" % len(names[k])
            hdr.append(l)
        body = [b"/* part %d of %d of %s (split_units.py v%s) */\n" % (k, last + 1, src.name.encode(), VERSION.encode()),
                b"\n".join(hdr) + b"\n", ch]
        if k == last:
            protos = b"".join(b"void %s(GuestContext* c);\n" % nm for ns in names[:-1] for nm in ns)
            body += [protos, tail]
        p = out_dir / f"{src.stem}__p{k}.c"
        data = b"".join(body)
        if not (p.exists() and p.stat().st_size == len(data) and p.read_bytes() == data):
            tmp = p.with_suffix(".c.part")
            tmp.write_bytes(data)
            tmp.replace(p)
        parts.append(p)
    return parts


def main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("unit", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--mb", type=float, default=8.0)
    a = ap.parse_args()
    parts = split_unit(a.unit, a.out, int(a.mb * 2 ** 20))
    if parts is None:
        print("left whole")
    else:
        for p in parts:
            print(p, p.stat().st_size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
