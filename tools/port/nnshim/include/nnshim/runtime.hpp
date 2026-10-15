// nnshim: shared adapter infrastructure (families, counters, trace).
//
// Families: each adapter belongs to an inventory family (docs/port/INVENTORY.md).
// Each family's mode is set in the mod.json "config":
//
//   "families": { "hid": "native", "fs": "observe", "time": false, "account": true }
//
//   native  (or true)  the adapter handles the call (PLA_SKIP) if the resource is its own;
//   observe            the adapter does NOT handle it (PLA_CONTINUE), but counts it and, if the
//                      family implements it (fs), records the call in the trace: this is the
//                      "HLE" side of A/B tests;
//   off     (or false) plain PLA_CONTINUE: the family behaves as if the shim did not exist.
//
// Without "families", every family hooked in mod.json is "native" (as in the
// prototype). Counters are logged on shutdown and written to "stats_path" (JSON) every
// few seconds.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <string>

#include "nnshim/config.hpp"

namespace nnshim {

enum class Family : int {
    Hid, Fs, Time, Settings, Applet, Account, Err, Swkbd, Font, Net, Audio, Os, Fps, Ui, Count
};
enum class Mode : int { Off = 0, Observe = 1, Native = 2 };

const char* FamilyName(Family f);
bool FamilyFromName(const std::string& name, Family& out);   // accepts aliases: oe, pl
Mode FamilyMode(Family f);
void SetFamilyMode(Family f, Mode m);

// Per-adapter counters (one per exported function). They register themselves in a global
// list on construction (static objects in each abi_*.cpp).
struct AdapterStats {
    const char* name;
    Family family;
    std::atomic<uint64_t> calls{0};    // times the runtime entered the adapter
    std::atomic<uint64_t> native{0};   // handled by the shim (PLA_SKIP)
    std::atomic<uint64_t> passed{0};   // passed to the original (PLA_CONTINUE)
    void (*observe)(void* ctx) = nullptr;   // optional: what to do in observe mode
    AdapterStats* next = nullptr;
    AdapterStats(const char* n, Family f);
};
AdapterStats* AdapterList();

// Free-form counters (e.g. "fs.validate.bytes"), for validation and tests.
void CounterAdd(const char* name, uint64_t delta = 1);
uint64_t CounterGet(const char* name);

// General configuration (families, trace, statistics). Call once in pla_mod_init.
struct RuntimeConfig {
    std::string trace_path;    // empty = no trace
    std::string stats_path;    // empty = no statistics file
    int stats_interval_s = 5;
    bool hle_backing = true;   // v1: time/oe/account Initialize also run in the HLE
};
void ConfigureRuntime(const cfg::Value& root);
const RuntimeConfig& Runtime();

// Text trace (one line per event, timestamped in ms since startup).
bool TraceEnabled();
void Trace(const char* fmt, ...);

// Statistics: JSON with families, adapters and free-form counters.
std::string StatsJson();
void WriteStatsFile();
void StartStatsThread();
void StopStatsThread();

// Ms since the shim was loaded.
uint64_t UptimeMs();

// Shim modules (a family or a group): they self-register with a static object and
// pla_mod_init/pla_mod_shutdown (abi_mod.cpp) walk them. So a new module (e.g.
// audio_*.cpp) does not need to touch abi_mod.cpp.
struct ModuleHooks {
    const char* name;
    void (*init)(const cfg::Value& config);
    void (*shutdown)();
    ModuleHooks* next = nullptr;
    ModuleHooks(const char* n, void (*i)(const cfg::Value&), void (*s)());
};
ModuleHooks* ModuleList();

// Process exit (ExitProcess, console close): DllMain sets it on DLL_PROCESS_DETACH with
// lpReserved != NULL, before the CRT runs static destructors. By then the other threads are
// dead and the loader lock is held: do not join threads or release COM/XAudio2
// resources.
bool ProcessExiting();
void SetProcessExiting();

// std::thread for globals: if still "joinable" when destroyed (the host exited without calling
// pla_mod_shutdown), it detaches instead of calling std::terminate() (abort on exit).
struct ExitSafeThread : std::thread {
    ExitSafeThread() noexcept = default;
    ExitSafeThread& operator=(std::thread&& t) noexcept {
        std::thread::operator=(std::move(t));
        return *this;
    }
    ~ExitSafeThread() {
        if (joinable()) detach();
    }
};

} // namespace nnshim
