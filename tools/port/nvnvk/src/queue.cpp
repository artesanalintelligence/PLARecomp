// Queue, sync and window hooks.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <chrono>

#include "exec.h"
#include "objects.h"

namespace nvnvk {

std::atomic<uint64_t> g_game_wait_ns[4];

namespace {

std::mutex g_sync_mu;
std::atomic<uint64_t> g_acquire_wait_ns{0};
inline uint64_t NsSince(std::chrono::steady_clock::time_point t0) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}
std::unordered_map<uint64_t, uint64_t> g_sync_value;  // Sync* -> timeline value (exclusive mode)
std::atomic<uint32_t> g_acquire_counter{0};
// Timeline value of the last present of each window texture (exclusive). As on the Switch, acquiring a
// texture waits until the GPU has finished its previous present: without this the game ran several frames
// ahead of the GPU and overwrote data the GPU had yet to read (sprint 7, unified memory).
std::unordered_map<uint64_t, std::vector<uint64_t>> g_present_value;

// defer_submit (sprint 7): in NVN the GPU does not start submitted work until nvnQueueFlush (or a queue operation
// that implies it: FenceSync, Present, Finish). With unified memory the GPU reads the data when it executes, so
// submissions are held until then: the game may write a submission's data after submitting it and before Flush.
std::mutex g_pend_mu;
std::vector<uint64_t> g_pending;
std::atomic<uint64_t> g_deferred_submits{0}, g_flushes{0};

void FlushPending(pla_ctx* ctx) {
    std::vector<uint64_t> h;
    {
        std::lock_guard<std::mutex> lk(g_pend_mu);
        h.swap(g_pending);
    }
    if (!h.empty()) {
        g_flushes.fetch_add(1, std::memory_order_relaxed);
        g_exec.Submit(ctx, h.data(), (uint32_t)h.size());
        if (g_cfg.game_wait == 1) g_vk.WaitValue(g_exec.PlannedValue(), UINT64_MAX);
    }
}

pla_action SubmitCommands(pla_ctx* ctx) {
    const uint32_t n = std::min<uint32_t>((uint32_t)X(ctx, 1), 4096);
    std::vector<uint64_t> handles(n);
    if (n) g_guest.Read(ctx, X(ctx, 2), handles.data(), n * 8);
    if (g_cfg.exclusive && g_cfg.defer_submit) {
        g_deferred_submits.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_pend_mu);
        g_pending.insert(g_pending.end(), handles.begin(), handles.end());
        return PLA_SKIP;
    }
    g_exec.Submit(ctx, handles.data(), n);
    if (g_cfg.exclusive && g_cfg.game_wait == 1) g_vk.WaitValue(g_exec.PlannedValue(), UINT64_MAX);
    return g_cfg.exclusive ? PLA_SKIP : PLA_CONTINUE;
}

pla_action PresentTexture(pla_ctx* ctx) {
    if (g_cfg.exclusive) FlushPending(ctx);
    g_exec.Present(ctx, X(ctx, 1), (int)X(ctx, 2));
    if (g_cfg.exclusive) {
        const int idx = (int)X(ctx, 2);
        if (idx >= 0 && idx < 16) {
            std::lock_guard<std::mutex> lk(g_sync_mu);
            auto& v = g_present_value[X(ctx, 1)];
            if ((int)v.size() <= idx) v.resize(idx + 1, 0);
            v[idx] = g_exec.PlannedValue();
        }
        // pla_api 1.2: the runtime counts FPS (title, PERF) and closes the splash with this.
        if (PLA_API_HAS(g_guest.api, frame_presented) && g_guest.api->frame_presented) g_guest.api->frame_presented();
        const auto t_wait = std::chrono::steady_clock::now();
        if (g_cfg.game_wait == 2) g_vk.WaitValue(g_exec.PlannedValue(), UINT64_MAX);
        if (g_cfg.game_wait == 3) {
            // Like the Switch's double buffering: when frame N is presented, the GPU must have finished N-1. With more
            // latency the game overwrites data (bone palettes...) the GPU has not read yet (unified memory).
            static uint64_t prev = 0;
            if (prev && g_vk.CompletedValue() < prev) g_vk.WaitValue(prev, UINT64_MAX);
            prev = g_exec.PlannedValue();
        }
        if (g_cfg.game_wait >= 2) g_game_wait_ns[1].fetch_add(NsSince(t_wait), std::memory_order_relaxed);
        SetX(ctx, 0, 0);
        return PLA_SKIP;
    }
    return PLA_CONTINUE;
}

pla_action AcquireTexture(pla_ctx* ctx) {
    if (!g_cfg.exclusive) return PLA_CONTINUE;
    WindowInfo wi;
    uint32_t n = 2;
    if (g_obj.Window(X(ctx, 0), &wi) && !wi.textures.empty()) n = (uint32_t)wi.textures.size();
    const int32_t idx = (int32_t)(g_acquire_counter.fetch_add(1) % n);
    g_guest.Write(ctx, X(ctx, 2), &idx, 4);
    uint64_t last = 0;
    {
        std::lock_guard<std::mutex> lk(g_sync_mu);
        auto it = g_present_value.find(X(ctx, 0));
        if (g_cfg.acquire_wait && it != g_present_value.end() && idx < (int)it->second.size()) last = it->second[idx];
        g_sync_value[X(ctx, 1)] = last;  // the sync signals when the GPU finishes that present
    }
    if (last && g_vk.CompletedValue() < last) {
        const auto t0 = std::chrono::steady_clock::now();
        g_vk.WaitValue(last, UINT64_MAX);
        const uint64_t ns = NsSince(t0);
        g_acquire_wait_ns.fetch_add(ns, std::memory_order_relaxed);
        g_game_wait_ns[2].fetch_add(ns, std::memory_order_relaxed);
    }
    SetX(ctx, 0, 0);  // success
    return PLA_SKIP;
}

pla_action FenceSync(pla_ctx* ctx) {
    if (!g_cfg.exclusive) return PLA_CONTINUE;
    FlushPending(ctx);
    const uint64_t v = g_exec.PlannedValue();
    std::lock_guard<std::mutex> lk(g_sync_mu);
    g_sync_value[X(ctx, 1)] = v;
    return PLA_SKIP;
}

pla_action SyncWait(pla_ctx* ctx) {
    if (!g_cfg.exclusive) return PLA_CONTINUE;
    uint64_t v = 0;
    {
        std::lock_guard<std::mutex> lk(g_sync_mu);
        auto it = g_sync_value.find(X(ctx, 0));
        if (it != g_sync_value.end()) v = it->second;
    }
    uint32_t res = 0;  // ALREADY_SIGNALED
    if (v && g_vk.CompletedValue() < v) {
        const uint64_t timeout = X(ctx, 1);
        const auto t0 = std::chrono::steady_clock::now();
        res = g_vk.WaitValue(v, timeout) ? 1 : 2;  // CONDITION_SATISFIED / TIMEOUT_EXPIRED
        g_game_wait_ns[0].fetch_add(NsSince(t0), std::memory_order_relaxed);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 30) {
            static std::atomic<int> logs{0};
            if (logs.fetch_add(1) < 200)
                NVK_INFO("game wait: SyncWait %.1f ms (value %llu, completed %llu, planned %llu, res %u)", ms,
                         (unsigned long long)v, (unsigned long long)g_vk.CompletedValue(),
                         (unsigned long long)g_exec.PlannedValue(), res);
        }
    }
    SetX(ctx, 0, res);
    return PLA_SKIP;
}

pla_action QueueFlush(pla_ctx* ctx) {
    if (!g_cfg.exclusive) return PLA_CONTINUE;
    FlushPending(ctx);
    static uint64_t last = 0;
    const uint64_t d = g_deferred_submits.load(), f = g_flushes.load();
    if (d / 2000 != last) {
        last = d / 2000;
        NVK_INFO("submissions held until Flush/Fence/Present: %llu submissions, %llu flushes", (unsigned long long)d, (unsigned long long)f);
    }
    return PLA_SKIP;
}
pla_action QueueFinish(pla_ctx* ctx) {
    if (!g_cfg.exclusive) return PLA_CONTINUE;
    FlushPending(ctx);
    const auto t0 = std::chrono::steady_clock::now();
    g_vk.WaitValue(g_exec.PlannedValue(), UINT64_MAX);
    g_game_wait_ns[3].fetch_add(NsSince(t0), std::memory_order_relaxed);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 30) {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 200) NVK_INFO("game wait: QueueFinish %.1f ms", ms);
    }
    return PLA_SKIP;
}

}  // namespace

void RegisterQueueHooks(PreFn* pre, PostFn*) {
    pre[FN_nvnQueueSubmitCommands] = SubmitCommands;
    pre[FN_nvnQueuePresentTexture] = PresentTexture;
    pre[FN_nvnWindowAcquireTexture] = AcquireTexture;
    pre[FN_nvnQueueFenceSync] = FenceSync;
    pre[FN_nvnSyncWait] = SyncWait;
    pre[FN_nvnQueueFlush] = QueueFlush;
    pre[FN_nvnQueueFinish] = QueueFinish;
}

}  // namespace nvnvk
