// SPDX-License-Identifier: GPL-3.0-or-later
// NVN control format (NVNshaderData::control) inferred from the game's 1.1.1 RomFS:
// 8709 distinct controls, cross-checked against suyu's pipeline cache. See
// docs/port/SHADERS.md §3. [V] = verified with data; [S] = assumed.
#pragma once

#include <cstdint>

namespace nvnshader::ctrl {

inline constexpr uint32_t kControlMagic = 0x98761234; // +0x000 [V]
inline constexpr uint32_t kDataMagic = 0x12345678;    // data+0x00 in graphics [V]

inline constexpr uint32_t kMagic = 0x000;
inline constexpr uint32_t kFormatVersion = 0x004;   // 1 [V]
inline constexpr uint32_t kControlSize0 = 0x020;    // = control size [V]
inline constexpr uint32_t kControlSize = 0x6F0;     // = control size [V]
inline constexpr uint32_t kCodeSize = 0x6F8;        // code bytes [V approx.]
inline constexpr uint32_t kConstSize = 0x6FC;       // c1 bank bytes [V]
inline constexpr uint32_t kConstOffset = 0x700;     // c1 bank offset in data [V]
inline constexpr uint32_t kDataSize = 0x704;        // total data size [V]
inline constexpr uint32_t kHeaderSize = 0x708;      // 0x30 graphics / 0 compute [V]
inline constexpr uint32_t kNumRegisters = 0x70C;    // GPRs used [V]
inline constexpr uint32_t kStage = 0x714;           // 0 VS, 1 FS, 5 CS [V]
inline constexpr uint32_t kWorkgroupX = 0x748;      // compute [V with suyu's cache]
inline constexpr uint32_t kWorkgroupY = 0x74C;
inline constexpr uint32_t kWorkgroupZ = 0x750;
inline constexpr uint32_t kSharedMemory = 0x754;    // compute, bytes [V: 2 CS, = 32 threads x stride]
inline constexpr uint32_t kMinControlSize = 0x800;

// Transform feedback (5 VS in the game, cross-checked with suyu's cache). One varying per buffer:
inline constexpr uint32_t kXfbVector = 0x0DC;     // u32 per buffer (stride 0x80): 0x20 + n = generic n
inline constexpr uint32_t kXfbComponents = 0x4DC; // u32 per buffer (stride 0x80): components
inline constexpr uint32_t kXfbBufferStride = 0x80;
inline constexpr uint32_t kXfbStrides = 0x6DC;    // u16 per buffer: stride in bytes
inline constexpr uint32_t kXfbEnabled = 0x6E8;    // u8 per buffer: 1 = used
inline constexpr uint32_t kXfbCount = 0x6EC;      // u8: number of buffers

} // namespace nvnshader::ctrl
