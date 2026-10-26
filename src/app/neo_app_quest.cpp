// The Bot view's Monster Quest page: how the run goes, the quest (the monster quests an NPC of the atlas gives
// through its dialog), its NPC, the map to hunt in (where the grade's monster lives) and the options. The
// rules: bot/act_quest.cpp.
#include "app/neo_app.h"
#include "app/atlas.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace nj {

namespace {

using namespace bui;

// The quests the bot can run: an NPC of the atlas has it in a QuestId slot and its dialog the take.
struct QuestRow { int id = 0; std::vector<int> npcs; };

const std::vector<QuestRow>& quest_rows(const GameData& gd)
{
    static std::vector<QuestRow> rows;
    static const GameData* builtFor = nullptr;
    if (builtFor == &gd) return rows;
    builtFor = &gd;
    rows.clear();
    const Atlas& at = gd.atlas();
    for (const MonQuestInfo& q : gd.monster_quests()) {
        QuestRow r;
        r.id = q.id;
        for (int i : at.quest_givers(q.id)) {
            const AtlasNpc* n = at.npc(i);
            const int s = n->quest_slot(q.id);
            if (n->call(act::npcfn::monq_accept(s)) || (s == 0 && n->call(act::npcfn::MonQuest_Once_Accept))) r.npcs.push_back(i);
        }
        if (!r.npcs.empty()) rows.push_back(std::move(r));
    }
    return rows;
}

bool has_text(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    auto low = [](std::string s) { for (char& ch : s) ch = char(std::tolower((unsigned char)ch)); return s; };
    return low(hay).find(low(needle)) != std::string::npos;
}

std::string clock_text(uint32_t ms) { return fmt("%u:%02u", ms / 60000, (ms / 1000) % 60); }

} // namespace

// The Monster Quest's activity panel (the Key Quest's look): what it does now, the kills, the last request.
void NeoApp::monq_activity(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    QuestAction& q = bot.quest;
    const uint64_t now = now_filetime();
    const double since = bot.last_request_at() && now > bot.last_request_at() ? double(now - bot.last_request_at()) / 1e7 : -1.0;
    const MonQuestState& m = w.monq;
    Activity act;
    if (!q.enabled) {
        act.state = "OFF";
        act.line = "switch the Monster Quest on (top right of this page)";
    } else if (!bot.running) {
        act.tone = 3;
        act.state = "NOT RUNNING";
        act.line = bot.stopWhy.empty() ? std::string("the bot is not started: press Start") : "stopped: " + bot.stopWhy;
    } else if (since >= 15.0 && !w.changingZone && !q.hunting(w)) {
        act.tone = 3;
        act.state = fmt("STANDING STILL %.0f s", since);
        act.line = bot.doing.empty() ? q.status : bot.doing;
    } else if (w.changingZone) {
        act.tone = 4;
        act.state = "CHANGING MAP";
        act.line = bot.doing;
    } else if (m.has && m.success) {
        act.tone = 1;
        act.state = "HANDING IN";
        act.line = q.status;
    } else if (q.hunting(w)) {
        act.tone = 1;
        act.state = "HUNTING";
        act.line = q.status;
    } else if (m.has) {
        act.tone = 1;
        act.state = "TO THE HUNT MAP";
        act.line = q.status;
    } else {
        act.tone = 1;
        act.state = "TAKING IT";
        act.line = q.status;
    }
    if (m.has) {
        act.step = fmt("%s, run %u: kill %s", m.name.empty() ? fmt("quest %u", m.qid).c_str() : m.name.c_str(), m.run,
                       w.data() ? w.data()->mob_name(m.monster).c_str() : "?");
        act.frac = m.target > 0 ? std::min(1.0f, float(m.kills) / float(m.target)) : 0.0f;
        act.progress = fmt("%d / %d killed", m.kills, m.target);
    } else if (const MonQuestInfo* qi = q.info(w)) {
        act.step = qi->name + " (to take)";
    }
    act.showLog = bot.running && q.enabled && act.tone >= 2;
    activity_box(s, q, "Monster Quest", act);
}

void NeoApp::bot_quest(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    QuestAction& q = bot.quest;
    const uint64_t now = now_filetime();
    ui::page_header("Monster Quest", bot.running && q.enabled ? q.status.c_str() : "a repeatable monster quest, round and round",
                    &q.enabled);
    ImGui::Dummy(ImVec2(0, 2));
    monq_activity(s);
    const GameData* gd = w.data();
    if (!gd) { note("No game data (the db folder)."); return; }
    const Atlas& at = gd->atlas();

    // ── the numbers ──
    const double hours = bot.running && now > bot.startedAt ? double(now - bot.startedAt) / 36e9 : 0.0;
    ui::stat("HANDED IN (RUN)", std::to_string(q.runs).c_str(), 140);
    ImGui::SameLine();
    ui::stat("RUNS / H", hours > 0.01 ? fmt("%.1f", q.runs / hours).c_str() : "-", 100);
    ImGui::SameLine();
    ui::stat("EXP (RUN)", thousands(q.expGained).c_str(), 140);
    ImGui::SameLine();
    ui::stat("GALDER (RUN)", thousands(q.gelderGained).c_str(), 130);
    ImGui::SameLine();
    ui::stat("HANDED IN (SESSION)", std::to_string(w.monq.completed).c_str(), 160);

    // ── now ──
    section_title("Now");
    const MonQuestState& m = w.monq;
    if (m.has) {
        ImGui::Text("%s", m.name.empty() ? fmt("quest %u", m.qid).c_str() : m.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, "#%u, run %u, kill %s", m.qid, m.run, gd->mob_name(m.monster).c_str());
        const float frac = m.target > 0 ? std::min(1.0f, float(m.kills) / float(m.target)) : 0.0f;
        ui::bar(frac, m.success ? ui::kOk : ImVec4(0.75f, 0.22f, 0.25f, 1), 260,
                fmt("%d / %d killed", m.kills, m.target).c_str());
        ImGui::SameLine(0, 14);
        if (m.success)
            ImGui::TextColored(ui::kOk, "all killed: to be handed in%s", m.portal && m.running(now) ? " (the portal back is open)" : "");
        else if (m.running(now))
            ImGui::TextColored(ui::kMuted, "%s left of %s", clock_text(m.remainMs).c_str(), clock_text(m.totalMs).c_str());
        else
            ImGui::TextColored(ui::kWarn, "its timer is not running");
    } else {
        ImGui::TextColored(ui::kMuted, "no monster quest on");
    }
    if (!q.plan.empty() && q.enabled) ImGui::TextColored(ui::kMuted, "next: %s", q.plan.c_str());
    if (m.doneAt) ImGui::TextColored(ui::kMuted, "last hand-in: %lld EXP, %lld TM EXP, %lld galder", (long long)m.lastExp,
                                     (long long)m.lastTm, (long long)m.lastGelder);

    // ── the quest ──
    section_title("Quest");
    static char search[64] = "";
    static bool forMe = true;
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##qsearch", "search: quest, NPC, monster, id", search, sizeof(search));
    ImGui::SameLine(0, 14);
    toggle_row("only the ones for my level", &forMe);
    const int lv = w.self.level;
    const std::vector<QuestRow>& rows = quest_rows(*gd);
    ImGui::BeginChild("qlist", ImVec2(0, 230), ImGuiChildFlags_Borders);
    if (begin_table("quests", 5)) {
        ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("NPC", 0, 220);
        ImGui::TableSetupColumn("Levels", 0, 70);
        ImGui::TableSetupColumn("For you", 0, 230);
        ImGui::TableSetupColumn("EXP", 0, 90);
        ImGui::TableHeadersRow();
        for (const QuestRow& r : rows) {
            const MonQuestInfo* qi = gd->monster_quest(r.id);
            if (!qi) continue;
            const int g = qi->grade_for(lv);
            const bool fits = g >= 0 && qi->level_ok(lv);
            if (forMe && !fits && r.id != q.questId) continue;
            const AtlasNpc* n0 = at.npc(r.npcs.front());
            const std::string mobName = g >= 0 ? gd->mob_name(uint32_t(qi->grades[size_t(g)].monster)) : std::string();
            if (search[0] && !has_text(qi->name + " " + n0->name + " " + mobName + " " + std::to_string(r.id), search)) continue;
            ImGui::TableNextRow();
            ImGui::PushID(r.id);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(fmt("%s##q", qi->name.c_str()).c_str(), q.questId == r.id, ImGuiSelectableFlags_SpanAllColumns)) {
                if (q.questId != r.id) { q.npcKey.clear(); q.huntZone = 0; }
                q.questId = r.id;
            }
            ImGui::SameLine();
            ImGui::TextColored(ui::kMuted, "#%d", r.id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmt("%s, %s%s", n0->name.c_str(), gd->map_name(n0->zone).c_str(),
                                       r.npcs.size() > 1 ? fmt(" (+%zu)", r.npcs.size() - 1).c_str() : "").c_str());
            ImGui::TableNextColumn();
            if (qi->minLevel <= 0 && qi->maxLevel <= 0) ImGui::TextColored(ui::kMuted, "any");
            else ImGui::TextUnformatted(fmt("%d-%d", qi->minLevel, qi->maxLevel).c_str());
            ImGui::TableNextColumn();
            if (fits) {
                const MonQuestGrade& gr = qi->grades[size_t(g)];
                ImGui::TextUnformatted(fmt("%d x %s in %d min", gr.cnt, mobName.c_str(), gr.timeMin).c_str());
            } else {
                ImGui::TextColored(ui::kMuted, "not at level %d", lv);
            }
            ImGui::TableNextColumn();
            if (fits) ImGui::TextUnformatted(thousands(qi->grades[size_t(g)].exp).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (rows.empty()) note("No monster quest of the db has an NPC of the atlas with its take in the dialog.");

    const MonQuestInfo* qi = q.info(w);
    if (!qi) {
        note("Choose a quest above. The bot takes it from its NPC, kills its monster in the map you choose below, "
             "comes back through the success portal (or walks), hands it in and takes it again.");
        return;
    }

    // its grades: what to kill at which level
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Text("%s", qi->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "#%d  -  %s  -  %s%s", qi->id, qi->client.c_str(), qi->pos.c_str(),
                       qi->maxCount >= 99 ? ", no limit" : fmt(", %d times at most", qi->maxCount).c_str());
    const int myGrade = qi->grade_for(lv);
    if (begin_table("grades", 5)) {
        ImGui::TableSetupColumn("Levels", 0, 80);
        ImGui::TableSetupColumn("Kill", 0, 240);
        ImGui::TableSetupColumn("Time", 0, 70);
        ImGui::TableSetupColumn("EXP", 0, 100);
        ImGui::TableSetupColumn("TM EXP", 0, 80);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < qi->grades.size(); ++i) {
            const MonQuestGrade& gr = qi->grades[i];
            const ImVec4 col = int(i) == myGrade ? ui::kText : ui::kMuted;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%d-%d%s", gr.minLv, gr.maxLv, int(i) == myGrade ? "  <" : "");
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%d x %s", gr.cnt, gd->mob_name(uint32_t(gr.monster)).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%d min", gr.timeMin);
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", thousands(gr.exp).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(col, "%s", thousands(gr.tmxp).c_str());
        }
        ImGui::EndTable();
    }
    if (myGrade < 0 || !qi->level_ok(lv)) ImGui::TextColored(ui::kWarn, "Level %d: no grade of this quest fits you.", lv);

    // its NPC
    section_title("NPC");
    int slot = -1;
    const AtlasNpc* giver = q.giver(w, &slot);
    for (int i : at.quest_givers(qi->id)) {
        const AtlasNpc* n = at.npc(i);
        const int sl = n->quest_slot(qi->id);
        const NpcScriptCall* take = n->call(act::npcfn::monq_accept(sl));
        if (!take && sl == 0) take = n->call(act::npcfn::MonQuest_Once_Accept);
        const NpcScriptCall* give = n->call(act::npcfn::monq_complete(sl));
        if (!give && sl == 0) give = n->call(act::npcfn::MonQuest_NH_Complete);
        ImGui::PushID(i);
        bool picked = giver == n;
        ImGui::BeginDisabled(!take);
        if (ImGui::RadioButton(fmt("%s, %s", n->name.c_str(), at.map_label(n->zone).c_str()).c_str(), picked)) q.npcKey = n->key();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, "%s", !take ? "(its dialog has no take)" : !give ? "(its dialog has no hand-in)"
                                             : fmt("(slot %d)", sl).c_str());
        ImGui::PopID();
    }
    if (giver && qi->returnMap > 0)
        ImGui::TextColored(ui::kMuted, "The success portal brings you back to %s.", at.map_label(qi->returnMap).c_str());

    // the map to hunt in
    section_title("Where to hunt");
    const uint16_t mob = q.mob(w);
    const std::vector<MobSpawn> spawns = mob ? gd->spawns_of(mob) : std::vector<MobSpawn>();
    const int hz = q.hunt_zone(w);
    ImGui::TextColored(ui::kMuted, "%s lives in %zu map(s); the bot fights only in the chosen one.", mob ? gd->mob_name(mob).c_str() : "-",
                       spawns.size());
    if (begin_table("huntmaps", 2)) {
        ImGui::TableSetupColumn("Map", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("About", 0, 120);
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(fmt("Auto: where most of it lives%s", q.huntZone == 0 && hz ? fmt(" (%s)", gd->map_name(hz).c_str()).c_str() : "").c_str(),
                              q.huntZone == 0, ImGuiSelectableFlags_SpanAllColumns))
            q.huntZone = 0;
        ImGui::TableNextColumn();
        for (const MobSpawn& sp : spawns) {
            ImGui::TableNextRow();
            ImGui::PushID(sp.zone);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(at.map_label(sp.zone).c_str(), q.huntZone == sp.zone, ImGuiSelectableFlags_SpanAllColumns))
                q.huntZone = sp.zone;
            ImGui::TableNextColumn();
            ImGui::TextColored(ui::kMuted, "%d of it", sp.count);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (q.huntZone > 0 && hz != q.huntZone)
        ImGui::TextColored(ui::kWarn, "%s does not live in %s: the bot hunts in %s.", gd->mob_name(mob).c_str(),
                           gd->map_name(q.huntZone).c_str(), hz ? gd->map_name(hz).c_str() : "-");
    if (!hz) ImGui::TextColored(ui::kWarn, "No map is known for this monster: choose another quest.");

    // ── options ──
    section_title("Options");
    toggle_row("Come back through the success portal (it works while the quest's timer runs)", &q.usePortal);
    toggle_row("Take it again after each hand-in", &q.repeat);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "Stop after");
    ImGui::SameLine(110);
    slider("##maxruns", &q.maxRuns, 0, 300, q.maxRuns ? "%d hand-ins" : "no limit", 200);
    note("On, the Monster Quest turns the Drill off and the Attack on (and the Drill turns them off). The Attack then "
         "fights only in the hunt map and only the quest's monster (plus whoever hits you there, with the Attack's "
         "\"fight back\"), with its weapon and skill rotation; nothing is fought on the way. A kill counts only when "
         "you hurt the monster first. Loot works in the hunt map too, and with none of the monster to take the bot "
         "patrols its spawn spots, round after round in a changing order (Roam & Travel tab: the pause at each spot). The "
         "quest is handed in only after the server said all are killed: before that the server drops you.");
}

} // namespace nj
