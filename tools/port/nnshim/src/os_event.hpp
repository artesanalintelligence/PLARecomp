// nnshim: kernel events handed to the guest as nn::os::SystemEventType.
//
// Some SDK functions return a system event (a readable handle of a KEvent) that the game
// waits on with nn::os::WaitSystemEvent/TryWait. With pla_api extension 1.1 the shim creates
// the KEvent in the runtime (event_create), requests a handle for the game
// (event_guest_handle) and initializes the guest struct just like SDK 11.4's
// nn::os::AttachReadableHandleToSystemEvent (our own disassembly of the sdk module:
// sdk+0x346d70 calls os::detail::AttachInterProcessEvent, sdk+0x3497b0, and sets the state):
//
//   SystemEventType (0x30 bytes)
//     +0x00 u64 wait list: prev = next = the struct itself (empty list)
//     +0x10 u8  auto_clear (EventClearMode == AutoClear(1))
//     +0x11 u8  InterProcessEvent state = 1 (initialized)
//     +0x12 u8  readable handle managed (the game closes it when destroying the event)
//     +0x13 u8  writable handle managed = 0
//     +0x14 u32 readable handle
//     +0x18 u32 writable handle = 0 (none)
//     +0x28 u8  SystemEventType state = 2 (InitializedAsInterProcessEvent)
//
// Without the extension (runtime 1.0) these functions return 0 and the adapter leaves the
// call to the original SDK.
#pragma once

#include <cstdint>

#include "abi_common.hpp"

namespace nnshim::osev {

constexpr uint32_t kClearManual = 0;
constexpr uint32_t kClearAuto = 1;

inline bool Available() {
    return g_api && PLA_API_HAS(g_api, event_destroy) && g_api->event_create && g_api->event_guest_handle;
}

// Writes a SystemEventType initialized with `handle` at `ev_va`. false if the write fails.
inline bool AttachReadableHandle(Guest& g, uint64_t ev_va, uint32_t handle, bool managed, uint32_t clear_mode) {
    uint8_t s[0x29] = {};
    for (int i = 0; i < 8; ++i) {
        s[0x00 + i] = static_cast<uint8_t>(ev_va >> (8 * i));
        s[0x08 + i] = static_cast<uint8_t>(ev_va >> (8 * i));
    }
    s[0x10] = clear_mode == kClearAuto ? 1 : 0;
    s[0x11] = 1;
    s[0x12] = managed ? 1 : 0;
    s[0x13] = 0;
    for (int i = 0; i < 4; ++i) s[0x14 + i] = static_cast<uint8_t>(handle >> (8 * i));
    // +0x18 writable handle = 0; +0x1C..+0x27: the SDK leaves the rest of the union (EventType) alone,
    // but here it is zeroed: the event was uninitialized (state 0).
    s[0x28] = 2;
    return g.write(ev_va, s, sizeof s) == sizeof s;
}

// Creates a runtime KEvent (signaled if `signaled`) and returns a new readable handle in the
// game's table (0 if the API is missing or it fails). `out_event` receives the runtime id to
// signal it later (event_signal) or release it (event_destroy).
inline uint32_t NewGuestEvent(const char* name, bool signaled, uint64_t* out_event) {
    if (out_event) *out_event = 0;
    if (!Available()) return 0;
    const uint64_t ev = g_api->event_create(name);
    if (!ev) return 0;
    if (signaled) g_api->event_signal(ev);   // queued; the handle is requested afterwards, in order
    const uint32_t h = g_api->event_guest_handle(ev);
    if (!h) {
        g_api->event_destroy(ev);
        return 0;
    }
    if (out_event) *out_event = ev;
    else g_api->event_destroy(ev);   // the game keeps its handle
    return h;
}

} // namespace nnshim::osev
