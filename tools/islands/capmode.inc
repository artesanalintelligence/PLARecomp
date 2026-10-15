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
