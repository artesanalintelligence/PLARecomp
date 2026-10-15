// nvnvk_replay: replays an .nvkc capture (src/capture.h) with the same nvnvk.dll, without the game.
//
//   nvnvk_replay <capture.nvkc> [--dll build\port\nvnvk\nvnvk.dll] [--config '{"capture_dir":...}']
//                [--frames N] [--present] [--loop K] [--validation] [--quiet]
//
// Implements a fake pla_api: registers from the CALL/POST records, guest memory from the MEM records
// (sparse page image), call_data per guest thread. Calls the DLL's nvk_pre/nvk_post in captured
// order. The default config is shadow mode, no window, and a PPM dump every 300 frames in
// build\port\nvnvk\replay\.
// SPDX-License-Identifier: MIT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pla_mod_api.h"

namespace second_presenter {
void Start();
void Stop();
}  // namespace second_presenter

namespace {

#pragma pack(push, 1)
struct NvkcHeader {
    char magic[4];
    uint32_t version;
    uint64_t sdk_base, main_base;
};
#pragma pack(pop)
enum : uint8_t { kRecCall = 1, kRecPost = 2, kRecBlob = 3, kRecMem = 4, kRecEnd = 5, kRecFrame = 6 };

struct Ctx {
    uint64_t x[31] = {};
    uint64_t v[32][2] = {};
    uint64_t pc = 0;
    uint64_t cd[4] = {};
};

NvkcHeader g_hdr;
std::unordered_map<uint32_t, std::unique_ptr<Ctx>> g_ctx;
std::unordered_map<uint64_t, std::unique_ptr<uint8_t[]>> g_pages;  // 4 KiB page -> data
std::unordered_map<uint64_t, std::vector<uint8_t>> g_blobs;
bool g_quiet = false;
thread_local std::vector<uint8_t> t_scratch;

uint8_t* Page(uint64_t va, bool create) {
    const uint64_t pg = va >> 12;
    auto it = g_pages.find(pg);
    if (it != g_pages.end()) return it->second.get();
    if (!create) return nullptr;
    auto p = std::make_unique<uint8_t[]>(4096);
    std::memset(p.get(), 0, 4096);
    uint8_t* raw = p.get();
    g_pages.emplace(pg, std::move(p));
    return raw;
}

void MemWrite(uint64_t va, const uint8_t* src, size_t n) {
    while (n) {
        const size_t off = va & 4095, chunk = std::min<size_t>(n, 4096 - off);
        std::memcpy(Page(va, true) + off, src, chunk);
        va += chunk;
        src += chunk;
        n -= chunk;
    }
}

void MemRead(uint64_t va, uint8_t* dst, size_t n) {
    while (n) {
        const size_t off = va & 4095, chunk = std::min<size_t>(n, 4096 - off);
        if (const uint8_t* p = Page(va, false))
            std::memcpy(dst, p + off, chunk);
        else
            std::memset(dst, 0, chunk);
        va += chunk;
        dst += chunk;
        n -= chunk;
    }
}

// ---------------------------------------------------------------- fake pla_api
uint64_t GetX(pla_ctx* c, unsigned n) { return n < 31 ? reinterpret_cast<Ctx*>(c)->x[n] : 0; }
void SetX(pla_ctx* c, unsigned n, uint64_t v) {
    if (n < 31) reinterpret_cast<Ctx*>(c)->x[n] = v;
}
uint64_t GetSp(pla_ctx*) { return 0; }
void SetSp(pla_ctx*, uint64_t) {}
uint64_t GetPc(pla_ctx* c) { return reinterpret_cast<Ctx*>(c)->pc; }
void SetPc(pla_ctx*, uint64_t) {}
void GetV(pla_ctx* c, unsigned n, uint64_t out[2]) {
    out[0] = n < 32 ? reinterpret_cast<Ctx*>(c)->v[n][0] : 0;
    out[1] = n < 32 ? reinterpret_cast<Ctx*>(c)->v[n][1] : 0;
}
void SetV(pla_ctx* c, unsigned n, uint64_t lo, uint64_t hi) {
    if (n < 32) reinterpret_cast<Ctx*>(c)->v[n][0] = lo, reinterpret_cast<Ctx*>(c)->v[n][1] = hi;
}
size_t ReadMem(pla_ctx*, uint64_t va, void* dst, size_t n) {
    MemRead(va, static_cast<uint8_t*>(dst), n);
    return n;
}
size_t WriteMem(pla_ctx*, uint64_t va, const void* src, size_t n) {
    MemWrite(va, static_cast<const uint8_t*>(src), n);
    return n;
}
uint64_t ModuleBase(const char* m) {
    if (!std::strcmp(m, "sdk")) return g_hdr.sdk_base;
    if (!std::strcmp(m, "main")) return g_hdr.main_base;
    return 0;
}
uint64_t* CallData(pla_ctx* c) { return reinterpret_cast<Ctx*>(c)->cd; }
void Log(int level, const char* msg) {
    if (!g_quiet || level >= PLA_LOG_WARNING) std::printf("[mod %d] %s\n", level, msg);
}
void* HostPointer(uint64_t va, size_t n) {
    // Copy to a temporary buffer: the backend only reads (or writes and it is lost, as in shadow).
    if (t_scratch.size() < n) t_scratch.resize(n);
    MemRead(va, t_scratch.data(), n);
    return t_scratch.data();
}
// --fault-arena: host_pointer returns reserved no-access memory (like a page suyu protects with
// use_reactive_flushing) and there is no host_backing_pointer; forces the trapped-fault path.
bool g_fault_arena = false;
uint8_t* g_noaccess = nullptr;
void* HostPointerFault(uint64_t, size_t n) {
    if (!g_noaccess)
        g_noaccess = static_cast<uint8_t*>(VirtualAlloc(nullptr, 128ull << 20, MEM_RESERVE, PAGE_NOACCESS));
    return n <= (128ull << 20) ? g_noaccess : nullptr;
}
void* NullWindow() { return nullptr; }
uint64_t Ticks() { return 0; }

template <class T>
bool Get(std::FILE* f, T* v) {
    return std::fread(v, sizeof(T), 1, f) == 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: nvnvk_replay <capture.nvkc> [--dll path] [--config json] [--frames N] [--present] "
                             "[--validation] [--every N] [--out dir] [--quiet] [--fault-arena]\n");
        return 2;
    }
    std::string path = argv[1], dll = "build\\port\\nvnvk\\nvnvk.dll", extra, out = "build\\port\\nvnvk\\replay";
    uint64_t max_frames = 0;
    bool present = false, validation = false, second = false;
    int every = 300;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--dll") dll = next();
        else if (a == "--config") extra = next();
        else if (a == "--frames") max_frames = std::stoull(next());
        else if (a == "--present") present = true;
        else if (a == "--validation") validation = true;
        else if (a == "--every") every = std::stoi(next());
        else if (a == "--out") out = next();
        else if (a == "--quiet") g_quiet = true;
        else if (a == "--fault-arena") g_fault_arena = true;
        else if (a == "--second-presenter") second = true;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 1;
    }
    std::setvbuf(f, nullptr, _IOFBF, 16 << 20);
    if (!Get(f, &g_hdr) || std::memcmp(g_hdr.magic, "NVKC", 4) != 0) {
        std::fprintf(stderr, "not an NVKC capture\n");
        return 1;
    }
    HMODULE h = LoadLibraryA(dll.c_str());
    if (!h) {
        std::fprintf(stderr, "cannot load %s (%lu)\n", dll.c_str(), GetLastError());
        return 1;
    }
    auto init = reinterpret_cast<int (*)(const pla_api*, const char*)>(GetProcAddress(h, "pla_mod_init"));
    auto shutdown = reinterpret_cast<void (*)()>(GetProcAddress(h, "pla_mod_shutdown"));
    auto pre = reinterpret_cast<pla_action (*)(pla_ctx*)>(GetProcAddress(h, "nvk_pre"));
    auto post = reinterpret_cast<void (*)(pla_ctx*)>(GetProcAddress(h, "nvk_post"));
    if (!init || !pre || !post) {
        std::fprintf(stderr, "the DLL does not export pla_mod_init/nvk_pre/nvk_post\n");
        return 1;
    }
    static pla_api api{};
    api.version = PLA_MOD_API_VERSION;
    api.size = sizeof(pla_api);
    api.get_x = GetX;
    api.set_x = SetX;
    api.get_sp = GetSp;
    api.set_sp = SetSp;
    api.get_pc = GetPc;
    api.set_pc = SetPc;
    api.get_v = GetV;
    api.set_v = SetV;
    api.read_mem = ReadMem;
    api.write_mem = WriteMem;
    api.module_base = ModuleBase;
    api.call_data = CallData;
    api.log = Log;
    api.host_pointer = g_fault_arena ? HostPointerFault : HostPointer;
    api.host_backing_pointer = g_fault_arena ? nullptr : HostPointer;
    api.window_handle = NullWindow;
    api.guest_ticks = Ticks;
    for (char& c : out)
        if (c == '\\') c = '/';
    // --config keys go first: nvnvk takes the first occurrence of each key.
    std::string cfg = "{";
    if (!extra.empty()) {
        std::string e = extra;
        while (!e.empty() && (e.front() == '{' || e.front() == ' ')) e.erase(e.begin());
        while (!e.empty() && (e.back() == '}' || e.back() == ' ')) e.pop_back();
        if (!e.empty()) cfg += e + ", ";
    }
    cfg += "\"mode\": \"shadow\", \"present\": " + std::string(present ? "true" : "false") +
                      ", \"validation\": " + (validation ? "true" : "false") + ", \"capture_dir\": \"" + out +
                      "\", \"capture_every\": " + std::to_string(every) +
                      ", \"log_dir\": \"" + out + "\", \"shader_cache_dir\": \"D:/PLARecomp/cache/nvnvk-shaders\"" +
                      ", \"pipeline_cache\": \"build/port/nvnshader/prewarm/pipelines.bin\", \"import_memory\": false, \"replay\": true";
    cfg += "}";
    if (second) second_presenter::Start();
    if (init(&api, cfg.c_str()) != 0) {
        std::fprintf(stderr, "pla_mod_init failed\n");
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t calls = 0, posts = 0, frames = 0, mem_bytes = 0;
    std::vector<std::pair<uint64_t, uint64_t>> pending;  // (va, blob) of the current group
    std::vector<uint32_t> pend_size;
    uint8_t kind = 0, rec = 0;
    Ctx* cur = nullptr;
    auto apply = [&]() {
        for (size_t i = 0; i < pending.size(); ++i) {
            auto it = g_blobs.find(pending[i].second);
            if (it == g_blobs.end()) continue;
            MemWrite(pending[i].first, it->second.data(), std::min<size_t>(pend_size[i], it->second.size()));
            mem_bytes += pend_size[i];
        }
        pending.clear();
        pend_size.clear();
    };
    bool stop = false;
    while (!stop && Get(f, &rec)) {
        switch (rec) {
            case kRecCall:
            case kRecPost: {
                uint32_t tid;
                uint64_t pc;
                Get(f, &tid);
                Get(f, &pc);
                auto& c = g_ctx[tid];
                if (!c) c = std::make_unique<Ctx>();
                cur = c.get();
                cur->pc = pc;
                if (rec == kRecCall) {
                    uint64_t x[8], v[4];
                    std::fread(x, 8, 8, f);
                    std::fread(v, 8, 4, f);
                    std::memcpy(cur->x, x, sizeof x);
                    for (int i = 0; i < 4; ++i) cur->v[i][0] = v[i], cur->v[i][1] = 0;
                } else {
                    uint64_t x[2];
                    std::fread(x, 8, 2, f);
                    cur->x[0] = x[0];
                    cur->x[1] = x[1];
                }
                kind = rec;
                break;
            }
            case kRecBlob: {
                uint64_t id;
                uint32_t sz;
                Get(f, &id);
                Get(f, &sz);
                std::vector<uint8_t> d(sz);
                if (sz && std::fread(d.data(), 1, sz, f) != sz) stop = true;
                g_blobs[id] = std::move(d);
                break;
            }
            case kRecMem: {
                uint64_t va, blob;
                uint32_t sz;
                Get(f, &va);
                Get(f, &sz);
                Get(f, &blob);
                pending.emplace_back(va, blob);
                pend_size.push_back(sz);
                break;
            }
            case kRecEnd:
                apply();
                if (cur) {
                    if (kind == kRecCall) {
                        pre(reinterpret_cast<pla_ctx*>(cur));
                        ++calls;
                    } else {
                        post(reinterpret_cast<pla_ctx*>(cur));
                        ++posts;
                    }
                }
                cur = nullptr;
                break;
            case kRecFrame: {
                uint64_t n;
                Get(f, &n);
                ++frames;
                if (!g_quiet && frames % 100 == 0) {
                    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::printf("frame %llu (%.1f s, %.1f frames/s)\n", (unsigned long long)frames, s, frames / s);
                }
                if (max_frames && frames >= max_frames) stop = true;
                break;
            }
            default:
                std::fprintf(stderr, "unknown record %u at %lld\n", rec, (long long)_ftelli64(f));
                stop = true;
                break;
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("replayed: %llu calls, %llu posts, %llu frames in %.2f s (%.1f frames/s), %.1f MB of memory applied, "
                "%zu pages\n",
                (unsigned long long)calls, (unsigned long long)posts, (unsigned long long)frames, s, frames / s,
                mem_bytes / 1048576.0, g_pages.size());
    if (second) second_presenter::Stop();
    if (shutdown) shutdown();
    std::fclose(f);
    return 0;
}
