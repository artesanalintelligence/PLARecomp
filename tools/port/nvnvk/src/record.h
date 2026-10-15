// Recording of NVN command buffers into our own stream (independent of the driver).
// Each CommandBuffer* call from the game becomes a packet with its arguments already resolved
// (states copied by value, textures described, arrays copied), because NVN copies the data at
// record time: the game may change its structures afterwards.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "nvnvk.h"

namespace nvnvk {

enum class Op : uint16_t {
    BindBlend, BindColor, BindChannelMask, BindDepthStencil, BindMultisample, BindPolygon,
    BindVertexAttrib, BindVertexStream, BindVertexBuffer, BindProgram,
    BindUniformBuffer, BindStorageBuffer, BindTexture, BindImage,
    SetRenderTargets, SetViewports, SetScissors, SetDepthRanges,
    SetStencilRef, SetStencilMask, SetStencilValueMask, SetBlendColor, SetAlphaRef,
    SetDepthBounds, SetPolygonOffsetClamp, SetLineWidth, SetPointSize, SetDepthClamp,
    SetRasterizerDiscard, SetSampleMask, SetTexturePool, SetSamplerPool, SetConservativeRaster,
    DrawArrays, DrawElements, DispatchCompute, DrawTransformFeedback,
    ClearColor, ClearDepthStencil, ClearTexture,
    CopyBufferToBuffer, CopyBufferToTexture, CopyTextureToBuffer, CopyTextureToTexture, Downsample,
    Barrier, ReportCounter, ResetCounter, CallCommands,
    BindTransformFeedbackBuffer, BeginTransformFeedback, EndTransformFeedback,
    Count
};

const char* OpName(Op op);

// Packet: 4-byte header + payload (multiple of 4).
struct PacketHeader {
    Op op;
    uint16_t words;  // payload size in 32-bit words
};

// Fixed-size payloads
struct PBindState { uint32_t data[6]; };            // states (up to 0x18 bytes)
struct PVertexAttrib { uint32_t count; uint32_t attr[16]; };
struct PVertexStream { uint32_t count; uint32_t pad; uint64_t stream[16]; };  // stride | divisor<<32
struct PVertexBuffer { uint32_t index, pad; uint64_t addr, size; };
struct PProgram { uint64_t program; uint32_t stages, pad; };
struct PBuffer { uint32_t stage, index; uint64_t addr, size; };
struct PTexture { uint32_t stage, index; uint64_t handle; };
struct PRenderTargets { uint32_t count, pad; TexDesc color[8]; TexDesc depth; };
struct PRects { uint32_t first, count; float v[16 * 4]; };
struct PFaceValue { uint32_t faces, value; };
struct PFloat4 { float v[4]; };
struct PDepthBounds { uint32_t enable; float mn, mx; };
struct PPool { uint64_t pool; uint64_t cpu; uint32_t count, pad; uint64_t mempool; };
struct PDrawArrays { uint32_t mode, first, count, base_instance, instance_count; };
struct PDrawElements { uint32_t mode, type, count, pad; uint64_t index_addr; int32_t base_vertex; uint32_t base_instance, instance_count, pad2; };
struct PDispatch { uint32_t x, y, z; };
struct PClearColor { uint32_t index, mask; uint32_t color[4]; };
struct PClearDepthStencil { float depth; uint32_t depth_mask, stencil, stencil_mask; };
struct PRegion { int32_t x, y, z, w, h, d; };
struct PCopyBuffer { uint64_t src, dst, size; };
struct PCopyBufTex { uint64_t buffer; TexDesc tex; PRegion region; uint32_t flags, pad; };
struct PCopyTexTex { TexDesc src, dst; PRegion src_region, dst_region; uint32_t flags, pad; };
struct PDownsample { TexDesc src, dst; };
struct PClearTexture { TexDesc tex; PRegion region; uint32_t color[4]; uint32_t mask, pad; };
struct PCounter { uint32_t type, pad; uint64_t addr; };
struct PCall { uint32_t count, pad; uint64_t handles[1]; };  // variable size

// A recorded command buffer (immutable once finished).
struct Recording {
    std::vector<uint32_t> words;
    uint64_t cmdbuf = 0;   // game's CommandBuffer*
    uint64_t handle = 0;   // handle returned to the game
    uint64_t serial = 0;
    mutable std::atomic<uint64_t> last_used{0};  // last frame it was executed (or created) in
};
using RecordingPtr = std::shared_ptr<const Recording>;

// Looks up the recording of a handle (thread-safe). Null if unknown.
RecordingPtr FindRecording(uint64_t handle);
// Forgets recordings not executed in the last `age` frames. Returns how many remain.
// Retired ones go to *dead (if not null) so they can be freed off the game thread.
size_t SweepRecordings(uint64_t frame, uint64_t age, std::vector<RecordingPtr>* dead = nullptr);

}  // namespace nvnvk
