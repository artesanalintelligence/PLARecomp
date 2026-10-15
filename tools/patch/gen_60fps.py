#!/usr/bin/env python3
"""Generates the framerate patches for PLA 1.1.1: unlocked FPS with dynamic delta and fixed 60 FPS
(docs/research/06-framerate.md).

Outputs (in recomp/patches/ by default), format pla-patchlist-v1:
  unlocked-fps/pla-111-unlocked-fps.json   unlocked FPS with dynamic delta (main goal)
  60fps/pla-111-60fps-fixed.json           fixed 60 FPS (by-product)
  suyu/<mod>/exefs/*.pchtxt     mods for %APPDATA%/suyu/load/01001F5010DFA000/
  ips/<variant>/<BUILDID>.ips   the same patches as IPS32 (offset + 0x100)

Each site is checked against the original main.nso: if the original bytes do not
match what is expected, the script aborts. All new instructions are
hand-encoded and verified by disassembling them with capstone.

Usage:
    python tools/patch/gen_60fps.py                       # uses data/exefs/main
    python tools/patch/gen_60fps.py --nso <main> --out <dir>
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import nsoimg  # noqa: E402

TITLE_ID = "01001F5010DFA000"
BUILD_ID = "AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E"

# --- addresses (offsets of the main module) -----------------------------------------
FRAME_NS_TABLE = 0x397E8D0          # u64[5]: ns per frame for interval 1..5
FRAME_NS_30 = FRAME_NS_TABLE + 8    # 33333334 (index 1, the one used by the main loop)
PRESENT_INTERVAL = 0x2AA278C        # add w1, w8, #1 -> nvnWindowSetPresentInterval(win, cfg+1)
CLOCK_SET_MODE = 0x2783048          # vtable_7104171528::vf1(this, i, mode): clock[i].ns = table[mode]
GET_SYSTEM_TICK_PLT = 0x32A3020     # nn::os::GetSystemTick() (stub PLT)
CAVE = 0x32A5700                    # cave in the .text -> .rodata padding (0x32A5690..0x32A6000)
VARS = 0x4350F00                    # variables in the free tail of .bss (0x4350E88..0x4351000)
VAR_LAST_TICK, VAR_FRAME_NS, VAR_DT, VAR_NEG_DT, VAR_NEG10_INV_DT, VAR_INV_DT = 0x00, 0x08, 0x10, 0x14, 0x1C, 0x20
DT_MIN_NS = 1_000_000      # 1/1000 s
DT_MAX_NS = 66_666_667     # 1/15 s (below 15 FPS the game slows down instead of jumping)

# "1/30 per frame" sites: movz wR,#0x8889 / movk wR,#0x3d08,lsl#16 pairs (the offset is the movk).
PER_FRAME_1_30 = [
    (0x13B33A0, "vtable_7104114778::vf9: timer +0x48c += 1/30"),
    (0x167FF10, "FUN_710167f988: timer +0x190 += 1/30"),
    (0x16832C4, "FUN_710168300c: timer +0x1a0 += 1/30"),
    (0x16832E8, "FUN_710168300c: timer +0x198 += 1/30 (6.0 s threshold)"),
    (0x16836D4, "FUN_7101683528: timer +0x19c += 1/30"),
    (0x1982934, "vtable_7104139aa0::vf33: displacement = vel * k * 1/30 (see also 0x1982960)"),
    (0x1983764, "FUN_7101983760: returns the fixed 1/30 step (5 movement callers)"),
    (0x198E024, "vtable_7104139aa0::vf78: velocity integration * 1/30"),
    (0x198EC70, "vtable_7104139aa0::vf82: velocity integration * 1/30"),
    (0x1A0BAF4, "vtable_710413db48::vf84: pos += v * 1/30"),
    (0x1A0BEE8, "vtable_710413db48::vf84: v * 1/30 (vector)"),
    (0x1A0DA24, "vtable_710413db48::vf35: displacement = v * 1/30"),
    (0x1A0EEA4, "vtable_710413db48::vf82: integration * 1/30"),
    (0x1A0F308, "vtable_710413db48::vf119: step = 1/30 (default value)"),
    (0x1A0F49C, "vtable_710413db48::vf119: step = 1/30 (w22)"),
    (0x1A0F560, "vtable_710413db48::vf119: step = 1/30"),
    (0x1A17B14, "vtable_710413db48::vf70: pos += v * 1/30"),
    (0x1A17F40, "vtable_710413db48::vf70: pos += v * 1/30"),
    (0x1A19108, "vtable_710413db48::vf70: FUN_7101727df4(dt=1/30, ...)"),
    (0x1A196C8, "FUN_7101a19654: FUN_7101727df4(dt=1/30, ...)"),
    (0x1B48A4C, "vtable_710414bc28::vf18: timer +0x254 += f * 1/30"),
    (0x1B48A98, "vtable_710414bc28::vf18: timer +0x24c += f * 1/30"),
    (0x1B49008, "vtable_710414bc28::vf18: timer +0x24c += f * 1/30"),
    (0x1B493F8, "vtable_710414bc28::vf18: timer +0x250 += f * 1/30"),
    (0x1D1B7D0, "vtable_7104152798::vf9: prediction pos + v * 1/30"),
    (0x1D5D688, "FUN_7101d5d59c: FUN_71024ac8c4(1/30, ...) (minute counter)"),
    (0x257AC34, "vtable_71041629a8::vf119: FUN_7101713904(f * k * 1/30)"),
    (0x257C9F8, "vtable_71041629a8::vf84: pos += v * 1/30"),
    (0x257CE3C, "vtable_71041629a8::vf84: v * 1/30 (vector)"),
    (0x2795234, "FUN_7102795204: fixed dt 1/30 unless flag +0x150"),
    (0x29FAF90, "vtable_7104183390::vf17: time accumulator += 1/30 (wraps at 10000)"),
    (0x2AF25F8, "FUN_7102af25b4: fixed dt 1/30 unless flag +0x121"),
]
# "-1/30 per frame" sites: movz w8,#0x8889 / movk w8,#0xbd08,lsl#16 pairs (the offset is the movk). The first
# framerate study only looked for +1/30, so gravity (v.y += g * -1/30) and countdown timers kept the 30 FPS step:
# at 130 FPS a mount jump fell ~4x faster and barely left the ground.
PER_FRAME_NEG_1_30 = [
    (0xAFBBC8, "AI node (calls 0x1984fc8): s0 *= -1/30 (vertical step)"),
    (0x1984A74, "vtable_7104139aa0::vf37: gravity v.y += g(+0xf8) * scale(+0xbe8) * -1/30"),
    (0x198DA90, "vtable_7104139aa0::vf78: gravity * -1/30"),
    (0x1A0B3B0, "vtable_710413db48: timer +0x750 -= f * 1/30"),
    (0x1A158CC, "vtable_710413db48: gravity v.y += g(+0xf8) * k * -1/30"),
    (0x1AAB5A8, "FUN_7101aab*: timer +0x50 -= 1/30"),
    (0x1AAB5D8, "FUN_7101aab*: timer +0x54 -= 1/30"),
    (0x1B47F60, "vtable_710414bc28::vf18: timer [x21] -= f * 1/30"),
    (0x1B47F98, "vtable_710414bc28::vf18: timer +0x250 -= f * 1/30"),
    (0x1B47FD0, "vtable_710414bc28::vf18: timer +0x254 -= f * 1/30"),
    (0x25AC3DC, "ride controller: gravity * -1/30 (mount jumps)"),
]
# Sky (vtable_7104182xxx::vf7@0x29bfdac): the cloud layers scroll per frame by wind / (k * 30) and two more
# offsets (+0x5dc, +0x614) by speed / -300, assuming 30 FPS frames (clouds ran ~4.8x faster at 120 FPS).
# (fmov/address, register, value at 30 FPS): "30" -> 1/dt and "-300" -> -10/dt through a bl to a cave routine
# (the prologue saves x30; x16 is free at these points). The original instruction is checked.
SKY_SITES = [
    (0x29C1FA4, 11, 30.0, "sky vf7: s11 = 30 (divisor of the cloud UV scroll +0x608/+0x60c/+0x610)"),
    (0x29C2990, 2, 30.0, "sky vf7: s2 = 30 (k * 30, divisor of the +0x64c offset)"),
    (0x29C17FC, 1, -300.0, "sky vf7: s1 = -300 (+0x5dc += speed / -300 per frame)"),
    (0x29C1930, 1, -300.0, "sky vf7: s1 = -300 (+0x614 += speed / -300 per frame)"),
]
# FUN_7101d5d59c: FUN_7102a1a728((double)(float)(1/30), obj, 1): mov x8,#0x20000000 / movk #0x1111,lsl32 /
# movk #0x3fa1,lsl48 / fmov d0,x8  (the offset is the initial mov)
PER_FRAME_1_30_DOUBLE = [0x1D5D850, 0x1D5D86C, 0x1D5D888]
# vtable_7104139aa0::vf33: inverse of the step, mov w8,#0x41efffff (29.999998) ; dup v0.4s, w8
VF33_INV = 0x1982960
# vtable_7104137528::vf17: acumulador += f / 30.0  (fmov sX,#30.0 ; fdiv ...)
FDIV30_SITES = [
    (0x1A71A6C, 0, 1, 1, "vtable_7104137528::vf17: +0x618 += f / 30 (s1 = s1 / s0)"),   # fmov s0; fdiv s1,s1,s0
    (0x1A71ABC, 1, 4, 2, "vtable_7104137528::vf17: +0x618 += f / 30 (s4 = s2 / s1)"),   # fmov s1; fdiv s4,s2,s1
]


# --- minimal AArch64 encoder ----------------------------------------------------
def _rel(target: int, pc: int, bits: int, shift: int = 2) -> int:
    d = target - pc
    if d % (1 << shift):
        raise ValueError(f"misaligned target 0x{target:x}")
    v = d >> shift
    if not -(1 << (bits - 1)) <= v < (1 << (bits - 1)):
        raise ValueError(f"target out of range 0x{target:x} from 0x{pc:x}")
    return v & ((1 << bits) - 1)


def movz(rd, imm, hw=0, x=False): return (0xD2800000 if x else 0x52800000) | hw << 21 | imm << 5 | rd
def movk(rd, imm, hw=0, x=False): return (0xF2800000 if x else 0x72800000) | hw << 21 | imm << 5 | rd
def movn(rd, imm, hw=0, x=False): return (0x92800000 if x else 0x12800000) | hw << 21 | imm << 5 | rd


def adrp(rd, target, pc):
    imm = (target >> 12) - (pc >> 12)
    if not -(1 << 20) <= imm < (1 << 20):
        raise ValueError("adrp out of range")
    imm &= (1 << 21) - 1
    return 0x90000000 | (imm & 3) << 29 | (imm >> 2) << 5 | rd


def add_imm(rd, rn, imm): return 0x91000000 | imm << 10 | rn << 5 | rd
def _ldst(base, size, rt, rn, off):
    if off % size or not 0 <= off // size < 4096:
        raise ValueError(f"invalid offset {off:#x} for size {size}")
    return base | (off // size) << 10 | rn << 5 | rt


def ldr_x(rt, rn, off): return _ldst(0xF9400000, 8, rt, rn, off)
def str_x(rt, rn, off): return _ldst(0xF9000000, 8, rt, rn, off)
def ldr_w(rt, rn, off): return _ldst(0xB9400000, 4, rt, rn, off)
def ldr_s(rt, rn, off): return _ldst(0xBD400000, 4, rt, rn, off)
def str_s(rt, rn, off): return _ldst(0xBD000000, 4, rt, rn, off)
def ldr_q(rt, rn, off): return _ldst(0x3DC00000, 16, rt, rn, off)
def str_q(rt, rn, off): return _ldst(0x3D800000, 16, rt, rn, off)
def ldr_x_lit(rt, target, pc): return 0x58000000 | _rel(target, pc, 19) << 5 | rt
def ldr_d_lit(rt, target, pc): return 0x5C000000 | _rel(target, pc, 19) << 5 | rt
def b(target, pc): return 0x14000000 | _rel(target, pc, 26)
def bl(target, pc): return 0x94000000 | _rel(target, pc, 26)
def b_cond(cond, target, pc): return 0x54000000 | _rel(target, pc, 19) << 5 | cond
def cbz_x(rt, target, pc): return 0xB4000000 | _rel(target, pc, 19) << 5 | rt
def cbnz_x(rt, target, pc): return 0xB5000000 | _rel(target, pc, 19) << 5 | rt
def cbnz_w(rt, target, pc): return 0x35000000 | _rel(target, pc, 19) << 5 | rt
def cmp_w_imm(rn, imm): return 0x7100001F | imm << 10 | rn << 5
def cmp_x(rn, rm): return 0xEB00001F | rm << 16 | rn << 5
def csel_x(rd, rn, rm, cond): return 0x9A800000 | rm << 16 | cond << 12 | rn << 5 | rd
def sub_x(rd, rn, rm): return 0xCB000000 | rm << 16 | rn << 5 | rd
def mul_x(rd, rn, rm): return 0x9B007C00 | rm << 16 | rn << 5 | rd
def udiv_x(rd, rn, rm): return 0x9AC00800 | rm << 16 | rn << 5 | rd
def ucvtf_d_x(rd, rn): return 0x9E630000 | rn << 5 | rd
def fmul_d(rd, rn, rm): return 0x1E600800 | rm << 16 | rn << 5 | rd
def fmul_s(rd, rn, rm): return 0x1E200800 | rm << 16 | rn << 5 | rd
def fdiv_s(rd, rn, rm): return 0x1E201800 | rm << 16 | rn << 5 | rd
def fadd_s(rd, rn, rm): return 0x1E202800 | rm << 16 | rn << 5 | rd
def fcvt_s_d(rd, rn): return 0x1E624000 | rn << 5 | rd
def fcvt_d_s(rd, rn): return 0x1E22C000 | rn << 5 | rd
def fneg_s(rd, rn): return 0x1E214000 | rn << 5 | rd
def fmov_s_w(rd, rn): return 0x1E270000 | rn << 5 | rd
def ldr_s_lit(rt, target, pc): return 0x1C000000 | _rel(target, pc, 19) << 5 | rt
def fmov_s_imm(rd, imm8): return 0x1E201000 | imm8 << 13 | rd
def dup_4s_elem0(rd, rn): return 0x4E040400 | rn << 5 | rd
def stp_x_pre(rt, rt2, rn, off): return 0xA9800000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def ldp_x_post(rt, rt2, rn, off): return 0xA8C00000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def stp_x(rt, rt2, rn, off): return 0xA9000000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt
def ldp_x(rt, rt2, rn, off): return 0xA9400000 | ((off // 8) & 0x7F) << 15 | rt2 << 10 | rn << 5 | rt


MOV_X29_SP = 0x910003FD
BR_X3 = 0xD61F0060
RET = 0xD65F03C0
NOP = 0xD503201F
NE, LO, HI = 1, 3, 8
DUP_4S_W8 = 0x4E040D00  # dup v0.4s, w8
FMOV_30 = 0x3E  # imm8 of 30.0
FMOV_1 = 0x70   # imm8 of 1.0
FMOV_NEG10 = 0xA4  # imm8 of -10.0
F32_1_30 = 0x3D088889
F32_1_60 = 0x3C888889


def w(*words: int) -> bytes:
    return b"".join(struct.pack("<I", x & 0xFFFFFFFF) for x in words)


# --- list construction ------------------------------------------------------
class Builder:
    def __init__(self, nso: nsoimg.Nso):
        self.nso = nso
        self.patches: list[dict] = []
        try:
            import capstone
            self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        except ImportError:  # pragma: no cover
            raise SystemExit("capstone is required (pip install capstone) to verify the instructions")

    def dis(self, data: bytes, addr: int) -> list[str]:
        out = []
        for i in range(0, len(data), 4):
            ins = list(self.md.disasm(data[i:i + 4], addr + i))
            out.append(f"{ins[0].mnemonic} {ins[0].op_str}".strip() if ins else f".word 0x{struct.unpack_from('<I', data, i)[0]:08x}")
        return out

    def expect(self, off: int, data: bytes) -> None:
        cur = self.nso.read(off, len(data))
        if cur != data:
            raise SystemExit(f"main+0x{off:x}: unexpected bytes {cur.hex()} (expected {data.hex()}): "
                             "another version or an already patched main?")

    def add(self, off: int, new: bytes, note: str, code: bool = True, expect: bytes | None = None,
            region: str | None = None) -> None:
        orig = self.nso.read(off, len(new))
        if expect is not None and orig != expect:
            raise SystemExit(f"main+0x{off:x}: original {orig.hex()} != expected {expect.hex()}")
        if orig == new:
            return
        e = {"offset": f"0x{off:X}", "original": orig.hex(), "patched": new.hex(), "note": note}
        if region:
            e["region"] = region
        if code:
            e["asm_original"] = "; ".join(self.dis(orig, off)) if region != "cave" else "(padding)"
            e["asm_patched"] = "; ".join(self.dis(new, off))
        self.patches.append(e)


def check_asm(builder: Builder, words: bytes, addr: int, expected: list[str]) -> None:
    got = builder.dis(words, addr)
    norm = lambda s: " ".join(s.replace(",", " , ").split())  # noqa: E731
    for g, e in zip(got, expected):
        if norm(g) != norm(e):
            raise SystemExit(f"incorrect encoding at 0x{addr:x}: '{g}' != '{e}'")


def sky_expect(bd: Builder, off: int, reg: int, val: float) -> None:
    """The sky sites: fmov sR, #30.0 or fmov sR, w8 (w8 = -300.0 from the previous mov)."""
    if val == 30.0:
        bd.expect(off, w(fmov_s_imm(reg, FMOV_30)))
    else:
        bd.expect(off - 8, w(0x52B872C8))     # mov w8, #0xc3960000 (-300.0f)
        bd.expect(off, w(fmov_s_w(reg, 8)))


def site_pair(bd: Builder, movk_off: int, hi: int = 0x3D08) -> int:
    """Checks the 1/30 (or -1/30 with hi=0xBD08) movz/movk pair and returns the register."""
    word = struct.unpack("<I", bd.nso.read(movk_off, 4))[0]
    rd = word & 31
    bd.expect(movk_off - 4, w(movz(rd, 0x8889), movk(rd, hi, 1)))
    return rd


def gen_fixed(nso: nsoimg.Nso) -> Builder:
    bd = Builder(nso)
    bd.add(FRAME_NS_30, struct.pack("<Q", 16_666_667),
          "rodata: ns per frame of mode 1 (33333334 -> 16666667); read by the game clocks every frame",
          code=False, expect=struct.pack("<Q", 33_333_334))
    bd.expect(PRESENT_INTERVAL, w(0x11000501))  # add w1, w8, #1
    bd.add(PRESENT_INTERVAL, w(movz(1, 1)), "nvnWindowSetPresentInterval(win, 1) instead of cfg+1 (=2)")
    for off, note in PER_FRAME_1_30:
        rd = site_pair(bd, off)
        bd.add(off, w(movk(rd, 0x3C88, 1)), note + " -> 1/60")
    for off, note in PER_FRAME_NEG_1_30:
        rd = site_pair(bd, off, 0xBD08)
        bd.add(off, w(movk(rd, 0xBC88, 1)), note + " -> -1/60")
    for off in PER_FRAME_1_30_DOUBLE:
        bd.expect(off, w(movz(8, 0x2000, 1, x=True), movk(8, 0x1111, 2, x=True), movk(8, 0x3FA1, 3, x=True)))
        bd.add(off + 8, w(movk(8, 0x3F91, 3, x=True)),
              "FUN_7101d5d59c: FUN_7102a1a728((double)1/30, obj, 1) -> (double)(float)1/60")
    bd.expect(VF33_INV, w(movn(8, 0xBE10, 1)))  # mov w8, #0x41efffff (29.999998)
    bd.add(VF33_INV, w(movn(8, 0xBD90, 1)),
          "vtable_7104139aa0::vf33: inverse of the step 29.999998 -> 59.999996 (0x426fffff)")
    # vf17@0x1a7191c: fmov sA,#30 ; fdiv sD,sN,sA -> bl cave ; nop. The cave does sD = sN / 60.
    cave = CAVE
    for off, ra, rd_, rn_, note in FDIV30_SITES:
        bd.expect(off, w(fmov_s_imm(ra, FMOV_30), fdiv_s(rd_, rn_, ra)))
        helper = w(fmov_s_imm(ra, FMOV_30), fadd_s(ra, ra, ra), fdiv_s(rd_, rn_, ra), RET)
        bd.add(cave, helper, f"cave: s{rd_} = s{rn_} / 60 (for 0x{off:x})", region="cave")
        bd.add(off, w(bl(cave, off), NOP), note + " -> / 60 (bl to the cave; x30 is already saved in the prologue)")
        cave += len(helper)
    for off, reg, val, note in SKY_SITES:
        sky_expect(bd, off, reg, val)
        helper = w(ldr_s_lit(reg, cave + 8, cave), RET) + struct.pack("<f", val * 2) + w(NOP)
        bd.add(cave, helper, f"cave: s{reg} = {val * 2:g} (for 0x{off:x})", region="cave")
        bd.add(off, w(bl(cave, off)), note + f" -> {val * 2:g} (bl to the cave)")
        cave += len(helper)
    return bd


def gen_dynamic(nso: nsoimg.Nso) -> Builder:
    bd = Builder(nso)
    vpage, voff = VARS & ~0xFFF, VARS & 0xFFF
    bd.expect(PRESENT_INTERVAL, w(0x11000501))
    bd.add(PRESENT_INTERVAL, w(movz(1, 1)), "nvnWindowSetPresentInterval(win, 1) instead of cfg+1 (=2)")

    # 1) hook in vf1 of the clock manager: clock[i].ns = real time of the last frame.
    bd.expect(CLOCK_SET_MODE, w(0xF0008FC8, 0x91234108, 0xF862D902))  # adrp x8, 0x397e000
    hook = CAVE
    pc = hook
    code: list[int] = []
    labels: dict[str, int] = {}

    def emit(x):
        nonlocal pc
        code.append(x)
        pc += 4

    # Two passes to resolve labels.
    for _pass in range(2):
        code.clear()
        pc = hook
        L = lambda n: labels.get(n, hook)  # noqa: E731
        emit(cmp_w_imm(2, 1))                         # cmp w2, #1
        emit(b_cond(NE, L("orig"), pc))               # b.ne orig (other modes: original table)
        emit(cbnz_w(1, L("cached"), pc))              # only measured on clock 0
        emit(stp_x_pre(29, 30, 31, -0x20))            # stp x29, x30, [sp, #-0x20]!
        emit(MOV_X29_SP)                              # mov x29, sp
        emit(stp_x(0, 1, 31, 0x10))                   # stp x0, x1, [sp, #0x10]
        emit(bl(GET_SYSTEM_TICK_PLT, pc))             # bl nn::os::GetSystemTick
        emit(adrp(9, vpage, pc))                      # adrp x9, VARS
        emit(add_imm(9, 9, voff))                     # add x9, x9, #off
        emit(ldr_x(10, 9, VAR_LAST_TICK))             # ldr x10, [x9]          (previous tick)
        emit(str_x(0, 9, VAR_LAST_TICK))              # str x0, [x9]
        emit(cbz_x(10, L("nominal"), pc))             # first frame -> 1/60
        emit(sub_x(10, 0, 10))                        # x10 = ticks of the frame (19.2 MHz)
        emit(movz(11, 625, x=True))
        emit(mul_x(10, 10, 11))
        emit(movz(11, 12, x=True))
        emit(udiv_x(10, 10, 11))                      # ns = ticks * 625 / 12
        emit(ldr_x_lit(11, L("min_ns"), pc))
        emit(cmp_x(10, 11))
        emit(csel_x(10, 11, 10, LO))                  # ns = max(ns, DT_MIN)
        emit(ldr_x_lit(11, L("max_ns"), pc))
        emit(cmp_x(10, 11))
        emit(csel_x(10, 11, 10, HI))                  # ns = min(ns, DT_MAX)
        emit(b(L("store"), pc))
        labels["nominal"] = pc
        emit(ldr_x_lit(10, L("nominal_ns"), pc))
        labels["store"] = pc
        emit(str_x(10, 9, VAR_FRAME_NS))              # frame_ns
        emit(ucvtf_d_x(16, 10))
        emit(ldr_d_lit(17, L("ns_to_s"), pc))
        emit(fmul_d(16, 16, 17))
        emit(fcvt_s_d(16, 16))
        emit(str_s(16, 9, VAR_DT))                    # dt (s, float)
        emit(fneg_s(17, 16))
        emit(str_s(17, 9, VAR_NEG_DT))                # -dt (s, float)
        emit(fmov_s_imm(17, FMOV_1))
        emit(fdiv_s(17, 17, 16))
        emit(fmov_s_imm(16, FMOV_NEG10))
        emit(fmul_s(16, 16, 17))
        emit(str_s(16, 9, VAR_NEG10_INV_DT))          # -10/dt (s, float): -300 at 30 FPS
        emit(dup_4s_elem0(17, 17))
        emit(str_q(17, 9, VAR_INV_DT))                # 1/dt replicated x4
        emit(ldp_x(0, 1, 31, 0x10))
        emit(ldp_x_post(29, 30, 31, 0x20))
        labels["cached"] = pc
        emit(adrp(9, vpage, pc))
        emit(add_imm(9, 9, voff))
        emit(ldr_x(2, 9, VAR_FRAME_NS))
        emit(cbnz_x(2, L("tail"), pc))
        emit(ldr_x_lit(2, L("nominal_ns"), pc))
        labels["tail"] = pc
        emit(ldr_x(8, 0, 0))                          # ldr x8, [x0]
        emit(ldr_x(3, 8, 0x10))                       # ldr x3, [x8, #0x10]
        emit(BR_X3)                                   # vf2(this, i, ns)
        labels["orig"] = pc                           # copy of the original vf1
        emit(adrp(8, FRAME_NS_TABLE, pc))
        emit(add_imm(8, 8, FRAME_NS_TABLE & 0xFFF))
        emit(0xF862D902)                              # ldr x2, [x8, w2, sxtw #3]
        emit(ldr_x(8, 0, 0))
        emit(ldr_x(3, 8, 0x10))
        emit(BR_X3)
        if pc % 8:
            emit(NOP)
        labels["min_ns"] = pc; pc += 8               # noqa: E702
        labels["max_ns"] = pc; pc += 8               # noqa: E702
        labels["nominal_ns"] = pc; pc += 8           # noqa: E702
        labels["ns_to_s"] = pc; pc += 8              # noqa: E702
    blob = w(*code) + struct.pack("<QQQd", DT_MIN_NS, DT_MAX_NS, 16_666_667, 1e-9)
    check_asm(bd, w(*code[:3]), hook, ["cmp w2, #1"])
    bd.add(hook, blob, "cave: dynamic vf1. Measures the frame with nn::os::GetSystemTick, clamps it to "
          "[1/1000, 1/15] s and stores frame_ns, dt and 1/dt in .bss+0x4350F00", region="cave")
    bd.add(CLOCK_SET_MODE, w(b(hook, CLOCK_SET_MODE)),
          "vtable_7104171528::vf1: jump to the cave (the game clocks take the real time)")
    cave = hook + len(blob)
    cave = (cave + 15) & ~15

    # 2) 1/30 sites: movz wR,#0x8889 ; movk wR,#0x3d08 -> adrp xR, VARS ; ldr wR, [xR, #dt]
    for off, note in PER_FRAME_1_30:
        rd = site_pair(bd, off)
        bd.add(off - 4, w(adrp(rd, vpage, off - 4), ldr_w(rd, rd, voff + VAR_DT)), note + " -> real dt")
    for off, note in PER_FRAME_NEG_1_30:
        rd = site_pair(bd, off, 0xBD08)
        bd.add(off - 4, w(adrp(rd, vpage, off - 4), ldr_w(rd, rd, voff + VAR_NEG_DT)), note + " -> -dt")
    for off in PER_FRAME_1_30_DOUBLE:
        bd.expect(off, w(movz(8, 0x2000, 1, x=True), movk(8, 0x1111, 2, x=True), movk(8, 0x3FA1, 3, x=True),
                        0x9E670100))
        bd.add(off, w(adrp(8, vpage, off), ldr_s(0, 8, voff + VAR_DT), fcvt_d_s(0, 0), NOP),
              "FUN_7101d5d59c: FUN_7102a1a728((double)dt, obj, 1)")
    bd.expect(VF33_INV, w(movn(8, 0xBE10, 1), DUP_4S_W8))
    bd.add(VF33_INV, w(adrp(8, vpage, VF33_INV), ldr_q(0, 8, voff + VAR_INV_DT)),
          "vtable_7104139aa0::vf33: inverse of the step -> 1/dt (v0.4s)")
    for off, ra, rd_, rn_, note in FDIV30_SITES:
        bd.expect(off, w(fmov_s_imm(ra, FMOV_30), fdiv_s(rd_, rn_, ra)))
        helper = w(adrp(8, vpage, cave), ldr_s(ra, 8, voff + VAR_DT), fmul_s(rd_, rn_, ra), RET)
        bd.add(cave, helper, f"cave: s{rd_} = s{rn_} * dt (for 0x{off:x}; uses x8, free there)", region="cave")
        bd.add(off, w(bl(cave, off), NOP), note + " -> * dt (bl to the cave)")
        cave += len(helper)
    for off, reg, val, note in SKY_SITES:
        sky_expect(bd, off, reg, val)
        var = VAR_INV_DT if val == 30.0 else VAR_NEG10_INV_DT
        helper = w(adrp(16, vpage, cave), ldr_s(reg, 16, voff + var), RET)
        bd.add(cave, helper, f"cave: s{reg} = {'1/dt' if val == 30.0 else '-10/dt'} (for 0x{off:x}; uses x16)", region="cave")
        bd.add(off, w(bl(cave, off)), note + f" -> {'1/dt' if val == 30.0 else '-10/dt'} (bl to the cave)")
        cave += len(helper)
    return bd


def gen_interval0(nso: nsoimg.Nso) -> Builder:
    bd = Builder(nso)
    bd.expect(PRESENT_INTERVAL, w(0x11000501))
    bd.add(PRESENT_INTERVAL, w(movz(1, 0)),
           "nvnWindowSetPresentInterval(win, 0): no vsync wait on console; in suyu interval 0 "
           "is interpreted as x2 speed (vsync at 120 Hz with the limiter active)")
    return bd


def write_outputs(builder: Builder, out: Path, stem: str, mod_name: str, title: str, desc: str,
                  nso: nsoimg.Nso, runtime_vars: bool) -> None:
    patches = sorted(builder.patches, key=lambda p: int(p["offset"], 16))
    gap_lo, gap_hi = nso.text_gap()
    cave_end = max((int(p["offset"], 16) + len(p["patched"]) // 2 for p in patches
                    if gap_lo <= int(p["offset"], 16) < gap_hi), default=None)
    doc = {
        "format": "pla-patchlist-v1",
        "name": title,
        "description": desc,
        "title_id": TITLE_ID,
        "version": "1.1.1",
        "module": "main",
        "build_id": BUILD_ID,
        "offsets": "offset of the main module (= Ghidra address - 0x7100000000; = pchtxt offset with "
                   "offset_shift 0x100; IPS = offset + 0x100)",
        "text_end_original": f"0x{gap_lo:X}",
        "requires_text_extension_to": f"0x{(cave_end + 0xFF) & ~0xFF:X}" if cave_end else None,
        "runtime_vars_bss": {"base": f"0x{VARS:X}", "last_tick": "+0x00 u64", "frame_ns": "+0x08 u64",
                             "dt": "+0x10 f32", "neg_dt": "+0x14 f32", "neg10_inv_dt": "+0x1c f32", "inv_dt_x4": "+0x20 f32[4]",
                             "dt_clamp_ns": [DT_MIN_NS, DT_MAX_NS]} if runtime_vars else None,
        "generator": "tools/patch/gen_60fps.py",
        "patches": patches,
    }
    doc = {k: v for k, v in doc.items() if v is not None}
    out.mkdir(parents=True, exist_ok=True)
    jpath = out / f"{stem}.json"
    jpath.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")

    # pchtxt (IPSwitch): module offsets with offset_shift 0x100.
    lines = [f"@nsobid-{BUILD_ID}", "", f"# Title {TITLE_ID} v1.1.1 - {title}",
             "# Generated by tools/patch/gen_60fps.py; see docs/research/06-framerate.md", "",
             "@flag offset_shift 0x100", "", f"// {title}", "@enabled"]
    for p in patches:
        lines.append(f"// {p['note']}")
        lines.append(f"{int(p['offset'], 16):08X} {p['patched'].upper()}")
    lines += ["@stop", ""]
    mod = out / "suyu" / mod_name / "exefs"
    mod.mkdir(parents=True, exist_ok=True)
    (mod / f"{stem}.pchtxt").write_text("\n".join(lines), encoding="utf-8")

    # IPS32: "IPS32" + (u32 BE offset, u16 BE size, data)* + "EEOF". Offset = module + 0x100.
    ips = bytearray(b"IPS32")
    for p in patches:
        data = bytes.fromhex(p["patched"])
        off = int(p["offset"], 16) + 0x100
        for i in range(0, len(data), 0xFFFF):
            chunk = data[i:i + 0xFFFF]
            ips += struct.pack(">IH", off + i, len(chunk)) + chunk
    ips += b"EEOF"
    ipsdir = out / "ips" / stem
    ipsdir.mkdir(parents=True, exist_ok=True)
    (ipsdir / f"{BUILD_ID}.ips").write_bytes(bytes(ips))
    print(f"[{stem}] {len(patches)} patches -> {jpath}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nso", default=str(REPO / "data" / "exefs" / "main"))
    ap.add_argument("--out", default=str(REPO / "recomp" / "patches"), help="parent directory of 60fps/ and unlocked-fps/")
    args = ap.parse_args()
    nso = nsoimg.load(args.nso)
    if nso.build_id_short != BUILD_ID:
        raise SystemExit(f"build id {nso.build_id_short} != {BUILD_ID}")
    out = Path(args.out)
    write_outputs(gen_dynamic(nso), out / "unlocked-fps", "pla-111-unlocked-fps", "Unlocked FPS (PLA recomp)",
                  "Unlocked FPS (dynamic delta)",
                  "Present interval 1 (without the 30 cap) and the game clocks take the real time "
                  "of the last frame, clamped to [1/1000, 1/15] s. Sites that add 1/30 per frame read that dt "
                  "from .bss. The real cap is set by suyu's limiter/vsync.", nso, True)
    write_outputs(gen_interval0(nso), out / "unlocked-fps", "pla-111-present-interval-0",
                  "Unlocked FPS + present interval 0 (optional)", "Present interval 0 (optional)",
                  "Optional add-on for unlocked-fps: requests interval 0. In suyu it is equivalent to vsync at 120 Hz with "
                  "the limiter active. It is incompatible with the main patch (both touch 0x2AA278C): "
                  "use only for testing, applying it afterwards.", nso, False)
    write_outputs(gen_fixed(nso), out / "60fps", "pla-111-60fps-fixed", "60FPS fixed (PLA recomp)",
                  "Fixed 60 FPS (corrected delta)",
                  "Frame time from 33.3 ms to 16.7 ms, present interval 1 and the sites that add 1/30 "
                  "per frame go to 1/60. Logic runs at real speed only if the game holds 60 FPS.",
                  nso, False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
