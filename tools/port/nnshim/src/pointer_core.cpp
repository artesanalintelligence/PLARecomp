// nnshim::ptr: pure logic of the mouse pointer in the game's UI (see include/nnshim/pointer.hpp).
#include "nnshim/pointer.hpp"

#include <algorithm>
#include <cmath>

namespace nnshim::ptr {

Rect PaneGlobalRect(const Mat34& g, float w, float h, uint8_t bp) {
    // Local top-left corner (nn::ui2d::Pane::GetVtxPos, main+0x14430c).
    float x0 = 0, y0 = 0;
    switch (bp & 3) {
    case 0: x0 = -w * 0.5f; break;
    case 2: x0 = -w; break;
    default: x0 = 0; break;
    }
    switch ((bp >> 2) & 3) {
    case 0: y0 = h * 0.5f; break;
    case 2: y0 = h; break;
    default: y0 = 0; break;
    }
    const float xs[2] = {x0, x0 + w}, ys[2] = {y0 - h, y0};
    Rect r{1e30f, 1e30f, -1e30f, -1e30f};
    for (float x : xs)
        for (float y : ys) {
            const float gx = g.m[0][0] * x + g.m[0][1] * y + g.m[0][3];
            const float gy = g.m[1][0] * x + g.m[1][1] * y + g.m[1][3];
            r.x0 = std::min(r.x0, gx);
            r.x1 = std::max(r.x1, gx);
            r.y0 = std::min(r.y0, gy);
            r.y1 = std::max(r.y1, gy);
        }
    return r;
}

Rect GlobalToScreen(const Rect& r, const Proj& p, float fw, float fh) {
    auto map = [&](float x, float y, float& ox, float& oy) {
        if (p.valid) {
            const float cx = p.m[0][0] * x + p.m[0][1] * y + p.m[0][3];
            const float cy = p.m[1][0] * x + p.m[1][1] * y + p.m[1][3];
            float cw = p.m[3][0] * x + p.m[3][1] * y + p.m[3][3];
            if (std::fabs(cw) < 1e-6f) cw = 1;
            ox = (cx / cw + 1.0f) * 0.5f;
            oy = (1.0f - cy / cw) * 0.5f;
        } else {
            ox = x / fw + 0.5f;
            oy = 0.5f - y / fh;
        }
    };
    float ax, ay, bx, by;
    map(r.x0, r.y0, ax, ay);
    map(r.x1, r.y1, bx, by);
    return Rect{std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
}

Rect Intersect(const Rect& a, const Rect& b) {
    return Rect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

uint64_t Fnv1a64(const char* s, size_t n) {
    uint64_t h = 0xcbf29ce484222645ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(s[i]);
        h *= 0x100000001b3ull;
    }
    return h;
}

int HitTest(const Snapshot& s, float x, float y) {
    int best = -1, best_layer = -1;
    float best_d = 1e30f;
    for (size_t i = 0; i < s.items.size(); ++i) {
        const Rect& r = s.items[i].r;
        if (r.W() <= 0 || r.H() <= 0 || !r.Contains(x, y)) continue;
        const float d = std::max(std::fabs(x - r.Cx()) / r.W(), std::fabs(y - r.Cy()) / r.H());
        if (s.items[i].layer > best_layer || (s.items[i].layer == best_layer && d < best_d)) {
            best_layer = s.items[i].layer;
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int SelectedIn(const Snapshot& s, uint64_t group) {
    for (size_t i = 0; i < s.items.size(); ++i)
        if (s.items[i].group == group && s.items[i].selected) return static_cast<int>(i);
    return -1;
}

namespace {
int FindId(const Snapshot& s, uint64_t id, uint64_t group) {
    for (size_t i = 0; i < s.items.size(); ++i)
        if (s.items[i].id == id && s.items[i].group == group) return static_cast<int>(i);
    return -1;
}
int GroupSize(const Snapshot& s, uint64_t group) {
    int n = 0;
    for (const Item& it : s.items) n += it.group == group ? 1 : 0;
    return n;
}
} // namespace

void Navigator::Reset() {
    const uint64_t p = presses_, a = accepts_;
    *this = Navigator{};
    presses_ = p;
    accepts_ = a;
}

void Navigator::Step(const Snapshot& s, const In& in, const NavConfig& c, Out& out) {
    out = Out{};
    const int hv = in.has_pos ? HitTest(s, in.x, in.y) : -1;
    out.hover = hv;
    const uint64_t hid = hv >= 0 ? s.items[hv].id : 0, hg = hv >= 0 ? s.items[hv].group : 0;
    const bool moved = in.has_pos && (std::fabs(in.x - last_x_) > 1e-4f || std::fabs(in.y - last_y_) > 1e-4f);
    if (in.has_pos) {
        last_x_ = in.x;
        last_y_ = in.y;
    }
    if (hid != hover_id_ || hg != hover_group_) {
        hover_id_ = hid;
        hover_group_ = hg;
        dwell_ = 0;
        hover_by_motion_ = moved;   // content that scrolls under a still pointer does not retarget
    } else {
        dwell_ += in.dt_ms;
        if (moved) hover_by_motion_ = true;
    }

    auto set_target = [&](uint64_t id, uint64_t group) {
        if (id != target_id_ || group != target_group_) {
            fail_ = 0;
            swap_axis_ = false;
        }
        target_id_ = id;
        target_group_ = group;
        const int i = FindId(s, id, group);
        if (i >= 0) {
            target_cx_ = s.items[i].r.Cx();
            target_cy_ = s.items[i].r.Cy();
        }
    };
    if (in.click) {
        if (hv >= 0) {
            set_target(hid, hg);
            pending_accept_ = true;
            out.click_consumed = true;
        } else if (s.has_stick_cursor && st_ == St::Idle) {
            out.click_consumed = true;
            if (s.stick_cursor.Contains(in.x, in.y)) {   // on the cursor: accept
                stick_active_ = false;
                press_btn_ = c.accept;
                press_accept_ = true;
                phase_ms_ = static_cast<float>(std::max(c.accept_ms, 1));
                st_ = St::Pressing;
                press_seq_ = s.seq;
                from_sel_ = 0;
                ++accepts_;
                out.buttons = c.accept;
                return;
            }
            stick_active_ = true;
            stick_tx_ = in.x;
            stick_ty_ = in.y;
            stick_ms_ = 0;
        } else if (!s.items.empty() && !c.outside_click_accept) {
            out.click_consumed = true;
        }
    }
    if (stick_active_) {   // drive the stick cursor towards the click
        stick_ms_ += in.dt_ms;
        if (!s.has_stick_cursor || stick_ms_ > static_cast<float>(c.stick_timeout_ms)) {
            stick_active_ = false;
        } else {
            const float dx = (stick_tx_ - s.stick_cursor.Cx()) * c.aspect, dy = stick_ty_ - s.stick_cursor.Cy();
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < c.stick_done) {
                stick_active_ = false;
            } else {
                const float m = std::clamp(d * c.stick_gain, c.stick_min, 1.0f);
                out.stick = true;
                out.lx = static_cast<int32_t>(std::lround(dx / d * m * 32767.0f));
                out.ly = static_cast<int32_t>(std::lround(-dy / d * m * 32767.0f));   // screen y down, stick y up
                return;
            }
        }
    }
    if (!pending_accept_) {
        if (c.hover_select && hv >= 0 && hover_by_motion_ && dwell_ >= static_cast<float>(c.dwell_ms)) set_target(hid, hg);
        else if (hv < 0 && st_ == St::Idle) target_id_ = 0;
    }

    auto start = [&](uint64_t btn, bool accept) {
        press_btn_ = btn;
        press_accept_ = accept;
        phase_ms_ = static_cast<float>(std::max(accept ? c.accept_ms : c.press_ms, 1));
        st_ = St::Pressing;
        press_seq_ = s.seq;
        const int sel = SelectedIn(s, target_group_);
        from_sel_ = sel >= 0 ? s.items[sel].id : 0;
        ++presses_;
        out.buttons = btn;
    };

    switch (st_) {
    case St::Pressing:
        out.buttons = press_btn_;
        phase_ms_ -= in.dt_ms;
        if (phase_ms_ <= 0) {
            st_ = St::Settling;
            phase_ms_ = 0;
        }
        return;
    case St::Settling: {
        phase_ms_ += in.dt_ms;
        const int sel = SelectedIn(s, target_group_);
        const uint64_t cur = sel >= 0 ? s.items[sel].id : 0;
        const bool moved = cur != from_sel_;
        const bool frames = s.seq >= press_seq_ + static_cast<uint64_t>(std::max(c.min_frames, 0));
        const bool done = moved ? frames && phase_ms_ >= static_cast<float>(c.gap_ms) : phase_ms_ >= static_cast<float>(c.settle_ms);
        if (!done) return;
        if (!press_accept_) {
            if (moved) fail_ = 0;
            else {
                ++fail_;
                swap_axis_ = !swap_axis_;
            }
        }
        st_ = St::Idle;
        break;
    }
    case St::Idle: break;
    }

    if (!target_id_) return;
    const int ti = FindId(s, target_id_, target_group_);
    if (ti < 0) {   // the target is gone (screen changed or scrolled away)
        target_id_ = 0;
        pending_accept_ = false;
        return;
    }
    {   // the list scrolled and the target moved away from where it was chosen: drop it
        const Rect& tr = s.items[ti].r;
        if (std::fabs(tr.Cx() - target_cx_) > 0.5f * tr.W() || std::fabs(tr.Cy() - target_cy_) > 0.5f * tr.H()) {
            target_id_ = 0;
            pending_accept_ = false;
            hover_by_motion_ = false;
            return;
        }
    }
    const int sel = SelectedIn(s, target_group_);
    if (sel < 0 && GroupSize(s, target_group_) > 1) {
        // A list without highlight is not the active one (e.g. the inventory grid under its context menu): ignore.
        target_id_ = 0;
        pending_accept_ = false;
        return;
    }
    if (sel < 0 || s.items[sel].id == target_id_) {
        // Reached (or a lone button without highlight: then the click is a plain accept).
        if (pending_accept_ && !s.items[ti].no_accept) {
            ++accepts_;
            start(s.items[ti].accept ? s.items[ti].accept : c.accept, true);
        }
        pending_accept_ = false;
        target_id_ = 0;
        return;
    }
    if (fail_ >= c.max_fail) {   // the d-pad does not get there (e.g. another element of the screen in between)
        target_id_ = 0;
        pending_accept_ = false;
        return;
    }
    const Rect& h = s.items[sel].r;
    const Rect& t = s.items[ti].r;
    const float dx = (t.Cx() - h.Cx()) * c.aspect, dy = t.Cy() - h.Cy();
    const bool aligned_x = std::fabs(t.Cx() - h.Cx()) < 0.5f * std::min(t.W(), h.W());
    const bool aligned_y = std::fabs(dy) < 0.5f * std::min(t.H(), h.H());
    bool vertical = !aligned_y && (aligned_x || std::fabs(dy) >= std::fabs(dx));
    if (swap_axis_ && !aligned_x && !aligned_y) vertical = !vertical;
    if (aligned_x && aligned_y) vertical = std::fabs(dy) >= std::fabs(dx);
    const Item& ht = s.items[sel];
    const uint64_t up = ht.up ? ht.up : c.up, down = ht.down ? ht.down : c.down;
    const uint64_t left = ht.left ? ht.left : c.left, right = ht.right ? ht.right : c.right;
    start(vertical ? (dy > 0 ? down : up) : (dx > 0 ? right : left), false);
}

} // namespace nnshim::ptr
