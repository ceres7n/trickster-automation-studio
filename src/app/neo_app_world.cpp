// World + Character tabs: what the world model rebuilt from the packets.
#include "app/neo_app.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "app/game_data.h"
#include "app/gfx.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace nj {

namespace {

const ImVec4 kColMonster = ImVec4(1.00f, 0.42f, 0.42f, 1.0f);
const ImVec4 kColPlayer  = ImVec4(0.45f, 0.78f, 1.00f, 1.0f);
const ImVec4 kColNpc     = ImVec4(0.55f, 0.92f, 0.55f, 1.0f);
const ImVec4 kColPet     = ImVec4(0.80f, 0.60f, 1.00f, 1.0f);
const ImVec4 kColItem    = ImVec4(1.00f, 0.85f, 0.30f, 1.0f);
const ImVec4 kColOk      = ImVec4(0.50f, 0.90f, 0.50f, 1.0f);
const ImVec4 kColErr     = ImVec4(1.00f, 0.42f, 0.42f, 1.0f);
const ImVec4 kColDead    = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);

const char* kStatNames[13] = { "AP", "AC", "DX", "MaxMP", "MA", "MD", "MaxWT", "DA", "LK", "MaxHP", "DP", "HV", "Attr" };

ImVec4 kind_color(const Entity& e)
{
    if (e.dead) return kColDead;
    switch (e.kind) {
    case EK_MONSTER: return kColMonster;
    case EK_PLAYER:  return kColPlayer;
    case EK_PET:     return kColPet;
    default:         return kColNpc;
    }
}

// 0 monsters, 1 players, 2 npcs (npc / shop / skill master), 3 pets
int kind_group(uint16_t kind)
{
    switch (kind) {
    case EK_MONSTER: return 0;
    case EK_PLAYER:  return 1;
    case EK_PET:     return 3;
    default:         return 2;
    }
}

float dist(int ax, int ay, int bx, int by)
{
    float dx = float(ax - bx), dy = float(ay - by);
    return std::sqrt(dx * dx + dy * dy);
}

void bar(const char* label, int64_t v, int64_t max, ImVec4 col, float width)
{
    float frac = (max > 0 && v >= 0) ? std::min(1.0f, float(v) / float(max)) : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    std::string text = v < 0 ? std::string(label) + " ?" : fmt("%s %lld / %lld", label, (long long)v, (long long)max);
    ImGui::ProgressBar(frac, ImVec2(width, 0), text.c_str());
    ImGui::PopStyleColor();
}

// a game icon (20 px) then the text on the same line
void icon_label(const gfx::Icon& ic, const char* text, const ImVec4& color)
{
    ui::icon(ic, 20.0f);
    ImGui::SameLine(0, 6);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (20.0f - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(color, "%s", text);
}

std::string age(uint64_t then, uint64_t now)
{
    if (!then || now < then) return "-";
    uint64_t s = (now - then) / 10000000ull;
    return s < 60 ? fmt("%llus", (unsigned long long)s) : fmt("%llum%02llus", (unsigned long long)(s / 60),
                                                            (unsigned long long)(s % 60));
}

} // namespace

bool NeoApp::draw_radar(const WorldModel& w, float size, int& clickX, int& clickY, const Bot* bot)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const float scale = (size * 0.5f) / radarRange_;
    int selfX, selfY;                                // centre = where we are now (the move interpolated)
    w.self_pos_at(now_filetime(), selfX, selfY);
    ImGui::InvisibleButton("radar", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft |
                                                        ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
        radarRange_ = std::clamp(radarRange_ * (ImGui::GetIO().MouseWheel > 0 ? 0.8f : 1.25f), 100.0f, 5000.0f);
    bool clicked = false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        clickX = std::clamp(selfX + int(std::lround((m.x - c.x) / scale)), 0, 0xFFFF);
        clickY = std::clamp(selfY + int(std::lround((m.y - c.y) / scale)), 0, 0xFFFF);
        clicked = true;
    }

    dl->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), ui::col(ui::kSurface), 10.0f);
    dl->PushClipRect(p0, ImVec2(p0.x + size, p0.y + size), true);
    // the map's walls (.md3 layer 1), blocked cells merged into runs per row; coarser when zoomed out
    if (const MapGrid* map = gameData_.map(w.self.zone)) {
        const float cellPx = 16.0f * scale;
        const int step = std::max(1, int(std::ceil(2.0f / cellPx)));
        const int half = int(radarRange_ / 16.0f) + 2;
        const int ccx = selfX / 16, ccy = selfY / 16;
        const ImU32 wall = ui::col(ui::kBorder, 0.95f);
        for (int gy = std::max(0, ccy - half); gy <= std::min(map->h - 1, ccy + half); gy += step) {
            int run = -1;
            for (int gx = std::max(0, ccx - half); gx <= std::min(map->w - 1, ccx + half) + step; gx += step) {
                const bool b = gx < map->w && map->cell[size_t(gy) * size_t(map->w) + size_t(gx)] == 1;
                if (b && run < 0) run = gx;
                if (!b && run >= 0) {
                    const float x0 = c.x + (run * 16 - selfX) * scale, x1 = c.x + (gx * 16 - selfX) * scale;
                    const float y0 = c.y + (gy * 16 - selfY) * scale, y1 = c.y + ((gy + step) * 16 - selfY) * scale;
                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), wall);
                    run = -1;
                }
            }
        }
    }
    for (int ring = 1; ring <= 3; ++ring)
        dl->AddCircle(c, size * 0.5f * ring / 3.0f, ui::col(ui::kBorder, 0.6f), 64);

    auto to_screen = [&](int x, int y) {
        return ImVec2(c.x + (x - selfX) * scale, c.y + (y - selfY) * scale);
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    std::string tip;
    float tipDist = 9.0f;

    // the map's portals (atlas: the .md3 type-1 regions) and where they lead
    if (const AtlasMap* am = gameData_.atlas().map(w.self.zone)) {
        for (const AtlasPortal& p : am->portals) {
            const ImVec2 a = to_screen(p.x1, p.y1), b = to_screen(p.x2, p.y2);
            dl->AddRectFilled(a, b, IM_COL32(170, 110, 255, 60));
            dl->AddRect(a, b, IM_COL32(170, 110, 255, 200), 0.0f, 0, 1.5f);
            const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            float d = std::hypot(mouse.x - m.x, mouse.y - m.y);
            if (hovered && d < std::max(tipDist, 0.5f * std::max(b.x - a.x, b.y - a.y))) {
                tipDist = d;
                tip = "portal to " + gameData_.atlas().map_label(p.toZone);
            }
        }
    }

    for (const auto& kv : w.groundItems) {
        const GroundItem& g = kv.second;
        ImVec2 s = to_screen(g.x, g.y);
        dl->AddRectFilled(ImVec2(s.x - 2.5f, s.y - 2.5f), ImVec2(s.x + 2.5f, s.y + 2.5f), ImGui::GetColorU32(kColItem));
        float d = std::hypot(mouse.x - s.x, mouse.y - s.y);
        if (hovered && d < tipDist) {
            tipDist = d;
            tip = fmt("%s x%u  (item #%u)", gameData_.item_name(g.itemId).c_str(), g.count, g.oid);
        }
    }
    for (const auto& kv : w.entities) {
        const Entity& e = kv.second;
        if (!showKind_[kind_group(e.kind)] || (hideDead_ && e.dead)) continue;
        ImVec2 s = to_screen(e.x, e.y);
        const ImU32 col = ImGui::GetColorU32(kind_color(e));
        if (e.moving) dl->AddLine(s, to_screen(e.dx, e.dy), (col & 0x00FFFFFF) | 0x60000000);
        dl->AddCircleFilled(s, e.kind == EK_PLAYER ? 4.5f : 3.5f, col);
        if (e.kind == EK_MONSTER && e.lastTarget == w.self.eid && w.self.eid)
            dl->AddCircle(s, 7.0f, IM_COL32(255, 80, 80, 255));   // it swung at us
        float d = std::hypot(mouse.x - s.x, mouse.y - s.y);
        if (hovered && d < tipDist) {
            tipDist = d;
            tip = fmt("%s  Lv %u  #%u%s", e.name.empty() ? entity_kind_name(e.kind) : e.name.c_str(), e.level, e.eid,
                      e.dead ? "  (dead)" : "");
        }
    }
    if (bot) {
        const ImVec2 a = to_screen(bot->anchorX, bot->anchorY);   // the start point
        dl->AddCircle(a, 5.0f, ui::col(ui::kMuted), 12, 1.5f);
        const RoamAction& r = bot->roam;                    // the monster quest's spawn spots (visited this round: muted)
        if (r.spotZone() == w.self.zone)
            for (size_t i = 0; i < r.spots.size(); ++i) {
                const RoamAction::Spot& sp = r.spots[i];
                const ImVec2 q = to_screen(sp.x, sp.y);
                const bool goal = int(i) == r.spotGoal();
                dl->AddCircle(q, goal ? 8.0f : 6.0f, ui::col(sp.done ? ui::kMuted : ui::kAccent, goal ? 1.0f : 0.75f), 16,
                              goal ? 2.0f : 1.2f);
                const float d = std::hypot(mouse.x - q.x, mouse.y - q.y);
                if (hovered && d < tipDist) {
                    tipDist = d;
                    tip = fmt("spawn spot %zu: %d points%s", i + 1, sp.points, sp.done ? " (visited this round)" : "");
                }
            }
        if (bot->roam.active()) {
            const ImVec2 d = to_screen(bot->roam.destX, bot->roam.destY);
            dl->AddLine(c, d, ui::col(ui::kAccent, 0.5f), 1.0f);
            dl->AddCircleFilled(d, 3.5f, ui::col(ui::kAccent), 12);
        }
        if (bot->attack.target) {
            auto it = w.entities.find(bot->attack.target);
            if (it != w.entities.end()) dl->AddCircle(to_screen(it->second.x, it->second.y), 9.0f, ui::col(ui::kWarn), 20, 2.0f);
        }
        const Navigator& nav = bot->town.nav;          // a trip: the legs of its walk
        if (nav.active() && !nav.legs().empty()) {
            ImVec2 prev = c;
            for (size_t i = nav.leg(); i < nav.legs().size(); ++i) {
                const ImVec2 q = to_screen(nav.legs()[i].first, nav.legs()[i].second);
                dl->AddLine(prev, q, IM_COL32(120, 220, 140, 200), 1.5f);
                dl->AddCircleFilled(q, 2.5f, IM_COL32(120, 220, 140, 255), 8);
                prev = q;
            }
        }
    }
    if (w.self.moving) dl->AddLine(c, to_screen(w.self.dx, w.self.dy), IM_COL32(255, 255, 255, 110));
    if (moveSet_) {                                  // the quick-action move point
        const ImVec2 m = to_screen(moveX_, moveY_);
        dl->AddLine(ImVec2(m.x - 5, m.y - 5), ImVec2(m.x + 5, m.y + 5), IM_COL32(255, 210, 80, 255), 1.5f);
        dl->AddLine(ImVec2(m.x - 5, m.y + 5), ImVec2(m.x + 5, m.y - 5), IM_COL32(255, 210, 80, 255), 1.5f);
    }
    dl->AddCircleFilled(c, 5.0f, IM_COL32(255, 255, 255, 255));
    dl->PopClipRect();
    dl->AddText(ImVec2(p0.x + 10, p0.y + 8), ui::col(ui::kMuted), fmt("%.0f", radarRange_).c_str());
    if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
    return clicked;
}

void NeoApp::draw_world()
{
    Session* s = current();
    if (!s) {
        ImGui::TextDisabled("No client. Launch or attach one in Clients.");
        return;
    }
    const WorldModel& w = *s->world;
    const SelfState& me = w.self;
    const uint64_t now = now_filetime();

    // header
    ImGui::Text("%s", me.name.empty() ? "(character not seen yet)" : me.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Lv %d  TM %d  zone %d  (%d, %d)%s  #%u", me.level, me.tmLevel, me.zone, me.x, me.y,
                        me.moving ? fmt(" -> (%d, %d)", me.dx, me.dy).c_str() : "", me.eid);
    if (me.dead) { ImGui::SameLine(); ImGui::TextColored(kColMonster, "DEAD"); }
    bar("HP", me.hp, me.hpMax, ImVec4(0.75f, 0.20f, 0.25f, 1.0f), 260);
    ImGui::SameLine();
    bar("MP", me.mp, me.mpMax, ImVec4(0.20f, 0.40f, 0.85f, 1.0f), 260);
    ImGui::SameLine();
    ImGui::TextDisabled("EXP +%lld  TM +%lld  kills %d  deaths %d", (long long)me.expGained, (long long)me.tmGained,
                        me.kills, me.deaths);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("Max HP / MP = the highest value seen (the client derives the real max from the stats).\n"
                          "Model: %llu packets applied, %llu malformed%s%s", (unsigned long long)w.applied,
                          (unsigned long long)w.malformed, w.lastMalformed.empty() ? "" : ", last ",
                          w.lastMalformed.c_str());

    // radar | lists
    const float radarSize = std::min(360.0f, ImGui::GetContentRegionAvail().y - 60.0f);
    ImGui::BeginGroup();
    int clickX = 0, clickY = 0;
    if (draw_radar(w, std::max(160.0f, radarSize), clickX, clickY, &s->bot)) {
        moveX_ = clickX; moveY_ = clickY; moveSet_ = true;
        do_action(*s, act::move(w, clickX, clickY, quickRun_, now));
    }
    ImGui::Checkbox("Monsters", &showKind_[0]); ImGui::SameLine();
    ImGui::Checkbox("Players", &showKind_[1]);
    ImGui::Checkbox("NPCs", &showKind_[2]); ImGui::SameLine();
    ImGui::Checkbox("Pets", &showKind_[3]); ImGui::SameLine();
    ImGui::Checkbox("Hide dead", &hideDead_);
    ImGui::TextDisabled("wheel = zoom, right-click = move there");
    ImGui::SetItemTooltip("Right-click fills a C2S 0x18 move in the Send tab (and sends it when \"Send at once\" "
                          "is on there).");
    if (!actionStatus_.empty()) ImGui::TextColored(actionStatusOk_ ? kColOk : kColErr, "%s%s", actionStatus_.c_str(),
                                                   quickSend_ ? "" : "  (in Send)");
    ImGui::EndGroup();
    ImGui::SameLine();

    ImGui::BeginChild("worldlists", ImVec2(0, 0));
    if (ImGui::BeginTabBar("worldtabs")) {
        if (ImGui::BeginTabItem(fmt("Entities (%zu)###ents", w.entities.size()).c_str())) {
            struct Row { const Entity* e; float d; };
            std::vector<Row> rows;
            for (const auto& kv : w.entities) {
                const Entity& e = kv.second;
                if (!showKind_[kind_group(e.kind)] || (hideDead_ && e.dead)) continue;
                rows.push_back({ &e, dist(e.x, e.y, me.x, me.y) });
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
            if (ImGui::BeginTable("ents", 8, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                             ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Id", 0, 50);
                ImGui::TableSetupColumn("Kind", 0, 70);
                ImGui::TableSetupColumn("Name", 0, 170);
                ImGui::TableSetupColumn("Lv", 0, 35);
                ImGui::TableSetupColumn("HP", 0, 110);
                ImGui::TableSetupColumn("Dist", 0, 55);
                ImGui::TableSetupColumn("Pos", 0, 100);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const Row& r : rows) {
                    const Entity& e = *r.e;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%u", e.eid);
                    ImGui::TableNextColumn(); ImGui::TextColored(kind_color(e), "%s", entity_kind_name(e.kind));
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%u", e.level);
                    ImGui::TableNextColumn();
                    if (e.hp >= 0) ImGui::Text("%lld / %lld", (long long)e.hp, (long long)e.maxHp);
                    else if (e.maxHp) ImGui::TextDisabled("full? / %lld", (long long)e.maxHp);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", r.d);
                    ImGui::TableNextColumn(); ImGui::Text("%d, %d", e.x, e.y);
                    ImGui::TableNextColumn();
                    std::string st;
                    if (e.dead) st += "dead ";
                    if (e.moving) st += fmt("-> %d,%d ", e.dx, e.dy);
                    if (e.sit) st += fmt("sit %u ", e.sit);
                    if (e.owner) st += "owner " + w.entity_label(e.owner) + " ";
                    if (e.lastTarget) st += "target " + w.entity_label(e.lastTarget) + " ";
                    if (e.hitBySelf) st += "hit by you";
                    ImGui::TextUnformatted(st.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(fmt("Ground items (%zu)###items", w.groundItems.size()).c_str())) {
            struct Row { const GroundItem* g; float d; };
            std::vector<Row> rows;
            for (const auto& kv : w.groundItems) rows.push_back({ &kv.second, dist(kv.second.x, kv.second.y, me.x, me.y) });
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
            if (ImGui::BeginTable("gitems", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Id", 0, 50);
                ImGui::TableSetupColumn("Item", 0, 230);
                ImGui::TableSetupColumn("Count", 0, 55);
                ImGui::TableSetupColumn("Dist", 0, 55);
                ImGui::TableSetupColumn("Pos", 0, 100);
                ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const Row& r : rows) {
                    const GroundItem& g = *r.g;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%u", g.oid);
                    ImGui::TableNextColumn(); icon_label(gfx::item(g.itemId), gameData_.item_name(g.itemId).c_str(), kColItem);
                    ImGui::TableNextColumn(); ImGui::Text("%u", g.count);
                    ImGui::TableNextColumn(); ImGui::Text("%.0f", r.d);
                    ImGui::TableNextColumn(); ImGui::Text("%d, %d", g.x, g.y);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(age(g.seenAt, now).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(fmt("Events (%zu)###events", w.events.size()).c_str())) {
            ImGui::Checkbox("Follow", &eventsFollow_);
            ImGui::BeginChild("evlist", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (const std::string& ev : w.events) ImGui::TextUnformatted(ev.c_str());
            if (eventsFollow_) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void NeoApp::draw_character()
{
    Session* s = current();
    if (!s) {
        ImGui::TextDisabled("No client.");
        return;
    }
    const WorldModel& w = *s->world;
    const SelfState& me = w.self;
    if (!gameDataStatus_.empty()) ImGui::TextDisabled("%s", gameDataStatus_.c_str());

    if (ImGui::BeginTable("charcols", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableNextRow();

        // ── stats + equipment ──
        ImGui::TableNextColumn();
        ImGui::BeginChild("charleft", ImVec2(0, 0));
        ImGui::SeparatorText("Character");
        ImGui::Text("%s", me.name.empty() ? "?" : me.name.c_str());
        ImGui::TextDisabled("uid %llu  entity #%u  species %u  hair %u  f_type %u", (unsigned long long)me.charUid,
                            me.eid, me.species, me.hair, me.fType);
        ImGui::Text("Level %d   TM %d   TM points %s", me.level, me.tmLevel,
                    me.tmPoints < 0 ? "?" : fmt("%d", me.tmPoints).c_str());
        bar("HP", me.hp, me.hpMax, ImVec4(0.75f, 0.20f, 0.25f, 1.0f), -1);
        bar("MP", me.mp, me.mpMax, ImVec4(0.20f, 0.40f, 0.85f, 1.0f), -1);
        ImGui::Text("EXP %lld   TM EXP %lld", (long long)me.exp, (long long)me.tmExp);
        ImGui::Text("Galder %lld", (long long)me.money);
        ImGui::Text("Zone %d   home %d   pos (%d, %d)", me.zone, me.homeZone, me.x, me.y);

        ImGui::SeparatorText("Stat points");
        if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_SizingStretchSame)) {
            for (int i = 0; i < 13; ++i) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", kStatNames[i]);
                ImGui::SameLine();
                ImGui::Text("%u", me.statPoints[i]);
            }
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Equipment");
        for (int sl = 0; sl < 23; ++sl) {
            if (!me.equip[sl]) continue;
            const InvItem* it = w.find_item(me.equip[sl]);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%2d", sl);
            ImGui::SameLine();
            icon_label(it ? gfx::item(it->itemId) : gfx::Icon{},
                       it ? gameData_.item_name(it->itemId).c_str() : fmt("uid %llu", (unsigned long long)me.equip[sl]).c_str(), ui::kText);
        }
        ImGui::EndChild();

        // ── inventory + skills ──
        ImGui::TableNextColumn();
        ImGui::BeginChild("charright", ImVec2(0, 0));
        if (ImGui::BeginTabBar("chartabs")) {
            if (ImGui::BeginTabItem(fmt("Inventory (%zu)###inv", w.inventory.size()).c_str())) {
                if (ImGui::BeginTable("inv", 6, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Item", 0, 240);
                    ImGui::TableSetupColumn("Id", 0, 60);
                    ImGui::TableSetupColumn("Count", 0, 55);
                    ImGui::TableSetupColumn("Type", 0, 50);
                    ImGui::TableSetupColumn("Worn", 0, 45);
                    ImGui::TableSetupColumn("Uid", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for (const InvItem& i : w.inventory) {
                        const ItemInfo* info = gameData_.item(i.itemId);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        icon_label(gfx::item(i.itemId), gameData_.item_name(i.itemId).c_str(), ui::kText);
                        ImGui::TableNextColumn(); ImGui::Text("%u", i.itemId);
                        ImGui::TableNextColumn(); ImGui::Text("%u", i.count);
                        ImGui::TableNextColumn(); ImGui::Text("%d", info ? info->type : -1);
                        ImGui::TableNextColumn();
                        if (i.equipSlot >= 0) ImGui::TextColored(kColNpc, "%d", i.equipSlot);
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)i.uid);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(fmt("Skills (%zu)###skills", w.skills.size()).c_str())) {
                if (ImGui::BeginTable("skills", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                   ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Id", 0, 60);
                    ImGui::TableSetupColumn("Skill", 0, 240);
                    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();
                    for (const SkillEntry& sk : w.skills) {
                        const SkillInfo* info = gameData_.skill(sk.id);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%u", sk.id);
                        ImGui::TableNextColumn(); icon_label(gfx::skill(sk.id), gameData_.skill_name(sk.id).c_str(), ui::kText);
                        ImGui::TableNextColumn();
                        if (info && info->maxLevel) ImGui::Text("%u / %d", sk.level, info->maxLevel);
                        else ImGui::Text("%u", sk.level);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

} // namespace nj
