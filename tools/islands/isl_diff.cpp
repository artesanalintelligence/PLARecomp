// isl_diff: offline differential test of the native islands against the recompiled original (FPS4-ISLANDS).
//
// One process holds both versions: the shipped export's block functions for main+0x2A697AC (+ TRS helpers,
// + the skin palette loop region) and the native islands. For N random scenarios (skeleton objects with random
// hierarchies, dirty/ssc bits, TRS values including clamp-triggering tiny scales, subnormals, constraint joints
// of every type) it runs the original from a fresh memory image, snapshots every touched guest page outside the
// stack, rebuilds the same image, runs the island, snapshots again and compares byte by byte. A scenario where
// the original leaves the slice (constraint types that call libm-using helpers) must be one where the island
// bails, and is not compared.
//
//   isl_diff.exe <main.elf> [scenarios=20000] [seed=1] [-v]
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

extern "C" {
#include "recomp_runtime.h"
}

// ---------------------------------------------------------------------------------------------- guest memory
static const uint64_t MEMSZ = UINT64_C(1) << 39;
static uint8_t* g_mem;
static uint64_t kModBase = UINT64_C(0x1000000000);
static const char* g_elf_path;
static uint64_t g_mod_lo, g_mod_hi;  // module pages: committed once, never reset
static const uint64_t kStackTop = UINT64_C(0x3000000000), kStackSize = 0x100000;
static std::vector<uint64_t> g_pages;

static inline uint64_t sm(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static void Pattern(uint64_t va, uint8_t* p) {
    uint64_t* w = (uint64_t*)p;
    for (int i = 0; i < 512; i++) w[i] = sm(va + i * 8) & 0x3f3f3f3f3f3f3f3fULL;  // finite floats, small ints
}
static LONG CALLBACK Veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* a = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
    if (a < g_mem || a >= g_mem + MEMSZ + 65536) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* p = (uint8_t*)((uintptr_t)a & ~(uintptr_t)4095);
    if (!VirtualAlloc(p, 4096, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t va = (uint64_t)(p - g_mem);
    Pattern(va, p);
    g_pages.push_back(va);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void ResetMemory() {
    for (uint64_t va : g_pages) VirtualFree(g_mem + va, 4096, MEM_DECOMMIT);
    g_pages.clear();
}
static bool Excluded(uint64_t va) {
    return (va >= kStackTop - kStackSize && va < kStackTop) || (va >= g_mod_lo && va < g_mod_hi);
}
static std::map<uint64_t, uint64_t> Snapshot() {
    std::map<uint64_t, uint64_t> m;
    for (uint64_t va : g_pages) {
        if (Excluded(va)) continue;
        uint64_t h = 0x1234;
        const uint64_t* w = (const uint64_t*)(g_mem + va);
        for (int k = 0; k < 512; k++) h = sm(h ^ w[k]);
        m[va] = h;
    }
    return m;
}
static uint64_t PatternHash(uint64_t va) {
    alignas(16) uint8_t buf[4096];
    Pattern(va, buf);
    uint64_t h = 0x1234;
    const uint64_t* w = (const uint64_t*)buf;
    for (int k = 0; k < 512; k++) h = sm(h ^ w[k]);
    return h;
}
template <class T> static T& G(uint64_t va) { return *(T*)(g_mem + va); }

// ---------------------------------------------------------------------------------------------- recompiled side
extern "C" {
uint64_t g_module_base = UINT64_C(0x1000000000);
uintptr_t g_recomp_fm2_base;
uint32_t g_recomp_gg_word = 0;
int g_recomp_guard_host_v2 = 2;
BlockFn* g_recomp_chain_idx;
static uint64_t g_unhandled;
void recomp_unhandled(GuestContext* c, uint32_t insn, uint64_t pc) { c->pc = pc; c->halted = 2; ++g_unhandled; }
void recomp_svc(GuestContext* c, unsigned) { c->halted = 2; }
void recomp_code_guard_gen(GuestContext*, uint64_t, const uint32_t*, uint32_t, int, uint32_t*, uint32_t) {}
BlockFn recomp_xchain_fn(uint64_t) { return 0; }
uint64_t recomp_ldxr(GuestContext*, uint64_t, uint32_t) { return 0; }
uint32_t recomp_stxr(GuestContext*, uint64_t, uint32_t, uint64_t) { return 1; }
void recomp_ldxp(GuestContext*, uint64_t, uint32_t, uint64_t* lo, uint64_t* hi) { *lo = *hi = 0; }
uint32_t recomp_stxp(GuestContext*, uint64_t, uint32_t, uint64_t, uint64_t) { return 1; }
void recomp_clrex(GuestContext*) {}
void recomp_barrier(void) {}
uint64_t recomp_smulh(uint64_t a, uint64_t b) { return (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64); }
uint64_t recomp_umulh(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * (unsigned __int128)b) >> 64); }
#include "fpest.inc"
}
#include "blocks.inc"
static void StopFn(GuestContext*) {}
// sqrtf@plt (main+0x32A3090) -> the guest libm's `fsqrt s0, s0; ret`, with AArch64 semantics.
static void SqrtfBlock(GuestContext* c) {
    uint32_t b = (uint32_t)c->vreg[0][0], r;
    if ((c->fpcr >> 24) & 1 && !(b & 0x7f800000u) && (b & 0x007fffffu)) b &= 0x80000000u;
    if ((b & 0x7fffffffu) > 0x7f800000u) r = (c->fpcr >> 25) & 1 ? 0x7fc00000u : (b | 0x00400000u);
    else if ((b & 0x80000000u) && (b & 0x7fffffffu)) r = 0x7fc00000u;
    else { float f; memcpy(&f, &b, 4); f = sqrtf(f); memcpy(&r, &f, 4); }
    c->vreg[0][0] = r;
    c->vreg[0][1] = 0;
    c->pc = c->x[30];
}
static std::unordered_map<uint64_t, BlockFn> g_blk;

// Runs the original from pc until pc == stop. Returns 0 ok, 1 left the slice (pc), 2 halted.
static int RunOriginal(GuestContext* c, uint64_t stop, uint64_t* left) {
    for (int guard = 0; guard < 10000000; ++guard) {
        if (c->pc == stop) return 0;
        auto it = g_blk.find(c->pc - kModBase);
        if (it == g_blk.end()) { *left = c->pc - kModBase; return 1; }
        c->chain_budget = 1 << 30;
        it->second(c);
        if (c->halted) return 2;
    }
    return 2;
}

// ---------------------------------------------------------------------------------------------- native side
static std::vector<uint64_t> g_ntrace;
static bool g_tracing;
#define NL_TRACE(pc) do { if (g_tracing) g_ntrace.push_back(pc); } while (0)
uint64_t g_main = UINT64_C(0x1000000000);
thread_local uint32_t g_fpcr;
struct MMH {
    static inline uint8_t* P(uint64_t va) { return g_mem + (va & (MEMSZ - 1)); }
    static inline uint8_t* W(uint64_t va) { return P(va); }
};
#include "isl_core.h"
static int g_guard_mode;  // 1: refuse about a third of the joints (exercises the region's deopt path)
template <class MM> static bool skel_guard(Regs& R) { return g_guard_mode == 0 || (sm(R.x[1] * 977 + R.x[0]) % 3) != 0; }

// ---------------------------------------------------------------------------------------------- ELF
static void LoadModule(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> all(size);
    fread(all.data(), 1, size, f);
    fclose(f);
    const uint64_t phoff = *(uint64_t*)&all[0x20];
    const uint16_t phentsize = *(uint16_t*)&all[0x36], phnum = *(uint16_t*)&all[0x38];
    uint64_t hi = 0;
    for (int i = 0; i < phnum; ++i) {
        const uint8_t* ph = &all[phoff + (uint64_t)i * phentsize];
        if (*(uint32_t*)ph != 1) continue;
        const uint64_t off = *(uint64_t*)(ph + 8), va = *(uint64_t*)(ph + 16), fsz = *(uint64_t*)(ph + 32),
                       msz = *(uint64_t*)(ph + 40);
        VirtualAlloc(g_mem + kModBase + (va & ~4095ull), ((va & 4095) + msz + 4095) & ~4095ull, MEM_COMMIT, PAGE_READWRITE);
        memcpy(g_mem + kModBase + va, &all[off], fsz);
        hi = (std::max)(hi, va + msz);
    }
    // R_AARCH64_RELATIVE relocations
    const uint64_t shoff = *(uint64_t*)&all[0x28];
    const uint16_t shentsize = *(uint16_t*)&all[0x3a], shnum = *(uint16_t*)&all[0x3c];
    for (int i = 0; i < shnum; ++i) {
        const uint8_t* sh = &all[shoff + (uint64_t)i * shentsize];
        if (*(uint32_t*)(sh + 4) != 4) continue;  // SHT_RELA
        const uint64_t off = *(uint64_t*)(sh + 24), sz = *(uint64_t*)(sh + 32);
        for (uint64_t k = 0; k + 24 <= sz; k += 24) {
            const uint64_t r_off = *(uint64_t*)&all[off + k], info = *(uint64_t*)&all[off + k + 8];
            const int64_t add = *(int64_t*)&all[off + k + 16];
            if ((uint32_t)info == 1027) G<uint64_t>(kModBase + r_off) = kModBase + add;
        }
    }
    g_mod_lo = kModBase;
    g_mod_hi = kModBase + ((hi + 4095) & ~4095ull);
}

// ---------------------------------------------------------------------------------------------- scenarios
struct Rng {
    uint64_t s;
    uint64_t next() { return s = sm(s); }
    float uf(float a, float b) { return a + (b - a) * (float)((next() >> 40) * (1.0 / 16777216.0)); }
    int ui(int n) { return (int)(next() % (uint64_t)n); }
};
static float Val(Rng& r, int mode) {  // value distributions for TRS / matrices
    const int k = r.ui(100);
    if (mode == 1) {  // scale
        if (k < 70) return r.uf(0.5f, 2.0f);
        if (k < 78) return -r.uf(0.5f, 2.0f);
        if (k < 84) return r.uf(-1e-12f, 1e-12f) * 0.5f;   // below the 1e-12 clamp
        if (k < 88) return 0.0f;
        if (k < 92) return r.uf(1e-12f, 3e-12f);
        if (k < 96) { uint32_t b = (uint32_t)(r.next() & 0x007fffff) | 1; float f; memcpy(&f, &b, 4); return f; }  // subnormal
        return r.uf(-100.0f, 100.0f);
    }
    if (k < 90) return r.uf(-1.0f, 1.0f) * (mode == 2 ? 100.0f : 1.0f);
    if (k < 94) return 0.0f;
    if (k < 97) { uint32_t b = (uint32_t)(r.next() & 0x807fffff); float f; memcpy(&f, &b, 4); return f; }  // subnormal
    return r.uf(-1e-30f, 1e-30f);
}

struct Scenario {
    uint64_t obj, parent, dirty, ssc, local, world, con, cons, aux;
    int n, j;
    uint32_t fpcr;
};
static const uint64_t kObj = UINT64_C(0x2000000000);

static Scenario Build(uint64_t seed, int opts) {
    Rng r{seed};
    Scenario s{};
    s.n = 1 + r.ui(r.ui(4) == 0 ? 300 : 40);
    s.obj = kObj;
    s.parent = kObj + 0x10000;
    s.dirty = kObj + 0x20000;
    s.ssc = kObj + 0x21000;
    s.local = kObj + 0x30000;
    s.world = kObj + 0x50000;
    s.con = kObj + 0x70000;
    s.cons = kObj + 0x80000;
    s.aux = kObj + 0xA0000;
    G<uint64_t>(s.obj + 0x100) = s.parent;
    G<uint64_t>(s.obj + 0x138) = s.dirty;
    G<uint64_t>(s.obj + 0x178) = s.ssc;
    G<uint64_t>(s.obj + 0x270) = s.local;
    G<uint64_t>(s.obj + 0x278) = s.local + 0x30 * (uint64_t)s.n;
    G<uint64_t>(s.obj + 0x2a8) = s.world;
    G<uint64_t>(s.obj + 0x2b0) = s.world + 0x40 * (uint64_t)s.n;
    G<uint64_t>(s.obj + 0x2e0) = s.con;
    G<uint64_t>(s.obj + 0x318) = s.cons;
    G<uint64_t>(s.obj + 0x238) = s.aux;
    const bool any_order = !(opts & 1) && r.ui(4) == 0;
    int ncons = 0;
    for (int i = 0; i < s.n; ++i) {
        int p = -1;
        if (i > 0 && r.ui(10) != 0) p = any_order ? r.ui(s.n) : r.ui(i);
        if (any_order) {  // keep it a tree: no cycles (parent chain must reach a root)
            int q = p, steps = 0;
            while (q >= 0 && steps < 400) { if (q == i) { p = -1; break; } q = (int16_t)G<uint16_t>(s.parent + 2 * q); ++steps; }
            if (steps >= 400) p = -1;
        }
        G<uint16_t>(s.parent + 2 * i) = (uint16_t)(int16_t)p;
        int16_t ci = -1;
        if ((opts & 1) && i > 0 && r.ui(5) == 0) {  // one constraint per joint, targets before it (no cycles)
            ci = (int16_t)ncons++;
            float* f = &G<float>(s.cons + 0xd0 * ci);
            for (int k = 0; k < 0xd0 / 4; ++k) f[k] = Val(r, 0);
            uint8_t* b = &G<uint8_t>(s.cons + 0xd0 * ci);
            *(uint16_t*)(b + 0) = (uint16_t)r.ui(i);
            *(uint16_t*)(b + 2) = (uint16_t)r.ui(i);
            *(uint16_t*)(b + 4) = (uint16_t)r.ui(i);
            *(uint16_t*)(b + 0x50) = (uint16_t)r.ui(6);
            *(uint32_t*)(b + 0x70) = (uint32_t)r.ui(2);
        }
        G<uint16_t>(s.con + 2 * i) = (uint16_t)ci;
        float* l = &G<float>(s.local + 0x30 * i);
        for (int k = 0; k < 4; ++k) l[k] = Val(r, 1);
        for (int k = 4; k < 8; ++k) l[k] = Val(r, 0);
        for (int k = 8; k < 12; ++k) l[k] = Val(r, 2);
        float* w = &G<float>(s.world + 0x40 * i);
        for (int k = 0; k < 16; ++k) w[k] = Val(r, k >= 12 ? 2 : 0);
        float* a = &G<float>(s.aux + 0x30 * i);
        for (int k = 0; k < 12; ++k) a[k] = Val(r, 0);
    }
    for (int w = 0; w < (s.n + 31) / 32 + 1; ++w) {
        G<uint32_t>(s.dirty + 4 * w) = (uint32_t)r.next();
        G<uint32_t>(s.ssc + 4 * w) = (uint32_t)r.next() & (r.ui(2) ? 0xffffffffu : 0u);
    }
    s.j = r.ui(s.n);
    G<uint32_t>(s.dirty + 4 * (s.j / 32)) |= 1u << (s.j & 31);
    s.fpcr = r.ui(4) ? 0x1000000u : 0u;  // the game runs with FZ
    return s;
}

// Capture mode: rebuild the guest memory of one in-game record (islands-captures.bin) and run both versions
// with block traces; prints the dirty-bit/world comparison and the first divergence of the block sequences.
static int CaptureMode(const char* file, int want) {
    FILE* f = fopen(file, "rb");
    if (!f) { perror(file); return 2; }
    int rec = 0;
    while (true) {
        char magic[4];
        if (fread(magic, 1, 4, f) != 4) break;
        uint32_t kind; uint64_t h[4]; uint32_t jn[2];
        fread(&kind, 4, 1, f); fread(h, 8, 4, f); fread(jn, 4, 2, f);
        struct B { uint32_t tag; uint64_t va; std::vector<uint8_t> b; };
        std::vector<B> bl;
        for (int i = 0; i < 13; ++i) {
            B b; uint32_t sz;
            fread(&b.tag, 4, 1, f); fread(&b.va, 8, 1, f); fread(&sz, 4, 1, f);
            b.b.resize(sz); if (sz) fread(b.b.data(), 1, sz, f);
            bl.push_back(std::move(b));
        }
        ++rec;
        if (want ? rec != want : kind != 1) continue;
        if (h[0] != kModBase) printf("note: record main %#llx, harness module %#llx (main-relative pointers in the data would differ)\n",
                                     (unsigned long long)h[0], (unsigned long long)kModBase);
        if (h[0] != kModBase) {
            kModBase = g_module_base = g_main = h[0];
            LoadModule(g_elf_path);
        }
        auto load = [&]() {
            ResetMemory();
            for (auto& b : bl) {
                if (b.tag == 8 || b.tag == 9 || b.tag == 10 || b.tag == 11 || b.b.empty()) continue;
                memcpy(g_mem + b.va, b.b.data(), b.b.size());
            }
        };
        auto grab = [&](uint32_t tag) {
            for (auto& b : bl) if (b.tag == tag) return std::vector<uint8_t>(g_mem + b.va, g_mem + b.va + b.b.size());
            return std::vector<uint8_t>();
        };
        load();
        GuestContext c; memset(&c, 0, sizeof c);
        c.x[0] = h[1]; c.x[1] = jn[0]; c.x[31] = h[2]; c.x[30] = kModBase + 0x10; c.fpcr = h[3]; c.pc = kModBase + 0x2A697AC;
        std::vector<uint64_t> otrace;
        for (int g = 0; g < 1000000 && c.pc != kModBase + 0x10; ++g) {
            auto it = g_blk.find(c.pc - kModBase);
            if (it == g_blk.end()) { printf("original left the slice at %#llx\n", (unsigned long long)(c.pc - kModBase)); break; }
            otrace.push_back(c.pc - kModBase);
            c.chain_budget = 1;
            it->second(&c);
        }
        const auto dirty_o = grab(4), world_o = grab(7);
        load();
        Regs R; R.x[0] = h[1]; R.x[1] = jn[0]; R.sp = h[2];
        g_fpcr = (uint32_t)h[3];
        const unsigned old = _mm_getcsr();
        _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | ((h[3] >> 24) & 1 ? 0x8040u : 0));
        g_ntrace.clear(); g_tracing = true;
        const int rn = fn_2a697ac<MMH>(R);
        g_tracing = false;
        _mm_setcsr(old);
        const auto dirty_n = grab(4), world_n = grab(7);
        std::unordered_map<uint64_t, int> lab;
        for (uint64_t p : g_ntrace) lab[p] = 1;
        std::vector<uint64_t> of;
        for (uint64_t p : otrace) if (lab.count(p)) of.push_back(p);
        printf("record %d kind %u j %u: island rc %d, dirty %s, world %s; in-game original dirty %s\n", rec, kind, jn[0], rn,
               dirty_o == dirty_n ? "same" : "DIFFER", world_o == world_n ? "same" : "DIFFER",
               dirty_o == bl[8].b ? "== offline original" : "!= offline original");
        size_t k = 0;
        while (k < of.size() && k < g_ntrace.size() && of[k] == g_ntrace[k]) ++k;
        printf("  label sequences agree for %zu entries (original %zu, island %zu)\n", k, of.size(), g_ntrace.size());
        for (size_t q = (k > 6 ? k - 6 : 0); q < k + 4; ++q)
            printf("   %3zu  orig %#llx  isl %#llx\n", q, q < of.size() ? (unsigned long long)of[q] : 0ull,
                   q < g_ntrace.size() ? (unsigned long long)g_ntrace[q] : 0ull);
        if (want) break;
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: isl_diff <main.elf> [scenarios] [seed] [-v]\n"); return 2; }
    const int count = argc > 2 ? atoi(argv[2]) : 20000;
    const uint64_t seed0 = argc > 3 ? strtoull(argv[3], 0, 0) : 1;
    const bool verbose = argc > 4;
    g_mem = (uint8_t*)VirtualAlloc(0, MEMSZ + 65536, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_mem) { printf("reserve failed\n"); return 2; }
    g_recomp_fm2_base = (uintptr_t)g_mem;
    AddVectoredExceptionHandler(1, Veh);
    LoadModule(argv[1]);
    const size_t slots = (0x32a580cULL >> 2) + 1;
    g_recomp_chain_idx = (BlockFn*)malloc(slots * sizeof(BlockFn));
    for (size_t i = 0; i < slots; ++i) g_recomp_chain_idx[i] = StopFn;
    for (const auto& b : kBlocks) { g_recomp_chain_idx[b.pc >> 2] = b.fn; g_blk[b.pc] = b.fn; }
    g_blk[0x32A3090] = SqrtfBlock;
    g_elf_path = argv[1];
    if (argc > 3 && !strcmp(argv[2], "--capture")) return CaptureMode(argv[3], argc > 4 ? atoi(argv[4]) : 0);
    printf("blocks %zu, module %#llx-%#llx\n", sizeof kBlocks / sizeof kBlocks[0], (unsigned long long)g_mod_lo,
           (unsigned long long)g_mod_hi);
    long long pdiff_total = 0;
    long long tests = 0, same = 0, differ = 0, left_ok = 0, left_bad = 0, bail_only = 0, halted = 0;
    std::map<uint64_t, int> left_where;
    for (int t = 0; t < count; ++t) {
        const uint64_t seed = sm(seed0 * 1000003 + t);
        const int opts = (t % 3 == 2) ? 1 : 0;  // a third with constraint joints
        // ---- original
        ResetMemory();
        Scenario s = Build(seed, opts);
        GuestContext c;
        memset(&c, 0, sizeof c);
        c.x[0] = s.obj;
        c.x[1] = (uint64_t)s.j;
        c.x[31] = kStackTop - 0x1000;
        c.x[30] = kModBase + 0x10;
        c.fpcr = s.fpcr;
        c.pc = kModBase + 0x2A697AC;
        c.pending_svc = ~0ULL;
        uint64_t left = 0;
        const int ro = RunOriginal(&c, kModBase + 0x10, &left);
        const auto ma = Snapshot();
        // ---- island
        ResetMemory();
        Build(seed, opts);
        Regs R;
        R.x[0] = s.obj;
        R.x[1] = (uint64_t)s.j;
        R.sp = kStackTop - 0x1000;
        g_fpcr = s.fpcr;
        const unsigned old = _mm_getcsr();
        _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | ((s.fpcr >> 24) & 1 ? 0x8040u : 0));
        const int rn = fn_2a697ac<MMH>(R);
        _mm_setcsr(old);
        const auto mb = Snapshot();
        if (getenv("ISL_DIFF_SEED") && strtoull(getenv("ISL_DIFF_SEED"), 0, 0) == seed) {
            std::vector<uint8_t> wb(g_mem + s.world, g_mem + s.world + 64 * s.n);
            ResetMemory();
            Build(seed, opts);
            GuestContext c2;
            memset(&c2, 0, sizeof c2);
            c2.x[0] = s.obj; c2.x[1] = (uint64_t)s.j; c2.x[31] = kStackTop - 0x1000; c2.x[30] = kModBase + 0x10;
            c2.fpcr = s.fpcr; c2.pc = kModBase + 0x2A697AC;
            uint64_t l2 = 0;
            RunOriginal(&c2, kModBase + 0x10, &l2);
            for (int i = 0; i < 16 * s.n; ++i) {
                uint32_t a = G<uint32_t>(s.world + 4 * i), b; memcpy(&b, &wb[4 * i], 4);
                if (a != b) { float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); printf("  joint %d f%d: original %08x (%.9g) island %08x (%.9g)\n", i / 16, i % 16, a, fa, b, fb); }
            }
        }
        ++tests;
        if (ro == 2) { ++halted; continue; }
        if (ro == 1) {
            left_where[left]++;
            if (rn == NL_BAIL) ++left_ok;
            else { ++left_bad; if (left_bad <= 10) printf("LEFT-BUT-NO-BAIL seed %llu at %#llx\n", (unsigned long long)seed, (unsigned long long)left); }
            continue;
        }
        if (rn != NL_OK) { ++bail_only; continue; }
        bool eq = true;
        std::vector<uint64_t> pages;
        for (auto& [va, h] : ma) pages.push_back(va);
        for (auto& [va, h] : mb) pages.push_back(va);
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        uint64_t bad_va = 0;
        for (uint64_t va : pages) {
            auto ia = ma.find(va), ib = mb.find(va);
            const uint64_t ha = ia != ma.end() ? ia->second : PatternHash(va);
            const uint64_t hb = ib != mb.end() ? ib->second : PatternHash(va);
            if (ha != hb) { eq = false; bad_va = va; break; }
        }
        if (eq) ++same;
        else {
            ++differ;
            if (differ <= 20 || verbose)
                printf("DIFF seed %llu n %d j %d fpcr %x opts %d: page %#llx\n", (unsigned long long)seed, s.n, s.j,
                       s.fpcr, opts, (unsigned long long)bad_va);
        }
    }
    // ---- timing: original vs island on the same scenarios (world/dirty restored before every repetition)
    {
        double to = 0, tn = 0;
        long long nb = 0;
        for (int t = 0; t < 400; ++t) {
            const uint64_t seed = sm(seed0 * 31 + t);
            ResetMemory();
            Scenario s = Build(seed, 0);
            std::vector<uint8_t> w0(g_mem + s.world, g_mem + s.world + 64 * s.n), d0(g_mem + s.dirty, g_mem + s.dirty + 4 * (s.n / 32 + 1));
            auto restore = [&] { memcpy(g_mem + s.world, w0.data(), w0.size()); memcpy(g_mem + s.dirty, d0.data(), d0.size()); };
            const unsigned old = _mm_getcsr();
            _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | ((s.fpcr >> 24) & 1 ? 0x8040u : 0));
            g_fpcr = s.fpcr;
            for (int rep = 0; rep < 50; ++rep) {
                restore();
                GuestContext c;
                memset(&c, 0, sizeof c);
                c.x[0] = s.obj; c.x[1] = (uint64_t)s.j; c.x[31] = kStackTop - 0x1000; c.x[30] = kModBase + 0x10;
                c.fpcr = s.fpcr; c.pc = kModBase + 0x2A697AC;
                uint64_t left = 0;
                uint64_t t0 = __rdtsc();
                RunOriginal(&c, kModBase + 0x10, &left);
                to += (double)(__rdtsc() - t0);
                restore();
                Regs R;
                R.x[0] = s.obj; R.x[1] = (uint64_t)s.j; R.sp = kStackTop - 0x1000;
                t0 = __rdtsc();
                fn_2a697ac<MMH>(R);
                tn += (double)(__rdtsc() - t0);
                ++nb;
            }
            _mm_setcsr(old);
        }
        printf("timing (skeleton, %lld calls, warm): original %.0f cycles/call, island %.0f cycles/call (%.1fx)\n", nb,
               to / nb, tn / nb, to / tn);
    }
    // ---- update-all loop region: original 0x2A6BAF4.. until 0x2A6BB60 vs the native region (whole skeleton)
    long long lt = 0, lsame = 0, ldiff = 0, lleft = 0, ldeopt = 0;
    g_guard_mode = 1;
    for (int t = 0; t < count / 4; ++t) {
        const uint64_t seed = sm(seed0 * 104729 + t);
        const int opts = (t % 3 == 2) ? 1 : 0;
        auto setup = [&](GuestContext& c, Scenario& s) {
            memset(&c, 0, sizeof c);
            Rng r{seed ^ 0x55};
            for (int i = 0; i < 31; ++i) c.x[i] = r.next();
            c.x[20] = s.obj;
            c.x[31] = kStackTop - 0x1000;
            c.fpcr = s.fpcr;
            c.pc = kModBase + 0x2A6BAF4;
            c.pending_svc = ~0ULL;
        };
        ResetMemory();
        Scenario s = Build(seed, opts);
        GuestContext ca;
        setup(ca, s);
        uint64_t left = 0;
        const int ro = RunOriginal(&ca, kModBase + 0x2A6BB60, &left);
        const auto ma = Snapshot();
        ResetMemory();
        Build(seed, opts);
        GuestContext cb;
        setup(cb, s);
        g_fpcr = s.fpcr;
        const unsigned old = _mm_getcsr();
        _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | ((s.fpcr >> 24) & 1 ? 0x8040u : 0));
        const int rn = fn_2a6baf4<MMH>((GC*)&cb);
        _mm_setcsr(old);
        const auto mb = Snapshot();
        ++lt;
        if (ro != 0) { ++lleft; continue; }
        if (cb.pc != kModBase + 0x2A6BB60) {  // deopt: the original continues from the island's exit state
            uint64_t left2 = 0;
            cb.chain_budget = 1;
            if (RunOriginal(&cb, kModBase + 0x2A6BB60, &left2) != 0) { ++lleft; continue; }
            ++ldeopt;
        }
        const auto mb2 = Snapshot();
        // registers the loop leaves: x8/x9 scratch and the callee-clobbered ones differ by design (the original's
        // calls clobber caller-saved registers); compare memory and the registers the continuation reads.
        const bool eq = rn == NL_EXIT && ma == mb2 && ca.x[20] == cb.x[20] && ca.x[22] == cb.x[22] && ca.x[19] == cb.x[19];
        if (eq) ++lsame;
        else if (++ldiff <= 10) printf("LOOP DIFF seed %llu n %d\n", (unsigned long long)seed, s.n);
    }
    printf("update-all loop region: %lld runs, identical %lld, DIFFERENT %lld, left the slice (not compared) %lld, compared after a deopt %lld\n", lt, lsame, ldiff, lleft, ldeopt);
    g_guard_mode = 0;
    // ---- generic leaf-function regions: random register state (x0-x7 point into a heap area), random floats
    struct GenRegion { uint64_t entry; int (*fn)(GC*); const char* name; };
    const GenRegion gens[] = {{0x2A3781C, fn_2a3781c<MMH>, "mtx_decompose"}};
    for (const GenRegion& gr : gens) {
        long long gt = 0, gsame = 0, gdiff = 0;
        for (int t = 0; t < count / 4; ++t) {
            const uint64_t seed = sm(seed0 * 15485863 + t + gr.entry);
            auto build = [&](GuestContext& c) {
                Rng r{seed};
                memset(&c, 0, sizeof c);
                for (int i = 0; i < 31; ++i) c.x[i] = i < 8 ? kObj + ((r.next() % 0x40000) & ~15ull) : r.next();
                for (int i = 0; i < 32; ++i) {
                    float f[4];
                    for (int k = 0; k < 4; ++k) f[k] = Val(r, k == 3 ? 2 : 0);
                    memcpy(c.vreg[i], f, 16);
                }
                for (int i = 0; i < 64; ++i) {  // matrices-like data around the argument pointers
                    float* p = &G<float>(c.x[i % 8] + 4 * (i / 8));
                    *p = Val(r, (i & 3) == 3 ? 2 : (r.ui(3) ? 0 : 1));
                }
                c.x[31] = kStackTop - 0x1000;
                c.x[30] = kModBase + 0x10;
                c.fpcr = r.ui(4) ? 0x1000000u : 0u;
                c.pc = kModBase + gr.entry;
                c.pending_svc = ~0ULL;
            };
            ResetMemory();
            GuestContext ca;
            build(ca);
            uint64_t left = 0;
            const int ro = RunOriginal(&ca, kModBase + 0x10, &left);
            const auto ma = Snapshot();
            ResetMemory();
            GuestContext cb;
            build(cb);
            g_fpcr = (uint32_t)cb.fpcr;
            const unsigned old = _mm_getcsr();
            _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | ((cb.fpcr >> 24) & 1 ? 0x8040u : 0));
            const int rn = gr.fn((GC*)&cb);
            _mm_setcsr(old);
            const auto mb = Snapshot();
            ++gt;
            const bool eq = ro == 0 && rn == NL_EXIT && cb.pc == ca.pc && ma == mb && !memcmp(ca.x, cb.x, sizeof ca.x) &&
                            !memcmp(ca.vreg, cb.vreg, sizeof ca.vreg);
            if (eq) ++gsame;
            else if (++gdiff <= 10) {
                printf("%s DIFF seed %llu ro %d rn %d mem %d\n", gr.name, (unsigned long long)seed, ro, rn, (int)(ma == mb));
                for (int i = 0; i < 32; ++i) if (ca.x[i] != cb.x[i]) printf("  x%d %llx vs %llx\n", i, ca.x[i], cb.x[i]);
                for (int i = 0; i < 32; ++i) if (memcmp(ca.vreg[i], cb.vreg[i], 16)) printf("  v%d %016llx%016llx vs %016llx%016llx\n", i, ca.vreg[i][1], ca.vreg[i][0], cb.vreg[i][1], cb.vreg[i][0]);
            }
        }
        printf("%s region: %lld runs, identical %lld, DIFFERENT %lld\n", gr.name, gt, gsame, gdiff);
        pdiff_total += gdiff;
    }
    // ---- skin palette region: original blocks 0x29C98BC.. until the epilogue vs the native region
    long long pt = 0, psame = 0, pdiff = 0;
    for (int t = 0; t < count / 4; ++t) {
        const uint64_t seed = sm(seed0 * 7919 + t);
        GuestContext c0;
        auto build = [&](GuestContext& c) {
            Rng r{seed};
            memset(&c, 0, sizeof c);
            const int n = r.ui(r.ui(4) ? 120 : 400);
            const uint64_t out = kObj, idx = kObj + 0x100000, A = kObj + 0x200000, B = kObj + 0x300000;
            for (int i = 0; i < n; ++i) {
                G<uint16_t>(idx + 2 * i) = (uint16_t)(int16_t)(r.ui(6) ? r.ui(n + 1) : -1 - r.ui(3));
                for (int k = 0; k < 16; ++k) { G<float>(A + 64 * i + 4 * k) = Val(r, 0); G<float>(B + 64 * i + 4 * k) = Val(r, k >= 12 ? 2 : 0); }
            }
            for (int i = 0; i < 32; ++i) { c.x[i] = r.next(); c.vreg[i][0] = r.next(); c.vreg[i][1] = r.next(); }
            c.x[0] = out; c.x[19] = idx; c.x[20] = B; c.x[22] = A; c.x[21] = (c.x[21] & ~0xffffull) | (uint64_t)n;
            c.x[31] = kStackTop - 0x1000;
            c.fpcr = r.ui(4) ? 0x1000000u : 0u;
            c.pc = kModBase + 0x29C98BC;
            c.pending_svc = ~0ULL;
            c.n = c.z = c.c = c.v = 0;
        };
        ResetMemory();
        GuestContext ca;
        build(ca);
        c0 = ca;
        uint64_t left = 0;
        const int ro = RunOriginal(&ca, kModBase + 0x29C995C, &left);
        const auto ma = Snapshot();
        ResetMemory();
        GuestContext cb;
        build(cb);
        g_fpcr = (uint32_t)cb.fpcr;
        const unsigned old = _mm_getcsr();
        _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | ((cb.fpcr >> 24) & 1 ? 0x8040u : 0));
        const int rn = fn_29c98bc<MMH>((GC*)&cb);
        _mm_setcsr(old);
        const auto mb = Snapshot();
        ++pt;
        bool eq = ro == 0 && rn == NL_EXIT && ma == mb && cb.pc == ca.pc && !memcmp(ca.x, cb.x, sizeof ca.x) &&
                  !memcmp(ca.vreg, cb.vreg, sizeof ca.vreg);
        // flags: the epilogue does not read them, the region writes what it computed
        if (eq) ++psame;
        else {
            ++pdiff;
            if (pdiff <= 10) {
                printf("PALETTE DIFF seed %llu ro %d rn %d pc %llx/%llx mem %d\n", (unsigned long long)seed, ro, rn,
                       (unsigned long long)ca.pc, (unsigned long long)cb.pc, (int)(ma == mb));
                for (int i = 0; i < 32; ++i) if (ca.x[i] != cb.x[i]) printf("  x%d %llx vs %llx\n", i, ca.x[i], cb.x[i]);
                for (int i = 0; i < 32; ++i) if (memcmp(ca.vreg[i], cb.vreg[i], 16)) printf("  v%d differs\n", i);
            }
        }
    }
    printf("palette region: %lld runs, identical %lld, DIFFERENT %lld\n", pt, psame, pdiff);
    printf("scenarios %lld: identical %lld, DIFFERENT %lld | original left the slice %lld (island bailed %lld, "
           "did not %lld) | island-only bail %lld | original halted %lld\n",
           tests, same, differ, left_ok + left_bad, left_ok, left_bad, bail_only, halted);
    for (auto& [pc, n] : left_where) printf("  left at main+%#llx: %d\n", (unsigned long long)pc, n);
    return (differ || left_bad || pdiff || ldiff || pdiff_total) ? 1 : 0;
}
