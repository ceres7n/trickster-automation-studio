#pragma once
// Small widgets shared by the Bot view's pages (neo_app_bot.cpp, neo_app_town.cpp).
#include "app/gfx.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include <cstdint>
#include <string>

namespace nj::bui {

constexpr float kIcon = 28.0f;

inline std::string thousands(int64_t v)
{
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return (v < 0 ? "-" : "") + s;
}

inline void toggle_row(const char* text, bool* v)
{
    ImGui::PushID(text);
    ui::toggle("##t", v);
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
}

inline void slider(const char* text, int* v, int lo, int hi, const char* format = "%d", float width = 200.0f)
{
    ImGui::SetNextItemWidth(width);
    ImGui::SliderInt(text, v, lo, hi, format);
}

// An icon then a text, vertically centred on it.
inline void icon_text(const gfx::Icon& ic, const std::string& text, const ImVec4& color = ui::kText, float size = kIcon)
{
    ui::icon(ic, size);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(color, "%s", text.c_str());
}

// a muted paragraph, wrapped to the page
inline void note(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// the next text of an icon row's cell, centred on the icon's height
inline void mid()
{
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
}

inline bool begin_table(const char* id, int cols)
{
    return ImGui::BeginTable(id, cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                                           ImGuiTableFlags_PadOuterX);
}

inline void section_title(const char* text)
{
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushFont(ui::fonts.bold);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

} // namespace nj::bui
