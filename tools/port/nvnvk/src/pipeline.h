// Global Device modes that affect shader translation and the viewport.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace nvnvk {
struct DeviceModes {
    uint32_t origin_mode = 1;  // nvnDeviceSetWindowOriginMode: 0 lower-left (GL), 1 upper-left
    uint32_t depth_mode = 1;   // nvnDeviceSetDepthMode: 0 [-w, w] (GL), 1 [0, w]
};
extern DeviceModes g_modes;
}  // namespace nvnvk
