// nnshim::hid: pure mouse logic (speed -> right stick, wheel -> presses).
//
// Cursor capture (Win32) lives in hid_core.cpp; here only the math, with no system
// dependencies, so it can be tested in nnshim_selftest.
#include "nnshim/hid.hpp"

#include <algorithm>
#include <cmath>

namespace nnshim::hid {

void MouseStick::Update(const MouseConfig& c, float dx, float dy, float dt, int32_t& rx, int32_t& ry) {
    rx = ry = 0;
    if (!(dt > 0)) dt = 1e-3f;
    const bool moved = dx != 0 || dy != 0;
    idle_ms_ = moved ? 0 : idle_ms_ + dt * 1000.0f;
    const float raw_x = dx / dt, raw_y = dy / dt;   // px/s
    if (c.smoothing_ms <= 0) {
        vx_ = raw_x;
        vy_ = raw_y;
    } else {
        const float a = 1.0f - std::exp(-dt * 1000.0f / c.smoothing_ms);
        vx_ += a * (raw_x - vx_);
        vy_ += a * (raw_y - vy_);
    }
    // No movement for a while: snap to center (otherwise the average's tail keeps the camera turning).
    if (idle_ms_ >= static_cast<float>(std::max(c.idle_stop_ms, 1))) vx_ = vy_ = 0;
    const float speed = std::sqrt(vx_ * vx_ + vy_ * vy_);
    if (speed <= std::max(c.min_speed, 1e-3f)) return;
    float m = std::min(speed * std::max(c.sensitivity, 0.0f) / 1000.0f, 1.0f);
    if (c.curve > 0 && c.curve != 1.0f) m = std::pow(m, c.curve);
    const float ad = std::clamp(c.anti_deadzone, 0.0f, 0.9f);
    const float out = std::min(ad + (1.0f - ad) * m, 1.0f);
    const float fx = vx_ / speed * out * (c.invert_x ? -1.0f : 1.0f);
    const float fy = -vy_ / speed * out * (c.invert_y ? -1.0f : 1.0f);   // mouse down = stick down
    rx = static_cast<int32_t>(std::lround(std::clamp(fx, -1.0f, 1.0f) * 32767.0f));
    ry = static_cast<int32_t>(std::lround(std::clamp(fy, -1.0f, 1.0f) * 32767.0f));
}

void WheelPulser::AddNotches(int n) { queue_ = std::clamp(queue_ + n, -4, 4); }

uint64_t WheelPulser::Step(float dt_ms, uint64_t up_buttons, uint64_t down_buttons, int pulse_ms) {
    const float pulse = static_cast<float>(std::max(pulse_ms, 1));
    if (phase_ms_ > 0) {
        phase_ms_ -= dt_ms;
        if (phase_ms_ <= 0) {
            if (on_) {   // end of the press: released gap of the same length
                on_ = false;
                phase_ms_ = pulse;
            } else phase_ms_ = 0;
        }
    }
    if (!on_ && phase_ms_ <= 0 && queue_ != 0) {
        cur_ = queue_ > 0 ? 1 : -1;
        queue_ -= cur_;
        on_ = true;
        phase_ms_ = pulse;
    }
    return on_ ? (cur_ > 0 ? up_buttons : down_buttons) : 0;
}

void MenuMouse::Update(const MouseConfig& c, bool left, bool right, bool middle, float dx, float dy, float dt_ms,
                       uint64_t left_btn, uint64_t right_btn, uint64_t middle_btn, uint64_t& buttons, int32_t& lx,
                       int32_t& ly) {
    if (!c.menu_drag) {
        // No drag: the left click is the button as is (hold = hold).
        if (left) buttons |= left_btn;
        if (left && !left_prev_) ++clicks_;
    } else if (left) {
        if (!left_prev_) {
            drag_dist_ = 0;
            dragging_ = false;
            stick_.Reset();
        }
        drag_dist_ += std::sqrt(dx * dx + dy * dy);
        if (drag_dist_ >= static_cast<float>(std::max(c.menu_drag_threshold_px, 0))) dragging_ = true;
        if (dragging_) {
            MouseConfig sc = c;
            sc.sensitivity = c.sensitivity * c.menu_drag_sensitivity;
            int32_t sx = 0, sy = 0;
            stick_.Update(sc, dx, dy, dt_ms * 1e-3f, sx, sy);
            if (sx || sy) {
                lx = sx;
                ly = sy;
            }
        }
    } else {
        if (left_prev_ && !dragging_) {   // click without drag: short press on release
            pulse_ms_ = static_cast<float>(std::max(c.menu_click_ms, 1));
            ++clicks_;
        }
        dragging_ = false;
    }
    if (pulse_ms_ > 0) {
        buttons |= left_btn;
        pulse_ms_ -= dt_ms;
    }
    if (right) buttons |= right_btn;
    if (middle) buttons |= middle_btn;
    left_prev_ = left;
}

uint64_t ButtonsFromList(const std::string& list) {
    uint64_t m = 0;
    std::string cur;
    for (size_t i = 0; i <= list.size(); ++i) {
        const char ch = i < list.size() ? list[i] : ',';
        if (ch == '+' || ch == ',' || ch == '|' || ch == ' ') {
            if (!cur.empty()) m |= ButtonFromName(cur);
            cur.clear();
        } else cur.push_back(ch);
    }
    return m & 0xFFFFull;
}

} // namespace nnshim::hid
