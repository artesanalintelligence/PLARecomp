// SPDX-FileCopyrightText: 2026 PLARecomp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port of the fork's native-mod loader (core/arm/recomp/recomp_mods.cpp and recomp_native.cpp,
// PLARecomp code) to this host: same mod.json format, hook model and pla_api; the kernel
// events behind pla_api are this host's own.

#include "mods.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "pla_mod_api.h"

#include "host.h"
#include "json.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "recomp.h"

namespace plahost::mods {
namespace {

GuestContext* View(pla_ctx* c) {
    return reinterpret_cast<GuestContext*>(c);
}

struct Mod {
    std::string id;
    std::filesystem::path dir;
    HMODULE lib = nullptr;
    void (*shutdown)() = nullptr;
};

struct Handler {
    pla_pre_fn pre = nullptr;
    pla_post_fn post = nullptr;
    const Mod* mod = nullptr;
};

struct Site {
    std::string label;
    BlockFn original = nullptr;
    BlockFn* slot = nullptr;
    std::vector<Handler> handlers;
    bool any_post = false;
    std::atomic<u64> calls{0};
};

struct Frame {
    u64 ret = 0;
    u64 sp = 0;
    const Site* site = nullptr;
    u64 data[4]{};
};

std::vector<std::unique_ptr<Mod>> g_mods;
std::unordered_map<u64, std::unique_ptr<Site>> g_sites;
std::atomic<bool> g_retired{false};
// nvnvk paces the frames itself (fps_limit, changed live from its menu); the host then does not add its own cap.
bool g_renderer_paces = false;
std::mutex g_frames_lock;
std::unordered_map<u64, std::vector<Frame>> g_frames;
thread_local const Mod* t_current_mod = nullptr;
thread_local u64* t_call_data = nullptr;

// ---- pla_api ----

u64 ApiGetX(pla_ctx* c, unsigned n) { return n <= 30 ? View(c)->x[n] : 0; }
void ApiSetX(pla_ctx* c, unsigned n, u64 v) { if (n <= 30) View(c)->x[n] = v; }
u64 ApiGetSp(pla_ctx* c) { return View(c)->x[31]; }
void ApiSetSp(pla_ctx* c, u64 v) { View(c)->x[31] = v; }
u64 ApiGetPc(pla_ctx* c) { return View(c)->pc; }
void ApiSetPc(pla_ctx* c, u64 v) { View(c)->pc = v; }
void ApiGetV(pla_ctx* c, unsigned n, u64 out[2]) {
    if (n < 32) {
        out[0] = View(c)->vreg[n][0];
        out[1] = View(c)->vreg[n][1];
    } else {
        out[0] = out[1] = 0;
    }
}
void ApiSetV(pla_ctx* c, unsigned n, u64 lo, u64 hi) {
    if (n < 32) {
        View(c)->vreg[n][0] = lo;
        View(c)->vreg[n][1] = hi;
    }
}
size_t ApiReadMem(pla_ctx*, u64 va, void* dst, size_t size) { return mem::Read(va, dst, size); }
size_t ApiWriteMem(pla_ctx*, u64 va, const void* src, size_t size) { return mem::Write(va, src, size); }
u64 ApiModuleBase(const char* name) {
    if (!name) return 0;
    const recomp::Image* im = recomp::FindImage(name);
    return im ? im->base : 0;
}
u64* ApiCallData(pla_ctx*) {
    thread_local u64 scratch[4];
    return t_call_data ? t_call_data : scratch;
}
void ApiLog(int level, const char* message) {
    const char* id = t_current_mod ? t_current_mod->id.c_str() : "mod";
    const log::Level l = level <= PLA_LOG_TRACE   ? log::Trace
                         : level == PLA_LOG_DEBUG ? log::Debug
                         : level == PLA_LOG_INFO  ? log::Info
                         : level == PLA_LOG_WARNING ? log::Warning
                                                  : log::Error;
    log::Raw(l, "Mod", std::string{"["} + id + "] " + (message ? message : ""));
}

// Events: ids -> kernel events (the same objects the game's handles refer to).
std::mutex g_events_lock;
std::unordered_map<u64, std::shared_ptr<kernel::Event>> g_events;
u64 g_next_event = 1;

std::shared_ptr<kernel::Event> FindEvent(u64 id) {
    std::scoped_lock lk{g_events_lock};
    const auto it = g_events.find(id);
    return it == g_events.end() ? nullptr : it->second;
}
u64 Register(std::shared_ptr<kernel::Event> ev) {
    std::scoped_lock lk{g_events_lock};
    const u64 id = g_next_event++;
    g_events[id] = std::move(ev);
    return id;
}
u64 ApiEventCreate(const char* name) { return Register(kernel::NewEvent(name ? name : "pla_api")); }
u32 ApiEventGuestHandle(u64 id) {
    auto ev = FindEvent(id);
    return ev ? kernel::ReadableHandle(ev) : 0;
}
u64 ApiEventFromGuestHandle(u32 handle) {
    auto ev = kernel::Get<kernel::Event>(handle);
    return ev ? Register(ev) : 0;
}
int ApiEventSignal(u64 id) {
    auto ev = FindEvent(id);
    if (!ev) return -1;
    kernel::Signal(*ev);
    return 0;
}
int ApiEventClear(u64 id) {
    auto ev = FindEvent(id);
    if (!ev) return -1;
    kernel::Clear(*ev);
    return 0;
}
int ApiEventWait(u64 id, int64_t timeout_ns) {
    auto ev = FindEvent(id);
    if (!ev) return -1;
    return kernel::WaitHost(*ev, timeout_ns);
}
void ApiEventDestroy(u64 id) {
    std::scoped_lock lk{g_events_lock};
    g_events.erase(id);
}
void* ApiHostPointer(u64 va, size_t size) { return mem::HostPointer(va, size); }
void* ApiBackingPointer(u64 va, size_t size) { return mem::BackingPointer(va, size); }
void* ApiWindow() { return host::WindowHandle(); }
u64 ApiGuestTicks() { return kernel::GuestTicks(); }

void PaceFrame() {
    using clock = std::chrono::steady_clock;
    const int cap = host::Opts().fps_cap;
    if (cap <= 0 || g_renderer_paces) return;
    static clock::time_point next{};
    const auto period = std::chrono::nanoseconds{1'000'000'000LL / cap};
    auto now = clock::now();
    if (next.time_since_epoch().count() == 0 || now - next > period * 4) next = now;
    if (now < next) {
        const auto coarse = next - now - std::chrono::microseconds{1500};
        if (coarse > std::chrono::nanoseconds::zero()) std::this_thread::sleep_for(coarse);
        while (clock::now() < next) std::this_thread::yield();
    }
    next += period;
}
void ApiFramePresented() {
    PaceFrame();
    host::g_frames.fetch_add(1, std::memory_order_relaxed);
}
u64 ApiGuestAlloc(size_t size, void** host_ptr) {
    if (host_ptr) *host_ptr = nullptr;
    if (!size || size > (size_t{1} << 32)) return 0;
    const u64 va = mem::MapAnywhere(mem::kStackStart, mem::kStackSize, size, mem::Stack, mem::RW);
    if (va && host_ptr) *host_ptr = mem::Ptr(va);
    LOGI("Mods", "pla_api guest_alloc: %#zx bytes at %#llx", size, va);
    return va;
}
int ApiGuestFree(u64 va, size_t size) {
    return mem::Unmap(va, AlignUp(size, kPageSize)) == kOk ? 0 : -1;
}

const pla_api kApi{
    PLA_MOD_API_VERSION, static_cast<uint32_t>(sizeof(pla_api)),
    ApiGetX, ApiSetX, ApiGetSp, ApiSetSp, ApiGetPc, ApiSetPc, ApiGetV, ApiSetV, ApiReadMem, ApiWriteMem,
    ApiModuleBase, ApiCallData, ApiLog,
    ApiEventCreate, ApiEventGuestHandle, ApiEventFromGuestHandle, ApiEventSignal, ApiEventClear, ApiEventWait,
    ApiEventDestroy, ApiHostPointer, ApiBackingPointer, ApiWindow, ApiGuestTicks,
    ApiFramePresented, ApiGuestAlloc, ApiGuestFree,
};

// ---- dispatch ----

void HookThunk(void* raw) {
    GuestContext* c = static_cast<GuestContext*>(raw);
    const auto it = g_sites.find(c->pc);
    if (it == g_sites.end() || g_retired.load(std::memory_order_acquire)) {
        c->halted = kHaltBreakpoint;
        return;
    }
    Site& site = *it->second;
    site.calls.fetch_add(1, std::memory_order_relaxed);
    Frame frame{};
    t_call_data = frame.data;
    for (const Handler& h : site.handlers) {
        if (!h.pre) continue;
        t_current_mod = h.mod;
        const pla_action action = h.pre(reinterpret_cast<pla_ctx*>(raw));
        t_current_mod = nullptr;
        if (action == PLA_SKIP) {
            t_call_data = nullptr;
            c->pc = c->x[30];
            return;
        }
        if (action == PLA_REDIRECT) {
            t_call_data = nullptr;
            return;
        }
    }
    t_call_data = nullptr;
    if (site.any_post) {
        frame.ret = c->x[30];
        frame.sp = c->x[31];
        frame.site = &site;
        std::scoped_lock lk{g_frames_lock};
        auto& stack = g_frames[c->tpidrro_el0];
        if (stack.size() < 4096) {
            stack.push_back(frame);
            c->x[30] = kReturnTrap;
        }
    }
    site.original(raw);
}

bool ParseU64(const json::Value& v, u64& out) {
    if (v.type == json::Value::Number || v.type == json::Value::String) {
        if (v.text.empty()) return false;
        char* end = nullptr;
        out = std::strtoull(v.text.c_str(), &end, 0);
        return end && *end == '\0';
    }
    return false;
}

bool Listed(const char* env, const std::string& id) {
    const char* v = std::getenv(env);
    if (!v || !*v) return false;
    std::string_view s{v};
    while (!s.empty()) {
        const auto comma = s.find(',');
        if (s.substr(0, comma) == id) return true;
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return false;
}

void ApplyOverrides(const std::string& id, json::Value& manifest, const std::string& spec) {
    std::string_view s{spec};
    while (!s.empty()) {
        const auto semi = s.find(';');
        const std::string_view item = s.substr(0, semi);
        s = semi == std::string_view::npos ? std::string_view{} : s.substr(semi + 1);
        const auto colon = item.find(':');
        const auto eq = item.find('=');
        if (colon == std::string_view::npos || eq == std::string_view::npos || eq < colon || item.substr(0, colon) != id) {
            continue;
        }
        const std::string key{item.substr(colon + 1, eq - colon - 1)};
        const std::string value{item.substr(eq + 1)};
        json::Value* node = manifest.find("config");
        if (!node || !node->is_object()) {
            json::Value obj;
            obj.type = json::Value::Object;
            node = &manifest.set("config", obj);
        }
        std::string_view k{key};
        while (true) {
            const auto dot = k.find('.');
            const std::string part{k.substr(0, dot)};
            if (dot == std::string_view::npos) {
                node->set(part, json::FromLiteralOrString(value));
                break;
            }
            json::Value* child = node->find(part);
            if (!child || !child->is_object()) {
                json::Value obj;
                obj.type = json::Value::Object;
                child = &node->set(part, obj);
            }
            node = child;
            k.remove_prefix(dot + 1);
        }
        LOGI("Mods", "%s: config %s = %s", id.c_str(), key.c_str(), value.c_str());
    }
}

struct Pending {
    std::string label;
    u64 address = 0;
    BlockFn* slot = nullptr;
    Handler handler;
};

void LoadMod(const std::filesystem::path& dir, u64 title_id, const std::string& main_build_id,
             std::vector<Pending>& hooks) {
    std::ifstream in(dir / "mod.json", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    json::Value manifest;
    std::string err;
    const std::string where = host::Utf8(dir.wstring());
    if (!json::Parse(ss.str(), manifest, &err) || !manifest.is_object()) {
        LOGE("Mods", "%s: mod.json is invalid: %s", where.c_str(), err.c_str());
        return;
    }
    auto mod = std::make_unique<Mod>();
    mod->id = manifest.str("id", host::Utf8(dir.filename().wstring()));
    mod->dir = dir;
    const bool required = mod->id == "nvnvk" || mod->id == "nnshim"; // renderer and platform layer
    if (!required && std::getenv("PLAHOST_REQUIRED_MODS_ONLY")) {
        LOGI("Mods", "%s skipped (mods=0)", mod->id.c_str());
        return;
    }
    // The platform layer and the renderer are not optional here: load them even if marked disabled.
    const bool forced = Listed("PLA_RECOMP_MODS_FORCE", mod->id) || required;
    if ((!manifest.boolean("enabled", true) || std::filesystem::exists(dir / "disabled")) && !forced) {
        LOGI("Mods", "%s is disabled", mod->id.c_str());
        return;
    }
    if (mod->id == "nvnvk") {
        // nvnvk is this host's renderer: always exclusive. It also owns the FPS cap: PLA.ini/--fps-cap only seed
        // its fps_limit, so the in-game menu can change the cap live (a second, startup-only cap in the host
        // used to keep e.g. 30 FPS after the menu was set to unlimited).
        ApplyOverrides(mod->id, manifest, "nvnvk:mode=exclusive");
        if (host::Opts().fps_cap_set)
            ApplyOverrides(mod->id, manifest, "nvnvk:fps_limit=" + std::to_string(host::Opts().fps_cap));
        g_renderer_paces = true;
    }
    if (const char* set = std::getenv("PLA_RECOMP_MOD_SET"); set && *set) {
        ApplyOverrides(mod->id, manifest, set);
    }
    if (const json::Value* t = manifest.find("title_id")) {
        u64 title = 0;
        std::string s = t->text;
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
        title = std::strtoull(s.c_str(), nullptr, 16);
        if (title != title_id) {
            LOGE("Mods", "%s is for title %s, not %016llX", mod->id.c_str(), t->text.c_str(), title_id);
            return;
        }
    }
    const std::string library = manifest.str("library");
    const json::Value* hook_list = manifest.find("hooks");
    if (library.empty() || !hook_list || !hook_list->is_array()) {
        LOGE("Mods", "%s: mod.json needs \"library\" and \"hooks\"", mod->id.c_str());
        return;
    }
    struct Wanted { std::string label, pre, post; u64 address; BlockFn* slot; };
    std::vector<Wanted> wanted;
    for (const auto& h : hook_list->arr) {
        const std::string module = h.str("module", "main");
        u64 offset = 0;
        const json::Value* off = h.find("offset");
        if (!off || !ParseU64(*off, offset)) {
            LOGE("Mods", "%s: a hook has no valid \"offset\"", mod->id.c_str());
            return;
        }
        char label[96];
        std::snprintf(label, sizeof label, "%s+%#llx", module.c_str(), offset);
        const recomp::Image* im = recomp::FindImage(module);
        if (!im) {
            LOGE("Mods", "%s: %s - no recompiled image for module %s", mod->id.c_str(), label, module.c_str());
            return;
        }
        const u64 address = im->base + offset;
        if ((offset & 3) || address < im->lo || address > im->hi) {
            LOGE("Mods", "%s: %s is outside the image's code", mod->id.c_str(), label);
            return;
        }
        BlockFn* slot = im->index + ((address - im->lo) >> 2);
        if (!*slot) {
            LOGE("Mods", "%s: %s has no recompiled block", mod->id.c_str(), label);
            return;
        }
        if (const json::Value* e = h.find("expect")) {
            std::vector<u64> words;
            if (e->is_array()) {
                for (const auto& w : e->arr) {
                    u64 v;
                    if (!ParseU64(w, v)) { words.clear(); break; }
                    words.push_back(v);
                }
            } else {
                u64 v;
                if (ParseU64(*e, v)) words.push_back(v);
            }
            if (words.empty()) {
                LOGE("Mods", "%s: %s has an unreadable \"expect\"", mod->id.c_str(), label);
                return;
            }
            for (size_t k = 0; k < words.size(); ++k) {
                const u32 got = mem::IsMapped(address + 4 * k, 4) ? mem::Load<u32>(address + 4 * k) : 0;
                if (got != static_cast<u32>(words[k])) {
                    LOGE("Mods", "%s: %s expected %#010x at +%zu, found %#010x (different game version?); mod not loaded",
                         mod->id.c_str(), label, static_cast<u32>(words[k]), 4 * k, got);
                    return;
                }
            }
        }
        Wanted w{label, h.str("pre"), h.str("post"), address, slot};
        if (w.pre.empty() && w.post.empty()) {
            LOGE("Mods", "%s: %s names neither \"pre\" nor \"post\"", mod->id.c_str(), label);
            return;
        }
        wanted.push_back(std::move(w));
    }
    if (const json::Value* b = manifest.find("build_id"); b && b->is_string() && !main_build_id.empty()) {
        std::string want = b->text, have = main_build_id;
        for (auto& ch : want) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        // A manifest may give the first 20 bytes only.
        if (have.compare(0, want.size(), want) != 0) {
            LOGE("Mods", "%s is for build %s, main is %s; mod not loaded", mod->id.c_str(), b->text.c_str(), have.c_str());
            return;
        }
    }
    const auto lib_path = dir / host::Wide(library);
    mod->lib = LoadLibraryExW(lib_path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod->lib) {
        LOGE("Mods", "%s: cannot load %s (error %lu)", mod->id.c_str(), host::Utf8(lib_path.wstring()).c_str(), GetLastError());
        return;
    }
    const auto sym = [&](const std::string& n) {
        return reinterpret_cast<void*>(GetProcAddress(mod->lib, n.c_str()));
    };
    auto* version = reinterpret_cast<u32 (*)()>(sym("pla_mod_api_version"));
    if (!version || version() != PLA_MOD_API_VERSION) {
        LOGE("Mods", "%s: built for another mod API", mod->id.c_str());
        return;
    }
    std::vector<Pending> resolved;
    for (const auto& w : wanted) {
        Pending p{w.label, w.address, w.slot, {}};
        if (!w.pre.empty() && !(p.handler.pre = reinterpret_cast<pla_pre_fn>(sym(w.pre)))) {
            LOGE("Mods", "%s: %s does not export %s", mod->id.c_str(), library.c_str(), w.pre.c_str());
            return;
        }
        if (!w.post.empty() && !(p.handler.post = reinterpret_cast<pla_post_fn>(sym(w.post)))) {
            LOGE("Mods", "%s: %s does not export %s", mod->id.c_str(), library.c_str(), w.post.c_str());
            return;
        }
        resolved.push_back(p);
    }
    if (auto* init = reinterpret_cast<int (*)(const pla_api*, const char*)>(sym("pla_mod_init"))) {
        const json::Value* cfg = manifest.find("config");
        const std::string config = cfg ? cfg->dump() : std::string{"{}"};
        if (std::getenv("PLAHOST_LOG_MOD_CONFIG")) LOGI("Mods", "%s config: %s", mod->id.c_str(), config.substr(0, 600).c_str());
        t_current_mod = mod.get();
        const int rc = init(&kApi, config.c_str());
        t_current_mod = nullptr;
        if (rc != 0) {
            LOGE("Mods", "%s: pla_mod_init returned %d; mod not loaded", mod->id.c_str(), rc);
            return;
        }
    }
    mod->shutdown = reinterpret_cast<void (*)()>(sym("pla_mod_shutdown"));
    for (auto& p : resolved) {
        p.handler.mod = mod.get();
        hooks.push_back(p);
    }
    LOGI("Mods", "loaded %s (%s, %zu hook(s))", mod->id.c_str(), host::Utf8(lib_path.wstring()).c_str(), resolved.size());
    g_mods.push_back(std::move(mod));
}

} // namespace

void Activate(u64 title_id, const std::wstring& root, const std::string& main_build_id) {
    char tid[17];
    std::snprintf(tid, sizeof tid, "%016llX", title_id);
    const auto dir = std::filesystem::path(root) / tid;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        LOGW("Mods", "no mods folder %s", host::Utf8(dir.wstring()).c_str());
        return;
    }
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_directory() && std::filesystem::exists(e.path() / "mod.json")) dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    std::vector<Pending> hooks;
    for (const auto& d : dirs) {
        LoadMod(d, title_id, main_build_id, hooks);
    }
    for (const auto& p : hooks) {
        auto& site = g_sites[p.address];
        if (!site) {
            site = std::make_unique<Site>();
            site->label = p.label;
            site->slot = p.slot;
            site->original = *p.slot;
        }
        site->handlers.push_back(p.handler);
        site->any_post |= p.handler.post != nullptr;
    }
    for (auto& [pc, site] : g_sites) {
        *site->slot = &HookThunk;
    }
    LOGI("Mods", "%zu mod(s), %zu hooked address(es)", g_mods.size(), g_sites.size());
}

void Shutdown() {
    if (g_retired.exchange(true)) return;
    for (auto& m : g_mods) {
        if (m->shutdown) {
            t_current_mod = m.get();
            m->shutdown();
            t_current_mod = nullptr;
        }
    }
}

bool Retired() {
    return g_retired.load(std::memory_order_acquire);
}

bool OnReturnTrap(GuestContext* c) {
    Frame frame{};
    {
        std::scoped_lock lk{g_frames_lock};
        const auto it = g_frames.find(c->tpidrro_el0);
        if (it == g_frames.end()) return false;
        auto& stack = it->second;
        while (!stack.empty() && stack.back().sp < c->x[31]) stack.pop_back();
        if (stack.empty() || stack.back().sp != c->x[31]) return false;
        frame = stack.back();
        stack.pop_back();
        if (stack.empty()) g_frames.erase(it);
    }
    c->pc = frame.ret;
    if (g_retired.load(std::memory_order_acquire)) return true;
    t_call_data = frame.data;
    const auto& hs = frame.site->handlers;
    for (auto h = hs.rbegin(); h != hs.rend(); ++h) {
        if (h->post) {
            t_current_mod = h->mod;
            h->post(reinterpret_cast<pla_ctx*>(c));
            t_current_mod = nullptr;
        }
    }
    t_call_data = nullptr;
    return true;
}

std::string Report() {
    std::string out = "Hooks:\n";
    std::vector<std::pair<u64, std::string>> v;
    for (const auto& [pc, s] : g_sites) v.emplace_back(s->calls.load(), s->label);
    std::sort(v.rbegin(), v.rend());
    for (const auto& [n, label] : v) {
        char b[128];
        std::snprintf(b, sizeof b, "  %-24s %12llu\n", label.c_str(), static_cast<unsigned long long>(n));
        out += b;
    }
    return out;
}

} // namespace plahost::mods
