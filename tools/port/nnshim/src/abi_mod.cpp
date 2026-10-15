// nnshim: mod lifecycle (pla_mod_api.h) and common adapter utilities.
//
// AAPCS64 adapters live in one file per family (abi_hid.cpp, abi_fs.cpp,
// abi_time.cpp, abi_applet.cpp, abi_err.cpp, abi_swkbd.cpp, audio_*.cpp). Each adapter is a
// `pre` hook (recomp/MODDING.md): it reads arguments from
// X0-X7, calls the native core, writes results into guest memory and returns
//   PLA_SKIP      the shim handled the call (X0 = nn::Result or return value);
//   PLA_CONTINUE  the resource is not the shim's (foreign mount or handle) or the family is
//                 off/observing: the original SDK function runs, which in v1
//                 talks to suyu's HLE.
// Adapters neither block nor call guest code (they cannot: see MODDING.md).
//
// Each file registers its initialization with a static nnshim::ModuleHooks; here they are
// only iterated (no need to touch this file to add a family).
#include <cstdarg>
#include <cstdio>
#include <string>

#include "abi_common.hpp"
#include "nnshim/config.hpp"
#include "nnshim/hid.hpp"
#include "nnshim/runtime.hpp"
#include "pla_mod_api.h"

namespace nnshim {

const pla_api* g_api = nullptr;

void Log(int level, const char* fmt, ...) {
    if (!g_api) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_api->log(level, line);
}

std::string Utf16ToUtf8(const std::u16string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) o.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return o;
}

std::u16string Utf8ToUtf16(const std::string& s) {
    std::u16string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { ++i; continue; }
        if (i + static_cast<size_t>(n) > s.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        i += static_cast<size_t>(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            o.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            o.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            o.push_back(static_cast<char16_t>(cp));
        }
    }
    return o;
}

bool LoadU16String(Guest& g, uint64_t va, std::u16string& out, size_t max_chars) {
    out.clear();
    if (!va) return false;
    char16_t buf[32];
    while (out.size() < max_chars) {
        const size_t got = g.read(va + out.size() * 2, buf, sizeof buf) / 2;
        if (!got) return false;
        for (size_t i = 0; i < got && out.size() < max_chars; ++i) {
            if (!buf[i]) return true;
            out.push_back(buf[i]);
        }
    }
    return true;
}

} // namespace nnshim

using namespace nnshim;

// =========================== mod: lifecycle ===============================================
NNSHIM_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

NNSHIM_EXPORT int pla_mod_init(const pla_api* api, const char* config_json) {
    // API 1.0 (up to `log`) is enough; 1.1 extension members are queried with PLA_API_HAS.
    if (!api || api->version != PLA_MOD_API_VERSION || !PLA_API_HAS(api, log)) return 1;
    g_api = api;
    cfg::Value config;
    std::string err;
    if (!cfg::Parse(config_json, config, &err)) {
        Log(PLA_LOG_ERROR, "nnshim: invalid config JSON (%s); using defaults", err.c_str());
        config = cfg::Value{};
    }
    ConfigureRuntime(config);
    std::string fams;
    for (int i = 0; i < static_cast<int>(Family::Count); ++i) {
        const Mode m = FamilyMode(static_cast<Family>(i));
        fams += std::string(i ? " " : "") + FamilyName(static_cast<Family>(i)) + "=" +
                (m == Mode::Native ? "native" : m == Mode::Observe ? "observe" : "off");
    }
    Log(PLA_LOG_INFO, "nnshim: families: %s", fams.c_str());
    for (ModuleHooks* m = ModuleList(); m; m = m->next)
        if (m->init) m->init(config);
    StartStatsThread();
    return 0;
}

#ifdef _WIN32
// Only marks process exit (see ProcessExiting in runtime.hpp). The CRT calls DllMain on
// DLL_PROCESS_DETACH before running the DLL's static destructors.
extern "C" int __stdcall DllMain(void*, unsigned long reason, void* reserved) {
    if (reason == 0 /* DLL_PROCESS_DETACH */ && reserved != nullptr) SetProcessExiting();
    if (reason == 0) nnshim::hid::ReleaseCursorOnExit();   // captured mouse: do not leave the cursor clipped
    return 1;
}
#endif

NNSHIM_EXPORT void pla_mod_shutdown(void) {
    for (ModuleHooks* m = ModuleList(); m; m = m->next)
        if (m->shutdown) m->shutdown();
    // Counter summary in the suyu log (in addition to stats_path).
    for (AdapterStats* s = AdapterList(); s; s = s->next) {
        const uint64_t calls = s->calls.load();
        if (calls)
            Log(PLA_LOG_INFO, "nnshim: %-48s %-8s calls=%llu native=%llu passed=%llu", s->name,
                FamilyName(s->family), static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(s->native.load()), static_cast<unsigned long long>(s->passed.load()));
    }
    StopStatsThread();
    g_api = nullptr;
}

// For runtimes without mod.json (v1.5 or the v0 executable): statistics as JSON.
NNSHIM_EXPORT const char* nnshim_stats_json(void) {
    static std::string s;
    s = StatsJson();
    return s.c_str();
}
