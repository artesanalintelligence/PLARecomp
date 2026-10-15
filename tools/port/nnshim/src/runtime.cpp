// nnshim: families, counters, trace and statistics (see include/nnshim/runtime.hpp).
#include "nnshim/runtime.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace nnshim {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

std::atomic<int> g_modes[static_cast<int>(Family::Count)] = {};
bool g_modes_init = [] {
    for (auto& m : g_modes) m.store(static_cast<int>(Mode::Native));
    return true;
}();

AdapterStats* g_list = nullptr;   // static zero-init: safe before the ctors run

std::mutex g_counter_mutex;
std::map<std::string, uint64_t>& Counters() {
    static std::map<std::string, uint64_t> m;
    return m;
}

RuntimeConfig g_rt;
std::mutex g_trace_mutex;
std::FILE* g_trace = nullptr;

std::mutex g_stats_mutex;
std::condition_variable g_stats_cv;
ExitSafeThread g_stats_thread;
bool g_stats_stop = false;

const char* const kNames[] = {"hid", "fs", "time", "settings", "applet", "account",
                              "err", "swkbd", "font", "net", "audio", "os", "fps", "ui"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Family::Count));

std::string Escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o.push_back(c);
    }
    return o;
}

} // namespace

const char* FamilyName(Family f) {
    const int i = static_cast<int>(f);
    return (i >= 0 && i < static_cast<int>(Family::Count)) ? kNames[i] : "?";
}

bool FamilyFromName(const std::string& name, Family& out) {
    std::string n = name;
    if (n == "oe") n = "applet";
    if (n == "pl") n = "font";
    for (int i = 0; i < static_cast<int>(Family::Count); ++i)
        if (n == kNames[i]) { out = static_cast<Family>(i); return true; }
    return false;
}

Mode FamilyMode(Family f) {
    (void)g_modes_init;
    return static_cast<Mode>(g_modes[static_cast<int>(f)].load(std::memory_order_relaxed));
}

void SetFamilyMode(Family f, Mode m) { g_modes[static_cast<int>(f)].store(static_cast<int>(m)); }

AdapterStats::AdapterStats(const char* n, Family f) : name(n), family(f) {
    next = g_list;
    g_list = this;
}

AdapterStats* AdapterList() { return g_list; }

namespace {
ModuleHooks* g_modules = nullptr;
}

ModuleHooks::ModuleHooks(const char* n, void (*i)(const cfg::Value&), void (*s)()) : name(n), init(i), shutdown(s) {
    // Append to the list: init order is link order (stable across builds).
    ModuleHooks** p = &g_modules;
    while (*p) p = &(*p)->next;
    *p = this;
}

ModuleHooks* ModuleList() { return g_modules; }

void CounterAdd(const char* name, uint64_t delta) {
    std::lock_guard lk(g_counter_mutex);
    Counters()[name] += delta;
}

uint64_t CounterGet(const char* name) {
    std::lock_guard lk(g_counter_mutex);
    auto it = Counters().find(name);
    return it == Counters().end() ? 0 : it->second;
}

void ConfigureRuntime(const cfg::Value& root) {
    for (int i = 0; i < static_cast<int>(Family::Count); ++i) SetFamilyMode(static_cast<Family>(i), Mode::Native);
    if (const cfg::Value* fam = root.Find("families"); fam && fam->IsObject()) {
        for (const auto& [k, v] : fam->obj) {
            Family f;
            if (!FamilyFromName(k, f)) continue;
            Mode m = Mode::Native;
            if (v.type == cfg::Value::Bool) m = v.b ? Mode::Native : Mode::Off;
            else if (v.type == cfg::Value::String) {
                if (v.str == "observe") m = Mode::Observe;
                else if (v.str == "off" || v.str == "hle" || v.str == "false") m = Mode::Off;
            }
            SetFamilyMode(f, m);
        }
    }
    g_rt.trace_path = root.GetString("trace_path");
    g_rt.stats_path = root.GetString("stats_path");
    g_rt.stats_interval_s = static_cast<int>(root.GetInt("stats_interval_s", 5));
    g_rt.hle_backing = root.GetBool("hle_backing", true);
    std::lock_guard lk(g_trace_mutex);
    if (g_trace) { std::fclose(g_trace); g_trace = nullptr; }
    if (!g_rt.trace_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(g_rt.trace_path).parent_path(), ec);
#ifdef _WIN32
        g_trace = _wfopen(std::filesystem::u8path(g_rt.trace_path).c_str(), L"w");
#else
        g_trace = std::fopen(g_rt.trace_path.c_str(), "w");
#endif
    }
}

const RuntimeConfig& Runtime() { return g_rt; }

namespace {
std::atomic<bool> g_process_exiting{false};
}
bool ProcessExiting() { return g_process_exiting.load(std::memory_order_relaxed); }
void SetProcessExiting() { g_process_exiting.store(true, std::memory_order_relaxed); }

uint64_t UptimeMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count());
}

bool TraceEnabled() { return g_trace != nullptr; }

void Trace(const char* fmt, ...) {
    if (!g_trace) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::lock_guard lk(g_trace_mutex);
    if (!g_trace) return;
    std::fprintf(g_trace, "%llu %s\n", static_cast<unsigned long long>(UptimeMs()), line);
}

std::string StatsJson() {
    std::string o = "{\n  \"uptime_ms\": " + std::to_string(UptimeMs()) + ",\n  \"families\": {";
    for (int i = 0; i < static_cast<int>(Family::Count); ++i) {
        const Mode m = FamilyMode(static_cast<Family>(i));
        o += std::string(i ? ", " : "") + "\"" + kNames[i] + "\": \"" +
             (m == Mode::Native ? "native" : m == Mode::Observe ? "observe" : "off") + "\"";
    }
    o += "},\n  \"adapters\": [";
    bool first = true;
    for (AdapterStats* s = g_list; s; s = s->next) {
        const uint64_t calls = s->calls.load();
        if (!calls) continue;
        o += std::string(first ? "\n" : ",\n") + "    {\"name\": \"" + s->name + "\", \"family\": \"" +
             FamilyName(s->family) + "\", \"calls\": " + std::to_string(calls) +
             ", \"native\": " + std::to_string(s->native.load()) + ", \"passed\": " + std::to_string(s->passed.load()) + "}";
        first = false;
    }
    o += "\n  ],\n  \"counters\": {";
    {
        std::lock_guard lk(g_counter_mutex);
        first = true;
        for (const auto& [k, v] : Counters()) {
            o += std::string(first ? "\n" : ",\n") + "    \"" + Escape(k) + "\": " + std::to_string(v);
            first = false;
        }
    }
    o += "\n  }\n}\n";
    return o;
}

void WriteStatsFile() {
    if (g_rt.stats_path.empty()) return;
    const auto path = std::filesystem::u8path(g_rt.stats_path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
#ifdef _WIN32
        std::FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
        if (!f) return;
        const std::string j = StatsJson();
        std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
    }
    std::filesystem::rename(tmp, path, ec);
    {
        std::lock_guard lk(g_trace_mutex);
        if (g_trace) std::fflush(g_trace);
    }
}

void StartStatsThread() {
    if (g_rt.stats_path.empty() || g_stats_thread.joinable()) return;
    g_stats_stop = false;
    g_stats_thread = std::thread([] {
        std::unique_lock lk(g_stats_mutex);
        while (!g_stats_stop) {
            g_stats_cv.wait_for(lk, std::chrono::seconds(g_rt.stats_interval_s > 0 ? g_rt.stats_interval_s : 5));
            lk.unlock();
            WriteStatsFile();
            lk.lock();
        }
    });
}

void StopStatsThread() {
    {
        std::lock_guard lk(g_stats_mutex);
        g_stats_stop = true;
    }
    g_stats_cv.notify_all();
    if (g_stats_thread.joinable()) g_stats_thread.join();
    WriteStatsFile();
    std::lock_guard lk(g_trace_mutex);
    if (g_trace) { std::fclose(g_trace); g_trace = nullptr; }
}

} // namespace nnshim
