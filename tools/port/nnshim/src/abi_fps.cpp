// nnshim: game fixes for unlocked FPS ("fps" family; gen_mod_json.py --fps-fixes).
//
// Particle effects (nn::vfx) frozen above 60 FPS — sprint 1 item E2.
//   The game's effect manager (main+0x2b8fe98) computes the effect speed like this:
//       frameRate = speed * (float)(int)(dt_ns / 16666666.0)      ; main+0x2b90024..0x2b9004c
//   and passes it to main+0x1837d0 (walks the groups and calls nn::vfx::System::Calc, main+0x165ebc).
//   The (int) truncates: with dt < 16.67 ms (above 60 FPS) frameRate = 0 and emitters do not advance;
//   that is why, without a limiter, sending out a creature and moves showed no dust, sparkles or
//   impacts (with --limiter at 60 they did). At 45 FPS they also ran 25% slow (trunc(1.33) = 1).
//   Fix: "pre" hook at main+0x1837d0 (its only caller is that manager) that recomputes V0 without
//   truncation, from the same data as the original: X20 = group node (+0x18 group, +0x1c paused,
//   +0x20 speed), and the frame time from the game clock (main+0x4350F08, ns). Keeps the original's two
//   intentional zeros: the paused node and group 0x3f with its flag (W8 at the call).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "abi_common.hpp"

using namespace nnshim;

namespace {

constexpr uint64_t kVfxCallerRet = 0x2b900e8;   // return address of the bl to 0x1837d0
constexpr uint64_t kFrameNs = 0x4350F08;        // g.frame_ns of the game clock (06-framerate.md §2)
uint64_t g_main = 0;

void InitFps(const cfg::Value&) { g_main = g_api->module_base("main"); }
ModuleHooks g_fps_module{"fps", &InitFps, nullptr};

float GetS0(pla_ctx* ctx) {
    uint64_t v[2] = {};
    g_api->get_v(ctx, 0, v);
    float f;
    std::memcpy(&f, &v[0], 4);
    return f;
}

} // namespace

// ---- Input manager button repeat (sprint 1 item E1) ---------------------------------------------
// main+0xc9a9b4 (input manager update, once per frame; X0 = manager) computes the "repeat" bits
// (+0xb8; Lua IsPadRepeat = repeat | press) with a FRAME counter (+0x18): when the buttons change it is
// loaded with the initial delay (+0x10) and then with the interval (+0x14). The values are frames of
// the original game (30 FPS): at 75 FPS a 70 ms tap already repeated, and in the bag menu one ZL/ZR
// press switched two tabs (measured: one with `--limiter`, two unlimited).
// Fix: before each update, +0x10/+0x14 are scaled by (33.3 ms / frame dt) so the delay and the
// interval last as long in real time as at 30 FPS. Base values are captured the first time and
// whenever the game changes them (if the current value is not the last one the shim wrote).
namespace {
struct RepeatBase {
    int32_t base_delay = 0, base_interval = 0;
    int32_t last_delay = -1, last_interval = -1;
};
std::mutex g_rep_mu;
std::map<uint64_t, RepeatBase> g_rep;
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_PadRepeatScale) {
    PlaGuest g(ctx);
    if (!g_main) return PLA_CONTINUE;
    const uint64_t mgr = g.x(0);
    int32_t cur[2] = {};
    uint64_t frame_ns = 0;
    if (!mgr || g.read(mgr + 0x10, cur, sizeof cur) != sizeof cur || !g.load(g_main + kFrameNs, frame_ns) ||
        frame_ns == 0)
        return PLA_CONTINUE;
    int32_t want[2];
    {
        std::lock_guard lk(g_rep_mu);
        RepeatBase& b = g_rep[mgr];
        if (cur[0] != b.last_delay || cur[1] != b.last_interval) {   // first time, or the game changed it
            b.base_delay = cur[0];
            b.base_interval = cur[1];
        }
        const double k = 33333333.0 / static_cast<double>(frame_ns);   // current frames per 30 FPS frame
        auto scale = [k](int32_t v) {
            if (v <= 0) return v;
            const double s = std::round(v * k);
            return static_cast<int32_t>(s < 1.0 ? 1.0 : (s > 100000.0 ? 100000.0 : s));
        };
        want[0] = scale(b.base_delay);
        want[1] = scale(b.base_interval);
        b.last_delay = want[0];
        b.last_interval = want[1];
    }
    if (want[0] != cur[0] || want[1] != cur[1]) g.write(mgr + 0x10, want, sizeof want);
    CounterAdd("fps.repeat.frames");
    return PLA_CONTINUE;
}

// void FUN_71001837d0(float frameRate /*S0*/, vfx::System* sys, int group, int mode)
NNSHIM_ADAPTER(Fps, nnshim_fps_VfxCalcRate) {
    PlaGuest g(ctx);
    if (!g_main || g.x(30) != g_main + kVfxCallerRet) return PLA_CONTINUE;
    const uint64_t node = g.x(20);
    uint8_t paused = 1;
    int32_t group = 0;
    float speed = 0.0f;
    uint64_t frame_ns = 0;
    if (!g.load(node + 0x1c, paused) || !g.load(node + 0x18, group) || !g.load(node + 0x20, speed) ||
        !g.load(g_main + kFrameNs, frame_ns))
        return PLA_CONTINUE;
    if (paused) return PLA_CONTINUE;                                   // intentional zero
    if (group == 0x3f && (g.x(8) & 0xff) != 0) return PLA_CONTINUE;   // intentional zero (fcsel)
    const float before = GetS0(ctx);
    const float rate = speed * static_cast<float>(static_cast<double>(frame_ns) / 16666666.0);
    uint32_t bits;
    std::memcpy(&bits, &rate, 4);
    g_api->set_v(ctx, 0, bits, 0);
    CounterAdd(before == 0.0f && rate > 0.0f ? "fps.vfx.rescued" : "fps.vfx.corrected");
    return PLA_CONTINUE;
}

// ---- Battle sequences (.wbin) — sprint 4 item U1 ------------------------------------------------
// 0x330-byte sequence object (vtable 0x40d3338, created by main+0x9db43c for bin/effect/battle/sequence/*.wbin).
// Its update, vf11 = main+0x9dbeac, advances the timeline frame like this:
//       frame(+0xe8) = min(frame + speed(+0xf0), end(+0xec))        ; main+0x9dc414..
// once per call, with speed = 1.0 = one 30 FPS frame. Models animate on the real clock, so at
// 92 FPS the camera and the creature's send-out ran 3x faster than the character's ball-throw
// animation (the creature appeared before the throw; measured with screenshot bursts, U1).
// Fix: "pre" on vf11 that sets speed = base × (real time since that object's previous call / 33.3 ms)
// and "post" that restores the base (other readers of +0xf0 see it untouched). speed 0 (paused) is left alone.
namespace {
struct SeqClock {
    std::chrono::steady_clock::time_point last{};
};
std::mutex g_seq_mu;
std::map<uint64_t, SeqClock> g_seq;
std::chrono::steady_clock::time_point g_seq_prune{};
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_SeqStep) {
    PlaGuest g(ctx);
    if (!g_main) return PLA_CONTINUE;
    const uint64_t obj = g.x(0);
    float base = 0.0f;
    uint64_t frame_ns = 0;
    if (!obj || !g.load(obj + 0xf0, base) || !(base > 0.0f) || !g.load(g_main + kFrameNs, frame_ns))
        return PLA_CONTINUE;
    const auto now = std::chrono::steady_clock::now();
    double dt_ns = static_cast<double>(frame_ns);
    {
        std::lock_guard lk(g_seq_mu);
        SeqClock& c = g_seq[obj];
        if (c.last.time_since_epoch().count() != 0) {
            const double d = std::chrono::duration<double, std::nano>(now - c.last).count();
            // Long gap (sequence stopped, loading): one game-clock frame, as if it had continued.
            dt_ns = d > 100e6 ? static_cast<double>(frame_ns) : d;
        }
        c.last = now;
        if (now - g_seq_prune > std::chrono::seconds(10)) {
            for (auto it = g_seq.begin(); it != g_seq.end();)
                it = (now - it->second.last > std::chrono::seconds(10)) ? g_seq.erase(it) : std::next(it);
            g_seq_prune = now;
        }
    }
    if (!(dt_ns > 0.0)) dt_ns = static_cast<double>(frame_ns);
    const float want = base * static_cast<float>(std::min(dt_ns, 100e6) / 33333333.0);
    uint32_t bb, wb;
    std::memcpy(&bb, &base, 4);
    std::memcpy(&wb, &want, 4);
    uint64_t* cd = g_api->call_data(ctx);
    cd[0] = obj;
    cd[1] = bb;
    cd[2] = wb;
    g.write(obj + 0xf0, &want, 4);
    CounterAdd("fps.seq.steps");
    return PLA_CONTINUE;
}

NNSHIM_EXPORT void nnshim_fps_SeqStep_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g_api->call_data(ctx);
    if (!cd[0]) return;
    uint32_t cur = 0;
    if (!g.load(cd[0] + 0xf0, cur) || cur != static_cast<uint32_t>(cd[2])) return;   // the game changed it
    const uint32_t base = static_cast<uint32_t>(cd[1]);
    g.write(cd[0] + 0xf0, &base, 4);
}

// ---- Thrown balls / throwables in flight -------------------------------------------------------
// The flight update (vtable_7104137528::vf17 = main+0x1a7191c, state +0x5f8 == 1) rewrites the Havok
// body's velocity ONCE PER FRAME with an air drag and an upward lift, without dt:
//       h = |v with y = 0|;  v = (v + (0,1,0,0) * k * h * f) * (1 - f * a)       ; main+0x1a71b90..0x1a71bf0
// a = +0x5a0 (S8), k = +0x5a4 (S9), f = time scale (vf6, stored at the caller's [sp]), then
// setLinearVelocity (main+0x2adb28c, bl at 0x1a71bf8). Havok integrates gravity and position with the
// real dt, so at 100 FPS the drag and the lift were applied 3.3x per second: the ball floated down
// slowly and fell short.
// Fix: "pre" at main+0x2adb28c, only for that call site (LR): undoes the per-frame step on the vector
// at X1 and reapplies it for dt: drag c^(dt*30), lift * (dt*30). With dt = 1/30 it is the original.
namespace {
constexpr uint64_t kThrowSetVelRet = 0x1a71bfc;   // return address of the bl at 0x1a71bf8

float GetSn(PlaGuest& g, unsigned n) {
    uint64_t v[2] = {};
    g.get_v(n, v);
    float f;
    std::memcpy(&f, &v[0], 4);
    return f;
}
} // namespace

// void setLinearVelocity(body /*X0*/, const hkVector4* v /*X1*/)
NNSHIM_ADAPTER(Fps, nnshim_fps_ThrowDrag) {
    PlaGuest g(ctx);
    if (!g_main || g.x(30) != g_main + kThrowSetVelRet) return PLA_CONTINUE;
    const uint64_t pv = g.x(1);
    float v[4], f = 0.0f;
    uint64_t frame_ns = 0;
    if (g.read(pv, v, sizeof v) != sizeof v || !g.load(pv - 0x10, f) || !g.load(g_main + kFrameNs, frame_ns) || frame_ns == 0)
        return PLA_CONTINUE;
    CounterAdd("fps.throw.flight_us", frame_ns / 1000);   // real flight time (A/B measurement)
    static const bool off = std::getenv("NNSHIM_THROW_DRAG_OFF") != nullptr;   // measure only
    if (off) return PLA_CONTINUE;
    const float a = GetSn(g, 8), k = GetSn(g, 9);
    const float c = 1.0f - f * a;
    if (!(c > 1e-6f) || !(c <= 1.0f)) return PLA_CONTINUE;
    const float s = static_cast<float>(std::min(static_cast<double>(frame_ns), 100e6) / 33333333.0);
    const float vx = v[0] / c, vz = v[2] / c, vw = v[3] / c;     // the lift only touches y
    const float lift = k * std::sqrt(vx * vx + vz * vz + vw * vw) * f;
    const float vy = v[1] / c - lift;
    const float cs = std::pow(c, s);
    const float out[4] = {vx * cs, (vy + lift * s) * cs, vz * cs, vw * cs};
    g.write(pv, out, sizeof out);
    CounterAdd("fps.throw.drag");
    return PLA_CONTINUE;
}

// ---- AI behavior tree: frames since the last evaluation -----------------------------------------
// The behavior tree context (FUN_7101050e00) counts, since the node's last evaluation, frames (+0x88,
// int, += 1 per frame) and seconds (+0x84, float, += clock ns / 1e9). The getter main+0x1059a78
// (`ldr w0,[x0,#0x88]; ret`) gives the frames to 17 nodes (0xaf2xxx..0xb55xxx): WaitFrame
// (+0x13c -= n), TurnFrames (+0x144 += n) and gauges `v += rate * n` (FUN_7101aa0150..0x7101aa07e8).
// They are 30 FPS frames: at 100 FPS waits, turns and gauges ran 3.3x faster.
// Fix: "pre" that puts n = 30 FPS frames of the elapsed seconds (with a per-context fractional carry)
// in +0x88 and "post" that restores the game's value. Several nodes read the same context in one
// evaluation: same seconds and < 2 ms apart = same evaluation, same n (the carry advances once).
namespace {
struct AiFrames {
    std::chrono::steady_clock::time_point last{};
    uint32_t sec_bits = 0;
    int32_t n = 0;
    double carry = 0.0;
};
std::mutex g_ai_mu;
std::map<uint64_t, AiFrames> g_ai;
std::chrono::steady_clock::time_point g_ai_prune{};
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_AiFrames) {
    PlaGuest g(ctx);
    uint64_t* cd = g_api->call_data(ctx);
    cd[0] = 0;
    const uint64_t c = g.x(0);
    struct {
        float sec;
        int32_t frames;
    } cur{};
    if (!c || g.read(c + 0x84, &cur, sizeof cur) != sizeof cur || cur.frames <= 0 || !(cur.sec >= 0.0f) ||
        cur.sec > 10.0f)
        return PLA_CONTINUE;
    uint32_t bits;
    std::memcpy(&bits, &cur.sec, 4);
    const auto now = std::chrono::steady_clock::now();
    int32_t n;
    {
        std::lock_guard lk(g_ai_mu);
        AiFrames& a = g_ai[c];
        if (a.sec_bits == bits && now - a.last < std::chrono::milliseconds(2)) {
            n = a.n;   // another node of the same evaluation
        } else {
            const double f = a.carry + cur.sec * 30.0;
            n = static_cast<int32_t>(std::floor(f));
            a.carry = std::min(f - n, 1.0);
            a.n = n;
            a.sec_bits = bits;
        }
        a.last = now;
        if (now - g_ai_prune > std::chrono::seconds(10)) {
            for (auto it = g_ai.begin(); it != g_ai.end();)
                it = (now - it->second.last > std::chrono::seconds(10)) ? g_ai.erase(it) : std::next(it);
            g_ai_prune = now;
        }
    }
    if (n == cur.frames) return PLA_CONTINUE;
    cd[0] = c;
    cd[1] = static_cast<uint32_t>(cur.frames);
    cd[2] = static_cast<uint32_t>(n);
    g.write(c + 0x88, &n, 4);
    CounterAdd("fps.ai.frames");
    return PLA_CONTINUE;
}

NNSHIM_EXPORT void nnshim_fps_AiFrames_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g_api->call_data(ctx);
    if (!cd[0]) return;
    int32_t cur = 0;
    if (!g.load(cd[0] + 0x88, cur) || cur != static_cast<int32_t>(cd[2])) return;
    const int32_t orig = static_cast<int32_t>(cd[1]);
    g.write(cd[0] + 0x88, &orig, 4);
}

// ---- Map cursor ----------------------------------------------------------------------------------
// The map screens move the cursor by pos += stick * speed per frame, without dt, and pass the new
// position to their setter (clamp + layout):
//   vtable_71041047f8::vf17 (main+0x120f1f4): pos +0xd60/+0xd64, speed +0xd70 -> bl 0x1212d60 at 0x120f49c
//   vtable_7104106768::vf17 (main+0x1245db8): pos +0x95c/+0x960, speed +0x970 -> bl 0x124a394 at 0x1246304
// At 100 FPS the cursor flew 3.3x faster. Fix: "pre" on both setters, only from those call sites (LR):
// new = old + (new - old) * dt * 30, in the {x, y} at X1.
namespace {
struct CursorSite {
    uint64_t ret, pos;
};
constexpr CursorSite kCursorSites[] = {{0x120f4a0, 0xd60}, {0x1246308, 0x95c}};
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_MapCursor) {
    PlaGuest g(ctx);
    if (!g_main) return PLA_CONTINUE;
    const uint64_t lr = g.x(30) - g_main;
    const CursorSite* site = nullptr;
    for (const auto& s : kCursorSites)
        if (lr == s.ret) site = &s;
    if (!site) return PLA_CONTINUE;
    float nw[2], old[2];
    uint64_t frame_ns = 0;
    if (g.read(g.x(1), nw, sizeof nw) != sizeof nw || g.read(g.x(0) + site->pos, old, sizeof old) != sizeof old ||
        !g.load(g_main + kFrameNs, frame_ns) || frame_ns == 0)
        return PLA_CONTINUE;
    const float s = static_cast<float>(std::min(static_cast<double>(frame_ns), 100e6) / 33333333.0);
    const float out[2] = {old[0] + (nw[0] - old[0]) * s, old[1] + (nw[1] - old[1]) * s};
    g.write(g.x(1), out, sizeof out);
    CounterAdd("fps.map.cursor");
    return PLA_CONTINUE;
}

// ---- Climbing mount: stop easing ------------------------------------------------------
// vtable_7104163ae0::vf18 (main+0x2549fa0), state +0xd00 == 2: +0xd04 += 1 per frame against the
// parameter "climbing_move_stop_frame" (30 FPS frames) and eases +0xd50.. toward +0xd60.. with it.
// Fix: "pre" that, while in state 2, keeps the 30 FPS frames of real time and leaves +0xd04 at
// floor(acc) - 1 (the original then adds 1).
// The same function also drives the climbing animation: +0xd50.. eases toward the per-frame displacement
// +0xd60.. and r = |+0xd50| / (speed / 30) blends idle -> climb (r < 1) or speeds the cycle up
// (r > 1). With ClimbMove the displacement is per real frame, so at 100 FPS r ~= 0.3 and the legs crawled.
// Fix: the pre also scales +0xd50.. and +0xd60.. by 1 / (dt * 30) (displacement per 30 FPS frame) and the
// post scales them back (nothing else in the function reads them; the ease is linear).
namespace {
std::mutex g_climb_mu;
struct ClimbAcc {
    double acc = 0.0;
    int32_t written = 0;
};
std::map<uint64_t, ClimbAcc> g_climb;

// Debug: NNSHIM_CLIMB_LOG=<file> logs the climbing values per call.
FILE* ClimbLog() {
    static FILE* f = [] { const char* p = std::getenv("NNSHIM_CLIMB_LOG"); return p ? std::fopen(p, "w") : nullptr; }();
    return f;
}
double ClimbT() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
float Len4(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]); }

// +0xd50..+0xd6f (eased and target displacement) *= k
bool ScaleClimbDisp(PlaGuest& g, uint64_t obj, float k) {
    float v[8];
    if (g.read(obj + 0xd50, v, sizeof v) != sizeof v) return false;
    for (float& c : v) c *= k;
    g.write(obj + 0xd50, v, sizeof v);
    return true;
}
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_ClimbStop) {
    PlaGuest g(ctx);
    uint64_t* cd = g_api->call_data(ctx);
    cd[0] = 0;
    if (!g_main) return PLA_CONTINUE;
    const uint64_t obj = g.x(0);
    int32_t state = 0, cnt = 0;
    uint64_t frame_ns = 0;
    if (!obj || !g.load(obj + 0xd00, state) || !g.load(obj + 0xd04, cnt) || !g.load(g_main + kFrameNs, frame_ns) ||
        frame_ns == 0)
        return PLA_CONTINUE;
    const float k = static_cast<float>(33333333.0 / std::clamp(static_cast<double>(frame_ns), 1e6, 100e6));
    if (FILE* lf = ClimbLog()) {
        float v[8] = {};
        g.read(obj + 0xd50, v, sizeof v);
        std::fprintf(lf, "S %.4f %llu st=%d c=%d d50=%.5f d60=%.5f\n", ClimbT(), (unsigned long long)frame_ns, state, cnt,
                     Len4(v), Len4(v + 4));
    }
    if (k != 1.0f && ScaleClimbDisp(g, obj, k)) {
        cd[0] = obj;
        std::memcpy(&cd[1], &k, sizeof k);
    }
    std::lock_guard lk(g_climb_mu);
    if (state != 2) {
        g_climb.erase(obj);
        return PLA_CONTINUE;
    }
    auto it = g_climb.find(obj);
    // Entered state 2, or the game wrote the counter (neither our value nor our value + its 1).
    if (it == g_climb.end() || (cnt != it->second.written && cnt != it->second.written + 1))
        it = g_climb.insert_or_assign(obj, ClimbAcc{static_cast<double>(cnt), cnt}).first;
    ClimbAcc& a = it->second;
    a.acc += std::min(static_cast<double>(frame_ns), 100e6) / 33333333.0;
    const int32_t want = std::max(static_cast<int32_t>(std::floor(a.acc)) - 1, cnt - 1);
    a.written = want;
    if (want != cnt) g.write(obj + 0xd04, &want, 4);
    CounterAdd("fps.climb.stop");
    return PLA_CONTINUE;
}

NNSHIM_EXPORT void nnshim_fps_ClimbStop_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g_api->call_data(ctx);
    if (!cd[0]) return;
    float k = 1.0f;
    std::memcpy(&k, &cd[1], sizeof k);
    ScaleClimbDisp(g, cd[0], 1.0f / k);
    CounterAdd("fps.climb.anim");
}

// ---- Climbing mount: climbing movement ------------------------------------------------
// FUN_71025540e8 (climbing move, called from the ride controller's Lua binding once per frame) turns the
// move speed parameter (units/s, hashed table entry +0x30, S8) into a per-frame displacement:
//       disp = dir * (speed / 30.0)          ; main+0x25543dc..0x25543f4 -> [x29-0xb0] and [sp+0x90]
// then calls main+0x199b800 (bl at 0x255440c) and rotates [sp+0x90] by the result (0x2554530..0x2554638).
// At 100 FPS that 1/30 s step per frame climbed 3.3x faster. Fix: "pre" at main+0x199b800, only from that
// call (LR): both stored copies of disp *= dt * 30 (the rotation is linear).
namespace {
constexpr uint64_t kClimbMoveRet = 0x2554410;   // return address of the bl at 0x255440c
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_ClimbMove) {
    PlaGuest g(ctx);
    if (!g_main || g.x(30) != g_main + kClimbMoveRet) return PLA_CONTINUE;
    uint64_t frame_ns = 0;
    if (!g.load(g_main + kFrameNs, frame_ns) || frame_ns == 0) return PLA_CONTINUE;
    const float s = static_cast<float>(std::min(static_cast<double>(frame_ns), 100e6) / 33333333.0);
    for (const uint64_t va : {g.x(29) - 0xb0, g.sp() + 0x90}) {
        float v[4];
        if (g.read(va, v, sizeof v) != sizeof v) return PLA_CONTINUE;
        if (FILE* lf = ClimbLog(); lf && va == g.sp() + 0x90)
            std::fprintf(lf, "M %.4f %llu disp=%.5f\n", ClimbT(), (unsigned long long)frame_ns, Len4(v));
        for (float& c : v) c *= s;
        g.write(va, v, sizeof v);
    }
    CounterAdd("fps.climb.move");
    return PLA_CONTINUE;
}

// ---- Script parameters counted in frames (AppConfig "*_frame") -----------------------------------------
// The player and ride scripts (haxe -> Lua 5.3) count frames: e.g. the climbing ride's climb start adds 1 to a
// frame counter every frame against AppConfig.GetIntValue(<ride>, "up_climbing_start_move_end_frame") and eases
// the position with now / end.
// Those parameters are 30 FPS frames, so at 130 FPS every wait, blend and ease built on them ran 4.3x faster
// (the climb start looked instantaneous; buddy reactions, look-around intervals, button buffers...).
// Fix: "pre"/"post" on the script getters GetIntValue (main+0xd9e8dc) and GetFloatValue (main+0xd9f2d0), the
// lua_CFunctions behind AppConfig.Get*Value(file, key): when the key names a frame count the returned number
// is scaled by (current FPS / 30), so the per-frame counters reach it after the same real time.
// Lua 5.3 layout: lua_State+0x10 = top; TValue = {value u64, tt i32} (16 bytes); TString data at +0x18.
namespace {
constexpr int kLuaTNumInt = 0x13, kLuaTNumFlt = 0x03;

bool IsFrameKey(const char* k) {
    const size_t n = std::strlen(k);
    const auto ends = [&](const char* s) {
        const size_t m = std::strlen(s);
        return n >= m && std::memcmp(k + n - m, s, m) == 0;
    };
    return ends("_frame") || ends("_frame_min") || ends("_frame_max") || ends("_frame_air") ||
           (std::strstr(k, "_frame_") != nullptr && !std::strstr(k, "framerate"));
}

bool LuaString(PlaGuest& g, uint64_t tv, char* out, size_t n) {
    uint64_t ts = 0;
    int32_t tt = 0;
    out[0] = 0;
    if (!g.load(tv, ts) || !g.load(tv + 8, tt) || (tt & 0xf) != 4 || !ts) return false;
    if (g.read(ts + 0x18, out, n - 1) == 0) return false;
    out[n - 1] = 0;
    return true;
}

// 30 FPS frames per real frame (1 at 30 FPS, 4.3 at 130 FPS), from the game clock.
double FrameScale(PlaGuest& g) {
    uint64_t frame_ns = 0;
    if (!g_main || !g.load(g_main + kFrameNs, frame_ns) || frame_ns == 0) return 1.0;
    return 33333333.0 / std::clamp(static_cast<double>(frame_ns), 1e6, 66666667.0);
}
} // namespace

NNSHIM_ADAPTER(Fps, nnshim_fps_ScriptFrames) {
    PlaGuest g(ctx);
    uint64_t* cd = g_api->call_data(ctx);
    cd[0] = 0;
    const uint64_t L = g.x(0);
    uint64_t top = 0;
    char key[80];
    if (!L || !g.load(L + 0x10, top) || !LuaString(g, top - 16, key, sizeof key) || !IsFrameKey(key))
        return PLA_CONTINUE;
    cd[0] = L;
    return PLA_CONTINUE;
}

NNSHIM_EXPORT void nnshim_fps_ScriptFrames_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    const uint64_t* cd = g_api->call_data(ctx);
    if (!cd[0]) return;
    uint64_t top = 0;
    int32_t tt = 0;
    if (!g.load(cd[0] + 0x10, top) || !g.load(top - 8, tt)) return;
    const double s = FrameScale(g);
    if (tt == kLuaTNumInt) {
        int64_t v = 0;
        if (!g.load(top - 16, v) || v <= 0 || v > 100000) return;
        const int64_t scaled = std::max<int64_t>(1, std::llround(static_cast<double>(v) * s));
        g.write(top - 16, &scaled, 8);
    } else if (tt == kLuaTNumFlt) {
        double v = 0;
        if (!g.load(top - 16, v) || !(v > 0.0) || v > 100000.0) return;
        v *= s;
        g.write(top - 16, &v, 8);
    } else {
        return;
    }
    CounterAdd("fps.script.frames");
}
