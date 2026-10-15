#!/usr/bin/env python3
"""Writes the game's effective RomFS (the update's BKTR patch applied over the base game) as one
romfs.bin, without external tools or an emulator.

The result is the same file the fork's export dialog writes to <package>/exefs/romfs.bin
(PatchManager::PatchRomFS over the base program NCA): the update's program NCA carries a patch
RomFS section whose IVFC data level lives in a virtual "indirect" storage. Each indirect entry maps
a virtual range either to the base program NCA's RomFS section (storage 0) or to the update's own
section data (storage 1), which is AES-CTR encrypted with a per-range counter generation
(the AesCtrEx bucket tree). Both trees sit at the end of the update's section, under plain
AES-CTR.

  python tools/extract/romfs_bktr.py --dump <folder with base + update> --out <romfs.bin>
         [--title-id 01001F5010DFA000] [--keys prod.keys] [--title-keys title.keys]

The base program may be an NCA inside an NSP/XCI or an NCZ inside an NSZ (read through its zstd
stream; nothing is decompressed to disk). The update must be uncompressed (it is small).
"""
from __future__ import annotations

import argparse
import bisect
import struct
import sys
import time
from pathlib import Path
from typing import BinaryIO, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nxfmt as nx  # noqa: E402

try:
    from Crypto.Cipher import AES
except ImportError:  # pragma: no cover
    AES = None

NODE_SIZE = 0x4000
CHUNK = 16 << 20


class BktrError(Exception):
    pass


def _bucket_entries(table: bytes, entry_size: int, count: int) -> List[bytes]:
    """Entries of a bucket tree (one L1 offset node, then 0x4000-byte entry sets)."""
    per_set = (NODE_SIZE - 0x10) // entry_size
    sets = (count + per_set - 1) // per_set
    offsets_per_node = (NODE_SIZE - 0x10) // 8
    if sets > offsets_per_node:
        raise BktrError(f"bucket tree with {sets} entry sets needs L2 nodes (not supported)")
    out: List[bytes] = []
    for i in range(sets):
        base = NODE_SIZE + i * NODE_SIZE
        idx, n, _end = struct.unpack_from("<IIq", table, base)
        if idx != i:
            raise BktrError(f"bucket tree entry set {i} has index {idx}")
        for j in range(n):
            p = base + 0x10 + j * entry_size
            out.append(table[p:p + entry_size])
    if len(out) != count:
        raise BktrError(f"bucket tree has {len(out)} entries, header says {count}")
    return out


class PatchedRomfs:
    """Virtual RomFS of an update program NCA over the base program NCA."""

    def __init__(self, base: nx.Nca, patch: nx.Nca, base_reader=None):
        self.base, self.patch = base, patch
        self.bsec = next((s for s in base.sections if s.fs_type == 0), None)
        self.psec = next((s for s in patch.sections if s.fs_type == 0), None)
        if not self.bsec or not self.psec:
            raise BktrError("base or update program NCA has no RomFS section")
        h = self.psec.raw_header
        if any(h[0x148:0x1A0]):
            raise BktrError("the update's RomFS section uses sparse or compressed storage (not supported)")
        if self.psec.enc_type not in (nx.ENC_CTREX, nx.ENC_CTREX_SKIP):
            raise BktrError(f"the update's RomFS section is not a BKTR patch (encryption {self.psec.enc_type})")
        if any(self.bsec.raw_header[0x148:0x1A0]):
            raise BktrError("the base RomFS section uses sparse or compressed storage (not supported)")
        ind_off, ind_size, ind_magic, _v, ind_count = struct.unpack_from("<QQ4sII", h, 0x100)
        aes_off, aes_size, aes_magic, _v2, aes_count = struct.unpack_from("<QQ4sII", h, 0x120)
        if ind_magic != b"BKTR" or aes_magic != b"BKTR":
            raise BktrError("bad BKTR headers in the update's RomFS section")
        self.upper = self.psec.upper_counter
        # Both trees are plain AES-CTR in the section (read_section with a CTR section works).
        meta_sec = nx.FsSection(self.psec.index, self.psec.start, self.psec.end, 0, 0, nx.ENC_CTR,
                                self.psec.upper_counter, self.psec.raw_header)
        ind = patch.read_section(meta_sec, ind_off, ind_size)
        aes = patch.read_section(meta_sec, aes_off, aes_size)
        self.ind = [struct.unpack("<qqi", e) for e in _bucket_entries(ind, 0x14, ind_count)]
        self.ind_end = struct.unpack_from("<q", ind, 8)[0]
        self.aes = []
        for e in _bucket_entries(aes, 0x10, aes_count):
            off, enc, gen = struct.unpack("<qB3xi", e)
            self.aes.append((off, enc, gen & 0xFFFFFFFF))
        self.aes_end = aes_off
        self.ind_starts = [e[0] for e in self.ind]
        self.aes_starts = [e[0] for e in self.aes]
        self.data_off, self.size = self.psec.data_region()
        self.base_reader = base_reader  # callable(off, size) -> bytes, for an NCZ base

    # -- storages --
    def _read_base(self, off: int, size: int, f: BinaryIO) -> bytes:
        if self.base_reader:
            return self.base_reader(off, size)
        return self.base.read_section(self.bsec, off, size, f)

    def _read_patch(self, off: int, size: int, f: BinaryIO) -> bytes:
        out = bytearray()
        i = bisect.bisect_right(self.aes_starts, off) - 1
        if i < 0:
            raise BktrError(f"AesCtrEx tree does not cover {off:#x}")
        key = self.patch.content_key()
        while size:
            start, enc, gen = self.aes[i]
            end = self.aes[i + 1][0] if i + 1 < len(self.aes) else self.aes_end
            n = min(size, end - off)
            if n <= 0:
                raise BktrError(f"AesCtrEx tree does not cover {off:#x}")
            abs_off = self.psec.start + off
            if enc == 1:  # not encrypted
                out += self.patch.region.read(abs_off, n, f)
            else:
                aligned = abs_off & ~0xF
                pad = abs_off - aligned
                raw = self.patch.region.read(aligned, (pad + n + 0xF) & ~0xF, f)
                upper = (self.upper & 0xFFFFFFFF00000000) | gen
                iv = struct.pack(">QQ", upper, aligned >> 4)
                out += AES.new(key, AES.MODE_CTR, nonce=b"", initial_value=iv).decrypt(raw)[pad:pad + n]
            off += n
            size -= n
            i += 1
        return bytes(out)

    def pieces(self, voff: int, size: int):
        """(storage, physical offset, length) pieces of the virtual range."""
        i = bisect.bisect_right(self.ind_starts, voff) - 1
        while size:
            v, p, st = self.ind[i]
            end = self.ind[i + 1][0] if i + 1 < len(self.ind) else self.ind_end
            n = min(size, end - voff)
            if n <= 0:
                raise BktrError(f"indirect tree does not cover {voff:#x}")
            yield st, p + (voff - v), n
            voff += n
            size -= n
            i += 1

    def write(self, out: Path, progress=None, base_ncz: Optional[nx.FileRegion] = None) -> int:
        """Writes the RomFS. With base_ncz (the base program as an NCZ member of an NSZ), the base ranges
        are taken from one sequential pass over its zstd stream and scattered to their output positions."""
        tmp = out.with_name(out.name + ".part")
        out.parent.mkdir(parents=True, exist_ok=True)
        done = 0
        pieces, pos = [], 0
        for st, p, n in self.pieces(self.data_off, self.size):
            pieces.append((st, p, n, pos))
            pos += n
        with self.base.region.open() as fb, self.patch.region.open() as fp, open(tmp, "wb") as o:
            o.truncate(self.size)
            if base_ncz is not None:
                # First the base ranges, in stream order (almost the output order): writing far past the
                # end of the written data makes NTFS zero-fill the gap, i.e. write the file twice.
                want = sorted((self.bsec.start + p, n, opos) for st, p, n, opos in pieces if st == 0)
                for a, n, opos, data in ncz_stream_ranges(base_ncz, want):
                    o.seek(opos)
                    o.write(data)
                    done += len(data)
                    if progress:
                        progress(done, self.size)
            for st, p, n, opos in pieces:
                if st == 0 and base_ncz is not None:
                    continue
                o.seek(opos)
                while n:
                    k = min(n, CHUNK)
                    o.write(self._read_base(p, k, fb) if st == 0 else self._read_patch(p, k, fp))
                    p += k
                    n -= k
                    done += k
                    if progress:
                        progress(done, self.size)
        if tmp.stat().st_size != self.size or done != self.size:
            raise BktrError("short write")
        tmp.replace(out)
        return self.size


NCZ_BODY = 0x4000


def ncz_stream_ranges(region: nx.FileRegion, wanted):
    """Decompresses an NCZ's body once, in order, and yields (nca offset, length, output position, bytes) for
    every wanted (nca offset, length, output position) range, sorted by nca offset; ranges may overlap.
    NCZ bodies hold the NCA's sections already decrypted (nsz format), from NCA offset 0x4000 on."""
    import zstandard
    magic, count = struct.unpack("<8sQ", region.read(NCZ_BODY, 0x10))
    if magic != b"NCZSECTN":
        raise BktrError(f"{region.name}: not an NCZ")
    pos = NCZ_BODY + 0x10 + 0x40 * count
    if region.read(pos, 8) == b"NCZBLOCK":
        raise BktrError(f"{region.name}: block-compressed NCZ (not supported here; decompress it with nsz -D)")
    pending = list(wanted)
    active = []  # [start, end, output position]
    i = 0
    cur = NCZ_BODY
    with region.open() as f:
        f.seek(region.offset + pos)

        class _Src:
            def __init__(self, left):
                self.left = left

            def read(self, n=-1):
                n = self.left if n is None or n < 0 else min(n, self.left)
                b = f.read(n)
                self.left -= len(b)
                return b

        with zstandard.ZstdDecompressor().stream_reader(_Src(region.size - pos), read_size=1 << 20) as s:
            while i < len(pending) or active:
                if not active and i < len(pending) and pending[i][0] > cur:
                    # skip ahead to the next wanted range
                    skip = pending[i][0] - cur
                    while skip:
                        b = s.read(min(skip, CHUNK))
                        if not b:
                            raise BktrError(f"{region.name}: stream ends before {pending[i][0]:#x}")
                        skip -= len(b)
                        cur += len(b)
                b = s.read(CHUNK)
                if not b:
                    raise BktrError(f"{region.name}: stream ends at {cur:#x}")
                end = cur + len(b)
                while i < len(pending) and pending[i][0] < end:
                    a, n, opos = pending[i]
                    if a < cur:
                        raise BktrError("NCZ range already passed")
                    active.append([a, a + n, opos])
                    i += 1
                keep = []
                for r in active:
                    lo, hi = max(r[0], cur), min(r[1], end)
                    if lo < hi:
                        yield lo, hi - lo, r[2] + (lo - r[0]), b[lo - cur:hi - cur]
                    if r[1] > end:
                        keep.append(r)
                active = keep
                cur = end


def find_programs(dumps: List[Path], tid: int, keys_path: Optional[Path], title_keys: Optional[Path]):
    """Base and update program NCAs from folders and/or container files. NSZ members are taken as they
    are (an .ncz keeps the original NCA header); an uncompressed copy of the same content wins.
    Returns (base Nca, update Nca, base NCZ region or None)."""
    import extract as ex  # noqa: E402 (tools/extract)
    inv = ex.Inventory()
    for d in dumps:
        if d.is_dir():
            sub = ex.scan_dump(d, d / ".romfs-work")
            inv.ncas += sub.ncas
            inv.tickets += sub.tickets
            files = sub.compressed
        else:
            files = [d]
        for p in files:
            ext = p.suffix.lower()
            if ext in (".nsp", ".nsz"):
                for f in nx.parse_pfs0(nx.FileRegion(p, 0, p.stat().st_size, p.name)):
                    low = f.name.lower()
                    if low.endswith((".nca", ".ncz")):
                        inv.ncas.append(ex.NcaEntry(f, p))
                    elif low.endswith(".tik"):
                        inv.tickets.append((f, p))
            elif ext == ".xci":
                for part, members in nx.parse_xci(p).items():
                    if part != "update":
                        for f in members:
                            if f.name.lower().endswith(".nca"):
                                inv.ncas.append(ex.NcaEntry(f, p))
            elif ext == ".nca":
                inv.ncas.append(ex.NcaEntry(nx.FileRegion(p, 0, p.stat().st_size, p.name), p))
    # one entry per content id, uncompressed first
    seen, uniq = set(), []
    for e in sorted(inv.ncas, key=lambda e: e.region.name.lower().endswith(".ncz")):
        cid = e.content_id or id(e)
        if cid not in seen:
            seen.add(cid)
            uniq.append(e)
    inv.ncas = uniq
    keys = ex.load_keys(keys_path, title_keys)
    for reg, _src in inv.tickets:
        try:
            t = nx.parse_ticket(reg.read(0, reg.size))
            keys.add_titlekey(t.rights_id, nx.ticket_titlekey(t, keys))
        except (nx.FormatError, nx.KeyError_):
            pass
    titles = ex.read_cnmts(ex.open_ncas(inv, keys))
    base = next((t for t in titles if t.cnmt.title_id == tid and t.cnmt.meta_type == 0x80), None)
    upds = sorted((t for t in titles if t.cnmt.title_id == tid + 0x800 and t.cnmt.meta_type == 0x81),
                  key=lambda t: t.cnmt.version)
    bp = base.contents.get(1) if base else None
    up = upds[-1].contents.get(1) if upds else None
    if up and up.region.name.lower().endswith(".ncz"):
        raise BktrError("the update is compressed (NSZ): decompress it first (nsz -D); it is small")
    ncz = bp.region if bp and bp.region.name.lower().endswith(".ncz") else None
    return (bp.nca if bp else None), (up.nca if up else None), ncz


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dump", type=Path, required=True, action="append",
                    help="folder or container file (NSP/NSZ/XCI/NCA) with the base game and/or the update; repeatable")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--title-id", default="01001F5010DFA000")
    ap.add_argument("--keys", type=Path)
    ap.add_argument("--title-keys", type=Path)
    a = ap.parse_args(argv)
    if AES is None:
        print("pycryptodome is required", file=sys.stderr)
        return 2
    t0 = time.time()
    try:
        base, patch, ncz = find_programs(a.dump, int(a.title_id, 16), a.keys, a.title_keys)
        if not base:
            raise BktrError(f"no base program NCA for {a.title_id} in {', '.join(map(str, a.dump))}")
        if not patch:
            raise BktrError(f"no update program NCA in {', '.join(map(str, a.dump))}")
        r = PatchedRomfs(base, patch)
    except (BktrError, nx.FormatError, nx.KeyError_) as e:
        print(f"romfs_bktr: {e}", file=sys.stderr)
        return 1
    last = [0.0]

    def progress(done, total):
        now = time.time()
        if now - last[0] >= 5 or done == total:
            last[0] = now
            print(f"  {done >> 20} / {total >> 20} MB ({100 * done / total:.0f} %)", flush=True)

    print(f"romfs_bktr: {len(r.ind)} indirect entries, {len(r.aes)} AesCtrEx entries, "
          f"{r.size} bytes -> {a.out}" + (" (base streamed from its NCZ)" if ncz else ""), flush=True)
    try:
        r.write(a.out, progress, ncz)
    except (BktrError, nx.FormatError) as e:
        print(f"romfs_bktr: {e}", file=sys.stderr)
        return 1
    print(f"romfs_bktr: done in {time.time() - t0:.1f} s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
