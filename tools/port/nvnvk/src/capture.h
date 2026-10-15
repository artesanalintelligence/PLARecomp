// Session capture for the offline replayer (tools/nvnvk_replay).
//
// With "capture_path" in the config, nvnvk writes every hooked call (registers X0-X7, V0-V3 and the
// returned X0/X1) and all guest memory the backend reads while handling that call. The replayer loads
// the same nvnvk.dll with a fake pla_api that serves those registers and that memory, so the backend
// can be debugged without the game (seconds per iteration instead of minutes in the queue). While
// capturing, hooks are serialized with a mutex so the file order is a valid execution order.
//
// Format (little endian): NvkcHeader header and records [u8 type][payload]:
//   CALL  u32 tid, u64 pc, u64 x[8], u64 v[4]       (start of a pre)
//   POST  u32 tid, u64 pc, u64 x0, u64 x1            (start of a post)
//   BLOB  u64 id, u32 size, bytes                     (new content; referenced by id)
//   MEM   u64 va, u32 size, u64 blob                  (what the backend read in the current call)
//   END                                               (end of the call: apply MEM and execute)
//   FRAME u64 n                                       (after each present)
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

#include "nvnvk.h"

namespace nvnvk {

enum : uint8_t { kRecCall = 1, kRecPost = 2, kRecBlob = 3, kRecMem = 4, kRecEnd = 5, kRecFrame = 6 };

#pragma pack(push, 1)
struct NvkcHeader {
    char magic[4];  // "NVKC"
    uint32_t version;
    uint64_t sdk_base, main_base;
};
#pragma pack(pop)
inline constexpr uint32_t kNvkcVersion = 1;

class Capture {
public:
    bool Open(const std::string& path, uint64_t max_frames, uint64_t max_mb);
    bool active() const { return active_.load(std::memory_order_acquire); }
    // Inside the hooks (with the mutex held by mod_main).
    void Call(pla_ctx* ctx, uint64_t pc);
    void Post(pla_ctx* ctx, uint64_t pc);
    void Mem(uint64_t va, const void* data, size_t n);
    void End();
    void Frame(uint64_t n);
    void FlushNow();  // after each submit: if the process crashes later, the capture reaches that submit
    void Close();
    std::mutex& mu() { return mu_; }

private:
    void Put(const void* p, size_t n);
    std::mutex mu_;
    std::atomic<bool> active_{false};
    std::FILE* f_ = nullptr;
    uint64_t max_frames_ = 0, max_bytes_ = 0, bytes_ = 0, blobs_ = 0, blob_bytes_ = 0, flushed_ = 0;
    std::unordered_map<uint64_t, uint64_t> seen_;  // content hash -> id
};
extern Capture g_capture;

// Fast 64-bit hash (non-cryptographic) for deduplication.
uint64_t FastHash(const void* p, size_t n);

}  // namespace nvnvk
