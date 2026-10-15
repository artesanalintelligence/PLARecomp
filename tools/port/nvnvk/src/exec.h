// Execution of NVN recordings on Vulkan.
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

#include "record.h"
#include "textures.h"
#include "vkctx.h"

namespace nvnvk {

struct VertexBinding {
    uint64_t addr = 0, size = 0;
};

// NVN state bound during execution.
struct NvnState {
    uint32_t blend[8][2] = {};          // BlendState per target (8 bytes)
    uint32_t color = 0x08030000;        // ColorState
    uint32_t channel_mask = 0xFFFFFFFF;
    uint32_t depth_stencil[2] = {0x00000022, 0x11181118};
    uint32_t multisample[6] = {5, 0, 0, 0, 0, 0};
    uint32_t polygon = 0x14;
    uint32_t vattrib[16] = {};
    uint32_t num_vattrib = 0;
    uint64_t vstream[16] = {};
    uint32_t num_vstream = 0;
    VertexBinding vbuf[16];
    uint64_t program = 0;
    uint32_t program_stages = 0;
    VertexBinding ubo[6][18];
    VertexBinding ssbo[6][16];
    uint64_t tex[6][32] = {};
    uint64_t img[6][8] = {};
    TexDesc rt_color[8];
    uint32_t num_rt = 0;
    TexDesc rt_depth;
    float viewport[16][4] = {};
    int32_t scissor[16][4] = {};
    float depth_range[16][2] = {};
    uint32_t stencil_ref[2] = {}, stencil_mask[2] = {0xFF, 0xFF}, stencil_vmask[2] = {0xFF, 0xFF};
    float blend_color[4] = {};
    float alpha_ref = 0;
    uint32_t depth_bounds_enable = 0;
    float depth_bounds[2] = {0, 1};
    float poly_offset[3] = {};
    float line_width = 1, point_size = 1;
    uint32_t depth_clamp = 0, raster_discard = 0, sample_mask = 0xFFFFFFFF;
    uint64_t tex_pool_cpu = 0, smp_pool_cpu = 0;
    uint32_t tex_pool_count = 0, smp_pool_count = 0;
    bool rt_dirty = true;
    bool xfb_active = false;
    VertexBinding xfb_buf[4];
    uint64_t xfb_ctrl = 0;                 // Begin/EndTransformFeedback control buffer
    mutable bool xfb_resume = false;       // the xfb_counters values are valid
    // Per XFB buffer: its counter is still valid (sprint 7). Binding ANOTHER buffer to that slot makes the hardware write
    // from its start; previously we continued from the previous buffer's counter (each skinning mesh of the same session
    // was written shifted: its start kept stale data -> exploding vertices, "frozen" models).
    mutable uint32_t xfb_resume_mask = 0;
};

class Executor {
public:
    // From the game hooks. With async submission (exclusive + async_submit) they only enqueue: the
    // Vulkan work is done in order by a dedicated thread.
    void Submit(pla_ctx* ctx, const uint64_t* handles, uint32_t n);
    void Present(pla_ctx* ctx, uint64_t window, int index);
    // Timeline value the last accepted submit/present will reach (for FenceSync).
    uint64_t PlannedValue() const;
    void StartAsync();
    void StopAsync();
    bool async() const { return async_; }
    void CaptureNext(const std::string& path);
    void BeginRendering();
    void OcclBegin();  // opens the occlusion segment (inside the render pass) if none is open
    void OcclEnd();
    void EndRendering();
    bool rendering() const { return rendering_; }

private:
    struct Job {
        bool present = false;
        std::vector<RecordingPtr> recs;
        TexDesc win;  // present
    };
    void SubmitNow(pla_ctx* ctx, const std::vector<RecordingPtr>& recs);
    void PresentNow(pla_ctx* ctx, const TexDesc& d);
    void Enqueue(Job&& j);
    void Worker();
    void Run(const Recording& r, int depth);
    void Barrier(uint32_t bits);
    void ReportCounter(const struct PCounter& c);
    void Draw(const struct PDrawArrays* da, const struct PDrawElements* de);
    void ClearColor(const PClearColor& p);
    void ClearDepthStencil(const PClearDepthStencil& p);
    void Copy(Op op, const uint32_t* payload);
    bool ReadGuestTic(uint32_t id, uint32_t out[8]);
    bool ReadGuestTsc(uint32_t id, uint32_t out[8]);
    void DumpImage(Image* im, const std::string& path);
    void DumpRenderTargets(uint64_t frame, int sec);

    pla_ctx* ctx_ = nullptr;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    NvnState st_;
    bool rendering_ = false;
    VkExtent2D rt_extent_{};
    std::string capture_path_;
    uint64_t frame_ = 0;
    void HitchCheck();
    uint64_t evict_tick_ = 0;
    bool occl_active_ = false;
    uint32_t chunk_draws_ = 0;  // draws since the last submitted chunk (chunk_draws)
    // Only measured after a type-1 report and for a few draws (the occlusion test boxes); the rest of the frame has no
    // active query (o-real1: with a query on every draw, -11 % while mounted).
    uint32_t occl_armed_ = 0;  // unused transform feedback buffers are recycled every 128 submits
    // Hitch diagnostics (hitch_ms): time of the last present and counters at frame start.
    std::chrono::steady_clock::time_point last_present_{};
    double frame_ema_ms_ = 0;
    uint64_t hitch_base_[10] = {};
    uint64_t phase_base_[8] = {};
    int hitch_logs_ = 0;
    bool async_ = false;
    std::atomic<uint64_t> planned_{0};
    bool xfb_check_armed_ = false;
    // Antialiasing without touching the UI (aa_hud): window textures seen at present and draws made to them this frame;
    // after draw number aa_scene_draw the scene is copied (Presenter::SnapScene).
    uint64_t win_gpu_[4] = {};
    uint32_t win_idx_ = 0, win_draws_ = 0;
    bool scene_snapped_ = false;
    bool IsWindowTex(uint64_t gpu) const {
        for (uint64_t g : win_gpu_)
            if (g && g == gpu) return true;
        return false;
    }
    std::thread worker_;
    std::mutex qmu_;
    std::condition_variable qcv_, qspace_;
    std::deque<Job> jobs_;
    bool quit_ = false;
    size_t next_sec_ = 0;
    std::string post_dump_;  // final-pass capture pending for the end of this present
    bool post_dump_srgb_ = false;
};
extern Executor g_exec;

// Implemented in pipeline.cpp: prepares pipeline + descriptors and emits the draw.
// Returns false if it could not (no shader translator, unsupported shader...).
bool EmitDraw(Executor* ex, pla_ctx* ctx, VkCommandBuffer cmd, const NvnState& st, const struct PDrawArrays* da,
              const struct PDrawElements* de, VkFormat* color_formats, uint32_t num_colors, VkFormat depth_format,
              VkSampleCountFlagBits samples);
bool EmitDispatch(pla_ctx* ctx, VkCommandBuffer cmd, const NvnState& st, uint32_t x, uint32_t y, uint32_t z);
void PipelineInit();
void PipelineShutdown();
void PipelineBeginSubmit();
// snap_vram: records the pending host->device snapshot copies (start of a submission, outside a render pass).
void PipelineSnapPromote(VkCommandBuffer cmd);
void PipelineReport();
// Hitch diagnostics counters: variants, us translating, skipped draws, us waiting on pipelines.
void PipelineCounters(uint64_t out[4]);
void PipelineInvalidateUploads();
bool PipelineUnified();  // unified memory active in this submit (pipeline.cpp)
extern thread_local uint64_t t_xfb_draw_ctrl;  // != ~0: the next Draw is a DrawTransformFeedback with this control
bool PipelineHasScatters();  // there are written gathered bindings pending to be written back
void PipelineXfbCheckBegin();             // xfb_check diagnostics: record this frame's XFB draws
void PipelineXfbCheckEnd(pla_ctx* ctx, uint64_t frame);   // and check them (with the GPU idle)
void PipelineFlushScatters(VkCommandBuffer cmd);  // write them back (outside the render pass)
void PipelineInvalidateGathers();           // after a barrier/copy: gathered data is no longer valid
void SyncGpuBuffers(VkCommandBuffer cmd);  // at the start of each submit: CPU writes -> own buffers
void QueueWriteback(VkCommandBuffer cmd);
void ResolveWriteback(FrameSlot& f);  // at the end of each submit: copies what kept SSBOs wrote back to the guest
// Accumulated ns: uploads, descriptors, variants, pipelines, draws (total), XFB fingerprint.
void PipelineCpuCounters(uint64_t out[6]);
extern std::atomic<uint64_t> g_phase_copy_ns, g_phase_flush_ns;
struct ScopedPhase {
    std::atomic<uint64_t>& a;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit ScopedPhase(std::atomic<uint64_t>& x) : a(x) {}
    ~ScopedPhase() {
        a.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                    std::memory_order_relaxed);
    }
};
void PipelineBeginFrame();  // on each present: refills the pipeline wait budget
extern std::atomic<uint64_t> g_counter_types[32];  // ReportCounter per type (diagnostics)
// Called by the texture cache before recording a copy (outside a render pass).
void OnTransferBegin();

}  // namespace nvnvk
