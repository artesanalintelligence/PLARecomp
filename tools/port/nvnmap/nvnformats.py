#!/usr/bin/env python3
"""Driver NVN format table (sdk+0xc62028, 0x30 bytes per format) -> Maxwell formats and VkFormat.

Each entry (12 u32; index = NVN format enum value) is used by the driver itself:
  w0  capabilities (bits: texture, render target, vertex...)
  w1  class (3 = 32-bit color, 7/8 = 8/16-byte BC, 9 = ASTC, 0x18-0x1b = depth)
  w4  low byte = bytes per element (per block when compressed); bit 16 = sRGB; bit 24 = normalized
  w5  Maxwell vertex attribute format (bits 21-26 size, 27-29 type); copied by
      nvnCommandBufferBindVertexAttribState (sdk+0x48e3xx: `+0x14`)
  w6  "class" render target format (the one used by clears)
  w7  Maxwell render target format (RT_FORMAT)
  w9  TIC word 0 (texture format + component types + default swizzle)
  w10 image format (load/store)
  w11 low byte: size class (1, 2, 3, 4 = bytes; 5 = 8 bytes; 6 = 16 bytes)
Translation to Vulkan goes through the Maxwell format (public suyu/envytools/nouveau enums);
no list of NVN names is used: output names are the Vulkan format names.

Outputs: docs/port/data/nvn_formats.csv and tools/port/nvnvk/generated/nvn_formats.inc
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sdkelf import Img, ROOT  # noqa: E402

TABLE = 0xC62028
STRIDE = 0x30

# Maxwell TextureFormat (TIC word 0, bits 0-6)
TF = {0x01: "R32G32B32A32", 0x02: "R32G32B32", 0x03: "R16G16B16A16", 0x04: "R32G32", 0x05: "R32B24G8",
      0x06: "ETC2_RGB", 0x07: "X8B8G8R8", 0x08: "A8B8G8R8", 0x09: "A2B10G10R10", 0x0a: "ETC2_RGB_PTA",
      0x0b: "ETC2_RGBA", 0x0c: "R16G16", 0x0d: "G8R24", 0x0e: "G24R8", 0x0f: "R32", 0x10: "BC6H_S16",
      0x11: "BC6H_U16", 0x12: "A4B4G4R4", 0x13: "A5B5G5R1", 0x14: "A1B5G5R5", 0x15: "B5G6R5",
      0x16: "B6G5R5", 0x17: "BC7U", 0x18: "G8R8", 0x19: "EAC", 0x1a: "EACX2", 0x1b: "R16", 0x1c: "Y8_VIDEO",
      0x1d: "R8", 0x1e: "G4R4", 0x1f: "R1", 0x20: "E5B9G9R9", 0x21: "B10G11R11", 0x22: "G8B8G8R8",
      0x23: "B8G8R8G8", 0x24: "DXT1", 0x25: "DXT23", 0x26: "DXT45", 0x27: "DXN1", 0x28: "DXN2",
      0x29: "Z24S8", 0x2a: "X8Z24", 0x2b: "S8Z24", 0x2f: "Z32", 0x30: "Z32_X24S8", 0x3a: "Z16",
      0x40: "ASTC_4X4", 0x41: "ASTC_5X5", 0x42: "ASTC_6X6", 0x44: "ASTC_8X8", 0x45: "ASTC_10X10",
      0x46: "ASTC_12X12", 0x50: "ASTC_5X4", 0x51: "ASTC_6X5", 0x52: "ASTC_8X6", 0x53: "ASTC_10X8",
      0x54: "ASTC_12X10", 0x55: "ASTC_8X5", 0x56: "ASTC_10X5", 0x57: "ASTC_10X6"}
CT = {1: "SNORM", 2: "UNORM", 3: "SINT", 4: "UINT", 5: "SNORM_FP16", 6: "UNORM_FP16", 7: "FLOAT"}
SW = {0: "0", 2: "R", 3: "G", 4: "B", 5: "A", 6: "1i", 7: "1"}
# Maxwell RenderTargetFormat -> VkFormat (the one that decides whether a format is sRGB)
RT = {0xc0: "R32G32B32A32_SFLOAT", 0xc1: "R32G32B32A32_SINT", 0xc2: "R32G32B32A32_UINT",
      0xc3: "R32G32B32A32_SFLOAT", 0xc4: "R32G32B32A32_SINT", 0xc5: "R32G32B32A32_UINT",
      0xc6: "R16G16B16A16_UNORM", 0xc7: "R16G16B16A16_SNORM", 0xc8: "R16G16B16A16_SINT",
      0xc9: "R16G16B16A16_UINT", 0xca: "R16G16B16A16_SFLOAT", 0xcb: "R32G32_SFLOAT", 0xcc: "R32G32_SINT",
      0xcd: "R32G32_UINT", 0xce: "R16G16B16A16_SFLOAT", 0xcf: "B8G8R8A8_UNORM", 0xd0: "B8G8R8A8_SRGB",
      0xd1: "A2B10G10R10_UNORM_PACK32", 0xd2: "A2B10G10R10_UINT_PACK32", 0xd5: "R8G8B8A8_UNORM",
      0xd6: "R8G8B8A8_SRGB", 0xd7: "R8G8B8A8_SNORM", 0xd8: "R8G8B8A8_SINT", 0xd9: "R8G8B8A8_UINT",
      0xda: "R16G16_UNORM", 0xdb: "R16G16_SNORM", 0xdc: "R16G16_SINT", 0xdd: "R16G16_UINT",
      0xde: "R16G16_SFLOAT", 0xdf: "A2B10G10R10_UNORM_PACK32", 0xe0: "B10G11R11_UFLOAT_PACK32",
      0xe3: "R32_SINT", 0xe4: "R32_UINT", 0xe5: "R32_SFLOAT", 0xe6: "B8G8R8A8_UNORM",
      0xe7: "B8G8R8A8_SRGB", 0xe8: "R5G6B5_UNORM_PACK16", 0xe9: "A1R5G5B5_UNORM_PACK16",
      0xea: "R8G8_UNORM", 0xeb: "R8G8_SNORM", 0xec: "R8G8_SINT", 0xed: "R8G8_UINT", 0xee: "R16_UNORM",
      0xef: "R16_SNORM", 0xf0: "R16_SINT", 0xf1: "R16_UINT", 0xf2: "R16_SFLOAT", 0xf3: "R8_UNORM",
      0xf4: "R8_SNORM", 0xf5: "R8_SINT", 0xf6: "R8_UINT", 0xf7: "R8_UNORM", 0xf8: "A1R5G5B5_UNORM_PACK16",
      0xf9: "R8G8B8A8_UNORM", 0xfa: "R8G8B8A8_SRGB", 0xfb: "R8G8B8A8_UNORM", 0xfc: "R8G8B8A8_SRGB",
      0xfd: "R8G8B8A8_UNORM", 0xfe: "R8G8B8A8_UNORM", 0xff: "R8G8B8A8_UINT"}


def vk_from_tic(fmt: int, ct: str, srgb: bool) -> str:
    n = TF.get(fmt, "")
    f = {"UNORM": "UNORM", "SNORM": "SNORM", "SINT": "SINT", "UINT": "UINT", "FLOAT": "SFLOAT",
         "UNORM_FP16": "UNORM", "SNORM_FP16": "SNORM"}.get(ct, "UNORM")
    s = "SRGB" if srgb else f
    simple = {"R32G32B32A32": f"R32G32B32A32_{f}", "R32G32B32": f"R32G32B32_{f}",
              "R16G16B16A16": f"R16G16B16A16_{f}", "R32G32": f"R32G32_{f}", "R16G16": f"R16G16_{f}",
              "R32": f"R32_{f}", "R16": f"R16_{f}", "R8": f"R8_{s}", "G8R8": f"R8G8_{s}",
              "A8B8G8R8": f"R8G8B8A8_{s}", "X8B8G8R8": f"R8G8B8A8_{s}",
              "A2B10G10R10": "A2B10G10R10_UINT_PACK32" if f == "UINT" else "A2B10G10R10_UNORM_PACK32",
              "A4B4G4R4": "A4B4G4R4_UNORM_PACK16", "A1B5G5R5": "A1R5G5B5_UNORM_PACK16",
              "A5B5G5R1": "R5G5B5A1_UNORM_PACK16", "B5G6R5": "R5G6B5_UNORM_PACK16",
              "E5B9G9R9": "E5B9G9R9_UFLOAT_PACK32", "B10G11R11": "B10G11R11_UFLOAT_PACK32",
              "DXT1": f"BC1_RGBA_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "DXT23": f"BC2_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "DXT45": f"BC3_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "DXN1": f"BC4_{'SNORM' if f == 'SNORM' else 'UNORM'}_BLOCK",
              "DXN2": f"BC5_{'SNORM' if f == 'SNORM' else 'UNORM'}_BLOCK",
              "BC7U": f"BC7_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "BC6H_S16": "BC6H_SFLOAT_BLOCK", "BC6H_U16": "BC6H_UFLOAT_BLOCK",
              "Z16": "D16_UNORM", "Z32": "D32_SFLOAT", "Z24S8": "D24_UNORM_S8_UINT",
              "S8Z24": "D24_UNORM_S8_UINT", "X8Z24": "D24_UNORM_S8_UINT", "Z32_X24S8": "D32_SFLOAT_S8_UINT",
              "G8R24": "D24_UNORM_S8_UINT", "G24R8": "D24_UNORM_S8_UINT",
              "ETC2_RGB": f"ETC2_R8G8B8_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "ETC2_RGBA": f"ETC2_R8G8B8A8_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "ETC2_RGB_PTA": f"ETC2_R8G8B8A1_{'SRGB' if srgb else 'UNORM'}_BLOCK",
              "EAC": "EAC_R11_UNORM_BLOCK", "EACX2": "EAC_R11G11_UNORM_BLOCK"}
    if n in simple:
        return simple[n]
    if n.startswith("ASTC_"):
        return f"ASTC_{n[5:].lower()}_{'SRGB' if srgb else 'UNORM'}_BLOCK"
    return ""


def main() -> int:
    im = Img()
    rows = []
    i = 0
    while True:
        w = [im.u32(TABLE + i * STRIDE + 4 * k) for k in range(12)]
        if i > 0 and not any(w):
            break
        tic = w[9]
        fmt = tic & 0x7F
        ct = CT.get((tic >> 7) & 7, "?")
        sw = "".join(SW.get((tic >> s) & 7, "?") for s in (19, 22, 25, 28))
        rt = w[7] & 0xFF
        rtvk = RT.get(rt, "")
        srgb = bool(w[4] & 0x10000)  # w4 bit 16: sRGB conversion (UNORM/SRGB pairs differ only here)
        vk = vk_from_tic(fmt, ct, srgb) if fmt else ""
        vsize = (w[5] >> 21) & 0x3F
        vtype = (w[5] >> 27) & 7
        rows.append(dict(nvn=i, caps=f"0x{w[0]:x}", bytes=w[4] & 0xFF, srgb=int(srgb), tic_format=TF.get(fmt, f"0x{fmt:x}") if fmt else "",
                         comp=ct if fmt else "", swizzle=sw if fmt else "", rt_format=f"0x{rt:x}" if rt else "",
                         rt_vk=rtvk, vtx_size=f"0x{vsize:x}" if w[5] else "", vtx_type=vtype if w[5] else "",
                         vk=vk, w4=f"0x{w[4]:x}", w10=f"0x{w[10]:x}"))
        i += 1
    out = ROOT / "docs" / "port" / "data" / "nvn_formats.csv"
    with open(out, "w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)
    inc = ROOT / "tools" / "port" / "nvnvk" / "generated" / "nvn_formats.inc"
    lines = ["// Generated by tools/port/nvnmap/nvnformats.py from the driver's format table (sdk+0xc62028).",
             "// NVN_FORMAT(nvn, VkFormat, bytes, tic_word0, rt_format, vertex_word)"]
    for r, i2 in zip(rows, range(len(rows))):
        w = [im.u32(TABLE + i2 * STRIDE + 4 * k) for k in range(12)]
        vk = f"VK_FORMAT_{r['vk']}" if r["vk"] else (f"VK_FORMAT_{r['rt_vk']}" if r["rt_vk"] else "VK_FORMAT_UNDEFINED")
        lines.append(f"NVN_FORMAT({i2}, {vk}, {w[4] & 0xFF}, 0x{w[9]:08x}, 0x{w[7]:x}, 0x{w[5]:08x})")
    inc.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(rows)} formats -> {out}, {inc}")
    for r in rows:
        print(r["nvn"], r["tic_format"], r["comp"], r["swizzle"], r["rt_vk"], r["vk"], r["bytes"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
