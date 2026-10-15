// nvnvk mod entry point: configuration, hook table and dispatch.
// SPDX-License-Identifier: MIT
#include <chrono>
#include <intrin.h>
#include <thread>
#include <condition_variable>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>

#include "capture.h"
#include "exec.h"
#include "record.h"
#include "hostmem.h"
#include "nvnvk.h"
#include "options.h"
#include "overlay.h"
#include "textures.h"
#include "vkctx.h"

namespace nvnvk {

Config g_cfg;
Stats g_stats;

namespace {

struct FnInfo {
    const char* name;
    uint32_t offset;
    uint32_t flags;
};
const FnInfo kFns[] = {
#define NVN_FN(idx, name, off, fl) {#name, off, fl},
#include "nvn_funcs.inc"
#undef NVN_FN
};

PreFn g_pre[FN_COUNT] = {};
PostFn g_post[FN_COUNT] = {};
// Hook PC -> function index: open addressing, 1024 slots for ~200 hooks (FPS-1: one probe on the game thread instead of
// an unordered_map lookup per NVN call). Filled once before any hook runs.
struct PcSlot {
    uint64_t pc = 0;
    uint32_t idx = 0;
};
PcSlot g_by_pc[1024];
inline uint32_t PcHash(uint64_t pc) { return (uint32_t)(((pc >> 2) * 0x9E3779B97F4A7C15ull) >> 54); }
void PcInsert(uint64_t pc, uint32_t idx) {
    for (uint32_t h = PcHash(pc);; h = (h + 1) & 1023)
        if (!g_by_pc[h].pc || g_by_pc[h].pc == pc) {
            g_by_pc[h] = {pc, idx};
            return;
        }
}
inline bool PcFind(uint64_t pc, uint32_t* idx) {
    for (uint32_t h = PcHash(pc);; h = (h + 1) & 1023) {
        if (g_by_pc[h].pc == pc) {
            *idx = g_by_pc[h].idx;
            return true;
        }
        if (!g_by_pc[h].pc) return false;
    }
}
std::FILE* g_log = nullptr;
std::mutex g_log_mu;
std::atomic<uint64_t> g_calls[FN_COUNT];
bool g_skip_unhandled[FN_COUNT] = {};
std::atomic<bool> g_warned[FN_COUNT];
bool g_ready = false;
std::chrono::steady_clock::time_point g_t0;
uint64_t g_last_report_frame = 0;

std::string CfgStr(const char* json, const char* key, const char* def) {
    std::string j = json ? json : "";
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return def;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return def;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) ++p;
    if (p < j.size() && j[p] == '"') {
        std::string out;
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] == '\\' && p + 1 < j.size()) ++p;
            out += j[p];
        }
        return out;
    }
    size_t e = j.find_first_of(",}", p);
    std::string v = j.substr(p, e - p);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return v;
}
bool CfgBool(const char* json, const char* key, bool def) {
    std::string v = CfgStr(json, key, def ? "true" : "false");
    return v == "true" || v == "1";
}

std::chrono::steady_clock::time_point g_last_report_t;
uint64_t g_last_report_frames = 0, g_last_gpu_ns = 0, g_last_submit_ns = 0;

// ---- crash diagnostics: logs exceptions whose address is inside this DLL
uintptr_t g_mod_lo = 0, g_mod_hi = 0;
std::atomic<int> g_faults{0};

LONG CALLBACK FaultHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363 /* C++ */ || code == 0x406D1388 /* thread name */ || code < 0x80000000) return EXCEPTION_CONTINUE_SEARCH;
    if (t_guarded > 0 && code == EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;  // caught by GuardedCopy
    const uintptr_t pc = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    const bool inside = pc >= g_mod_lo && pc < g_mod_hi;
    if (!inside && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    char frames[512];
    int fn = 0, found = 0;
    frames[0] = 0;
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
    for (int i = 0; i < 2048 && found < 24 && fn < (int)sizeof frames - 24; ++i) {
        uintptr_t v = 0;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (v >= g_mod_lo && v < g_mod_hi) {
            fn += std::snprintf(frames + fn, sizeof frames - fn, " %llx", (unsigned long long)(v - g_mod_lo));
            ++found;
        }
        if (!inside && i >= 256 && !found) break;  // someone else's fault (e.g. suyu fastmem): not ours
    }
    if (!inside && !found) return EXCEPTION_CONTINUE_SEARCH;
    if (g_faults.fetch_add(1) > 4) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE where = nullptr;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &where))
        GetModuleFileNameA(where, mod, sizeof mod);
    Log(PLA_LOG_ERROR, "CRASH: code %08lx at %s+%llx (address %llx); nvnvk on the stack:%s", code, mod,
        (unsigned long long)(pc - (uintptr_t)where),
        ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1] : 0ull,
        frames);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallFaultHandler() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallFaultHandler), &self);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(self) + dos->e_lfanew);
    g_mod_lo = (uintptr_t)self;
    g_mod_hi = g_mod_lo + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, FaultHandler);
}

void Report() {
    {
        char b[512];
        int n = 0;
        for (int t = 0; t < 32; ++t)
            if (const uint64_t c = g_counter_types[t].load()) n += std::snprintf(b + n, sizeof b - n, " t%d=%llu", t, (unsigned long long)c);
        if (n) NVK_INFO("ReportCounter by type:%s", b);
    }
    const auto now = std::chrono::steady_clock::now();
    const uint64_t fr = g_stats.frames.load(), gns = g_vk.gpu_ns.load();
    if (g_last_report_t.time_since_epoch().count()) {
        const double dt = std::chrono::duration<double>(now - g_last_report_t).count();
        const uint64_t df = fr - g_last_report_frames;
        const uint64_t sns = g_stats.submit_ns.load();
        NVK_HOST("rate: %.1f frames/s, nvnvk GPU %.3f ms/frame, submit CPU %.3f ms/frame", dt > 0 ? df / dt : 0.0,
                 df ? (gns - g_last_gpu_ns) / 1e6 / df : 0.0, df ? (sns - g_last_submit_ns) / 1e6 / df : 0.0);
        g_last_submit_ns = sns;
        static uint64_t lw[4] = {};
        uint64_t w[4];
        for (int i = 0; i < 4; ++i) w[i] = g_game_wait_ns[i].load(std::memory_order_relaxed);
        if (df)
            NVK_HOST("game threads blocked in nvnvk (ms/frame): GPU fence (SyncWait) %.2f, present (previous frame's GPU) %.2f, "
                     "window acquire %.2f, submit queue full/Finish %.2f",
                     (w[0] - lw[0]) / 1e6 / df, (w[1] - lw[1]) / 1e6 / df, (w[2] - lw[2]) / 1e6 / df, (w[3] - lw[3]) / 1e6 / df);
        for (int i = 0; i < 4; ++i) lw[i] = w[i];
    }
    g_last_report_t = now;
    g_last_report_frames = fr;
    g_last_gpu_ns = gns;
    NVK_INFO("frames=%llu presents=%llu submits=%llu recordings=%llu commands=%llu draws=%llu (not drawn %llu) "
             "clears=%llu copies=%llu textures=%llu (reuploaded %llu) pipelines=%llu upload=%.1f MB dispatches=%llu "
             "arena_faults=%llu",
             (unsigned long long)g_stats.frames.load(), (unsigned long long)g_stats.presents.load(),
             (unsigned long long)g_stats.submits.load(), (unsigned long long)g_stats.recordings.load(),
             (unsigned long long)g_stats.cmds_recorded.load(), (unsigned long long)g_stats.draws.load(),
             (unsigned long long)g_stats.draws_skipped.load(), (unsigned long long)g_stats.clears.load(),
             (unsigned long long)g_stats.copies.load(), (unsigned long long)g_stats.textures.load(),
             (unsigned long long)g_stats.reuploads.load(),
             (unsigned long long)g_stats.pipelines.load(), g_stats.uploads_bytes.load() / 1048576.0,
             (unsigned long long)g_stats.dispatches.load(), (unsigned long long)g_guarded_faults.load());
}

}  // namespace

// Asynchronous log (sprint 6, X2, PERF request): Log() only enqueues; a thread writes the file (one fflush per batch)
// and forwards only warnings and errors to the PLA log (api->log writes to the console on the calling thread: with
// the party loaded, the periodic report on the game's NVN thread caused 100-190 ms frames). The same thread runs the
// periodic report (Report/PipelineReport) when the present asks for it.
namespace {
struct LogLine {
    int level;
    std::string text;  // with the timestamp and the newline
};
std::deque<LogLine> g_logq;
std::condition_variable g_log_cv;
std::thread g_log_thread;
bool g_log_quit = false;
std::atomic<bool> g_log_async{false}, g_report_req{false};
std::vector<RecordingPtr> g_log_graveyard;  // retired recordings: freed on the log thread

void WriteLines(std::deque<LogLine>& q) {
    if (g_log) {
        for (const LogLine& l : q) std::fputs(l.text.c_str(), g_log);
        std::fflush(g_log);
    }
    if (g_guest.api)
        for (const LogLine& l : q)
            if ((l.level & 0xFF) >= PLA_LOG_WARNING || (l.level & kLogToHost)) {
                const size_t b = l.text.find("] ");
                std::string msg = b == std::string::npos ? l.text : l.text.substr(b + 2);
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
                if (l.level & kLogToHost) msg = "nvnvk: " + msg;
                g_guest.api->log(l.level & 0xFF, msg.c_str());
            }
}

void LogThread() {
    std::unique_lock<std::mutex> lk(g_log_mu);
    for (;;) {
        g_log_cv.wait_for(lk, std::chrono::milliseconds(500),
                          [] { return g_log_quit || !g_logq.empty() || g_report_req.load(); });
        if (g_report_req.exchange(false)) {
            lk.unlock();
            Report();
            PipelineReport();
            lk.lock();
        }
        std::deque<LogLine> q;
        q.swap(g_logq);
        std::vector<RecordingPtr> graves;
        graves.swap(g_log_graveyard);
        const bool quit = g_log_quit;
        lk.unlock();
        WriteLines(q);
        graves.clear();
        lk.lock();
        if (quit && g_logq.empty()) return;
    }
}

void StartLogThread() {
    g_log_async = true;
    g_log_thread = std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"nvnvk log");
        LogThread();
    });
}

void StopLogThread() {
    if (!g_log_async.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log_quit = true;
    }
    g_log_cv.notify_all();
    if (g_log_thread.joinable()) g_log_thread.join();
}
}  // namespace

void Log(int level, const char* fmt, ...) {
    const bool to_host = (level & kLogToHost) != 0;
    level &= 0xFF;
    if (level < g_cfg.log_level) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    char line[2100];
    std::snprintf(line, sizeof line, "[%9.3f] %s\n", t, buf);
    if (g_log_async.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lk(g_log_mu);
            g_logq.push_back({level | (to_host ? kLogToHost : 0), line});
        }
        if (level >= PLA_LOG_WARNING || to_host) g_log_cv.notify_one();  // the rest goes in the next batch (<= 0.5 s)
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        if (g_log) {
            std::fputs(line, g_log);
            std::fflush(g_log);
        }
    }
    if ((level >= PLA_LOG_WARNING || to_host) && g_guest.api) g_guest.api->log(level, (std::string("nvnvk: ") + buf).c_str());
}

const char* FnName(uint32_t fn) { return fn < FN_COUNT ? kFns[fn].name : "?"; }

}  // namespace nvnvk

using namespace nvnvk;

bool nvnvk::InitVulkan() {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (g_vk.dev) return true;
    const pla_api* api = g_guest.api;
    if (!g_vk.Init()) return false;
    {
        // FPS-1: cost of a QueryPerformanceCounter call on this machine. ~20 ns with the invariant TSC; on machines
        // where Windows uses another clock source (HPET/ACPI, some hypervisors) it can be ~0.5-1 us, and every clock
        // read in a per-draw or per-NVN-call path then multiplies.
        LARGE_INTEGER f, a, b, x;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < 20000; ++i) QueryPerformanceCounter(&x);
        QueryPerformanceCounter(&b);
        const double ns = (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / 20000.0;
        NVK_HOST("timer: QueryPerformanceCounter %.0f ns/call, frequency %lld Hz%s", ns, (long long)f.QuadPart,
                 ns > 200 ? " (SLOW clock source: per-call timing costs real CPU here)" : "");
    }
    ApplyAutoPreset(g_vk.low_end);
    g_hostmem.Init();
    PipelineInit();
    if (g_cfg.present) {
        HWND game = nullptr;
        if (g_cfg.exclusive && g_cfg.game_window && PLA_API_HAS(api, window_handle) && api->window_handle)
            game = static_cast<HWND>(api->window_handle());
        if (game) {
            NVK_INFO("presenting in the game window (%p)", (void*)game);
            if (!g_present.StartOnWindow(game)) {
                NVK_WARN("could not present in the game window: using our own window");
                g_present.Start(g_cfg.window_width, g_cfg.window_height);
            }
        } else {
            g_present.Start(g_cfg.window_width, g_cfg.window_height);
        }
    }
    return true;
}

extern "C" {

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* api, const char* config_json) {
    g_t0 = std::chrono::steady_clock::now();
    InstallFaultHandler();
    g_guest.api = api;
    g_guest.host_ptr = PLA_API_HAS(api, host_pointer) && api->host_pointer;
    g_guest.sdk_base = api->module_base("sdk");
    g_guest.main_base = api->module_base("main");
    g_cfg.exclusive = CfgStr(config_json, "mode", "shadow") == "exclusive";
    g_cfg.present = CfgBool(config_json, "present", true);
    g_cfg.vsync = CfgBool(config_json, "vsync", false);
    g_cfg.validation = CfgBool(config_json, "validation", false);
    g_cfg.capture_dir = CfgStr(config_json, "capture_dir", "");
    g_cfg.capture_every = std::stoi(CfgStr(config_json, "capture_every", "0"));
    g_cfg.capture_max = std::stoi(CfgStr(config_json, "capture_max", "0"));
    g_cfg.t0 = g_t0;
    {
        std::string secs = CfgStr(config_json, "capture_secs", "");  // "20,30,40"
        size_t p = 0;
        while (p < secs.size()) {
            size_t e = secs.find(',', p);
            if (e == std::string::npos) e = secs.size();
            if (e > p) g_cfg.capture_secs.push_back(std::stod(secs.substr(p, e - p)));
            p = e + 1;
        }
    }
    g_cfg.shader_cache_dir = CfgStr(config_json, "shader_cache_dir", "");
    g_cfg.log_level = std::stoi(CfgStr(config_json, "log_level", "2"));
    g_cfg.pipeline_cache = CfgStr(config_json, "pipeline_cache", "");
    g_cfg.pipeline_cache_out = CfgStr(config_json, "pipeline_cache_out", "");
    g_cfg.import_memory = CfgBool(config_json, "import_memory", true);
    g_cfg.import_in_shadow = CfgBool(config_json, "import_in_shadow", false);
    g_cfg.isolate_layers = CfgBool(config_json, "isolate_layers", true);
    g_cfg.native_cmdmem = CfgBool(config_json, "native_cmdmem", true);
    g_cfg.import_arena = CfgBool(config_json, "import_arena", false);
    g_cfg.unified_memory = CfgBool(config_json, "unified_memory", true);
    g_cfg.unified_copy_gpu_only = CfgBool(config_json, "unified_copy_gpu_only", false);
    g_cfg.unified_copy_gpu = CfgBool(config_json, "unified_copy_gpu", true);
    g_cfg.unified_barriers = CfgBool(config_json, "unified_barriers", true);
    g_cfg.unified_barriers_all = CfgBool(config_json, "unified_barriers_all", false);
    g_cfg.import_bind = CfgBool(config_json, "import_bind", true);
    g_cfg.import_backing = CfgBool(config_json, "import_backing", true);
    g_cfg.ubo_full_range = CfgBool(config_json, "ubo_full_range", false);
    g_cfg.ssbo_from_ubo = CfgBool(config_json, "ssbo_from_ubo", true);
    g_cfg.pool_evict_overlap = CfgBool(config_json, "pool_evict_overlap", true);
    g_cfg.texel_buffers = CfgBool(config_json, "texel_buffers", true);
    g_cfg.renderdoc_dir = CfgStr(config_json, "renderdoc_dir", "");
    g_cfg.xfb_check = CfgBool(config_json, "xfb_check", false);
    g_cfg.xfb_trace = (uint32_t)std::stoul(CfgStr(config_json, "xfb_trace", "0"));
    g_cfg.xfb_trace_from = std::stod(CfgStr(config_json, "xfb_trace_from", "0"));
    g_cfg.xfb_trace_to = std::stod(CfgStr(config_json, "xfb_trace_to", "1000000000"));
    g_cfg.xfb_rebind_reset = CfgBool(config_json, "xfb_rebind_reset", true);
    g_cfg.xfb_begin_resume = CfgBool(config_json, "xfb_begin_resume", false);
    g_cfg.recording_max_age = std::stoull(CfgStr(config_json, "recording_max_age", "0"));
    g_cfg.vram_pools = CfgBool(config_json, "vram_pools", true);
    g_cfg.vbib_snapshot = CfgBool(config_json, "vbib_snapshot", true);
    g_cfg.vbib_trim = CfgBool(config_json, "vbib_trim", false);
    g_cfg.cpu_stats = CfgBool(config_json, "cpu_stats", false);
    g_cfg.state_dedup = CfgBool(config_json, "state_dedup", true);
    g_cfg.snap_cache_mb = (uint32_t)std::stoul(CfgStr(config_json, "snap_cache_mb", "128"));
    g_cfg.snap_vram = CfgStr(config_json, "snap_vram", "auto");
    g_cfg.host_read_stats = CfgBool(config_json, "host_read_stats", true);
    g_cfg.preset = CfgStr(config_json, "preset", "auto");
    g_cfg.lowend_force = CfgBool(config_json, "lowend_force", false);
    g_cfg.anisotropy = (uint32_t)std::stoul(CfgStr(config_json, "anisotropy", "0"));
    g_cfg.sharpen = std::stof(CfgStr(config_json, "sharpen", "0"));
    {
        const std::string aa = CfgStr(config_json, "aa", "off");
        g_cfg.aa = aa == "fxaa" ? 1 : (aa == "smaa" ? 2 : 0);
    }
    g_cfg.aa_hud = CfgBool(config_json, "aa_hud", true);
    g_cfg.aa_debug = CfgBool(config_json, "aa_debug", false);
    g_cfg.trace_window = CfgBool(config_json, "trace_window", false);
    g_cfg.aa_scene_draw = (uint32_t)std::max(1ul, std::stoul(CfgStr(config_json, "aa_scene_draw", "1")));
    g_cfg.upscale_sharp = CfgStr(config_json, "upscale", "bilinear") == "sharp";
    g_cfg.fps_limit = (uint32_t)std::stoul(CfgStr(config_json, "fps_limit", "0"));
    g_cfg.brightness = std::stof(CfgStr(config_json, "brightness", "0"));
    g_cfg.contrast = std::stof(CfgStr(config_json, "contrast", "1"));
    g_cfg.saturation = std::stof(CfgStr(config_json, "saturation", "1"));
    g_cfg.show_fps = CfgBool(config_json, "show_fps", false);
    g_cfg.import_chunk_mb = (uint32_t)std::stoul(CfgStr(config_json, "import_chunk_mb", "512"));
    g_cfg.pool_init_delay_us_per_mb = (uint32_t)std::stoul(CfgStr(config_json, "pool_init_delay_us_per_mb", "0"));
    g_cfg.unified_ssbo_drop_max = std::stoull(CfgStr(config_json, "unified_ssbo_drop_max", "0"));
    g_cfg.unified_mask = (uint32_t)std::stoul(CfgStr(config_json, "unified_mask", "63"));
    g_cfg.import_verify = CfgBool(config_json, "import_verify", false);
    g_cfg.acquire_wait = CfgBool(config_json, "acquire_wait", true);
    g_cfg.defer_submit = CfgBool(config_json, "defer_submit", true);
    g_cfg.game_wait = std::stoi(CfgStr(config_json, "game_wait", "0"));
    g_cfg.game_window = CfgBool(config_json, "game_window", true);
    g_cfg.replay = CfgBool(config_json, "replay", false);
    g_cfg.async_submit = CfgBool(config_json, "async_submit", true);
    g_cfg.flip_viewport = CfgBool(config_json, "flip_viewport", false);
    g_cfg.capture_path = CfgStr(config_json, "capture_path", "");
    g_cfg.capture_frames = std::stoull(CfgStr(config_json, "capture_frames", "1200"));
    g_cfg.capture_max_mb = std::stoull(CfgStr(config_json, "capture_max_mb", "4000"));
    g_cfg.window_width = std::stoi(CfgStr(config_json, "window_width", "1280"));
    g_cfg.window_height = std::stoi(CfgStr(config_json, "window_height", "720"));
    std::string log_dir = CfgStr(config_json, "log_dir", "");
    if (!log_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        char stamp[32];
        std::time_t t = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
        g_cfg.log_path = log_dir + "/nvnvk-" + stamp + ".log";
        g_log = std::fopen(g_cfg.log_path.c_str(), "w");
        if (g_log) StartLogThread();
    }
    if (!g_cfg.capture_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_cfg.capture_dir, ec);
    }
    if (!g_guest.sdk_base) {
        NVK_ERR("nvnvk: no sdk base");
        return 1;
    }
    for (uint32_t i = 0; i < FN_COUNT; ++i) PcInsert(g_guest.sdk_base + kFns[i].offset, i);
    RegisterObjectHooks(g_pre, g_post);
    RegisterRecordHooks(g_pre, g_post);
    RegisterQueueHooks(g_pre, g_post);
    for (uint32_t i = 0; i < FN_COUNT; ++i) {
        const std::string n = kFns[i].name;
        g_skip_unhandled[i] = n.rfind("nvnCommandBuffer", 0) == 0 && n != "nvnCommandBufferInitialize" &&
                              n != "nvnCommandBufferFinalize" && n.find("DebugLabel") == std::string::npos &&
                              n.find("Builder") == std::string::npos;
    }
    g_cfg.execute = CfgBool(config_json, "execute", true);
    g_cfg.dump_rts = CfgBool(config_json, "dump_rts", false);
    g_cfg.start_after = std::stoull(CfgStr(config_json, "start_after", "0"));
    g_cfg.sync_check = CfgBool(config_json, "sync_check", false);
    // In shadow mode, robustness by default: suyu's VkDevice and ours share the process GPU address space
    // (WDDM), so an out-of-range access of ours can hit suyu's memory (intro7b: with robustness, 60 s without a
    // crash; without it, it crashes on its first Present).
    g_cfg.robust = CfgBool(config_json, "robust", !g_cfg.exclusive);
    g_cfg.skip_draws = CfgBool(config_json, "skip_draws", false);
    {
        const std::string tr = CfgStr(config_json, "trace_rt", "");
        size_t p = 0;
        while (p < tr.size()) {
            size_t e = tr.find('/', p);
            if (e == std::string::npos) e = tr.size();
            if (e > p) g_cfg.trace_gpus.push_back(std::stoull(tr.substr(p, e - p), nullptr, 16));
            p = e + 1;
        }
    }
    g_cfg.trace_sec = std::stod(CfgStr(config_json, "trace_sec", "0"));
    g_cfg.trace_lines = std::stoi(CfgStr(config_json, "trace_lines", "400"));
    g_cfg.log_tex_updates = CfgBool(config_json, "log_tex_updates", false);
    g_cfg.log_gpubufs = CfgBool(config_json, "log_gpubufs", false);
    g_cfg.xfb_validate = CfgBool(config_json, "xfb_validate", false);
    g_cfg.gpubuf_sync = CfgBool(config_json, "gpubuf_sync", true);
    g_cfg.gpubuf_writeback = CfgBool(config_json, "gpubuf_writeback", true);
    g_cfg.gpubuf_cover = CfgBool(config_json, "gpubuf_cover", true);
    g_cfg.xfb_partial_max = std::stoull(CfgStr(config_json, "xfb_partial_max", "256"));
    g_cfg.gpubuf_cover_max = std::stoull(CfgStr(config_json, "gpubuf_cover_max", "8388608"));
    g_cfg.xfb_init = CfgBool(config_json, "xfb_init", true);
    g_cfg.chunk_draws = (uint32_t)std::stoul(CfgStr(config_json, "chunk_draws", "300"));
    {
        const std::string o = CfgStr(config_json, "occlusion", "real");
        g_cfg.occlusion_precise = CfgBool(config_json, "occlusion_precise", true);
        g_cfg.occlusion_window = (uint32_t)std::stoul(CfgStr(config_json, "occlusion_window", "64"));
        g_cfg.occlusion = o == "real" || o == "2" ? 2 : o == "fake" || o == "simulado" /* legacy */ || o == "1" ? 1 : 0;
    }
    g_cfg.ssbo_persist = CfgBool(config_json, "ssbo_persist", true);
    g_cfg.ssbo_persist_stages = (uint32_t)std::stoul(CfgStr(config_json, "ssbo_persist_stages", "63"));
    g_cfg.ssbo_cpu_blacklist = CfgBool(config_json, "ssbo_cpu_blacklist", false);
    g_cfg.ssbo_cpu_page = CfgBool(config_json, "ssbo_cpu_page", false);
    g_cfg.ssbo_persist_rw_only = CfgBool(config_json, "ssbo_persist_rw_only", false);
    g_cfg.xfb_partial_vertex = CfgBool(config_json, "xfb_partial_vertex", true);
    g_cfg.counter_value_mask = std::stoull(CfgStr(config_json, "counter_value_mask", "2"));
    g_cfg.counter_value = std::stoull(CfgStr(config_json, "counter_value", "1048576"));
    g_cfg.pipeline_threads = std::stoi(CfgStr(config_json, "pipeline_threads", "2"));
    g_cfg.pipeline_wait_ms = std::stod(CfgStr(config_json, "pipeline_wait_ms", "4"));
    g_cfg.async_variants = CfgBool(config_json, "async_variants", true);
    g_cfg.pipeline_frame_budget_ms = std::stod(CfgStr(config_json, "pipeline_frame_budget_ms", "4"));
    g_cfg.pipeline_min_wait_ms = std::stod(CfgStr(config_json, "pipeline_min_wait_ms", "1"));
    g_cfg.xfb_idle_frames = std::stoull(CfgStr(config_json, "xfb_idle_frames", "0"));
    g_cfg.gpu = CfgStr(config_json, "gpu", "");
    g_cfg.hitch_ms = std::stod(CfgStr(config_json, "hitch_ms", "25"));
    g_cfg.hitch_phase_ms = std::stod(CfgStr(config_json, "hitch_phase_ms", "50"));
    g_cfg.trace_rtsets = CfgBool(config_json, "trace_rtsets", false);
    g_cfg.trace_compute = CfgBool(config_json, "trace_compute", false);
    g_cfg.trace_writes_only = CfgBool(config_json, "trace_writes_only", false);
    if (!CfgBool(config_json, "vulkan", true)) {
        // Diagnostics: hooks only (the mod stays inactive, as if there were no Vulkan).
        NVK_INFO("nvnvk: vulkan=false; hooks only, mod inactive");
        return 0;
    }
    // Shadow with start_after: the VkDevice is created late (after N frames), so it does not overlap suyu's
    // swapchain creation and first Presents (with our device created at boot, shadow mode crashes ~60 % of the
    // time on suyu's first Present; without it, 0 of 6).
    // In-game overlay menu (overlay.cpp): exclusive mode only (in shadow mode suyu presents).
    if (g_cfg.exclusive) g_overlay.Configure(config_json);
    if (!g_cfg.exclusive && g_cfg.start_after) {
        NVK_INFO("shadow: VkDevice deferred until frame %llu", (unsigned long long)g_cfg.start_after);
    } else if (!nvnvk::InitVulkan()) {
        NVK_ERR("nvnvk: Vulkan not available; the mod stays inactive");
        return g_cfg.exclusive ? 1 : 0;
    }
    // Capture also works in exclusive mode (to debug offline what renders wrong in the game); submission is then
    // synchronous, so memory reads land in the capture next to their call.
    if (!g_cfg.capture_path.empty() && g_cfg.exclusive) g_cfg.async_submit = false;
    if (!g_cfg.capture_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_cfg.capture_path).parent_path(), ec);
        if (!g_capture.Open(g_cfg.capture_path, g_cfg.capture_frames, g_cfg.capture_max_mb))
            NVK_WARN("could not open capture %s", g_cfg.capture_path.c_str());
    }
    // shadow_async (diagnostics/alternative): also in shadow mode, Vulkan on its own thread instead of the guest
    // thread (a suyu fiber in the dist). Data is read at execution time.
    if ((g_cfg.exclusive || CfgBool(config_json, "shadow_async", false)) && g_cfg.async_submit) g_exec.StartAsync();
    g_ready = true;
    NVK_INFO("nvnvk ready: mode %s, host_pointer=%d, sdk=%llx", g_cfg.exclusive ? "exclusive" : "shadow",
             (int)g_guest.host_ptr, (unsigned long long)g_guest.sdk_base);
    return 0;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    {
        std::lock_guard<std::mutex> lk(g_capture.mu());
        g_capture.Close();
    }
    Report();
    StopLogThread();  // drains the queue; from here on the log is synchronous again
    if (g_log) {
        std::fprintf(g_log, "calls per function:\n");
        for (uint32_t i = 0; i < FN_COUNT; ++i)
            if (g_calls[i].load()) std::fprintf(g_log, "  %10llu %s\n", (unsigned long long)g_calls[i].load(), kFns[i].name);
    }
    g_ready = false;
    g_exec.StopAsync();
    g_present.Stop();
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        PipelineShutdown();
        g_hostmem.Shutdown();
        g_tex.Shutdown();
        g_vk.Shutdown();
    }
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
}

// TSC ticks in ~20 ms (calibrated on first use against QueryPerformanceCounter).
static uint64_t TscSlowHook() {
    static const uint64_t ticks = [] {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const uint64_t t0 = __rdtsc();
        do {
            QueryPerformanceCounter(&b);
        } while ((b.QuadPart - a.QuadPart) * 1000 < f.QuadPart * 2);  // 2 ms
        const uint64_t dt = __rdtsc() - t0;
        const double per_ms = (double)dt * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart) / 1000.0;
        return (uint64_t)(per_ms * 20.0);
    }();
    return ticks;
}

PLA_MOD_EXPORT pla_action nvk_pre(pla_ctx* ctx) {
    const uint64_t pc = g_guest.api->get_pc(ctx);
    uint32_t idx = 0;
    if (!PcFind(pc, &idx)) return PLA_CONTINUE;
    // Diagnostic count (FPS-1): plain relaxed store, no locked add (game threads recording at once shared these lines).
    g_calls[idx].store(g_calls[idx].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    g_guest.api->call_data(ctx)[3] = idx;
    std::unique_lock<std::mutex> cap;
    if (g_capture.active()) {
        cap = std::unique_lock<std::mutex>(g_capture.mu());
        g_capture.Call(ctx, pc);
    }
    if (!g_ready || !g_pre[idx]) {
        if (cap.owns_lock()) g_capture.End();
        if (g_ready && g_cfg.exclusive && g_skip_unhandled[idx]) {
            // Exclusive: the driver has no command memory; no CommandBuffer* call may reach it.
            if (!g_warned[idx].exchange(true)) NVK_WARN("exclusive: %s not implemented (skipped)", kFns[idx].name);
            SetX(ctx, 0, 0);
            return PLA_SKIP;
        }
        return PLA_CONTINUE;
    }
    const uint64_t t_hook = __rdtsc();
    const pla_action a = g_pre[idx](ctx);
    if (__rdtsc() - t_hook > TscSlowHook()) {
        // X2 (sprint 6): hooks that block the game thread (not counted as "submit CPU"). FPS-1: TSC instead of two
        // steady_clock reads per NVN call; the threshold is calibrated once (~20 ms).
        const double ms = (double)(__rdtsc() - t_hook) * 20.0 / (double)TscSlowHook();
        {
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 300) NVK_INFO("slow hook: %s %.1f ms", kFns[idx].name, ms);
        }
    }
    if (cap.owns_lock()) {
        g_capture.End();
        if (idx == FN_nvnQueuePresentTexture) g_capture.Frame(g_stats.frames.load());
        else if (idx == FN_nvnQueueSubmitCommands) g_capture.FlushNow();
    }
    if (idx == FN_nvnQueuePresentTexture) {
        const uint64_t f = g_stats.frames.load();
        if (f - g_last_report_frame >= 600) {
            g_last_report_frame = f;
            const auto t_rep = std::chrono::steady_clock::now();
            if (g_log_async.load()) {
                g_report_req = true;  // done by the log thread, not the game's
                g_log_cv.notify_one();
            } else {
                Report();
                PipelineReport();
            }
            std::vector<RecordingPtr> dead;
            // recording_max_age (default 0 = never): recordings not executed for 1200 frames used to be dropped, but the
            // game records each mount's commands once when the area loads and calls them (CallCommands) only when the
            // mount is summoned: after >12 s the mount's skinning did not run (frozen pose, sprint 9). Handles are
            // addresses in the game's command memory: their number is bounded and a reused handle replaces the old one.
            const size_t live = g_cfg.recording_max_age ? SweepRecordings(f, g_cfg.recording_max_age, g_log_async.load() ? &dead : nullptr)
                                                        : SweepRecordings(f, ~0ull >> 1, nullptr);
            if (!dead.empty()) {
                std::lock_guard<std::mutex> lk(g_log_mu);
                for (auto& r : dead) g_log_graveyard.push_back(std::move(r));  // freed by the log thread
            }
            NVK_DBG("live recordings: %zu", live);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_rep).count();
            if (ms > 20) NVK_INFO("slow periodic report: %.1f ms (live recordings %zu)", ms, live);
        }
    }
    return a;
}

PLA_MOD_EXPORT void nvk_post(pla_ctx* ctx) {
    const uint64_t idx = g_guest.api->call_data(ctx)[3];
    if (!g_ready || idx >= FN_COUNT || !g_post[idx]) return;
    std::unique_lock<std::mutex> cap;
    if (g_capture.active()) {
        cap = std::unique_lock<std::mutex>(g_capture.mu());
        g_capture.Post(ctx, g_guest.sdk_base + kFns[idx].offset);
    }
    g_post[idx](ctx);
    if (cap.owns_lock()) g_capture.End();
}

}  // extern "C"
