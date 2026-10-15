// nnshim: shared pieces of the AAPCS64 adapters (abi_*.cpp).
//
// An adapter is declared with NNSHIM_ADAPTER(Family, exported_name) { ... body ... }:
// the macro exports `exported_name` (the mod.json "pre"), counts the call, checks the
// family's mode (runtime.hpp) and runs the body only in native mode. The body returns
// PLA_SKIP (handled) or PLA_CONTINUE (foreign resource: to the original SDK).
//
//   NNSHIM_ADAPTER(Time, nnshim_time_GetCurrentTime) {
//       PlaGuest g(ctx);
//       ...
//       return PLA_SKIP;
//   }
//
// NNSHIM_OBSERVER(name) { ... } optionally defines what the adapter does in observe mode
// (only read arguments and store them in call_data for a "post").
//
// Adapters only use pla_api (guest registers and memory): a runtime implementing that
// interface (v1.5) reuses them unchanged.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "nnshim/guest.hpp"
#include "nnshim/runtime.hpp"
#include "pla_mod_api.h"

namespace nnshim {

extern const pla_api* g_api;

class PlaGuest final : public Guest {
public:
    explicit PlaGuest(pla_ctx* c) : c_(c) {}
    uint64_t x(unsigned n) override { return g_api->get_x(c_, n); }
    void set_x(unsigned n, uint64_t v) override { g_api->set_x(c_, n, v); }
    size_t read(uint64_t va, void* dst, size_t n) override { return g_api->read_mem(c_, va, dst, n); }
    size_t write(uint64_t va, const void* src, size_t n) override { return g_api->write_mem(c_, va, src, n); }
    uint64_t sp() { return g_api->get_sp(c_); }
    void get_v(unsigned n, uint64_t out[2]) { g_api->get_v(c_, n, out); }
    void set_v(unsigned n, uint64_t lo, uint64_t hi) { g_api->set_v(c_, n, lo, hi); }
    uint64_t* call_data() { return g_api->call_data(c_); }
    pla_ctx* ctx() { return c_; }

private:
    pla_ctx* c_;
};

void Log(int level, const char* fmt, ...);

// User controls file (abi_hid.cpp): reads and parses it. false if missing or invalid.
bool LoadControlsFile(const cfg::Value& c, cfg::Value& out, std::string& path);

// RomFS read served by the shim (abi_fs.cpp): nnshim::ui replaces button icons there
// (abi_ui.cpp). `dst` holds the `n` bytes read from `off`; they go to guest address `guest_va`.
void UiOnRomRead(uint64_t handle, int64_t off, uint8_t* dst, uint64_t n, uint64_t guest_va);

// Runs an adapter according to its family's mode and updates its counters.
inline pla_action RunAdapter(AdapterStats& s, pla_ctx* ctx, pla_action (*body)(pla_ctx*)) {
    s.calls.fetch_add(1, std::memory_order_relaxed);
    const Mode m = FamilyMode(s.family);
    if (m != Mode::Native) {
        if (m == Mode::Observe && s.observe) s.observe(ctx);
        s.passed.fetch_add(1, std::memory_order_relaxed);
        return PLA_CONTINUE;
    }
    const pla_action r = body(ctx);
    (r == PLA_SKIP ? s.native : s.passed).fetch_add(1, std::memory_order_relaxed);
    return r;
}

// UTF-16 (guest char16_t) <-> host UTF-8.
std::string Utf16ToUtf8(const std::u16string& s);
std::u16string Utf8ToUtf16(const std::string& s);
bool LoadU16String(Guest& g, uint64_t va, std::u16string& out, size_t max_chars);

} // namespace nnshim

#define NNSHIM_ADAPTER(FAM, NAME)                                                                       \
    static pla_action NAME##_body(pla_ctx* ctx);                                                        \
    static ::nnshim::AdapterStats NAME##_stats{#NAME, ::nnshim::Family::FAM};                           \
    extern "C" PLA_MOD_EXPORT pla_action NAME(pla_ctx* ctx) {                                          \
        return ::nnshim::RunAdapter(NAME##_stats, ctx, &NAME##_body);                                   \
    }                                                                                                   \
    static pla_action NAME##_body([[maybe_unused]] pla_ctx* ctx)

#define NNSHIM_OBSERVER(NAME)                                                                           \
    static void NAME##_observe(pla_ctx* ctx);                                                           \
    static void NAME##_observe_thunk(void* c) { NAME##_observe(static_cast<pla_ctx*>(c)); }             \
    static const bool NAME##_observe_reg = (NAME##_stats.observe = &NAME##_observe_thunk, true);       \
    static void NAME##_observe([[maybe_unused]] pla_ctx* ctx)

#define NNSHIM_EXPORT extern "C" PLA_MOD_EXPORT
