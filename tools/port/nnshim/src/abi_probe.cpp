// nnshim: generic caller probe (tool for v1.5 stage 5).
//
// `nnshim_probe` is a "pre" hook that can be placed on any guest function (gen_mod_json.py
// --probe): it changes nothing (PLA_CONTINUE), counts calls per function (counter
// probe.<module+offset>) and logs the first `probe.max_per_site` calls of each with their
// frame chain (X30 and X29), to find which SDK or game code issues each IPC request that
// still reaches the HLE (SUYU_IPC_STATS).
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "abi_common.hpp"

using namespace nnshim;

namespace {

int g_max_per_site = 3;
std::mutex g_mutex;
std::map<uint64_t, unsigned> g_seen;

std::string ModOff(uint64_t va) {
    char buf[64];
    uint64_t best = 0;
    const char* best_name = nullptr;
    for (const char* m : {"main", "sdk", "subsdk0", "subsdk1", "rtld"}) {
        const uint64_t b = g_api->module_base(m);
        if (b && va >= b && b > best) { best = b; best_name = m; }
    }
    if (best_name) std::snprintf(buf, sizeof buf, "%s+0x%llx", best_name, static_cast<unsigned long long>(va - best));
    else std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(va));
    return buf;
}

void InitProbe(const cfg::Value& c) {
    g_max_per_site = static_cast<int>(c.GetInt("probe.max_per_site", 3));
    std::lock_guard lk(g_mutex);
    g_seen.clear();
}

ModuleHooks g_probe_module{"probe", &InitProbe, nullptr};

} // namespace

NNSHIM_EXPORT pla_action nnshim_probe(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t pc = g_api->get_pc(ctx);
    const std::string site = ModOff(pc);
    CounterAdd(("probe." + site).c_str());
    unsigned n;
    {
        std::lock_guard lk(g_mutex);
        n = g_seen[pc]++;
    }
    if (static_cast<int>(n) >= g_max_per_site) return PLA_CONTINUE;
    std::string chain = ModOff(g.x(30));
    uint64_t fp = g.x(29);
    for (int i = 0; i < 6 && fp && (fp & 0xF) == 0; ++i) {
        uint64_t frame[2] = {};
        if (g.read(fp, frame, sizeof frame) != sizeof frame || !frame[1]) break;
        chain += " < " + ModOff(frame[1]);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    Log(PLA_LOG_INFO, "nnshim: probe %s #%u x0=%#llx x1=%#llx callers: %s", site.c_str(), n + 1,
        static_cast<unsigned long long>(g.x(0)), static_cast<unsigned long long>(g.x(1)), chain.c_str());
    Trace("probe %s %s", site.c_str(), chain.c_str());
    return PLA_CONTINUE;
}
