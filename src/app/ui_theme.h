#pragma once
// The look of Trickster Automation Studio: VS Code's "Dark Modern" — activity bar, side bar, editor tabs, a bottom
// panel and the status bar — on ImGui, plus the small widgets the pages share (toggle, pill, stat
// tile, meter, the game's icons with a cool-time sweep).
#include "app/gfx.h"
#include "imgui.h"
#include <cstdint>

namespace nj::ui {

// palette (VS Code Dark Modern)
extern const ImVec4 kBg;            // editor               #1F1F1F
extern const ImVec4 kSurface;       // side bar / panel / tabs / activity bar   #181818
extern const ImVec4 kSurface2;      // inputs, buttons      #313131
extern const ImVec4 kBorder;        //                      #2B2B2B
extern const ImVec4 kHover;         // list hover           #2A2D2E
extern const ImVec4 kSelect;        // list selection       #04395E
extern const ImVec4 kText, kMuted, kAccent, kOk, kWarn, kErr;
extern const ImVec4 kStatusIdle;    // status bar: no client     #68217A (VS Code: no folder)
extern const ImVec4 kStatusLive;    //             client ready  #007ACC
extern const ImVec4 kStatusRun;     //             bot running   #CC6633 (VS Code: debugging)
ImU32 col(const ImVec4& c, float alpha = 1.0f);

struct Fonts {
    ImFont* ui = nullptr;           // Segoe UI 14
    ImFont* bold = nullptr;         // Segoe UI Semibold 14
    ImFont* big = nullptr;          // Segoe UI Semibold 20 (numbers, titles)
    ImFont* small = nullptr;        // Segoe UI 12 (captions, section headers)
    ImFont* mono = nullptr;         // Consolas 14
    ImFont* title = nullptr;        // Segoe UI Light 34 (the Home page)
};
extern Fonts fonts;

void load_fonts();                  // before the first frame
void apply_theme();

// widgets
bool toggle(const char* id, bool* v);                       // a switch; true when flipped
void pill(const char* text, const ImVec4& color);           // a rounded status label
bool button_primary(const char* label, const ImVec2& size = ImVec2(0, 0), const ImVec4& color = kAccent);
void label(const char* text);                               // small muted caption
void stat(const char* caption, const char* value, float width);   // caption over a big value
void meter(float frac, const ImVec4& color, float width, float height = 4.0f);
// a bar with its text on it ("1234 / 2000")
void bar(float frac, const ImVec4& color, float width, const char* text, float height = 16.0f);
// segmented control: n options; returns true when changed
bool segmented(const char* id, int* value, const char* const* items, int n);
// A card: a bordered block with a header (title, a muted status, an optional enable switch on the
// right). Always pair with card_end().
void card_begin(const char* id, const char* title, const char* status, bool* enabled);
void card_end();
void help(const char* text);                                // a muted (?) with a wrapped tooltip
// A page header: the title, a muted status, an optional enable switch at the right.
void page_header(const char* title, const char* status, bool* enabled);

// the game's icons
// size x size, centred, the image kept at its own aspect; an empty icon draws a muted square.
void icon(const gfx::Icon& ic, float size, float alpha = 1.0f);
// the icon with a dark sweep over the part of the cool time left (frac 1 = all of it) + the seconds
void icon_cool(const gfx::Icon& ic, float size, float frac, const char* text);

// VS Code chrome
// Editor tabs: n labels; dots[i] = a filled circle after the label (e.g. an enabled action). true = changed.
bool tab_strip(const char* id, int* sel, const char* const* labels, int n, const bool* dots = nullptr,
               float height = 35.0f);
// A side bar section header (chevron + UPPERCASE title); returns the open state.
bool section(const char* title, bool* open);
enum class Glyph { Bot, Debug, Play, Stop, Dot, Gear, Plus, File, Plug };
// An activity bar button (48 x 48): the glyph, the left indicator when active. A caption puts a short
// text under a smaller glyph; a badge is a status dot on its corner. true = clicked.
bool activity_button(const char* id, Glyph g, bool active, const char* tip, const char* caption = nullptr,
                     const ImVec4* badge = nullptr);
// A VS Code link: link-blue text, underlined on hover; an optional glyph before it. true = clicked.
bool link(const char* text, Glyph g = Glyph::Dot, bool withGlyph = false);
extern const ImVec4 kLink;          // #3794FF
void glyph(ImDrawList* dl, Glyph g, ImVec2 centre, float size, ImU32 color);
// A status bar item: text on the bar's colour, a lighter hover. true = clicked.
bool status_item(const char* text, const char* tip = nullptr);

} // namespace nj::ui
