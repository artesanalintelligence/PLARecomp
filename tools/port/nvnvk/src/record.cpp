// nvnCommandBuffer* hooks: record into Recording (record.h).
// SPDX-License-Identifier: MIT
#include "record.h"

#include <algorithm>
#include <cstdarg>

namespace nvnvk {

namespace {

struct Recorder {
    std::shared_ptr<Recording> cur;
    bool recording = false;
    size_t last_words = 0;  // size of this command buffer's previous recording (FPS-1: reserve instead of regrowing)
};

std::mutex g_rec_mu;
std::unordered_map<uint64_t, std::unique_ptr<Recorder>> g_recorders;
std::mutex g_handles_mu;
std::unordered_map<uint64_t, RecordingPtr> g_handles;
std::atomic<uint64_t> g_serial{1};

thread_local uint64_t t_last_cb = 0;
thread_local Recorder* t_last_rec = nullptr;

Recorder* Get(uint64_t cb) {
    if (cb == t_last_cb && t_last_rec) return t_last_rec;
    std::lock_guard<std::mutex> lk(g_rec_mu);
    auto& r = g_recorders[cb];
    if (!r) r = std::make_unique<Recorder>();
    t_last_cb = cb;
    t_last_rec = r.get();
    return r.get();
}

inline pla_action Ret() { return g_cfg.exclusive ? PLA_SKIP : PLA_CONTINUE; }

template <class T>
void Emit(pla_ctx* ctx, Op op, const T& payload, size_t bytes = sizeof(T)) {
    Recorder* r = Get(X(ctx, 0));
    if (!r->cur) {
        // Implicit recording (the game did not call BeginRecording on this object, as far as we saw)
        r->cur = std::make_shared<Recording>();
        r->cur->cmdbuf = X(ctx, 0);
    }
    const uint32_t words = (uint32_t)((bytes + 3) / 4);
    auto& w = r->cur->words;
    PacketHeader h{op, (uint16_t)words};
    uint32_t hw;
    std::memcpy(&hw, &h, 4);
    w.push_back(hw);
    const size_t at = w.size();
    w.resize(at + words, 0);
    std::memcpy(&w[at], &payload, bytes);
    // Diagnostic count: no locked add (FPS-1; several game threads record at once). Approximate under contention.
    g_stats.cmds_recorded.store(g_stats.cmds_recorded.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// ---- begin/end ---------------------------------------------------------------------------
pla_action BeginRecording(pla_ctx* ctx) {
    Recorder* r = Get(X(ctx, 0));
    r->cur = std::make_shared<Recording>();
    r->cur->cmdbuf = X(ctx, 0);
    r->cur->serial = g_serial.fetch_add(1);
    // The game re-records the same command buffers every frame with similar sizes: one allocation instead of the
    // vector's doubling steps (heap traffic on the game's NVN threads).
    r->cur->words.reserve(std::max<size_t>(64, r->last_words + r->last_words / 8));
    r->recording = true;
    // Exclusive with native command memory (default): the driver's command buffer also "records"
    // (it stays empty because the commands are skipped), so its unhooked getters
    // (GetCommandMemoryFree, ...) stay consistent. excl1 aborted (2162-0001) right after
    // BeginRecording when Begin/AddCommandMemory/AddControlMemory were skipped.
    return g_cfg.native_cmdmem ? PLA_CONTINUE : Ret();
}

std::atomic<uint64_t> g_handle_clobber{0};
void Publish(uint64_t handle, std::shared_ptr<Recording> rec) {
    rec->handle = handle;
    const uint64_t f = g_stats.frames.load(std::memory_order_relaxed);
    rec->last_used = f;
    std::lock_guard<std::mutex> lk(g_handles_mu);
    auto& slot = g_handles[handle];
    // Diagnostic: the driver gave the same handle to another recording still in use (used in recent frames and
    // created earlier): the new one replaces it and whoever calls it (CallCommands) will execute something else.
    if (slot && slot->last_used.load(std::memory_order_relaxed) + 3 >= f && slot->serial + 0 < rec->serial) {
        const uint64_t n = g_handle_clobber.fetch_add(1);
        if (n < 30 || (n % 1000) == 0)
            NVK_WARN("recording clobbered: handle %llx (cmdbuf %llx, %zu words, used in frame %llu) by another from cmdbuf %llx "
                     "(%zu words) in frame %llu; %llu times",
                     (unsigned long long)handle, (unsigned long long)slot->cmdbuf, slot->words.size(),
                     (unsigned long long)slot->last_used.load(), (unsigned long long)rec->cmdbuf, rec->words.size(),
                     (unsigned long long)f, (unsigned long long)n + 1);
    }
    slot = std::move(rec);
}

pla_action EndRecordingPre(pla_ctx* ctx) {
    Recorder* r = Get(X(ctx, 0));
    std::shared_ptr<Recording> rec = std::move(r->cur);
    r->cur.reset();
    r->recording = false;
    if (rec) r->last_words = rec->words.size();
    if (!rec) rec = std::make_shared<Recording>();
    g_stats.recordings.fetch_add(1, std::memory_order_relaxed);
    if (g_cfg.exclusive && !g_cfg.native_cmdmem) {
        const uint64_t handle = 0x7E00000000000000ull | rec->serial;
        if (g_cfg.replay) {
            // Replayer: the post carries the handle the driver returned in the capture; it is also published
            // under that value so CallCommands/QueueSubmitCommands (read from the capture) find it.
            g_guest.api->call_data(ctx)[2] = reinterpret_cast<uint64_t>(new std::shared_ptr<Recording>(rec));
        }
        Publish(handle, std::move(rec));
        SetX(ctx, 0, handle);
        return PLA_SKIP;
    }
    // shadow: the driver provides the handle; it is published in the post
    uint64_t* cd = g_guest.api->call_data(ctx);
    auto* raw = new std::shared_ptr<Recording>(std::move(rec));
    cd[2] = reinterpret_cast<uint64_t>(raw);
    return PLA_CONTINUE;
}

void EndRecordingPost(pla_ctx* ctx) {
    uint64_t* cd = g_guest.api->call_data(ctx);
    auto* raw = reinterpret_cast<std::shared_ptr<Recording>*>(cd[2]);
    if (!raw) return;
    cd[2] = 0;  // a repeated post (or one without its pre, at the end of a capture) must not reuse it
    if (*raw) Publish(X(ctx, 0), std::move(*raw));
    delete raw;
}

// Command memory management: not needed in exclusive mode (nothing is recorded into game memory).
pla_action MemorySkip(pla_ctx*) { return g_cfg.native_cmdmem ? PLA_CONTINUE : Ret(); }
pla_action GetMemoryUsed(pla_ctx* ctx) {
    if (!g_cfg.exclusive || g_cfg.native_cmdmem) return PLA_CONTINUE;
    SetX(ctx, 0, 0);
    return PLA_SKIP;
}

// ---- states ------------------------------------------------------------------------------
template <Op OP, size_t BYTES>
pla_action BindState(pla_ctx* ctx) {
    PBindState p{};
    g_guest.Read(ctx, X(ctx, 1), p.data, BYTES);
    Emit(ctx, OP, p, BYTES);
    return Ret();
}

pla_action BindVertexAttrib(pla_ctx* ctx) {
    PVertexAttrib p{};
    p.count = std::min<uint32_t>((uint32_t)X(ctx, 1), 16);
    if (p.count) g_guest.Read(ctx, X(ctx, 2), p.attr, p.count * 4);
    Emit(ctx, Op::BindVertexAttrib, p);
    return Ret();
}

pla_action BindVertexStream(pla_ctx* ctx) {
    PVertexStream p{};
    p.count = std::min<uint32_t>((uint32_t)X(ctx, 1), 16);
    if (p.count) g_guest.Read(ctx, X(ctx, 2), p.stream, p.count * 8);
    Emit(ctx, Op::BindVertexStream, p);
    return Ret();
}

pla_action BindVertexBuffer(pla_ctx* ctx) {
    PVertexBuffer p{(uint32_t)X(ctx, 1), 0, X(ctx, 2), X(ctx, 3)};
    Emit(ctx, Op::BindVertexBuffer, p);
    return Ret();
}

pla_action BindProgram(pla_ctx* ctx) {
    PProgram p{X(ctx, 1), (uint32_t)X(ctx, 2), 0};
    Emit(ctx, Op::BindProgram, p);
    return Ret();
}

template <Op OP>
pla_action BindBuffer(pla_ctx* ctx) {
    PBuffer p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), X(ctx, 3), X(ctx, 4)};
    Emit(ctx, OP, p);
    return Ret();
}

template <Op OP>
pla_action BindTex(pla_ctx* ctx) {
    PTexture p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), X(ctx, 3)};
    Emit(ctx, OP, p);
    return Ret();
}

pla_action SetRenderTargets(pla_ctx* ctx) {
    PRenderTargets p{};
    p.count = std::min<uint32_t>((uint32_t)X(ctx, 1), 8);
    uint64_t colors[8] = {}, views[8] = {};
    if (p.count && X(ctx, 2)) g_guest.Read(ctx, X(ctx, 2), colors, p.count * 8);
    if (p.count && X(ctx, 3)) g_guest.Read(ctx, X(ctx, 3), views, p.count * 8);
    for (uint32_t i = 0; i < p.count; ++i)
        if (colors[i]) ReadTexDesc(ctx, colors[i], views[i], &p.color[i]);
    if (X(ctx, 4)) ReadTexDesc(ctx, X(ctx, 4), X(ctx, 5), &p.depth);
    Emit(ctx, Op::SetRenderTargets, p);
    return Ret();
}

template <Op OP, unsigned PER>
pla_action SetRects(pla_ctx* ctx) {
    PRects p{};
    p.first = (uint32_t)X(ctx, 1);
    p.count = std::min<uint32_t>((uint32_t)X(ctx, 2), 16);
    if (p.count) g_guest.Read(ctx, X(ctx, 3), p.v, p.count * PER * 4);
    Emit(ctx, OP, p, 8 + p.count * PER * 4);
    return Ret();
}

pla_action SetScissor(pla_ctx* ctx) {
    PRects p{};
    p.first = (uint32_t)X(ctx, 1);
    p.count = 1;
    int32_t r[4] = {(int32_t)X(ctx, 2), (int32_t)X(ctx, 3), (int32_t)X(ctx, 4), (int32_t)X(ctx, 5)};
    std::memcpy(p.v, r, 16);
    Emit(ctx, Op::SetScissors, p, 8 + 16);
    return Ret();
}

pla_action SetDepthRange(pla_ctx* ctx) {
    PRects p{};
    p.first = 0;
    p.count = 1;
    p.v[0] = S(ctx, 0);
    p.v[1] = S(ctx, 1);
    Emit(ctx, Op::SetDepthRanges, p, 8 + 8);
    return Ret();
}

template <Op OP>
pla_action FaceValue(pla_ctx* ctx) {
    PFaceValue p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2)};
    Emit(ctx, OP, p);
    return Ret();
}

pla_action SetBlendColor(pla_ctx* ctx) {
    PFloat4 p{};
    g_guest.Read(ctx, X(ctx, 1), p.v, 16);
    Emit(ctx, Op::SetBlendColor, p);
    return Ret();
}

template <Op OP>
pla_action OneFloat(pla_ctx* ctx) {
    PFloat4 p{};
    p.v[0] = S(ctx, 0);
    Emit(ctx, OP, p, 4);
    return Ret();
}

template <Op OP>
pla_action OneInt(pla_ctx* ctx) {
    PFaceValue p{(uint32_t)X(ctx, 1), 0};
    Emit(ctx, OP, p);
    return Ret();
}

pla_action SetDepthBounds(pla_ctx* ctx) {
    PDepthBounds p{(uint32_t)(X(ctx, 1) & 0xFF), S(ctx, 0), S(ctx, 1)};
    Emit(ctx, Op::SetDepthBounds, p);
    return Ret();
}

pla_action SetPolygonOffsetClamp(pla_ctx* ctx) {
    PFloat4 p{{S(ctx, 0), S(ctx, 1), S(ctx, 2), 0}};
    Emit(ctx, Op::SetPolygonOffsetClamp, p);
    return Ret();
}

template <Op OP>
pla_action SetPool(pla_ctx* ctx) {
    PPool p{};
    p.pool = X(ctx, 1);
    if (p.pool) {
        // TexturePool/SamplerPool: +0 MemoryPool*, +8 offset, +0x10 descriptor count
        uint64_t raw[3] = {};
        g_guest.Read(ctx, p.pool, raw, sizeof raw);
        p.mempool = raw[0];
        PoolInfo mp;
        if (g_reg.PoolByObj(raw[0], &mp)) p.cpu = mp.cpu + raw[1];
        p.count = (uint32_t)raw[2];
    }
    Emit(ctx, OP, p);
    return Ret();
}

// ---- draws -------------------------------------------------------------------------------
pla_action DrawArrays(pla_ctx* ctx) {
    PDrawArrays p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3), 0, 1};
    Emit(ctx, Op::DrawArrays, p);
    return Ret();
}
pla_action DrawArraysInstanced(pla_ctx* ctx) {
    PDrawArrays p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3), (uint32_t)X(ctx, 4),
                  (uint32_t)X(ctx, 5)};
    Emit(ctx, Op::DrawArrays, p);
    return Ret();
}
pla_action DrawElements(pla_ctx* ctx) {
    PDrawElements p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3), 0, X(ctx, 4), 0, 0, 1, 0};
    Emit(ctx, Op::DrawElements, p);
    return Ret();
}
pla_action DrawElementsBaseVertex(pla_ctx* ctx) {
    PDrawElements p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3), 0, X(ctx, 4),
                    (int32_t)X(ctx, 5), 0, 1, 0};
    Emit(ctx, Op::DrawElements, p);
    return Ret();
}
pla_action DrawElementsInstanced(pla_ctx* ctx) {
    PDrawElements p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3), 0, X(ctx, 4),
                    (int32_t)X(ctx, 5), (uint32_t)X(ctx, 6), (uint32_t)X(ctx, 7), 0};
    Emit(ctx, Op::DrawElements, p);
    return Ret();
}
pla_action DispatchCompute(pla_ctx* ctx) {
    PDispatch p{(uint32_t)X(ctx, 1), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3)};
    Emit(ctx, Op::DispatchCompute, p);
    return Ret();
}

// ---- clears and copies -----------------------------------------------------------------
pla_action ClearColor(pla_ctx* ctx) {
    PClearColor p{};
    p.index = (uint32_t)X(ctx, 1);
    g_guest.Read(ctx, X(ctx, 2), p.color, 16);
    p.mask = (uint32_t)X(ctx, 3);
    Emit(ctx, Op::ClearColor, p);
    return Ret();
}
pla_action ClearDepthStencil(pla_ctx* ctx) {
    PClearDepthStencil p{S(ctx, 0), (uint32_t)(X(ctx, 1) & 0xFF), (uint32_t)X(ctx, 2), (uint32_t)X(ctx, 3)};
    Emit(ctx, Op::ClearDepthStencil, p);
    return Ret();
}
pla_action ClearTexture(pla_ctx* ctx) {
    PClearTexture p{};
    ReadTexDesc(ctx, X(ctx, 1), X(ctx, 2), &p.tex);
    if (X(ctx, 3)) g_guest.Read(ctx, X(ctx, 3), &p.region, sizeof p.region);
    g_guest.Read(ctx, X(ctx, 4), p.color, 16);
    p.mask = (uint32_t)X(ctx, 5);
    Emit(ctx, Op::ClearTexture, p);
    return Ret();
}
pla_action CopyBufferToBuffer(pla_ctx* ctx) {
    PCopyBuffer p{X(ctx, 1), X(ctx, 2), X(ctx, 3)};
    Emit(ctx, Op::CopyBufferToBuffer, p);
    return Ret();
}
pla_action CopyBufferToTexture(pla_ctx* ctx) {
    PCopyBufTex p{};
    p.buffer = X(ctx, 1);
    ReadTexDesc(ctx, X(ctx, 2), X(ctx, 3), &p.tex);
    if (X(ctx, 4)) g_guest.Read(ctx, X(ctx, 4), &p.region, sizeof p.region);
    p.flags = (uint32_t)X(ctx, 5);
    Emit(ctx, Op::CopyBufferToTexture, p);
    return Ret();
}
pla_action CopyTextureToBuffer(pla_ctx* ctx) {
    PCopyBufTex p{};
    ReadTexDesc(ctx, X(ctx, 1), X(ctx, 2), &p.tex);
    if (X(ctx, 3)) g_guest.Read(ctx, X(ctx, 3), &p.region, sizeof p.region);
    p.buffer = X(ctx, 4);
    p.flags = (uint32_t)X(ctx, 5);
    Emit(ctx, Op::CopyTextureToBuffer, p);
    return Ret();
}
pla_action CopyTextureToTexture(pla_ctx* ctx) {
    PCopyTexTex p{};
    ReadTexDesc(ctx, X(ctx, 1), X(ctx, 2), &p.src);
    if (X(ctx, 3)) g_guest.Read(ctx, X(ctx, 3), &p.src_region, sizeof p.src_region);
    ReadTexDesc(ctx, X(ctx, 4), X(ctx, 5), &p.dst);
    if (X(ctx, 6)) g_guest.Read(ctx, X(ctx, 6), &p.dst_region, sizeof p.dst_region);
    p.flags = (uint32_t)X(ctx, 7);
    Emit(ctx, Op::CopyTextureToTexture, p);
    return Ret();
}
pla_action Downsample(pla_ctx* ctx) {
    PDownsample p{};
    ReadTexDesc(ctx, X(ctx, 1), 0, &p.src);
    ReadTexDesc(ctx, X(ctx, 2), 0, &p.dst);
    Emit(ctx, Op::Downsample, p);
    return Ret();
}

// ---- misc ---------------------------------------------------------------------------------
pla_action Barrier(pla_ctx* ctx) {
    PFaceValue p{(uint32_t)X(ctx, 1), 0};
    Emit(ctx, Op::Barrier, p);
    return Ret();
}
pla_action ReportCounter(pla_ctx* ctx) {
    PCounter p{(uint32_t)X(ctx, 1), 0, X(ctx, 2)};
    Emit(ctx, Op::ReportCounter, p);
    return Ret();
}
pla_action ResetCounter(pla_ctx* ctx) {
    PCounter p{(uint32_t)X(ctx, 1), 0, 0};
    Emit(ctx, Op::ResetCounter, p);
    return Ret();
}
pla_action CallCommands(pla_ctx* ctx) {
    const uint32_t n = std::min<uint32_t>((uint32_t)X(ctx, 1), 4096);
    std::vector<uint64_t> buf(2 + n);
    buf[0] = n;
    if (n) g_guest.Read(ctx, X(ctx, 2), &buf[1], n * 8);
    // payload: count(u32) pad(u32) handles[n]
    std::vector<uint8_t> raw(8 + n * 8);
    std::memcpy(raw.data(), &n, 4);
    if (n) std::memcpy(raw.data() + 8, &buf[1], n * 8);
    Recorder* r = Get(X(ctx, 0));
    if (!r->cur) {
        r->cur = std::make_shared<Recording>();
        r->cur->cmdbuf = X(ctx, 0);
    }
    auto& w = r->cur->words;
    const uint32_t words = (uint32_t)(raw.size() / 4);
    if (words < 0xFFFF) {
        PacketHeader h{Op::CallCommands, (uint16_t)words};
        uint32_t hw;
        std::memcpy(&hw, &h, 4);
        w.push_back(hw);
        const size_t at = w.size();
        w.resize(at + words);
        std::memcpy(&w[at], raw.data(), raw.size());
    }
    return Ret();
}
pla_action BindXfbBuffer(pla_ctx* ctx) {
    PBuffer p{0, (uint32_t)X(ctx, 1), X(ctx, 2), X(ctx, 3)};
    Emit(ctx, Op::BindTransformFeedbackBuffer, p);
    return Ret();
}
pla_action XfbBeginEnd(pla_ctx* ctx) {
    PCounter p{0, 0, X(ctx, 1)};
    Emit(ctx, Op::BeginTransformFeedback, p);
    return Ret();
}
pla_action XfbEnd(pla_ctx* ctx) {
    PCounter p{0, 0, X(ctx, 1)};
    Emit(ctx, Op::EndTransformFeedback, p);
    return Ret();
}
pla_action DrawXfb(pla_ctx* ctx) {
    PCounter p{(uint32_t)X(ctx, 1), 0, X(ctx, 2)};
    Emit(ctx, Op::DrawTransformFeedback, p);
    return Ret();
}

}  // namespace

const char* OpName(Op op) {
    static const char* const k[] = {
        "BindBlend", "BindColor", "BindChannelMask", "BindDepthStencil", "BindMultisample", "BindPolygon",
        "BindVertexAttrib", "BindVertexStream", "BindVertexBuffer", "BindProgram",
        "BindUniformBuffer", "BindStorageBuffer", "BindTexture", "BindImage",
        "SetRenderTargets", "SetViewports", "SetScissors", "SetDepthRanges",
        "SetStencilRef", "SetStencilMask", "SetStencilValueMask", "SetBlendColor", "SetAlphaRef",
        "SetDepthBounds", "SetPolygonOffsetClamp", "SetLineWidth", "SetPointSize", "SetDepthClamp",
        "SetRasterizerDiscard", "SetSampleMask", "SetTexturePool", "SetSamplerPool", "SetConservativeRaster",
        "DrawArrays", "DrawElements", "DispatchCompute", "DrawTransformFeedback",
        "ClearColor", "ClearDepthStencil", "ClearTexture",
        "CopyBufferToBuffer", "CopyBufferToTexture", "CopyTextureToBuffer", "CopyTextureToTexture", "Downsample",
        "Barrier", "ReportCounter", "ResetCounter", "CallCommands",
        "BindTransformFeedbackBuffer", "BeginTransformFeedback", "EndTransformFeedback"};
    const unsigned i = (unsigned)op;
    return i < sizeof(k) / sizeof(k[0]) ? k[i] : "?";
}

RecordingPtr FindRecording(uint64_t handle) {
    std::lock_guard<std::mutex> lk(g_handles_mu);
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return nullptr;
    it->second->last_used.store(g_stats.frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return it->second;
}

size_t SweepRecordings(uint64_t frame, uint64_t age, std::vector<RecordingPtr>* dead) {
    std::lock_guard<std::mutex> lk(g_handles_mu);
    for (auto it = g_handles.begin(); it != g_handles.end();) {
        if (frame > age && it->second->last_used.load(std::memory_order_relaxed) + age < frame) {
            if (dead) dead->push_back(std::move(it->second));
            it = g_handles.erase(it);
        } else {
            ++it;
        }
    }
    return g_handles.size();
}

void RegisterRecordHooks(PreFn* pre, PostFn* post) {
    pre[FN_nvnCommandBufferBeginRecording] = BeginRecording;
    pre[FN_nvnCommandBufferEndRecording] = EndRecordingPre;
    post[FN_nvnCommandBufferEndRecording] = EndRecordingPost;
    pre[FN_nvnCommandBufferAddCommandMemory] = MemorySkip;
    pre[FN_nvnCommandBufferAddControlMemory] = MemorySkip;
    pre[FN_nvnCommandBufferSetMemoryCallback] = MemorySkip;
    pre[FN_nvnCommandBufferSetMemoryCallbackData] = MemorySkip;
    pre[FN_nvnCommandBufferGetCommandMemoryUsed] = GetMemoryUsed;
    pre[FN_nvnCommandBufferGetControlMemoryUsed] = GetMemoryUsed;

    pre[FN_nvnCommandBufferBindBlendState] = BindState<Op::BindBlend, 8>;
    pre[FN_nvnCommandBufferBindColorState] = BindState<Op::BindColor, 4>;
    pre[FN_nvnCommandBufferBindChannelMaskState] = BindState<Op::BindChannelMask, 4>;
    pre[FN_nvnCommandBufferBindDepthStencilState] = BindState<Op::BindDepthStencil, 8>;
    pre[FN_nvnCommandBufferBindMultisampleState] = BindState<Op::BindMultisample, 0x18>;
    pre[FN_nvnCommandBufferBindPolygonState] = BindState<Op::BindPolygon, 4>;
    pre[FN_nvnCommandBufferBindVertexAttribState] = BindVertexAttrib;
    pre[FN_nvnCommandBufferBindVertexStreamState] = BindVertexStream;
    pre[FN_nvnCommandBufferBindVertexBuffer] = BindVertexBuffer;
    pre[FN_nvnCommandBufferBindProgram] = BindProgram;
    pre[FN_nvnCommandBufferBindUniformBuffer] = BindBuffer<Op::BindUniformBuffer>;
    pre[FN_nvnCommandBufferBindStorageBuffer] = BindBuffer<Op::BindStorageBuffer>;
    pre[FN_nvnCommandBufferBindTexture] = BindTex<Op::BindTexture>;
    pre[FN_nvnCommandBufferBindImage] = BindTex<Op::BindImage>;
    pre[FN_nvnCommandBufferSetRenderTargets] = SetRenderTargets;
    pre[FN_nvnCommandBufferSetViewports] = SetRects<Op::SetViewports, 4>;
    pre[FN_nvnCommandBufferSetScissors] = SetRects<Op::SetScissors, 4>;
    pre[FN_nvnCommandBufferSetScissor] = SetScissor;
    pre[FN_nvnCommandBufferSetDepthRanges] = SetRects<Op::SetDepthRanges, 2>;
    pre[FN_nvnCommandBufferSetDepthRange] = SetDepthRange;
    pre[FN_nvnCommandBufferSetStencilRef] = FaceValue<Op::SetStencilRef>;
    pre[FN_nvnCommandBufferSetStencilMask] = FaceValue<Op::SetStencilMask>;
    pre[FN_nvnCommandBufferSetStencilValueMask] = FaceValue<Op::SetStencilValueMask>;
    pre[FN_nvnCommandBufferSetBlendColor] = SetBlendColor;
    pre[FN_nvnCommandBufferSetAlphaRef] = OneFloat<Op::SetAlphaRef>;
    pre[FN_nvnCommandBufferSetDepthBounds] = SetDepthBounds;
    pre[FN_nvnCommandBufferSetPolygonOffsetClamp] = SetPolygonOffsetClamp;
    pre[FN_nvnCommandBufferSetLineWidth] = OneFloat<Op::SetLineWidth>;
    pre[FN_nvnCommandBufferSetPointSize] = OneFloat<Op::SetPointSize>;
    pre[FN_nvnCommandBufferSetDepthClamp] = OneInt<Op::SetDepthClamp>;
    pre[FN_nvnCommandBufferSetRasterizerDiscard] = OneInt<Op::SetRasterizerDiscard>;
    pre[FN_nvnCommandBufferSetSampleMask] = OneInt<Op::SetSampleMask>;
    pre[FN_nvnCommandBufferSetConservativeRasterEnable] = OneInt<Op::SetConservativeRaster>;
    pre[FN_nvnCommandBufferSetTexturePool] = SetPool<Op::SetTexturePool>;
    pre[FN_nvnCommandBufferSetSamplerPool] = SetPool<Op::SetSamplerPool>;
    pre[FN_nvnCommandBufferDrawArrays] = DrawArrays;
    pre[FN_nvnCommandBufferDrawArraysInstanced] = DrawArraysInstanced;
    pre[FN_nvnCommandBufferDrawElements] = DrawElements;
    pre[FN_nvnCommandBufferDrawElementsBaseVertex] = DrawElementsBaseVertex;
    pre[FN_nvnCommandBufferDrawElementsInstanced] = DrawElementsInstanced;
    pre[FN_nvnCommandBufferDispatchCompute] = DispatchCompute;
    pre[FN_nvnCommandBufferClearColor] = ClearColor;
    pre[FN_nvnCommandBufferClearDepthStencil] = ClearDepthStencil;
    pre[FN_nvnCommandBufferClearTexture] = ClearTexture;
    pre[FN_nvnCommandBufferCopyBufferToBuffer] = CopyBufferToBuffer;
    pre[FN_nvnCommandBufferCopyBufferToTexture] = CopyBufferToTexture;
    pre[FN_nvnCommandBufferCopyTextureToBuffer] = CopyTextureToBuffer;
    pre[FN_nvnCommandBufferCopyTextureToTexture] = CopyTextureToTexture;
    pre[FN_nvnCommandBufferDownsample] = Downsample;
    pre[FN_nvnCommandBufferBarrier] = Barrier;
    pre[FN_nvnCommandBufferReportCounter] = ReportCounter;
    pre[FN_nvnCommandBufferResetCounter] = ResetCounter;
    pre[FN_nvnCommandBufferCallCommands] = CallCommands;
    pre[FN_nvnCommandBufferBindTransformFeedbackBuffer] = BindXfbBuffer;
    pre[FN_nvnCommandBufferBeginTransformFeedback] = XfbBeginEnd;
    pre[FN_nvnCommandBufferEndTransformFeedback] = XfbEnd;
    pre[FN_nvnCommandBufferDrawTransformFeedback] = DrawXfb;
}

}  // namespace nvnvk
