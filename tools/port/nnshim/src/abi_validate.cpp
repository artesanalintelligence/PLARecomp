// nnshim: records the A/B service comparisons (see validate.hpp).
#include "validate.hpp"

#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>

#include "nnshim/runtime.hpp"
#include "abi_common.hpp"
#include "pla_mod_api.h"

namespace nnshim {


namespace {
std::mutex g_mutex;
std::map<std::string, unsigned> g_logged;   // differences already logged, per function
constexpr unsigned kLogPerName = 3;
constexpr unsigned kTracePerName = 50;   // per-frame functions (hid): don't flood the trace
} // namespace

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

void ValidateResult(const char* name, bool equal, const std::string& hle, const std::string& shim) {
    const std::string key = std::string("val.") + name + (equal ? ".ok" : ".diff");
    CounterAdd(key.c_str());
    CounterAdd(equal ? "val.ok" : "val.diff");
    if (equal) return;
    unsigned n;
    {
        std::lock_guard lk(g_mutex);
        n = g_logged[name]++;
    }
    if (n < kTracePerName) Trace("val %s diff hle=%s shim=%s", name, hle.c_str(), shim.c_str());
    if (n < kLogPerName) Log(PLA_LOG_WARNING, "nnshim: A/B %s differs: hle=%s shim=%s", name, hle.c_str(), shim.c_str());
}

} // namespace nnshim
