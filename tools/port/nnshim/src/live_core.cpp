// nnshim::live: live settings channel of nvnvk's in-game menu (see include/nnshim/live.hpp).
#include "nnshim/live.hpp"
#include "nnshim/runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nnshim::live {
namespace {

using VersionFn = uint32_t (*)();
using GetFn = int (*)(const char*, char*, int);
std::atomic<VersionFn> g_version{nullptr};
std::atomic<GetFn> g_get{nullptr};
std::chrono::steady_clock::time_point g_probe{};
std::mutex g_mu;
std::condition_variable g_cv;
std::vector<Listener> g_listeners;
bool g_stop = false;
ExitSafeThread g_thread;

bool Resolve() {
    if (g_version.load()) return true;
#ifdef _WIN32
    const auto now = std::chrono::steady_clock::now();
    if (now - g_probe < std::chrono::seconds(1)) return false;
    g_probe = now;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"nvnvk.dll", &m) || !m) return false;
    auto get = reinterpret_cast<GetFn>(reinterpret_cast<void*>(GetProcAddress(m, "nvnvk_live_get")));
    auto ver = reinterpret_cast<VersionFn>(reinterpret_cast<void*>(GetProcAddress(m, "nvnvk_live_version")));
    if (!get || !ver) return false;
    g_get = get;
    g_version = ver;
    return true;
#else
    return false;
#endif
}

void ThreadMain() {
    uint32_t seen = 0;
    std::unique_lock lk(g_mu);
    while (!g_stop) {
        g_cv.wait_for(lk, std::chrono::milliseconds(100));
        if (g_stop) break;
        const std::vector<Listener> ls = g_listeners;
        lk.unlock();
        const uint32_t v = Version();
        const bool changed = v != seen;
        seen = v;
        for (Listener f : ls) f(changed);
        lk.lock();
    }
}

} // namespace

uint32_t Version() {
    if (!Resolve()) return 0;
    return g_version.load()();
}

bool Get(const char* key, std::string& out) {
    GetFn f = g_get.load();
    if (!f) return false;
    char buf[512];
    const int n = f(key, buf, static_cast<int>(sizeof buf));
    if (n < 0) return false;
    out.assign(buf, static_cast<size_t>(n));
    return true;
}

void Subscribe(Listener fn) {
    std::lock_guard lk(g_mu);
    g_listeners.push_back(fn);
    if (!g_thread.joinable()) {
        g_stop = false;
        g_thread = std::thread(ThreadMain);
    }
}

void Stop() {
    {
        std::lock_guard lk(g_mu);
        g_stop = true;
        g_listeners.clear();
    }
    g_cv.notify_all();
    if (g_thread.joinable() && g_thread.get_id() != std::this_thread::get_id()) g_thread.join();
}

} // namespace nnshim::live
