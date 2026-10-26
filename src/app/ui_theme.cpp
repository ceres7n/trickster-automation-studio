#include "app/ui_theme.h"
#include "app/text_util.h"
#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

namespace nj::ui {

namespace {
constexpr ImVec4 rgb(int hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}
} // namespace

const ImVec4 kBg         = rgb(0x1F1F1F);
const ImVec4 kSurface    = rgb(0x181818);
const ImVec4 kSurface2   = rgb(0x313131);
const ImVec4 kBorder     = rgb(0x2B2B2B);
const ImVec4 kHover      = rgb(0x2A2D2E);
const ImVec4 kSelect     = rgb(0x04395E);
const ImVec4 kText       = rgb(0xCCCCCC);
const ImVec4 kMuted      = rgb(0x9D9D9D);
const ImVec4 kAccent     = rgb(0x0078D4);
const ImVec4 kOk         = rgb(0x89D185);
const ImVec4 kWarn       = rgb(0xCCA700);
const ImVec4 kErr        = rgb(0xF14C4C);
const ImVec4 kStatusIdle = rgb(0x68217A);
const ImVec4 kStatusLive = rgb(0x007ACC);
const ImVec4 kStatusRun  = rgb(0xCC6633);
const ImVec4 kLink       = rgb(0x3794FF);

Fonts fonts;

ImU32 col(const ImVec4& c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

void load_fonts()
{
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    const std::string dir = std::string(win) + "\\Fonts\\";
    auto add = [&](const char* file, float size, const char* hangul) -> ImFont* {
        if (!file_exists(dir + file)) return nullptr;
        ImFont* f = io.Fonts->AddFontFromFileTTF((dir + file).c_str(), size);
        if (f && hangul && file_exists(dir + hangul)) {   // the client's CP949 text (names, chat)
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF((dir + hangul).c_str(), size, &merge, io.Fonts->GetGlyphRangesKorean());
        }
        return f;
    };
    fonts.ui = add("segoeui.ttf", 15.0f, "malgun.ttf");
    if (!fonts.ui) fonts.ui = io.Fonts->AddFontDefault();
    fonts.bold = add("seguisb.ttf", 15.0f, "malgunbd.ttf");
    fonts.big = add("seguisb.ttf", 21.0f, nullptr);
    fonts.small = add("segoeui.ttf", 12.5f, "malgun.ttf");
    fonts.mono = add("consola.ttf", 14.0f, nullptr);
    fonts.title = add("segoeuil.ttf", 34.0f, nullptr);
    if (!fonts.title) fonts.title = add("segoeui.ttf", 34.0f, nullptr);
    if (!fonts.bold) fonts.bold = fonts.ui;
    if (!fonts.big) fonts.big = fonts.ui;
    if (!fonts.small) fonts.small = fonts.ui;
    if (!fonts.title) fonts.title = fonts.big;
}

void apply_theme()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(8, 4);
    s.IndentSpacing = 16.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 8.0f;
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 2.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 2.0f;
    s.TabRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBorderSize = 0.0f;
    s.TabBarBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(0, 3);

    ImVec4* c = s.Colors;
    const ImVec4 none(0, 0, 0, 0);
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = kMuted;
    c[ImGuiCol_WindowBg]             = kBg;
    c[ImGuiCol_ChildBg]              = none;
    c[ImGuiCol_PopupBg]              = rgb(0x1F1F1F);
    c[ImGuiCol_Border]               = rgb(0x3C3C3C);
    c[ImGuiCol_BorderShadow]         = none;
    c[ImGuiCol_FrameBg]              = kSurface2;
    c[ImGuiCol_FrameBgHovered]       = rgb(0x3A3A3A);
    c[ImGuiCol_FrameBgActive]        = rgb(0x3A3A3A);
    c[ImGuiCol_TitleBg]              = kSurface;
    c[ImGuiCol_TitleBgActive]        = kSurface;
    c[ImGuiCol_MenuBarBg]            = kSurface;
    c[ImGuiCol_ScrollbarBg]          = none;
    c[ImGuiCol_ScrollbarGrab]        = rgb(0x797979, 0.40f);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x646464, 0.70f);
    c[ImGuiCol_ScrollbarGrabActive]  = rgb(0xBFBFBF, 0.40f);
    c[ImGuiCol_CheckMark]            = rgb(0xFFFFFF);
    c[ImGuiCol_SliderGrab]           = kAccent;
    c[ImGuiCol_SliderGrabActive]     = rgb(0x026EC1);
    c[ImGuiCol_Button]               = kSurface2;
    c[ImGuiCol_ButtonHovered]        = rgb(0x3C3C3C);
    c[ImGuiCol_ButtonActive]         = rgb(0x454545);
    c[ImGuiCol_Header]               = kSelect;
    c[ImGuiCol_HeaderHovered]        = kHover;
    c[ImGuiCol_HeaderActive]         = kSelect;
    c[ImGuiCol_Separator]            = kBorder;
    c[ImGuiCol_SeparatorHovered]     = kAccent;
    c[ImGuiCol_SeparatorActive]      = kAccent;
    c[ImGuiCol_ResizeGrip]           = none;
    c[ImGuiCol_ResizeGripHovered]    = none;
    c[ImGuiCol_ResizeGripActive]     = none;
    c[ImGuiCol_Tab]                  = kSurface;
    c[ImGuiCol_TabHovered]           = kBg;
    c[ImGuiCol_TabSelected]          = kBg;
    c[ImGuiCol_TabSelectedOverline]  = kAccent;
    c[ImGuiCol_TabDimmed]            = kSurface;
    c[ImGuiCol_TabDimmedSelected]    = kBg;
    c[ImGuiCol_PlotHistogram]        = kAccent;
    c[ImGuiCol_TableHeaderBg]        = kSurface;
    c[ImGuiCol_TableBorderStrong]    = kBorder;
    c[ImGuiCol_TableBorderLight]     = kBorder;
    c[ImGuiCol_TableRowBg]           = none;
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextSelectedBg]       = rgb(0x264F78);
    c[ImGuiCol_NavHighlight]         = kAccent;
}

bool toggle(const char* id, bool* v)
{
    const float h = ImGui::GetFrameHeight() * 0.62f, w = h * 1.8f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float dy = (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    const bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + dy), b(p.x + w, p.y + dy + h);
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, *v ? col(kAccent, hov ? 1.0f : 0.9f) : col(kSurface2, hov ? 1.0f : 0.9f), h * 0.5f);
    if (!*v) dl->AddRect(a, b, col(rgb(0x3C3C3C)), h * 0.5f);
    const float r = h * 0.5f - 2.5f;
    const float cx = *v ? b.x - h * 0.5f : a.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, a.y + h * 0.5f), r, col(*v ? ImVec4(1, 1, 1, 1) : kMuted), 20);
    return clicked;
}

void pill(const char* text, const ImVec4& color)
{
    ImGui::PushFont(fonts.small);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 pad(7, 2);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (ImGui::GetFrameHeight() - ts.y - pad.y * 2) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + ts.x + pad.x * 2, y + ts.y + pad.y * 2), col(color, 0.18f), 3.0f);
    dl->AddText(ImVec2(p.x + pad.x, y + pad.y), col(color), text);
    ImGui::Dummy(ImVec2(ts.x + pad.x * 2, ImGui::GetFrameHeight()));
    ImGui::PopFont();
}

bool button_primary(const char* label_, const ImVec2& size, const ImVec4& color)
{
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.0f, color.x * 1.12f + 0.02f),
                                                         std::min(1.0f, color.y * 1.12f + 0.02f),
                                                         std::min(1.0f, color.z * 1.12f + 0.02f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x * 0.85f, color.y * 0.85f, color.z * 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool r = ImGui::Button(label_, size);
    ImGui::PopStyleColor(5);
    return r;
}

void label(const char* text)
{
    ImGui::PushFont(fonts.small);
    ImGui::TextColored(kMuted, "%s", text);
    ImGui::PopFont();
}

void stat(const char* caption, const char* value, float width)
{
    ImGui::BeginGroup();
    ImGui::PushFont(fonts.small);
    ImGui::TextColored(kMuted, "%s", caption);
    ImGui::PopFont();
    ImGui::PushFont(fonts.big);
    ImGui::TextUnformatted(value);
    ImGui::PopFont();
    ImGui::EndGroup();
    const float used = ImGui::GetItemRectSize().x;
    if (used < width) { ImGui::SameLine(0, 0); ImGui::Dummy(ImVec2(width - used, 1)); }
}

void meter(float frac, const ImVec4& color, float width, float height)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col(kSurface2), 1.0f);
    if (frac > 0) dl->AddRectFilled(p, ImVec2(p.x + width * frac, p.y + height), col(color), 1.0f);
    ImGui::Dummy(ImVec2(width, height));
}

void bar(float frac, const ImVec4& color, float width, const char* text, float height)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col(kSurface2), 2.0f);
    if (frac > 0) dl->AddRectFilled(p, ImVec2(p.x + width * frac, p.y + height), col(color, 0.85f), 2.0f);
    if (text && *text) {
        ImGui::PushFont(fonts.small);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (height - ts.y) * 0.5f), col(ImVec4(1, 1, 1, 0.95f)), text);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(width, height));
}

bool segmented(const char* id, int* value, const char* const* items, int n)
{
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine();
        const bool on = *value == i;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kSurface2);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? rgb(0x026EC1) : rgb(0x3C3C3C));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : kText);
        if (ImGui::Button(items[i])) { *value = i; changed = true; }
        ImGui::PopStyleColor(3);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

namespace {
std::vector<bool> g_cardDim;                    // card_begin pushed a dim alpha
}

void card_begin(const char* id, const char* title, const char* status, bool* enabled)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSurface);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild(id, ImVec2(-1, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                             ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if ((!title || !*title) && !status && !enabled) { g_cardDim.push_back(false); return; }   // no header
    page_header(title, status, enabled);
    const bool dim = enabled && !*enabled;
    if (dim) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45f);
    g_cardDim.push_back(dim);
}

void card_end()
{
    if (!g_cardDim.empty()) {
        if (g_cardDim.back()) ImGui::PopStyleVar();
        g_cardDim.pop_back();
    }
    ImGui::EndChild();
}

void page_header(const char* title, const char* status, bool* enabled)
{
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(fonts.bold);
    ImGui::TextUnformatted(title ? title : "");
    ImGui::PopFont();
    const float right = ImGui::GetContentRegionMax().x;
    const float sw = ImGui::GetFrameHeight() * 0.62f * 1.8f;
    if (status && *status) {
        ImGui::SameLine(0, 12);
        const float avail = right - ImGui::GetCursorPosX() - (enabled ? sw + 12 : 0);
        ImGui::AlignTextToFramePadding();
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(sp, ImVec2(sp.x + std::max(0.0f, avail), sp.y + 40), true);
        ImGui::TextColored(kMuted, "%s", status);
        ImGui::PopClipRect();
    }
    if (enabled) {
        ImGui::SameLine(right - sw);
        toggle("##on", enabled);
    }
}

void help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ── icons ───────────────────────────────────────────────────────────────────
void icon(const gfx::Icon& ic, float size, float alpha)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ic.ok()) {
        const float sc = size / std::max(ic.w, ic.h);
        const float w = ic.w * sc, h = ic.h * sc;
        const ImVec2 a(std::floor(p.x + (size - w) * 0.5f), std::floor(p.y + (size - h) * 0.5f));
        dl->AddImage(ic.tex, a, ImVec2(a.x + w, a.y + h), ic.uv0, ic.uv1, col(ImVec4(1, 1, 1, alpha)));
    } else {
        dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + size - 2, p.y + size - 2), col(kSurface2, alpha), 3.0f);
    }
    ImGui::Dummy(ImVec2(size, size));
}

void icon_cool(const gfx::Icon& ic, float size, float frac, const char* text)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    icon(ic, size, frac > 0 ? 0.55f : 1.0f);
    if (frac <= 0) return;
    frac = std::min(frac, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // a clockwise sweep from 12 o'clock over what is left
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const float r = size * 0.72f;
    const float kPi = 3.14159265f;
    const float a0 = -kPi * 0.5f, a1 = a0 + frac * kPi * 2.0f;
    dl->PushClipRect(p, ImVec2(p.x + size, p.y + size), true);
    dl->PathLineTo(c);
    dl->PathArcTo(c, r, a0, a1, 32);
    dl->PathFillConvex(col(ImVec4(0, 0, 0, 0.55f)));
    dl->PopClipRect();
    if (text && *text) {
        ImGui::PushFont(fonts.small);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 tp(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f);
        dl->AddText(ImVec2(tp.x + 1, tp.y + 1), col(ImVec4(0, 0, 0, 1)), text);
        dl->AddText(tp, col(ImVec4(1, 1, 1, 1)), text);
        ImGui::PopFont();
    }
}

// ── VS Code chrome ──────────────────────────────────────────────────────────
bool tab_strip(const char* id, int* sel, const char* const* labels, int n, const bool* dots, float height)
{
    bool changed = false;
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float full = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(start, ImVec2(start.x + full, start.y + height), col(kSurface));
    const float dotW = dots ? 14.0f : 0.0f;
    float natural = 0.0f;                       // the padding shrinks (26 -> 8) when the tabs do not fit
    for (int i = 0; i < n; ++i) natural += ImGui::CalcTextSize(labels[i]).x + 26.0f + dotW;
    const float pad = natural <= full || n == 0 ? 26.0f : std::max(8.0f, 26.0f - (natural - full) / float(n));
    float x = start.x;
    for (int i = 0; i < n; ++i) {
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const float w = ts.x + pad + dotW;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, height))) { if (*sel != i) changed = true; *sel = i; }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = *sel == i;
        const ImVec2 a(x, start.y), b(x + w, start.y + height);
        if (on) dl->AddRectFilled(a, b, col(kBg));
        else if (hov) dl->AddRectFilled(a, b, col(rgb(0x1F1F1F)));
        if (on) dl->AddRectFilled(a, ImVec2(b.x, a.y + 1.5f), col(kAccent));
        dl->AddLine(ImVec2(b.x - 0.5f, a.y), ImVec2(b.x - 0.5f, b.y), col(kBorder));
        dl->AddText(ImVec2(x + pad * 0.5f, start.y + (height - ts.y) * 0.5f), col(on ? ImVec4(1, 1, 1, 1) : kMuted), labels[i]);
        if (dots) {
            const ImVec2 dc(x + pad * 0.5f + ts.x + 10.0f, start.y + height * 0.5f);
            if (dots[i]) dl->AddCircleFilled(dc, 3.5f, col(on ? ImVec4(1, 1, 1, 0.9f) : kMuted), 12);
            else dl->AddCircle(dc, 3.5f, col(kMuted, 0.6f), 12, 1.0f);
        }
        x += w;
    }
    // the strip's bottom border except under the active tab
    dl->AddLine(ImVec2(start.x, start.y + height - 0.5f), ImVec2(start.x + full, start.y + height - 0.5f), col(kBorder));
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return changed;
}

bool section(const char* title, bool* open)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 22.0f;
    ImGui::PushID(title);
    if (ImGui::InvisibleButton("##sec", ImVec2(w, h))) *open = !*open;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(kHover));
    // chevron
    const ImVec2 c(p.x + 10, p.y + h * 0.5f);
    const ImU32 cc = col(kText);
    if (*open) dl->AddTriangleFilled(ImVec2(c.x - 4, c.y - 2), ImVec2(c.x + 4, c.y - 2), ImVec2(c.x, c.y + 3), cc);
    else dl->AddTriangleFilled(ImVec2(c.x - 2, c.y - 4), ImVec2(c.x - 2, c.y + 4), ImVec2(c.x + 3, c.y), cc);
    ImGui::PushFont(fonts.small);
    std::string up = title;
    for (char& ch : up) ch = char(toupper((unsigned char)ch));
    dl->AddText(fonts.bold, fonts.small->FontSize, ImVec2(p.x + 20, p.y + (h - fonts.small->FontSize) * 0.5f), cc, up.c_str());
    ImGui::PopFont();
    return *open;
}

void glyph(ImDrawList* dl, Glyph g, ImVec2 c, float s, ImU32 color)
{
    const float t = std::max(1.5f, s * 0.075f);
    switch (g) {
    case Glyph::Bot: {      // a robot head: rounded square, antenna, two eyes
        const float r = s * 0.36f;
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.55f), ImVec2(c.x + r, c.y + r), color, s * 0.12f, 0, t);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.55f), ImVec2(c.x, c.y - r * 1.05f), color, t);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 1.15f), t * 1.1f, color, 12);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y + r * 0.18f), t * 1.15f, color, 12);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y + r * 0.18f), t * 1.15f, color, 12);
        dl->AddLine(ImVec2(c.x - r * 1.25f, c.y + r * 0.1f), ImVec2(c.x - r, c.y + r * 0.1f), color, t);
        dl->AddLine(ImVec2(c.x + r, c.y + r * 0.1f), ImVec2(c.x + r * 1.25f, c.y + r * 0.1f), color, t);
        break;
    }
    case Glyph::Debug: {    // a bug: body, head, legs, feelers
        const float r = s * 0.22f;
        dl->AddEllipse(ImVec2(c.x, c.y + r * 0.35f), ImVec2(r, r * 1.3f), color, 0.0f, 24, t);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 1.6f), color, t * 0.8f);
        dl->AddCircle(ImVec2(c.x, c.y - r * 1.2f), r * 0.55f, color, 16, t);
        for (int i = -1; i <= 1; ++i) {
            const float y = c.y + r * 0.35f + i * r * 0.75f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x - r * 1.75f, y + i * r * 0.35f), color, t);
            dl->AddLine(ImVec2(c.x + r, y), ImVec2(c.x + r * 1.75f, y + i * r * 0.35f), color, t);
        }
        dl->AddLine(ImVec2(c.x - r * 0.35f, c.y - r * 1.65f), ImVec2(c.x - r * 0.9f, c.y - r * 2.2f), color, t);
        dl->AddLine(ImVec2(c.x + r * 0.35f, c.y - r * 1.65f), ImVec2(c.x + r * 0.9f, c.y - r * 2.2f), color, t);
        break;
    }
    case Glyph::Play: {
        const float r = s * 0.32f;
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.75f, c.y - r), ImVec2(c.x - r * 0.75f, c.y + r), ImVec2(c.x + r, c.y), color);
        break;
    }
    case Glyph::Stop: {
        const float r = s * 0.28f;
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, 1.5f);
        break;
    }
    case Glyph::Dot:
        dl->AddCircleFilled(c, s * 0.22f, color, 16);
        break;
    case Glyph::Gear: {     // a cog: ring, 8 teeth, a hole
        const float r = s * 0.27f;
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159265f / 4.0f;
            const ImVec2 d(std::cos(a), std::sin(a));
            dl->AddLine(ImVec2(c.x + d.x * r * 0.9f, c.y + d.y * r * 0.9f), ImVec2(c.x + d.x * r * 1.45f, c.y + d.y * r * 1.45f),
                        color, t * 1.6f);
        }
        dl->AddCircle(c, r, color, 24, t * 1.3f);
        dl->AddCircle(c, r * 0.42f, color, 16, t);
        break;
    }
    case Glyph::Plus: {
        const float r = s * 0.3f;
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), color, t * 1.3f);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), color, t * 1.3f);
        break;
    }
    case Glyph::File: {     // a page with a folded corner
        const float w = s * 0.26f, hh = s * 0.34f, f = s * 0.12f;
        dl->PathLineTo(ImVec2(c.x - w, c.y - hh));
        dl->PathLineTo(ImVec2(c.x + w - f, c.y - hh));
        dl->PathLineTo(ImVec2(c.x + w, c.y - hh + f));
        dl->PathLineTo(ImVec2(c.x + w, c.y + hh));
        dl->PathLineTo(ImVec2(c.x - w, c.y + hh));
        dl->PathStroke(color, ImDrawFlags_Closed, t);
        break;
    }
    case Glyph::Plug: {     // attach: two prongs into a body
        const float r = s * 0.2f;
        dl->AddRect(ImVec2(c.x - r, c.y - r * 0.3f), ImVec2(c.x + r, c.y + r * 0.9f), color, 2.0f, 0, t);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y - r * 0.3f), ImVec2(c.x - r * 0.45f, c.y - r * 1.2f), color, t);
        dl->AddLine(ImVec2(c.x + r * 0.45f, c.y - r * 0.3f), ImVec2(c.x + r * 0.45f, c.y - r * 1.2f), color, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.9f), ImVec2(c.x, c.y + r * 1.6f), color, t);
        break;
    }
    }
}

bool activity_button(const char* id, Glyph g, bool active, const char* tip, const char* caption, const ImVec4* badge)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = 48.0f;
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) dl->AddRectFilled(p, ImVec2(p.x + 2, p.y + s), col(kAccent));
    const ImU32 fg = col(active || hov ? rgb(0xD7D7D7) : rgb(0x868686));
    const ImVec2 gc(p.x + s * 0.5f, p.y + (caption ? 19.0f : s * 0.5f));
    glyph(dl, g, gc, caption ? 21.0f : 26.0f, fg);
    if (caption && *caption) {
        const float fs = 11.0f;
        const ImVec2 ts = fonts.small->CalcTextSizeA(fs, FLT_MAX, 0.0f, caption);
        dl->PushClipRect(ImVec2(p.x + 2, p.y), ImVec2(p.x + s - 2, p.y + s), true);
        dl->AddText(fonts.small, fs, ImVec2(p.x + std::max(3.0f, (s - ts.x) * 0.5f), p.y + 33.0f), fg, caption);
        dl->PopClipRect();
    }
    if (badge) {
        const ImVec2 bc(gc.x + 9.0f, gc.y + 7.0f);
        dl->AddCircleFilled(bc, 5.0f, col(kSurface), 12);
        dl->AddCircleFilled(bc, 3.5f, col(*badge), 12);
    }
    if (tip && hov) ImGui::SetTooltip("%s", tip);
    return clicked;
}

bool link(const char* text, Glyph g, bool withGlyph)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float gw = withGlyph ? 22.0f : 0.0f;
    const bool clicked = ImGui::InvisibleButton(text, ImVec2(ts.x + gw, ts.y + 2));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = col(hov ? rgb(0x4DAAFF) : kLink);
    if (withGlyph) glyph(dl, g, ImVec2(p.x + 8, p.y + ts.y * 0.5f + 1), 16.0f, c);
    dl->AddText(ImVec2(p.x + gw, p.y), c, text);
    if (hov) dl->AddLine(ImVec2(p.x + gw, p.y + ts.y), ImVec2(p.x + gw + ts.x, p.y + ts.y), c);
    return clicked;
}

bool status_item(const char* text, const char* tip)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(fonts.small);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float h = ImGui::GetContentRegionAvail().y;
    const float w = ts.x + 16.0f;
    ImGui::PushID(text);
    const bool clicked = ImGui::InvisibleButton("##st", ImVec2(w, h > 0 ? h : 22.0f));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(ImVec4(1, 1, 1, 0.12f)));
    dl->AddText(ImVec2(p.x + 8.0f, p.y + (h - ts.y) * 0.5f), col(ImVec4(1, 1, 1, 1)), text);
    ImGui::PopFont();
    if (tip && hov) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(0, 0);
    return clicked;
}

} // namespace nj::ui
