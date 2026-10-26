// The Bot view's Story page: the numbers, the chapters (episode by episode: each one's scenario quests, what is done,
// whether it has a way and the special maps on it; a click picks the chapter to reach), where the run starts, the
// chosen chapter's quests, the options and the plan. The rules: bot/act_story.cpp + bot/quest_runner.cpp; the chapters,
// the dialogs and the maps only an item opens: quest_graph.cpp.
#include "app/neo_app.h"
#include "app/atlas.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/quest_graph.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include <algorithm>
#include <set>

namespace nj {

namespace {

using namespace bui;

// A chapter's plan on its own (the book + the bag now): a way or what stops it, and the special steps on it.
struct ChapterPreview {
    bool ok = false;
    std::string why;                    // blocked: the first job that stops it
    int left = 0;                       // jobs still to do
    std::string special;                // the maps only an item opens, the exchanges ("Hologram Chamber (Hologram Port)")
};

std::string npc_where(const GameData& gd, int npc)
{
    const AtlasNpc* n = gd.atlas().npc(npc);
    return n ? n->name + ", " + gd.map_name(n->zone) : std::string("-");
}

// where a job goes: its take's NPC, a gift's / an exchange's NPC (-1 = none)
int job_npc(const QuestGraph& g, const QJob& j)
{
    if (j.shop >= 0) return j.shop;
    if (j.compound) {
        const std::vector<QCompound>& cp = g.compounds(j.giveItem);
        return j.accept >= 0 && size_t(j.accept) < cp.size() ? cp[size_t(j.accept)].place.npc : -1;
    }
    if (j.exchange) {
        const std::vector<QExchange>& ex = g.exchanges(j.giveItem);
        return j.accept >= 0 && size_t(j.accept) < ex.size() ? ex[size_t(j.accept)].place.npc : -1;
    }
    if (j.giveItem) {
        const std::vector<QPlace>& gp = g.give_places(j.giveItem);
        return j.accept >= 0 && size_t(j.accept) < gp.size() ? gp[size_t(j.accept)].npc : -1;
    }
    const QuestPlaces* qp = g.places(j.q);
    return qp && j.accept >= 0 && size_t(j.accept) < qp->accepts.size() ? qp->accepts[size_t(j.accept)].npc : -1;
}

std::string job_name(const GameData& gd, const QuestGraph& g, const QJob& j)
{
    if (j.shop >= 0) return "buy " + gd.item_name(j.giveItem);
    if (j.compound) return "compound " + gd.item_name(j.giveItem);
    if (j.exchange) return "exchange for " + gd.item_name(j.giveItem);
    if (j.giveItem) return "from an NPC: " + gd.item_name(j.giveItem);
    return (j.q.mon ? "[M] " : j.q.scen ? "" : "[Q] ") + g.quest_name(j.q);
}

// the special steps of a plan: the maps only an item opens that its NPCs stand in, its exchanges
std::string special_of(const GameData& gd, const QuestGraph& g, const std::vector<QJob>& jobs)
{
    std::set<std::string> seen;
    std::string out;
    auto add = [&](const std::string& s) { if (seen.insert(s).second) out += (out.empty() ? "" : "; ") + s; };
    for (const QJob& j : jobs) {
        if (j.exchange) add("exchange: " + gd.item_name(j.giveItem));
        if (j.shop >= 0) add("buy: " + gd.item_name(j.giveItem));
        if (j.compound) add("compound: " + gd.item_name(j.giveItem));
        std::vector<int> npcs{ job_npc(g, j) };
        if (const QuestPlaces* qp = g.places(j.q))
            if (j.complete >= 0 && size_t(j.complete) < qp->completes.size()) npcs.push_back(qp->completes[size_t(j.complete)].npc);
        for (int n : npcs) {
            const AtlasNpc* a = gd.atlas().npc(n);
            int root = 0;
            const uint32_t it = a ? g.entry_item(a->zone, &root) : 0;
            if (it) add(gd.map_name(a->zone) + " (" + gd.item_name(it) + ")");
        }
    }
    return out;
}

// every chapter's preview, worked out again every 3 s (the page draws every frame)
const std::vector<ChapterPreview>& chapter_previews(const WorldModel& w, const StoryAction& st, const QuestBook& book)
{
    static std::vector<ChapterPreview> cache;
    static double at = -10.0;
    const GameData* gd = w.data();
    const QuestGraph& g = gd->quest_graph();
    const double t = ImGui::GetTime();
    if (t - at <= 3.0 && cache.size() == g.chapters().size()) return cache;
    at = t;
    cache.assign(g.chapters().size(), ChapterPreview{});
    const QuestRunner::View v(book, w);
    for (size_t i = 0; i < g.chapters().size(); ++i) {
        ChapterPreview& p = cache[i];
        std::vector<QJob> jobs;
        p.ok = g.expand(QKey{ false, g.chapters()[i].last, true }, 1, v, jobs);
        if (p.ok) st.add_providers(w, v, jobs);
        for (const QJob& j : jobs) {
            if (j.blocked && p.why.empty()) { p.ok = false; p.why = (j.giveItem ? job_name(*gd, g, j) : g.quest_name(j.q)) + ": " + j.why; }
            p.left += !j.giveItem && v.hist_lo(j.q) < j.target;
        }
        p.special = special_of(*gd, g, jobs);
    }
    return cache;
}

// the plan for the settings (the bot not running: a preview), every 2 s
const std::vector<QJob>& plan_preview(const WorldModel& w, const StoryAction& st, const QuestBook& book)
{
    static std::vector<QJob> plan;
    static double at = -10.0;
    const double t = ImGui::GetTime();
    if (t - at > 2.0) { at = t; plan = st.make_plan(w, book); }
    return plan;
}

std::string chapter_label(const QuestGraph::Chapter& c) { return fmt("Ep %d-%d %s", c.episode, c.chapter, c.name.c_str()); }

} // namespace

void NeoApp::bot_story(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    StoryAction& st = bot.story;
    ui::page_header("Story Quest", bot.running && st.enabled ? st.status.c_str()
                                                       : "the scenario quests, chapter by chapter - taken and handed in through the dialogs",
                    &st.enabled);
    ImGui::Dummy(ImVec2(0, 2));
    quest_activity(s, st, "Story Quest");
    const GameData* gd = w.data();
    if (!gd) { note("No game data (the db folder)."); return; }
    const QuestGraph& g = gd->quest_graph();
    const std::vector<QuestGraph::Chapter>& chs = g.chapters();
    if (chs.empty()) { note("No chapters in the data (scenario_quest_db.yml)."); return; }
    const QuestRunner::View v(bot.book, w);
    const bool live = bot.running && st.enabled;
    if (st.toChapter >= int(chs.size())) st.toChapter = -1;
    if (st.fromChapter >= int(chs.size())) st.fromChapter = -1;

    // ── the numbers ──
    int doneCh = 0;
    for (const auto& c : chs) doneCh += v.hist_lo(QKey{ false, c.last, true }) >= 1;
    const int start = st.start_chapter(w, bot.book);
    ui::stat("CHAPTERS DONE", fmt("%d / %zu", doneCh, chs.size()).c_str(), 130);
    ImGui::SameLine();
    ui::stat("STARTS AT", start >= 0 && start < int(chs.size()) ? fmt("Ep %d-%d", chs[size_t(start)].episode, chs[size_t(start)].chapter).c_str()
                                                                : "all done", 110);
    ImGui::SameLine();
    ui::stat("TARGET", st.toChapter >= 0 ? fmt("Ep %d-%d", chs[size_t(st.toChapter)].episode, chs[size_t(st.toChapter)].chapter).c_str() : "-", 100);
    ImGui::SameLine();
    ui::stat("TAKEN (RUN)", std::to_string(st.taken).c_str(), 110);
    ImGui::SameLine();
    ui::stat("HANDED IN (RUN)", std::to_string(st.handedIn).c_str(), 140);
    ImGui::SameLine();
    ui::stat("SCENARIO ON", fmt("%zu / 6", w.quests.sactive.size()).c_str(), 110);
    if (live && !st.plan.empty()) ImGui::TextColored(ui::kMuted, "next: %s", st.plan.c_str());
    if (live && st.errand_on()) ImGui::TextColored(ui::kAccent, "at the NPC: %s", st.walk().status.c_str());

    // ── the chapters ──
    section_title("Chapters");
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "  click the chapter to reach: the bot does every chapter from the start one up to it");
    const std::vector<ChapterPreview>& pv = chapter_previews(w, st, bot.book);
    if (begin_table("stchapters", 7)) {
        ImGui::TableSetupColumn("Ep", 0, 40);
        ImGui::TableSetupColumn("Chapter", 0, 250);
        ImGui::TableSetupColumn("Level", 0, 46);
        ImGui::TableSetupColumn("Quests", 0, 62);
        ImGui::TableSetupColumn("State", 0, 70);
        ImGui::TableSetupColumn("Way", 0, 230);
        ImGui::TableSetupColumn("Special steps", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < chs.size(); ++i) {
            const QuestGraph::Chapter& c = chs[i];
            const bool done = v.hist_lo(QKey{ false, c.last, true }) >= 1;
            int qDone = 0;
            bool on = false;
            for (int q : c.quests) {
                qDone += v.hist_lo(QKey{ false, q, true }) >= 1;
                on = on || v.active(QKey{ false, q, true });
            }
            const bool inRun = st.toChapter >= 0 && int(i) >= start && int(i) <= st.toChapter;
            ImGui::TableNextRow();
            ImGui::PushID(int(i));
            ImGui::TableNextColumn();
            if (ImGui::Selectable(fmt("%d-%d##ch", c.episode, c.chapter).c_str(), st.toChapter == int(i) || inRun,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                st.toChapter = st.toChapter == int(i) ? -1 : int(i);
            ImGui::TableNextColumn();
            ImGui::TextColored(done ? ui::kMuted : ui::kText, "%s", c.name.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scenario quests %d..%d", c.first, c.last);
            ImGui::TableNextColumn();
            ImGui::TextColored(c.minLevel > w.self.level && w.self.level > 0 ? ui::kWarn : ui::kText, "%d", c.minLevel);
            ImGui::TableNextColumn();
            ImGui::TextColored(qDone >= int(c.quests.size()) ? ui::kOk : ui::kText, "%d / %zu", qDone, c.quests.size());
            ImGui::TableNextColumn();
            if (done) ImGui::TextColored(ui::kOk, "done");
            else if (on) ImGui::TextColored(ui::kAccent, "on");
            else if (int(i) == start) ImGui::TextColored(ui::kText, "next");
            else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            const ChapterPreview* p = i < pv.size() ? &pv[i] : nullptr;
            if (done) ImGui::TextColored(ui::kMuted, "-");
            else if (!p) ImGui::TextColored(ui::kMuted, "?");
            else if (p->ok) {
                ImGui::TextColored(ui::kOk, "ok");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%d step(s) known to be left; the quests before it whose history is not known yet are taken as done "
                                      "until a dialog tells otherwise", p->left);
            } else {
                ImGui::TextColored(ui::kWarn, "%s", p->why.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.explain(QKey{ false, c.last, true }, v).c_str());
            }
            ImGui::TableNextColumn();
            if (p && !done) ImGui::TextColored(ui::kMuted, "%s", p->special.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ── where it starts ──
    section_title("Start");
    {
        const std::string autoText = start >= 0 && start < int(chs.size()) ? "Auto: the first chapter not done (" + chapter_label(chs[size_t(start)]) + ")"
                                                                           : std::string("Auto: the first chapter not done (all done)");
        const std::string cur = st.fromChapter < 0 ? autoText : chapter_label(chs[size_t(st.fromChapter)]);
        ImGui::SetNextItemWidth(420);
        if (ImGui::BeginCombo("##stfrom", cur.c_str())) {
            if (ImGui::Selectable(autoText.c_str(), st.fromChapter < 0)) st.fromChapter = -1;
            for (size_t i = 0; i < chs.size(); ++i)
                if (ImGui::Selectable(fmt("%s##f%zu", chapter_label(chs[i]).c_str(), i).c_str(), st.fromChapter == int(i))) st.fromChapter = int(i);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, "a chapter done before (the bot knows it from the server / the dialogs) is skipped anyway");
    }
    if (st.toChapter < 0) {
        note("Click the chapter to reach above. The bot then takes each scenario quest from its NPC as soon as the dialog offers it, "
             "gets its items (killing where they drop, digging where they are buried, another quest's reward, an NPC's gift, an "
             "exchange shop), hands it in and goes on - the quests the NPCs' dialogs put first are done first.");
        return;
    }

    // ── the chosen chapter's quests ──
    const QuestGraph::Chapter& tc = chs[size_t(st.toChapter)];
    section_title(("Chapter quests: " + chapter_label(tc)).c_str());
    if (begin_table("stquests", 5)) {
        ImGui::TableSetupColumn("Quest", 0, 240);
        ImGui::TableSetupColumn("NPC", 0, 280);
        ImGui::TableSetupColumn("Done", 0, 50);
        ImGui::TableSetupColumn("Asks for", 0, 260);
        ImGui::TableSetupColumn("Gives", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int q : tc.quests) {
            const QKey k{ false, q, true };
            const QuestInfo* qi = gd->scenario_quest(q);
            const QuestPlaces* qp = g.places(k);
            ImGui::TableNextRow();
            ImGui::PushID(q);
            ImGui::TableNextColumn();
            const bool qd = v.hist_lo(k) >= 1;
            ImGui::TextColored(qd ? ui::kMuted : ui::kText, "%s", qi ? qi->name.c_str() : g.quest_name(k).c_str());
            if (ImGui::IsItemHovered() && qi) ImGui::SetTooltip("#%d - %s\nLevel %d-%d", qi->id, qi->comment.c_str(), qi->minLevel, qi->maxLevel);
            ImGui::TableNextColumn();
            if (qp && !qp->accepts.empty()) ImGui::TextUnformatted(npc_where(*gd, qp->accepts.front().npc).c_str());
            else ImGui::TextColored(ui::kWarn, "no NPC");
            ImGui::TableNextColumn();
            if (qd) ImGui::TextColored(ui::kOk, "done");
            else if (v.active(k)) ImGui::TextColored(ui::kAccent, "on");
            else ImGui::TextColored(ui::kMuted, "%s", v.hist_hi(k) == 0 ? "no" : "?");
            ImGui::TableNextColumn();
            std::string asks;
            if (qi) for (const QuestNeed& n : qi->src) asks += fmt("%s%s x%d", asks.empty() ? "" : ", ", gd->item_name(n.item).c_str(), n.cnt);
            ImGui::TextColored(ui::kMuted, "%s", asks.empty() ? "-" : asks.c_str());
            ImGui::TableNextColumn();
            std::string gives;
            if (qi)
                if (const QuestResultRow* row = qi->result_for(1))
                    for (const QuestNeed& n : row->items) gives += fmt("%s%s x%d", gives.empty() ? "" : ", ", gd->item_name(n.item).c_str(), n.cnt);
            ImGui::TextColored(ui::kMuted, "%s", gives.empty() ? "-" : gives.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ── options ──
    section_title("Options");
    toggle_row("Monster quests: back through the success portal (while the quest's timer runs)", &st.usePortal);
    toggle_row("Buy the items a shop sells (quicker than hunting them)", &st.buyItems);
    ImGui::SameLine(0, 16);
    ImGui::BeginDisabled(!st.buyItems);
    slider("##buymax", &st.buyMaxGelder, 1000, 5000000, "at most %d galder a purchase", 280);
    ImGui::EndDisabled();
    toggle_row("Compound the items a compounder makes (its ingredients are gathered like the rest)", &st.compoundItems);
    ui::help("An item the plan needs: bought when a shop sells it (the cheapest one reached from here; never below the Town's "
             "\"galder always kept\"); else an exchange / a compound with what the bag already holds; else the quests that "
             "give it, an NPC, an exchange shop; a compounder last (its rate: more tries than items, the failures leave its "
             "waste item).");

    // ── the plan ──
    section_title(live ? "The plan" : "The plan (a preview: what Start would do now)");
    const std::vector<QJob>& plan = live ? st.jobs() : plan_preview(w, st, bot.book);
    const std::vector<QuestRunner::JobInfo>* info = live && st.job_info().size() == plan.size() ? &st.job_info() : nullptr;
    if (plan.empty()) {
        ImGui::TextColored(ui::kOk, "nothing left to do: the chapters chosen are done");
    } else if (begin_table("stplan", 6)) {
        ImGui::TableSetupColumn("#", 0, 22);
        ImGui::TableSetupColumn("Step", 0, 280);
        ImGui::TableSetupColumn("Done", 0, 60);
        ImGui::TableSetupColumn("State", 0, 100);
        ImGui::TableSetupColumn("NPC", 0, 280);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < plan.size(); ++i) {
            const QJob& j = plan[i];
            const int lo = v.hist_lo(j.q), hi = v.hist_hi(j.q);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(job_name(*gd, g, j).c_str());
            ImGui::TableNextColumn();
            if (j.giveItem) ImGui::TextColored(ui::kText, "%d/%d", std::min(w.count_item(j.giveItem), j.target), j.target);
            else if (j.keepActive) ImGui::TextColored(ui::kMuted, "on");
            else ImGui::TextColored(hi < 0 ? ui::kMuted : ui::kText, hi < 0 ? "%d+/%d" : "%d/%d", std::min(lo, j.target), j.target);
            ImGui::TableNextColumn();
            if (info) {
                const QuestRunner::JobInfo& f = (*info)[i];
                const ImVec4 col = f.state == QuestRunner::JDone ? ui::kOk : f.state == QuestRunner::JOn ? ui::kAccent
                                 : f.state == QuestRunner::JBlocked || f.state == QuestRunner::JWaitItem || f.state == QuestRunner::JNoWay ? ui::kWarn : ui::kMuted;
                ImGui::TextColored(col, "%s", QuestRunner::state_name(f.state));
            } else {
                ImGui::TextColored(j.blocked ? ui::kWarn : v.active(j.q) ? ui::kAccent : ui::kMuted, "%s",
                                   j.blocked ? "blocked" : v.active(j.q) ? "on" : "to do");
            }
            ImGui::TableNextColumn();
            const int npc = job_npc(g, j);
            if (npc >= 0) ImGui::TextUnformatted(npc_where(*gd, npc).c_str());
            else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            std::string note2 = info ? (*info)[i].note : j.why;
            if (note2.empty())
                for (const QCond& c : j.assumed) if (c.kind == QCond::HistGE) note2 += (note2.empty() ? "takes as done: " : "; ") + g.cond_text(c);
            if (note2.empty() && j.q.mon)
                if (const MonQuestInfo* mi = gd->monster_quest(j.q.id)) {
                    const int gr = mi->grade_for(w.self.level);
                    if (gr >= 0) note2 = fmt("kill %d x %s in %d min", mi->grades[size_t(gr)].cnt, gd->mob_name(uint32_t(mi->grades[size_t(gr)].monster)).c_str(),
                                             mi->grades[size_t(gr)].timeMin);
                }
            if (note2.empty() && npc >= 0)
                if (const AtlasNpc* a = gd->atlas().npc(npc))
                    if (const uint32_t it = g.entry_item(a->zone))
                        note2 = fmt("%s opens %s", gd->item_name(it).c_str(), gd->map_name(a->zone).c_str());
            ImGui::TextColored(ui::kMuted, "%s", note2.c_str());
        }
        ImGui::EndTable();
    }
    if (live && !st.gather.list.empty()) {
        std::string items;
        for (const HuntItem& h : st.gather.list)
            items += fmt("%s%s %d/%d", items.empty() ? "" : ", ", gd->item_name(h.item).c_str(), w.count_item(h.item), h.want);
        ImGui::TextColored(ui::kMuted, "items: %s", items.c_str());
        if (st.gather.source().zone) ImGui::TextColored(ui::kMuted, "getting them: %s", st.gather.source().label.c_str());
    }
    note("Each scenario quest is taken and handed in at its NPC the way you would click it (the dialog's checks answer with the "
         "next message; what they tell of the history is learned). A scenario quest is taken as soon as its NPC offers it - "
         "at most 6 at once. Some NPCs stand in maps only an item opens: the Hologram Chambers (a Hologram Port / the "
         "Holographic Photo, used up: such a quest's items are got first, so one trip does it), Rosaline's Memory (the Approved "
         "Memory Kit 2: JOKER's exchange shop gives it for the Memory Kit 2), Ocean's End (Love Hunter Robin moves you there "
         "while the Contract of Secrecy is in the bag); the earlier quests give those items, the bot keeps them and uses them "
         "only when no portal / teleport gets there. The way out of a Hologram Chamber is Don Cavalier. What the server told "
         "of each quest is kept per character (profiles\\quests_<id>.txt) with the Key Quest's.");
}

} // namespace nj
