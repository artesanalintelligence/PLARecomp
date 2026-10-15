// nnshim: "SDK function address -> native adapter" table.
//
// It is the interface for any override system that does not read mod.json (e.g. the
// standalone executable or the v1.5 runtime): it walks the table and patches its block
// index at (module, offset) with the adapter. Built by joining, on the mangled symbol,
// src/shim_table.inc (what is implemented) with generated/plt_imports.inc (where it is) and
// generated/sdk_symbols.inc (internal SDK functions).
#include "nnshim/table.h"

#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pla_mod_api.h"

// Adapter declarations (defined in abi_*.cpp).
#define SHIM(site, status, mangled, fn) SHIM_DECL_##status(fn)
#define SHIM_DECL_NATIVE(fn) extern "C" pla_action fn(pla_ctx*);
#define SHIM_DECL_PASS(fn)
#define SHIM_DECL_TODO(fn)
#include "shim_table.inc"
#if __has_include("audio_table.inc")
#include "audio_table.inc"
#endif
#undef SHIM

namespace {

struct ShimRow { const char* mangled; int site; int status; pla_pre_fn fn; };
enum { SITE_PLT = 0, SITE_SDK = 1, SITE_SDKX = 2 };
enum { ST_NATIVE = 0, ST_PASS = 1, ST_TODO = 2 };
#define SHIM_FN_NATIVE(fn) &fn
#define SHIM_FN_PASS(fn) nullptr
#define SHIM_FN_TODO(fn) nullptr
const ShimRow kShim[] = {
#define SHIM(site, status, mangled, fn) {mangled, SITE_##site, ST_##status, SHIM_FN_##status(fn)},
#include "shim_table.inc"
#if __has_include("audio_table.inc")
#include "audio_table.inc"
#endif
#undef SHIM
};

struct ImportRow { const char* family; uint64_t plt; const char* module; uint64_t sdk_off; uint32_t callsites; const char* mangled; };
#define NN_IMPORT(fam, plt, mod, off, cs, mangled) {#fam, plt, #mod, off, cs, mangled},
const ImportRow kImports[] = {
#include "generated/plt_imports.inc"
};
#undef NN_IMPORT

struct SdkRow { const char* family; const char* module; uint64_t off; const char* mangled; };
#define NN_SDKSYM(fam, mod, off, mangled) {#fam, #mod, off, mangled},
const SdkRow kSdkSyms[] = {
#include "generated/sdk_symbols.inc"
};
#undef NN_SDKSYM

std::once_flag g_once;
std::vector<nnshim_entry> g_table;

void Build() {
    std::set<std::pair<std::string, uint64_t>> seen;
    for (const ShimRow& s : kShim) {
        if (s.status != ST_NATIVE) continue;
        nnshim_entry e{};
        e.mangled = s.mangled;
        e.adapter = s.fn;
        bool found = false;
        if (s.site == SITE_SDKX) {
            for (const SdkRow& r : kSdkSyms) {
                if (std::strcmp(r.mangled, s.mangled) != 0) continue;
                e.family = r.family;
                e.sdk_module = r.module;
                e.sdk_offset = r.off;
                e.hook_sdk = 1;
                found = true;
                break;
            }
        } else {
            for (const ImportRow& r : kImports) {
                if (std::strcmp(r.mangled, s.mangled) != 0) continue;
                e.family = r.family;
                e.main_plt_offset = r.plt;
                e.sdk_module = r.module;
                e.sdk_offset = r.sdk_off;
                e.hook_sdk = s.site == SITE_SDK && r.sdk_off != 0;
                found = true;
                break;
            }
        }
        if (!found) continue;
        const auto key = e.hook_sdk ? std::make_pair(std::string(e.sdk_module), e.sdk_offset)
                                    : std::make_pair(std::string("main"), e.main_plt_offset);
        if (!seen.insert(key).second) continue;   // same definition (e.g. D1/D2)
        g_table.push_back(e);
    }
}

} // namespace

extern "C" const nnshim_entry* nnshim_get_table(size_t* count) {
    std::call_once(g_once, Build);
    if (count) *count = g_table.size();
    return g_table.data();
}
