// nvnvk: NVN -> Vulkan backend for PLA 1.1.1 (port v2). Common declarations.
//
// Model (docs/port/NVN-SEMANTICS.md §1): builders/state stay in the original driver; the
// backend hooks command buffer recording, submission, synchronization and the window.
//   "shadow" mode:    every hook returns PLA_CONTINUE (suyu keeps drawing); the backend
//                     records and executes in parallel in its own window and can dump captures.
//   "exclusive" mode: the recording/submit/present/sync hooks return PLA_SKIP: the emulated
//                     GPU gets no work and only the backend draws.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pla_mod_api.h"

namespace nvnvk {

// ---------------------------------------------------------------- logging
void Log(int level, const char* fmt, ...);
bool InitVulkan();  // creates the VkDevice, the translator and the window (once; mod_main.cpp)
#define NVK_INFO(...) ::nvnvk::Log(PLA_LOG_INFO, __VA_ARGS__)
#define NVK_WARN(...) ::nvnvk::Log(PLA_LOG_WARNING, __VA_ARGS__)
#define NVK_ERR(...) ::nvnvk::Log(PLA_LOG_ERROR, __VA_ARGS__)
#define NVK_DBG(...) ::nvnvk::Log(PLA_LOG_DEBUG, __VA_ARGS__)
// Info line also forwarded to the host log (pla.log): GPU choice, present path, periodic rate (FPS-1).
constexpr int kLogToHost = 0x100;
#define NVK_HOST(...) ::nvnvk::Log(PLA_LOG_INFO | ::nvnvk::kLogToHost, __VA_ARGS__)

// ---------------------------------------------------------------- configuration
struct Config {
    bool exclusive = false;      // PLA_SKIP on recording/submit/present
    bool present = true;         // open our own window and present
    bool vsync = false;          // unlocked FPS by default
    int window_width = 1280, window_height = 720;
    std::string capture_dir;     // if not empty: dump the presented texture every capture_every frames
    int capture_every = 0;
    int capture_max = 0;
    std::vector<double> capture_secs;  // timed dumps (seconds since the mod loaded)
    std::chrono::steady_clock::time_point t0;
    std::string log_path;
    std::string shader_cache_dir;
    bool validation = false;
    int log_level = PLA_LOG_INFO;
    std::string pipeline_cache;      // initial VkPipelineCache (prewarmed by nvnshader_vkcheck)
    std::string pipeline_cache_out;  // where to save it on exit (empty = don't save)
    bool flip_viewport = false;
    bool replay = false;
    bool async_submit = true;        // exclusive: Vulkan on its own thread (exec.h)             // set by nvnvk_replay (handle aliasing in exclusive)
    bool game_window = true;
    bool import_memory = true;       // VK_EXT_external_memory_host for the pools (hostmem.h)
    bool import_in_shadow = false;
    // Unified memory (sprint 7): with imported pools, every buffer (vertex, index, UBO, SSBO, XFB, copies) is bound
    // directly on the game's memory, as on the Switch. No own buffers or shadow copies: what the GPU writes lands in
    // the game's memory and what the CPU writes is seen by the GPU without any detection.
    bool unified_memory = true;
    bool unified_copy_gpu_only = false;
    bool unified_copy_gpu = true;
    bool unified_barriers = true;
    bool unified_barriers_all = false;
    bool import_bind = true;
    bool import_backing = true;  // import the whole backing memory once (in chunks) instead of each pool (hostmem.h)
    bool ubo_full_range = false;  // test: with unified memory, bind UBOs with the shader's size (not the game's)
    bool ssbo_from_ubo = true;  // SSBOs whose pointer lives in a game UBO (pipeline.cpp, BuildWrites)
    bool pool_evict_overlap = true;  // a new pool overlapping another (in GPU space) replaces it (objects.cpp)
    bool texel_buffers = true;  // bind buffer textures/images (pipeline.cpp, BuildWrites)
    std::string renderdoc_dir;  // portable RenderDoc folder: .rdc capture at each capture_secs (exec.cpp)
    bool xfb_check = false;  // diagnostic: every 60 frames, check the positions written by XFB (unified memory)
    uint32_t xfb_trace = 0;  // diagnostic: every N frames between xfb_trace_from and _to (s), one line per XFB draw with hashes
    double xfb_trace_from = 0, xfb_trace_to = 1e9;
    bool vram_pools = true;  // unified memory: CPU_NO_ACCESS pools in VRAM (hostmem.h, InitVram)
    bool vbib_snapshot = true;  // unified memory: VB/IB from pools the GPU never wrote are copied at record time (hostmem.h)
    bool vbib_trim = false;  // with vbib_snapshot: copy only the vertices/instances the draw uses (pipeline.cpp)
    // FPS-1 (submit CPU): fine-grained "CPU per draw" timers cost ~2 QPC reads per phase and texture; off by default.
    bool cpu_stats = false;
    bool state_dedup = true;
    // snap_cache (FPS-1): vbib_snapshot copies kept across submissions in a persistent host-visible arena and reused while
    // the source bytes hash the same (pipeline.cpp, SnapCached). 0 MB = off (copy into the frame ring every submission).
    uint32_t snap_cache_mb = 128;
    // snap_vram (FPS-1, laptops): a snapshot reused unchanged is copied once (GPU copy at the start of the next
    // submission) into a device-local arena of the same size and read from VRAM from then on. Default on for a discrete
    // GPU (on an integrated one device-local memory is system memory: nothing to gain). "auto" | "true" | "false".
    std::string snap_vram = "auto";
    bool host_read_stats = true;  // per-frame report of the bytes the GPU reads from host memory, by binding kind
    // Graphics preset (FPS-1): "auto" (Performance on an integrated or low-VRAM GPU, otherwise the files' values),
    // "quality", "performance" or "custom". Only "auto" changes anything at startup (ApplyAutoPreset, options.cpp);
    // the menu writes the individual settings when a preset is chosen.
    std::string preset = "auto";
    bool lowend_force = false;  // test: treat the chosen GPU as low-end (exercises the auto preset on a discrete GPU)  // EmitDraw: skip vkCmdSet*/vertex binds whose value did not change in this command buffer
    uint32_t anisotropy = 0;  // graphics: forced minimum anisotropy (0 = the game's; 16 = 16x)
    float tex_lod_bias = 0;  // graphics: extra mip LOD bias for mipmapped samplers (menu, live; engine_options texture_lod_bias)
    float sharpen = 0;  // graphics: final pass sharpening (0 = off, 0..1)
    int aa = 0;  // graphics: final pass antialiasing (option `aa`: 0 "off", 1 "fxaa", 2 "smaa")
    // With aa: don't smooth the UI. After draw number aa_scene_draw into the window texture (the scene composite) the
    // frame is copied; at present, only pixels the UI didn't change afterwards are smoothed.
    bool aa_hud = true;
    bool aa_debug = false;  // diagnostic: with aa_hud, paint the pixels treated as UI red
    bool trace_window = false;  // diagnostic: trace the draws into the window textures (like trace_rt)
    uint32_t aa_scene_draw = 1;
    bool upscale_sharp = false;  // graphics: Lanczos scaling to the window (option `upscale` = "sharp") instead of bilinear
    uint32_t fps_limit = 0;  // graphics: FPS limit at present (0 = no limit)
    float brightness = 0, contrast = 1, saturation = 1;  // graphics: final pass color adjustments
    bool show_fps = false;  // graphics: FPS counter at the top left
    uint64_t recording_max_age = 0;  // frames without execution after which a recording is forgotten (0 = never; was 1200)
    bool xfb_begin_resume = false;  // diagnostic: Begin resumes if ctrl+4.. is non-zero in the guest (pre-s9 behavior)
    bool xfb_rebind_reset = true;  // binding a different XFB buffer resets its counter (exec.cpp)
    uint32_t import_chunk_mb = 512;
    uint32_t pool_init_delay_us_per_mb = 0;  // diagnostic: artificial delay when creating each pool (without import)  // diagnostic: false = import the pools but bind nothing from them
    uint64_t unified_ssbo_drop_max = 0;  // diagnostic: written SSBOs of up to N bytes don't reach the game's memory  // diagnostic: barrier after every draw  // barriers after each XFB session and each dispatch (unified memory)  // CopyBufferToBuffer also on the GPU (unified memory)
    uint32_t unified_mask = 63;  // diagnostic: direct binding kinds (pipeline.cpp, Upload)
    bool import_verify = false;
    bool acquire_wait = true;
    bool defer_submit = true;
    // Diagnostic/safety (sprint 7): the game waits for the GPU after every submit (1) or after every present (2).
    int game_wait = 0;  // hold submits until nvnQueueFlush/FenceSync/Present/Finish (queue.cpp)  // WindowAcquireTexture waits for the GPU to finish the previous present of that texture  // diagnostic: compare what the GPU reads from imported pools with the CPU (hostmem.h)  // CopyBufferToBuffer only on the GPU (no CPU copy at record time)
    bool trace_writes_only = false;  // trace: don't log reads (SAMPLES)
    bool trace_compute = false;    // trace: every dispatch with its SSBOs and images
    bool trace_rtsets = false;     // trace: every render target set change
    bool log_tex_updates = false;  // log every CopyBufferToTexture and every re-upload due to a CPU change
    bool log_gpubufs = false;      // log every transform feedback buffer created
    bool xfb_validate = false;     // (superseded by gpubuf_sync; it used to discard buffers with GPU writes)
    bool gpubuf_sync = true;
    bool gpubuf_writeback = true;  // GPU writes to retained SSBOs go back to guest memory (unified memory)
    bool gpubuf_cover = true;      // a VB/SSBO spanning several own buffers (contiguous XFB/compute outputs) is served with their union
    uint64_t xfb_partial_max = 256; // partial read of an own buffer only if the VB overruns its end by at most this      // ranges partly touching an own buffer: own buffer with the union
    uint64_t gpubuf_cover_max = 8ull << 20;       // CPU writes into an own buffer's memory -> copied on top
    bool xfb_init = true;
    uint32_t chunk_draws = 300;    // submit what was recorded every N draws (0 = a single submit per job)
    uint32_t occlusion_window = 64;
    bool occlusion_precise = true;  // exact samples (false: 0/1 per query; cheaper on some GPUs)  // draws measured after each type 1 report
    int occlusion = 2;             // ReportCounter type 1: 2 real (VkQueryPool, one frame of latency), 1 simulated, 0 zero
    bool ssbo_persist = true;
    uint32_t ssbo_persist_stages = 63;  // stages (bit = NVN stage, 5 = compute) whose written SSBOs are retained
    bool ssbo_persist_rw_only = false;  // retain only SSBOs the shader both reads AND writes (in-place simulation)
    bool ssbo_cpu_page = false;         // in GPU SSBOs, if the CPU touches a page, the CPU wins for the whole page
    bool ssbo_cpu_blacklist = false;    // don't retain SSBOs whose memory the CPU also writes
    bool xfb_partial_vertex = true;   // VB starting inside an own buffer and extending past it: read from it (Q4); TRIANGLES is covered earlier by gpubuf_cover
    uint64_t counter_value_mask = 1ull << 1;  // ReportCounter types that receive counter_value (bit = type)
    uint64_t counter_value = 0x100000;          // initialize new XFB buffers with guest memory (sprint 6)      // discard XFB buffers whose guest memory the CPU rewrote
    // Background pipelines (sprint 5): threads create new VkPipelines; the draw waits at most
    // pipeline_wait_ms and, if not ready, is skipped that frame (transform feedback ones always
    // wait). pipeline_threads=0: synchronous creation as before.
    int pipeline_threads = 2;
    double pipeline_wait_ms = 4.0;
    double pipeline_frame_budget_ms = 4.0;
    double pipeline_min_wait_ms = 1.0;      // minimum wait per draw even when the budget is exhausted  // total wait per frame (pipelines + variants); after that, no wait
    uint64_t xfb_idle_frames = 0;  // free XFB buffers unused for N frames (0 = never: they're written once
                                   // and read much later; freeing them would read guest garbage)
    bool async_variants = true;    // also translation of new programs (except transform feedback)
    // GPU: empty = the discrete one (or the first); a number = vkEnumeratePhysicalDevices index;
    // other text = the first one whose name contains it.
    std::string gpu;
    double hitch_phase_ms = 50.0;  // with more submit CPU time than this in a frame, per-phase breakdown
    double hitch_ms = 25.0;        // log frames longer than this (and than 2x the average); 0 = off
    int trace_lines = 400;       // trace line limit
    double trace_sec = 0;        // the trace starts after N seconds
    std::vector<uint64_t> trace_gpus;  // "trace_rt" diagnostic: GPU addresses (hex, "/"-separated)
    bool Traced(uint64_t g) const {
        for (uint64_t t : trace_gpus)
            if (t == g) return true;
        return false;
    }
    bool skip_draws = false;    // diagnostic: execute everything except draws (no pipelines)
    bool sync_check = false;    // diagnostic: wait on every submit and log if the device is lost
    bool robust = false;         // robustBufferAccess + robustness2 (out-of-range accesses without GPU faults)
    bool dump_rts = false;       // at each capture_secs, also dump every render target of the frame
    uint64_t start_after = 0;    // shadow: start executing after N game frames
    bool execute = true;        // diagnostic: false = record without executing anything in Vulkan
    bool native_cmdmem = true;   // exclusive: the driver also handles Begin/EndRecording and command memory
    bool isolate_layers = true;  // disables implicit overlay layers in our instance
    bool import_arena = false;       // allow importing the fastmem arena (unstable; see hostmem.cpp)   // in shadow the game doesn't wait for our GPU: copy by default         // exclusive: present in the game's window (window_handle)      // debugging: inverts the viewport orientation criterion
    std::string capture_path;        // capture for the offline replayer (capture.h)
    uint64_t capture_frames = 1200, capture_max_mb = 4000;
};
extern Config g_cfg;
// FPS-1: time the game's threads spend blocked inside nvnvk hooks (ns): 0 SyncWait, 1 present (game_wait), 2 window
// acquire, 3 submit queue full / QueueFinish. Reported per frame with the `rate:` line.
extern std::atomic<uint64_t> g_game_wait_ns[4];
// Valid region of the presented texture ((w << 16) | h; 0 = all of it). render_res reports it through
// nvnvk_live_report("render_res.screen", "WxH") when the game renders a smaller screen inside render targets reserved
// at a larger size (live resolution changes, docs/port/LIVE-SETTINGS.md).
extern std::atomic<uint32_t> g_present_crop;

// ---------------------------------------------------------------- guest access
struct Guest {
    const pla_api* api = nullptr;
    bool host_ptr = false;  // pla_api 1.1: host_pointer available
    // Copies [va, va+n) to dst. ctx may be null if host_ptr.
    bool Read(pla_ctx* ctx, uint64_t va, void* dst, size_t n) const;
    bool Write(pla_ctx* ctx, uint64_t va, const void* src, size_t n) const;
    // Host pointer to guest memory (null if there is no host_pointer or it isn't contiguous).
    const uint8_t* Ptr(uint64_t va, size_t n) const;
    template <class T>
    T Get(pla_ctx* ctx, uint64_t va) const {
        T v{};
        Read(ctx, va, &v, sizeof v);
        return v;
    }
    uint64_t sdk_base = 0, main_base = 0;
};
extern Guest g_guest;
extern thread_local int t_guarded;              // >0 inside a guarded copy (objects.cpp)
extern std::atomic<uint64_t> g_guarded_faults;  // faults caught while reading/writing the arena

// ---------------------------------------------------------------- known NVN objects
struct PoolInfo {
    uint64_t obj = 0;     // the game's MemoryPool*
    uint64_t cpu = 0;     // storage (guest VA)
    uint64_t size = 0;
    uint64_t gpu = 0;     // buffer GPU address (pool+0x40, GetBufferAddress)
    uint64_t alias[2] = {};  // other GPU mappings of the same pool (+0x50, +0x60): the texture one
    uint32_t flags = 0;
    const uint8_t* host = nullptr;  // contiguous physical backing of the whole pool (or null): reads without API calls
};

class Registry {
public:
    void AddPool(const PoolInfo& p);
    void RemovePool(uint64_t obj);
    // Host (backing) pointer for [gpu, gpu+avail) if the pool has contiguous backing; otherwise null.
    const uint8_t* GpuToHost(uint64_t gpu, uint64_t* avail);
    bool PoolByObj(uint64_t obj, PoolInfo* out);
    // Translates GPU address -> guest VA (CPU). 0 if it isn't in any known pool.
    uint64_t GpuToCpu(uint64_t gpu, uint64_t* avail = nullptr, uint32_t* flags = nullptr);
    std::vector<PoolInfo> Pools();
    // Diagnoses a GpuToCpu miss: live pools containing the address (overlaps) and finalized pools that contained it.
    std::string Explain(uint64_t gpu);

private:
    std::mutex mu_;
    std::unordered_map<uint64_t, PoolInfo> by_obj_;
    struct Removed {
        PoolInfo p;
        uint64_t frame;
    };
    std::vector<Removed> removed_;  // most recently finalized pools (ring of 4096)
    size_t removed_next_ = 0;
    std::vector<PoolInfo> sorted_;  // by gpu
    void Resort();
};
extern Registry g_reg;

// Description of an NVN texture read from the game's Texture object (+ optional view).
struct TexDesc {
    uint64_t gpu = 0;         // GPU address of the data
    uint64_t obj = 0;         // Texture* (for debugging)
    uint32_t format = 0;      // NVN format
    uint32_t target = 0;      // NVN target
    uint32_t width = 0, height = 0, depth = 0;
    uint32_t levels = 1, samples = 0;
    uint32_t flags = 0;
    uint32_t stride = 0;
    // view
    uint32_t view_format = 0;  // 0 = the texture's
    uint32_t base_level = 0, num_levels = 0, base_layer = 0, num_layers = 0;
    bool valid() const { return gpu != 0 && width != 0; }
};
bool ReadTexDesc(pla_ctx* ctx, uint64_t tex, uint64_t view, TexDesc* out);

// ---------------------------------------------------------------- hooks
using PreFn = pla_action (*)(pla_ctx* ctx);
using PostFn = void (*)(pla_ctx* ctx);

// NVN function indices (nvn_funcs.inc).
enum Fn : uint32_t {
#define NVN_FN(idx, name, off, fl) FN_##name = idx,
#include "nvn_funcs.inc"
#undef NVN_FN
    FN_COUNT
};
const char* FnName(uint32_t fn);

inline uint64_t X(pla_ctx* c, unsigned n) { return g_guest.api->get_x(c, n); }
inline float S(pla_ctx* c, unsigned n) { return pla_get_s(g_guest.api, c, n); }
inline void SetX(pla_ctx* c, unsigned n, uint64_t v) { g_guest.api->set_x(c, n, v); }

// Register handlers (record.cpp, objects.cpp, queue.cpp).
void RegisterRecordHooks(PreFn* pre, PostFn* post);
void RegisterObjectHooks(PreFn* pre, PostFn* post);
void RegisterQueueHooks(PreFn* pre, PostFn* post);

// statistics
struct Stats {
    std::atomic<uint64_t> frames{0}, submits{0}, draws{0}, draws_skipped{0}, clears{0}, copies{0},
        cmds_recorded{0}, recordings{0}, presents{0}, pipelines{0}, textures{0}, uploads_bytes{0}, dispatches{0},
        submit_ns{0}, present_ns{0}, reuploads{0};
};
extern Stats g_stats;

}  // namespace nvnvk
