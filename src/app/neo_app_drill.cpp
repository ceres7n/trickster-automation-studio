// The Bot view's Drill page: drill mode's settings, the dig in progress, the drill and the bag's other drills,
// this map's buried items and what the bot knows of them (a little map of the cells around us), the detection
// skills, the DA buffs (the Buffs action's own list) and the finds. The rules: bot/act_drill.cpp.
#include "app/neo_app.h"
#include "app/atlas.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/gfx.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nj {

namespace {

using namespace bui;

const char* ground_name(int n)
{
    static const char* const k[] = { "any ground", "sand", "water", "earth", "rock", "snow", "not diggable" };
    return n >= 0 && n <= 6 ? k[n] : "?";
}

bool da_buff(const SkillInfo& si)
{
    // ESADaUp (Sixth Sense), ESADaUpScroll, the Bear's MindUp / AngerEnergy (es.cpp: stat 7 = DA); Boiling
    // (Crazy Drilling, ESAction 1391) fills the Crazy Drill gauge faster
    return si.esAction == 1092 || si.esAction == 1305 || si.esAction == 1525 || si.esAction == 1540 || si.esAction == 1391;
}

std::string secs_text(double s)
{
    if (s < 0) s = 0;
    if (s > 30.0 * 86400.0) return "-";         // (no time: an offline capture)
    const long long n = (long long)s;
    if (n >= 3600) return fmt("%lld:%02lld:%02lld", n / 3600, n / 60 % 60, n % 60);
    return s >= 60 ? fmt("%lld:%02lld", n / 60, n % 60) : fmt("%.1f s", s);
}

// how long ago `ft` was ("-" for no time)
std::string ago(uint64_t now, uint64_t ft)
{
    if (!ft || ft > now) return "-";
    return secs_text(double(now - ft) / 1e7) + " ago";
}

// the cell map's colours
const ImVec4 kCellUnknown(0.38f, 0.38f, 0.42f, 1);
const ImVec4 kCellOther(0.29f, 0.29f, 0.33f, 1);      // unknown, another ground than the drill's
const ImVec4 kCellScanned(0.17f, 0.24f, 0.36f, 1);    // a detector saw nothing that deep
const ImVec4 kCellEmpty(0.26f, 0.16f, 0.16f, 1);
const ImVec4 kCellHard(0.34f, 0.25f, 0.16f, 1);       // a ground that cannot be dug
const ImVec4 kCellClaimed(0.55f, 0.35f, 0.75f, 1);
const ImVec4 kCellBad(0.60f, 0.22f, 0.22f, 1);        // the server said "not diggable"

} // namespace

void NeoApp::bot_drill(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    DrillAction& d = bot.drill;
    const uint64_t now = now_filetime();
    ui::page_header("Drill", bot.running && d.enabled ? d.status.c_str() : "dig the buried items (C2S 0x2C / 0x31)", &d.enabled);
    ImGui::Dummy(ImVec2(0, 2));

    // ── the numbers ──
    const DrillStats& st = w.drillStats;
    const double hours = bot.running && now > bot.startedAt ? double(now - bot.startedAt) / 36e9 : 0.0;
    ui::stat("FINDS (RUN)", std::to_string(d.runFinds).c_str(), 120);
    ImGui::SameLine();
    ui::stat("FINDS / H", hours > 0.01 ? fmt("%.0f", d.runFinds / hours).c_str() : "-", 110);
    ImGui::SameLine();
    ui::stat("DIGS", std::to_string(st.digs).c_str(), 90);
    ImGui::SameLine();
    ui::stat("FOUND", std::to_string(st.finds).c_str(), 90);
    ImGui::SameLine();
    ui::stat("LIFE USED", std::to_string(st.lifeUsed).c_str(), 110);
    ImGui::SameLine();
    ui::stat("PROBES (RUN)", std::to_string(d.probes).c_str(), 120);

    // ── behaviour ──
    section_title("Behaviour");
    toggle_row("Fight back: kill the monsters that attack you while drilling", &d.fightBack);
    ImGui::SameLine();
    ui::help("Drill mode never hunts. With this on, a monster that swings at you is fought with the Attack tab's "
             "weapon and skills (whatever its lists say), between digs. A swing ends a dig anyway unless you have "
             "Invincible Drill (3001); with it the dig goes on and is dropped only when more than 1.5 s are left.");
    static const char* kArea[] = { "Around the start point", "Whole map" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "Area");
    ImGui::SameLine(110);
    ui::segmented("##area", &d.area, kArea, 2);
    if (d.area == DrillAction::AroundStart) {
        ImGui::SameLine(0, 12);
        slider("##radius", &d.radius, 48, 3000, "radius %d", 180);
        ImGui::SameLine(0, 12);
        toggle_row("move on when it runs dry", &d.moveOn);
        ImGui::SameLine();
        ui::help("When nothing is left to dig around the start point (or every block there keeps failing), the start "
                 "point moves to the nearest block that still has cells to dig.");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "Lines");
    ImGui::SameLine(110);
    slider("##lines", &d.lineCells, 5, 21, "blocks %d cells a side", 220);
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "(without a detector)");
    ImGui::SameLine();
    ui::help("The map is dug block by block, row by row in a snake from the block's nearest corner. With a detection "
             "skill the block is its square (Basic Detection Lv 11: 15 x 15) and it is cast once, when the block "
             "starts: then only its points are dug. Without one, the blocks have this size. A block where too many "
             "probes in a row find nothing (4 / the chance a cell holds an item your drill gets) is left for 10 min, "
             "and the next one is taken away from it: another area.");
    static const char* kGround[] = { "Any ground", "The drill's ground first", "Only the drill's ground" };
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "Ground");
    ImGui::SameLine(110);
    ui::segmented("##ground", &d.ground, kGround, 3);
    ImGui::SameLine();
    ui::help("A dig on another ground than the drill's (its GroundNature) costs twice the life. \"First\" walks up "
             "to 4 cells farther for the drill's own ground; \"only\" never digs another one.");
    toggle_row("Dig the cells with nothing to find too (the dig's EXP, 1 drill life each)", &d.forExp);
    toggle_row("Pick up the finds (they are yours for 30 s), whatever the Loot lists say", &d.pickUp);
    toggle_row("Swap drills: a worn-out one, or one too short for this map, for the best of the bag", &d.swap);
    note("Each cell is probed first: S2C 371 tells whether an item lies there and how deep. An item the drill reaches "
         "is dug out (reported once the gauge would have got there, one start: 1 life); anything else is cancelled at "
         "once for free. Cells are taken block by block, in lines; known empty cells are never tried again. While a "
         "dig is open only potions are used: a skill, sitting or an attack would end it; buffs, rest, loot and town "
         "trips wait for the end of the dig.");

    // ── now ──
    section_title("Now");
    const DrillSession& ds = w.drill;
    const bool open = w.drill_open(now);
    if (open) {
        static const char* kPhase[] = { "idle", "asked", "walking to the cell", "digging", "waiting for the result" };
        ImGui::Text("%s  (%d, %d)", kPhase[std::min<int>(ds.phase, 4)], ds.x, ds.y);
        if (ds.phase >= DrillSession::Info) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kMuted, "   map depth %d  -  %s  -  %s%s", ds.mapDepth,
                               ds.itemDepth ? fmt("an item %d deep", ds.itemDepth).c_str() : "no item",
                               ground_name(ds.nature), ds.sensed ? "  -  sensed" : "");
        }
    } else {
        ImGui::TextColored(ui::kMuted, "no dig open");
    }
    if (d.hasBlock && d.blockZone == w.self.zone) {
        ImGui::TextColored(ui::kMuted, "block at (%d, %d), %d x %d cells: %d still to dig  -  %d finds in %d probes  -  "
                           "nothing %d in a row (left at %d)%s", 16 * d.blockCellX, 16 * d.blockCellY, d.blockSide, d.blockSide,
                           d.blockLeft, d.blockFinds, d.blockProbes, d.blockFails, d.failLimit,
                           d.blockScan ? fmt("  -  scanned with %s", gameData_.skill_name(d.blockScan).c_str()).c_str() : "");
    }
    if (!d.plan.empty()) ImGui::TextColored(ui::kMuted, "last plan: %s", d.plan.c_str());
    if (w.drillLast.at) {
        static const char* kWhy[] = { "", "cancelled", "nothing", "dug out", "too deep for the drill", "", "drill worn out",
                                      "ground not diggable", "Crazy Drill find", "nothing", "interrupted by a hit" };
        const uint8_t r = w.drillLast.reason;
        const char* why = r < 11 ? kWhy[r] : r == 19 ? "Crazy Drill find" : r == 15 ? "no Crazy Drill here" : "?";
        ImGui::TextColored(ui::kMuted, "last result: %s%s  (%s)", why,
                           w.drillLast.item ? fmt(" - %s", gameData_.item_name(w.drillLast.item).c_str()).c_str() : "",
                           ago(now, w.drillLast.at).c_str());
    }
    if (w.cdm_on(now))
        ui::pill(fmt("CRAZY DRILL  %s left", secs_text(double(w.cdmUntil - now) / 1e7).c_str()).c_str(), ui::kWarn);
    else if (w.skill_level(5019) && w.cdmGauge >= 0)
        ImGui::TextColored(ui::kMuted, "Crazy Drill gauge %d %%", w.cdmGauge);

    // ── the drill ──
    section_title("Drill");
    const InvItem* di = equipped_drill(w);
    const DrillInfo* dinfo = equipped_drill_info(w);
    const MapDigInfo* md = gameData_.map_dig(w.self.zone);
    const MapGrid* g = gameData_.map(w.self.zone);
    static int shareZone = -1;              // each ground's share (%) of the map's diggable cells
    static int shares[8] = {};
    if (shareZone != w.self.zone) {
        shareZone = w.self.zone;
        int count[8] = {}, total = 0;
        if (g)
            for (int y = 0; y < g->h; ++y)
                for (int x = 0; x < g->w; ++x)
                    if (g->diggable(x, y)) { ++total; ++count[g->nature_at(x, y) & 7]; }
        for (int n = 0; n < 8; ++n) shares[n] = total ? count[n] * 100 / total : 0;
    }
    if (!di) {
        ImGui::TextColored(ui::kWarn, "no drill in slot 7");
    } else {
        icon_text(gfx::item(di->itemId), gameData_.item_name(di->itemId));
        if (dinfo) {
            ImGui::SameLine(340);
            mid();
            ui::bar(dinfo->drillLife > 0 ? float(di->count) / float(dinfo->drillLife) : 1.0f, di->count ? ui::kOk : ui::kErr, 220,
                    fmt("life %u / %d", di->count, dinfo->drillLife).c_str());
            const int nat = dinfo->groundNature;
            ImGui::TextColored(ui::kMuted, "reaches %d deep  -  grade %d  -  %s%s", dinfo->digDepth, dinfo->grade,
                               ground_name(nat), nat && g ? fmt(" (%d %% of this map)", shares[nat & 7]).c_str() : "");
            if (md) {
                const int a = std::min(dinfo->digDepth, md->minDepth), b = std::min(dinfo->digDepth, md->maxDepth);
                ImGui::TextColored(ui::kMuted, "a find here takes %s to %s (the gauge: bar %d, area %d, %d per scoop)",
                                   secs_text(drill_dig_ms(*dinfo, a) / 1000.0).c_str(), secs_text(drill_dig_ms(*dinfo, b) / 1000.0).c_str(),
                                   dinfo->barSpeed, dinfo->barEffArea, dinfo->scoopMass);
                const double rate = gameData_.drill_exp_rate(dinfo->grade);
                const int lv = std::max(1, w.self.level);
                ImGui::TextColored(ui::kMuted, "EXP per dig %.0f..%.0f  -  TM per find %d..%d (TM below 500)",
                                   std::max(1.0, rate * a * (lv + 400) * 0.0005), std::max(1.0, rate * b * (lv + 400) * 0.0005),
                                   4 * (3 * lv + a) * dinfo->grade, 4 * (3 * lv + b) * dinfo->grade);
            }
        } else {
            ImGui::TextColored(ui::kWarn, "no Item_DrillParam row for it");
        }
    }
    // the bag's other drills
    int others = 0;
    for (const InvItem& it : w.inventory) {
        const ItemInfo* ii = gameData_.item(it.itemId);
        if (!ii || ii->type != 6 || it.uid == w.self.equip[7]) continue;
        if (!others++) ImGui::TextColored(ui::kMuted, "in the bag:");
        const DrillInfo* x = gameData_.drill(it.itemId);
        icon_text(gfx::item(it.itemId), gameData_.item_name(it.itemId), ui::kText, 22);
        ImGui::SameLine(340);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
        if (x) ImGui::TextColored(ui::kMuted, "life %u / %d  -  reaches %d  -  grade %d  -  %s", it.count, x->drillLife, x->digDepth,
                                  x->grade, ground_name(x->groundNature));
    }

    // ── this map ──
    section_title("This map");
    if (!md) {
        ImGui::TextColored(ui::kMuted, "%s: nothing is buried here", gameData_.map_name(w.self.zone).c_str());
    } else {
        ImGui::Text("%s", gameData_.map_name(w.self.zone).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, "  an item in %.0f %% of the diggable cells, %d..%d deep%s", double(md->itemPct), md->minDepth,
                           md->maxDepth, (md->option & 512) ? "  -  no Crazy Drill Mode" : "");
        if (dinfo) {
            if (dinfo->digDepth < md->minDepth)
                ImGui::TextColored(ui::kErr, "the drill reaches %d: none of these items", dinfo->digDepth);
            else if (dinfo->digDepth < md->maxDepth)
                ImGui::TextColored(ui::kWarn, "the drill reaches %d: only the items down to it", dinfo->digDepth);
            if (g && dinfo->groundNature && shares[dinfo->groundNature & 7] < 25)
                ImGui::TextColored(ui::kWarn, "the drill's ground (%s) is %d %% of this map: a dig elsewhere costs twice the life%s",
                                   ground_name(dinfo->groundNature), shares[dinfo->groundNature & 7],
                                   d.ground == DrillAction::OnlyGround ? " (and \"only the drill's ground\" leaves the rest)" : "");
        }
        if (g) {
            std::string grounds;
            for (int n = 1; n <= 5; ++n)
                if (shares[n]) grounds += fmt("%s%s %d %%", grounds.empty() ? "" : "  -  ", ground_name(n), shares[n]);
            if (shares[0]) grounds += fmt("%s(none) %d %%", grounds.empty() ? "" : "  -  ", shares[0]);
            ImGui::TextColored(ui::kMuted, "grounds: %s", grounds.c_str());
        }
        // what we know
        int nItem = 0, nDeep = 0, nEmpty = 0, nDet = 0, nNone = 0;
        const int need = dinfo ? std::min(dinfo->digDepth, md->maxDepth) : md->maxDepth;
        if (const auto* z = w.buried_here())
            for (const auto& kv : *z) {
                const BuriedCell& b = kv.second;
                if (b.state == BuriedCell::Item) (b.depth <= need ? nItem : nDeep)++;
                else if (b.state == BuriedCell::Empty) ++nEmpty;
                else if (b.state == BuriedCell::Detected) ++nDet;
                else if (b.state == BuriedCell::Unknown && b.noneTo >= need) ++nNone;
            }
        ImGui::TextColored(ui::kMuted, "known cells: %d items within reach, %d detected, %d too deep, %d empty, %d scanned empty",
                           nItem, nDet, nDeep, nEmpty, nNone);

        // the cells around us
        if (g) {
            constexpr int R = 22;
            constexpr float px = 7.0f;
            const float side = (2 * R + 1) * px;
            int mx = 0, my = 0;
            w.self_pos_at(now, mx, my);
            const int ccx = mx / 16, ccy = my / 16;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##cells", ImVec2(side, side));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), ui::col(ui::kSurface));
            for (int dy = -R; dy <= R; ++dy)
                for (int dx = -R; dx <= R; ++dx) {
                    const int cx = ccx + dx, cy = ccy + dy;
                    if (!g->inside(cx, cy)) continue;
                    ImVec4 c;
                    if (g->raw(cx, cy) == 1) continue;                        // a wall: the background
                    else if (!g->diggable(cx, cy)) c = kCellHard;
                    else {
                        c = kCellUnknown;
                        if (dinfo && dinfo->groundNature && g->nature_at(cx, cy) != dinfo->groundNature) c = kCellOther;
                        if (const BuriedCell* b = w.buried_at(cx, cy)) {
                            switch (b->state) {
                            case BuriedCell::Empty:      c = kCellEmpty; break;
                            case BuriedCell::Item:       c = b->depth <= need ? ui::kOk : ui::kWarn; break;
                            case BuriedCell::Detected:   c = ui::kAccent; break;
                            case BuriedCell::Undiggable: c = kCellBad; break;
                            case BuriedCell::Claimed:    c = kCellClaimed; break;
                            default: if (b->noneTo >= need) c = kCellScanned; break;
                            }
                        }
                    }
                    const ImVec2 a(p0.x + (dx + R) * px, p0.y + (dy + R) * px);
                    dl->AddRectFilled(a, ImVec2(a.x + px - 1, a.y + px - 1), ui::col(c));
                }
            auto at = [&](int ux, int uy) {
                return ImVec2(p0.x + (float(ux) / 16.0f - ccx + R) * px, p0.y + (float(uy) / 16.0f - ccy + R) * px);
            };
            // the blocks left for failing, then the block being worked
            auto cellPos = [&](int cx, int cy) { return ImVec2(p0.x + (cx - ccx + R) * px, p0.y + (cy - ccy + R) * px); };
            dl->PushClipRect(p0, ImVec2(p0.x + side, p0.y + side), true);
            for (const auto& kv : d.poor()) {
                if (now >= kv.second || int(kv.first >> 32) != (w.self.zone & 0xFFFF)) continue;
                const int bx = int16_t((kv.first >> 16) & 0xFFFF), by = int16_t(kv.first & 0xFFFF);
                dl->AddRect(cellPos(bx, by), cellPos(bx + d.blockSide, by + d.blockSide), ui::col(ui::kErr, 0.8f), 0, 0, 1.5f);
            }
            if (d.hasBlock && d.blockZone == w.self.zone)
                dl->AddRect(cellPos(d.blockCellX, d.blockCellY), cellPos(d.blockCellX + d.blockSide, d.blockCellY + d.blockSide),
                            ui::col(ui::kAccent), 0, 0, 2.0f);
            dl->PopClipRect();
            if (d.area == DrillAction::AroundStart && (bot.anchorX || bot.anchorY))
                dl->AddCircle(at(bot.anchorX, bot.anchorY), float(d.radius) / 16.0f * px, ui::col(ui::kMuted, 0.6f), 64);
            if (d.hasTarget) dl->AddCircle(at(d.targetX, d.targetY), px * 0.9f, ui::col(ui::kText), 16, 1.5f);
            dl->AddCircleFilled(at(mx, my), px * 0.6f, ui::col(ui::kText));
            ImGui::SameLine();
            ImGui::BeginGroup();
            auto key = [&](const ImVec4& c, const char* t) {
                const ImVec2 q = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(q.x, q.y + 4), ImVec2(q.x + 10, q.y + 14), ui::col(c));
                ImGui::Dummy(ImVec2(14, 16));
                ImGui::SameLine();
                ImGui::TextColored(ui::kMuted, "%s", t);
            };
            key(ui::kOk, "an item the drill reaches");
            key(ui::kAccent, "detected (an item down to the reach)");
            key(ui::kWarn, "an item too deep for the drill");
            key(kCellUnknown, "nothing known");
            key(kCellOther, "nothing known, another ground (twice the life)");
            key(kCellScanned, "a scan saw nothing that deep");
            key(kCellEmpty, "empty (probed / dug)");
            key(kCellClaimed, "another player's dig");
            key(kCellHard, "ground that cannot be dug");
            key(kCellBad, "the server said: not diggable");
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(ui::kMuted, "you: the white dot  -  the next cell: the ring");
            ImGui::TextColored(ui::kMuted, "the block being dug: the blue frame");
            ImGui::TextColored(ui::kMuted, "a block left for failing: a red frame");
            ImGui::TextColored(ui::kMuted, "the area: the circle round the start point");
            ImGui::EndGroup();
        }
    }

    // ── detection: every learned detector skill (Basic Detection 3013, Item Detector 3102) ──
    section_title("Detection skills  -  one scan when a block starts");
    int shown = 0;
    BotCtx ctx{ w, now, 0, 0, false, bot, Action{} };
    if (begin_table("detect", 6)) {
        ImGui::TableSetupColumn("Use", 0, 50);
        ImGui::TableSetupColumn("Skill", 0, 240);
        ImGui::TableSetupColumn("Reach", 0, 70);
        ImGui::TableSetupColumn("Square", 0, 70);
        ImGui::TableSetupColumn("Cool / MP", 0, 90);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        for (const SkillEntry& sk : w.skills) {
            const SkillInfo* si = gameData_.skill(sk.id);
            if (!si || (si->esAction != 1170 && si->esAction != 1147)) continue;
            const SkillLevelInfo* li = gameData_.skill_level(sk.id, sk.level);
            const int reach = DrillAction::detector_reach(w, sk.id);
            ++shown;
            ImGui::TableNextRow();
            ImGui::PushID(sk.id);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            bool on = d.detects(sk.id);
            if (ImGui::Checkbox("##use", &on)) d.set_detect(sk.id, on);
            ImGui::TableNextColumn();
            icon_text(gfx::skill(sk.id), fmt("%s  Lv %u", gameData_.skill_name(sk.id).c_str(), sk.level));
            ImGui::TableNextColumn(); mid(); ImGui::Text("%d", reach);
            ImGui::TableNextColumn(); mid();
            if (li && li->radius > 0) ImGui::Text("%d x %d", 2 * int(li->radius) / 16, 2 * int(li->radius) / 16); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn(); mid();
            if (li) ImGui::Text("%.1f s / %d", li->coolTime, li->mpCon);
            ImGui::TableNextColumn(); mid();
            std::string why;
            if (md && reach < md->minDepth) ImGui::TextColored(ui::kWarn, "shows nothing here (the items lie %d..%d)", md->minDepth, md->maxDepth);
            else if (si->esAction == 1147 && li && li->applyRatio <= 0.9f) ImGui::TextColored(ui::kMuted, "reach = DA x %.0f / 10, at a point within %d", double(li->depthRatio), li->range);
            else if (!skill_ready(ctx, sk.id, &why)) ImGui::TextColored(ui::kMuted, "%s", why.c_str());
            else ImGui::TextColored(ui::kMuted, si->esAction == 1170 ? "cast from the block's centre: its square is the block" : "cast at the block's centre, from within its range");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!shown) ImGui::TextColored(ui::kMuted, "no detection skill learned (Basic Detection 3013, Item Detector 3102)");
    if (w.detectAt)
        ImGui::TextColored(ui::kMuted, "last scan %s: %s", ago(now, w.detectAt).c_str(),
                           w.detectFound < 0 ? "failed (its ApplyRatio roll)" : fmt("%d item%s", w.detectFound, w.detectFound == 1 ? "" : "s").c_str());

    // ── sense: DA ──
    section_title("Sense (DA)  -  the buffs are kept up by the Buffs action, between digs");
    const int da = w.stat_base(ST_DA);
    if (da > 0)
        ImGui::TextColored(ui::kMuted, "DA %d without buffs: a dig is sensed %d %% of the time (an empty sensed cell other than the "
                           "last one gives the probe's EXP)", da, std::min(100, da + 10));
    int buffs = 0;
    for (const SkillEntry& sk : w.skills) {
        const SkillInfo* si = gameData_.skill(sk.id);
        if (!si || !da_buff(*si) || !usable_skill(w, sk)) continue;
        ++buffs;
        ImGui::PushID(sk.id + 100000);
        bool k = bot.buffs.kept(sk.id);
        if (ImGui::Checkbox("##keep", &k)) {
            bot.buffs.set_kept(sk.id, k);
            if (k) bot.buffs.enabled = true;            // as on the Buffs page: checking one turns the action on
        }
        ImGui::SameLine();
        const Effect* e = w.effect_of(sk.id, now);
        icon_text(gfx::buff(sk.id), fmt("%s  Lv %u%s", gameData_.skill_name(sk.id).c_str(), sk.level,
                                       si->esAction == 1391 ? "  (Crazy Drill gauge)" : ""));
        ImGui::SameLine(380);
        mid();
        if (e && e->endAt) ImGui::TextColored(ui::kOk, "up, %s left", secs_text(double(e->endAt > now ? e->endAt - now : 0) / 1e7).c_str());
        else if (e) ImGui::TextColored(ui::kOk, "up");
        else ImGui::TextColored(ui::kMuted, k ? "kept - cast between digs" : "off");
        ImGui::PopID();
    }
    if (!buffs) ImGui::TextColored(ui::kMuted, "no DA buff learned (Sixth Sense 3006, Concentration 4704, Aura of Anger 4901)");

    // ── the finds ──
    // ── the finds: what was dug up, the checked ones sold by the Supply trip ──
    section_title("Finds  -  check the ones to sell");
    TownAction& t = bot.town;
    bool autoSell = t.enabled && t.sell && t.weightPct > 0;
    if (ui::toggle("##autosell", &autoSell)) {
        if (autoSell) {                         // the Town action's trip, with its sell errand (Auto-Sell), by weight
            t.enabled = t.sell = true;
            if (!t.weightPct) t.weightPct = 90;
        } else {
            t.weightPct = 0;
        }
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sell the checked items when the bag weighs");
    ImGui::SameLine();
    ImGui::BeginDisabled(!autoSell);
    int pct = t.weightPct ? t.weightPct : 90;
    slider("##sellpct", &pct, 50, 100, "%d %% or more", 160);
    if (autoSell) t.weightPct = pct;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ui::help("A town trip (the Auto-Supply tab's shop): it sells the checked items, runs the other errands set there, and comes "
             "back here to dig on. It starts between two digs, never in the middle of one. The max weight is your MaxWT stat "
             "as the server counts it (a rare item weighs once).");
    {
        const int maxWt = w.stat_base(ST_MAXWT);
        const int64_t wt = w.bag_weight();
        if (maxWt > 0)
            ui::bar(float(std::min<int64_t>(wt, maxWt)) / float(maxWt), wt * 100 >= int64_t(t.weightPct ? t.weightPct : 90) * maxWt ? ui::kWarn : ui::kOk,
                    320, fmt("weight %lld / %d (%lld %%)", (long long)wt, maxWt, (long long)(wt * 100 / maxWt)).c_str());
        const Atlas& at = gameData_.atlas();
        const int si = t.shopKey.empty() ? -1 : at.find_npc(t.shopKey);
        ImGui::SameLine();
        if (si < 0) {
            ImGui::TextColored(autoSell ? ui::kWarn : ui::kMuted, "no shop chosen:");
            ImGui::SameLine();
            if (ui::link("pick one on the Auto-Supply tab")) botTab_ = BT_Supply;
        } else {
            ImGui::TextColored(ui::kMuted, "sold at %s (%s)", at.npc(si)->name.c_str(), at.map_label(at.npc(si)->zone).c_str());
            ImGui::SameLine();
            if (ui::link("Auto-Sell tab")) botTab_ = BT_Sell;
        }
    }
    if (st.foundCount.empty()) {
        ImGui::TextColored(ui::kMuted, "nothing dug up yet");
    } else if (begin_table("finds", 5)) {
        ImGui::TableSetupColumn("Sell", 0, 44);
        ImGui::TableSetupColumn("Item", 0, 300);
        ImGui::TableSetupColumn("Found", 0, 60);
        ImGui::TableSetupColumn("In the bag", 0, 80);
        ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        std::vector<std::pair<int, uint32_t>> rows;             // the most found first
        for (const auto& kv : st.foundCount) rows.emplace_back(kv.second, kv.first);
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        IdFilter& f = t.sellList;
        for (const auto& row : rows) {
            const uint32_t id = row.second;
            const ItemInfo* ii = gameData_.item(id);
            const bool never = ii && ((ii->options & 0x200) || ii->type == 30);   // not sellable / galder
            ImGui::TableNextRow();
            ImGui::PushID(int(id));
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            bool sells = !never && (f.mode == IdFilter::OnlyWhite ? f.white.count(id) != 0 : f.black.count(id) == 0);
            ImGui::BeginDisabled(never);
            if (ImGui::Checkbox("##sell", &sells)) {
                if (f.mode == IdFilter::OnlyWhite) { if (sells) f.white.insert(id); else f.white.erase(id); }
                else { if (sells) f.black.erase(id); else f.black.insert(id); }
            }
            ImGui::EndDisabled();
            if (never && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("the shops do not buy it");
            ImGui::TableNextColumn(); icon_text(gfx::item(id), gameData_.item_name(id));
            ImGui::TableNextColumn(); mid(); ImGui::Text("%d", row.first);
            ImGui::TableNextColumn(); mid(); ImGui::Text("%d", w.count_item(id));
            ImGui::TableNextColumn(); mid();
            for (auto it = st.found.rbegin(); it != st.found.rend(); ++it)
                if (it->second == id) {
                    ImGui::TextColored(ui::kMuted, "%s", ago(now, it->first).c_str());
                    break;
                }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} // namespace nj
