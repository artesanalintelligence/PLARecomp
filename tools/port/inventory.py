#!/usr/bin/env python3
"""Inventory of the platform boundary of `main` (game 1.1.1) to size the port.

Reads local artifacts only (nothing from the binary is published except addresses and SDK
symbol names, like the CSVs already in the repo):

  data/main.elf                    imports (.dynsym), PLT stubs and code (.text)
  data/main_functions.csv          function bounds (to count distinct callers)
  data/exefs/{sdk,subsdk0,subsdk1} SDK modules (converted to ELF in build/port/)
  tools/ghidra/out/decomp/<addr>.c pseudo-C of the NVN loaders (name of each slot)

What it measures:
  (a) each import of main: family/service, PLT stub, call sites (BL and B to the stub),
      distinct caller functions, direct references to its GOT slot
      (pointer taken or data) and which SDK module defines it (with its offset);
  (b) the NVN API: the game loads the 523 pointers with nvnDeviceGetProcAddress into global
      tables; reads (ADRP + LDR) of each slot outside the loaders are counted;
  (c) what each SDK module exports and how much of it main imports.

Outputs:
  docs/port/data/imports.csv, docs/port/data/nvn_api.csv, docs/port/data/sdk_exports.csv
  tools/port/nnshim/generated/plt_imports.inc   (X-macro: PLT and SDK offsets per import)
  stdout: Markdown tables for docs/port/INVENTORY.md (with --md they are written to that file
          between the <!-- inventory:begin --> and <!-- inventory:end --> markers)

Usage:
  python tools/port/inventory.py [--md]
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "analysis"))
from elfimage import ElfImage  # noqa: E402
from demangle import demangle_many  # noqa: E402

BASE = 0x71_0000_0000
SDK_MODULES = ["sdk", "subsdk0", "subsdk1"]
NVN_LOADERS = [0x7102A8E180, 0x7100778570]   # FUN_s that fill the tables with GetProcAddress
NVN_BOOTSTRAP_USER = 0x7102A8B6BC             # calls nvnBootstrapLoader
OUT_DATA = ROOT / "docs" / "port" / "data"
OUT_INC = ROOT / "tools" / "port" / "nnshim" / "generated" / "plt_imports.inc"
CACHE = ROOT / "build" / "port"

# ---------------------------------------------------------------- classification
# (family, regex, plan). Order matters: the first matching pattern wins.
FAMILY_RULES = [
    ("hid",       r"^nn::hid::",                                 "native (SDL2/XInput)"),
    ("fs",        r"^nn::fs::",                                  "native (host folders)"),
    ("time",      r"^nn::time::",                                "native (host clock)"),
    ("settings",  r"^nn::settings::",                            "native (constants/config)"),
    ("applet",    r"^nn::(oe|ae|applet|apm|pctl)::",             "native (constants + focus/window)"),
    ("account",   r"^nn::account::",                             "native (fixed local user)"),
    ("err",       r"^(nn::(err|diag|result)::|nn::Result|nnResult|nndetailDiag)", "native (log/dialog)"),
    ("swkbd",     r"^nn::swkbd::",                               "native (SDL text input)"),
    ("font",      r"^nn::(pl|fontll)::",                         "pl: native (host font); fontll: recompiled"),
    ("vi",        r"^nn::vi::",                                  "HLE in v1, native (SDL window) in v2"),
    ("audio",     r"^nn::(audio|codec|atk)::",                   "HLE in v1, native (XAudio2/SDL) in v1.5"),
    ("os",        r"^(nn::os::|nnos|pthread_|sched_|sem_)",      "HLE in v1 (HLE kernel threads); native with own runtime"),
    ("net-local", r"^nn::ldn::",                                 "stub: no local communication"),
    ("net",       r"^(nn::(nifm|socket|ssl|nsd|bcat|friends|prepo|npns|nex|aoc|ec|news|olsc)::|nnsocket|nnssl)",
                                                                 "stub: offline (nifm unavailable)"),
    ("mem/util",  r"^nn::(mem|lmem|util|crypto|ngc|detail)::",  "recompiled (user-mode library, no IPC)"),
    ("nvn",       r"^nvn",                                       "v2: NVN→Vulkan"),
    ("glslc",     r"^glslc",                                     "v2: unused in production (subsdk1 GLSL compiler)"),
    ("c++rt",     r"^(std::|__cxa|_Unwind|__gxx|typeinfo|vtable|virtual thunk|non-virtual thunk|operator new|operator delete|TLS init|void std::)",
                                                                 "recompiled (SDK libc++) or host libc++"),
    ("link",      r"^(__rel_|__nnmusl_init_dso)",                "no code (linker symbols)"),
    ("crt",       r".*",                                         "recompiled (SDK musl) or host CRT"),
]
_FAMILY_RE = [(f, re.compile(p), plan) for f, p, plan in FAMILY_RULES]


def family_of(name: str) -> tuple[str, str]:
    for fam, rx, plan in _FAMILY_RE:
        if rx.search(name):
            return fam, plan
    return "crt", ""


def subservice(dname: str) -> str:
    m = re.match(r"nn::(\w+)::", dname)
    if m:
        return m.group(1)
    for p in ("nnsocket", "nnssl", "nnos", "nnResult", "nndetailDiag", "pthread", "nvn", "glslc"):
        if dname.startswith(p):
            return p
    return ""


def short(dname: str) -> str:
    """Name without parameters (to group overloads)."""
    depth, out = 0, []
    for ch in dname:
        if ch == "(" and depth == 0 and out and not "".join(out).endswith("operator"):
            break
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        out.append(ch)
    return "".join(out)


# ---------------------------------------------------------------- code helpers
def code_words(im: ElfImage):
    """(absolute pcs, words) of all executable code."""
    pcs, words = [], []
    for lo, hi in im.code_ranges:
        lo4 = (lo + 3) & ~3
        n = (hi - lo4) // 4
        w = np.frombuffer(im.read(lo4, n * 4), dtype="<u4")
        words.append(w)
        pcs.append(np.arange(n, dtype=np.int64) * 4 + im.to_abs(lo4))
    return np.concatenate(pcs), np.concatenate(words)


def branch_targets(pcs, w):
    """(pc, target, is_BL) of every immediate B/BL."""
    op = w & 0xFC000000
    sel = (op == 0x94000000) | (op == 0x14000000)
    imm = (w[sel] & 0x03FFFFFF).astype(np.int64)
    imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
    return pcs[sel], pcs[sel] + imm * 4, op[sel] == 0x94000000


def adrp_ldr_refs(pcs, w, window: int = 32):
    """(ldr_pc, address) of each LDR Xt, [xB, #imm] whose base xB comes from
    ADRP xB (or from ADRP xR + ADD xB, xR, #imm12).

    The base is tracked while no instruction writes xB (Rd/Rt field = B) and the block
    does not end; it is a linear approximation without control flow, so a base that lives
    in a callee-saved register across the whole function is only counted in its first
    stretch (totals are a lower bound)."""
    n = len(w)
    is_adrp = (w & 0x9F000000) == 0x90000000
    rdf = (w & 31).astype(np.int64)
    immlo = ((w >> 29) & 3).astype(np.int64)
    immhi = ((w >> 5) & 0x7FFFF).astype(np.int64)
    imm = (immhi << 2) | immlo
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = (pcs & ~0xFFF) + (imm << 12)
    is_ldr = (w & 0xFFC00000) == 0xF9400000                 # LDR Xt, [Xn, #imm12*8]
    is_addi = (w & 0xFFC00000) == 0x91000000                # ADD Xd, Xn, #imm12 (unshifted)
    rn = ((w >> 5) & 31).astype(np.int64)
    off = ((w >> 10) & 0xFFF).astype(np.int64) * 8
    addimm = ((w >> 10) & 0xFFF).astype(np.int64)
    op = w & 0xFC000000
    is_branch = ((op == 0x14000000) | (op == 0x94000000) | ((w & 0xFF000010) == 0x54000000)
                 | ((w & 0x7E000000) == 0x34000000) | ((w & 0x7E000000) == 0x36000000)
                 | ((w & 0xFE000000) == 0xD6000000))
    is_store = (w & 0x3A400000) == 0x38000000  # LDR/STR family with L=0 (approx.)

    def follow(start, reg, base, collect_adds):
        keep = start + window < n
        start, reg, base = start[keep], reg[keep], base[keep]
        alive = np.ones(len(start), dtype=bool)
        hit_pc, hit_addr, add_i, add_r, add_b = [], [], [], [], []
        for k in range(1, window + 1):
            j = start + k
            ok = alive & is_ldr[j] & (rn[j] == reg)
            hit_pc.append(pcs[j[ok]])
            hit_addr.append(base[ok] + off[j[ok]])
            if collect_adds:
                ad = alive & is_addi[j] & (rn[j] == reg)
                add_i.append(j[ad]); add_r.append(rdf[j[ad]]); add_b.append(base[ad] + addimm[j[ad]])
            alive &= ~(((w[j] & 31) == reg) & ~is_store[j]) & ~is_branch[j]
        out = (np.concatenate(hit_pc), np.concatenate(hit_addr))
        if collect_adds:
            out += (np.concatenate(add_i), np.concatenate(add_r), np.concatenate(add_b))
        return out

    ai = np.nonzero(is_adrp)[0]
    p1, a1, ji, jr, jb = follow(ai, rdf[ai], page[ai], True)
    p2, a2 = follow(ji, jr, jb, False)
    pc = np.concatenate([p1, p2])
    addr = np.concatenate([a1, a2])
    # the same LDR may match two different bases; keep one per LDR
    _, uniq = np.unique(pc, return_index=True)
    return pc[uniq], addr[uniq]


def load_function_starts():
    starts = []
    with open(ROOT / "data" / "main_functions.csv", newline="") as f:
        for row in csv.DictReader(f):
            starts.append(int(row["Address"], 16))
    starts.sort()
    return np.array(starts, dtype=np.int64)


def ensure_sdk_elf(mod: str) -> Path:
    out = CACHE / f"{mod}.elf"
    if not out.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        subprocess.run([sys.executable, str(ROOT / "tools" / "extract" / "nso2elf.py"),
                        str(ROOT / "data" / "exefs" / mod), str(out)], check=True,
                       stdout=subprocess.DEVNULL)
    return out


# ---------------------------------------------------------------- NVN
_SLOT_RE = re.compile(r'DAT_([0-9a-f]+)\s*=\s*\(\*param_2\)\(param_1,"(nvn\w+)"\)')
NVN_VERBS = {
    "Set", "Get", "Initialize", "Finalize", "Bind", "Draw", "Dispatch", "Copy", "Clear", "Submit",
    "Flush", "Finish", "Present", "Acquire", "Map", "Invalidate", "Begin", "End", "Push", "Insert",
    "Add", "Wait", "Reset", "Signal", "Register", "Install", "Generate", "Compute", "Save",
    "Restore", "Discard", "Resolve", "Downsample", "Barrier", "Report", "Update", "Call", "Apply",
    "Is", "Has", "Write", "Read", "Load", "Store", "Tiled", "Fence", "Enable", "Disable",
    "Render", "Change", "Inherit", "Free", "Query", "Replace", "Bootstrap", "Lock", "Unlock",
    "Multi", "Pop", "Pause", "Resume", "Mark", "Wrap", "Set",
}


def nvn_object(name: str) -> str:
    toks = re.findall(r"[A-Z][a-z0-9]*|[a-z0-9]+", name[3:])
    obj = []
    for t in toks:
        if t in NVN_VERBS:
            break
        obj.append(t)
    return "".join(obj) or name[3:]


def nvn_slots():
    """{slot_address: name} of the two NVN pointer tables."""
    slots = {}
    for fn in NVN_LOADERS:
        p = ROOT / "tools" / "ghidra" / "out" / "decomp" / f"{fn:x}.c"
        if not p.exists():
            print(f"warning: missing {p} (NVN loader pseudo-C)", file=sys.stderr)
            continue
        for addr, name in _SLOT_RE.findall(p.read_text(encoding="utf-8", errors="replace")):
            slots[int(addr, 16)] = (name, fn)
    return slots


def nvn_decomp_refs(slots, rescan: bool) -> dict[int, int]:
    """Mentions of each NVN slot in Ghidra's pseudo-C (all functions), minus the
    loader's assignment. Scanning the ~160,000 functions takes a few minutes: the
    result is cached in build/port/nvn_decomp_refs.txt ("<count> DAT_<addr>")."""
    cache = CACHE / "nvn_decomp_refs.txt"
    if rescan or not cache.exists():
        rx = re.compile(r"DAT_71042(?:a5|a6|f0|f1)[0-9a-f]{3}")
        cnt = collections.Counter()
        d = ROOT / "tools" / "ghidra" / "out" / "decomp"
        for p in d.glob("*.c"):
            cnt.update(rx.findall(p.read_text(encoding="utf-8", errors="replace")))
        CACHE.mkdir(parents=True, exist_ok=True)
        cache.write_text("".join(f"{v} {k}\n" for k, v in cnt.most_common()), encoding="utf-8")
    out = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        n, name = line.split()
        a = int(name[4:], 16)
        if a in slots:
            out[a] = max(int(n) - 1, 0)
    return out


# ---------------------------------------------------------------- main
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--md", action="store_true", help="rewrite the generated section of docs/port/INVENTORY.md")
    ap.add_argument("--decomp-refs", action="store_true",
                    help="also count the mentions of each NVN slot in tools/ghidra/out/decomp "
                         "(uses the build/port cache if present; --rescan regenerates it)")
    ap.add_argument("--rescan", action="store_true")
    args = ap.parse_args(argv)

    im = ElfImage(ROOT / "data" / "main.elf")
    pcs, w = code_words(im)
    fstarts = load_function_starts()

    def func_of(pc_arr):
        i = np.searchsorted(fstarts, pc_arr, side="right") - 1
        return np.where(i >= 0, fstarts[np.clip(i, 0, None)], 0)

    # --- imports and PLT stubs
    imports = sorted({s.name for s in im.symbols if s.dynamic and not s.defined})
    plt = {im.to_abs(va): name for va, name in im.plt_stubs().items()}
    plt_by_name = collections.defaultdict(list)
    for a, n in plt.items():
        plt_by_name[n].append(a)
    got_slots = collections.defaultdict(list)  # name -> GOT slots (abs)
    for off, p in im.pointers.items():
        if p.import_name:
            got_slots[p.import_name].append(im.to_abs(off))

    bpc, btgt, bl = branch_targets(pcs, w)
    plt_arr = np.array(sorted(plt), dtype=np.int64)
    hit = np.isin(btgt, plt_arr)
    cs_count = collections.Counter(btgt[hit].tolist())
    callers = collections.defaultdict(set)
    for f, t in zip(func_of(bpc[hit]).tolist(), btgt[hit].tolist()):
        callers[t].add(f)

    lpc, laddr = adrp_ldr_refs(pcs, w)
    plt_set = set(plt)
    # LDRs of GOT slots outside the PLT stubs themselves = pointer taken or imported data
    slot_to_name = {s: n for n, ss in got_slots.items() for s in ss}
    got_arr = np.array(sorted(slot_to_name), dtype=np.int64)
    gsel = np.isin(laddr, got_arr)
    got_refs = collections.Counter()
    for pc, a in zip(lpc[gsel].tolist(), laddr[gsel].tolist()):
        if (pc - 4) not in plt_set:          # the stub's LDR is at stub+4
            got_refs[slot_to_name[a]] += 1

    # --- SDK: exports and definition of each import
    sdk_defs = {}
    sdk_summary = {}
    for mod in SDK_MODULES:
        sim = ElfImage(ensure_sdk_elf(mod))
        funcs = [s for s in sim.defined_symbols() if s.dynamic]
        sdk_summary[mod] = funcs
        for s in funcs:
            sdk_defs.setdefault(s.name, (mod, s.va, s.type))

    dm = demangle_many(imports + [s.name for fs in sdk_summary.values() for s in fs if s.name.startswith("_Z")])

    rows = []
    for name in imports:
        d = dm.get(name) or name
        fam, plan = family_of(d)
        stubs = sorted(plt_by_name.get(name, []))
        cs = sum(cs_count[s] for s in stubs)
        ncall = len(set().union(*[callers[s] for s in stubs])) if stubs else 0
        mod, sva, stype = sdk_defs.get(name, ("", None, ""))
        rows.append(dict(
            family=fam, service=subservice(d), name=d, mangled=name,
            plt=";".join(f"0x{s - BASE:x}" for s in stubs),
            callsites=cs, callers=ncall, got_refs=got_refs.get(name, 0),
            sdk_module=mod, sdk_offset=(f"0x{sva:x}" if sva is not None else ""),
            sdk_type=stype.replace("STT_", "").lower(), v1_plan=plan))

    OUT_DATA.mkdir(parents=True, exist_ok=True)
    with open(OUT_DATA / "imports.csv", "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0]))
        wr.writeheader()
        wr.writerows(sorted(rows, key=lambda r: (r["family"], r["name"])))

    # --- X-macro for the shim
    OUT_INC.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT_INC, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Generated by tools/port/inventory.py: do not edit by hand.\n"
                " * NN_IMPORT(family, main_plt_offset, sdk_module, sdk_offset, callsites, \"mangled\")\n"
                " * main_plt_offset: offset of the PLT stub in main (0 if the import has no stub).\n"
                " * sdk_offset: offset of the definition in its module (0 if not in sdk/subsdk). */\n")
        for r in sorted(rows, key=lambda r: (r["family"], r["mangled"])):
            if r["family"] in ("link",):
                continue
            for p in (r["plt"].split(";") if r["plt"] else ["0x0"]):
                f.write(f'NN_IMPORT({r["family"].replace("/", "_").replace("-", "_").replace("+", "x")}, {p}, '
                        f'{r["sdk_module"] or "none"}, {r["sdk_offset"] or "0x0"}, {r["callsites"]}, '
                        f'"{r["mangled"]}")\n')

    # --- NVN
    slots = nvn_slots()
    slot_arr = np.array(sorted(slots), dtype=np.int64)
    nsel = np.isin(laddr, slot_arr)
    nvn_pc, nvn_addr = lpc[nsel], laddr[nsel]
    nvn_fn = func_of(nvn_pc)
    loaders = set(NVN_LOADERS)
    nvn_cs = collections.Counter()
    nvn_callers = collections.defaultdict(set)
    for fn, a in zip(nvn_fn.tolist(), nvn_addr.tolist()):
        if fn in loaders:
            continue
        nvn_cs[a] += 1
        nvn_callers[a].add(fn)
    # who calls each loader
    loader_callers = {fn: sorted(set(func_of(bpc[btgt == fn]).tolist())) for fn in NVN_LOADERS + [NVN_BOOTSTRAP_USER]}

    dref = nvn_decomp_refs(slots, args.rescan) if args.decomp_refs else {}
    nvn_rows = []
    by_name = collections.defaultdict(lambda: {"tables": 0, "callsites": 0, "callers": set(), "decomp": 0})
    for a, (name, fn) in slots.items():
        e = by_name[name]
        e["tables"] += 1
        e["callsites"] += nvn_cs[a]
        e["decomp"] += dref.get(a, 0)
        e["callers"] |= nvn_callers[a]
        e.setdefault("slots", []).append(f"0x{a - BASE:x}@{'A' if fn == NVN_LOADERS[0] else 'B'}:{nvn_cs[a]}")
    for name, e in by_name.items():
        nvn_rows.append(dict(name=name, object=nvn_object(name), callsites=e["callsites"],
                             callers=len(e["callers"]), decomp_refs=e["decomp"] if dref else "",
                             slots=" ".join(e["slots"])))
    nvn_rows.sort(key=lambda r: (r["object"], r["name"]))
    with open(OUT_DATA / "nvn_api.csv", "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=list(nvn_rows[0]))
        wr.writeheader()
        wr.writerows(nvn_rows)

    # --- SDK exports summary
    used_names = set(imports)
    sdk_rows = []
    for mod, funcs in sdk_summary.items():
        c = collections.Counter()
        cu = collections.Counter()
        for s in funcs:
            if s.type != "STT_FUNC":
                continue
            d = dm.get(s.name) or s.name
            key = subservice(d) or ("android" if "android::" in d else family_of(d)[0])
            c[key] += 1
            if s.name in used_names:
                cu[key] += 1
        for k, v in c.items():
            sdk_rows.append(dict(module=mod, namespace=k, exported_funcs=v, used_by_main=cu[k]))
    sdk_rows.sort(key=lambda r: (r["module"], -r["exported_funcs"]))
    with open(OUT_DATA / "sdk_exports.csv", "w", newline="", encoding="utf-8") as f:
        wr = csv.DictWriter(f, fieldnames=list(sdk_rows[0]))
        wr.writeheader()
        wr.writerows(sdk_rows)

    # IPC interfaces the sdk can use (CMIF proxies)
    ipc = set()
    for s in sdk_summary["sdk"]:
        d = dm.get(s.name) or ""
        for m in re.finditer(r"CmifProxyImpl<(nn::[\w:]+?)(?:,|<)", d):
            ipc.add(m.group(1))

    md = render_md(rows, nvn_rows, sdk_rows, sdk_summary, used_names, ipc, loader_callers, slots)
    if args.md:
        path = ROOT / "docs" / "port" / "INVENTORY.md"
        text = path.read_text(encoding="utf-8") if path.exists() else "<!-- inventory:begin -->\n<!-- inventory:end -->\n"
        b, e = "<!-- inventory:begin -->", "<!-- inventory:end -->"
        pre, rest = text.split(b, 1)
        _, post = rest.split(e, 1)
        path.write_text(pre + b + "\n" + md + "\n" + e + post, encoding="utf-8", newline="\n")
        print(f"wrote {path}")
    else:
        print(md)
    return 0


def render_md(rows, nvn_rows, sdk_rows, sdk_summary, used_names, ipc, loader_callers, slots) -> str:
    L = []
    fam = collections.OrderedDict()
    for r in rows:
        e = fam.setdefault(r["family"], {"imports": 0, "callsites": 0, "got": 0, "services": collections.Counter(),
                                         "plan": r["v1_plan"], "nostub": 0})
        e["imports"] += 1
        e["callsites"] += r["callsites"]
        e["got"] += r["got_refs"]
        e["services"][r["service"] or "-"] += 1
        if not r["plt"]:
            e["nostub"] += 1
    tot_i = sum(e["imports"] for e in fam.values())
    tot_c = sum(e["callsites"] for e in fam.values())
    L.append(f"### (a) main imports by family\n")
    L.append(f"Total: **{tot_i} imports** ({sum(1 for r in rows if r['plt'])} with a PLT stub), "
             f"**{tot_c} direct call sites** (BL/B to the stub). `GOT` = direct reads of the slot "
             f"(function pointer taken or imported data).\n")
    L.append("| Family | Imports | Call sites | GOT | Sub-services (imports) | v1 plan |")
    L.append("|---|---:|---:|---:|---|---|")
    for k, e in sorted(fam.items(), key=lambda kv: -kv[1]["callsites"]):
        svc = ", ".join(f"{s} {n}" for s, n in e["services"].most_common(8) if s != "-")
        L.append(f"| {k} | {e['imports']} | {e['callsites']} | {e['got']} | {svc} | {e['plan']} |")
    L.append("")

    # details of the families that matter for v1
    for f in ("hid", "fs", "time", "settings", "applet", "account", "err", "swkbd", "font", "vi", "audio", "os",
              "net-local", "net"):
        rs = [r for r in rows if r["family"] == f]
        if not rs:
            continue
        L.append(f"<details><summary><b>{f}</b>: {len(rs)} imports, {sum(r['callsites'] for r in rs)} call sites</summary>\n")
        L.append("| Function | PLT (main+) | Call sites | Callers | Defined in |")
        L.append("|---|---|---:|---:|---|")
        for r in sorted(rs, key=lambda r: -r["callsites"]):
            L.append(f"| `{r['name']}` | {r['plt'] or '-'} | {r['callsites']} | {r['callers']} | "
                     f"{r['sdk_module']}{'+' + r['sdk_offset'] if r['sdk_offset'] else ''} |")
        L.append("\n</details>\n")

    # (b) NVN
    has_dec = any(r["decomp_refs"] != "" for r in nvn_rows)
    used_cs = [r for r in nvn_rows if r["callsites"] > 0]
    used = [r for r in nvn_rows if r["callsites"] > 0 or (has_dec and r["decomp_refs"] > 0)]
    L.append("### (b) NVN API\n")
    L.append(f"- `nvn*` names loaded via `nvnDeviceGetProcAddress`: **{len(nvn_rows)}** "
             f"(two tables of {len(slots) // 2} slots: A = `FUN_7102a8e180` → `main+0x42f0030…`, "
             f"B = `FUN_7100778570` → `main+0x42a5ce8…`). It is the full generic loader: it loads everything, used or not.")
    L.append(f"- NVN functions whose slot is read in machine code (ADRP+LDR or ADRP+ADD+LDR, outside "
             f"the loaders): **{len(used_cs)}**, with **{sum(r['callsites'] for r in used_cs)}** call sites "
             f"(lower bound: a base living in a callee-saved register is not tracked).")
    if has_dec:
        L.append(f"- Also counting mentions in Ghidra's pseudo-C: **{len(used)} functions used**, "
                 f"{sum(r['decomp_refs'] for r in nvn_rows)} mentions (practical upper bound).")
    for fn, cl in loader_callers.items():
        L.append(f"- Callers of `FUN_{fn:x}`: {', '.join(f'`FUN_{c:x}`' for c in cl) or '(indirect only)'}")
    L.append("- Direct import: only `nvnBootstrapLoader` (from `sdk`); everything else goes through the tables.\n")
    objs = collections.OrderedDict()
    for r in nvn_rows:
        e = objs.setdefault(r["object"], {"total": 0, "used": 0, "cs": 0, "dec": 0})
        e["total"] += 1
        if r in used:
            e["used"] += 1
        e["cs"] += r["callsites"]
        e["dec"] += r["decomp_refs"] or 0
    L.append("| NVN object | Functions in table | Used | Call sites (code) | Mentions (pseudo-C) |")
    L.append("|---|---:|---:|---:|---:|")
    for k, e in sorted(objs.items(), key=lambda kv: (-kv[1]["used"], -kv[1]["total"])):
        L.append(f"| {k} | {e['total']} | {e['used']} | {e['cs']} | {e['dec'] if has_dec else '-'} |")
    L.append("")
    L.append("<details><summary>NVN functions used</summary>\n")
    L.append("| Function | Object | Call sites (code) | Caller functions | Mentions (pseudo-C) |")
    L.append("|---|---|---:|---:|---:|")
    for r in sorted(used, key=lambda r: (-r["callsites"], -(r["decomp_refs"] or 0), r["name"])):
        L.append(f"| `{r['name']}` | {r['object']} | {r['callsites']} | {r['callers']} | {r['decomp_refs']} |")
    L.append("\n</details>\n")

    # (c) SDK
    L.append("### (c) What the SDK exports and what main uses\n")
    L.append("| Module | Exported symbols | Functions | Imported by main |")
    L.append("|---|---:|---:|---:|")
    for mod, funcs in sdk_summary.items():
        nf = sum(1 for s in funcs if s.type == "STT_FUNC")
        nu = sum(1 for s in funcs if s.name in used_names)
        L.append(f"| {mod} | {len(funcs)} | {nf} | {nu} |")
    L.append("")
    L.append("Namespaces with the most exports (sdk) and how many main imports:\n")
    L.append("| Module | Namespace | Exported functions | Used by main |")
    L.append("|---|---|---:|---:|")
    for r in sdk_rows:
        if r["used_by_main"] or r["exported_funcs"] >= 150:
            L.append(f"| {r['module']} | {r['namespace']} | {r['exported_funcs']} | {r['used_by_main']} |")
    L.append("")
    L.append(f"IPC interfaces (CMIF proxies) that `sdk` can open: **{len(ipc)}** "
             f"(superset of what the game uses; it is what suyu's HLE serves in v0).\n")
    by_ns = collections.Counter(re.match(r"nn::(\w+)", x).group(1) for x in ipc)
    L.append(", ".join(f"{k} {v}" for k, v in by_ns.most_common()))
    return "\n".join(L)


if __name__ == "__main__":
    sys.exit(main())
