# SPDX-License-Identifier: MIT
"""Parser for the BNSH container (nn::gfx ResShaderFile) with NVN code.

Format inferred from the game's own RomFS (clean-room) and public community documentation
(Switch Toolbox / BfshaLibrary). See docs/port/SHADERS.md.

Layout (all "addresses" are offsets within the file; the file is relocatable and carries
an _RLT table):

  0x00 BinaryFileHeader (0x20): "BNSH\\0\\0\\0\\0", version u32, BOM, align log2, ...,
       u32 name_off, u16 relocated, u16 first_block (0x60), u32 rlt_off, u32 file_size
  0x60 'grsc' ResShaderContainerData:
       +0x10 u16 api_type (4 = NVN), u16 api_version
       +0x18 u32 compiler_version, +0x1C u32 variation_count
       +0x20 u64 variations (array of ResShaderVariationData, 0x40 bytes each)
       +0x28 u64 memory_pool / code block
  ResShaderVariationData (0x40): u64 source_program, u64 il_program, u64 binary_program,
       u64 parent, 0x20 reserved
  ResShaderProgramData: ShaderInfoData { u8 flags, u8 code_type, u8 source_format, u8 rsv,
       u32 binary_format, u64 stage_code[6] (VS, HS, DS, GS, PS, CS) } ...
       +0x60 u32 obj_size, +0x68 u64 obj, +0x70 u64 parent, +0x78 u64 reflection
  NvnShaderCode (per stage): +0x08 u64 control, +0x10 u64 data, +0x18 u32 data_size,
       +0x1C u32 control_size
  data   = graphics: 0x30-byte NVN header (magic 0x12345678) + SPH (0x50) + code;
           compute: the code directly (no header or SPH)
  control= NVN control section (magic 0x98761234), with metadata and sometimes the
           intermediate NVvp5.0/NVfp5.0 assembly as text.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

STAGES = ("vertex", "hull", "domain", "geometry", "fragment", "compute")
NVN_DATA_MAGIC = 0x12345678
NVN_CONTROL_MAGIC = 0x98761234


@dataclass
class StageCode:
    stage: str
    control_off: int
    control: bytes
    data_off: int
    data: bytes

    @property
    def is_compute(self) -> bool:
        return self.stage == "compute"

    @property
    def code(self) -> bytes:
        """Bytes the backend uploads to the GPU from the program address
        (SPH + instructions for graphics; instructions for compute).

        Graphics stages carry a 0x30-byte NVN header (magic 0x12345678) before the SPH;
        compute has no header: the code starts at byte 0."""
        return self.data[self.header_size:]

    @property
    def header_size(self) -> int:
        return 0x30 if struct.unpack_from("<I", self.data, 0)[0] == NVN_DATA_MAGIC else 0

    def code_hash(self) -> str:
        return hashlib.sha1(self.data).hexdigest()[:16]


@dataclass
class Variation:
    index: int
    stages: dict[str, StageCode] = field(default_factory=dict)
    reflection_off: int = 0


@dataclass
class BnshFile:
    name: str
    version: int
    api_type: int
    api_version: int
    compiler_version: int
    variations: list[Variation]
    raw: bytes


def _u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def _u64(d, o):
    return struct.unpack_from("<Q", d, o)[0]


def parse(data: bytes) -> BnshFile:
    if data[:4] != b"BNSH":
        raise ValueError("not a BNSH file")
    version = _u32(data, 0x08)
    name_off = _u32(data, 0x10)
    first_block = _u16(data, 0x16)
    name = ""
    if 0 < name_off < len(data):
        ln = _u16(data, name_off - 2) if name_off >= 2 else 0
        name = data[name_off:name_off + ln].decode("utf-8", "replace") if ln else ""
    g = first_block
    if data[g:g + 4] != b"grsc":
        raise ValueError("missing grsc block")
    api_type = _u16(data, g + 0x10)
    api_version = _u16(data, g + 0x12)
    compiler_version = _u32(data, g + 0x18)
    nvar = _u32(data, g + 0x1C)
    var_off = _u64(data, g + 0x20)
    variations = []
    for i in range(nvar):
        vo = var_off + i * 0x40
        prog = _u64(data, vo + 0x10)
        var = Variation(i)
        if prog:
            var.reflection_off = _u64(data, prog + 0x78)
            for s, name_s in enumerate(STAGES):
                co = _u64(data, prog + 0x08 + s * 8)
                if not co:
                    continue
                ctrl = _u64(data, co + 0x08)
                dat = _u64(data, co + 0x10)
                dsz = _u32(data, co + 0x18)
                csz = _u32(data, co + 0x1C)
                var.stages[name_s] = StageCode(name_s, ctrl, data[ctrl:ctrl + csz], dat,
                                               data[dat:dat + dsz])
        variations.append(var)
    return BnshFile(name, version, api_type, api_version, compiler_version, variations, data)


def find_all(blob: bytes):
    """Finds embedded BNSH containers (VFXB/ptcl, loose files...)."""
    pos = 0
    while True:
        i = blob.find(b"BNSH\0\0\0\0", pos)
        if i < 0:
            return
        size = _u32(blob, i + 0x1C) if i + 0x20 <= len(blob) else 0
        if 0x60 < size <= len(blob) - i:
            yield i, blob[i:i + size]
            pos = i + size
        else:
            pos = i + 4
