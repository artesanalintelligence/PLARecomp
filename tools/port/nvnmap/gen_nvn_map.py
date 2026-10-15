#!/usr/bin/env python3
"""Static NVN name -> address map in `sdk` (game 1.1.1, nnSdk 11.4.0).

How the driver works (own disassembly of build/port/sdk.elf, see NVN-SEMANTICS.md §1):
  * sdk+0x469fd0 `lookup(name)`: requires the "nvn" prefix and binary-searches the rest
    over a table of 523 u32 offsets (sdk+0xabe798) into a block of sorted, unprefixed names
    (sdk+0xabafd5). Returns the index or -1.
  * sdk+0x496ff0 `internal GetProcAddress`: static pointer table (*(sdk+0xc78d50) =
    sdk+0xc63e58, RELATIVE relocs) indexed by that index.
  * sdk+0x49af40 `nvnDeviceGetProcAddress(device, name)`: same search over the .bss table
    (*(sdk+0xc78d68) = sdk+0xd2b988), which the driver fills at init and the interception
    layer (nvnInterceptionSetProcAddressNVX, sdk+0x49af90) can overwrite.
All three addresses are located by pattern, not hardcoded.

Output: docs/port/data/nvn_sdk_map.csv
  name,index,sdk_offset,shared_with,object,callsites,used
"""
from __future__ import annotations
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sdkelf import Img, ROOT  # noqa: E402

OUT = ROOT / "docs" / "port" / "data" / "nvn_sdk_map.csv"
API = ROOT / "docs" / "port" / "data" / "nvn_api.csv"


def find_tables(im: Img):
    o = im.data.find(b"BlendStateGetAdvancedMode\0")
    names = im.o2v(o)
    o2 = im.data.find(struct.pack("<3I", 0, 0x1A, 0x3D), o)
    offs = im.o2v(o2)
    # lookup(): the function that references both tables
    refs = im.adrp_refs(offs, offs + 1)
    lookup_ref = min(pc for pc, _ in refs)
    # entry count: the binary search's "mov w23, #hi" (hi = n - 1)
    n = None
    for k in range(1, 8):
        w = im.u32(lookup_ref - 4 * k)
        if (w & 0xFFE00000) == 0x52800000:  # MOVZ w
            n = ((w >> 5) & 0xFFFF) + 1
            break
    return names, offs, n, lookup_ref


def main() -> int:
    im = Img()
    names, offs, n, lookup_ref = find_tables(im)
    table = im.ptr(0xC78D50)
    entries = []
    for i in range(n):
        nm = "nvn" + im.cstr(names + im.u32(offs + 4 * i))
        entries.append((nm, i, im.ptr(table + 8 * i)))
    by_addr = {}
    for nm, i, a in entries:
        by_addr.setdefault(a, []).append(nm)
    api = {}
    if API.exists():
        for r in csv.DictReader(open(API, newline="")):
            api[r["name"]] = r
    with open(OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "index", "sdk_offset", "shared_with", "object", "callsites", "used"])
        for nm, i, a in entries:
            others = [x for x in by_addr[a] if x != nm]
            r = api.get(nm, {})
            used = int(r.get("callsites", 0) or 0) > 0 or int(r.get("decomp_refs", 0) or 0) > 0
            w.writerow([nm, i, f"0x{a:x}", " ".join(others), r.get("object", ""),
                        r.get("callsites", ""), int(used)])
    shared = sum(1 for a, v in by_addr.items() if len(v) > 1)
    missing = [nm for nm in api if nm not in {e[0] for e in entries}]
    print(f"{n} names, {len(by_addr)} distinct addresses, {shared} shared addresses; "
          f"lookup at sdk+0x{lookup_ref:x}; table sdk+0x{table:x}")
    print(f"names in nvn_api.csv not present in the driver: {missing}")
    print(f"-> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
