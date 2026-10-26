// The Bot view's Item Hunt page: the numbers, what it is doing, the options, the items to add (search) and the
// list (how many of each, where each comes from: Auto or a chosen source), then where the selected item comes
// from. The rules: bot/act_hunt.cpp; the drops / buried items: item_sources.cpp.
#include "app/neo_app.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/item_sources.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <map>

namespace nj {

namespace {

using namespace bui;

// an item's sources, worked out again every 2 s (the page draws them every frame)
const std::vector<HuntSource>& sources_of(const WorldModel& w, uint32_t item)
{
    static std::map<uint32_t, std::pair<double, std::vector<HuntSource>>> cache;
    const double t = ImGui::GetTime();
    auto& e = cache[item];
    if (e.second.empty() || t - e.first > 2.0) {
        e.first = t;
        e.second = ItemHuntAction::sources(w, item);
    }
    return e.second;
}

std::string per_text(const HuntSource& s)
{
    return s.kind == HuntSource::Kill ? fmt("%.3f a kill", s.perUnit) : fmt("%.1f %% of a find", s.perUnit * 100.0);
}

std::string source_text(const HuntSource& s)
{
    return fmt("%s  -  %s, ~%.1f / h", s.label.c_str(), per_text(s).c_str(), s.perHour);
}

std::string hours_text(double h)
{
    return h < 1.0 ? fmt("~%.0f min", std::max(1.0, h * 60.0)) : fmt("~%.1f h", h);
}

// the smart plan for the list as it is now (the bot not running: a preview), worked out again every 2 s
const HuntPlan& preview_plan(const WorldModel& w, const ItemHuntAction& h)
{
    static HuntPlan plan;
    static double at = -10.0;
    const double t = ImGui::GetTime();
    if (t - at > 2.0) { at = t; plan = h.make_plan(w); }
    return plan;
}

} // namespace

void NeoApp::bot_hunt(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    ItemHuntAction& h = bot.hunt;
    const uint64_t now = now_filetime();
    ui::page_header("Item Hunt", bot.running && h.enabled ? h.status.c_str()
                                                          : "kill where the items drop, dig where they are buried, until the bag has them",
                    &h.enabled);
    ImGui::Dummy(ImVec2(0, 2));
    const GameData* gd = w.data();
    if (!gd) { note("No game data (the db folder)."); return; }

    // ── the numbers ──
    int done = 0, got = 0;
    for (size_t i = 0; i < h.list.size(); ++i) done += h.have(w, i) >= h.list[i].want;
    for (const auto& kv : h.gained) got += kv.second;
    const double hours = bot.running && h.startedAt && now > h.startedAt ? double(now - h.startedAt) / 36e9 : 0.0;
    ui::stat("ITEMS DONE", fmt("%d / %zu", done, h.list.size()).c_str(), 120);
    ImGui::SameLine();
    ui::stat("GOT (RUN)", std::to_string(got).c_str(), 110);
    ImGui::SameLine();
    ui::stat("PER HOUR", hours > 0.01 ? fmt("%.1f", got / hours).c_str() : "-", 100);
    ImGui::SameLine();
    const int cur = h.current();
    const bool live = h.enabled && bot.running;
    ui::stat("NOW", live && h.smart && h.source().zone ? gd->map_name(h.source().zone).c_str()
                    : cur >= 0 && size_t(cur) < h.list.size() ? gd->item_name(h.list[size_t(cur)].item).c_str() : "-", 300);
    if (h.enabled && !h.plan.empty()) ImGui::TextColored(ui::kMuted, "next: %s", h.plan.c_str());
    if (h.enabled && cur >= 0 && h.source().zone) {
        const HuntSource& src = h.source();
        ImGui::TextColored(ui::kMuted, "source: %s (%s, ~%.1f / h)%s", src.label.c_str(), per_text(src).c_str(), src.perHour,
                           src.why.empty() ? "" : fmt("  -  %s", src.why.c_str()).c_str());
    }

    // ── options ──
    section_title("Options");
    toggle_row("Smart: plan the whole list - the map that gives the most of it at once, every species there that drops it",
               &h.smart);
    toggle_row("Stop the bot when every item of the list is in the bag", &h.stopWhenDone);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "Next source");
    ImGui::SameLine(110);
    slider("##switch", &h.switchMin, 0, 120, h.switchMin ? "after %d min there without one" : "never (stay on the chosen one)", 300);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "Monsters");
    ImGui::SameLine(110);
    slider("##maxover", &h.maxOver, 0, 150, "up to %d levels over yours", 300);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The smart plan prefers species at most this many levels over yours (a map no way reaches is left out "
                          "too). An item that only stronger monsters drop is still hunted, from the weakest of them (the log says "
                          "so). The Key Quest / Story Quest item gathering uses this limit too.");

    // ── add ──
    section_title("Add items");
    static std::string query, queryFor;
    static std::vector<uint32_t> results;
    static int addCount = 10;
    ImGui::SetNextItemWidth(320);
    ImGui::InputTextWithHint("##hq", "search the items by name or id...", &query);
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "how many");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("##hn", &addCount, 1, 10);
    addCount = std::clamp(addCount, 1, 30000);
    if (query != queryFor) { queryFor = query; results = gameData_.search_items(query, 60); }
    ImGui::BeginChild("##hres", ImVec2(0, 150), ImGuiChildFlags_Borders);
    if (query.empty()) ImGui::TextColored(ui::kMuted, "type a name or an id; the ones that drop or are buried somewhere show where");
    else if (results.empty()) ImGui::TextColored(ui::kMuted, "nothing found");
    const ItemSources& is = gd->item_sources();
    for (uint32_t id : results) {
        ImGui::PushID(int(id));
        bool in = false;
        for (const HuntItem& x : h.list) in = in || x.item == id;
        ImGui::BeginDisabled(in);
        if (ImGui::SmallButton(in ? "in the list" : "+ Add")) {
            HuntItem it;
            it.item = id;
            it.want = addCount;
            h.list.push_back(it);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        icon_text(gfx::item(id), fmt("%s  #%u", gd->item_name(id).c_str(), id));
        ImGui::SameLine(0, 14);
        const size_t nm = is.mobs(id).size(), nd = is.digs(id).size();
        if (!nm && !nd) ImGui::TextColored(ui::kMuted, "no monster drops it, no map buries it");
        else ImGui::TextColored(ui::kMuted, "%s%s%s", nm ? fmt("%zu monster(s)", nm).c_str() : "", nm && nd ? ", " : "",
                                nd ? fmt("buried in %zu map(s)", nd).c_str() : "");
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ── the list ──
    section_title("The list");
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "  in this order; each from Auto (the best estimate) or the source you pick");
    static uint32_t selected = 0;
    int move = -1, dir = 0, del = -1;
    if (h.list.empty()) {
        ImGui::TextColored(ui::kMuted, "empty: add the items above, then Start");
    } else if (begin_table("huntlist", 7)) {
        ImGui::TableSetupColumn("#", 0, 22);
        ImGui::TableSetupColumn("Item", 0, 205);
        ImGui::TableSetupColumn("Have", 0, 50);
        ImGui::TableSetupColumn("Want", 0, 115);
        ImGui::TableSetupColumn("Source", 0, 345);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", 0, 130);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < h.list.size(); ++i) {
            HuntItem& it = h.list[i];
            const std::vector<HuntSource>& srcs = sources_of(w, it.item);
            const int have = h.have(w, i);
            const float midY = (kIcon - ImGui::GetTextLineHeight()) * 0.5f;
            const float midF = (kIcon - ImGui::GetFrameHeight()) * 0.5f;
            ImGui::TableNextRow();
            ImGui::PushID(int(i));
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY); ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            const float rowY = ImGui::GetCursorPosY();
            if (ImGui::Selectable("##sel", selected == it.item, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, kIcon))) selected = it.item;
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosY(rowY);
            icon_text(gfx::item(it.item), gd->item_name(it.item));
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            ImGui::TextColored(have >= it.want ? ui::kOk : ui::kText, "%d", have);
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midF);
            ImGui::SetNextItemWidth(105);
            ImGui::InputInt("##want", &it.want, 1, 10);
            it.want = std::clamp(it.want, 1, 30000);
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midF);
            const HuntSource* chosen = nullptr;
            for (const HuntSource& x : srcs) if (it.srcKind >= 0 && x.kind == it.srcKind && x.zone == it.srcZone) chosen = &x;
            const HuntSource* best = nullptr;
            for (const HuntSource& x : srcs) if (x.usable) { best = &x; break; }
            const std::string preview = chosen ? source_text(*chosen)
                                      : it.srcKind >= 0 ? std::string("(the chosen source is gone: Auto)")
                                      : best ? "Auto: " + best->label : std::string(srcs.empty() ? "no source" : "Auto: none usable");
            ImGui::SetNextItemWidth(335);
            if (ImGui::BeginCombo("##src", preview.c_str())) {
                if (ImGui::Selectable(best ? ("Auto: the best one (now " + source_text(*best) + ")").c_str() : "Auto: the best one",
                                      it.srcKind < 0)) { it.srcKind = -1; it.srcZone = 0; }
                for (const HuntSource& x : srcs) {
                    ImGui::PushID(int(x.kind * 100000 + x.zone));
                    ImGui::BeginDisabled(!x.usable);
                    const std::string t = source_text(x) + (x.why.empty() ? "" : "  (" + x.why + ")");
                    if (ImGui::Selectable(t.c_str(), chosen == &x)) { it.srcKind = x.kind; it.srcZone = x.zone; }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (have >= it.want) ImGui::TextColored(ui::kOk, "done");
            else if (h.smart && live && best) ImGui::TextColored(h.serves(it.item) ? ui::kAccent : ui::kMuted, h.serves(it.item) ? "here" : "later");
            else if (!best) {
                const HuntSource* f = srcs.empty() ? nullptr : &srcs.front();
                ImGui::TextColored(ui::kWarn, "%s", !f ? "no source" : f->kind != HuntSource::Dig ? "not usable"
                                                    : f->why.rfind("no drill", 0) == 0 ? "needs a drill" : "drill too short");
                if (f && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f->why.c_str());
            }
            else if (live && cur == int(i)) ImGui::TextColored(ui::kAccent, "hunting");
            else ImGui::TextColored(ui::kMuted, "queued");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midF);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move = int(i); dir = -1; }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(i + 1 == h.list.size());
            if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) { move = int(i); dir = 1; }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton("remove")) del = int(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move >= 0) std::swap(h.list[size_t(move)], h.list[size_t(move + dir)]);
    if (del >= 0) h.list.erase(h.list.begin() + del);
    note("Kill: the Attack takes only the species that drop it, only in that map (whatever its lists say; the Roam "
         "patrols their spawns). Dig: the Drill works in that map with its own settings (a drill in the bag). The list's "
         "items are always picked up and never sold or stored; they are counted in the bag. The estimate per hour only "
         "ranks the sources: per kill / per find come from the server's own tables and rolls, the pace is a guess "
         "(a fight 12-40 s by the monster's level over yours, the walk 2-14 s by how many targets the map keeps; a probe "
         "~3 s, a find ~8 s).");

    // ── the smart plan: where now, for what, killing whom; the route ahead; the other maps ──
    if (h.smart && !h.list.empty()) {
        section_title(live ? "The plan" : "The plan (a preview: what Start would do now)");
        const HuntPlan& pl = live && !h.smart_plan().need.empty() ? h.smart_plan() : preview_plan(w, h);
        int at = pl.cands.empty() ? -1 : 0;                     // the map hunted (running: the bot's, maybe not the best)
        if (live && h.source().zone)
            for (size_t i = 0; i < pl.cands.size(); ++i)
                if (pl.cands[i].src.kind == h.source().kind && pl.cands[i].src.zone == h.source().zone) at = int(i);
        if (pl.need.empty()) {
            ImGui::TextColored(ui::kOk, "nothing missing");
        } else if (at < 0) {
            ImGui::TextColored(ui::kWarn, "no map gives the missing items to this character (level, drill in the bag)");
        } else {
            const HuntPlan::Cand& c = pl.cands[size_t(at)];
            ImGui::Text("%s", c.src.label.c_str());
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ui::kMuted, "%zu item(s) here in %s; the whole list from here %s", c.covered.size(),
                               hours_text(c.farmHours).c_str(), hours_text(c.totalHours).c_str());
            if (begin_table("plannow", 4)) {
                ImGui::TableSetupColumn("Gives", 0, 260);
                ImGui::TableSetupColumn("Missing", 0, 70);
                ImGui::TableSetupColumn("There", 0, 110);
                ImGui::TableSetupColumn("Its best anywhere", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const auto& q : c.rate) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); icon_text(gfx::item(q.first), gd->item_name(q.first));
                    ImGui::TableNextColumn(); mid();
                    auto n = pl.need.find(q.first);
                    ImGui::Text("%d", n == pl.need.end() ? 0 : n->second);
                    ImGui::TableNextColumn(); mid(); ImGui::Text("~%.1f / h", q.second);
                    ImGui::TableNextColumn(); mid();
                    auto b = pl.best.find(q.first);
                    const double bv = b == pl.best.end() ? 0.0 : b->second;
                    ImGui::TextColored(bv > q.second * 1.001 ? ui::kMuted : ui::kOk, "%s",
                                       bv > q.second * 1.001 ? fmt("~%.1f / h elsewhere", bv).c_str() : "here");
                }
                ImGui::EndTable();
            }
            if (c.src.kind == HuntSource::Kill) {
                std::string kills;
                for (uint16_t m : c.src.mobs) {
                    const MobInfo* mi = gd->mob(m);
                    kills += fmt("%s%s Lv %d", kills.empty() ? "" : ", ", gd->mob_name(m).c_str(), mi ? mi->level : 0);
                }
                ImGui::TextColored(ui::kMuted, "kills: %s (%d of them in the map)", kills.c_str(), c.src.count);
                if (!c.skipped.empty()) {
                    std::string sk;
                    for (uint16_t m : c.skipped) {
                        const MobInfo* mi = gd->mob(m);
                        sk += fmt("%s%s Lv %d", sk.empty() ? "" : ", ", gd->mob_name(m).c_str(), mi ? mi->level : 0);
                    }
                    ImGui::TextColored(ui::kMuted, "left alone: %s (more than 10 levels over yours, or their items are quicker "
                                       "elsewhere / from the others)", sk.c_str());
                }
            }
            if (!pl.route.empty() && begin_table("planroute", 4)) {     // the route ahead
                ImGui::TableSetupColumn("#", 0, 22);
                ImGui::TableSetupColumn("Where", 0, 420);
                ImGui::TableSetupColumn("Done there", 0, 330);
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                double sum = 0.0;
                for (size_t k = 0; k < pl.route.size(); ++k) {
                    const HuntPlan::Step& st = pl.route[k];
                    sum += st.hours;
                    std::string dn;
                    for (uint32_t it : st.done) dn += (dn.empty() ? "" : ", ") + gd->item_name(it);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%zu", k + 1);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(pl.cands[size_t(st.cand)].src.label.c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(ui::kMuted, "%s", dn.empty() ? "-" : dn.c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(ui::kMuted, "%s (total %s)", hours_text(st.hours).c_str(),
                                                                 hours_text(sum).c_str());
                }
                ImGui::EndTable();
            }
            std::string missing;                                // missing items no map gives this character
            for (const auto& b : pl.best) if (b.second <= 0.0) missing += (missing.empty() ? "" : ", ") + gd->item_name(b.first);
            if (!missing.empty()) ImGui::TextColored(ui::kWarn, "no usable source now: %s", missing.c_str());
            if (pl.cands.size() > 1) {                          // the other maps
                std::string others;
                for (size_t i = 0, n = 0; i < pl.cands.size() && n < 4; ++i) {
                    if (int(i) == at) continue;
                    others += fmt("%s%s (the list %s)", others.empty() ? "" : ";  ", pl.cands[i].src.label.c_str(),
                                  hours_text(pl.cands[i].totalHours).c_str());
                    ++n;
                }
                ImGui::TextColored(ui::kMuted, "next best: %s", others.c_str());
            }
        }
        note("Each map is weighed by the time to finish the whole list starting there: the items worth getting there (an "
             "item quicker at its best place, the way included - about 6 min a map -, is left for later), the farm there "
             "killing every species that drops one of them (an item done takes its species out), then the rest. The "
             "quickest is hunted; the bot moves on when another is 15 % quicker. Per kill / per find come from the "
             "server's tables and rolls; the pace is a guess (a fight 12-40 s by the level, the walk 2-14 s by how many "
             "targets the map keeps), so the times are an estimate.");
    }

    // ── where the selected item comes from ──
    bool selIn = false;
    for (const HuntItem& x : h.list) selIn = selIn || x.item == selected;
    if (!selIn && !h.list.empty()) selected = h.list.front().item;
    if (!h.list.empty()) {
        section_title(("Where " + gd->item_name(selected) + " comes from").c_str());
        const std::vector<HuntSource>& srcs = sources_of(w, selected);
        if (srcs.empty()) {
            ImGui::TextColored(ui::kWarn, "no monster drops it and no map buries it");
        } else if (begin_table("huntsrc", 5)) {
            ImGui::TableSetupColumn("Source", 0, 360);
            ImGui::TableSetupColumn("Monsters", 0, 330);
            ImGui::TableSetupColumn("Each", 0, 120);
            ImGui::TableSetupColumn("Per hour", 0, 80);
            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const HuntSource& x : srcs) {
                ImGui::TableNextRow();
                const ImVec4 col = x.usable ? ui::kText : ui::kMuted;
                ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", x.label.c_str());
                ImGui::TableNextColumn();
                if (x.kind == HuntSource::Kill) {
                    std::string m;
                    for (size_t k = 0; k < x.mobs.size() && k < 3; ++k) {
                        const MobInfo* mi = gd->mob(x.mobs[k]);
                        m += fmt("%s%s Lv %d", k ? ", " : "", gd->mob_name(x.mobs[k]).c_str(), mi ? mi->level : 0);
                    }
                    if (x.mobs.size() > 3) m += fmt(" +%zu", x.mobs.size() - 3);
                    ImGui::TextColored(col, "%s  (%d there)", m.c_str(), x.count);
                } else if (const MapDigInfo* md = gd->map_dig(x.zone)) {
                    ImGui::TextColored(col, "buried in %.0f %% of the cells, %d..%d deep", md->itemPct, md->minDepth, md->maxDepth);
                }
                ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", per_text(x).c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(col, "~%.1f", x.perHour);
                ImGui::TableNextColumn(); ImGui::TextColored(x.usable ? ui::kMuted : ui::kWarn, "%s", x.why.c_str());
            }
            ImGui::EndTable();
        }
    }
}

} // namespace nj
