"""Verifies decompiled functions: exact match (O) or functional equivalence with Unicorn (F).

  python tools/decomp/verify.py 0x7100123456            # one function (looks for its marker in src/)
  python tools/decomp/verify.py --file src/field/Foo.cpp  # all functions with a marker in the TU
  python tools/decomp/verify.py FUN_7100123456 --cases 96 -v

Per-function flow:
  1. Finds the definition with marker `// 0x7100123456` in src/ (or in --file).
  2. Compiles the TU with clang (tools/decomp/compiler.json) -> build/decomp/*.o
  3. Exact match: relocates our function as if it were at the original address
     (external symbols -> original addresses) and compares bytes -> quality O.
  4. Otherwise, differential equivalence (tools/decomp/emu.py) over N cases -> quality F if all
     agree. Otherwise it prints the differences and marks W.
  5. Updates data/decomp_status.csv (unless --no-status) and data/decomp_symbols.csv.

Coverage options (add cases after the basic ones; the basic ones do not change):
  --ret-set [N]       N cases (default --cases) in which each intercepted call returns a value
                      from a small set (0, 1, -1, seeded pointer, x0, x1...) and its sret (x8) is filled
  --sret              add a copy of the basic cases with the calls' sret (x8) filled
  --seed-globals [N]  N cases (default --cases/2) with the .data/.bss pages the function references
                      seeded with pointers and values (already-initialized singletons)
  --follow            add a copy of the basic cases (and use it in the extra ones) in which calls to
                      already-verified main functions (O/F) really execute instead of being intercepted
                      (covers loops like `while (t != x) t = t->parent`)
  --deep              all of the above

Exit codes: 0 = all O/F, 1 = some failure, 2 = usage/compilation error.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (ADDR_NAME_RE, DATA, DATA_SYMBOLS_CSV, ROOT, agent_name, fmt, functions,  # noqa: E402
                    parse_addr)
import status as st  # noqa: E402
from demangle import demangle_many  # noqa: E402
from emu import (SCRATCH, STACK_SIZE, STACK_TOP, ArityOracle, Machine, make_cases,  # noqa: E402
                 referenced_data_pages)
from image import load_image  # noqa: E402
from objlink import LinkError, ObjFile, compile_tu, link, relocate_function_at  # noqa: E402
from srcindex import MarkerIndex  # noqa: E402

SNAPSHOT_DIR = DATA / "snapshots"
M64 = (1 << 64) - 1


# ------------------------------------------------------------------ resolution
class Resolver:
    def __init__(self, image, funcs, markers: MarkerIndex, dem: dict):
        self.im, self.funcs, self.markers, self.dem = image, funcs, markers, dem
        self.cache: dict[str, int | None] = {}
        self.data_syms = {}
        if DATA_SYMBOLS_CSV.exists():
            with open(DATA_SYMBOLS_CSV, newline="", encoding="utf-8") as f:
                for r in csv.DictReader(f):
                    try:
                        self.data_syms[r["Name"]] = parse_addr(r["Address"])
                    except (KeyError, ValueError):
                        pass
        self.decomp_syms = st.symbols_by_name()

    def marker_for(self, name: str):
        return self.markers.lookup_symbol(name, self.dem.get(name))

    def __call__(self, name: str) -> int | None:
        if name in self.cache:
            return self.cache[name]
        v = self._resolve(name)
        self.cache[name] = v
        return v

    def _resolve(self, name: str) -> int | None:
        if not name:
            return None
        m = self.marker_for(name)
        if m:
            return m.addr
        mm = ADDR_NAME_RE.search(name)
        if mm:
            return int(mm.group(1), 16)
        im = self.im
        if name in im.plt_by_name:
            return im.plt_by_name[name]
        if name in im.import_addr:
            return im.import_addr[name]
        if name in im.dynsyms:
            return im.dynsyms[name]
        f = self.funcs.by_name.get(name)
        if f:
            return f.addr
        if name in self.data_syms:
            return self.data_syms[name]
        if name in self.decomp_syms:
            return self.decomp_syms[name]
        return None


# ------------------------------------------------------------------ return type
_W8 = {"bool", "char", "s8", "u8", "int8_t", "uint8_t", "signed char", "unsigned char"}
_W16 = {"s16", "u16", "short", "unsigned short", "int16_t", "uint16_t", "char16_t"}
_W32 = {"int", "s32", "u32", "unsigned", "unsigned int", "int32_t", "uint32_t", "signed", "char32_t", "wchar_t"}


def ret_kind(marker) -> tuple[str, int] | None:
    parts = marker.qualname.split("::")
    if parts[-1].startswith("~") or (len(parts) >= 2 and parts[-1] == parts[-2]):
        return None
    r = " ".join(marker.ret.replace("const ", " ").replace("volatile ", " ").split())
    if r in ("", "void"):
        return None if r == "void" or "operator" not in parts[-1] else ("x", 64)
    if "*" in r or "&" in r:
        return ("x", 64)
    base = r.split("::")[-1] if r.startswith(("std::", "::")) else r
    if base in _W8:
        return ("x", 8)
    if base in _W16:
        return ("x", 16)
    if base in _W32:
        return ("x", 32)
    if base in ("float", "f32"):
        return ("v", 32)
    if base in ("double", "f64"):
        return ("v", 64)
    return ("x", 64)


# ------------------------------------------------------------------ comparison
class Comparator:
    def __init__(self, machine: Machine, image, linked, funcs, oracle: ArityOracle):
        self.m, self.im, self.ln, self.funcs, self.oracle = machine, image, linked, funcs, oracle

    def lbl(self, a: int) -> str:
        if self.im.base <= a < self.im.base + self.im.size:
            return f"{self.funcs.label(a)} ({a:#x})"
        if a in self.im.imports:
            return f"import {self.im.imports[a]}"
        if SCRATCH <= a < SCRATCH + 0x400000:
            for n, sa in self.ln.sym_addr.items():
                if sa == a:
                    return f"local {n} ({a:#x})"
            return f"local {a:#x}"
        return f"{a:#x}"

    def _scratch_ro_obj(self, b: int):
        for s, e, n in self.ln.objects:
            if s <= b < e:
                return s, e, n
        for s, e, n in self.ln.ro_ranges:
            if s <= b < e:
                return s, e, n
        return None

    def _cstr_scratch(self, b: int) -> bytes | None:
        o = b - SCRATCH
        blob = self.m.scratch_blob
        if not (0 <= o < len(blob)):
            return None
        end = blob.find(b"\0", o, o + 4096)
        return None if end < 0 else blob[o:end]

    def equiv(self, a: int, b: int) -> bool:
        """a = value in the original, b = value in ours."""
        if a == b:
            return True
        lo = STACK_TOP - STACK_SIZE
        if lo <= a < self.m.sp0 and lo <= b < self.m.sp0:
            return True   # pointers to the local stack: the frame layout may differ
        if self.im.contains(a) and self.im.is_readonly(a):
            ro = self._scratch_ro_obj(b)
            if ro:
                sa = self.im.cstring(a, 4096)
                sb = self._cstr_scratch(b)
                if sa is not None and sb is not None and sa == sb:
                    return True
                s, e, n = ro
                k = e - b
                if 0 < k <= 4096 and self.im.read(a, k) == self.m.scratch_blob[b - SCRATCH:b - SCRATCH + k]:
                    return True
            elif self.im.contains(b) and self.im.is_readonly(b):
                sa, sb = self.im.cstring(a, 4096), self.im.cstring(b, 4096)
                if sa is not None and sa == sb:
                    return True
        return False

    def mem_diffs(self, ma: dict, mb: dict, limit=6) -> list[str]:
        out = []
        words = sorted({x & ~7 for x in ma} | {x & ~7 for x in mb})
        for w in words:
            ka = [ma.get(w + i) for i in range(8)]
            kb = [mb.get(w + i) for i in range(8)]
            if ka == kb:
                continue
            init = None
            ba, bb = bytearray(8), bytearray(8)
            for i in range(8):
                if ka[i] is None or kb[i] is None:
                    if init is None:
                        init = self.m.initial_bytes(w, 8)
                ba[i] = ka[i] if ka[i] is not None else init[i]
                bb[i] = kb[i] if kb[i] is not None else init[i]
            if ba == bb:
                continue
            va, vb = int.from_bytes(ba, "little"), int.from_bytes(bb, "little")
            if self.equiv(va, vb):
                continue
            where = self.lbl_data(w)
            out.append(f"memory {where}: original={va:#018x} ours={vb:#018x}")
            if len(out) >= limit:
                out.append("...")
                break
        return out

    def lbl_data(self, a: int) -> str:
        if self.im.contains(a):
            return f"{a:#x} (game global data)"
        if SCRATCH <= a < SCRATCH + 0x400000:
            for s, e, n in self.ln.objects:
                if s <= a < e:
                    return f"{a:#x} (TU-LOCAL data {n}+{a - s:#x}: does it need to be declared as a game global?)"
            return f"{a:#x} (TU-local section)"
        if STACK_TOP - STACK_SIZE <= a < STACK_TOP:
            return f"{a:#x} (caller stack / stack arguments)"
        return f"{a:#x}"

    def compare(self, ra, rb, rk) -> list[str]:
        d = []
        if ra.timeout and rb.timeout:
            return ["__inconclusive__ both exceed the instruction limit"]
        if ra.timeout != rb.timeout:
            d.append(f"instruction limit: original={'yes' if ra.timeout else 'no'} ours={'yes' if rb.timeout else 'no'}")
        if (ra.fault is None) != (rb.fault is None):
            d.append(f"memory fault: original={ra.fault} ours={rb.fault}")
        if not rb.callee_saved_ok and rb.fault is None:
            d.append("ours " + rb.callee_saved_msg)
        # calls
        ca, cb = ra.calls, rb.calls
        for k in range(max(len(ca), len(cb))):
            if k >= len(ca):
                d.append(f"call #{k} is EXTRA in ours: {self.lbl(cb[k].target)}")
                break
            if k >= len(cb):
                d.append(f"call #{k} is MISSING in ours: {self.lbl(ca[k].target)}")
                break
            a, b = ca[k], cb[k]
            if a.target != b.target:
                d.append(f"call #{k}: original calls {self.lbl(a.target)}, ours calls {self.lbl(b.target)}")
                break
            spec = self.oracle.args_of(a.target)
            warn_unknown = spec is None
            if spec is None:
                spec = {"x": {0: 64}, "v": {}}
            for i, wbits in sorted(spec["x"].items()):
                mask = (1 << wbits) - 1
                va, vb = a.x[i] & mask, b.x[i] & mask
                if va != vb and not (wbits == 64 and self.equiv(va, vb)):
                    d.append(f"call #{k} to {self.lbl(a.target)}: x{i} original={va:#x} ours={vb:#x}")
            for i, wbits in sorted(spec["v"].items()):
                mask = (1 << wbits) - 1
                if (a.v[i] & mask) != (b.v[i] & mask):
                    d.append(f"call #{k} to {self.lbl(a.target)}: v{i} ({wbits} bits) "
                             f"original={a.v[i] & mask:#x} ours={b.v[i] & mask:#x}")
            md = self.mem_diffs(a.mem, b.mem, limit=3)
            if md:
                d.append(f"before call #{k} to {self.lbl(a.target)}: " + "; ".join(md))
            if warn_unknown and any(a.x[i] != b.x[i] for i in range(1, 8)):
                pass  # target of unknown arity: only x0 is compared
            if d:
                break
        if ra.noreturn != rb.noreturn and not d:
            d.append(f"noreturn call: original={ra.noreturn} ours={rb.noreturn}")
        if ra.fault and rb.fault:
            return d   # both fault: memory/return on fault is not comparable
        if rk and not d and not ra.noreturn:
            reg, wbits = rk
            mask = (1 << wbits) - 1
            if reg == "x":
                va, vb = ra.ret_x[0] & mask, rb.ret_x[0] & mask
                if va != vb and not (wbits == 64 and self.equiv(va, vb)):
                    d.append(f"return x0 ({wbits} bits): original={va:#x} ours={vb:#x}")
            else:
                va, vb = ra.ret_v[0] & mask, rb.ret_v[0] & mask
                if va != vb:
                    d.append(f"return v0 ({wbits} bits): original={va:#x} ours={vb:#x}")
        if not d:
            d.extend(self.mem_diffs(ra.written, rb.written))
        return d


# ------------------------------------------------------------------ verification
class TU:
    """A compiled file + everything needed to verify its functions."""

    def __init__(self, src: Path, markers: MarkerIndex, verbose=False):
        self.src = src
        self.markers = markers
        t = time.time()
        self.obj_path, self.diag, self.cmd = compile_tu(src)
        self.compile_time = time.time() - t
        self.obj = ObjFile(self.obj_path) if self.obj_path else None
        self.dem = {}
        if self.obj:
            names = [s.name for s in self.obj.syms if s.name and s.name.startswith("_Z")]
            self.dem = {k: v for k, v in demangle_many(names).items() if v}

    def cleanup(self):
        if self.obj_path and self.obj_path.exists():
            try:
                self.obj_path.unlink()
            except OSError:
                pass


def find_symbol(tu: TU, addr: int):
    cands = []
    rel = tu.src.relative_to(ROOT).as_posix() if tu.src.resolve().is_relative_to(ROOT) else None
    for s in tu.obj.functions():
        m = tu.markers.lookup_symbol(s.name, tu.dem.get(s.name), file=rel)
        if m and m.addr == addr:
            cands.append(s)
    if not cands:
        return None
    cands.sort(key=lambda s: (s.size == 0, "C1E" in s.name, s.name))
    return cands[0]


def tu_addresses(tu: TU) -> list[int]:
    """Addresses to verify in a TU: the markers of DEFINITIONS in this file and, in addition, the
    markers of declarations (e.g. inside the class, in the header) whose function is DEFINED
    in this .o as a strong symbol (not the inline/weak ones from headers, which every TU emits)."""
    rel = tu.src.relative_to(ROOT).as_posix()
    addrs = {m.addr for m in tu.markers.markers if m.file == rel and m.is_def and m.kind == "func"}
    if tu.obj is not None:
        for sy in tu.obj.functions():
            if sy.bind == "STB_WEAK" or sy.size == 0:
                continue
            m = tu.markers.lookup_symbol(sy.name, tu.dem.get(sy.name), file=rel)
            if m is not None and m.kind == "func" and (not m.is_def or m.file == rel):
                addrs.add(m.addr)
    return sorted(addrs)


def verify_one(addr: int, tu: TU, args, image, funcs, machine: Machine | None) -> dict:
    t0 = time.time()
    f = funcs.at(addr)
    res = {"addr": addr, "name": funcs.demangled(f) if f else fmt(addr), "quality": "W", "msgs": [], "time": 0.0,
           "coverage": "", "cases": "", "symbol": "", "file": tu.src.relative_to(ROOT).as_posix()}
    if f is None:
        res["msgs"].append("the address is not a function start in data/main_functions.csv")
        return res
    sym = find_symbol(tu, addr)
    if sym is None:
        defs = [m for m in tu.markers.by_addr.get(addr, []) if m.is_def]
        hint = f" (marker at {defs[0].file}:{defs[0].line} as {defs[0].qualname})" if defs else ""
        res["msgs"].append(f"cannot find the symbol of {fmt(addr)} in the .o{hint}: is the function inline/not emitted, "
                           f"or does the marker name not match the definition?")
        return res
    res["symbol"] = sym.name
    resolver = Resolver(image, funcs, tu.markers, tu.dem)

    # -- exact match
    got_vals = {}
    for s, e in ((0x7104270878, 0x7104270878 + 0x1998), (0x7104272210, 0x7104272210 + 0x172B0)):
        for a in range(s, e, 8):
            got_vals.setdefault(image.u64(a), a)
    orig = image.read(addr, f.size)
    ours, problems = relocate_function_at(tu.obj, sym, addr, resolver, got_lookup=got_vals.get, orig=orig,
                                          orig_read=lambda a, n: image.read(a, n) if image.contains(a, n) else b"")
    if not problems and ours == orig:
        res["quality"] = "O"
        res["time"] = time.time() - t0
        return res
    res["exact"] = (f"size {len(ours)} vs {f.size}" if len(ours) != f.size else
                    f"{sum(1 for i in range(0, len(ours), 4) if ours[i:i + 4] != orig[i:i + 4])} differing instructions")
    if problems and args.verbose:
        res["msgs"].extend("match: " + p for p in problems[:5])

    # -- equivalence
    prefer = lambda n: resolver.marker_for(n) is not None  # noqa: E731
    try:
        ln = link(tu.obj, SCRATCH, resolver, prefer_original=prefer)
    except LinkError as e:
        res["msgs"].append(f"link: {e}")
        return res
    if ln.unresolved:
        for n in ln.unresolved[:10]:
            res["msgs"].append(f"unresolved symbol: {n}  [{tu.dem.get(n, n)}]  -> declare the function/data with "
                               f"name FUN_/DAT_<addr> or put a // 0x71... marker above its declaration")
        return res
    machine.load_scratch(bytes(ln.blob))
    entry = ln.sym_addr[sym.name]
    # executed locally: our function and the TU helpers that are not game functions
    own_ours = [(ln.sym_addr[s.name], ln.sym_addr[s.name] + max(s.size, 4)) for s in tu.obj.functions()
                if s.name in ln.sym_addr and (s.name == sym.name or s.name not in ln.bindings)]
    own_orig = [(f.addr, f.end)]
    marker = next((m for m in tu.markers.by_addr.get(addr, []) if m.is_def), None) or         next((m for m in tu.markers.by_addr.get(addr, []) if m.kind == "func"), None)
    rk = ret_kind(marker) if marker else ("x", 64)
    snaps = []
    sd = SNAPSHOT_DIR / f"{addr:x}"
    if sd.exists():
        for p in sorted(sd.glob("*.json")):
            j = json.loads(p.read_text(encoding="utf-8"))
            j["_file"] = p.name
            snaps.append(j)
    n_rset = args.ret_set if args.ret_set is not None else 0
    n_glob = args.seed_globals if args.seed_globals is not None else 0
    gpages = referenced_data_pages(image, f.addr, f.size) if n_glob else []
    cases = make_cases(addr, args.cases, snaps, ret_set=n_rset, sret=args.sret, global_pages=gpages,
                       n_globals=n_glob if gpages else 0, image=image, follow=bool(args.follow))
    n_base = args.cases + len(snaps)
    oracle = ArityOracle(image, funcs)
    machine.oracle = oracle
    machine.follow_skip = f.addr
    cmp = Comparator(machine, image, ln, funcs, oracle)
    covered = set()
    fails, inconcl, both_fault = [], 0, 0
    for c in cases:
        ra = machine.run(f.addr, own_orig, c)
        for a, s in ra.blocks:
            covered.update(range(a, a + s, 4))
        rb = machine.run(entry, own_ours, c)
        d = cmp.compare(ra, rb, rk)
        if d and d[0].startswith("__inconclusive__"):
            inconcl += 1
            continue
        if ra.fault and rb.fault:
            both_fault += 1
        if d:
            fails.append((c, d, ra, rb))
    n_ok = len(cases) - len(fails) - inconcl
    cov = 100.0 * len([a for a in covered if f.addr <= a < f.end]) / max(1, f.size // 4)
    res["coverage"] = f"{cov:.0f}"
    res["cases"] = f"{n_ok}/{len(cases)}"
    n_clean = n_ok - both_fault
    # thresholds over the basic cases: the extra cases (--ret-set, --seed-globals) can only add
    need_ok, need_clean = max(4, n_base // 4), max(4, n_base // 8)
    if not fails and n_ok >= need_ok and n_clean < need_clean:
        res["msgs"].append(f"inconclusive: only {n_clean} cases finish without a memory fault; "
                           f"a real snapshot or more cases are needed")
    if not fails and n_ok >= need_ok and n_clean >= need_clean:
        res["quality"] = "F"
        if both_fault:
            res["msgs"].append(f"note: {both_fault} cases end in the same memory fault in both")
        if cov < 60:
            res["msgs"].append(f"warning: low coverage ({cov:.0f} %): verification is weak; consider a larger --cases "
                               f"or real snapshots")
    else:
        if not fails:
            if inconcl:
                res["msgs"].append(f"inconclusive: {inconcl} cases exceed the instruction limit")
        else:
            c, d, ra, rb = fails[0]
            res["msgs"].append(f"{len(fails)}/{len(cases)} cases differ. First case {c.name}:")
            res["msgs"].extend("  " + x for x in d[:12])
            if args.verbose:
                res["msgs"].append("  inputs: " + ", ".join(f"x{i}={c.regs.get(i, 0):#x}" for i in range(8)))
                res["msgs"].append(f"  original calls: {[cmp.lbl(x.target) for x in ra.calls][:10]}")
                res["msgs"].append(f"  our calls:       {[cmp.lbl(x.target) for x in rb.calls][:10]}")
    res["time"] = time.time() - t0
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="*", help="addresses / FUN_xxx / names")
    ap.add_argument("--file", help="TU to compile (default: the one containing the function's marker)")
    ap.add_argument("--cases", type=int, default=48, help="random cases per function (48)")
    ap.add_argument("--ret-set", type=int, nargs="?", const=-1, default=None, metavar="N",
                    help="add N cases with returns from a small set + sret (default N = --cases)")
    ap.add_argument("--sret", action="store_true", help="add the basic cases with the calls' sret (x8) filled")
    ap.add_argument("--seed-globals", type=int, nargs="?", const=-1, default=None, metavar="N",
                    help="add N cases with the referenced .data/.bss seeded (default N = --cases/2)")
    ap.add_argument("--follow", action="store_true",
                    help="execute calls to already-verified main functions (O/F) instead of intercepting them")
    ap.add_argument("--deep", action="store_true", help="= --ret-set --seed-globals --follow --sret")
    ap.add_argument("--agent", help="agent name (or PLA_AGENT variable)")
    ap.add_argument("--no-status", action="store_true", help="do not touch data/decomp_status.csv")
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    if not args.targets and not args.file:
        ap.error("give addresses or --file")
    if args.deep:
        args.ret_set = -1 if args.ret_set is None else args.ret_set
        args.seed_globals = -1 if args.seed_globals is None else args.seed_globals
        args.follow = args.sret = True
    if args.ret_set == -1:
        args.ret_set = args.cases
    if args.seed_globals == -1:
        args.seed_globals = max(4, args.cases // 2)
    funcs = functions()
    markers = MarkerIndex()
    image = load_image()

    # group by file
    jobs: dict[Path, list[int]] = {}
    if args.file:
        src = (ROOT / args.file).resolve() if not Path(args.file).is_absolute() else Path(args.file)
        rel = src.relative_to(ROOT).as_posix()
        addrs = []
        for t in args.targets:
            fn = funcs.resolve(t)
            addrs.append(fn.addr if fn else parse_addr(t))
        jobs[src] = addrs or None   # None: all of the TU's (tu_addresses, after compiling)
    else:
        for t in args.targets:
            fn = funcs.resolve(t)
            a = fn.addr if fn else parse_addr(t)
            defs = [m for m in markers.by_addr.get(a, []) if m.is_def and m.file.startswith("src/")]
            if not defs:
                print(f"{fmt(a)}: no definition with marker // {fmt(a)} in src/", file=sys.stderr)
                return 2
            jobs.setdefault(ROOT / defs[0].file, []).append(a)

    machine = Machine(image, funcs.label)
    if args.follow:
        rows = st.read()
        machine.follow = {a: funcs.at(a).end for a, r in rows.items()
                          if r.get("quality") in ("O", "F") and funcs.at(a) is not None}
    results = []
    rc = 0
    for src, addrs in jobs.items():
        tu = TU(src, markers, args.verbose)
        explicit = addrs is not None
        if addrs is None:
            rel = src.relative_to(ROOT).as_posix()
            addrs = tu_addresses(tu) if tu.obj is not None else \
                sorted({m.addr for m in markers.markers if m.file == rel and m.is_def and m.kind == "func"})
        if tu.obj is None:
            print(f"compilation ERROR in {src}:\n{tu.diag}", file=sys.stderr)
            if not args.no_status:
                cur_rows = st.read()
                ups = []
                for a in addrs:
                    f = funcs.at(a)
                    cur = cur_rows.get(a, {})
                    if st.RANK.get(cur.get("quality", "U"), 0) <= 1:
                        ups.append((a, f.size if f else None,
                                    dict(quality="W", notes="does not compile", file=src.relative_to(ROOT).as_posix())))
                st.update_many(ups)
            rc = 2
            continue
        try:
            tu_results = []
            for a in addrs:
                r = verify_one(a, tu, args, image, funcs, machine)
                results.append(r)
                tu_results.append(r)
                if r["quality"] not in ("O", "F"):
                    rc = max(rc, 1)
            if not args.no_status and tu_results:
                # A single transaction per TU (one lock and one write of each CSV): with several agents
                # at once, writing the whole CSV for every function multiplied the contention.
                cur_rows = st.read()
                ups, syms = [], []
                for r in tu_results:
                    a = r["addr"]
                    if not r["symbol"] and not explicit:
                        continue   # no definition in this .o: its status is left alone (it may belong to another TU)
                    f = funcs.at(a)
                    note = "" if r["quality"] in ("O", "F") else (r["msgs"][0] if r["msgs"] else "fails")[:200]
                    cur = cur_rows.get(a, {})
                    if r["quality"] == "W" and cur.get("quality") in ("O", "F"):
                        note = f"REGRESSION (was {cur['quality']}): " + note
                    ups.append((a, f.size if f else None,
                                dict(quality=r["quality"], agent=agent_name(args.agent), file=r["file"],
                                     symbol=r["symbol"], coverage=r["coverage"], cases=r["cases"], notes=note,
                                     claimed_at="" if r["quality"] in ("O", "F") else None)))
                    if r["quality"] in ("O", "F") and r["symbol"]:
                        syms.append((a, r["symbol"], tu.dem.get(r["symbol"], r["symbol"]), r["file"]))
                st.update_many(ups)
                st.register_symbols(syms)
        finally:
            tu.cleanup()
        if tu.diag.strip() and args.verbose:
            print(tu.diag, file=sys.stderr)

    if args.json:
        print(json.dumps([{k: (fmt(v) if k == "addr" else v) for k, v in r.items()} for r in results], indent=1,
                         ensure_ascii=False))
        return rc
    for r in results:
        q = r["quality"]
        tag = {"O": "O  (matching)", "F": "F  (functional)"}.get(q, "FAIL")
        extra = f"  coverage {r['coverage']}%  cases {r['cases']}" if r["coverage"] else ""
        ex = f"  [match: {r['exact']}]" if r.get("exact") and q != "O" else ""
        print(f"{fmt(r['addr'])} {r['name']}: {tag}{extra}{ex}  {r['time']:.2f}s")
        for m in r["msgs"]:
            print("    " + m)
    return rc


if __name__ == "__main__":
    sys.exit(main())
