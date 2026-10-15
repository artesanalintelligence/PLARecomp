// SPDX-License-Identifier: MIT
#include "capture.h"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nvnvk {

Capture g_capture;

uint64_t FastHash(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (n * 0xC2B2AE3D27D4EB4Full);
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t a, b, c, d;
        std::memcpy(&a, p + i, 8);
        std::memcpy(&b, p + i + 8, 8);
        std::memcpy(&c, p + i + 16, 8);
        std::memcpy(&d, p + i + 24, 8);
        h = (h ^ (a * 0x87C37B91114253D5ull)) * 0x4CF5AD432745937Full;
        h = (h ^ (b * 0x52DCE729DA3ED7B5ull)) * 0x4CF5AD432745937Full;
        h = (h ^ (c * 0x87C37B91114253D5ull)) * 0x9E3779B97F4A7C15ull;
        h = (h ^ (d * 0x52DCE729DA3ED7B5ull)) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t a;
        std::memcpy(&a, p + i, 8);
        h = (h ^ (a * 0x87C37B91114253D5ull)) * 0x4CF5AD432745937Full;
        h ^= h >> 31;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

bool Capture::Open(const std::string& path, uint64_t max_frames, uint64_t max_mb) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    std::setvbuf(f_, nullptr, _IOFBF, 8 << 20);
    max_frames_ = max_frames;
    max_bytes_ = max_mb << 20;
    NvkcHeader h{{'N', 'V', 'K', 'C'}, kNvkcVersion, g_guest.sdk_base, g_guest.main_base};
    Put(&h, sizeof h);
    active_ = true;
    NVK_INFO("replay capture: %s (up to %llu frames, %llu MB)", path.c_str(),
             (unsigned long long)max_frames, (unsigned long long)max_mb);
    return true;
}

void Capture::Put(const void* p, size_t n) {
    std::fwrite(p, 1, n, f_);
    bytes_ += n;
}

void Capture::Call(pla_ctx* ctx, uint64_t pc) {
    if (!active()) return;
    const uint8_t t = kRecCall;
    Put(&t, 1);
#ifdef _WIN32
    const uint32_t tid = GetCurrentThreadId();
#else
    const uint32_t tid = 0;
#endif
    Put(&tid, 4);
    Put(&pc, 8);
    uint64_t x[8], v[4];
    for (unsigned i = 0; i < 8; ++i) x[i] = g_guest.api->get_x(ctx, i);
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t q[2];
        g_guest.api->get_v(ctx, i, q);
        v[i] = q[0];
    }
    Put(x, sizeof x);
    Put(v, sizeof v);
}

void Capture::Post(pla_ctx* ctx, uint64_t pc) {
    if (!active()) return;
    const uint8_t t = kRecPost;
    Put(&t, 1);
#ifdef _WIN32
    const uint32_t tid = GetCurrentThreadId();
#else
    const uint32_t tid = 0;
#endif
    Put(&tid, 4);
    Put(&pc, 8);
    const uint64_t x[2] = {g_guest.api->get_x(ctx, 0), g_guest.api->get_x(ctx, 1)};
    Put(x, sizeof x);
}

void Capture::Mem(uint64_t va, const void* data, size_t n) {
    if (!active() || !n) return;
    const uint64_t h = FastHash(data, n);
    auto it = seen_.find(h);
    uint64_t id;
    if (it == seen_.end()) {
        id = ++blobs_;
        seen_.emplace(h, id);
        const uint8_t t = kRecBlob;
        const uint32_t sz = (uint32_t)n;
        Put(&t, 1);
        Put(&id, 8);
        Put(&sz, 4);
        Put(data, n);
        blob_bytes_ += n;
    } else {
        id = it->second;
    }
    const uint8_t t = kRecMem;
    const uint32_t sz = (uint32_t)n;
    Put(&t, 1);
    Put(&va, 8);
    Put(&sz, 4);
    Put(&id, 8);
}

void Capture::End() {
    if (!active()) return;
    const uint8_t t = kRecEnd;
    Put(&t, 1);
    // Frequent flushes: if the game crashes, the capture is still usable up to that point.
    if (bytes_ - flushed_ > (4u << 20)) {
        std::fflush(f_);
        flushed_ = bytes_;
    }
}

void Capture::FlushNow() {
    if (!active() || bytes_ == flushed_) return;
    std::fflush(f_);
    flushed_ = bytes_;
}

void Capture::Frame(uint64_t n) {
    if (!active()) return;
    const uint8_t t = kRecFrame;
    Put(&t, 1);
    Put(&n, 8);
    std::fflush(f_);
    flushed_ = bytes_;
    if ((max_frames_ && n >= max_frames_) || (max_bytes_ && bytes_ >= max_bytes_)) Close();
}

void Capture::Close() {
    if (!f_) return;
    active_ = false;
    std::fclose(f_);
    f_ = nullptr;
    NVK_INFO("capture closed: %.1f MB, %llu blobs (%.1f MB unique content)", bytes_ / 1048576.0,
             (unsigned long long)blobs_, blob_bytes_ / 1048576.0);
    seen_.clear();
}

}  // namespace nvnvk
