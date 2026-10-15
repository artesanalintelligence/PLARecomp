// Setup theme and widgets. Same look as the in-game menu (tools/port/nvnvk/src/overlay_ui.cpp): near-black panels,
// violet accent, Lato, pills, underlined tabs and a bottom help bar with drawn keycaps.
// SPDX-License-Identifier: MIT
#pragma once
#include <string>

#include "imgui.h"

namespace setup::ui {

enum { kReg = 0, kBold = 1, kBlack = 2 };
extern ImFont* g_fonts[3];
extern float g_s;  // UI scale (1 = designed at 1280x800)

extern const ImVec4 kAccent, kAccent2, kWarn, kDanger, kWhite, kModal, kInk;

inline float S(float v) { return v * g_s; }
ImU32 C(const ImVec4& c, float a = 1.0f);
ImVec2 Measure(int f, float size, const char* t, float wrap = 0.0f);
void Text(ImDrawList* dl, int f, float size, ImVec2 p, ImU32 col, const char* t, float wrap = 0.0f);
float Ease(float t);
void ApplyStyle();

void Arrow(ImDrawList* dl, ImVec2 c, float r, int dir, ImU32 col);  // 0 up, 1 down, 2 left, 3 right
float Keycap(ImDrawList* dl, ImVec2 p, float h, const char* label, ImU32 border, float font_px = 14.0f);
// State icon: 1 ok (check), 0 missing (cross), -1 warning (!), 2 running (spinner), 3 pending (dot), 4 skipped (dash)
void StatusIcon(ImDrawList* dl, ImVec2 c, float r, int state, float alpha = 1.0f);
void Check(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float thickness);
void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, const ImVec4& col, bool animated);
void Background(ImDrawList* dl, float w, float h);

bool Pill(const char* label, bool selected, float minw = 0.0f, bool enabled = true);
bool TextButton(const char* label, const ImVec4& col, bool filled, float minw = 0.0f, bool enabled = true);
bool IconButton(const char* id, int kind, float size);  // 0 minus, 1 plus, 2 left, 3 right, 4 cross
bool Field(const char* id, char* buf, size_t n, float w, const char* hint);  // true when an edit is committed
void HelpBar(ImDrawList* dl, float cx, float y);

}  // namespace setup::ui
