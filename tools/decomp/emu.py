"""Differential equivalence with Unicorn: runs the original and the compiled function with the
same inputs and compares their observable effects.

Execution model (summary; details in docs/decomp/CONTRIBUTING.md):
- The whole main.elf is mapped at 0x7100000000 with relocations applied; our linked .o
  is mapped at SCRATCH (right after the image, so BL reaches all the code).
- Deterministic "lazy" memory: any unmapped page (< 2^48, except the null page)
  is created on first access with pseudorandom content derived from (case seed, page):
  a mix of zeros, small integers, floats and pointers to more lazy memory. This way arbitrary
  pointer chains can be followed and both executions see exactly the same memory.
- Every entry into code outside the function (BL, BLR, tail B, PLT, garbage pointers) is
  intercepted: the call is recorded (target, arguments, state of the memory written up to
  that moment), a deterministic value depending on (case, target, n-th call to that
  target) is returned, x2-x17 are clobbered with deterministic values and execution returns to LR.
  memcpy/memmove/memset/strlen/strcmp/strncmp/memcmp are emulated (they are transparent).
- Optional (verify.py --ret-set/--sret/--seed-globals/--follow): returns from a small set,
  filling of the indirect result (x8), seeded .data/.bss and real execution of already
  verified functions. Imports that do not return end the case; loops without progress and those
  with more than MAX_CALLS calls are cut off (inconclusive).
- What is compared: return value (per the declared type), call sequence (target +
  arguments the target really reads, by static analysis of the target), written
  memory (final state, excluding the local stack), callee-saved registers and memory faults.
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

import numpy as np
from capstone import CS_ARCH_ARM64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_MODE_ARM, Cs
from unicorn import (UC_ARCH_ARM64, UC_HOOK_BLOCK, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE,
                     UC_MEM_FETCH_UNMAPPED, UC_MODE_ARM, UC_PROT_ALL, Uc, UcError)
from unicorn import arm64_const as A

from image import IMPORT_BASE, Image

PAGE = 0x1000
SCRATCH = 0x71_0440_0000          # our code (within 128 MiB of all of .text)
SCRATCH_SIZE = 0x40_0000
IMPORT_SIZE = 0x10_0000
LAZY_BASE = 0x0000_1000_0000_0000  # generated pointers point here
LAZY_SPAN = 1 << 32
STACK_TOP = 0x0000_7FF0_0010_0000
STACK_SIZE = 0x10_0000
STACK_ARGS = 0x1_0000              # area above the initial SP (stack arguments)
SENTINEL = 0x0000_7FE0_0000_0000
TLS = LAZY_BASE + 0xF000_0000
MAX_INSNS = 2_000_000
NULL_LIMIT = 0x1_0000
CANON_LIMIT = 1 << 48

X = [getattr(A, f"UC_ARM64_REG_X{i}") for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
Q = [getattr(A, f"UC_ARM64_REG_Q{i}") for i in range(32)]
D = [getattr(A, f"UC_ARM64_REG_D{i}") for i in range(32)]
SP, PC = A.UC_ARM64_REG_SP, A.UC_ARM64_REG_PC

# Memory/input profiles: weights of (zero, small integer, pointer, random, float)
PROFILES = [
    ("pointers", [0.03, 0.04, 0.88, 0.02, 0.03]),
    ("zeros", [0.70, 0.10, 0.15, 0.02, 0.03]),
    ("pointers", [0.03, 0.04, 0.88, 0.02, 0.03]),
    ("integers", [0.20, 0.50, 0.15, 0.05, 0.10]),
    ("pointers", [0.05, 0.10, 0.75, 0.05, 0.05]),
    ("mixed", [0.20, 0.20, 0.30, 0.10, 0.20]),
]
TRANSPARENT = {"memcpy", "memmove", "memset", "strlen", "strcmp", "strncmp", "memcmp", "__memcpy_chk",
               "bzero"}
# Imports that do not return: the call ends the case (in both versions) even if the return address
# falls inside the function (the compiler may place code after the BL).
NORETURN_IMPORTS = {"abort", "exit", "_exit", "_Exit", "quick_exit", "__assert_fail", "__cxa_throw", "__cxa_rethrow",
                    "_ZSt9terminatev", "longjmp", "_longjmp", "__stack_chk_fail", "nndetailDiagAbortFormatImpl",
                    "_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv",
                    "_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv",
                    "_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv",
                    "_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv"}
NORETURN_PREFIXES = ("_ZN2nn4diag6detail9AbortImpl", "_ZN2nn4diag6detail5Abort", "_ZN2nn6detail13UnexpectedDefault")
M64 = (1 << 64) - 1
SPIN_CHECK = 2000     # visits to the same block after which a loop without progress is looked for
MAX_FOLLOW = 4000     # entries into "followed" functions (--follow) per execution
MAX_CALLS = 3000      # intercepted calls per execution: more = huge loop (inconclusive, like the limit)


def is_noreturn_import(name) -> bool:
    return bool(name) and (name in NORETURN_IMPORTS or name.startswith(NORETURN_PREFIXES))


def _seed(*parts) -> int:
    h = hashlib.blake2b(repr(parts).encode(), digest_size=8).digest()
    return int.from_bytes(h, "little")


def gen_words(seed: int, n: int, weights) -> np.ndarray:
    rng = np.random.default_rng(seed)
    kind = rng.choice(5, size=n, p=np.array(weights) / sum(weights))
    small = rng.integers(0, 16, size=n, dtype=np.uint64)
    big = rng.integers(0, 0x10000, size=n, dtype=np.uint64)
    mask = rng.random(n) < 0.2
    small[mask] = big[mask]
    ptr = (np.uint64(LAZY_BASE) + rng.integers(0, LAZY_SPAN >> 4, size=n, dtype=np.uint64) * np.uint64(16))
    # "random": mostly 32-bit integers; some full 64-bit values (non-canonical)
    rnd = rng.integers(0, 1 << 32, size=n, dtype=np.uint64)
    full = rng.random(n) < 0.1
    rnd[full] = rng.integers(0, 1 << 63, size=n, dtype=np.uint64)[full] | np.uint64(1 << 63)
    fl = rng.uniform(-64.0, 64.0, size=2 * n).astype(np.float32).view(np.uint32).astype(np.uint64)
    flt = fl[0::2] | (fl[1::2] << np.uint64(32))
    out = np.zeros(n, dtype=np.uint64)
    out[kind == 1] = small[kind == 1]
    out[kind == 2] = ptr[kind == 2]
    out[kind == 3] = rnd[kind == 3]
    out[kind == 4] = flt[kind == 4]
    return out


@dataclass
class Case:
    idx: int
    seed: int
    profile: str
    weights: list
    regs: dict = field(default_factory=dict)     # x0..x28 -> value
    vregs: dict = field(default_factory=dict)    # v0..v7 -> value (128 bits)
    mem: list = field(default_factory=list)      # [(addr, bytes)] from snapshots
    name: str = ""
    ret_set: bool = False    # returns of intercepted calls chosen from a small set (0, 1, -1, ...)
    sret: bool = False       # fill the indirect result (x8) of intercepted calls
    follow: bool = False     # execute the Machine.follow functions instead of intercepting them


@dataclass
class Call:
    target: int
    x: list
    v: list
    mem: dict        # state of the written memory (without local stack) at the time of the call
    retx: int = 0
    noreturn: bool = False


@dataclass
class RunResult:
    calls: list
    written: dict          # address -> final byte (non-local memory)
    ret_x: list            # x0, x1
    ret_v: list            # v0..v3 (128 bits)
    fault: str | None
    callee_saved_ok: bool
    callee_saved_msg: str
    blocks: set
    insns: int
    timeout: bool
    noreturn: bool = False


class Machine:
    def __init__(self, image: Image, labeler=None):
        self.im = image
        self.label = labeler or (lambda a: f"{a:#x}")
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc = uc
        uc.mem_map(image.base, (image.size + PAGE - 1) & ~(PAGE - 1), UC_PROT_ALL)
        uc.mem_write(image.base, bytes(image.blob))
        uc.mem_map(IMPORT_BASE, IMPORT_SIZE, UC_PROT_ALL)
        uc.mem_map(SCRATCH, SCRATCH_SIZE, UC_PROT_ALL)
        uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
        uc.mem_map(SENTINEL, PAGE, UC_PROT_ALL)
        self.scratch_blob = b""
        self._stack_zero = b"\0" * (STACK_SIZE - STACK_ARGS)
        self._page_cache: dict = {}
        self.lazy_pages: set[int] = set()
        self.case: Case | None = None
        self.own: list[tuple[int, int]] = []
        self.entry = 0
        self.undo: list[tuple[int, bytes]] = []
        self.written_ranges: dict[int, int] = {}
        self.calls: list[Call] = []
        self.call_counts: dict[int, int] = {}
        self.blocks: set = set()
        self.fault: str | None = None
        self.sp0 = STACK_TOP - STACK_ARGS
        self.n_blocks = 0
        # Options (set by verify.py): arity oracle (for the sret), main functions that are
        # really executed instead of intercepted ({entry: end}) and whether the sret is always filled.
        self.oracle = None
        self.follow: dict[int, int] = {}
        self.follow_skip = 0      # the function being verified (its recursion is not followed)
        self.sret_all = False
        self.active_follow: list[tuple[int, int]] = []
        self.n_follow = 0
        self.block_visits: dict[int, int] = {}
        self.block_state: dict[int, tuple] = {}
        self.spin = False
        self.stack_lo = STACK_TOP - STACK_SIZE
        self._noreturn = {a for a, n in image.plt.items() if is_noreturn_import(n)}
        self._noreturn.update(a for a, n in image.imports.items() if is_noreturn_import(n))
        uc.hook_add(UC_HOOK_BLOCK, self._on_block)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self._transparent = {a: n for a, n in image.plt.items() if n in TRANSPARENT}
        self._transparent.update({image.import_addr[n]: n for n in TRANSPARENT if n in image.import_addr})

    # ------------------------------------------------------------ memory
    def load_scratch(self, blob: bytes):
        if len(blob) > SCRATCH_SIZE:
            raise ValueError("the linked object does not fit in SCRATCH")
        self.scratch_blob = bytes(blob)
        self.uc.mem_write(SCRATCH, self.scratch_blob + b"\0" * (SCRATCH_SIZE - len(blob)))

    def lazy_page(self, page: int) -> bytes:
        c = self.case
        key = (c.seed, page)
        b = self._page_cache.get(key)
        if b is None:
            if len(self._page_cache) > 4096:
                self._page_cache.clear()
            b = gen_words(_seed("page", c.seed, page), PAGE // 8, c.weights).tobytes()
            self._page_cache[key] = b
        return b

    def initial_bytes(self, a: int, n: int) -> bytes:
        """Initial contents (before execution) of [a, a+n), the same for both executions."""
        out = bytearray()
        for x in range(a, a + n):
            out.append(self._initial_byte(x))
        return bytes(out)

    def _initial_byte(self, a: int) -> int:
        for (ma, mb) in self.case.mem:
            if ma <= a < ma + len(mb):
                return mb[a - ma]
        im = self.im
        if im.base <= a < im.base + im.size:
            return im.blob[a - im.base]
        if SCRATCH <= a < SCRATCH + SCRATCH_SIZE:
            o = a - SCRATCH
            return self.scratch_blob[o] if o < len(self.scratch_blob) else 0
        if STACK_TOP - STACK_SIZE <= a < STACK_TOP:
            return self._stack_init[a - (STACK_TOP - STACK_SIZE)]
        if IMPORT_BASE <= a < IMPORT_BASE + IMPORT_SIZE or SENTINEL <= a < SENTINEL + PAGE:
            return 0
        page = a & ~(PAGE - 1)
        return self.lazy_page(page)[a - page]

    def _map_lazy(self, page: int) -> bool:
        if page < NULL_LIMIT or page >= CANON_LIMIT:
            return False
        if page in self.lazy_pages:
            return True
        try:
            self.uc.mem_map(page, PAGE, UC_PROT_ALL)
        except UcError:
            return False
        self.uc.mem_write(page, self.lazy_page(page))
        self.lazy_pages.add(page)
        return True

    def _on_unmapped(self, uc, access, addr, size, value, ud):
        ok = True
        for page in {addr & ~(PAGE - 1), (addr + max(size, 1) - 1) & ~(PAGE - 1)}:
            ok = ok and self._map_lazy(page)
        if not ok and self.fault is None:
            kind = "read" if access != UC_MEM_FETCH_UNMAPPED else "execute"
            if access in (20, 22):  # WRITE_UNMAPPED
                kind = "write"
            self.fault = f"invalid {kind} at {addr:#x}"
        return ok

    def _on_write(self, uc, access, addr, size, value, ud):
        try:
            old = bytes(uc.mem_read(addr, size))
        except UcError:
            # the write hook fires before the unmapped-memory one: map now
            for page in {addr & ~(PAGE - 1), (addr + size - 1) & ~(PAGE - 1)}:
                if not self._is_mapped(page):
                    self._map_lazy(page)
            try:
                old = bytes(uc.mem_read(addr, size))
            except UcError:
                return
        self.undo.append((addr, old))
        if not (self.stack_lo <= addr < self.sp0):
            for a in range(addr, addr + size):
                self.written_ranges[a] = 1

    def host_write(self, addr: int, data: bytes):
        """Write done by the emulator (not by an instruction): Unicorn's hooks do not see it,
        so it is recorded by hand to undo it at the end and to compare it."""
        n = len(data)
        if n <= 0:
            return
        for page in range(addr & ~(PAGE - 1), addr + n, PAGE):
            if not self._is_mapped(page):
                if not self._map_lazy(page):
                    raise UcError(6)
        old = bytes(self.uc.mem_read(addr, n))
        self.undo.append((addr, old))
        self.uc.mem_write(addr, bytes(data))
        if not (self.stack_lo <= addr < self.sp0):
            for a in range(addr, addr + n):
                self.written_ranges[a] = 1

    def host_read(self, addr: int, n: int) -> bytes:
        """Emulator read that creates any missing lazy memory (as an instruction would)."""
        if n <= 0:
            return b""
        for page in range(addr & ~(PAGE - 1), addr + n, PAGE):
            if not self._is_mapped(page):
                if not self._map_lazy(page):
                    raise UcError(6)
        return bytes(self.uc.mem_read(addr, n))

    def _on_intr(self, uc, intno, ud):
        self.fault = f"interrupt/svc {intno} at {uc.reg_read(PC):#x}"
        uc.emu_stop()

    # ------------------------------------------------------------ calls
    def _in_own(self, a: int) -> bool:
        for lo, hi in self.own:
            if lo <= a < hi:
                return True
        for lo, hi in self.active_follow:
            if lo <= a < hi:
                return True
        return False

    def _in_follow(self, a: int) -> bool:
        for lo, hi in self.active_follow:
            if lo <= a < hi:
                return True
        return False

    def _spin_check(self, uc, addr: int) -> bool:
        """Loop without progress? (same block, same registers and no write since the last
        visit). Only checked after SPIN_CHECK visits to the block, so as not to slow things down."""
        k = self.block_visits.get(addr, 0) + 1
        self.block_visits[addr] = k
        if k < SPIN_CHECK or k % 64:
            return False   # (a truly stuck loop has the same state at every check)
        st = (tuple(uc.reg_read(X[i]) for i in range(31)), uc.reg_read(SP), uc.reg_read(A.UC_ARM64_REG_NZCV),
              len(self.undo))
        prev = self.block_state.get(addr)
        self.block_state[addr] = st
        return prev == st

    def _mem_snapshot(self) -> dict:
        if not self.written_ranges:
            return {}
        out = {}
        addrs = sorted(self.written_ranges)
        # read in contiguous runs
        i = 0
        while i < len(addrs):
            j = i
            while j + 1 < len(addrs) and addrs[j + 1] == addrs[j] + 1:
                j += 1
            data = self.uc.mem_read(addrs[i], addrs[j] - addrs[i] + 1)
            for k, b in enumerate(data):
                out[addrs[i] + k] = b
            i = j + 1
        return out

    def _on_block(self, uc, addr, size, ud):
        self.n_blocks += 1
        if self._in_own(addr):
            if self._spin_check(uc, addr):
                # wait loop without progress: cut off as if the limit were exhausted (inconclusive)
                self.spin = True
                uc.emu_stop()
                return
            if self.active_follow and not any(lo <= addr < hi for lo, hi in self.own):
                return   # code of a followed function (does not count toward coverage)
            if addr == self.entry and self._entered:
                lr = uc.reg_read(X[30])
                if not (self._in_own(lr - 4) and self._is_call_insn(lr - 4)):
                    self.blocks.add((addr, size))
                    return
                # recursion: treat as a call
            else:
                self._entered = True
                self.blocks.add((addr, size))
                return
        if addr == SENTINEL:
            return
        end = self.follow.get(addr) if self.case.follow else None
        if end is not None and self.n_follow < MAX_FOLLOW and addr not in self._noreturn and addr != self.follow_skip:
            # --follow: already-verified game function (O/F): really executed (the same in both)
            self.n_follow += 1
            if (addr, end) not in self.active_follow:
                self.active_follow.append((addr, end))
            return
        self._intercept(uc, addr)

    def _is_call_insn(self, a: int) -> bool:
        try:
            w = struct.unpack("<I", self.uc.mem_read(a, 4))[0]
        except UcError:
            return False
        return (w & 0xFC000000) == 0x94000000 or (w & 0xFFFFFC1F) == 0xD63F0000

    def _intercept(self, uc, target: int):
        name = self._transparent.get(target)
        if name:
            self._emulate_libc(uc, name)
            uc.reg_write(PC, uc.reg_read(X[30]))
            return
        if len(self.calls) >= MAX_CALLS:
            # loop over a huge "garbage" vector with one call per iteration: each call costs
            # much more than one instruction; cut off as if the limit were exhausted (inconclusive)
            self.spin = True
            uc.emu_stop()
            return
        xs = [uc.reg_read(X[i]) for i in range(9)]
        vs = [uc.reg_read(Q[i]) for i in range(8)]
        n = self.call_counts.get(target, 0)
        self.call_counts[target] = n + 1
        c = self.case
        rv = gen_words(_seed("ret", c.seed, target, n), 20, c.weights)
        x0 = int(rv[0])
        vsel = None
        if c.ret_set:
            # Return value chosen from a small set, deterministic per (case, target, n-th
            # call to that target): the same in the original and in ours.
            h = _seed("rset", c.seed, target, n)
            ptr = LAZY_BASE + ((h >> 8) % (LAZY_SPAN >> 4)) * 16
            x0 = [0, 1, M64, ptr, xs[0], xs[1], x0, 2][h % 8]
            vsel = (h >> 3) % 5
        if (c.sret or c.ret_set or self.sret_all) and self.oracle is not None:
            size = self.oracle.sret_size(target)
            if size:
                data = gen_words(_seed("sret", c.seed, target, n), (size + 7) // 8, c.weights).tobytes()[:size]
                try:
                    self.host_write(xs[8], data)
                except UcError:
                    pass
        self.calls.append(Call(target, xs, vs, self._mem_snapshot(), x0))
        uc.reg_write(X[0], x0)
        uc.reg_write(X[1], int(rv[1]))
        for i in range(2, 18):
            uc.reg_write(X[i], int(rv[i]))
        fl = np.random.default_rng(_seed("retv", c.seed, target, n)).uniform(-64, 64, 8)
        for i in range(4):
            lo = int(np.float32(fl[2 * i]).view(np.uint32)) | (int(np.float32(fl[2 * i + 1]).view(np.uint32)) << 32)
            if i == 0 and vsel is not None and vsel < 4:
                lo = [0, 0x3F800000, 0xBF800000, 0x3FF0000000000000][vsel]   # 0.0, 1.0f, -1.0f, 1.0 (double)
            uc.reg_write(Q[i], lo)
        lr = uc.reg_read(X[30])
        if target in self._noreturn:
            # import that does not return (abort, AbortImpl, __cxa_throw...): end of the case in both
            self.calls[-1].noreturn = True
            uc.reg_write(PC, SENTINEL)
            uc.emu_stop()
            return
        if lr != SENTINEL and not self._in_own(lr):
            # returns outside the function: the call does not return (abort, luaL_error...): end of the case
            self.calls[-1].noreturn = True
            uc.reg_write(PC, SENTINEL)
            uc.emu_stop()
            return
        uc.reg_write(PC, lr)

    def _emulate_libc(self, uc, name: str):
        x0, x1, x2 = (uc.reg_read(X[i]) for i in range(3))
        lim = 1 << 20

        def cstr(a):
            out = bytearray()
            while len(out) < lim:
                b = self.host_read(a + len(out), 1)[0]
                if b == 0:
                    return bytes(out)
                out.append(b)
            return bytes(out)
        try:
            # host_write: the hooks do not see these writes; this way they are undone at the end and compared
            if name in ("memcpy", "memmove", "__memcpy_chk"):
                if 0 < x2 < lim:
                    self.host_write(x0, self.host_read(x1, x2))
            elif name == "memset":
                if 0 < x2 < lim:
                    self.host_write(x0, bytes([x1 & 0xFF]) * x2)
            elif name == "bzero":
                if 0 < x1 < lim:
                    self.host_write(x0, b"\0" * x1)
            elif name == "strlen":
                uc.reg_write(X[0], len(cstr(x0)))
            elif name in ("strcmp", "strncmp", "memcmp"):
                if name == "memcmp":
                    a, b = self.host_read(x0, min(x2, lim)), self.host_read(x1, min(x2, lim))
                else:
                    a, b = cstr(x0), cstr(x1)
                    if name == "strncmp":
                        a, b = a[:x2], b[:x2]
                r = 0
                for p, q in zip(a + (b"\0" if name != "memcmp" else b""), b + (b"\0" if name != "memcmp" else b"")):
                    if p != q:
                        r = p - q
                        break
                uc.reg_write(X[0], r & 0xFFFFFFFFFFFFFFFF)
        except UcError:
            self.fault = f"{name}: invalid access"
            uc.emu_stop()

    # ------------------------------------------------------------ execution
    def run(self, entry: int, own: list, case: Case, count_limit: int = MAX_INSNS) -> RunResult:
        uc = self.uc
        self.case = case
        self.own = own
        self.entry = entry
        self._entered = False
        self.undo = []
        self.written_ranges = {}
        self.calls = []
        self.call_counts = {}
        self.blocks = set()
        self.fault = None
        self.n_blocks = 0
        self.active_follow = []
        self.n_follow = 0
        self.block_visits = {}
        self.block_state = {}
        self.spin = False
        self.stack_lo = STACK_TOP - STACK_SIZE
        # stack: zero below the SP; deterministic stack arguments above
        above = gen_words(_seed("stack", case.seed), STACK_ARGS // 8, case.weights).tobytes()
        self._stack_init = self._stack_zero + above
        uc.mem_write(STACK_TOP - STACK_ARGS, above)   # below the SP it stays zero (restored by the undo)
        for a, b in case.mem:
            for page in range(a & ~(PAGE - 1), a + len(b), PAGE):
                if not self._is_mapped(page):
                    self._map_lazy(page)
            uc.mem_write(a, b)
        for i in range(29):
            uc.reg_write(X[i], case.regs.get(i, 0))
        for i in range(32):
            uc.reg_write(Q[i], case.vregs.get(i, 0))
        uc.reg_write(X[29], self.sp0 + 0x40)
        uc.reg_write(X[30], SENTINEL)
        uc.reg_write(SP, self.sp0)
        uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, TLS)
        uc.reg_write(A.UC_ARM64_REG_TPIDRRO_EL0, TLS)
        uc.reg_write(A.UC_ARM64_REG_NZCV, 0)
        timeout = False
        try:
            uc.emu_start(entry, SENTINEL, count=count_limit)
            if (uc.reg_read(PC) != SENTINEL and self.fault is None) or self.spin:
                timeout = True
        except UcError as e:
            if self.fault is None:
                self.fault = f"CPU error: {e} at pc={uc.reg_read(PC):#x}"
        ret_x = [uc.reg_read(X[0]), uc.reg_read(X[1])]
        ret_v = [uc.reg_read(Q[i]) for i in range(4)]
        ok, msg = True, ""
        noret = bool(self.calls and self.calls[-1].noreturn)
        if self.fault is None and not timeout and not noret:
            bad = []
            for i in range(19, 30):
                if uc.reg_read(X[i]) != case.regs.get(i, 0) and not (i == 29 and uc.reg_read(X[29]) == self.sp0 + 0x40):
                    bad.append(f"x{i}")
            if uc.reg_read(SP) != self.sp0:
                bad.append("sp")
            for i in range(8, 16):
                if uc.reg_read(D[i]) != case.vregs.get(i, 0) & 0xFFFFFFFFFFFFFFFF:
                    bad.append(f"d{i}")
            if bad:
                ok, msg = False, "does not restore callee-saved registers: " + ", ".join(bad)
        written = self._mem_snapshot()
        res = RunResult(self.calls, written, ret_x, ret_v, self.fault, ok, msg, self.blocks, self.n_blocks, timeout,
                        noret)
        # undo writes and lazy pages
        for a, old in reversed(self.undo):
            try:
                uc.mem_write(a, old)
            except UcError:
                pass
        for page in self.lazy_pages:
            uc.mem_unmap(page, PAGE)
        self.lazy_pages = set()
        if case.mem:
            for a, b in case.mem:
                try:
                    uc.mem_write(a, self.initial_bytes_no_override(a, len(b)))
                except UcError:
                    pass
        return res

    def initial_bytes_no_override(self, a, n):
        saved = self.case.mem
        self.case.mem = []
        try:
            return self.initial_bytes(a, n)
        finally:
            self.case.mem = saved

    def _is_mapped(self, page: int) -> bool:
        for b, e, _ in self.uc.mem_regions():
            if b <= page <= e:
                return True
        return False


# ------------------------------------------------------------------ cases
def _gen_case(func_addr: int, i: int, tag: str = "case") -> Case:
    pname, w = PROFILES[i % len(PROFILES)]
    s = _seed(tag, func_addr, i)
    # arguments: mostly pointers; one in every 6 cases uses the case's profile
    vals = gen_words(_seed("args", s), 29, w if i % 6 == 5 else [0.03, 0.2, 0.72, 0.0, 0.05])
    regs = {k: int(vals[k]) for k in range(29)}
    regs[8] = int(LAZY_BASE + (_seed("x8", s) % (LAZY_SPAN >> 4)) * 16)
    if i % 8 == 0:   # always some case with all arguments as pointers
        for k in range(8):
            regs[k] = int(LAZY_BASE + (_seed("p", s, k) % (LAZY_SPAN >> 4)) * 16)
    rng = np.random.default_rng(_seed("v", s))
    vregs = {}
    for k in range(32):
        f = rng.uniform(-100, 100, 2).astype(np.float32).view(np.uint32)
        lo = int(f[0]) | (int(f[1]) << 32)
        if i % 2:   # in odd cases, d-registers with reasonable doubles
            lo = int(np.float64(rng.uniform(-100, 100)).view(np.uint64))
        vregs[k] = lo | (int(rng.integers(0, 1 << 62)) << 64)
    return Case(i, s, pname, w, regs, vregs, name=f"#{i} ({pname})")


def seeded_pages(image: Image, pages: list[int], seed: int, weights) -> list:
    """Pages of .data/.bss with zero words replaced by seeded content (pointers to
    lazy memory, integers...), as if the singletons and globals were already initialized.
    Non-zero words (initialized data, vtables) are preserved."""
    out = []
    for pg in pages:
        orig = np.frombuffer(image.read(pg, PAGE), dtype="<u8").copy()
        gen = gen_words(_seed("gpage", seed, pg), PAGE // 8, weights)
        z = orig == 0
        orig[z] = gen[z]
        out.append((pg, orig.tobytes()))
    return out


GOT_RANGE = (0x7104270878, 0x7104289500)


def referenced_data_pages(image: Image, addr: int, size: int, limit: int = 16) -> list[int]:
    """Writable pages (.data/.bss) that the function references with adrp+add/ldr, directly or
    through a GOT slot (the slot points to the global). Static analysis of the ORIGINAL."""
    writable = [(lo, hi) for lo, hi, p in image.segments if "w" in p]

    def is_w(a):
        return any(lo <= a < hi for lo, hi in writable) and not (GOT_RANGE[0] <= a < GOT_RANGE[1])
    pages: dict[str, int] = {}
    found: list[int] = []

    def add(a):
        pg = a & ~(PAGE - 1)
        if is_w(a) and pg not in found:
            found.append(pg)
            if (a + 0x200) & ~(PAGE - 1) != pg and is_w(pg + PAGE):
                found.append(pg + PAGE)   # object that may cross the page
    for ins in _CS.disasm(image.read(addr, size), addr):
        m = ins.mnemonic
        try:
            if m == "adrp":
                pages[ins.op_str.split(",")[0].strip()] = ins.operands[1].imm
                continue
            ops = ins.op_str
            if m == "add" and len(ins.operands) == 3 and ins.operands[2].type == 2:
                rn = ops.split(",")[1].strip()
                if rn in pages:
                    add(pages[rn] + ins.operands[2].imm)
            elif "[" in ops:
                base = ops[ops.index("[") + 1:].split("]")[0].split(",")[0].strip()
                if base in pages:
                    disp = 0
                    for op in ins.operands:
                        if op.type == 3:
                            disp = op.mem.disp
                    a = pages[base] + disp
                    if GOT_RANGE[0] <= a < GOT_RANGE[1] and image.contains(a, 8):
                        add(image.u64(a))
                    else:
                        add(a)
        except (IndexError, ValueError):
            pass
        if len(found) >= limit:
            break
    return found[:limit]


def make_cases(func_addr: int, n: int, snapshots: list | None = None, ret_set: int = 0, sret: bool = False,
               global_pages: list | None = None, n_globals: int = 0, image: Image | None = None,
               follow: bool = False) -> list[Case]:
    """Test cases. The first `n` (and the snapshot ones) are always the same as before; the new
    options ADD cases after them (the existing ones do not change):
      ret_set   cases in which each intercepted call returns a value from a small set
                (0, 1, -1, pointer to seeded memory, x0, x1, ...) and the sret (x8) is filled
      n_globals cases with the `global_pages` (referenced .data/.bss) seeded
                (requires `image`); half of them also with ret_set
      sret/follow  copy of the basic cases with the sret filled and/or with --follow (and the extra
                cases also with --follow)"""
    cases = [_gen_case(func_addr, i) for i in range(n)]
    for j, snap in enumerate(snapshots or []):
        pname, w = PROFILES[0]
        s = _seed("snap", func_addr, j)
        regs = {int(k[1:]): int(v, 0) for k, v in snap.get("regs", {}).items() if k.startswith("x")}
        vregs = {int(k[1:]): int(v, 0) for k, v in snap.get("regs", {}).items() if k[0] in "vq"}
        mem = [(int(m["addr"], 0), bytes.fromhex(m["hex"])) for m in snap.get("mem", [])]
        cases.append(Case(n + j, s, "snapshot", w, regs, vregs, mem, name=f"snapshot {snap.get('_file', j)}"))
    nb = len(cases)
    if sret or follow:
        # copy of the basic cases (same inputs) with sret and/or --follow: the basic ones do not change
        for c0 in cases[:nb]:
            c = Case(len(cases), c0.seed, c0.profile, c0.weights, dict(c0.regs), dict(c0.vregs), list(c0.mem),
                     sret=sret, follow=follow)
            c.name = f"#{c.idx} (= {c0.name}{', sret' if sret else ''}{', follow' if follow else ''})"
            cases.append(c)
    k0 = len(cases)
    for i in range(ret_set):
        c = _gen_case(func_addr, i, "case-rset")
        c.idx = k0 + i
        c.ret_set = True
        c.sret = True
        c.follow = follow
        c.name = f"#{c.idx} ({c.profile}, returns)"
        cases.append(c)
    k0 = len(cases)
    if n_globals and global_pages and image is not None:
        for i in range(n_globals):
            c = _gen_case(func_addr, i, "case-glob")
            c.idx = k0 + i
            c.mem = seeded_pages(image, global_pages, c.seed, c.weights)
            c.ret_set = c.sret = bool(i % 2)
            c.sret = c.sret or sret
            c.follow = follow
            c.name = f"#{c.idx} ({c.profile}, seeded globals{', returns' if c.ret_set else ''})"
            cases.append(c)
    return cases


# ------------------------------------------------------------------ arity of targets
_CS = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
_CS.detail = True


def _canon(name: str):
    """'w3' -> ('x', 3, 32); 's2' -> ('v', 2, 32); 'q1' -> ('v', 1, 128)."""
    if not name:
        return None
    c, rest = name[0], name[1:]
    if not rest.isdigit():
        return None
    i = int(rest)
    if c in "xw" and i <= 8:
        return ("x", i, 64 if c == "x" else 32)
    if c in "bhsdqv" and i <= 7:
        return ("v", i, {"b": 8, "h": 16, "s": 32, "d": 64, "q": 128, "v": 128}[c])
    return None


class ArityOracle:
    """Which argument registers a target really reads (static analysis of the original)."""

    def __init__(self, image: Image, funcs):
        self.im = image
        self.funcs = funcs
        self.cache: dict[int, dict | None] = {}

    def sret_size(self, target: int) -> int | None:
        """If the target uses x8 as an indirect result pointer (reads it before writing it):
        (estimated) size of the object it writes through it; otherwise None."""
        if not hasattr(self, "_sret"):
            self._sret = {}
        if target in self._sret:
            return self._sret[target]
        spec = self.args_of(target)
        r = None
        if spec is not None and 8 in spec.get("x", {}) and self.im.is_code(target):
            r = self._sret_extent(target)
        self._sret[target] = r
        return r

    def _sret_extent(self, target: int, depth: int = 0) -> int | None:
        """Largest offset+width written through x8 (or copies of x8) in the target,
        walking its linear code (up to 400 instructions; follows tail calls with x8 intact).
        None if no write is seen (not filled: better not to invent the size). Maximum 256."""
        alias = {"x8"}
        tail = None
        ext = 0
        f = self.funcs.containing(target)
        end = f.end if f is not None and f.addr == target else target + 1600
        try:
            insns = _CS.disasm(self.im.read(target, min(end - target, 1600)), target)
            for k, ins in enumerate(insns):
                if k >= 400:
                    break
                m, ops = ins.mnemonic, ins.op_str
                parts = [p.strip() for p in ops.split(",")]
                if m == "mov" and len(parts) == 2 and parts[1] in alias:
                    alias.add(parts[0])
                    continue
                if m.startswith("st") and "[" in ops:
                    base = ops[ops.index("[") + 1:].split("]")[0].split(",")[0].strip()
                    if base in alias:
                        disp = 0
                        for op in ins.operands:
                            if op.type == 3:
                                disp = op.mem.disp
                        regs = [p for p in parts if p and not p.startswith("[") and p[0] in "xwqdsbhv"]
                        w = {"x": 8, "w": 4, "q": 16, "d": 8, "s": 4, "h": 2, "b": 1}.get(regs[0][0], 8) \
                            if regs else 8
                        if m in ("strb", "sturb"):
                            w = 1
                        elif m in ("strh", "sturh"):
                            w = 2
                        nreg = 2 if m.startswith("stp") or m.startswith("stnp") else 1
                        ext = max(ext, disp + w * nreg)
                try:
                    _, wr = ins.regs_access()
                    for r in wr:
                        nm = ins.reg_name(r)
                        if nm in alias and not (m == "mov" and parts and parts[0] == nm):
                            alias.discard(nm)
                except Exception:  # noqa: BLE001
                    pass
                if m in ("bl", "blr"):   # x8-x18 are not preserved across a call
                    alias = {r for r in alias if r[0] == "x" and r[1:].isdigit() and 19 <= int(r[1:]) <= 28}
                if m == "b" and "x8" in alias and ins.operands and ins.operands[0].type == 2:
                    t = ins.operands[0].imm
                    if f is None or not (f.addr <= t < f.end):
                        tail = t      # tail call: the target writes the result
                if not alias or m in ("ret", "br", "b"):
                    break
        except Exception:  # noqa: BLE001
            pass
        if tail is not None and depth < 2 and self.im.is_code(tail):
            sub = self._sret_extent(tail, depth + 1)
            if sub:
                ext = max(ext, sub)
        if ext <= 0:
            return None
        return min(max(ext, 8), 256)

    def args_of(self, target: int) -> dict | None:
        if target in self.cache:
            return self.cache[target]
        r = self._analyze(target, depth=0)
        self.cache[target] = r
        return r

    def _analyze(self, target: int, depth: int):
        im = self.im
        if target in im.plt or IMPORT_BASE <= target < IMPORT_BASE + IMPORT_SIZE:
            name = im.plt.get(target) or im.imports.get(target)
            return self._from_name(name)
        if not im.is_code(target):
            return None
        used: dict = {}
        seen = set()
        stack = [(target, frozenset(), depth)]
        budget = 600
        f0 = self.funcs.containing(target)
        while stack and budget > 0:
            pc, written, dep = stack.pop()
            written = set(written)
            while budget > 0:
                if (pc, dep) in seen:
                    break
                seen.add((pc, dep))
                budget -= 1
                try:
                    ins = next(_CS.disasm(im.read(pc, 4), pc))
                except (StopIteration, ValueError):
                    break
                try:
                    rd, wr = ins.regs_access()
                except Exception:
                    rd, wr = [], []
                for r in rd:
                    c = _canon(ins.reg_name(r))
                    if c and (c[0], c[1]) not in written:
                        k = (c[0], c[1])
                        used[k] = max(used.get(k, 0), c[2])
                for r in wr:
                    c = _canon(ins.reg_name(r))
                    if c:
                        written.add((c[0], c[1]))
                m = ins.mnemonic
                if ins.group(CS_GRP_RET) or m in ("br", "brk", "udf", "hlt"):
                    if m == "br":   # indirect jump: assume everything not written is passed
                        pass
                    break
                if ins.group(CS_GRP_CALL) or m in ("bl", "blr"):
                    for i in range(9):
                        written.add(("x", i))
                    for i in range(8):
                        written.add(("v", i))
                    pc += 4
                    continue
                if ins.group(CS_GRP_JUMP) or m.startswith("b.") or m in ("cbz", "cbnz", "tbz", "tbnz", "b"):
                    tgt = None
                    for op in ins.operands:
                        if op.type == 2:   # IMM
                            tgt = op.imm
                    if m == "b":
                        if tgt is None:
                            break
                        same = f0 is not None and f0.addr <= tgt < f0.end
                        if same:
                            pc = tgt
                            continue
                        if dep < 2:   # tail call: continue at the target
                            stack.append((tgt, frozenset(written), dep + 1))
                        break
                    if tgt is not None:
                        stack.append((tgt, frozenset(written), dep))
                    pc += 4
                    continue
                pc += 4
        return {"x": {i: used[("x", i)] for i in range(9) if ("x", i) in used},
                "v": {i: used[("v", i)] for i in range(8) if ("v", i) in used}}

    @staticmethod
    def _from_name(name: str | None):
        if not name:
            return None
        from demangle import demangle
        dem = demangle(name) if name.startswith("_Z") else None
        if not dem:
            # known C functions
            known = {"memcpy": 3, "memmove": 3, "memset": 3, "strlen": 1, "strcmp": 2, "strncmp": 3, "memcmp": 3,
                     "free": 1, "malloc": 1, "__cxa_guard_acquire": 1, "__cxa_guard_release": 1,
                     "__cxa_atexit": 3, "__cxa_pure_virtual": 0, "abort": 0}
            n = known.get(name)
            return None if n is None else {"x": {i: 64 for i in range(n)}, "v": {}}
        from srcindex import split_demangled
        kind, qn, npar = split_demangled(dem)
        if npar < 0:
            return None
        # methods: +1 for this (we do not know if it is static: assumed not)
        has_this = "::" in qn and not qn.startswith(("nn::os::", "nn::fs::")) or False
        n = npar + (1 if has_this else 0)
        ptypes = dem[dem.find("(") + 1: dem.rfind(")")]
        nf = ptypes.count("float") + ptypes.count("double")
        return {"x": {i: 64 for i in range(min(n - nf, 8))}, "v": {i: 64 for i in range(min(nf, 8))}}
