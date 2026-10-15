// nvntrace: pla-mods runtime mod that logs every call the game makes to the NVN API.
//
// Hooks the NVN driver functions inside `sdk` (docs/port/data/nvn_sdk_map.csv) with a `pre`
// that returns PLA_CONTINUE (and a `post` where the return value is needed). It changes nothing
// in the game's behavior. Writes a binary file (format in trace_format.h) read by
// tools/nvntrace_view.py.
//
// Config ("config" object of mod.json):
//   trace_dir      output folder (created)
//   max_mb         file cap; once reached, records stop being written (counters continue)
//   detail_frames  frame ranges "a-b,c-d" with full detail (CommandBuffer and setters);
//                  outside them only object functions are logged (Initialize,
//                  Finalize, Register, Map, SetShaders, Queue, Window, Sync...)
//
// SPDX-License-Identifier: MIT
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pla_mod_api.h"
#include "trace_format.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static uint32_t host_tid() { return GetCurrentThreadId(); }
#else
#include <pthread.h>
static uint32_t host_tid() { return (uint32_t)(uintptr_t)pthread_self(); }
#endif

namespace {

enum : uint32_t { F_POST = 1, F_DUMPARGS = 2, F_DUMPOBJ = 4, F_USED = 8, F_SETTER = 16, F_HOT = 32 };
inline unsigned nargs(uint32_t f) { unsigned n = (f >> 8) & 0xF; return n == 0xF ? 4u : n; }

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
constexpr uint32_t kNumFns = sizeof(kFns) / sizeof(kFns[0]);

// Indices the tracer handles specially (resolved by name at init).
struct Special {
    int present = -1, pool_init = -1, pool_get_addr = -1, set_shaders = -1, set_shaders_ext = -1,
        acquire = -1;
} g_sp;

const pla_api* g_api = nullptr;
uint64_t g_sdk_base = 0;
std::unordered_map<uint64_t, uint32_t> g_by_pc;  // guest address -> index

std::mutex g_lock;
std::FILE* g_file = nullptr;
std::vector<uint8_t> g_buf;
uint64_t g_written = 0;
uint64_t g_max_bytes = 1500ull << 20;
bool g_full = false;
std::string g_dir;
std::string g_base_name;

std::atomic<uint64_t> g_seq{0};
std::atomic<uint32_t> g_frame{0};
std::atomic<uint64_t> g_counts[kNumFns];
std::vector<std::pair<uint32_t, uint32_t>> g_detail;  // [a, b] inclusive

std::unordered_map<uint64_t, uint64_t> g_blob_hash;  // address -> hash of the last dump
struct Pool {
    uint64_t cpu = 0, size = 0, gpu = 0;
    uint32_t flags = 0;
};
std::unordered_map<uint64_t, Pool> g_pools;  // MemoryPool object -> data
std::unordered_set<uint64_t> g_code_dumped;

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

bool detail(uint32_t frame) {
    for (auto& r : g_detail)
        if (frame >= r.first && frame <= r.second) return true;
    return false;
}

void flush_locked() {
    if (g_file && !g_buf.empty()) {
        std::fwrite(g_buf.data(), 1, g_buf.size(), g_file);
        g_written += g_buf.size();
        g_buf.clear();
    }
    if (g_written >= g_max_bytes && !g_full) {
        g_full = true;
        if (g_api) g_api->log(PLA_LOG_WARNING, "nvntrace: max_mb reached, stopping writes");
    }
}

// Writes a record (header + payload) with the lock held.
void put_locked(uint8_t type, uint16_t fn, const void* payload, uint32_t size, const void* extra = nullptr,
                uint32_t extra_size = 0) {
    if (g_full || !g_file) return;
    nvt::RecHeader h{};
    uint32_t pad = (8 - ((sizeof h + size + extra_size) & 7)) & 7;
    h.size = (uint32_t)(sizeof h + size + extra_size + pad);
    h.type = type;
    h.fn = fn;
    h.tid = host_tid();
    h.frame = g_frame.load(std::memory_order_relaxed);
    h.seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&h);
    g_buf.insert(g_buf.end(), hp, hp + sizeof h);
    if (size) g_buf.insert(g_buf.end(), (const uint8_t*)payload, (const uint8_t*)payload + size);
    if (extra_size) g_buf.insert(g_buf.end(), (const uint8_t*)extra, (const uint8_t*)extra + extra_size);
    g_buf.insert(g_buf.end(), pad, 0);
    if (g_buf.size() >= (4u << 20)) flush_locked();
}

bool plausible_ptr(uint64_t v) { return v >= 0x1000000ull && v < (1ull << 39) && (v & 3) == 0; }

// Dumps [va, va+len) if it changed since the last dump of that address (or always with force).
void dump_locked(pla_ctx* ctx, uint16_t fn, uint64_t va, uint32_t len, uint32_t kind, bool force = false) {
    if (g_full || !len) return;
    std::vector<uint8_t> tmp(len);
    size_t got = g_api->read_mem(ctx, va, tmp.data(), len);
    if (!got) return;
    tmp.resize(got);
    uint64_t h = fnv1a(tmp.data(), got) ^ ((uint64_t)got << 48) ^ kind;
    auto it = g_blob_hash.find(va);
    if (!force && it != g_blob_hash.end() && it->second == h) return;
    g_blob_hash[va] = h;
    nvt::BlobHeader b{};
    b.addr = va;
    b.len = (uint32_t)got;
    b.kind = kind;
    put_locked(nvt::REC_BLOB, fn, &b, sizeof b, tmp.data(), (uint32_t)got);
}

void note_locked(const std::string& s) { put_locked(nvt::REC_NOTE, 0xFFFF, s.data(), (uint32_t)s.size()); }

void write_counts() {
    if (g_dir.empty()) return;
    std::string p = g_dir + "/" + g_base_name + ".counts.csv";
    std::FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "name,index,sdk_offset,calls,used\n");
    for (uint32_t i = 0; i < kNumFns; ++i)
        std::fprintf(f, "%s,%u,0x%x,%llu,%u\n", kFns[i].name, i, kFns[i].offset,
                     (unsigned long long)g_counts[i].load(), (kFns[i].flags & F_USED) ? 1u : 0u);
    std::fprintf(f, "#frames,%u\n", g_frame.load());
    std::fclose(f);
}

// ShaderData = {u64 data (GPU address), u64 control}; ShaderDataExt adds a u64 (24 bytes).
void shaders_locked(pla_ctx* ctx, uint16_t fn, uint64_t count, uint64_t data, unsigned stride_q) {
    if (count == 0 || count > 8 || !plausible_ptr(data)) return;
    uint64_t sd[24] = {};
    g_api->read_mem(ctx, data, sd, count * stride_q * 8);
    dump_locked(ctx, fn, data, (uint32_t)(count * stride_q * 8), nvt::BLOB_ARG, true);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t gpu = sd[i * stride_q], control = sd[i * stride_q + 1];
        if (plausible_ptr(control)) dump_locked(ctx, fn, control, 0x1000, nvt::BLOB_SHADER_CONTROL);
        if (g_code_dumped.count(gpu)) continue;
        for (auto& [obj, p] : g_pools) {
            if (p.gpu && gpu >= p.gpu && gpu < p.gpu + p.size) {
                uint64_t off = gpu - p.gpu;
                uint64_t len = p.size - off;
                if (len > 0x20000) len = 0x20000;
                std::vector<uint8_t> tmp(len);
                size_t got = g_api->read_mem(ctx, p.cpu + off, tmp.data(), len);
                nvt::BlobHeader b{};
                b.addr = gpu;
                b.len = (uint32_t)got;
                b.kind = nvt::BLOB_SHADER_CODE;
                put_locked(nvt::REC_BLOB, fn, &b, sizeof b, tmp.data(), (uint32_t)got);
                g_code_dumped.insert(gpu);
                break;
            }
        }
    }
}

std::string cfg_str(const char* json, const char* key, const char* def) {
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
    return j.substr(p, e - p);
}

}  // namespace

extern "C" {

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* api, const char* config_json) {
    g_api = api;
    g_sdk_base = api->module_base("sdk");
    if (!g_sdk_base) {
        api->log(PLA_LOG_ERROR, "nvntrace: no sdk base");
        return 1;
    }
    for (uint32_t i = 0; i < kNumFns; ++i) {
        g_by_pc[g_sdk_base + kFns[i].offset] = i;
        std::string n = kFns[i].name;
        if (n == "nvnQueuePresentTexture") g_sp.present = (int)i;
        if (n == "nvnMemoryPoolInitialize") g_sp.pool_init = (int)i;
        if (n == "nvnMemoryPoolGetBufferAddress") g_sp.pool_get_addr = (int)i;
        if (n == "nvnProgramSetShaders") g_sp.set_shaders = (int)i;
        if (n == "nvnProgramSetShadersExt") g_sp.set_shaders_ext = (int)i;
        if (n == "nvnWindowAcquireTexture") g_sp.acquire = (int)i;
    }
    g_dir = cfg_str(config_json, "trace_dir", "nvntrace");
    g_max_bytes = std::stoull(cfg_str(config_json, "max_mb", "1500")) << 20;
    std::string det = cfg_str(config_json, "detail_frames", "0-400");
    for (size_t p = 0; p < det.size();) {
        size_t e = det.find(',', p);
        std::string r = det.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t d = r.find('-');
        uint32_t a = (uint32_t)std::stoul(r.substr(0, d));
        uint32_t b = d == std::string::npos ? a : (uint32_t)std::stoul(r.substr(d + 1));
        g_detail.emplace_back(a, b);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    char stamp[32];
    std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    g_base_name = std::string("nvn-") + stamp;
    std::string path = g_dir + "/" + g_base_name + ".nvt";
    g_file = std::fopen(path.c_str(), "wb");
    if (!g_file) {
        api->log(PLA_LOG_ERROR, ("nvntrace: could not open " + path).c_str());
        return 1;
    }
    g_buf.reserve(5u << 20);
    // File header
    nvt::FileHeader fh{};
    std::memcpy(fh.magic, nvt::kMagic, 8);
    fh.version = nvt::kVersion;
    fh.num_fns = kNumFns;
    fh.sdk_base = g_sdk_base;
    fh.main_base = api->module_base("main");
    std::lock_guard<std::mutex> lk(g_lock);
    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&fh);
    g_buf.insert(g_buf.end(), hp, hp + sizeof fh);
    for (uint32_t i = 0; i < kNumFns; ++i) {
        uint16_t n = (uint16_t)std::strlen(kFns[i].name);
        g_buf.insert(g_buf.end(), (uint8_t*)&n, (uint8_t*)&n + 2);
        g_buf.insert(g_buf.end(), kFns[i].name, kFns[i].name + n);
    }
    while (g_buf.size() & 7) g_buf.push_back(0);
    flush_locked();
    api->log(PLA_LOG_INFO, ("nvntrace: writing " + path).c_str());
    return 0;
}

PLA_MOD_EXPORT void pla_mod_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    note_locked("shutdown");
    flush_locked();
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    write_counts();
}

PLA_MOD_EXPORT pla_action nvt_pre(pla_ctx* ctx) {
    const uint64_t pc = g_api->get_pc(ctx);
    auto it = g_by_pc.find(pc);
    if (it == g_by_pc.end()) return PLA_CONTINUE;
    const uint32_t idx = it->second;
    const FnInfo& fi = kFns[idx];
    g_counts[idx].fetch_add(1, std::memory_order_relaxed);
    uint64_t* cd = g_api->call_data(ctx);
    for (int i = 0; i < 3; ++i) cd[i] = g_api->get_x(ctx, i);
    cd[3] = idx;

    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    const bool hot = (fi.flags & (F_SETTER | F_HOT)) != 0;
    const bool rec = !hot || detail(frame);
    if (rec) {
        nvt::CallRec c{};
        for (unsigned i = 0; i < 8; ++i) c.x[i] = g_api->get_x(ctx, i);
        for (unsigned i = 0; i < 4; ++i) {
            uint64_t v[2];
            g_api->get_v(ctx, i, v);
            c.v[i] = v[0];
        }
        c.lr = g_api->get_x(ctx, PLA_REG_LR);
        c.sp = g_api->get_sp(ctx);
        std::lock_guard<std::mutex> lk(g_lock);
        put_locked(nvt::REC_CALL, (uint16_t)idx, &c, sizeof c);
        if ((int)idx == g_sp.set_shaders || (int)idx == g_sp.set_shaders_ext) {
            shaders_locked(ctx, (uint16_t)idx, c.x[1], c.x[2], (int)idx == g_sp.set_shaders_ext ? 3u : 2u);
        } else if ((fi.flags & F_DUMPARGS) && !(fi.flags & F_SETTER)) {
            for (unsigned i = 1; i < nargs(fi.flags); ++i)
                if (plausible_ptr(c.x[i])) dump_locked(ctx, (uint16_t)idx, c.x[i], 0x100, nvt::BLOB_ARG);
        }
        if ((int)idx == g_sp.pool_init && plausible_ptr(c.x[1])) {
            uint64_t b[3] = {};
            g_api->read_mem(ctx, c.x[1] + 0x10, b, sizeof b);
            Pool p;
            p.cpu = b[0];
            p.size = b[1];
            p.flags = (uint32_t)b[2];
            g_pools[c.x[0]] = p;
        }
    }
    if ((int)idx == g_sp.present) {
        uint32_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        std::lock_guard<std::mutex> lk(g_lock);
        put_locked(nvt::REC_FRAME, (uint16_t)idx, nullptr, 0);
        if (f % 600 == 0) {
            flush_locked();
            if (g_file) std::fflush(g_file);
            write_counts();
        }
    }
    return PLA_CONTINUE;
}

PLA_MOD_EXPORT void nvt_post(pla_ctx* ctx) {
    // In the post the PC is already the return address: call_data = {X0, X1, X2, index}.
    const uint64_t* cd = g_api->call_data(ctx);
    const uint32_t idx = (uint32_t)cd[3];
    if (idx >= kNumFns) return;
    const FnInfo& fi = kFns[idx];
    const uint64_t ret = g_api->get_x(ctx, 0);
    if ((int)idx == g_sp.pool_get_addr) {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_pools.find(cd[0]);
        if (it != g_pools.end()) it->second.gpu = ret;
    }
    const bool hot = (fi.flags & (F_SETTER | F_HOT)) != 0;
    if (hot && !detail(g_frame.load(std::memory_order_relaxed))) return;
    nvt::RetRec r{};
    r.x0 = ret;
    uint64_t v[2];
    g_api->get_v(ctx, 0, v);
    r.v0 = v[0];
    std::lock_guard<std::mutex> lk(g_lock);
    put_locked(nvt::REC_RET, (uint16_t)idx, &r, sizeof r);
    if ((fi.flags & F_DUMPOBJ) && plausible_ptr(cd[0]))
        dump_locked(ctx, (uint16_t)idx, cd[0], 0x200, nvt::BLOB_OBJECT);
    if (std::strstr(fi.name, "Get") || (int)idx == g_sp.acquire) {
        for (unsigned i = 1; i < 3 && i < nargs(fi.flags); ++i)
            if (plausible_ptr(cd[i])) dump_locked(ctx, (uint16_t)idx, cd[i], 0x40, nvt::BLOB_OUT, true);
    }
}

}  // extern "C"
