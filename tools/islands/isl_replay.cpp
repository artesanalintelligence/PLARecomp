// isl_replay: offline replay harness for the skeleton island (FPS4-ISLANDS).
//
// Reads islands-captures.bin records written by the islands mod in verify mode (input memory of one top-level
// call of main+0x2A697AC + the world matrices / dirty bits the recompiled original produced in the game) and
// re-runs the native island on a reconstructed guest memory: the captured blobs, a scratch stack below the
// recorded SP and the main module's segments from main.elf at the recorded module base. Compares the island's
// output byte by byte with the original's and reports per-record results and native cycles per call.
//
//   isl_replay.exe <main.elf> <captures.bin> [more captures...] [-v] [-bench N]
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

uint64_t g_main;
thread_local uint32_t g_fpcr;

struct Range {
    uint64_t lo, hi;
    uint8_t* p;
};
static std::vector<Range> g_ranges;
static bool g_fault;
static uint8_t g_dummy[64];

struct MMReplay {
    static inline uint8_t* P(uint64_t va) {
        for (const Range& r : g_ranges)
            if (va >= r.lo && va < r.hi) return r.p + (va - r.lo);
        if (!g_fault) fprintf(stderr, "  unmapped guest read/write at %#llx\n", (unsigned long long)va);
        g_fault = true;
        return g_dummy;
    }
    static inline uint8_t* W(uint64_t va) { return P(va); }
};

#include "isl_core.h"
template <class MM> static bool skel_guard(Regs&) { return true; }

struct Blob {
    uint32_t tag;
    uint64_t va;
    std::vector<uint8_t> b;
};
struct Record {
    uint32_t kind;
    uint64_t main, obj, sp, fpcr;
    uint32_t j, n;
    std::map<uint32_t, Blob> blobs;
};

static bool ReadRecord(FILE* f, Record& r) {
    char magic[4];
    if (fread(magic, 1, 4, f) != 4) return false;
    if (memcmp(magic, "ISL1", 4)) { fprintf(stderr, "bad magic\n"); return false; }
    fread(&r.kind, 4, 1, f);
    uint64_t h[4];
    fread(h, 8, 4, f);
    r.main = h[0]; r.obj = h[1]; r.sp = h[2]; r.fpcr = h[3];
    uint32_t jn[2];
    fread(jn, 4, 2, f);
    r.j = jn[0]; r.n = jn[1];
    r.blobs.clear();
    for (int i = 0; i < 13; ++i) {
        Blob b;
        uint32_t sz;
        if (fread(&b.tag, 4, 1, f) != 1) return false;
        fread(&b.va, 8, 1, f);
        fread(&sz, 4, 1, f);
        b.b.resize(sz);
        if (sz && fread(b.b.data(), 1, sz, f) != sz) return false;
        r.blobs[b.tag] = std::move(b);
    }
    return true;
}

// main.elf PT_LOAD segments
struct Seg {
    uint64_t va;
    std::vector<uint8_t> data;
};
static std::vector<Seg> LoadElf(const char* path) {
    std::vector<Seg> segs;
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
    for (int i = 0; i < phnum; ++i) {
        const uint8_t* ph = &all[phoff + (uint64_t)i * phentsize];
        if (*(uint32_t*)ph != 1) continue;  // PT_LOAD
        const uint64_t off = *(uint64_t*)(ph + 8), va = *(uint64_t*)(ph + 16), fsz = *(uint64_t*)(ph + 32),
                       msz = *(uint64_t*)(ph + 40);
        Seg s;
        s.va = va;
        s.data.assign(msz, 0);
        memcpy(s.data.data(), &all[off], fsz);
        segs.push_back(std::move(s));
    }
    return segs;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: isl_replay <main.elf> <captures.bin>... [-v] [-bench N]\n"); return 2; }
    bool verbose = false;
    int bench = 0;
    std::vector<const char*> files;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "-bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else files.push_back(argv[i]);
    }
    std::vector<Seg> segs = LoadElf(argv[1]);
    std::vector<uint8_t> stack(0x40000);
    uint64_t total = 0, ok = 0, mism = 0, bail = 0, fault = 0, samples = 0, mismatch_recs = 0, isl_vs_isl = 0;
    double cyc = 0;
    uint64_t cyc_n = 0;
    for (const char* fn : files) {
        FILE* f = fopen(fn, "rb");
        if (!f) { perror(fn); continue; }
        Record r;
        while (ReadRecord(f, r)) {
            ++total;
            (r.kind ? mismatch_recs : samples)++;
            g_main = r.main;
            // Memory: blobs (copies), stack, main segments.
            std::vector<std::vector<uint8_t>> keep;
            g_ranges.clear();
            auto add = [&](uint64_t va, std::vector<uint8_t> b) {
                keep.push_back(std::move(b));
                g_ranges.push_back({va, va + keep.back().size(), keep.back().data()});
            };
            // outputs start from the pre-images
            add(r.blobs[7].va, r.blobs[7].b);  // world (pre)
            add(r.blobs[4].va, r.blobs[4].b);  // dirty (pre)
            for (uint32_t t : {1u, 2u, 3u, 5u, 6u, 12u, 13u}) if (r.blobs[t].b.size()) add(r.blobs[t].va, r.blobs[t].b);
            add(r.sp - stack.size(), std::vector<uint8_t>(stack.size()));
            for (Seg& s : segs) g_ranges.push_back({r.main + s.va, r.main + s.va + s.data.size(), s.data.data()});
            Regs R;
            R.x[0] = r.obj;
            R.x[1] = r.j & 0xffff;
            R.sp = r.sp;
            g_fault = false;
            const unsigned old = _mm_getcsr();
            _mm_setcsr(((old & ~0x6000u) | 0x1F80u) | (((r.fpcr >> 24) & 1) ? 0x8040u : 0));
            g_fpcr = (uint32_t)r.fpcr;
            const int rc = fn_2a697ac<MMReplay>(R);
            _mm_setcsr(old);
            if (g_fault) { ++fault; continue; }
            if (rc != NL_OK) { ++bail; continue; }
            const bool same = keep[0] == r.blobs[8].b && keep[1] == r.blobs[9].b;
            const bool same_isl = keep[0] == r.blobs[10].b && keep[1] == r.blobs[11].b;
            isl_vs_isl += same_isl;
            if (same) ++ok;
            else {
                ++mism;
                if (verbose || mism <= 10) {
                    size_t at = 0;
                    const auto& o = r.blobs[8].b;
                    while (at < o.size() && o[at] == keep[0][at]) ++at;
                    printf("MISMATCH rec %llu (kind %u) obj %#llx joint %u n %u: world byte %zu (joint %zu, float %zu)",
                           (unsigned long long)total, r.kind, (unsigned long long)r.obj, r.j, r.n, at, at / 64,
                           (at % 64) / 4);
                    if (at < o.size()) {
                        float a, b;
                        memcpy(&a, &o[at & ~3], 4);
                        memcpy(&b, &keep[0][at & ~3], 4);
                        printf(" original %.9g island %.9g", a, b);
                    }
                    printf("\n");
                }
            }
            if (bench > 0) {  // time the island on this record (outputs re-armed each time)
                for (int k = 0; k < bench; ++k) {
                    keep[0] = r.blobs[7].b;
                    keep[1] = r.blobs[4].b;
                    g_ranges[0].p = keep[0].data();
                    g_ranges[1].p = keep[1].data();
                    const uint64_t t0 = __rdtsc();
                    fn_2a697ac<MMReplay>(R);
                    cyc += (double)(__rdtsc() - t0);
                    ++cyc_n;
                }
            }
        }
        fclose(f);
    }
    printf("records %llu (samples %llu, in-game mismatches %llu): island == original %llu, differ %llu, bail %llu, "
           "unmapped %llu; island == in-game island %llu\n",
           (unsigned long long)total, (unsigned long long)samples, (unsigned long long)mismatch_recs,
           (unsigned long long)ok, (unsigned long long)mism, (unsigned long long)bail, (unsigned long long)fault,
           (unsigned long long)isl_vs_isl);
    if (cyc_n) printf("replay timing (slow replay memory model): %.0f cycles per call\n", cyc / cyc_n);
    return mism ? 1 : 0;
}
