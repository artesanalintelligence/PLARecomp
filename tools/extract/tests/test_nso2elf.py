"""Tests for nso2elf with a real module compiled with devkitA64 (ELF -> elf2nso -> NSO -> nso2elf -> ELF)."""

from __future__ import annotations

import hashlib
import struct
import subprocess
import sys

import pytest

import nso2elf
from elfutil import Elf

SHF_ALLOC = 2
SHT_NOBITS = 8
STT_TLS = 6


@pytest.fixture(scope="module")
def conv(built, tmp_path_factory):
    elf_path, nso_path = built
    nso = nso2elf.parse_nso(nso_path.read_bytes())
    mod = nso2elf.analyze(nso)
    out = tmp_path_factory.mktemp("conv") / "out.elf"
    out.write_bytes(nso2elf.build_elf(nso, mod=mod))
    return Elf.load(elf_path), Elf.load(out), nso, mod


# ---------------------------------------------------------------- NSO

def test_nso_segments_match_elf_loads(conv):
    orig, _, nso, _ = conv
    loads = [p for p in orig.phdrs if p.type == 1]
    assert len(loads) == 3
    for seg, ph in zip(nso.segments, loads):
        assert seg.mem_offset == ph.vaddr
        assert seg.size == ph.filesz
        assert seg.data == orig.data[ph.offset:ph.offset + ph.filesz]
        assert hashlib.sha256(seg.data).digest() == seg.sha256
        assert seg.compressed  # elf2nso compresses with LZ4
    assert not nso.warnings


def test_build_id_matches_note(conv):
    orig, _, nso, _ = conv
    note = orig.section(".note.gnu.build-id")
    desc = orig.section_bytes(note)[16:]
    assert nso.module_id.startswith(desc)
    assert nso.build_id_short == desc.hex().upper()


def test_bad_hash_is_detected(built):
    _, nso_path = built
    blob = bytearray(nso_path.read_bytes())
    blob[0xA0] ^= 0xFF  # corrupts the SHA-256 of .text in the header
    with pytest.raises(nso2elf.NsoError, match="SHA-256"):
        nso2elf.parse_nso(bytes(blob))
    assert nso2elf.parse_nso(bytes(blob), strict=False).warnings


def test_uncompressed_roundtrip(conv):
    _, _, nso, _ = conv
    raw = nso2elf.build_uncompressed_nso(nso)
    again = nso2elf.parse_nso(raw)
    assert again.flags & 7 == 0
    for a, b in zip(nso.segments, again.segments):
        assert a.data == b.data and a.mem_offset == b.mem_offset
    assert again.module_id == nso.module_id


# ---------------------------------------------------------------- bytes

def test_all_alloc_sections_identical(conv):
    """Each section with content of the original ELF is byte for byte in the converted one."""
    orig, new, _, _ = conv
    checked = 0
    for s in orig.shdrs:
        if not (s.flags & SHF_ALLOC) or s.type == SHT_NOBITS or s.size == 0 or s.flags & 0x400:
            continue  # 0x400 = SHF_TLS
        assert new.read(s.addr, s.size) == orig.section_bytes(s), s.name
        checked += 1
    assert checked >= 15


@pytest.mark.parametrize("name", [".text", ".rodata", ".data"])
def test_main_sections_byte_for_byte(conv, name):
    orig, new, _, _ = conv
    o, n = orig.section(name), new.section(name)
    assert n is not None, f"falta {name}"
    # the rebuilt section covers the original (at most it adds alignment padding)
    assert n.addr <= o.addr and o.addr + o.size <= n.addr + n.size
    assert n.addr + n.size - (o.addr + o.size) < 0x1000 and o.addr - n.addr < 0x1000
    assert new.section_bytes(n) == orig.read(n.addr, n.size)
    assert new.section_bytes(n)[o.addr - n.addr:o.addr - n.addr + o.size] == orig.section_bytes(o)


@pytest.mark.parametrize("name", [".dynamic", ".dynsym", ".dynstr", ".hash", ".gnu.hash", ".plt", ".got",
                                  ".got.plt", ".init_array", ".fini_array", ".eh_frame", ".note.gnu.build-id",
                                  ".init", ".fini"])
def test_structural_sections_located(conv, name):
    orig, new, _, _ = conv
    o, n = orig.section(name), new.section(name)
    assert o is not None and n is not None, name
    assert n.addr == o.addr, name
    if name in (".init", ".fini"):
        assert 0 < n.size <= o.size  # up to the first ret/b; the original includes padding
    else:
        assert n.size == o.size, name
    assert new.section_bytes(n) == orig.read(n.addr, n.size)


def test_relocation_sections(conv):
    orig, new, nso, mod = conv
    od = orig.dynamic()
    rela_plt = new.section(".rela.plt")
    assert rela_plt.addr == od[23] and rela_plt.size == od[2]  # DT_JMPREL / DT_PLTRELSZ
    assert new.shdrs[rela_plt.link].name == ".dynsym"
    assert new.shdrs[rela_plt.info].name == ".plt"
    if 36 in od:  # DT_RELR
        relr = new.section(".relr.dyn")
        assert relr.addr == od[36] and relr.size == od[35]
        assert relr.addr == orig.section(".relr.dyn").addr


# ---------------------------------------------------------------- symbols

def _symset(elf):
    return sorted((s.name, s.value, s.size, s.info, s.other, s.shndx) for s in elf.dynsyms())


def test_dynamic_symbols_recovered(conv):
    orig, new, _, _ = conv
    assert _symset(new) == _symset(orig)
    names = {s.name: s for s in new.dynsyms()}
    for want in ("tst_square", "tst_cube", "tst_hash", "gTable", "gCounter", "gZeroed", "main",
                 "_Z12tst_dispatchPKN3tst4BaseEi", "_ZTVN3tst7DerivedE", "_ZTIN3tst5OtherE",
                 "_ZTSN3tst4BaseE"):
        assert want in names, want
        assert names[want].shndx != 0
    imp = names["nx_test_import_func"]
    assert imp.shndx == 0  # unresolved import (like the SDK functions in a real NSO)
    assert names["nx_test_import_data"].shndx == 0


def test_symbol_section_indices_consistent(conv):
    """st_shndx of each symbol points to a section of the new ELF that contains its address."""
    _, new, _, _ = conv
    for s in new.dynsyms():
        if s.shndx == 0 or s.shndx >= 0xFF00 or s.type == STT_TLS:
            continue
        sec = new.shdrs[s.shndx]
        assert sec.type != 0, (s.name, s.shndx)
        assert sec.addr <= s.value <= sec.addr + sec.size, (s.name, sec.name)


def test_function_bytes_by_symbol(conv):
    orig, new, _, _ = conv
    for s in orig.dynsyms():
        if s.type == 2 and s.shndx != 0 and s.size:  # STT_FUNC definida
            assert new.read(s.value, s.size) == orig.read(s.value, s.size), s.name


# ---------------------------------------------------------------- estructura

def test_program_headers(conv):
    orig, new, nso, mod = conv
    loads = [p for p in new.phdrs if p.type == 1]
    assert [p.flags for p in loads] == [5, 4, 6]
    for p in loads:
        assert p.offset % 0x1000 == p.vaddr % 0x1000
    data = loads[2]
    bss = new.section(".bss")
    assert data.vaddr + data.memsz >= bss.addr + bss.size
    obss = orig.section(".bss")
    assert bss.addr <= obss.addr and obss.addr + obss.size <= bss.addr + bss.size
    dyn = next(p for p in new.phdrs if p.type == 2)
    assert dyn.vaddr == next(p for p in orig.phdrs if p.type == 2).vaddr
    assert new.type == 3 and new.machine == 183


def test_viking_requirements(conv):
    """What viking (nx-decomp-tools) needs from data/main.elf."""
    _, new, _, mod = conv
    dyn = new.dynamic()
    assert 4 in dyn, "viking requires DT_HASH"
    nchain = struct.unpack_from("<I", new.data, new.vaddr_to_offset(dyn[4]) + 4)[0]
    assert nchain == len(new.dynsyms())
    rela = new.section(".rela.dyn")
    assert rela is not None and new.shdrs[rela.link].type == 11
    plt = new.section(".plt")
    assert new.section(".got") is not None
    # get_plt_functions(): plt.sh_addr + (idx + 2) * 0x10 for each JUMP_SLOT
    jm = mod.relocs("JMPREL", "PLTRELSZ")
    syms = new.dynsyms()
    for idx, (off, sym, typ, add) in enumerate(jm):
        addr = plt.addr + (idx + 2) * 0x10
        insn = struct.unpack_from("<I", new.read(addr, 4))[0]
        assert insn & 0x9F00001F == 0x90000010  # adrp x16
        assert syms[sym].name
    assert any(syms[r[1]].name == "nx_test_import_func" for r in jm)


def test_base_address(conv, built):
    _, _, nso, _ = conv
    base = 0x7100000000
    e = Elf(nso2elf.build_elf(nso, base=base))
    loads = [p for p in e.phdrs if p.type == 1]
    assert loads[0].vaddr == base and e.entry == base
    assert e.section(".text").addr >= base
    t = e.section(".text")
    assert e.section_bytes(t) == nso.text.data[t.addr - base:t.addr - base + t.size]
    for s in e.dynsyms():
        if s.shndx and s.shndx < 0xFF00 and s.type != STT_TLS:
            assert s.value >= base, s.name
    d = e.dynamic()
    assert d[6] >= base and d[5] >= base  # DT_SYMTAB, DT_STRTAB rebasados


def test_resolve_got(built_rela):
    elf_path, nso_path = built_rela
    nso = nso2elf.parse_nso(nso_path.read_bytes())
    mod = nso2elf.analyze(nso)
    e = Elf(nso2elf.build_elf(nso, resolve_got=True, mod=mod))
    got = e.section(".got")
    n = 0
    for off, sym, typ, add in mod.relocs("RELA", "RELASZ"):
        if got.addr <= off < got.addr + got.size and typ == nso2elf.R_AARCH64_RELATIVE:
            assert struct.unpack("<Q", e.read(off, 8))[0] == add
            n += 1
    assert n > 0
    # nothing outside .got is touched
    t = e.section(".data")
    assert e.section_bytes(t) == nso.data.data[t.addr - nso.data.mem_offset:t.addr - nso.data.mem_offset + t.size]


def test_cli(built, tmp_path):
    _, nso_path = built
    out = tmp_path / "x.elf"
    unc = tmp_path / "x.nso"
    r = subprocess.run([sys.executable, str(nso2elf.__file__), str(nso_path), str(out),
                        "--export-uncompressed", str(unc)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert out.read_bytes()[:4] == b"\x7fELF"
    assert nso2elf.parse_nso(unc.read_bytes()).flags & 7 == 0


def test_readelf_accepts_output(conv, tmp_path):
    from conftest import DEVKITPRO
    readelf = DEVKITPRO / "devkitA64" / "bin" / "aarch64-none-elf-readelf.exe"
    if not readelf.is_file():
        pytest.skip("no readelf")
    _, new, _, _ = conv
    p = tmp_path / "r.elf"
    p.write_bytes(new.data)
    r = subprocess.run([str(readelf), "-a", "-W", str(p)], capture_output=True, text=True)
    assert r.returncode == 0
    assert "warning" not in r.stderr.lower() and "error" not in r.stderr.lower(), r.stderr
