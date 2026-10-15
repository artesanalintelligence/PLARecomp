"""Context of a function for decompiling it: what an agent reads before writing C++.

  python tools/decomp/ctx.py 0x7100123456
  python tools/decomp/ctx.py FUN_7100123456 --callers 40
  python tools/decomp/ctx.py 0x7100123456 --no-asm      # summary + pseudo-C only
  python tools/decomp/ctx.py 0x7100123456 --json

Includes: header (size, category, TU, status), vtable membership (RTTI or without RTTI),
annotated disassembly (capstone), outgoing calls with name and status, callers,
strings, global data and referenced vtables, declarations already present in include/ and src/
and the Ghidra pseudo-C (cache tools/ghidra/out/decomp/<dir>.c; if missing, it is requested from
the running Ghidra batch or Ghidra headless is launched on demand, ~10-60 s).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (CACHE, DECOMP_OUT, GHIDRA_OUT, fmt, functions, guess_category, load_json, triage,  # noqa: E402
                    tu_map)
from image import load_image  # noqa: E402
import status as st  # noqa: E402

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs  # noqa: E402


# ------------------------------------------------------------------ global indexes (cache)
def callgraph(im):
    """(sites, targets, kind) of all BL (0) and B (1) in the code, sorted by target."""
    from image import _key
    p = CACHE / f"callgraph_{_key()}.npz"
    if p.exists():
        z = np.load(p)
        return z["site"], z["target"], z["kind"]
    lo, hi = im.text
    words = np.frombuffer(bytes(im.blob[lo - im.base:hi - im.base]), dtype="<u4")
    pcs = np.arange(len(words), dtype=np.int64) * 4 + lo
    op = words & 0xFC000000
    isbl = op == 0x94000000
    isb = op == 0x14000000
    sel = isbl | isb
    imm = (words[sel] & 0x3FFFFFF).astype(np.int64)
    imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
    site = pcs[sel]
    target = site + imm * 4
    kind = np.where(isbl[sel], 0, 1).astype(np.int8)
    order = np.argsort(target, kind="stable")
    site, target, kind = site[order], target[order], kind[order]
    CACHE.mkdir(parents=True, exist_ok=True)
    import os
    tmp = p.with_name(f"callgraph_{os.getpid()}.tmp.npz")
    np.savez(tmp, site=site, target=target, kind=kind)
    os.replace(tmp, p)
    return site, target, kind


def vtable_index():
    """function address -> [(class or vtable, address_point, slot)] and address_point -> class."""
    p = CACHE / "vslots.json"
    src = GHIDRA_OUT / "rtti.json"
    if p.exists() and src.exists() and p.stat().st_mtime >= src.stat().st_mtime:
        d = json.loads(p.read_text(encoding="utf-8"))
        return ({int(k, 16): v for k, v in d["slots"].items()}, {int(k, 16): v for k, v in d["vtables"].items()})
    r = load_json(src, {}) or {}
    slots: dict[int, list] = {}
    vts: dict[int, str] = {}

    def add(name, vt):
        ap = int(vt["address_point"], 16)
        vts[ap] = name
        for i, e in enumerate(vt.get("entries", [])):
            if isinstance(e, str) and e.startswith("0x"):
                slots.setdefault(int(e, 16), []).append([name, f"{ap:#x}", i])
    for cname, c in r.get("classes", {}).items():
        for vt in c.get("vtables", []):
            add(cname, vt)
    for v in r.get("vtables_without_rtti", []):
        for vt in v.get("vtables", []):
            add(v["name"], vt)
    CACHE.mkdir(parents=True, exist_ok=True)
    from common import atomic_write_text
    atomic_write_text(p, json.dumps({"slots": {f"{k:x}": v for k, v in slots.items()},
                                     "vtables": {f"{k:x}": v for k, v in vts.items()}}))
    return slots, vts


# ------------------------------------------------------------------ function analysis
def printable(b: bytes) -> bool:
    if len(b) < 2:
        return False
    try:
        s = b.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return sum(ch.isprintable() or ch in "\n\t" for ch in s) >= 0.9 * len(s)


class Annotator:
    def __init__(self, im, funcs, vts, statusmap, markers):
        self.im, self.funcs, self.vts, self.status, self.markers = im, funcs, vts, statusmap, markers

    def describe(self, a: int) -> tuple[str, str]:
        """(kind, description) of a data/code address."""
        im = self.im
        if a in self.vts or a + 0x10 in self.vts:
            # a = address point, or a = _ZTV symbol (address point - 0x10). (Previously a - 0x10 was checked,
            # which labeled as vtable any data 0x10 bytes after an address point, e.g. "".)
            ap_ = a if a in self.vts else a + 0x10
            nm = self.vts[ap_]
            ms = [m for m in (self.markers.by_addr.get(ap_ - 0x10, []) if self.markers else []) if m.kind == "vtable"]
            if ms:
                nm += f" = class {ms[0].qualname} [{ms[0].file}:{ms[0].line}]"
            what = "address point" if a == ap_ else "start, _ZTV symbol"
            return "vtable", f"vtable of {nm} ({what}; _ZTV at {ap_ - 0x10:#x})"
        f = self.funcs.at(a)
        if f:
            return "func", self.fname(a)
        if a in im.got:
            return "got", f"GOT -> import {im.got[a]}"
        if im.contains(a, 8) and 0x7104270878 <= a < 0x7104289500:
            v = im.u64(a)
            if im.is_code(v):
                return "got", f"GOT -> {self.funcs.label(v)}"
            return "got", f"GOT -> {self.describe(v)[1] if v != a else fmt(v)}"
        if im.contains(a) and im.is_readonly(a) and not im.is_code(a):
            s = im.cstring(a, 300)
            if s is not None and printable(s):
                return "str", json.dumps(s.decode("utf-8", "replace"), ensure_ascii=False)
            if s == b"" and im.contains(a - 1) and im.blob[a - 1 - im.base] == 0:
                return "str", '"" (empty string, probably)'
        if im.contains(a):
            seg = "rodata" if im.is_readonly(a) else "data/bss"
            if seg == "rodata":
                seg += ": " + im.read(a, 16).hex(" ")
            elif im.contains(a, 8):
                v = im.u64(a)
                if im.contains(v) and im.is_readonly(v) and not im.is_code(v) and v not in self.vts:
                    cs = im.cstring(v, 200)
                    if cs is not None and (printable(cs) or cs == b""):
                        # pointer to string (global const char*), not a vtable
                        seg += f"; const char* -> {v:#x} {json.dumps(cs.decode('utf-8', 'replace'), ensure_ascii=False)}"
            ms = self.markers.by_addr.get(a) if self.markers else None
            nm = ms[0].qualname if ms else f"DAT_{a:x}"
            return "data", f"{nm} ({seg})"
        return "other", fmt(a)

    def fname(self, a: int) -> str:
        f = self.funcs.at(a)
        nm = self.funcs.demangled(f) if f else self.funcs.label(a)
        if a in self.im.plt:
            nm = f"{self.im.plt[a]} (PLT)"
        q = self.status.get(a, {}).get("quality", "")
        ms = self.markers.by_addr.get(a) if self.markers else None
        extra = ""
        if ms:
            extra = f" = {ms[0].qualname} [{ms[0].file}:{ms[0].line}]"
        return nm + (f" [{q}]" if q and q != "U" else "") + extra


def analyze(func, im, funcs, ann):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    code = im.read(func.addr, func.size)
    insns = list(md.disasm(code, func.addr))
    pages: dict[str, int] = {}
    refs = []        # (pc, addr)
    calls = []       # (pc, target, tail)
    local_targets = set()
    lines = []
    for ins in insns:
        note = ""
        m = ins.mnemonic
        ops = ins.op_str
        known = None   # (rd, value) left by this instruction
        if m == "adrp":
            known = (ops.split(",")[0].strip(), ins.operands[1].imm)
        elif m == "add" and len(ins.operands) == 3 and ins.operands[2].type == 2:
            rn = ops.split(",")[1].strip()
            if rn in pages:
                a = pages[rn] + ins.operands[2].imm
                refs.append((ins.address, a))
                note = ann.describe(a)[1]
                known = (ops.split(",")[0].strip(), a)
        elif m.startswith(("ldr", "str", "ldp", "stp", "ld", "st")) and "[" in ops:
            inner = ops[ops.index("[") + 1:].split("]")[0]
            parts = [p.strip() for p in inner.split(",")]
            if parts[0] in pages:
                off = 0
                for op in ins.operands:
                    if op.type == 3:  # MEM
                        off = op.mem.disp
                a = pages[parts[0]] + off
                if len(parts) > 1 and not parts[1].startswith("#"):
                    note = "table at " + ann.describe(a)[1]
                else:
                    note = ann.describe(a)[1]
                refs.append((ins.address, a))
        if m in ("bl", "b") or m.startswith("b.") or m in ("cbz", "cbnz", "tbz", "tbnz"):
            tgt = None
            for op in ins.operands:
                if op.type == 2:
                    tgt = op.imm
            if tgt is not None:
                if func.addr <= tgt < func.end:
                    local_targets.add(tgt)
                    note = f"-> L_{tgt - func.addr:x}"
                else:
                    calls.append((ins.address, tgt, m != "bl"))
                    note = ("tail call: " if m != "bl" else "") + ann.fname(tgt)
        elif m == "blr":
            note = "indirect call (vtable / function pointer?)"
        # overwritten registers: forget their value (except the one just computed)
        try:
            _, wr = ins.regs_access()
            for r in wr:
                pages.pop(ins.reg_name(r), None)
        except Exception:  # noqa: BLE001
            pass
        if m in ("bl", "blr"):
            for r in list(pages):
                if r.startswith("x") and r[1:].isdigit() and int(r[1:]) <= 18:
                    pages.pop(r)
        if known:
            pages[known[0]] = known[1]
        lines.append((ins.address, f"{ins.mnemonic} {ops}".strip(), note))
    out_lines = []
    for a, txt, note in lines:
        if a in local_targets:
            out_lines.append(f"L_{a - func.addr:x}:")
        out_lines.append(f"  {a - func.addr:5x}  {txt:<44}{'// ' + note if note else ''}")
    return out_lines, refs, calls


def get_pseudo_c(addr: int, allow_launch: bool, wait: float = 90.0) -> str | None:
    p = DECOMP_OUT / f"{addr:x}.c"
    if p.exists():
        return p.read_text(encoding="utf-8", errors="replace")
    e = DECOMP_OUT / f"{addr:x}.err"
    if e.exists():
        return "/* Ghidra could not decompile: " + e.read_text(encoding="utf-8").strip() + " */"
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import ghidra_decomp as gd
    if gd.server_alive():
        gd.request([addr], wait=wait)
    elif allow_launch:
        print(f"[ctx] launching Ghidra headless for {fmt(addr)} (~10-60 s)...", file=sys.stderr)
        subprocess.run([sys.executable, str(Path(gd.__file__)), "--addr", f"{addr:#x}"], capture_output=True)
    if p.exists():
        return p.read_text(encoding="utf-8", errors="replace")
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("func", help="address, FUN_xxx or name")
    ap.add_argument("--no-asm", action="store_true")
    ap.add_argument("--no-ghidra", action="store_true", help="do not show/request the pseudo-C")
    ap.add_argument("--no-launch", action="store_true", help="do not launch Ghidra if the pseudo-C is not cached")
    ap.add_argument("--callers", type=int, default=15, help="maximum number of callers to list")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)

    funcs = functions()
    f = funcs.resolve(a.func)
    if f is None:
        print(f"cannot find function {a.func}", file=sys.stderr)
        return 2
    im = load_image()
    slots, vts = vtable_index()
    smap = st.read()
    try:
        from srcindex import MarkerIndex
        markers = MarkerIndex()
    except Exception:  # noqa: BLE001
        markers = None
    ann = Annotator(im, funcs, vts, smap, markers)
    asm, refs, calls = analyze(f, im, funcs, ann)
    site, target, kind = callgraph(im)
    i0, i1 = np.searchsorted(target, f.addr), np.searchsorted(target, f.addr, side="right")
    callers = []
    for s, k in zip(site[i0:i1], kind[i0:i1]):
        c = funcs.containing(int(s))
        if c is not None and c.addr != f.addr:
            callers.append((c.addr, int(s), int(k)))
    uniq_callers = sorted({c[0] for c in callers})

    tri = triage()
    cat = tri.get(f.addr, "?") if tri else f"{guess_category(f)} (heuristic; data/triage.csv does not exist yet)"
    tu = tu_map().get(f.addr, "")
    stt = smap.get(f.addr, {})
    out = {
        "address": fmt(f.addr), "size": f.size, "name": funcs.demangled(f), "csv_name": f.name,
        "category": cat, "tu": tu, "status": stt.get("quality", "U"), "agent": stt.get("agent", ""),
        "vtable_slots": [{"vtable": s[0], "address_point": s[1], "slot": s[2]} for s in slots.get(f.addr, [])],
        "callees": [], "callers": [], "strings": [], "data": [], "vtables": [], "declared": [],
    }
    seen = set()
    for pc, t, tail in calls:
        if t in seen:
            continue
        seen.add(t)
        out["callees"].append({"address": fmt(t), "name": ann.fname(t), "tail": tail,
                               "size": funcs.at(t).size if funcs.at(t) else None})
    for c in uniq_callers[: a.callers]:
        cf = funcs.at(c)
        out["callers"].append({"address": fmt(c), "name": ann.fname(c), "size": cf.size if cf else None})
    out["n_callers"] = len(uniq_callers)
    seen = set()
    for pc, r in refs:
        if r in seen:
            continue
        seen.add(r)
        k, d = ann.describe(r)
        item = {"address": fmt(r), "desc": d, "at": f"+{pc - f.addr:#x}"}
        {"str": out["strings"], "vtable": out["vtables"]}.get(k, out["data"]).append(item)
        if k == "vtable":
            ap_ = r if r in vts else r - 0x10
            item["siblings"] = sum(1 for fs in slots.values() for s in fs if int(s[1], 16) == ap_)
    if markers:
        for m in markers.by_addr.get(f.addr, []):
            out["declared"].append(f"{m.file}:{m.line}: {m.decl}")
    out["ghidra"] = None if a.no_ghidra else get_pseudo_c(f.addr, not a.no_launch)

    if a.json:
        out["asm"] = asm
        print(json.dumps(out, indent=1, ensure_ascii=False))
        return 0
    P = print
    P(f"=== {out['name']} @ {out['address']}  ({f.size} bytes, {f.size // 4} instructions) ===")
    P(f"category: {cat} | TU: {tu or '(no data/tu_map.csv)'} | status: {out['status']}"
      f"{' (' + out['agent'] + ')' if out['agent'] else ''} | CSV name: {f.name}")
    if out["vtable_slots"]:
        P("in vtables: " + "; ".join(f"slot {s['slot']} of {s['vtable']} (ap {s['address_point']})"
                                     for s in out["vtable_slots"][:6]))
    if out["declared"]:
        P("already declared/defined in: " + "; ".join(out["declared"]))
    if not a.no_asm:
        P("\n--- disassembly ---")
        for ln in asm:
            P(ln)
    P(f"\n--- outgoing calls ({len(out['callees'])}) ---")
    for c in out["callees"]:
        P(f"  {c['address']}  {'(tail) ' if c['tail'] else ''}{c['name']}  [{c['size']} B]")
    P(f"\n--- callers ({out['n_callers']}, showing {len(out['callers'])}) ---")
    for c in out["callers"]:
        P(f"  {c['address']}  {c['name']}  [{c['size']} B]")
    if out["strings"]:
        P("\n--- strings ---")
        for s in out["strings"]:
            P(f"  {s['at']:>7} {s['address']}  {s['desc']}")
    if out["vtables"]:
        P("\n--- referenced vtables (constructor/destructor?) ---")
        for s in out["vtables"]:
            P(f"  {s['at']:>7} {s['address']}  {s['desc']}  ({s.get('siblings', 0)} virtual functions)")
    if out["data"]:
        P("\n--- global data / GOT ---")
        for s in out["data"]:
            P(f"  {s['at']:>7} {s['address']}  {s['desc']}")
    if not a.no_ghidra:
        P("\n--- Ghidra pseudo-C ---")
        P(out["ghidra"] or "(not available: run python tools/decomp/ghidra_decomp.py --addr "
                           f"{out['address']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
