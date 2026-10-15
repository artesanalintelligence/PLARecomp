#!/usr/bin/env python3
"""Viewer and summary for nvntrace .nvt traces (format in tools/port/nvnvk/trace/trace_format.h).

  python nvntrace_view.py TRACE.nvt summary            # calls per function, frames, objects
  python nvntrace_view.py TRACE.nvt frame 120          # calls of frame 120 as text
  python nvntrace_view.py TRACE.nvt calls nvnTextureInitialize [--limit 20] [--blobs]
  python nvntrace_view.py TRACE.nvt shaders OUTDIR     # extracts control + code of each shader
  python nvntrace_view.py TRACE.nvt verify-map         # compares calls with nvn_sdk_map.csv

The library (`Trace`) is also used by nvnfields.py and the backend tests.
"""
from __future__ import annotations

import argparse
import collections
import csv
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterator, List, Optional

ROOT = Path(__file__).resolve().parents[4]

REC_CALL, REC_RET, REC_BLOB, REC_FRAME, REC_NOTE = 1, 2, 3, 4, 5
BLOB_KINDS = {0: "arg", 1: "obj", 2: "control", 3: "code", 4: "out"}
HDR = struct.Struct("<IBBHIIQ")  # size type pad fn tid frame seq  (24 bytes)


@dataclass
class Rec:
    type: int
    fn: int
    tid: int
    frame: int
    seq: int
    x: List[int] = field(default_factory=list)
    v: List[int] = field(default_factory=list)
    lr: int = 0
    ret: Optional[int] = None
    retv: Optional[int] = None
    blobs: List[tuple] = field(default_factory=list)  # (kind, addr, bytes)
    text: str = ""

    def f32(self, i: int) -> float:
        return struct.unpack("<f", struct.pack("<I", self.v[i] & 0xFFFFFFFF))[0]


class Trace:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        if d[:8] != b"NVNTRC01":
            raise ValueError("not an nvntrace trace")
        ver, n, self.sdk_base, self.main_base = struct.unpack_from("<IIQQ", d, 8)
        p = 32
        self.names: List[str] = []
        for _ in range(n):
            ln = struct.unpack_from("<H", d, p)[0]
            self.names.append(d[p + 2:p + 2 + ln].decode())
            p += 2 + ln
        p = (p + 7) & ~7
        self.start = p
        self.index = {nm: i for i, nm in enumerate(self.names)}

    def raw(self) -> Iterator[tuple]:
        d, p, end = self.data, self.start, len(self.data)
        while p + HDR.size <= end:
            size, typ, _, fn, tid, frame, seq = HDR.unpack_from(d, p)
            if size < HDR.size or p + size > end:
                break
            yield typ, fn, tid, frame, seq, p + HDR.size, size - HDR.size
            p += size

    def records(self) -> Iterator[Rec]:
        """CALL with its input BLOBs; RET (with output BLOBs) is attached to the open CALL of the
        same thread and function if there is one, otherwise emitted on its own."""
        d = self.data
        pending: Dict[tuple, Rec] = {}
        last: Optional[Rec] = None
        for typ, fn, tid, frame, seq, p, n in self.raw():
            if typ == REC_CALL:
                vals = struct.unpack_from("<14Q", d, p)
                r = Rec(typ, fn, tid, frame, seq, list(vals[:8]), list(vals[8:12]), vals[12])
                pending[(tid, fn)] = r
                last = r
                yield r
            elif typ == REC_RET:
                x0, v0 = struct.unpack_from("<QQ", d, p)
                r = pending.pop((tid, fn), None)
                if r is not None:
                    r.ret, r.retv = x0, v0
                    last = r
                else:
                    last = Rec(typ, fn, tid, frame, seq, ret=x0, retv=v0)
                    yield last
            elif typ == REC_BLOB:
                addr, ln, kind = struct.unpack_from("<QII", d, p)
                blob = (kind, addr, d[p + 16:p + 16 + ln])
                if last is not None and last.fn == fn:
                    last.blobs.append(blob)
                else:
                    yield Rec(REC_BLOB, fn, tid, frame, seq, blobs=[blob])
            elif typ == REC_FRAME:
                yield Rec(REC_FRAME, fn, tid, frame, seq)
            elif typ == REC_NOTE:
                yield Rec(REC_NOTE, fn, tid, frame, seq, text=d[p:p + n].rstrip(b"\0").decode("utf-8", "replace"))

    def name(self, fn: int) -> str:
        return self.names[fn] if fn < len(self.names) else f"#{fn}"


def fmt_call(t: Trace, r: Rec, blobs: bool = False) -> str:
    nm = t.name(r.fn)
    args = ", ".join(f"{a:#x}" for a in r.x[:6])
    s = f"[f{r.frame} #{r.seq} t{r.tid}] {nm}({args})"
    if any(r.v):
        s += "  v=" + ",".join(f"{r.f32(i):g}" for i in range(4))
    if r.ret is not None:
        s += f" -> {r.ret:#x}"
    if r.lr:
        s += f"  @main+{r.lr - t.main_base:#x}" if r.lr >= t.main_base else f"  @{r.lr:#x}"
    if blobs:
        for kind, addr, b in r.blobs:
            s += f"\n      {BLOB_KINDS.get(kind, kind)} {addr:#x} [{len(b)}] " + b[:64].hex(" ", 4)
    return s


def cmd_summary(t: Trace, a) -> None:
    calls = collections.Counter()
    frames = set()
    per_frame = collections.Counter()
    blobs = collections.Counter()
    blob_bytes = 0
    notes = []
    for typ, fn, tid, frame, seq, p, n in t.raw():
        if typ == REC_CALL:
            calls[fn] += 1
            per_frame[frame] += 1
        elif typ == REC_FRAME:
            frames.add(frame)
        elif typ == REC_BLOB:
            kind = struct.unpack_from("<I", t.data, p + 12)[0]
            blobs[kind] += 1
            blob_bytes += n
        elif typ == REC_NOTE:
            notes.append(t.data[p:p + n].rstrip(b"\0").decode())
    print(f"{t.path.name}: {len(t.data) / 1e6:.1f} MB, sdk={t.sdk_base:#x} main={t.main_base:#x}")
    print(f"frames presented: {len(frames)}; distinct functions logged: {len(calls)}; "
          f"CALL records: {sum(calls.values())}")
    print(f"blobs: " + ", ".join(f"{BLOB_KINDS[k]}={v}" for k, v in sorted(blobs.items())) +
          f" ({blob_bytes / 1e6:.1f} MB)")
    if notes:
        print("notes:", notes[-3:])
    print("\ncalls logged per function (full detail only in detail_frames):")
    for fn, c in calls.most_common(a.top):
        print(f"  {c:9d}  {t.name(fn)}")
    if per_frame:
        fs = sorted(per_frame)
        sample = [f for f in fs if f in (1, 2, 10, 100, 200, 300, 400)]
        print("\nCALL per frame (samples):", ", ".join(f"f{f}={per_frame[f]}" for f in sample))


def cmd_frame(t: Trace, a) -> None:
    out = []
    for r in t.records():
        if r.frame == a.n and r.type in (REC_CALL, REC_RET):
            out.append(r)
        elif r.frame > a.n:
            break
    for r in out:  # printed at the end: RETs are attached after the CALL
        if a.grep and not any(g in t.name(r.fn) for g in a.grep):
            continue
        print(fmt_call(t, r, a.blobs))


def cmd_calls(t: Trace, a) -> None:
    want = {t.index[n] for n in a.names}
    out = []
    for r in t.records():
        if r.fn in want and r.type in (REC_CALL, REC_RET):
            out.append(r)
            if len(out) > a.limit:
                break
    for r in out[:a.limit]:
        print(fmt_call(t, r, a.blobs))


def cmd_shaders(t: Trace, a) -> None:
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    seen = set()
    for typ, fn, tid, frame, seq, p, ln in t.raw():
        if typ != REC_BLOB:
            continue
        addr, bl, kind = struct.unpack_from("<QII", t.data, p)
        if kind in (2, 3) and (kind, addr) not in seen:
            seen.add((kind, addr))
            ext = "control" if kind == 2 else "code"
            (out / f"{addr:012x}.{ext}.bin").write_bytes(t.data[p + 16:p + 16 + bl])
            n += 1
    print(f"{n} shader blobs -> {out}")


def cmd_verify_map(t: Trace, a) -> None:
    counts = collections.Counter()
    for typ, fn, *_ in t.raw():
        if typ == REC_CALL:
            counts[fn] += 1
    rows = list(csv.DictReader(open(ROOT / "docs/port/data/nvn_sdk_map.csv", newline="")))
    used_static = {r["name"] for r in rows if r["used"] == "1"}
    called = {t.name(fn) for fn in counts}
    print(f"calls seen: {len(called)} functions; statically used: {len(used_static)}")
    print(f"  seen and marked used: {len(called & used_static)}")
    print(f"  seen but NOT marked used: {sorted(called - used_static)}")
    print(f"  marked used and not seen (yet): {len(used_static - called)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace", type=Path)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary"); s.add_argument("--top", type=int, default=400)
    s = sub.add_parser("frame"); s.add_argument("n", type=int); s.add_argument("--blobs", action="store_true")
    s.add_argument("--grep", nargs="*", help="only functions whose name contains any of these strings")
    s = sub.add_parser("calls"); s.add_argument("names", nargs="+"); s.add_argument("--limit", type=int, default=50)
    s.add_argument("--blobs", action="store_true")
    s = sub.add_parser("shaders"); s.add_argument("outdir")
    sub.add_parser("verify-map")
    a = ap.parse_args()
    t = Trace(a.trace)
    {"summary": cmd_summary, "frame": cmd_frame, "calls": cmd_calls, "shaders": cmd_shaders,
     "verify-map": cmd_verify_map}[a.cmd](t, a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
