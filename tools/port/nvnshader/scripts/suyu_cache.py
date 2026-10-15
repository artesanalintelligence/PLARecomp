# SPDX-License-Identifier: MIT
"""Reader for suyu's pipeline cache (`vulkan.bin`, "yuzucach" version 18).

Each record: u32 num_envs; num_envs x serialized FileEnvironment; pipeline key
(GraphicsPipelineCacheKey = 6 hashes + FixedPipelineState, or a 24-byte
ComputePipelineCacheKey). The graphics key size is not known up front (it depends on the suyu
build): it is inferred by trying sizes until the whole file parses without errors.

Used to: (a) compare the code suyu saw in the real game with the RomFS BNSH code;
(b) get the real cbuf values (bank 1 = NVN driver constants, bank 2 = texture handles),
texture types and fixed state per pipeline.
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field

STAGE_NAMES = {0: "vertex_b", 1: "tess_ctrl", 2: "tess_eval", 3: "geometry", 4: "fragment",
               5: "compute", 6: "vertex_a"}


@dataclass
class Env:
    stage: int
    code: bytes
    local_memory_size: int
    texture_bound: int
    start_address: int
    read_lowest: int
    read_highest: int
    viewport_transform_state: int
    texture_types: dict = field(default_factory=dict)
    texture_formats: dict = field(default_factory=dict)
    cbuf_values: dict = field(default_factory=dict)
    cbuf_replacements: dict = field(default_factory=dict)
    sph: bytes = b""
    gp_passthrough: bytes = b""
    workgroup_size: tuple = (0, 0, 0)
    shared_memory_size: int = 0


@dataclass
class Record:
    envs: list
    key: bytes


def _parse_env(d: bytes, o: int):
    (code_size, ntt, ntf, ncb, ncr) = struct.unpack_from("<5Q", d, o)
    o += 40
    (lms, tb, sa, rl, rh, vts, stage) = struct.unpack_from("<7I", d, o)
    o += 28
    if code_size > (1 << 22) or stage > 6 or ntt > 4096 or ncb > 65536:
        raise ValueError("invalid record")
    code = d[o:o + code_size]
    o += code_size
    e = Env(stage, code, lms, tb, sa, rl, rh, vts)
    for _ in range(ntt):
        k, v = struct.unpack_from("<II", d, o)
        o += 8
        e.texture_types[k] = v
    for _ in range(ntf):
        k, v = struct.unpack_from("<Ii", d, o)
        o += 8
        e.texture_formats[k] = v
    for _ in range(ncb):
        k, v = struct.unpack_from("<QI", d, o)
        o += 12
        e.cbuf_values[(k >> 32, k & 0xFFFFFFFF)] = v
    for _ in range(ncr):
        k, v = struct.unpack_from("<QI", d, o)
        o += 12
        e.cbuf_replacements[(k >> 32, k & 0xFFFFFFFF)] = v
    if stage == 5:
        e.workgroup_size = struct.unpack_from("<3I", d, o)
        e.shared_memory_size = struct.unpack_from("<I", d, o + 12)[0]
        o += 16
    else:
        e.sph = d[o:o + 0x50]
        o += 0x50
        if stage == 3:
            e.gp_passthrough = d[o:o + 32]
            o += 32
    return e, o


def _parse_all(d: bytes, gkey: int, ckey: int = 24):
    o = 12
    recs = []
    while o < len(d):
        (n,) = struct.unpack_from("<I", d, o)
        o += 4
        if not 1 <= n <= 6:
            raise ValueError("invalid num_envs")
        envs = []
        for _ in range(n):
            e, o = _parse_env(d, o)
            envs.append(e)
        ks = ckey if envs[0].stage == 5 else gkey
        recs.append(Record(envs, d[o:o + ks]))
        o += ks
    if o != len(d):
        raise ValueError("trailing data mismatch")
    return recs


def load(path: str):
    d = open(path, "rb").read()
    if d[:8] != b"yuzucach":
        raise ValueError("not a suyu cache")
    version = struct.unpack_from("<I", d, 8)[0]
    last = None
    for gkey in range(64, 2048, 4):
        try:
            return version, gkey, _parse_all(d, gkey)
        except (ValueError, struct.error) as e:  # noqa: PERF203
            last = e
    raise ValueError(f"could not infer the key size: {last}")


if __name__ == "__main__":
    import collections
    ver, gkey, recs = load(sys.argv[1])
    st = collections.Counter(STAGE_NAMES[e.stage] for r in recs for e in r.envs)
    tb = collections.Counter(e.texture_bound for r in recs for e in r.envs)
    banks = collections.Counter(b for r in recs for e in r.envs for (b, _o) in e.cbuf_values)
    print(f"version {ver}, graphics key {gkey} bytes, {len(recs)} pipelines, stages {dict(st)}")
    print(f"texture_bound {dict(tb)}, cbuf reads per bank {dict(banks)}")
