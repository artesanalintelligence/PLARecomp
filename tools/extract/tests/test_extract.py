"""Tests for nxfmt.py and extract.py with synthetic NCAs/NSPs (random test keys)."""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

import pytest

import extract
import nso2elf
import nxfmt as nx
import synth

TID = 0x0100000000ABC000


# ------------------------------------------------------------------ cripto

def test_xts_ieee1619_vectors():
    # IEEE P1619 / XTS-AES-128, vectors 1 and 2
    k = bytes(32)
    ct = bytes.fromhex("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e")
    assert nx.xts_decrypt(k, ct, bytes(16)) == bytes(32)
    k = bytes([0x11] * 16 + [0x22] * 16)
    tweak = (0x3333333333).to_bytes(16, "little")
    ct = bytes.fromhex("c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0")
    assert nx.xts_decrypt(k, ct, tweak) == bytes([0x44] * 32)


def test_xts_roundtrip_nintendo_tweak():
    key = os.urandom(32)
    data = os.urandom(0xC00)
    assert nx.nintendo_xts_decrypt(key, synth.nintendo_xts_encrypt(key, data), 0) == data


# ------------------------------------------------------------------ contenedores

def test_pfs0_hfs0_parse():
    files = [("a.bin", b"hello"), ("b/c.txt", b"x" * 100), ("empty", b"")]
    for maker, parser in ((synth.pfs0, nx.parse_pfs0), (synth.hfs0, nx.parse_hfs0)):
        reg = nx.BytesRegion(maker(files), "t")
        got = parser(reg)
        assert [(f.name, f.read(0, f.size)) for f in got] == files


def test_ticket_and_cnmt():
    keys = synth.TestKeys()
    rid = bytes(range(16))
    ck = os.urandom(16)
    t = nx.parse_ticket(synth.make_ticket(keys, rid, ck))
    assert t.rights_id == rid and not t.is_personalized
    ks = nx.KeySet(prod={"titlekek_00": keys.titlekek})
    assert nx.aes_ecb_decrypt(keys.titlekek, nx.ticket_titlekey(t, ks)) == ck
    c = nx.parse_cnmt(synth.make_cnmt(TID | 0x800, 65536, 0x81, [("00" * 16, 1234, 1)]))
    assert c.title_id == TID | 0x800 and c.version == 65536 and c.type_name == "Patch"
    assert c.contents[0].size == 1234 and c.contents[0].ctype == 1


def _keyset(keys: synth.TestKeys) -> nx.KeySet:
    return nx.KeySet(prod={"header_key": keys.header_key, "key_area_key_application_00": keys.kaek,
                           "titlekek_00": keys.titlekek})


def test_nca_keyarea_and_titlekey():
    keys = synth.TestKeys()
    ks = _keyset(keys)
    exefs = [("main", b"M" * 5000), ("main.npdm", synth.make_npdm("Application", TID))]
    nca = nx.Nca(nx.BytesRegion(synth.make_nca(keys, 0, TID, [("pfs0", synth.pfs0(exefs))])), ks)
    assert nca.type_name == "Program" and nca.program_id == TID and not nca.has_rights_id
    assert [(f.name, f.read(0, f.size)) for f in nx.nca_pfs0_files(nca)] == exefs

    rid = bytes.fromhex("0100000000abc8000000000000000000")
    ck = os.urandom(16)
    blob = synth.make_nca(keys, 0, TID, [("pfs0", synth.pfs0(exefs))], rights_id=rid, content_key=ck)
    nca = nx.Nca(nx.BytesRegion(blob), ks)
    with pytest.raises(nx.KeyError_, match="titlekey"):
        nx.nca_pfs0_files(nca)[0].read(0, 4)
    t = nx.parse_ticket(synth.make_ticket(keys, rid, ck))
    ks.add_titlekey(t.rights_id, nx.ticket_titlekey(t, ks))
    nca = nx.Nca(nx.BytesRegion(blob), ks)
    assert nx.nca_pfs0_files(nca)[0].read(0, 5000) == b"M" * 5000


def test_nca_wrong_header_key():
    keys = synth.TestKeys()
    blob = synth.make_nca(keys, 0, TID, [("pfs0", synth.pfs0([("main", b"x")]))])
    bad = _keyset(synth.TestKeys())
    with pytest.raises(nx.KeyError_, match="header_key"):
        nx.Nca(nx.BytesRegion(blob), bad)


def test_romfs_control_nacp():
    keys = synth.TestKeys()
    nacp = synth.make_nacp("Game", "1.2.3")
    blob = synth.make_nca(keys, 2, TID, [("romfs", synth.romfs_single("control.nacp", nacp))])
    nca = nx.Nca(nx.BytesRegion(blob), _keyset(keys))
    f = nx.romfs_find(nca.section_view(0), "control.nacp")
    data = f.read(0, f.size)
    assert nx.nacp_display_version(data) == "1.2.3" and nx.nacp_title(data) == "Game"


# ------------------------------------------------------------------ extract.py end to end

@pytest.fixture()
def env(tmp_path, built_rela, built_relr):
    keys = synth.TestKeys()
    kdir = tmp_path / "keys"
    kdir.mkdir()
    keys.write(kdir / "prod.keys")
    dump = tmp_path / "dump"
    dump.mkdir()
    out = tmp_path / "data"
    base_main = built_relr[1].read_bytes()
    upd_main = built_rela[1].read_bytes()
    npdm = synth.make_npdm("Application", TID)
    exefs_base = [("main", base_main), ("main.npdm", npdm), ("rtld", base_main), ("sdk", base_main)]
    exefs_upd = [("main", upd_main), ("main.npdm", npdm), ("rtld", base_main), ("sdk", base_main),
                 ("subsdk0", upd_main)]
    return dict(keys=keys, kdir=kdir, dump=dump, out=out, exefs_base=exefs_base, exefs_upd=exefs_upd,
                upd_main=upd_main, base_main=base_main)


def _run(env, *extra):
    args = ["--dump", str(env["dump"]), "--out", str(env["out"]), "--keys", str(env["kdir"] / "prod.keys"),
            "--title-keys", str(env["kdir"] / "title.keys"), "--title-id", f"{TID:016X}",
            "--expect-version", "1.1.1", "--tool", "none", *extra]
    return extract.main(args)


def _write_base(env):
    (env["dump"] / "base.nsp").write_bytes(
        synth.make_title_nsp(env["keys"], TID, 0, "1.0.0", env["exefs_base"], patch=False))


def _write_update(env, version=262144, display="1.1.1", with_ticket=True):
    (env["dump"] / "update.nsp").write_bytes(
        synth.make_title_nsp(env["keys"], TID, version, display, env["exefs_upd"], patch=True,
                             with_ticket=with_ticket))


def test_extract_base_plus_update(env, capsys):
    _write_base(env)
    _write_update(env)
    assert _run(env) == 0
    out = env["out"]
    assert (out / "exefs" / "main").read_bytes() == env["upd_main"]
    assert sorted(p.name for p in (out / "exefs").iterdir()) == ["main", "main.npdm", "rtld", "sdk", "subsdk0"]
    assert (out / "main.nso").read_bytes() == env["upd_main"]
    assert (out / "main.elf").read_bytes()[:4] == b"\x7fELF"
    unc = nso2elf.parse_nso((out / "main.uncompressed.nso").read_bytes())
    assert unc.flags & 7 == 0 and unc.text.data == nso2elf.parse_nso(env["upd_main"]).text.data
    doc = json.loads((out / "target.json").read_text(encoding="utf-8"))
    assert doc["version"] == {"display": "1.1.1", "number": 262144, "source": "update"}
    assert doc["base_version"]["display"] == "1.0.0"
    assert doc["program_nca"]["rights_id"] is True
    nso = nso2elf.parse_nso(env["upd_main"])
    m = doc["exefs"]["main"]
    assert m["build_id"] == nso.build_id
    assert m["segments"]["text"]["size"] == nso.text.size
    assert doc["exefs"]["main.npdm"]["program_id"] == f"{TID:016X}"
    assert doc["main_elf"]["resolve_got"] is True
    assert not (env["dump"] / extract.WORKDIR_NAME).exists()


def test_extract_from_xci_and_update(env):
    nsp = nx.parse_pfs0(nx.BytesRegion(
        synth.make_title_nsp(env["keys"], TID, 0, "1.0.0", env["exefs_base"], patch=False)))
    (env["dump"] / "game.xci").write_bytes(synth.make_xci([(f.name, f.read(0, f.size)) for f in nsp]))
    _write_update(env)
    assert _run(env) == 0
    assert (env["out"] / "main.nso").read_bytes() == env["upd_main"]


def test_missing_update_is_an_error(env, capsys):
    _write_base(env)
    assert _run(env) == 1
    err = capsys.readouterr().err
    assert "not found in dump/" in err
    assert _run(env, "--allow-base") == 0
    assert (env["out"] / "main.nso").read_bytes() == env["base_main"]
    doc = json.loads((env["out"] / "target.json").read_text(encoding="utf-8"))
    assert doc["version"]["source"] == "base"


def test_wrong_version_is_an_error(env, capsys):
    _write_base(env)
    _write_update(env, version=196608, display="1.1.0")
    assert _run(env) == 1
    assert "1.1.0" in capsys.readouterr().err
    assert _run(env, "--any-version") == 0


def test_missing_ticket_is_an_error(env, capsys):
    _write_base(env)
    _write_update(env, with_ticket=False)
    assert _run(env) == 1
    assert "titlekey" in capsys.readouterr().err


def test_missing_keys_is_an_error(env, capsys):
    _write_base(env)
    os.remove(env["kdir"] / "prod.keys")
    assert _run(env) == 1
    assert "prod.keys" in capsys.readouterr().err


def test_empty_dump(env, capsys):
    assert _run(env) == 1
    assert "No game found" in capsys.readouterr().err


def test_exefs_folder_input(env):
    d = env["dump"] / "exefs"
    d.mkdir()
    for name, blob in env["exefs_upd"]:
        (d / name).write_bytes(blob)
    assert _run(env) == 0
    assert (env["out"] / "main.nso").read_bytes() == env["upd_main"]


def test_loose_ncas_with_tik(env):
    _write_base(env)
    _write_update(env)
    loose = env["dump"] / "loose"
    loose.mkdir()
    for nsp in ("base.nsp", "update.nsp"):
        p = env["dump"] / nsp
        for f in nx.parse_pfs0(nx.FileRegion(p, 0, p.stat().st_size, p.name)):
            (loose / f.name).write_bytes(f.read(0, f.size))
        p.unlink()
    assert _run(env) == 0
    assert (env["out"] / "main.nso").read_bytes() == env["upd_main"]


def test_extract_verifies_with_hactool(env):
    tool = extract.find_tool("auto")
    if not tool:
        pytest.skip("no hactool/hactoolnet in tools/bin (python tools/extract/bootstrap.py)")
    _write_base(env)
    _write_update(env)
    # hactool also needs the area keys (the test ones will do)
    args = ["--dump", str(env["dump"]), "--out", str(env["out"]), "--keys", str(env["kdir"] / "prod.keys"),
            "--title-keys", str(env["kdir"] / "title.keys"), "--title-id", f"{TID:016X}",
            "--expect-version", "1.1.1", "--tool", tool.kind]
    assert extract.main(args) == 0
    doc = json.loads((env["out"] / "target.json").read_text(encoding="utf-8"))
    assert doc["verified_with"] == tool.kind
