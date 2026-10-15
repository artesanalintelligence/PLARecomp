#!/usr/bin/env python3
"""Disassembles driver NVN functions by name: python nvndis.py nvnFoo [nvnBar...] [-n 60]"""
import sys, csv, capstone
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from sdkelf import Img, ROOT
im = Img()
m = {r['name']: int(r['sdk_offset'], 16) for r in csv.DictReader(open(ROOT / 'docs/port/data/nvn_sdk_map.csv'))}
inv = {v: k for k, v in m.items()}
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
n = 80
args = sys.argv[1:]
if '-n' in args:
    i = args.index('-n'); n = int(args[i + 1]); del args[i:i + 2]
for nm in args:
    a = int(nm, 16) if nm.startswith('0x') else m[nm if nm.startswith('nvn') else 'nvn' + nm]
    print('==', inv.get(a, nm), hex(a))
    for k, ins in enumerate(md.disasm(im.data[im.v2o(a):im.v2o(a) + 4 * n], a)):
        print('  %x %s %s' % (ins.address, ins.mnemonic, ins.op_str))
        if ins.mnemonic in ('ret', 'br') or (ins.mnemonic == 'b' and k > 0):
            break
