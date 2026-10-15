// nnshim::ptr: mouse pointer inside the game's own UI (docs/port/POINTER.md).
//
// The game has no pointer and no touch screen. Its UI is drawn with nn::ui2d (Layout -> tree of Pane).
// On every Layout::Draw (main+0x1353e0) the shim reads the visible panes with their on-screen rectangle
// (pane global matrix + DrawInfo projection) and uses that to decide which element is under the cursor
// and which one is highlighted. This header holds only the pure logic (no guest memory, no Win32) so it
// can be tested in nnshim_selftest.
//
// nn::ui2d structures read from the binary (game version 1.1.1, see POINTER.md section 1):
//   Layout:   +0x18 Pane* root, +0x28 layout size (float w, h).
//   DrawInfo: +0x10 projection matrix 4x4 (rows), +0x50 view matrix 3x4, +0xc0 Layout* being drawn.
//   Pane:     +0x00 vtable, +0x08 node in the parent's child list (prev, next), +0x18 parent,
//             +0x20 child list (head node: +0x20 prev, +0x28 next; child pane = node - 8),
//             +0x30 translate (x, y, z), +0x3c rotate, +0x48 scale, +0x50 size (w, h),
//             +0x58 flags (bit 0 visible, bit 7 = "visible and calculated this frame"), +0x59 alpha,
//             +0x5a global alpha, +0x5b base position (bits 0-1 x: 0 center, 1 left, 2 right;
//             bits 2-3 y: 0 center, 1 top, 2 bottom), +0x70 global matrix 3x4 (rows, includes the
//             view matrix), +0xb0 name (24 chars).
//   TextBox:  +0xd8 char16_t* text.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nnshim::ptr {

struct Mat34 {
    float m[3][4];
};

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // x0 <= x1, y0 <= y1
    bool Contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
    float Cx() const { return (x0 + x1) * 0.5f; }
    float Cy() const { return (y0 + y1) * 0.5f; }
    float Area() const { return W() * H(); }
};

// Bounding box of a pane in the layout's global space (y up, origin at the screen center), from its
// global matrix, size and base position.
Rect PaneGlobalRect(const Mat34& g, float w, float h, uint8_t base_pos);

// Global space -> normalized screen coordinates (0..1, y down) using the DrawInfo orthographic
// projection (4x4, rows). If the projection is not valid, a centered layout of
// fallback_w x fallback_h is assumed.
struct Proj {
    float m[4][4];
    bool valid = false;
};
Rect GlobalToScreen(const Rect& r, const Proj& p, float fallback_w = 1920.0f, float fallback_h = 1080.0f);

// Intersection (empty rect -> W() <= 0 or H() <= 0).
Rect Intersect(const Rect& a, const Rect& b);

// 64-bit FNV-1a (the game's HashedName).
uint64_t Fnv1a64(const char* s, size_t n);

// ---- what the game shows this frame --------------------------------------------------------------------
// A selectable element on screen (normalized screen coordinates, 0..1, y down). Elements with the same
// `group` are navigated with the d-pad as one list/grid; at most one per group is `selected` (highlighted).
struct Item {
    Rect r;
    uint64_t group = 0;
    uint64_t id = 0;        // stable while the screen is open (e.g. the game object or pane address)
    bool selected = false;
    int layer = 0;          // draw order of its layout in the frame: a higher layer is on top (context menus)
    // Per-group navigation (0 = the NavConfig default): e.g. header tabs that switch with ZL/ZR.
    uint64_t up = 0, down = 0, left = 0, right = 0;
    bool no_accept = false; // clicking only selects it (tabs)
    uint64_t accept = 0;    // button of a click on it (0 = NavConfig::accept), e.g. L/R on the pocket arrows
};
struct Snapshot {
    uint64_t seq = 0;       // frame counter of the game (FrameTick)
    std::vector<Item> items;
    // A free cursor moved with the left stick (the map): a click elsewhere drives the stick until the cursor gets
    // there; a click on the cursor accepts.
    bool has_stick_cursor = false;
    Rect stick_cursor;
};

// Item under the point (x, y): among the items that contain it, those of the topmost layer, and among them the one
// whose center is nearest (relative to its size); overlapping hit boxes of a grid then split like a Voronoi
// diagram. -1 = none.
int HitTest(const Snapshot& s, float x, float y);
// Selected item of a group, -1 = none.
int SelectedIn(const Snapshot& s, uint64_t group);

// ---- navigation: turns "the pointer is over item T" into d-pad / A presses -------------------------------
struct NavConfig {
    uint64_t up = 0, down = 0, left = 0, right = 0, accept = 0;   // button masks
    int dwell_ms = 60;        // the pointer must rest this long on an item before the highlight follows it
    int press_ms = 80;        // length of each injected d-pad press (context menus miss 50 ms presses)
    int accept_ms = 250;      // length of the accept press (at high frame rates the inventory misses A presses
                              // of ~100 ms: measured, see POINTER.md section 6)
    int settle_ms = 320;      // wait for the highlight to move after a press
    int gap_ms = 180;         // and, once it moved, let the game finish its cursor animation before the next
                              // press (faster presses leave the inventory ignoring A until the next move)
    int min_frames = 2;       // and at least this many game frames
    int max_fail = 4;         // presses without effect before giving up on a target
    float aspect = 16.0f / 9.0f;
    bool hover_select = true; // highlight follows the pointer (false: only on click)
    bool outside_click_accept = false;   // click outside every detected item = accept (default: ignored)
    float stick_gain = 8.0f;  // stick cursor: stick magnitude per unit of distance (normalized screen)
    float stick_min = 0.35f;  // and its floor (dead zone of the game)
    float stick_done = 0.012f;   // close enough
    int stick_timeout_ms = 3000;
};

class Navigator {
public:
    struct In {
        bool has_pos = false;     // pointer inside the game window
        float x = 0, y = 0;       // normalized screen coordinates
        bool click = false;       // left button released this sample (not a drag)
        float dt_ms = 0;
    };
    struct Out {
        uint64_t buttons = 0;     // to hold this sample
        bool click_consumed = false;   // the click was handled here (no legacy A on release)
        int hover = -1;           // index of the item under the pointer
        bool stick = false;       // drive the left stick this sample
        int32_t lx = 0, ly = 0;
    };
    void Step(const Snapshot& s, const In& in, const NavConfig& c, Out& out);
    void Reset();
    uint64_t Presses() const { return presses_; }
    uint64_t Accepts() const { return accepts_; }

private:
    enum class St { Idle, Pressing, Settling };
    St st_ = St::Idle;
    uint64_t hover_id_ = 0, hover_group_ = 0;
    float dwell_ = 0;
    bool hover_by_motion_ = false;   // the hover changed because the pointer moved (not the content)
    float last_x_ = -1, last_y_ = -1;
    float target_cx_ = 0, target_cy_ = 0;   // target position when chosen (a scrolled list moves it)
    uint64_t target_id_ = 0, target_group_ = 0;   // navigation target (0 = none)
    bool pending_accept_ = false;
    uint64_t press_btn_ = 0;
    bool press_accept_ = false;
    float phase_ms_ = 0;
    uint64_t press_seq_ = 0, from_sel_ = 0;
    int fail_ = 0;
    bool swap_axis_ = false;
    uint64_t presses_ = 0, accepts_ = 0;
    bool stick_active_ = false;
    float stick_tx_ = 0, stick_ty_ = 0, stick_ms_ = 0;
};

// ---- host side (implemented in abi_pointer.cpp; called by hid_core.cpp's polling thread in menu mode) ----
struct HostSample {
    bool has_pos = false;     // the cursor is inside the client area of the focused game window
    float x = 0, y = 0;       // normalized client coordinates (nvnvk stretches the game image to the window)
    bool left = false;        // left button held
    bool right = false;       // right button held
    float dt_ms = 0;
};
struct HostResult {
    uint64_t buttons = 0;     // d-pad / A presses to add this sample
    bool owns_left = false;   // the current left press belongs to the pointer: do not feed it to the legacy
                              // menu mouse (A on release, drag = left stick)
    bool stick = false;       // left stick from the pointer (stick cursor)
    int32_t lx = 0, ly = 0;
    bool right = false;       // right button for the menu mouse (B), stretched to at least pointer_back_ms
};
bool Enabled();                                   // "ui.mouse_pointer" (default true)
int AcceptMs();                                   // length of the A of a click
void HostPoll(const HostSample& in, HostResult& out);
void HostReset();                                 // leaving menu mode
// Virtual pointer for tests (UDP "ptr <x> <y>", normalized; "ptr off"). true if a position is set.
bool VirtualPos(float& x, float& y);
bool RemoteCommand(const std::string& cmd, const std::string& args);
// ui.pointer_trace: log the buttons and stick that the mouse gives in pointer mode when they change.
void TraceButtons(uint64_t buttons, int32_t lx, int32_t ly);

} // namespace nnshim::ptr
