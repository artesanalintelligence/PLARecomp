// nnshim: U1 diagnostics (sprint 4): looks for per-frame counters in battle sequences (.wbin).
//
// Changes nothing (PLA_CONTINUE). Two "pre" hooks:
//   nnshim_diag_SeqCtor  at main+0x9db43c (ctor of the 0x330-byte sequence object, X0 = object);
//   nnshim_diag_Frame    at main+0x282841c (dt distribution, once per frame from the main loop).
// Each frame reads the live objects (up to 40 s after creation) and counts, per 4-byte offset, how many
// times the integer rose by exactly 1 and how many the float rose by ~1.0 (frame counter) or ~dt*30 (real clock).
// When an object is retired it logs the offsets with the most increments.
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "abi_common.hpp"

using namespace nnshim;

namespace {

constexpr size_t kSize = 0x330;
constexpr size_t kWords = kSize / 4;
constexpr uint64_t kFrameNs = 0x4350F08;

struct Tracked {
    uint64_t va = 0;
    uint64_t born_ms = 0;
    unsigned frames = 0;
    bool have_prev = false;
    uint8_t prev[kSize] = {};
    unsigned inc_i[kWords] = {};
    unsigned inc_f1[kWords] = {};
    unsigned inc_fdt[kWords] = {};
};

std::mutex g_mu;
std::vector<Tracked> g_objs;
uint64_t g_main = 0;
unsigned g_seq = 0;

void Report(const Tracked& t) {
    std::string s;
    char buf[96];
    for (size_t w = 0; w < kWords; ++w) {
        const unsigned a = t.inc_i[w], b = t.inc_f1[w], c = t.inc_fdt[w];
        if (a * 4 >= t.frames && a >= 10) { std::snprintf(buf, sizeof buf, " +0x%zx:int%u", w * 4, a); s += buf; }
        if (b * 4 >= t.frames && b >= 10) { std::snprintf(buf, sizeof buf, " +0x%zx:f1=%u", w * 4, b); s += buf; }
        if (c * 4 >= t.frames && c >= 10) { std::snprintf(buf, sizeof buf, " +0x%zx:fdt=%u", w * 4, c); s += buf; }
    }
    Log(PLA_LOG_INFO, "nnshim diag seq: obj %#llx born %llu ms, %u frames:%s",
        static_cast<unsigned long long>(t.va), static_cast<unsigned long long>(t.born_ms), t.frames,
        s.empty() ? " (none)" : s.c_str());
}

} // namespace

NNSHIM_EXPORT pla_action nnshim_diag_SeqCtor(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t va = g.x(0);
    std::lock_guard lk(g_mu);
    for (auto it = g_objs.begin(); it != g_objs.end(); ++it)
        if (it->va == va) { Report(*it); g_objs.erase(it); break; }
    if (g_objs.size() >= 32) { Report(g_objs.front()); g_objs.erase(g_objs.begin()); }
    Tracked t;
    t.va = va;
    t.born_ms = UptimeMs();
    g_objs.push_back(t);
    ++g_seq;
    Log(PLA_LOG_INFO, "nnshim diag seq: new obj %#llx (#%u) at %llu ms, lr=%#llx", static_cast<unsigned long long>(va),
        g_seq, static_cast<unsigned long long>(t.born_ms), static_cast<unsigned long long>(g.x(30)));
    return PLA_CONTINUE;
}

NNSHIM_EXPORT pla_action nnshim_diag_Frame(pla_ctx* ctx) {
    PlaGuest g(ctx);
    if (!g_main) g_main = g_api->module_base("main");
    uint64_t ns = 0;
    g.read(g_main + kFrameNs, &ns, sizeof ns);
    const float dt30 = static_cast<float>(ns) * 30e-9f;
    const uint64_t now = UptimeMs();
    std::lock_guard lk(g_mu);
    for (auto it = g_objs.begin(); it != g_objs.end();) {
        Tracked& t = *it;
        if (now - t.born_ms > 40000) { Report(t); it = g_objs.erase(it); continue; }
        uint8_t cur[kSize];
        if (g.read(t.va, cur, kSize) != kSize) { ++it; continue; }
        if (t.have_prev) {
            ++t.frames;
            for (size_t w = 0; w < kWords; ++w) {
                int32_t a, b;
                std::memcpy(&a, t.prev + w * 4, 4);
                std::memcpy(&b, cur + w * 4, 4);
                if (b - a == 1) ++t.inc_i[w];
                float fa, fb;
                std::memcpy(&fa, &a, 4);
                std::memcpy(&fb, &b, 4);
                const float d = fb - fa;
                if (std::isfinite(d) && std::fabs(fa) < 1e6f) {
                    if (std::fabs(d - 1.0f) < 0.01f) ++t.inc_f1[w];
                    else if (dt30 > 0.05f && std::fabs(d - dt30) < 0.02f) ++t.inc_fdt[w];
                }
            }
        }
        std::memcpy(t.prev, cur, kSize);
        t.have_prev = true;
        ++it;
    }
    return PLA_CONTINUE;
}
