#!/usr/bin/env python3
"""Recovers the Itanium RTTI (typeinfo, hierarchy and vtables) of an AArch64 ELF.

Algorithm
  1. Typeinfo candidates: every aligned pointer slot (relocation or raw
     value) whose next slot points to a string that is a valid mangled
     type name (the content of a _ZTS*).
  2. Candidates are grouped by their vptr. Each group is identified as
     __class_type_info / __si_class_type_info / __vmi_class_type_info:
       a) by the symbol name (imported or local) of the __cxxabiv1 vtable;
       b) by the RTTI of that vtable itself (vtable[-1] -> typeinfo whose name
          is N10__cxxabiv117__class_type_infoE, etc.), useful if libc++/libsupc++
          is statically linked and without symbols;
       c) by the shape of the objects (heuristic), as a last resort.
  3. The bases of each typeinfo are read (si: 1 base; vmi: flags, no. of bases and
     pairs (typeinfo, offset_flags)).
  4. Vtables: every slot that points exactly to a class typeinfo, preceded
     by an integer offset_to_top (without relocation), is the RTTI entry of a
     sub-vtable; its "address point" is the next slot and its entries are the
     consecutive code pointers. Sub-vtables are grouped (primary with
     offset_to_top == 0 + secondary) and construction vtables (_ZTC) are
     told apart thanks to the VTTs.

Output: JSON (default tools/ghidra/out/rtti.json) with absolute addresses.

Usage:
    python tools/analysis/rtti.py data/main.elf -o tools/ghidra/out/rtti.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfimage import ElfImage, Pointer, add_common_args  # noqa: E402
from demangle import demangle_types, demangle_many  # noqa: E402

CXXABI = {
    "N10__cxxabiv117__class_type_infoE": "class",
    "N10__cxxabiv120__si_class_type_infoE": "si",
    "N10__cxxabiv121__vmi_class_type_infoE": "vmi",
    "N10__cxxabiv119__pointer_type_infoE": "pointer",
    "N10__cxxabiv129__pointer_to_member_type_infoE": "pointer_to_member",
    "N10__cxxabiv123__fundamental_type_infoE": "fundamental",
    "N10__cxxabiv117__array_type_infoE": "array",
    "N10__cxxabiv120__function_type_infoE": "function",
    "N10__cxxabiv116__enum_type_infoE": "enum",
}
CXXABI_VTABLE_SYMS = {"_ZTV" + k: v for k, v in CXXABI.items()}
CLASS_KINDS = ("class", "si", "vmi")
PURE_VIRTUAL = {"__cxa_pure_virtual", "__cxa_deleted_virtual"}

# First cheap filter for class type names (before demangling).
_TYPE_RE = re.compile(rb"^(?:N|Z|S[tabsiod]|S[0-9A-Z]*_|[1-9])[0-9A-Za-z_$.]*$")
_PTR_TYPE_RE = re.compile(rb"^(?:P|K|V|M)[0-9A-Za-z_$.]*$")


@dataclass
class TypeInfo:
    va: int
    vptr_key: tuple
    name_va: int
    mangled: str
    kind: str = "?"
    size: int = 16
    bases: list = field(default_factory=list)  # [(Pointer, offset, virtual, public)]
    vmi_flags: int = 0


@dataclass
class SubVtable:
    rtti_slot: int          # va of the slot with the pointer to the typeinfo
    typeinfo: int
    offset_to_top: int
    entries: list           # [Pointer]

    @property
    def address_point(self) -> int:
        return self.rtti_slot + 8


def ptr_key(p: Pointer) -> tuple:
    if p.target is not None:
        return ("va", p.target)
    return ("imp", p.import_name, p.addend)


class RttiAnalyzer:
    def __init__(self, img: ElfImage, use_symbols: bool = True, use_self_rtti: bool = True,
                 verbose: bool = False):
        self.img = img
        self.use_symbols = use_symbols
        self.use_self_rtti = use_self_rtti
        self.verbose = verbose
        self.typeinfos: dict[int, TypeInfo] = {}
        self.kind_of_key: dict[tuple, str] = {}
        self.key_how: dict[tuple, str] = {}
        self.subvtables: list[SubVtable] = []

    def log(self, *a):
        if self.verbose:
            print(*a, file=sys.stderr)

    # ------------------------------------------------------------ steps 1 and 2
    def find_candidates(self):
        img = self.img
        cands: dict[int, TypeInfo] = {}
        names: dict[int, bytes] = {}
        # The GOT contains loose pointers to typeinfo and to their names: it is excluded.
        got = self._got_ranges()
        for va, p in img.pointer_slots():
            if va % 8 or any(lo <= va < hi for lo, hi in got):
                continue
            q = img.pointer_at(va + 8)
            if q is None or q.target is None or not img.is_data(q.target):
                continue
            s = names.get(q.target)
            if s is None:
                s = img.cstring(q.target, 2048) or b""
                names[q.target] = s
            if s.startswith(b"*"):
                s = s[1:]  # GCC: '*' marks names with internal linkage (compared by strcmp)
            if not s or not (_TYPE_RE.match(s) or _PTR_TYPE_RE.match(s)):
                continue
            cands[va] = TypeInfo(va, ptr_key(p), q.target, s.decode("ascii"))
        # Strict validation: the name must demangle as a type.
        dem = demangle_types({t.mangled for t in cands.values()})
        self.demangled = {k: v for k, v in dem.items() if v}
        self.candidates = {va: t for va, t in cands.items() if t.mangled in self.demangled}
        self.log(f"typeinfo candidates: {len(self.candidates)} (out of {len(cands)} pointer+string pairs)")

    def classify_keys(self):
        img = self.img
        by_key: dict[tuple, list[TypeInfo]] = defaultdict(list)
        for t in self.candidates.values():
            by_key[t.vptr_key].append(t)
        local_syms = img.symbols_at() if self.use_symbols else {}
        for key, members in by_key.items():
            kind = how = None
            if key[0] == "imp":
                if self.use_symbols and key[1] in CXXABI_VTABLE_SYMS and key[2] == 16:
                    kind, how = CXXABI_VTABLE_SYMS[key[1]], "imported symbol"
            else:
                vt = key[1] - 16
                if self.use_symbols:
                    for s in local_syms.get(vt, []):
                        if s.name in CXXABI_VTABLE_SYMS:
                            kind, how = CXXABI_VTABLE_SYMS[s.name], "local symbol"
                if kind is None and self.use_self_rtti:
                    # vtable[-1] (at key-8) -> typeinfo of the __cxxabiv1 class itself
                    q = img.pointer_at(key[1] - 8)
                    if q is not None and q.target is not None:
                        r = img.pointer_at(q.target + 8)
                        if r is not None and r.target is not None:
                            nm = (img.cstring(r.target, 128) or b"").decode("ascii", "replace")
                            if nm in CXXABI:
                                kind, how = CXXABI[nm], "own __cxxabiv1 RTTI"
            if kind is not None:
                self.kind_of_key[key] = kind
                self.key_how[key] = how
        # Heuristic for the groups that remain unidentified.
        known_ti = set(self.candidates)
        for key, members in by_key.items():
            if key in self.kind_of_key:
                continue
            if not self._looks_like_vptr(key):
                continue
            kind = self._guess_kind(members, known_ti)
            if kind:
                self.kind_of_key[key] = kind
                self.key_how[key] = "heuristic"
        for key, kind in self.kind_of_key.items():
            self.log(f"vptr {key} -> {kind} ({self.key_how[key]}, {len(by_key[key])} typeinfos)")
        for t in self.candidates.values():
            k = self.kind_of_key.get(t.vptr_key)
            if k:
                t.kind = k
                self.typeinfos[t.va] = t

    def _got_ranges(self) -> list[tuple[int, int]]:
        img = self.img
        return [(sec["sh_addr"], sec["sh_addr"] + sec["sh_size"]) for name in (".got", ".got.plt")
                for sec in [img.section(name)] if sec is not None]

    def _looks_like_vptr(self, key: tuple) -> bool:
        """The vptr of a typeinfo points to vtable+16: [0][typeinfo][functions...]."""
        img = self.img
        if key[0] != "va":
            return False
        t = key[1]
        if not img.is_data(t) or t % 8:
            return False
        try:
            if img.has_reloc(t - 16) or img.s64(t - 16) != 0:
                return False
        except IndexError:
            return False
        if img.pointer_at(t - 8) is None:
            return False
        e = img.pointer_at(t)
        return e is not None and (e.import_name is not None or (e.target is not None and img.is_exec(e.target)))

    def _guess_kind(self, members: list[TypeInfo], known_ti: set[int]) -> str | None:
        img = self.img
        n = len(members)
        if all(_PTR_TYPE_RE.match(m.mangled.encode()) for m in members):
            return "pointer" if n >= 2 else None
        if not all(_TYPE_RE.match(m.mangled.encode()) for m in members):
            return None
        si_ok = vmi_ok = 0
        for m in members:
            p = img.pointer_at(m.va + 16)
            if p is not None and p.target in known_ti:
                si_ok += 1
            elif self._vmi_shape(m.va, known_ti):
                vmi_ok += 1
        if si_ok >= 0.9 * n and si_ok:
            return "si"
        if vmi_ok >= 0.9 * n and vmi_ok:
            return "vmi"
        if si_ok + vmi_ok <= 0.1 * n:
            # __class_type_info: we require that some be a base of another typeinfo
            # or have a vtable that references it (checked later).
            return "class?"
        return None

    def _vmi_shape(self, va: int, known_ti: set[int]) -> bool:
        img = self.img
        try:
            if img.has_reloc(va + 16):
                return False
            flags, count = img.u32(va + 16), img.u32(va + 20)
            if flags > 3 or not 1 <= count <= 128:
                return False
            for i in range(count):
                p = img.pointer_at(va + 24 + 16 * i)
                if p is None or (p.target is not None and p.target not in known_ti):
                    return False
                if img.has_reloc(va + 32 + 16 * i) or img.s64(va + 32 + 16 * i) & 0xFC:
                    return False
            return True
        except IndexError:
            return False

    # ------------------------------------------------------------------ paso 3
    def parse_bases(self):
        img = self.img
        for t in self.typeinfos.values():
            if t.kind == "si":
                t.size = 24
                p = img.pointer_at(t.va + 16)
                if p is not None:
                    t.bases.append((p, 0, False, True))
            elif t.kind == "vmi":
                t.vmi_flags = img.u32(t.va + 16)
                count = img.u32(t.va + 20)
                t.size = 24 + 16 * count
                for i in range(count):
                    p = img.pointer_at(t.va + 24 + 16 * i)
                    of = img.s64(t.va + 32 + 16 * i)
                    if p is not None:
                        t.bases.append((p, of >> 8, bool(of & 1), bool(of & 2)))
            elif t.kind == "pointer":
                t.size = 32

    # ------------------------------------------------------------------ paso 4
    def find_vtables(self):
        img = self.img
        class_ti = {va for va, t in self.typeinfos.items() if t.kind in CLASS_KINDS or t.kind == "class?"}
        # Ranges occupied by the typeinfos themselves (their bases also point to typeinfo).
        inside_ti = set()
        for t in self.typeinfos.values():
            inside_ti.update(range(t.va, t.va + t.size, 8))
        subs = []
        for va, p in img.pointer_slots():
            if p.target not in class_ti or va in inside_ti:
                continue
            if img.has_reloc(va - 8) or img.pointer_at(va - 8) is not None:
                continue
            try:
                ott = img.s64(va - 8)
            except IndexError:
                continue
            if not -0x10000000 <= ott <= 0 or ott % 4:
                continue
            entries = []
            q = va + 8
            while True:
                e = img.pointer_at(q)
                if e is None:
                    # GCC leaves some entries of the secondary sub-vtables of
                    # construction vtables as 0: in sub-vtables with offset_to_top < 0
                    # short null gaps followed by a code pointer are accepted.
                    gap = self._null_gap(q, class_ti) if ott < 0 else 0
                    if gap:
                        entries.extend([None] * gap)
                        q += 8 * gap
                        continue
                    break
                if e.target is not None and not img.is_exec(e.target):
                    break
                if e.target is None and e.import_name is None:
                    break
                entries.append(e)
                q += 8
            subs.append(SubVtable(va, p.target, ott, entries))
        subs.sort(key=lambda s: s.rtti_slot)
        self.subvtables = subs
        # The "class?" ones are confirmed if they have a vtable or are a base of another class.
        referenced = {s.typeinfo for s in subs}
        for t in self.typeinfos.values():
            for p, *_ in t.bases:
                if p.target is not None:
                    referenced.add(p.target)
        for t in self.typeinfos.values():
            if t.kind == "class?":
                t.kind = "class" if t.va in referenced else "?"
        self.typeinfos = {va: t for va, t in self.typeinfos.items() if t.kind != "?"}
        self.subvtables = [s for s in subs if s.typeinfo in self.typeinfos]

    def _null_gap(self, q: int, class_ti: set) -> int:
        img = self.img
        n = 0
        while n < 4:
            a = q + 8 * n
            if img.has_reloc(a):
                break
            try:
                if img.u64(a) != 0:
                    return 0
            except IndexError:
                return 0
            n += 1
        if n == 0:
            return 0
        e = img.pointer_at(q + 8 * n)
        if e is None or e.target is None or not img.is_exec(e.target):
            return 0
        return n

    def group_vtables(self):
        """Group sub-vtables into complete vtables and mark the construction ones."""
        img = self.img
        groups = []
        cur = None
        for s in self.subvtables:
            if s.offset_to_top == 0 or cur is None or cur["typeinfo"] != s.typeinfo:
                cur = {"typeinfo": s.typeinfo, "subs": [s], "construction": False}
                groups.append(cur)
            else:
                cur["subs"].append(s)
        ap_to_group = {}
        for g in groups:
            for s in g["subs"]:
                ap_to_group[s.address_point] = g
        # VTT: sequences of slots that point to address points.
        # VTT[0] is the primary vtable of the complete class C; groups
        # referenced in the same VTT with another typeinfo are construction ones.
        inside_ti = self._inside_ti()
        vtt_slots = sorted(va for va, p in img.pointer_slots()
                           if p.target in ap_to_group and va not in inside_ti)
        runs, run = [], []
        for va in vtt_slots:
            if run and va == run[-1] + 8:
                run.append(va)
            else:
                if run:
                    runs.append(run)
                run = [va]
        if run:
            runs.append(run)
        self.vtts = []
        for r in runs:
            first = ap_to_group[img.pointer_at(r[0]).target]
            if len(r) < 2 or first["subs"][0].offset_to_top != 0:
                continue
            self.vtts.append((r[0], len(r), first["typeinfo"]))
            for va in r[1:]:
                g = ap_to_group[img.pointer_at(va).target]
                if g is not first and g["typeinfo"] != first["typeinfo"]:
                    g["construction"] = True
                    g["construction_for"] = first["typeinfo"]
        self.groups = groups

    # --------------------------------------------------- vtables without RTTI
    def find_nortti_vtables(self, require_coderef: bool = True):
        """Vtables of classes compiled with -fno-rtti: [offset_to_top][0][functions...].

        By default it is required that the address point be materialized in code with
        ADRP+ADD (constructors store it in the object); this discards
        data structures that happen to have the same shape.
        """
        img = self.img
        known_aps = {s.address_point for s in self.subvtables}
        refs = None
        if require_coderef:
            try:
                import coderefs
                refs = set(int(x) for x in coderefs.adrp_refs(img)["add"])
            except ImportError:
                self.log("[warning] numpy not available: vtables without RTTI not confirmed by code")
        inside_ti = self._inside_ti()
        got = self._got_ranges()
        subs = []
        for va, p in img.pointer_slots():
            if va in known_aps or va in inside_ti or p.kind in ("GLOB_DAT", "JUMP_SLOT"):
                continue
            if any(lo <= va < hi for lo, hi in got):
                continue
            if p.target is None:
                if p.import_name is None:
                    continue
            elif not img.is_exec(p.target):
                continue
            try:
                if img.has_reloc(va - 8) or img.u64(va - 8) != 0 or img.has_reloc(va - 16):
                    continue
                ott = img.s64(va - 16)
            except IndexError:
                continue
            if not -0x1000000 <= ott <= 0 or ott % 8:
                continue
            # clang materializes vtable+16; GCC sometimes vtable and then adds 16
            if refs is not None and not any(va - d in refs for d in (0, 16, 24, 32)):
                continue
            entries = []
            q = va
            while True:
                e = img.pointer_at(q)
                if e is None or (e.target is None and e.import_name is None) or \
                        (e.target is not None and not img.is_exec(e.target)):
                    break
                entries.append(e)
                q += 8
            if not any(e.target is not None for e in entries):
                continue  # imports only: not an own vtable
            subs.append(SubVtable(va - 8, 0, ott, entries))
        subs.sort(key=lambda s: s.rtti_slot)
        groups, cur = [], None
        for sv in subs:
            if sv.offset_to_top == 0 or cur is None:
                cur = {"subs": [sv]}
                groups.append(cur)
            else:
                cur["subs"].append(sv)
        self.nortti_groups = groups
        self.log(f"vtables without RTTI: {len(groups)} ({len(subs)} sub-vtables)")

    # ------------------------------------------------------------------ output
    def run(self, nortti: bool = True, require_coderef: bool = True):
        self.find_candidates()
        self.classify_keys()
        self.parse_bases()
        self.find_vtables()
        self.group_vtables()
        self.nortti_groups = []
        if nortti:
            self.find_nortti_vtables(require_coderef)
        return self

    def has_virtual_bases(self, ti: int, seen=None) -> bool:
        seen = seen or set()
        if ti in seen or ti not in self.typeinfos:
            return False
        seen.add(ti)
        for p, _, virt, _ in self.typeinfos[ti].bases:
            if virt or (p.target is not None and self.has_virtual_bases(p.target, seen)):
                return True
        return False

    def vtable_start(self, g) -> tuple[int, bool]:
        """Start of the _ZTV symbol: vcall/vbase offsets may precede the offset_to_top."""
        img = self.img
        start = g["subs"][0].rtti_slot - 8
        if not self.has_virtual_bases(g["typeinfo"]):
            return start, True
        inside_ti = self._inside_ti()
        for _ in range(64):
            prev = start - 8
            if prev in inside_ti or img.pointer_at(prev) is not None or prev in self._sub_ranges():
                break
            try:
                v = img.s64(prev)
            except IndexError:
                break
            if not -0x10000000 <= v <= 0x10000000:
                break
            start = prev
        return start, False

    def _inside_ti(self):
        if not hasattr(self, "_inside_ti_cache"):
            s = set()
            for t in self.typeinfos.values():
                s.update(range(t.va, t.va + t.size, 8))
            self._inside_ti_cache = s
        return self._inside_ti_cache

    def _sub_ranges(self):
        if not hasattr(self, "_sub_cache"):
            s = set()
            for sv in self.subvtables:
                s.update(range(sv.rtti_slot - 8, sv.address_point + 8 * len(sv.entries), 8))
            self._sub_cache = s
        return self._sub_cache

    def to_json(self) -> dict:
        img = self.img
        A = lambda va: f"0x{img.to_abs(va):x}"  # noqa: E731

        def ptr_json(p: Pointer | None):
            if p is None:
                return None
            if p.target is not None:
                return A(p.target)
            return p.import_name + (f"+{p.addend:#x}" if p.addend else "")

        # Unique readable names per typeinfo
        names: dict[int, str] = {}
        used = defaultdict(int)
        for va, t in sorted(self.typeinfos.items()):
            used[self.demangled.get(t.mangled, t.mangled)] += 1
        for va, t in sorted(self.typeinfos.items()):
            n = self.demangled.get(t.mangled, t.mangled)
            names[va] = n if used[n] == 1 else f"{n}@{img.to_abs(va):x}"

        imported_ti_names = {}
        for t in self.typeinfos.values():
            for p, *_ in t.bases:
                if p.target is None and p.import_name:
                    imported_ti_names[p.import_name] = None
        dem_imp = demangle_many(list(imported_ti_names))

        def base_name(p: Pointer):
            if p.target is not None:
                return names.get(p.target, A(p.target))
            d = dem_imp.get(p.import_name)
            return d.replace("typeinfo for ", "") if d else p.import_name

        groups_by_ti = defaultdict(list)
        for g in self.groups:
            groups_by_ti[g["typeinfo"]].append(g)

        classes = {}
        for va, t in sorted(self.typeinfos.items()):
            if t.kind not in CLASS_KINDS:
                continue
            c = {
                "mangled": t.mangled,
                "typeinfo": A(va),
                "typeinfo_name": A(t.name_va),
                "kind": t.kind,
                "bases": [
                    {"name": base_name(p), "typeinfo": ptr_json(p), "offset": off,
                     "virtual": virt, "public": pub}
                    for p, off, virt, pub in t.bases
                ],
            }
            if t.kind == "vmi":
                c["vmi_flags"] = t.vmi_flags
            gs = groups_by_ti.get(va, [])
            main = [g for g in gs if not g["construction"]]
            constr = [g for g in gs if g["construction"]]
            if main:
                # If there are several candidates, the one with the most entries.
                g = max(main, key=lambda g: sum(len(s.entries) for s in g["subs"]))
                start, exact = self.vtable_start(g)
                c["vtable"] = A(start)
                c["vtable_start_exact"] = exact
                c["address_point"] = A(g["subs"][0].address_point)
                c["vtables"] = [
                    {"address_point": A(s.address_point), "offset_to_top": s.offset_to_top,
                     "entries": [ptr_json(e) for e in s.entries]}
                    for s in g["subs"]
                ]
                c["virtual_functions"] = c["vtables"][0]["entries"]
                others = [x for x in main if x is not g]
                if others:
                    c["other_vtables"] = [A(x["subs"][0].address_point) for x in others]
            if constr:
                c["construction_vtables"] = [
                    {"address_point": A(g["subs"][0].address_point),
                     "for": names.get(g.get("construction_for"), "?"),
                     "vtables": [{"address_point": A(s.address_point), "offset_to_top": s.offset_to_top,
                                  "entries": [ptr_json(e) for e in s.entries]} for s in g["subs"]]}
                    for g in constr]
            classes[names[va]] = c

        anon = []
        for g in self.nortti_groups:
            first = g["subs"][0]
            anon.append({
                "name": f"vtable_{img.to_abs(first.rtti_slot - 8):x}",
                "vtable": A(first.rtti_slot - 8),
                "address_point": A(first.address_point),
                "vtables": [{"address_point": A(s.address_point), "offset_to_top": s.offset_to_top,
                             "entries": [ptr_json(e) for e in s.entries]} for s in g["subs"]],
            })

        return {
            "elf": str(img.path),
            "base": f"0x{img.base:x}",
            "cxxabi_vptrs": {
                (f"{k[1]}+{k[2]:#x}" if k[0] == "imp" else A(k[1])): {"kind": v, "how": self.key_how[k]}
                for k, v in self.kind_of_key.items()
            },
            "stats": {
                "typeinfos": len(self.typeinfos),
                "classes": len(classes),
                "with_vtable": sum(1 for c in classes.values() if "vtable" in c),
                "subvtables": len(self.subvtables),
                "vtts": len(getattr(self, "vtts", [])),
                "vtables_without_rtti": len(anon),
            },
            "classes": classes,
            "vtables_without_rtti": anon,
        }


def analyze(img: ElfImage, nortti: bool = True, require_coderef: bool = True, **kw) -> dict:
    return RttiAnalyzer(img, **kw).run(nortti, require_coderef).to_json()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("-o", "--output", default=None, help="output rtti.json (default stdout)")
    ap.add_argument("--no-symbols", action="store_true", help="ignore symbols (simulates a binary without them)")
    ap.add_argument("--no-self-rtti", action="store_true", help="do not use the own RTTI of __cxxabiv1")
    ap.add_argument("--no-nortti", action="store_true", help="do not search for vtables without RTTI (-fno-rtti)")
    ap.add_argument("--no-coderef", action="store_true",
                    help="do not require vtables without RTTI to be referenced by ADRP+ADD")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    img = ElfImage(args.elf, args.base)
    res = analyze(img, nortti=not args.no_nortti, require_coderef=not args.no_coderef,
                  use_symbols=not args.no_symbols, use_self_rtti=not args.no_self_rtti,
                  verbose=args.verbose)
    text = json.dumps(res, indent=1, ensure_ascii=False)
    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(text, encoding="utf-8")
        st = res["stats"]
        print(f"rtti: {st['classes']} classes, {st['with_vtable']} with vtable, "
              f"{st['subvtables']} sub-vtables, {st['vtts']} VTT, "
              f"{st['vtables_without_rtti']} vtables without RTTI -> {args.output}", file=sys.stderr)
    else:
        sys.stdout.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
