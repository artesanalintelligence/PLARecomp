#!/usr/bin/env python3
"""Builds the modules of a recompiler export into recompiled_*.dll with the portable toolchain
(clang-cl + lld-link + our own headers and import libraries), without Visual Studio, the
Windows SDK or CMake/Ninja. See docs/port/TOOLCHAIN.md.

It reproduces the official build (the export's CMakeLists.txt with clang-cl and Ninja): the
same per-unit compile flags (read from CMakeLists.txt), the same object order and the same
output DLL. The only differences are where the CRT/SDK headers come from (our include/) and
the DLL startup code (lib/recomp_crt0.obj instead of msvcrt.lib).

    python build_modules.py --export <root>\\exports\\x --out D:\\work\\build
    python build_modules.py --module-dir <export>\\...\\aot_cache\\exefs\\sdk --out <dir> --jobs 4

Fast mode (default): the big generated units (up to ~80 MB of C, ~5.4 GB of RAM per clang-cl) are
split into ~8 MB parts (split_units.py: same functions, same tables, same DLL exports) and compiled by a
memory-aware scheduler: as many compilers as logical cores, as long as their estimated memory (from
the part size, calibrated with the measured peaks) fits in the free RAM; biggest first. All modules
share one queue. --split-mb 0 --jobs 3 gives the old behaviour (whole units, fixed job count).

Resumable: every object carries a signature (compiler + size/date of the source and of the
headers + flags); a rerun only compiles the missing or changed units. With --priority low the
compilers run "below normal" (the machine stays responsive, but on hybrid CPUs Windows moves
them to the efficiency cores and the build takes ~1.8x longer).

Progress lines: "[n/m p%] unit seconds peak-GB", where p is weighted by bytes of C (what the
installer's bar shows).
"""
from __future__ import annotations

import argparse
import bisect
import ctypes
import hashlib
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import split_units  # noqa: E402  (next to this script, also in the toolchain package)
import cpu_isa  # noqa: E402

# Target ISA (TOOLCHAIN.md §8): the units are compiled on the player's machine, so they may use its CPU's
# instructions. -ffp-contract=off keeps every guest FP operation a separate IEEE operation (no a*b+c fused into
# an FMA behind the emitter's back); explicit fma() calls still become one FMA instruction, which is exact.
ISA_FLAGS = {
    "x86-64": [],
    "x86-64-v2": ["/clang:-march=x86-64-v2", "/clang:-ffp-contract=off"],
    "x86-64-v3": ["/clang:-march=x86-64-v3", "/clang:-ffp-contract=off"],
}

GB = 1024 ** 3
MB = 1024 ** 2
PRIORITIES = {"normal": 0, "low": 0x00004000}  # BELOW_NORMAL_PRIORITY_CLASS
# Flags CMake applies by default to C with clang-cl in Release (CMAKE_C_FLAGS + _RELEASE).
CMAKE_BASE = ["/DWIN32", "/D_WINDOWS", "/W3", "/MD", "/O2", "/Ob2", "/DNDEBUG", "/std:c11"]
CRT0 = "recomp_crt0.obj"
# clang-cl peak memory ~= MEM_BASE + MEM_PER_BYTE * bytes of C (measured on the generated units, /O2;
# recalibrated during the build from the measured peaks).
MEM_BASE = 0.35 * GB
MEM_PER_BYTE = 70.0      # 8 MB parts peak at 0.4-1.0 GB (median 0.5); an 80 MB unit at ~5.4 GB

k32 = ctypes.windll.kernel32


class _PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + \
               [(n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                               "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                               "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]


k32.K32GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]


def process_memory(handle) -> tuple[int, int]:
    """(working set, peak working set) of a process handle (0, 0 if unknown)."""
    m = _PMC(); m.cb = ctypes.sizeof(_PMC)
    try:
        if not k32.K32GetProcessMemoryInfo(ctypes.c_void_p(int(handle)), ctypes.byref(m), m.cb):
            return 0, 0
    except (OSError, ValueError, TypeError):
        return 0, 0
    return int(m.WorkingSetSize), int(m.PeakWorkingSetSize)


class _PowerThrottling(ctypes.Structure):
    _fields_ = [("Version", ctypes.c_ulong), ("ControlMask", ctypes.c_ulong), ("StateMask", ctypes.c_ulong)]


k32.SetProcessInformation.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong]


def high_qos(handle) -> None:
    """Opt the process out of EcoQoS (ProcessPowerThrottling, EXECUTION_SPEED off). Windows otherwise treats
    compilers started from a hidden console as background work and, on hybrid CPUs, keeps them on the
    efficiency cores: a unit took 37 s instead of 23 s here (i9-14900KF)."""
    st = _PowerThrottling(1, 1, 0)
    try:
        k32.SetProcessInformation(ctypes.c_void_p(int(handle)), 4, ctypes.byref(st), ctypes.sizeof(st))
    except (OSError, ValueError, TypeError):
        pass


def cpu_seconds(handle) -> float:
    """User + kernel CPU time of a (finished) process."""
    t = [ctypes.c_ulonglong() for _ in range(4)]
    try:
        if not k32.GetProcessTimes(ctypes.c_void_p(int(handle)), *[ctypes.byref(x) for x in t]):
            return 0.0
    except (OSError, ValueError, TypeError):
        return 0.0
    return (t[2].value + t[3].value) / 1e7


def _memory():
    class MS(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    m = MS(); m.dwLength = ctypes.sizeof(MS)
    k32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m


def ram_total() -> int:
    return int(_memory().ullTotalPhys)


def ram_free() -> int:
    return int(_memory().ullAvailPhys)


def commit_free() -> int:
    """Commit charge still available (RAM + page file). A machine with a small page file runs out of commit
    before it runs out of free RAM: clang-cl then fails to start or dies (error 1455 / 0x5AF)."""
    return int(_memory().ullAvailPageFile)


class Module:
    """What the build needs from the emitted CMakeLists.txt."""

    def __init__(self, src: Path):
        self.src = src
        cm = (src / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
        m = re.search(r"set\(RECOMP_SOURCES\s+(.*?)\)", cm, re.S)
        if not m:
            raise SystemExit(f"{src}: RECOMP_SOURCES not found in CMakeLists.txt")
        self.sources = m.group(1).split()
        m = re.search(r"add_library\((\S+)\s+SHARED\s+([^)]*)\)", cm)
        if not m:
            raise SystemExit(f"{src}: SHARED library not found in CMakeLists.txt")
        self.target = m.group(1)
        self.extra = [t for t in m.group(2).split() if not t.startswith("${")]  # recomp_export.c recomp_runtime.c
        m2 = re.search(r'set_target_properties\(%s PROPERTIES[^)]*OUTPUT_NAME "([^"]+)"' % re.escape(self.target), cm)
        self.output = (m2.group(1) if m2 else self.target) + ".dll"
        m = re.search(r"target_compile_definitions\(%s PRIVATE ([^)]*)\)" % re.escape(self.target), cm)
        defs = m.group(1).split() if m else []
        self.defines = [f"-D{d}" for d in defs] + [f"-D{self.target}_EXPORTS"]
        # Per-unit options of the if(MSVC) branch (CMake treats clang-cl as MSVC);
        # /MP only applies to non-Ninja generators.
        opts: list[str] = []
        m = re.search(r'set\(_recomp_msvc_opts ([^)]*)\)', cm)
        if m:
            opts += re.findall(r'"([^"]+)"', m.group(1))
        for a in re.finditer(r'list\(APPEND _recomp_msvc_opts ("[^)]*)\)', cm):
            opts += [o for o in re.findall(r'"([^"]+)"', a.group(1)) if not o.startswith("/MP")]
        if not opts:
            raise SystemExit(f"{src}: _recomp_msvc_opts not found in CMakeLists.txt")
        self.unit_opts = opts


def signature(*parts) -> str:
    return hashlib.sha1("\x1f".join(map(str, parts)).encode()).hexdigest()


def stamp(p: Path) -> str:
    st = p.stat()
    return f"{p.name}:{st.st_size}:{st.st_mtime_ns}"


class Job:
    __slots__ = ("src", "obj", "flags", "sig", "size", "weight", "mb", "temp")

    def __init__(self, src, obj, flags, sig, size, weight, mb, temp):
        self.src, self.obj, self.flags, self.sig = src, obj, flags, sig
        self.size, self.weight, self.mb, self.temp = size, weight, mb, temp


class ModBuild:
    """One module: its units, objects (in link order), state file."""

    def __init__(self, mod: Module, bld: Path, tc: Path, unit_flags: list[str], all_flags: list[str] = ()):
        self.mod, self.bld = mod, bld
        self.objdir = bld / "obj"
        self.objdir.mkdir(parents=True, exist_ok=True)
        self.splitdir = bld / "split"
        self.state_p = bld / "state.json"
        self.state = json.loads(self.state_p.read_text(encoding="utf-8")) if self.state_p.exists() else {}
        inc = tc / "include"
        self.headers = ";".join(sorted(stamp(p) for p in list(inc.glob("*.h")) + list(mod.src.glob("*.h"))))
        common = ["/nologo", "/X", f"-I{mod.src}", f"-I{inc}"] + mod.defines + CMAKE_BASE
        all_flags = list(all_flags)
        self.units = [(f, common + all_flags) for f in mod.extra]            # CMake: base flags only
        self.units += [(f, common + mod.unit_opts + all_flags + unit_flags) for f in mod.sources]
        self.objs_by_unit: dict[int, list[Path]] = {}
        self.failed: list[str] = []

    def save_state(self):
        tmp = self.state_p.with_suffix(".json.part")
        tmp.write_text(json.dumps(self.state, indent=0), encoding="utf-8")
        os.replace(tmp, self.state_p)


class Builder:
    def __init__(self, a, tc: Path, log):
        self.a, self.tc, self.log = a, tc, log
        self.clang = tc / "bin" / "clang-cl.exe"
        self.ver = stamp(self.clang)
        self.env = dict(os.environ)
        self.env["INCLUDE"] = ""  # keep clang-cl from picking up the CRT/SDK of an installed VS
        self.prio = PRIORITIES[a.priority]
        self.split_bytes = int(a.split_mb * MB)
        self.cond = threading.Condition()
        self.pending: list[tuple] = []        # sorted (-size, seq, job, modbuild)
        self.seq = 0
        self.running: dict[int, list] = {}     # seq -> [estimated peak bytes, Popen or None]
        self.producing = True
        self.total_w = 0
        self.done_w = 0
        self.n_total = 0
        self.n_done = 0
        self.n_compiled = 0
        self.peaks: list[float] = []           # measured (peak - base) / size
        self.mem_per_byte = MEM_PER_BYTE
        self.max_peak = 0
        self.compile_cpu = 0.0
        self.prod_error = None
        self.max_running = 0

    # ---- memory model
    def estimate(self, size: int) -> int:
        if self.a.unit_mem_gb > 0:
            return int(self.a.unit_mem_gb * GB)
        return int(MEM_BASE + self.mem_per_byte * size)

    def learn(self, size: int, peak: int):
        if peak <= 0 or size < 2 * MB:
            return
        self.peaks.append(max(0.0, (peak - MEM_BASE) / size))
        if len(self.peaks) >= 5:
            # The 90th percentile of the ratios seen so far, plus 10 % (a compile that grows past its
            # estimate shows up in the free RAM, which the admission also checks).
            r = sorted(self.peaks)
            self.mem_per_byte = r[min(len(r) - 1, int(len(r) * 0.9))] * 1.1

    # ---- producer: decides the compile items of every unit (splitting the big ones)
    def produce(self, mbs: list[ModBuild]):
        try:
            items = []
            for mb in mbs:
                for i, (rel, flags) in enumerate(mb.units):
                    srcp = mb.mod.src / rel
                    items.append((srcp.stat().st_size, i, rel, flags, srcp, mb))
            est_parts = {}
            with self.cond:
                for size, i, rel, flags, srcp, mb in items:
                    self.total_w += size
                    n = 1
                    if self.split_bytes and size >= self.split_bytes * 3 // 2:
                        n = max(2, round(size / self.split_bytes))
                    est_parts[(id(mb), i)] = n
                    self.n_total += n
            for size, i, rel, flags, srcp, mb in sorted(items, key=lambda t: -t[0]):
                jobs, objs, done_w, n_items = self.unit_items(mb, i, rel, flags, srcp, size)
                with self.cond:
                    mb.objs_by_unit[i] = objs
                    self.n_total += n_items - est_parts[(id(mb), i)]
                    self.n_done += n_items - len(jobs)
                    self.done_w += done_w
                    for jb in jobs:
                        bisect.insort(self.pending, (-jb.size, self.seq, jb, mb))
                        self.seq += 1
                    self.cond.notify_all()
        except BaseException as e:  # reported by compile_all
            self.prod_error = e
            self.log(f"  ERROR preparing the units: {e!r}")
        finally:
            with self.cond:
                self.producing = False
                self.cond.notify_all()

    def unit_items(self, mb: ModBuild, i: int, rel: str, flags: list, srcp: Path, size: int):
        """-> (jobs to run, objects in link order, weight already done, number of items)."""
        base = rel.replace("/", "_").replace("\\", "_")
        fl = " ".join(flags)
        if self.split_bytes and size >= self.split_bytes * 3 // 2:
            key = "split:" + rel
            usig = signature(split_units.VERSION, self.split_bytes, stamp(srcp))
            man = mb.state.get(key)
            names = man["parts"] if man and man.get("sig") == usig else None
            if names is not None:
                if not names:  # recorded as not splittable
                    names = None
                else:
                    objs = [mb.objdir / (n + ".obj") for n in names]
                    sigs = [signature(self.ver, mb.headers, fl, usig, n) for n in names]
                    if all(o.exists() and mb.state.get(o.name) == s for o, s in zip(objs, sigs)):
                        return [], objs, size, len(objs)
            parts = split_units.split_unit(srcp, mb.splitdir, self.split_bytes)
            if parts:
                with self.cond:
                    mb.state[key] = {"sig": usig, "parts": [p.stem for p in parts]}
                psum = sum(p.stat().st_size for p in parts)
                jobs, objs, done_w = [], [], 0
                for p in parts:
                    obj = mb.objdir / (p.stem + ".obj")
                    objs.append(obj)
                    sig = signature(self.ver, mb.headers, fl, usig, p.stem)
                    w = size * p.stat().st_size / psum
                    if obj.exists() and mb.state.get(obj.name) == sig:
                        done_w += w
                        if not self.a.keep_split:
                            p.unlink(missing_ok=True)
                        continue
                    jobs.append(Job(p, obj, flags, sig, p.stat().st_size, w, mb, not self.a.keep_split))
                return jobs, objs, done_w, len(objs)
            with self.cond:
                mb.state[key] = {"sig": usig, "parts": []}
        obj = mb.objdir / (base + ".obj")
        sig = signature(self.ver, mb.headers, fl, stamp(srcp))
        if obj.exists() and mb.state.get(obj.name) == sig:
            return [], [obj], size, 1
        return [Job(srcp, obj, flags, sig, size, size, mb, False)], [obj], 0, 1

    # ---- one compile
    def run_job(self, seq: int, jb: Job, mb: ModBuild):
        tmp = jb.obj.with_suffix(".obj.part")
        a = time.time()
        peak, cpu = 0, 0.0
        try:
            # Relative source name, run from its folder: PGO keys static functions by "<file name>:<function>",
            # which must not depend on the build folder (a shipped profile has to match every install).
            p = subprocess.Popen([str(self.clang)] + jb.flags + ["-c", jb.src.name, f"/Fo{tmp}"], env=self.env,
                                 cwd=str(jb.src.parent), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 creationflags=self.prio)
            if not self.prio:
                high_qos(p._handle)
            with self.cond:
                self.running[seq][1] = p
            out, _ = p.communicate()
            peak = process_memory(p._handle)[1]
            cpu = cpu_seconds(p._handle)
            rc = p.returncode
            out = out.decode("utf-8", "replace")
        except OSError as e:
            rc, out = -1, str(e)
        dur = time.time() - a
        with self.cond:
            del self.running[seq]
            self.compile_cpu += cpu
            if rc != 0:
                mb.failed.append(jb.src.name)
                (mb.bld / f"{jb.obj.stem}.err.txt").write_text(out, encoding="utf-8")
                self.log(f"  ERROR {jb.src.name} (see {mb.bld / (jb.obj.stem + '.err.txt')})")
            else:
                os.replace(tmp, jb.obj)
                mb.state[jb.obj.name] = jb.sig
                mb.save_state()
                if jb.temp:
                    jb.src.unlink(missing_ok=True)
                self.learn(jb.size, peak)
                self.max_peak = max(self.max_peak, peak)
                self.n_done += 1
                self.n_compiled += 1
                self.done_w += jb.weight
                pct = min(100.0, 100.0 * self.done_w / max(1, self.total_w))
                self.log(f"  [{self.n_done}/{self.n_total} {pct:.1f}%] {jb.src.name} {dur:.0f} s (cpu {cpu:.0f} s) {peak / GB:.2f} GB")
            self.cond.notify_all()

    # ---- scheduler
    def growth(self) -> int:
        """Memory the running compilers are still expected to take (estimate minus current working set)."""
        g = 0
        for est, p in self.running.values():
            ws = process_memory(p._handle)[0] if p is not None and p.returncode is None else 0
            g += max(0, est - ws)
        return g

    def compile_all(self, mbs: list[ModBuild], max_jobs: int, margin: int):
        prod = threading.Thread(target=self.produce, args=(mbs,), daemon=True)
        prod.start()
        threads = []
        # Admission: start the biggest pending unit whose estimated peak fits in the free RAM, after
        # subtracting what the running compilers will still grow and a margin for the rest of the system.
        with self.cond:
            while True:
                while self.pending and len(self.running) < max_jobs:
                    g = self.growth()
                    room = min(ram_free(), commit_free()) - g - margin
                    pick = None
                    for k, (_, seq, jb, mb) in enumerate(self.pending):
                        if not self.running or self.estimate(jb.size) <= room:
                            pick = k
                            break
                    if pick is None:
                        break
                    _, seq, jb, mb = self.pending.pop(pick)
                    self.running[seq] = [self.estimate(jb.size), None]
                    self.max_running = max(self.max_running, len(self.running))
                    t = threading.Thread(target=self.run_job, args=(seq, jb, mb), daemon=True)
                    t.start()
                    threads.append(t)
                if not self.pending and not self.running and not self.producing:
                    break
                self.cond.wait(timeout=1.0)
        for t in threads:
            t.join()
        if self.prod_error:
            raise SystemExit(f"preparing the units failed: {self.prod_error!r}")


def isa_guard(tc: Path, isa: str) -> list[str]:
    """recomp_isa_guard_v<n>.obj for a DLL built for x86-64-v<n>: it refuses to load on an older CPU (recomp_crt0.c)."""
    if isa in ("x86-64-v2", "x86-64-v3"):
        g = tc / "lib" / f"recomp_isa_guard_v{isa[-1]}.obj"
        if g.exists():
            return [str(g)]
    return []


def link_module(mb: ModBuild, tc: Path, log, prio: int, extra: list[str] = ()) -> dict:
    mod, bld = mb.mod, mb.bld
    lld = tc / "bin" / "lld-link.exe"
    objs = [o for i in range(len(mb.units)) for o in mb.objs_by_unit[i]]
    dll = bld / mod.output
    libs = sorted(str(p) for p in (tc / "lib").glob("*.lib"))
    rsp = bld / "link.rsp"
    lines = ["/nologo", "/dll", "/machine:x64", "/INCREMENTAL:NO", "/version:0.0", "/nodefaultlib",
             "/entry:_DllMainCRTStartup", f"/out:{dll}", f"/implib:{dll.with_suffix('.lib')}",
             str(tc / "lib" / CRT0)] + list(extra) + [str(o) for o in objs] + libs
    rsp.write_text("\n".join(f'"{x}"' if " " in x else x for x in lines), encoding="utf-8")
    a = time.time()
    r = subprocess.run([str(lld), f"@{rsp}"], capture_output=True, text=True, errors="replace", creationflags=prio)
    t_link = time.time() - a
    if r.returncode != 0:
        (bld / "link.err.txt").write_text(r.stdout + r.stderr, encoding="utf-8")
        raise SystemExit(f"{mod.target}: link failed (see {bld / 'link.err.txt'}):\n{(r.stdout + r.stderr)[-2000:]}")
    res = {"module": mod.target, "dll": str(dll), "dll_mb": round(dll.stat().st_size / MB, 1),
           "objects": len(objs), "link_s": round(t_link, 1)}
    log(f"{mod.target}: {dll.name} {res['dll_mb']} MB, {len(objs)} objects, link {res['link_s']} s")
    return res


def find_modules(export: Path) -> list[Path]:
    for d in export.rglob("exefs"):
        if d.is_dir() and d.parent.name == "aot_cache":
            return sorted(p for p in d.iterdir() if (p / "CMakeLists.txt").exists())
    raise SystemExit(f"no aot_cache\\exefs under {export}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--export", type=Path, help="export folder (looks for aot_cache\\exefs\\<module>)")
    g.add_argument("--module-dir", type=Path, action="append", help="a module folder (with CMakeLists.txt); repeatable")
    ap.add_argument("--modules", default="", help="with --export: comma-separated list (default: all)")
    ap.add_argument("--out", type=Path, required=True, help="build folder; each module goes to <out>\\<module>")
    ap.add_argument("--toolchain", type=Path, default=HERE, help="portable toolchain folder (default: this script's)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="maximum concurrent compiles (default: logical cores; memory decides how many really run)")
    ap.add_argument("--split-mb", type=float, default=8.0,
                    help="split generated units bigger than 1.5x this into ~this many MB (0 = whole units, as before)")
    ap.add_argument("--unit-flags", default="",
                    help="extra clang-cl flags for the generated units only (experiments, e.g. /clang:-O1)")
    ap.add_argument("--isa", default="x86-64", choices=["auto"] + sorted(ISA_FLAGS),
                    help="target ISA of every unit (auto = this CPU's level, capped at x86-64-v3)")
    ap.add_argument("--all-flags", default="",
                    help="extra clang-cl flags for every unit, runtime included (experiments)")
    ap.add_argument("--profile-use", type=Path, default=None,
                    help="optimize with this clang profile (.profdata; functions it does not cover are unaffected)")
    ap.add_argument("--no-link", action="store_true", help="compile only (partial module dirs for experiments)")
    ap.add_argument("--keep-split", action="store_true", help="keep the split sources after compiling them")
    ap.add_argument("--mem-margin-gb", type=float, default=1.5,
                    help="free RAM always left for the rest of the system")
    ap.add_argument("--priority", choices=sorted(PRIORITIES), default="normal",
                    help="compiler priority (low = unobtrusive, but slower on hybrid CPUs)")
    ap.add_argument("--unit-mem-gb", type=float, default=0.0,
                    help="fixed memory per compile instead of the size-based estimate (old behaviour: 6)")
    a = ap.parse_args()

    tc = a.toolchain.resolve()
    for req in ("bin/clang-cl.exe", "bin/lld-link.exe", f"lib/{CRT0}", "include/windows.h"):
        if not (tc / req).exists():
            raise SystemExit(f"incomplete toolchain: missing {tc / req} (build the package with build-toolchain.ps1)")
    if a.export:
        dirs = find_modules(a.export)
        if a.modules:
            wanted = [m.strip() for m in a.modules.split(",") if m.strip()]
            dirs = [d for d in dirs if d.name in wanted]
    else:
        dirs = a.module_dir
    a.out.mkdir(parents=True, exist_ok=True)
    logf = open(a.out / "build_modules.log", "a", encoding="utf-8")
    loglock = threading.Lock()

    def log(s: str):
        line = time.strftime("%H:%M:%S ") + s
        with loglock:
            print(line, flush=True)
            logf.write(line + "\n"); logf.flush()

    isa = a.isa
    if isa == "auto":
        try:
            isa = cpu_isa.level()[0]
        except Exception as e:  # detection must never break an install: fall back to the baseline
            log(f"CPU level detection failed ({e!r}): x86-64")
            isa = "x86-64"
        if isa not in ISA_FLAGS:  # v4: the v3 code (AVX-512 not used, see TOOLCHAIN.md §8)
            isa = "x86-64-v3"
    all_flags = ISA_FLAGS[isa] + a.all_flags.split()
    if a.profile_use:
        prof = a.profile_use.resolve()
        # The stamp is part of the flags, so a new profile recompiles everything. Functions the profile does not
        # cover (other units, renamed or new functions) compile as without it: the warnings are off and
        # -pgso=false keeps LLVM from optimizing unprofiled/cold code for size (docs/port/PROGRESS-FPS2-CODEGEN.md).
        all_flags += [f"-fprofile-instr-use={prof}", "-Wno-profile-instr-unprofiled", "-Wno-profile-instr-missing",
                      "-Wno-profile-instr-out-of-date", "-Wno-backend-plugin", "-mllvm", "-pgso=false",
                      f"-DRECOMP_PGO_STAMP={stamp(prof).split(':', 1)[1].replace(':', '_')}"]
    max_jobs = a.jobs or (os.cpu_count() or 1)
    margin = int(a.mem_margin_gb * GB)
    b = Builder(a, tc, log)
    mbs = [ModBuild(Module(d), a.out / d.name, tc, a.unit_flags.split(), all_flags) for d in dirs]
    log(f"{', '.join(m.mod.target for m in mbs)}: ISA {isa}{', profile ' + str(a.profile_use) if a.profile_use else ''}; up to {max_jobs} compiles while the free RAM allows "
        f"(free {ram_free() / GB:.1f} of {ram_total() / GB:.1f} GB, margin {a.mem_margin_gb:g} GB), "
        + (f"units split at {a.split_mb:g} MB" if b.split_bytes else "whole units"))
    t0 = time.time()
    b.compile_all(mbs, max_jobs, margin)
    for mb in mbs:
        mb.save_state()
    t_comp = time.time() - t0
    failed = [f"{mb.mod.target}: {', '.join(mb.failed[:5])}" for mb in mbs if mb.failed]
    if failed:
        raise SystemExit("units failed: " + "; ".join(failed))
    log(f"compile {t_comp / 60:.1f} min ({b.n_compiled} compiled, up to {b.max_running} at once, "
        f"peak {b.max_peak / GB:.1f} GB per compiler, "
        f"{b.compile_cpu / 60:.0f} CPU-min)")
    if a.no_link:
        log("compile only (--no-link)")
        return 0
    results = []
    lt = []
    for mb in mbs:
        t = threading.Thread(target=lambda m=mb: results.append(link_module(m, tc, log, PRIORITIES[a.priority], isa_guard(tc, isa))))
        t.start(); lt.append(t)
    for t in lt:
        t.join()
    if len(results) != len(mbs):
        raise SystemExit("link failed (see the log)")
    summary = {"compile_min": round(t_comp / 60, 1), "total_min": round((time.time() - t0) / 60, 1),
               "compiled": b.n_compiled, "cpu_min": round(b.compile_cpu / 60, 1), "max_jobs": max_jobs, "max_running": b.max_running,
               "split_mb": a.split_mb, "modules": sorted(results, key=lambda r: r["module"])}
    log(f"done in {summary['total_min']} min")
    (a.out / "build_modules.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
