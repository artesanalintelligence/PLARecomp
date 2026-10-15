"""Tests for tools/analysis without the game: C++ ELFs compiled with devkitA64.

Run:  python -m pytest tools/analysis/tests -q
The headless Ghidra test is optional: RUN_GHIDRA_TESTS=1.
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ANALYSIS = HERE.parent
REPO = ANALYSIS.parents[1]
sys.path.insert(0, str(ANALYSIS))
sys.path.insert(0, str(HERE))

import build_fixtures  # noqa: E402
from elfimage import ElfImage, DEFAULT_BASE  # noqa: E402
import eh_frame  # noqa: E402
import rtti  # noqa: E402
import jumptables  # noqa: E402
import code_targets  # noqa: E402
import merge_functions  # noqa: E402

VARIANTS = ["default", "hidden", "static"]  # with RTTI; "nortti" is tested separately

# Expected hierarchy: class -> (kind, [(base, offset, virtual)])
EXPECTED = {
    "game::Base": ("class", []),
    "game::Derived": ("si", [("game::Base", 0, False)]),
    "game::Abstract": ("class", []),
    "game::Mixin": ("class", []),
    "game::Multi": ("vmi", [("game::Derived", 0, False), ("game::Mixin", 16, False)]),
    "game::VBase": ("class", []),
    "game::Left": ("vmi", [("game::VBase", -24, True)]),
    "game::Right": ("vmi", [("game::VBase", -24, True)]),
    "game::Diamond": ("vmi", [("game::Left", 0, False), ("game::Right", 16, False)]),
    "game::Impl": ("si", [("game::Abstract", 0, False)]),
    "game::Holder<int, 3>": ("si", [("game::Base", 0, False)]),
    "game::Holder<float, 4>": ("si", [("game::Base", 0, False)]),
    "(anonymous namespace)::LocalOnly": ("class", []),
}


@pytest.fixture(scope="session")
def fixtures(tmp_path_factory):
    out = tmp_path_factory.mktemp("elf")
    try:
        return build_fixtures.build(out)
    except FileNotFoundError as e:
        pytest.skip(str(e))


def symtab(path: Path) -> dict[str, tuple[int, int, str]]:
    """Ground truth: symbols of the unstripped ELF (name -> (va, size, kind))."""
    img = ElfImage(path)
    return {s.name: (s.va, s.size, s.type) for s in img.symbols if s.defined}


def text_funcs(path: Path) -> dict[int, int]:
    img = ElfImage(path)
    text = img.section(".text")
    lo, hi = text["sh_addr"], text["sh_addr"] + text["sh_size"]
    out = {}
    for s in img.defined_symbols("STT_FUNC"):
        if lo <= s.va < hi and s.size:
            out[s.va] = s.size
    return out


def strip_section_headers(src: Path, dst: Path) -> Path:
    """Copy the ELF without a section table (like a minimal main.elf)."""
    data = bytearray(src.read_bytes())
    # e_shoff (0x28, 8 bytes), e_shnum (0x3c, 2), e_shstrndx (0x3e, 2)
    struct.pack_into("<Q", data, 0x28, 0)
    struct.pack_into("<HH", data, 0x3C, 0, 0)
    dst.write_bytes(bytes(data))
    return dst


# ----------------------------------------------------------------- eh_frame
@pytest.mark.parametrize("variant", VARIANTS)
def test_eh_frame_matches_symbols(fixtures, variant):
    truth = text_funcs(fixtures[variant])
    img = ElfImage(fixtures[variant + ".stripped"])
    fdes, loc = eh_frame.collect(img)
    got = {f.start: f.size for f in fdes}
    assert loc.get("hdr_fde_count", len(fdes)) == loc["fde_count"]
    # Every function with an FDE matches its symbol exactly
    for start, size in got.items():
        assert start in truth, f"FDE at {start:#x} without symbol"
        assert truth[start] == size, f"{start:#x}: FDE {size} != symbol {truth[start]}"
    # and covers (almost) all .text functions of the code we compiled
    missing = [hex(a) for a in truth if a not in got]
    assert len(missing) <= len(truth) * 0.05, missing


def test_eh_frame_without_sections(fixtures, tmp_path):
    """Without section headers: located via PT_GNU_EH_FRAME and DT_RELA."""
    p = strip_section_headers(fixtures["hidden.stripped"], tmp_path / "nosec.elf")
    img = ElfImage(p)
    assert img.elf.num_sections() == 0
    fdes, loc = eh_frame.collect(img)
    assert loc["method"] == "PT_GNU_EH_FRAME"
    ref, _ = eh_frame.collect(ElfImage(fixtures["hidden.stripped"]))
    assert [(f.start, f.size) for f in fdes] == [(f.start, f.size) for f in ref]
    # Relocations are also read from PT_DYNAMIC
    assert img.num_relative > 0


def test_landing_pads(fixtures):
    """LSDA of .gcc_except_table: landing pads inside eh_catch and eh_cleanup."""
    truth = symtab(fixtures["eh"])
    img = ElfImage(fixtures["eh.stripped"])
    fdes, _ = eh_frame.collect(img)
    pads, errors = eh_frame.landing_pads(img, fdes)
    assert errors == 0
    for name in ("eh_catch", "eh_cleanup"):
        va, size, _ = truth[name]
        assert any(va < x < va + size for x in pads), name
    # Without -fexceptions there is no LSDA
    fdes2, _ = eh_frame.collect(ElfImage(fixtures["hidden.stripped"]))
    assert eh_frame.landing_pads(ElfImage(fixtures["hidden.stripped"]), fdes2)[0] == []


def test_base_address(fixtures):
    img = ElfImage(fixtures["hidden"])
    assert img.link_base == 0 and img.base == DEFAULT_BASE
    assert ElfImage(fixtures["hidden"], base=0).to_abs(0x1234) == 0x1234
    assert ElfImage(fixtures["hidden"], base="0x8000000").to_abs(0x10) == 0x8000010


# --------------------------------------------------------------------- RTTI
def check_rtti(res: dict, truth: dict, img: ElfImage):
    classes = res["classes"]
    for name, (kind, bases) in EXPECTED.items():
        assert name in classes, f"falta {name}; hay {sorted(classes)}"
        c = classes[name]
        assert c["kind"] == kind, name
        assert [(b["name"], b["offset"], b["virtual"]) for b in c["bases"]] == bases, name
        ti = truth.get("_ZTI" + c["mangled"])
        assert ti and int(c["typeinfo"], 16) == img.to_abs(ti[0]), name
        vt = truth.get("_ZTV" + c["mangled"])
        if vt:
            assert "vtable" in c, f"{name}: sin vtable"
            assert int(c["vtable"], 16) == img.to_abs(vt[0]), f"{name}: vtable {c['vtable']} != {vt[0]:#x}"
        else:
            assert "vtable" not in c, name


def vfunc_symbol(truth, img, addr: str) -> set[str]:
    va = img.to_va(int(addr, 16))
    return {n for n, (v, _, t) in truth.items() if v == va and t == "STT_FUNC"}


@pytest.mark.parametrize("variant", VARIANTS)
def test_rtti(fixtures, variant):
    truth = symtab(fixtures[variant])
    img = ElfImage(fixtures[variant + ".stripped"])
    res = rtti.analyze(img)
    check_rtti(res, truth, img)
    c = res["classes"]
    # Virtual entries: Derived = [D1, D0, id, update (inherited), extra]
    vf = c["game::Derived"]["virtual_functions"]
    assert "_ZNK4game7Derived2idEv" in vfunc_symbol(truth, img, vf[2])
    assert "_ZN4game4Base6updateEf" in vfunc_symbol(truth, img, vf[3])
    assert "_ZN4game7Derived5extraEv" in vfunc_symbol(truth, img, vf[4])
    # Multiple inheritance: secondary sub-vtable of Mixin with offset -16 and thunk
    multi = c["game::Multi"]["vtables"]
    assert [v["offset_to_top"] for v in multi] == [0, -16]
    assert "_ZThn16_N4game5Multi3mixEv" in vfunc_symbol(truth, img, multi[1]["entries"][2])
    # Diamond: three sub-vtables and construction vtables detected via VTT
    assert [v["offset_to_top"] for v in c["game::Diamond"]["vtables"]] == [0, -16, -32]
    for base in ("game::Left", "game::Right"):
        cons = c[base].get("construction_vtables", [])
        assert cons and cons[0]["for"] == "game::Diamond", base
    # Pure virtual functions -> __cxa_pure_virtual do not appear in concrete classes
    assert c["game::Impl"]["virtual_functions"][2].startswith("0x")


def test_rtti_heuristic_without_symbols(fixtures):
    """Static libsupc++ without symbols, without using the own RTTI of __cxxabiv1."""
    truth = symtab(fixtures["static"])
    img = ElfImage(fixtures["static.stripped"])
    res = rtti.analyze(img, use_symbols=False, use_self_rtti=False)
    assert {v["how"] for v in res["cxxabi_vptrs"].values()} == {"heuristic"}
    check_rtti(res, truth, img)


def test_rtti_self_identification(fixtures):
    img = ElfImage(fixtures["static.stripped"])
    res = rtti.analyze(img)
    hows = {v["kind"]: v["how"] for v in res["cxxabi_vptrs"].values()}
    assert hows["class"] == hows["si"] == hows["vmi"] == "own __cxxabiv1 RTTI"


def test_vtables_without_rtti(fixtures):
    """-fno-rtti: vtables [0][0][functions] confirmed by ADRP+ADD in code."""
    truth = symtab(fixtures["nortti"])
    img = ElfImage(fixtures["nortti.stripped"])
    res = rtti.analyze(img)
    assert res["classes"] == {}
    found = {int(v["vtable"], 16): v for v in res["vtables_without_rtti"]}
    expected = ["N4game4BaseE", "N4game7DerivedE", "N4game5MultiE", "N4game4ImplE",
                "N4game6HolderIiLi3EEE", "N4game6HolderIfLi4EEE", "N12_GLOBAL__N_19LocalOnlyE"]
    for m in expected:
        va = truth["_ZTV" + m][0]
        assert img.to_abs(va) in found, m
    multi = found[img.to_abs(truth["_ZTVN4game5MultiE"][0])]
    assert [v["offset_to_top"] for v in multi["vtables"]] == [0, -16]
    # No false positives: every vtable found starts at a _ZTV/_ZTC symbol
    starts = {v for n, (v, _, _) in truth.items() if n.startswith(("_ZTV", "_ZTC"))}
    for a, v in found.items():
        aps = {img.to_va(int(sv["address_point"], 16)) for sv in v["vtables"]}
        assert img.to_va(a) in starts or any(ap - 16 in starts or ap - 24 in starts for ap in aps), hex(a)


# --------------------------------------------------------------- jump tables
def test_jumptables(fixtures):
    for variant in VARIANTS:
        truth = symtab(fixtures[variant])
        img = ElfImage(fixtures[variant + ".stripped"])
        tables, _ = jumptables.find_jumptables(img)
        by_func = {}
        for name in ("sw_large", "jt_clang_byte", "jt_clang_half", "jt_clang_word"):
            va, size, _ = truth[name]
            hits = [t for t in tables if va <= t["br"] < va + size]
            assert len(hits) == 1, f"{variant}/{name}: {hits}"
            t = hits[0]
            assert t["bounded"]
            assert all(va <= x < va + size for x in t["targets"]), name
            by_func[name] = t
        assert by_func["sw_large"]["count"] == 40
        assert len(set(by_func["sw_large"]["targets"])) == 40
        # Hand-written clang tables: exact targets
        b = truth["jt_clang_byte"][0]
        assert by_func["jt_clang_byte"]["targets"] == [b + 0x24 + 8 * i for i in range(4)]
        assert by_func["jt_clang_byte"]["entry_size"] == 1
        h = truth["jt_clang_half"][0]
        assert by_func["jt_clang_half"]["targets"] == [h + 0x24 + 8 * i for i in range(3)]
        assert by_func["jt_clang_half"]["entry_size"] == 2
        w = truth["jt_clang_word"][0]
        assert by_func["jt_clang_word"]["targets"] == [w + 0x1c + 8 * i for i in range(4)]
        assert by_func["jt_clang_word"]["entry_size"] == 4


# ------------------------------------------------------------- code targets
def test_code_targets(fixtures, tmp_path):
    truth = symtab(fixtures["hidden"])
    img = ElfImage(fixtures["hidden.stripped"])
    targets, funcptrs, jt, rres, fdes = code_targets.collect(img, verbose=False)
    doc = code_targets.write_outputs(img, targets, funcptrs, tmp_path)
    offs = {t["off"]: t for t in doc["targets"]}
    for fn in text_funcs(fixtures["hidden"]):
        assert fn - img.link_base in offs and offs[fn]["kind"] == "function", hex(fn)
    # g_callbacks: function pointers in data (R_AARCH64_RELATIVE)
    slot = truth["g_callbacks"][0]
    cb = [f for f in funcptrs if slot <= f["slot"] < slot + 24]
    assert len(cb) == 3 and all(f["origin"] == "reloc" and f["reloc"] == "RELATIVE" for f in cb)
    assert {f["target"] for f in cb} == {truth["_ZL4cb_ai"][0], truth["_ZL4cb_bi"][0], truth["sink"][0]}
    plt = [t for t in doc["targets"] if "plt" in t["src"]]
    assert {t["import"] for t in plt} == {"_Znwm", "_ZdlPvm", "__dynamic_cast"}
    # jump table targets as blocks
    assert doc["counts"]["jumptable"] >= 40 + 4 + 3 + 4
    assert json.loads((tmp_path / "all_code_targets.json").read_text())["module"] == "main"
    vt = [f for f in funcptrs if f["origin"] == "vtable"]
    assert vt and all("class" in f for f in vt)


# ------------------------------------------------------------------- merge
def check_viking_csv(path: Path):
    with open(path, newline="") as f:
        lines = f.read().splitlines()
    assert lines[0] == "Address,Quality,Size,Name"
    prev_end = 0
    names = set()
    for line in lines[1:]:
        parts = line.split(",")
        assert len(parts) == 4, line
        a, q, size, name = int(parts[0], 16), parts[1], int(parts[2]), parts[3]
        assert len(parts[0]) == 18 and len(parts[2]) >= 6
        assert q in "OmMUWL"
        assert a >= prev_end, f"solapamiento en {a:#x}"
        prev_end = a + size
        if name:
            assert name not in names, f"nombre duplicado {name}"
            names.add(name)
    return lines[1:]


def test_merge_functions(fixtures, tmp_path):
    img = ElfImage(fixtures["hidden.stripped"])
    out = tmp_path / "main_functions.csv"
    rows, stats = merge_functions.merge(img, rtti.analyze(img), None, None)
    merge_functions.write_csv(rows, out)
    lines = check_viking_csv(out)
    names = [l.split(",")[3] for l in lines]
    assert any(n == "game::Derived::vf4" for n in names)
    assert any(n.startswith("game::Multi::thunk16_vf") for n in names)
    assert any(n.startswith("FUN_71") for n in names)
    # PLT stubs named after their import
    assert {"_Znwm", "_ZdlPvm", "__dynamic_cast"} <= set(names)
    # With symbols: mangled names as viking expects them
    img2 = ElfImage(fixtures["hidden"])
    rows2, _ = merge_functions.merge(img2, rtti.analyze(img2), None, None)
    merge_functions.write_csv(rows2, tmp_path / "sym.csv")
    names2 = {l.split(",")[3] for l in check_viking_csv(tmp_path / "sym.csv")}
    assert "_ZNK4game7Derived2idEv" in names2 or "_ZNK4game7Derived2idEv" in {r[3] for r in rows2}
    # The Quality and human names of the existing CSV are kept
    existing = merge_functions.read_viking_csv(out)
    a0 = sorted(existing)[5]
    existing[a0] = ("O", existing[a0][1], "MiNombre")
    rows3, _ = merge_functions.merge(img, rtti.analyze(img), None, existing)
    r = next(r for r in rows3 if r[0] == a0)
    assert r[1] == "O" and r[3] == "MiNombre"


def test_cli_smoke(fixtures, tmp_path):
    elf = str(fixtures["hidden.stripped"])
    py = sys.executable
    for args in (["eh_frame.py", elf, "--csv", str(tmp_path / "eh.csv")],
                 ["rtti.py", elf, "-o", str(tmp_path / "rtti.json")],
                 ["jumptables.py", elf, "-o", str(tmp_path / "jt.json")],
                 ["code_targets.py", elf, "--out-dir", str(tmp_path / "ct")],
                 ["merge_functions.py", elf, "-o", str(tmp_path / "f.csv"), "--out-dir", str(tmp_path),
                  "--rtti", str(tmp_path / "rtti.json")]):
        subprocess.run([py, str(ANALYSIS / args[0]), *args[1:]], check=True, capture_output=True)
    check_viking_csv(tmp_path / "f.csv")
    assert (tmp_path / "ct" / "all_code_targets.json").exists()


# ------------------------------------------------------------ Ghidra (optional)
@pytest.mark.skipif(os.environ.get("RUN_GHIDRA_TESTS") != "1", reason="RUN_GHIDRA_TESTS=1 to enable it")
def test_ghidra_headless(fixtures, tmp_path):
    elf = fixtures["hidden.stripped"]
    out = tmp_path / "out"
    subprocess.run([sys.executable, str(REPO / "tools" / "ghidra" / "analyze.py"), str(elf),
                    "--project-dir", str(tmp_path / "proj"), "--out-dir", str(out),
                    "--functions-csv", str(out / "ghidra_functions.csv")], check=True)
    lines = check_viking_csv(out / "ghidra_functions.csv")
    assert len(lines) >= 50
    names = {l.split(",")[3] for l in lines}
    assert any("game::Derived" in n for n in names), "no RTTI names were applied"
    assert (out / "strings.tsv").exists()
