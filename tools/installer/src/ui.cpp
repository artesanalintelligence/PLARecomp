// Setup theme and widgets (see ui.h). Adapted from the in-game menu (overlay_ui.cpp, same project).
// SPDX-License-Identifier: MIT
#include "ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include "imgui_internal.h"

namespace setup::ui {

ImFont* g_fonts[3] = {};
float g_s = 1.0f;

const ImVec4 kAccent{0.725f, 0.490f, 0.949f, 1.0f};   // violet
const ImVec4 kAccent2{0.855f, 0.729f, 0.969f, 1.0f};  // light violet
const ImVec4 kWarn{0.914f, 0.804f, 0.208f, 1.0f};     // yellow
const ImVec4 kDanger{0.973f, 0.376f, 0.224f, 1.0f};   // orange
const ImVec4 kWhite{0.949f, 0.949f, 0.949f, 1.0f};
const ImVec4 kModal{0.031f, 0.027f, 0.051f, 0.90f};
const ImVec4 kInk{0.05f, 0.04f, 0.08f, 1.0f};

ImU32 C(const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

ImVec2 Measure(int f, float size, const char* t, float wrap) {
    return g_fonts[f]->CalcTextSizeA(S(size), FLT_MAX, wrap, t);
}

void Text(ImDrawList* dl, int f, float size, ImVec2 p, ImU32 col, const char* t, float wrap) {
    dl->AddText(g_fonts[f], S(size), p, col, t, nullptr, wrap);
}

float Ease(float t) { return 1.0f - std::pow(1.0f - std::clamp(t, 0.0f, 1.0f), 3.0f); }

void ApplyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 16;
    st.ChildRounding = 12;
    st.FrameRounding = 8;
    st.PopupRounding = 12;
    st.ScrollbarRounding = 6;
    st.GrabRounding = 6;
    st.WindowBorderSize = 0;
    st.WindowPadding = {0, 0};
    st.FramePadding = {12, 9};
    st.DisabledAlpha = 1.0f;
    st.Colors[ImGuiCol_NavCursor] = kAccent;
    st.Colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    st.Colors[ImGuiCol_ScrollbarGrab] = {1, 1, 1, 0.18f};
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = {1, 1, 1, 0.3f};
    st.Colors[ImGuiCol_ScrollbarGrabActive] = kAccent;
    st.Colors[ImGuiCol_Text] = kWhite;
    st.Colors[ImGuiCol_FrameBg] = {1, 1, 1, 0.05f};
    st.Colors[ImGuiCol_FrameBgHovered] = {1, 1, 1, 0.09f};
    st.Colors[ImGuiCol_FrameBgActive] = {1, 1, 1, 0.12f};
    st.Colors[ImGuiCol_TextSelectedBg] = {kAccent.x, kAccent.y, kAccent.z, 0.45f};
    st.Colors[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    st.Colors[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    st.ScaleAllSizes(g_s);
    st.FontSizeBase = 18.0f * g_s;
}

void Arrow(ImDrawList* dl, ImVec2 c, float r, int dir, ImU32 col) {
    ImVec2 a, b, d;
    switch (dir) {
    case 0: a = {c.x, c.y - r}; b = {c.x - r, c.y + r * 0.6f}; d = {c.x + r, c.y + r * 0.6f}; break;
    case 1: a = {c.x, c.y + r}; b = {c.x + r, c.y - r * 0.6f}; d = {c.x - r, c.y - r * 0.6f}; break;
    case 2: a = {c.x - r, c.y}; b = {c.x + r * 0.6f, c.y + r}; d = {c.x + r * 0.6f, c.y - r}; break;
    default: a = {c.x + r, c.y}; b = {c.x - r * 0.6f, c.y - r}; d = {c.x - r * 0.6f, c.y + r}; break;
    }
    dl->AddTriangleFilled(a, b, d, col);
}

float Keycap(ImDrawList* dl, ImVec2 p, float h, const char* label, ImU32 border, float font_px) {
    const std::string n = label;
    const bool arrow = n == "UP" || n == "DOWN" || n == "LEFT" || n == "RIGHT";
    float w = h;
    if (!arrow) w = std::max(h, Measure(kBold, font_px, label).x + S(12));
    dl->AddRectFilled(p, {p.x + w, p.y + h}, C(kWhite, 0.10f), S(5));
    dl->AddRect(p, {p.x + w, p.y + h}, border, S(5), 0, std::max(1.0f, S(1.4f)));
    dl->AddLine({p.x + S(3), p.y + h - S(2)}, {p.x + w - S(3), p.y + h - S(2)}, C(kWhite, 0.25f), std::max(1.0f, S(1.5f)));
    const ImVec2 c{p.x + w * 0.5f, p.y + h * 0.47f};
    if (arrow) {
        Arrow(dl, c, h * 0.2f, n == "UP" ? 0 : n == "DOWN" ? 1 : n == "LEFT" ? 2 : 3, C(kWhite));
    } else {
        const ImVec2 ts = Measure(kBold, font_px, label);
        Text(dl, kBold, font_px, {c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, C(kWhite), label);
    }
    return w;
}

void Check(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float thickness) {
    const ImVec2 pts[3] = {{c.x - r * 0.55f, c.y + r * 0.02f}, {c.x - r * 0.12f, c.y + r * 0.45f}, {c.x + r * 0.6f, c.y - r * 0.42f}};
    dl->AddPolyline(pts, 3, col, 0, thickness);
}

void StatusIcon(ImDrawList* dl, ImVec2 c, float r, int state, float alpha) {
    const float th = std::max(1.5f, r * 0.2f);
    switch (state) {
    case 1:
        dl->AddCircleFilled(c, r, C(kAccent, 0.9f * alpha), 32);
        Check(dl, c, r * 0.8f, C(kInk, alpha), th * 1.1f);
        break;
    case 0:
        dl->AddCircleFilled(c, r, C(kDanger, 0.9f * alpha), 32);
        dl->AddLine({c.x - r * 0.38f, c.y - r * 0.38f}, {c.x + r * 0.38f, c.y + r * 0.38f}, C(kInk, alpha), th);
        dl->AddLine({c.x - r * 0.38f, c.y + r * 0.38f}, {c.x + r * 0.38f, c.y - r * 0.38f}, C(kInk, alpha), th);
        break;
    case -1:
        dl->AddCircleFilled(c, r, C(kWarn, 0.9f * alpha), 32);
        dl->AddLine({c.x, c.y - r * 0.5f}, {c.x, c.y + r * 0.12f}, C(kInk, alpha), th);
        dl->AddCircleFilled({c.x, c.y + r * 0.45f}, th * 0.6f, C(kInk, alpha), 8);
        break;
    case 2: {
        dl->AddCircle(c, r, C(kWhite, 0.12f * alpha), 32, th);
        const float t = (float)ImGui::GetTime() * 5.0f;
        dl->PathArcTo(c, r, t, t + IM_PI * 0.9f, 24);
        dl->PathStroke(C(kAccent, alpha), 0, th);
        break;
    }
    case 4:
        dl->AddCircle(c, r, C(kWhite, 0.15f * alpha), 32, std::max(1.0f, th * 0.6f));
        dl->AddLine({c.x - r * 0.4f, c.y}, {c.x + r * 0.4f, c.y}, C(kWhite, 0.35f * alpha), th * 0.8f);
        break;
    default:
        dl->AddCircle(c, r, C(kWhite, 0.25f * alpha), 32, std::max(1.0f, th * 0.6f));
        dl->AddCircleFilled(c, r * 0.22f, C(kWhite, 0.35f * alpha), 12);
        break;
    }
}

void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, const ImVec4& col, bool animated) {
    const float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, C(kWhite, 0.07f), r);
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac <= 0.0f) return;
    const float x = a.x + std::max(b.y - a.y, (b.x - a.x) * frac);
    dl->AddRectFilled(a, {x, b.y}, C(col, 0.92f), r);
    dl->AddRectFilled({a.x + r * 0.5f, a.y + 1}, {x - r * 0.5f, a.y + (b.y - a.y) * 0.45f}, C(kWhite, 0.10f), r * 0.6f);
    if (animated) {
        // Shine sweeping over the filled part.
        const float w = (x - a.x);
        const float t = std::fmod((float)ImGui::GetTime() * 0.6f, 1.4f) - 0.2f;
        const float cx = a.x + w * t, hw = S(60);
        const float x0 = std::max(a.x + r, cx - hw), x1 = std::min(x - r, cx + hw);
        if (x1 > x0) {
            const float xm = std::clamp(cx, x0, x1);
            dl->AddRectFilledMultiColor({x0, a.y + 1}, {xm, b.y - 1}, C(kWhite, 0.0f), C(kWhite, 0.22f), C(kWhite, 0.22f), C(kWhite, 0.0f));
            dl->AddRectFilledMultiColor({xm, a.y + 1}, {x1, b.y - 1}, C(kWhite, 0.22f), C(kWhite, 0.0f), C(kWhite, 0.0f), C(kWhite, 0.22f));
        }
    }
}

void Background(ImDrawList* dl, float w, float h) {
    dl->AddRectFilled({0, 0}, {w, h}, C({0.043f, 0.035f, 0.07f, 1.0f}));
    dl->AddRectFilledMultiColor({0, 0}, {w, h}, C({0.13f, 0.07f, 0.22f, 1.0f}, 0.55f), C({0.02f, 0.016f, 0.04f, 1.0f}, 0.0f),
                                C({0.02f, 0.016f, 0.04f, 1.0f}, 0.6f), C({0.08f, 0.04f, 0.14f, 1.0f}, 0.4f));
    // Abstract rings (like the in-game start screen) in the bottom-right corner.
    const ImVec2 c{w * 0.86f, h * 0.80f};
    const float t = (float)ImGui::GetTime();
    for (int i = 0; i < 3; ++i) {
        const float r = h * (0.30f + 0.09f * i);
        const float a0 = t * (0.06f + 0.03f * i) * (i % 2 ? -1.0f : 1.0f);
        for (int k = 0; k < 6; ++k) {
            const float s0 = a0 + k * (IM_PI / 3.0f);
            dl->PathArcTo(c, r, s0, s0 + IM_PI / 3.0f * 0.62f, 32);
            dl->PathStroke(C(kAccent, 0.10f - 0.025f * i), 0, std::max(1.0f, S(2.0f - 0.5f * i)));
        }
    }
}

bool Pill(const char* label, bool selected, float minw, bool enabled) {
    const char* fin = ImGui::FindRenderedTextEnd(label);
    const std::string vis_(label, fin);
    const ImVec2 ts = Measure(kReg, 17, vis_.c_str());
    const ImVec2 sz{std::max(minw, ts.x + S(28)), S(34)};
    if (!enabled) ImGui::BeginDisabled();
    const bool pr = ImGui::InvisibleButton(label, sz, ImGuiButtonFlags_EnableNav);
    if (!enabled) ImGui::EndDisabled();
    const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused(), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dis = enabled ? 1.0f : 0.4f;
    if (selected) {
        dl->AddRectFilled(a, b, C(kAccent, (act ? 0.40f : 0.26f) * dis), S(17));
        dl->AddRect(a, b, C(kAccent, 0.95f * dis), S(17), 0, std::max(1.0f, S(1.5f)));
    } else {
        dl->AddRectFilled(a, b, C(kWhite, (act ? 0.16f : hov ? 0.10f : 0.035f) * dis), S(17));
        dl->AddRect(a, b, C(kWhite, (hov ? 0.35f : 0.16f) * dis), S(17), 0, std::max(1.0f, S(1.0f)));
    }
    Text(dl, selected ? kBold : kReg, 17, {a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f},
          C(kWhite, (selected || hov ? 1.0f : 0.7f) * dis), vis_.c_str());
    return pr && enabled;
}

bool TextButton(const char* label, const ImVec4& col, bool filled, float minw, bool enabled) {
    const char* fin = ImGui::FindRenderedTextEnd(label);
    const std::string vis_(label, fin);
    const ImVec2 ts = Measure(kBold, 18, vis_.c_str());
    const ImVec2 sz{std::max(minw, ts.x + S(40)), S(42)};
    if (!enabled) ImGui::BeginDisabled();
    const bool pr = ImGui::InvisibleButton(label, sz, ImGuiButtonFlags_EnableNav);
    if (!enabled) ImGui::EndDisabled();
    const bool hov = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused()), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dis = enabled ? 1.0f : 0.35f;
    if (filled) dl->AddRectFilled(a, b, C(col, (act ? 1.0f : hov ? 0.95f : 0.8f) * dis), S(10));
    else dl->AddRectFilled(a, b, C(kWhite, (act ? 0.16f : hov ? 0.11f : 0.05f) * dis), S(10));
    dl->AddRect(a, b, C(filled ? col : (col.x == kWhite.x ? kWhite : col), (filled ? 1.0f : 0.3f) * dis), S(10), 0,
                std::max(1.0f, S(1.2f)));
    const ImU32 tc = filled ? C(kInk, enabled ? 1.0f : 0.6f) : C(col, (hov ? 1.0f : 0.85f) * dis);
    Text(dl, kBold, 18, {a.x + (sz.x - ts.x) * 0.5f, a.y + (sz.y - ts.y) * 0.5f}, tc, vis_.c_str());
    return pr && enabled;
}

bool IconButton(const char* id, int kind, float size) {
    const bool pr = ImGui::InvisibleButton(id, {size, size}, ImGuiButtonFlags_EnableNav);
    const bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused(), act = ImGui::IsItemActive();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, C(kWhite, act ? 0.18f : hov ? 0.12f : 0.05f), size * 0.5f);
    const ImU32 col = C(kWhite, hov ? 1.0f : 0.8f);
    const float r = size * 0.22f, th = std::max(1.5f, S(2.2f));
    switch (kind) {
    case 0: dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, th); break;
    case 1:
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, th);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, th);
        break;
    case 2: Arrow(dl, c, r * 0.9f, 2, col); break;
    case 3: Arrow(dl, c, r * 0.9f, 3, col); break;
    default:
        dl->AddLine({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, th);
        dl->AddLine({c.x - r, c.y + r}, {c.x + r, c.y - r}, col, th);
        break;
    }
    return pr;
}

bool Field(const char* id, char* buf, size_t n, float w, const char* hint) {
    ImGui::PushFont(g_fonts[kReg], S(16));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {S(12), S(8)});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, S(1.0f)));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.16f));
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint(id, hint, buf, n);
    const bool committed = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemFocused() || ImGui::IsItemActive()) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(a, b, C(kAccent, 0.9f), S(8), 0, std::max(1.0f, S(1.5f)));
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    return committed;
}

void HelpBar(ImDrawList* dl, float cx, float y) {
    struct Item {
        std::vector<const char*> keys;
        const char* text;
    };
    const Item items[] = {{{"Tab"}, "Navigate"}, {{"Enter"}, "Select"}, {{"Esc"}, "Back"}};
    const float h = S(24), gap = S(32);
    float total = 0;
    for (auto& it : items) {
        for (auto* g : it.keys) total += std::max(h, Measure(kBold, 13, g).x + S(12)) + S(4);
        total += S(6) + Measure(kReg, 16, it.text).x + gap;
    }
    total -= gap;
    float x = cx - total * 0.5f;
    for (auto& it : items) {
        for (auto* g : it.keys) x += Keycap(dl, {x, y}, h, g, C(kWhite, 0.5f), 13) + S(4);
        x += S(6);
        const ImVec2 ts = Measure(kReg, 16, it.text);
        Text(dl, kReg, 16, {x, y + (h - ts.y) * 0.5f}, C(kWhite, 0.6f), it.text);
        x += ts.x + gap;
    }
}

}  // namespace setup::ui
