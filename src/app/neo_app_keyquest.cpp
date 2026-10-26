// The Bot view's Key Quest page: the numbers, the towns (each one's key quest and its stickers), the town's sticker
// quests (a checkbox each: what it asks for, what the dialogs put before it, its sticker), the options and the plan.
// The rules: bot/act_keyquest.cpp; the dialogs, the gates and the towns: quest_graph.cpp.
#include "app/neo_app.h"
#include "app/atlas.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/item_sources.h"
#include "app/quest_graph.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace nj {

namespace {

using namespace bui;

std::string hours_text(double h)
{
    if (!std::isfinite(h)) return "-";
    return h < 1.0 ? fmt("~%.0f min", std::max(1.0, h * 60.0)) : fmt("~%.1f h", h);
}

// an item's best usable source an hour now (0 = none), worked out again every 30 s
double item_rate(const WorldModel& w, uint32_t item)
{
    static std::map<uint32_t, std::pair<double, double>> cache;
    const double t = ImGui::GetTime();
    auto& e = cache[item];
    if (e.first <= 0.0 || t - e.first > 30.0) {
        e.first = t;
        e.second = 0.0;
        for (const HuntSource& s : ItemHuntAction::sources(w, item))
            if (s.usable) { e.second = s.perHour; break; }
    }
    return e.second;
}

// A sticker quest on its own: its plan (the quests before it), whether it has a way, a rough time.
struct QuestPreview {
    std::vector<QJob> jobs;
    bool ok = false;
    double hours = 0.0;                 // the items at their best source + the monster quests' kills + the trips
    std::string before;                 // the quests the dialogs put first ("M266 x5 > Survey 2 x5")
    std::string assumed;                // what is taken as done (not known yet)
    std::string why;                    // no time: the item that stops it
};

QuestPreview preview_of(const WorldModel& w, const KeyQuestAction& k, const QuestBook& book, const StickerQuest& sq)
{
    QuestPreview p;
    const GameData* gd = w.data();
    const QuestGraph& g = gd->quest_graph();
    const QuestRunner::View v(book, w);
    p.ok = g.expand(sq.q, sq.runs, v, p.jobs);
    if (p.ok) k.add_providers(w, v, p.jobs);
    for (const QJob& j : p.jobs) p.ok = p.ok && !j.blocked;
    std::set<uint32_t> rewards;
    for (const QJob& j : p.jobs) {
        if (j.q.mon) continue;
        if (const QuestInfo* qi = gd->quest(j.q.id))
            for (int r = v.hist_lo(j.q) + 1; r <= j.target; ++r)
                if (const QuestResultRow* row = qi->result_for(r)) for (const QuestNeed& it : row->items) rewards.insert(it.item);
    }
    for (const QJob& j : p.jobs) {
        const int runs = std::max(0, j.target - v.hist_lo(j.q));
        if (!(j.q == sq.q)) p.before += fmt("%s%s%s x%d", p.before.empty() ? "" : " > ", j.q.mon ? "monster quest: " : "",
                                            j.q.mon ? gd->mob_name(gd->monster_quest(j.q.id) && !gd->monster_quest(j.q.id)->grades.empty()
                                                                       ? uint32_t(gd->monster_quest(j.q.id)->grades.front().monster) : 0).c_str()
                                                    : g.quest_name(j.q).c_str(), j.target);
        for (const QCond& c : j.assumed)
            if (c.kind == QCond::HistGE) p.assumed += (p.assumed.empty() ? "" : "; ") + g.cond_text(c);
        p.hours += 0.05;                                    // its NPC trips
        if (j.q.mon) {
            const MonQuestInfo* mi = gd->monster_quest(j.q.id);
            const int gr = mi ? mi->grade_for(w.self.level) : -1;
            p.hours += runs * (gr >= 0 ? std::max(0.05, mi->grades[size_t(gr)].cnt / 120.0) : 0.25) + runs * 0.05;
            continue;
        }
        const QuestInfo* qi = gd->quest(j.q.id);
        if (!qi) continue;
        for (const QuestNeed& n : qi->src) {
            const int want = n.cnt * runs - w.count_item(n.item);
            if (want <= 0) continue;
            const double r = item_rate(w, n.item);
            if (r > 0.0) { p.hours += want / r; continue; }
            if (rewards.count(n.item)) continue;            // (a quest before it gives it)
            p.hours = std::numeric_limits<double>::infinity();
            if (p.why.empty()) {
                const ItemSources& is = gd->item_sources();
                const bool any = !is.mobs(n.item).empty() || !is.digs(n.item).empty();
                p.why = gd->item_name(n.item) + (!any ? ": nobody gives it (a quest's reward / the bag)"
                                                     : is.mobs(n.item).empty() ? ": only dug up - a drill in the bag that reaches it"
                                                                               : ": its monsters are too strong for now");
            }
        }
    }
    if (!p.ok) p.hours = std::numeric_limits<double>::infinity();
    return p;
}

// the previews of a town's quests, worked out again every 2 s (the page draws every frame)
const std::map<int, QuestPreview>& previews(const WorldModel& w, const KeyQuestAction& k, const QuestBook& book, const KeyTown& t)
{
    static std::map<int, QuestPreview> cache;
    static uint32_t forTown = 0;
    static double at = -10.0;
    const double now = ImGui::GetTime();
    if (forTown != t.sticker || now - at > 2.0) {
        forTown = t.sticker;
        at = now;
        cache.clear();
        for (const StickerQuest& sq : t.quests) cache[sq.q.id] = preview_of(w, k, book, sq);
    }
    return cache;
}

// the plan for what is picked (the bot not running: a preview), every 2 s
const std::vector<QJob>& plan_preview(const WorldModel& w, const KeyQuestAction& k, const QuestBook& book)
{
    static std::vector<QJob> plan;
    static double at = -10.0;
    const double t = ImGui::GetTime();
    if (t - at > 2.0) { at = t; plan = k.make_plan(w, book); }
    return plan;
}

std::string npc_where(const GameData& gd, int npc)
{
    const AtlasNpc* n = gd.atlas().npc(npc);
    return n ? n->name + ", " + gd.map_name(n->zone) : std::string("-");
}

} // namespace

// The activity panel (Key Quest / Story Quest / Monster Quest): a live user asked for "something visual that shows it is
// being done" — a click, then the character standing there with nothing said.
void NeoApp::activity_box(Session& s, BotAction& a, const char* what, const Activity& act)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    const uint64_t now = now_filetime();
    const double t = ImGui::GetTime();
    const bool live = s.state == Session::State::Live && w.inWorld;
    const double since = bot.last_request_at() && now > bot.last_request_at() ? double(now - bot.last_request_at()) / 1e7 : -1.0;
    const ImVec4 tones[] = { ui::kMuted, ui::kOk, ui::kWarn, ui::kErr, ui::kAccent };
    const ImVec4 col = tones[std::clamp(act.tone, 0, 4)];
    ImGui::PushID(what);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float wdt = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Indent(14);
    // a pulsing dot while it works
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const bool busy = bot.running && a.enabled && since >= 0 && since < 15.0;
        const float al = busy ? 0.55f + 0.45f * float(std::sin(t * 6.0)) : 1.0f;
        dl->AddCircleFilled(ImVec2(c.x + 7, c.y + ImGui::GetFrameHeight() * 0.5f), 6.0f,
                            ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, al)), 20);
        ImGui::Dummy(ImVec2(18, ImGui::GetFrameHeight()));
    }
    ImGui::SameLine();
    ui::pill(act.state.c_str(), col);
    ImGui::SameLine(0, 10);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "%s", act.line.c_str());
    if (!bot.running && a.enabled) {
        ImGui::SameLine(0, 14);
        ImGui::BeginDisabled(!live || w.self.dead);
        if (ui::button_primary("Start", ImVec2(90, 0))) bot.start(w, now);
        ImGui::EndDisabled();
    }
    ImGui::TextColored(ui::kMuted, "step");
    ImGui::SameLine(70);
    ImGui::TextUnformatted(act.step.empty() ? "-" : act.step.c_str());
    if (!act.progress.empty()) {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, wdt * 0.55f));
        ui::bar(act.frac, ui::kAccent, std::max(120.0f, wdt * 0.40f - 20), act.progress.c_str(), ImGui::GetTextLineHeight() + 2);
    }
    ImGui::TextColored(ui::kMuted, "last request");
    ImGui::SameLine(110);
    if (since < 0) ImGui::TextColored(ui::kMuted, "none yet");
    else ImGui::TextColored(since >= 15.0 ? ui::kErr : since >= 5.0 ? ui::kWarn : ui::kText, "%.1f s ago - %s", since,
                            bot.doing.c_str());
    // standing still / waiting: the latest lines of the log
    if (bot.running && a.enabled && (since >= 15.0 || act.showLog)) {
        int shown = 0;
        for (auto it = bot.log.rbegin(); it != bot.log.rend() && shown < 4; ++it, ++shown)
            ImGui::TextColored(ui::kMuted, "  %s", it->c_str());
    }
    ImGui::Unindent(14);
    ImGui::Dummy(ImVec2(0, 4));
    const ImVec2 p1(p0.x + wdt, ImGui::GetCursorScreenPos().y);
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.55f)), 4.0f, 0, 1.5f);
    dl->AddRectFilled(p0, ImVec2(p0.x + 4, p1.y), ImGui::GetColorU32(col), 4.0f);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 4));
}

void NeoApp::quest_activity(Session& s, QuestRunner& qr, const char* what)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    const GameData* gd = w.data();
    const uint64_t now = now_filetime();
    const double since = bot.last_request_at() && now > bot.last_request_at() ? double(now - bot.last_request_at()) / 1e7 : -1.0;
    Activity act;
    if (!qr.enabled) {
        act.state = "OFF";
        act.line = std::string("switch the ") + what + " on (top right of this page)";
    } else if (!bot.running) {
        act.tone = 3;
        act.state = "NOT RUNNING";
        act.line = bot.stopWhy.empty() ? std::string("the bot is not started: press Start") : "stopped: " + bot.stopWhy;
    } else if (since >= 15.0 && !w.changingZone) {
        act.tone = 3;
        act.state = fmt("STANDING STILL %.0f s", since);
        act.line = bot.doing.empty() ? qr.status : bot.doing;
    } else if (w.changingZone) {
        act.tone = 4;
        act.state = "CHANGING MAP";
        act.line = bot.doing;
    } else if (qr.errand_on()) {
        act.tone = 1;
        act.state = "AT AN NPC";
        act.line = qr.errand_what() + ": " + qr.walk().status;
    } else if (qr.monq_hunting(w)) {
        act.tone = 1;
        act.state = "MONSTER QUEST";
        act.line = qr.plan;
    } else if (qr.gathering()) {
        act.tone = 1;
        act.state = "GETTING ITEMS";
        act.line = qr.plan;
    } else if (qr.idle()) {
        act.tone = 2;
        act.state = "WAITING";
        act.line = qr.plan.empty() ? qr.status : qr.plan;
    } else {
        act.tone = 1;
        act.state = "WORKING";
        act.line = bot.doing;
    }
    // the step and the progress
    const std::vector<QJob>& jobs = qr.jobs();
    const std::vector<QuestRunner::JobInfo>& info = qr.job_info();
    int done = 0, total = int(jobs.size());
    for (size_t i = 0; i < info.size() && i < jobs.size(); ++i) done += info[i].state == QuestRunner::JDone;
    if (gd && qr.errand_on()) act.step = qr.errand_what();
    else if (gd)
        for (size_t i = 0; i < info.size() && i < jobs.size() && act.step.empty(); ++i)
            if (info[i].state == QuestRunner::JOn || info[i].state == QuestRunner::JReady)
                act.step = gd->quest_graph().quest_name(jobs[i].q) + (info[i].state == QuestRunner::JOn ? " (on)" : " (to take)");
    act.frac = total ? float(done) / float(total) : 0.0f;
    act.progress = fmt("%d / %d jobs done", done, total);
    act.showLog = qr.idle();
    activity_box(s, qr, what, act);
}

void NeoApp::bot_keyquest(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    KeyQuestAction& k = bot.keyq;
    const uint64_t now = now_filetime();
    ui::page_header("Key Quest", bot.running && k.enabled ? k.status.c_str()
                                                          : "a town's sticker quests, then its key quest - taken and handed in through the dialogs",
                    &k.enabled);
    ImGui::Dummy(ImVec2(0, 2));
    quest_activity(s, k, "Key Quest");
    const GameData* gd = w.data();
    if (!gd) { note("No game data (the db folder)."); return; }
    const QuestGraph& g = gd->quest_graph();
    const QuestRunner::View v(bot.book, w);
    const KeyTown* town = g.town(k.sticker);
    const bool live = bot.running && k.enabled;

    // ── the numbers ──
    const int have = town ? w.count_item(town->sticker) : 0;
    ui::stat("STICKERS", town ? fmt("%d / %d", have, town->need).c_str() : "-", 110);
    ImGui::SameLine();
    int doneQ = 0;
    if (town)
        for (const StickerQuest& sq : town->quests) doneQ += k.picked.count(sq.q.id) && v.hist_lo(sq.q) >= sq.runs;
    ui::stat("PICKED DONE", town ? fmt("%d / %zu", doneQ, k.picked.size()).c_str() : "-", 120);
    ImGui::SameLine();
    ui::stat("TAKEN (RUN)", std::to_string(k.taken).c_str(), 110);
    ImGui::SameLine();
    ui::stat("HANDED IN (RUN)", std::to_string(k.handedIn).c_str(), 140);
    ImGui::SameLine();
    ui::stat("QUESTS ON", fmt("%zu / 6", w.quests.active.size()).c_str(), 100);
    if (live && !k.plan.empty()) ImGui::TextColored(ui::kMuted, "next: %s", k.plan.c_str());
    if (live && k.errand_on()) ImGui::TextColored(ui::kAccent, "at the NPC: %s", k.walk().status.c_str());

    // ── the towns ──
    section_title("Town");
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "  each town's key quest asks for its stickers; its people's quests give them");
    if (begin_table("kqtowns", 6)) {
        ImGui::TableSetupColumn("Town", 0, 190);
        ImGui::TableSetupColumn("Key quest", 0, 330);
        ImGui::TableSetupColumn("Level", 0, 50);
        ImGui::TableSetupColumn("Stickers", 0, 80);
        ImGui::TableSetupColumn("Key", 0, 70);
        ImGui::TableSetupColumn("Sticker quests", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const KeyTown& t : g.towns()) {
            const QuestInfo* kq = t.key.id ? gd->quest(t.key.id) : nullptr;
            const QuestPlaces* kp = t.key.id ? g.places(t.key) : nullptr;
            ImGui::TableNextRow();
            ImGui::PushID(int(t.sticker));
            ImGui::TableNextColumn();
            const float rowY = ImGui::GetCursorPosY();
            if (ImGui::Selectable("##town", k.sticker == t.sticker, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, kIcon))) {
                if (k.sticker != t.sticker) k.picked.clear();
                k.sticker = t.sticker;
            }
            ImGui::SameLine(0, 0);
            ImGui::SetCursorPosY(rowY);
            icon_text(gfx::item(t.sticker), t.name);
            ImGui::TableNextColumn(); mid();
            if (kq) ImGui::TextUnformatted(fmt("%s - %s", kq->name.c_str(), kp && !kp->accepts.empty() ? npc_where(*gd, kp->accepts.front().npc).c_str()
                                                                                                    : kq->client.c_str()).c_str());
            else ImGui::TextColored(ui::kMuted, "no key quest in the data");
            ImGui::TableNextColumn(); mid();
            if (kq) ImGui::Text("%d", kq->minLevel);
            ImGui::TableNextColumn(); mid();
            const int n = w.count_item(t.sticker);
            if (t.need) ImGui::TextColored(n >= t.need ? ui::kOk : ui::kText, "%d / %d", n, t.need);
            else ImGui::Text("%d", n);
            ImGui::TableNextColumn(); mid();
            if (t.key.id) {
                const int lo = v.hist_lo(t.key), hi = v.hist_hi(t.key);
                if (lo >= 1) ImGui::TextColored(ui::kOk, "done");
                else if (v.active(t.key)) ImGui::TextColored(ui::kAccent, "on");
                else ImGui::TextColored(ui::kMuted, "%s", hi == 0 ? "not yet" : "?");
            }
            ImGui::TableNextColumn(); mid();
            int doneHere = 0;
            for (const StickerQuest& sq : t.quests) doneHere += v.hist_lo(sq.q) >= sq.runs;
            ImGui::TextColored(ui::kMuted, "%zu (%d done that the bot knows of)", t.quests.size(), doneHere);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!town) {
        note("Choose a town above. Then tick its sticker quests: the bot takes each one from its NPC, gets its items (killing where "
             "they drop, digging where they are buried), hands it in until the run that gives the sticker, and does first the "
             "quests the NPCs' dialogs put before it (a chain, a monster quest to kill for). Then the key quest itself.");
        return;
    }

    // ── its sticker quests ──
    section_title(("Sticker quests: " + town->name).c_str());
    const std::map<int, QuestPreview>& pv = previews(w, k, bot.book, *town);
    double expected = 0.0;
    for (const StickerQuest& sq : town->quests)
        if (k.picked.count(sq.q.id) && v.hist_lo(sq.q) < sq.runs) expected += sq.stickers;
    const int missing = std::max(0, town->need - have);
    ImGui::TextColored(ui::kMuted, "%d in the bag, %d needed%s; the picked ones give ~%.1f more", have, town->need,
                       missing ? fmt(" (%d missing)", missing).c_str() : "", expected);
    if (ImGui::SmallButton(fmt("Pick the quickest (%d sticker%s)", missing, missing == 1 ? "" : "s").c_str())) {
        std::vector<std::pair<double, const StickerQuest*>> order;
        for (const StickerQuest& sq : town->quests) {
            auto it = pv.find(sq.q.id);
            if (it == pv.end() || !it->second.ok || !std::isfinite(it->second.hours) || v.hist_lo(sq.q) >= sq.runs) continue;
            order.push_back({ it->second.hours / std::max(0.01, sq.stickers), &sq });
        }
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        k.picked.clear();
        double got = 0.0;
        for (const auto& o : order) {
            if (got >= missing - 1e-6) break;
            k.picked.insert(o.second->q.id);
            got += o.second->stickers;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("All with a way")) {
        for (const StickerQuest& sq : town->quests) {
            auto it = pv.find(sq.q.id);
            if (it != pv.end() && it->second.ok && v.hist_lo(sq.q) < sq.runs) k.picked.insert(sq.q.id);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) k.picked.clear();
    // the rows: with a way first, then the quickest
    std::vector<const StickerQuest*> rows;
    for (const StickerQuest& sq : town->quests) rows.push_back(&sq);
    auto hrs = [&](const StickerQuest* sq) { auto it = pv.find(sq->q.id); return it == pv.end() ? 1e18 : it->second.hours; };
    std::stable_sort(rows.begin(), rows.end(), [&](const StickerQuest* a, const StickerQuest* b) {
        const double ha = hrs(a), hb = hrs(b);
        const bool fa = std::isfinite(ha), fb = std::isfinite(hb);
        if (fa != fb) return fa;
        return ha < hb;
    });
    const ItemSources& is = gd->item_sources();
    if (begin_table("kqquests", 8)) {
        ImGui::TableSetupColumn("", 0, 24);
        ImGui::TableSetupColumn("Quest", 0, 200);
        ImGui::TableSetupColumn("NPC", 0, 200);
        ImGui::TableSetupColumn("Done", 0, 46);
        ImGui::TableSetupColumn("Brings (each run)", 0, 230);
        ImGui::TableSetupColumn("Sticker", 0, 60);
        ImGui::TableSetupColumn("Time", 0, 62);
        ImGui::TableSetupColumn("Before it / state", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const StickerQuest* sq : rows) {
            const QuestInfo* qi = gd->quest(sq->q.id);
            const QuestPlaces* qp = g.places(sq->q);
            const auto it = pv.find(sq->q.id);
            const QuestPreview* p = it == pv.end() ? nullptr : &it->second;
            const int lo = v.hist_lo(sq->q), hi = v.hist_hi(sq->q);
            const bool done = lo >= sq->runs;
            ImGui::TableNextRow();
            ImGui::PushID(sq->q.id);
            ImGui::TableNextColumn();
            bool on = k.picked.count(sq->q.id) != 0;
            ImGui::BeginDisabled(done);
            if (ImGui::Checkbox("##pick", &on)) { if (on) k.picked.insert(sq->q.id); else k.picked.erase(sq->q.id); }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::TextColored(done ? ui::kMuted : ui::kText, "%s", qi ? qi->name.c_str() : "?");
            if (ImGui::IsItemHovered() && qi) ImGui::SetTooltip("#%d - %s\nLevel %d-%d, %d times at most", qi->id, qi->comment.c_str(),
                                                                qi->minLevel, qi->maxLevel, qi->maxCount);
            ImGui::TableNextColumn();
            if (qp && !qp->accepts.empty()) ImGui::TextUnformatted(npc_where(*gd, qp->accepts.front().npc).c_str());
            else ImGui::TextColored(ui::kWarn, "no NPC");
            ImGui::TableNextColumn();
            if (done) ImGui::TextColored(ui::kOk, "%d/%d", std::min(lo, sq->runs), sq->runs);
            else ImGui::TextColored(hi < 0 ? ui::kMuted : ui::kText, hi < 0 ? "%d+/%d" : "%d/%d", lo, sq->runs);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(hi < 0 ? "Not known yet: the bot learns it from the NPC's dialog."
                                                                 : "Times handed in, as the server told it.");
            ImGui::TableNextColumn();
            if (qi) {
                for (size_t n = 0; n < qi->src.size(); ++n) {
                    const QuestNeed& q = qi->src[n];
                    const bool src = !is.mobs(q.item).empty() || !is.digs(q.item).empty();
                    if (n) { ImGui::SameLine(0, 4); ImGui::TextColored(ui::kMuted, ","); ImGui::SameLine(0, 4); }
                    ImGui::TextColored(src ? ui::kText : ui::kWarn, "%s x%d", gd->item_name(q.item).c_str(), q.cnt);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", src ? fmt("%zu monster(s) drop it, %zu map(s) bury it; in the bag: %d", is.mobs(q.item).size(),
                                                         is.digs(q.item).size(), w.count_item(q.item)).c_str()
                                                    : fmt("No monster drops it, no map buries it: a quest's reward or the bag (%d in it).",
                                                          w.count_item(q.item)).c_str());
                }
            }
            ImGui::TableNextColumn();
            if (sq->stickers >= 0.999) ImGui::Text("run %d", sq->runs);
            else ImGui::TextColored(ui::kWarn, "%.0f %% run %d", sq->stickers * 100.0, sq->runs);
            ImGui::TableNextColumn();
            ImGui::TextColored(ui::kMuted, "%s", done ? "-" : p ? hours_text(p->hours).c_str() : "-");
            ImGui::TableNextColumn();
            if (done) ImGui::TextColored(ui::kOk, "done");
            else if (!p) ImGui::TextColored(ui::kMuted, "-");
            else if (!p->ok) {
                std::string why = "no way";
                for (const QJob& j : p->jobs)
                    if (j.blocked) { why = (j.q == sq->q ? std::string() : g.quest_name(j.q) + ": ") + j.why; break; }
                ImGui::TextColored(ui::kWarn, "%s", why.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.explain(sq->q, v).c_str());
            } else if (!p->before.empty()) {
                ImGui::TextColored(ui::kMuted, "after %s", p->before.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", (p->before + (p->assumed.empty() ? "" : "\nTaken as done (the dialog will tell): " + p->assumed)).c_str());
            } else if (!std::isfinite(p->hours)) {
                ImGui::TextColored(ui::kWarn, "%s", p->why.empty() ? "an item has no usable source" : p->why.c_str());
            } else {
                ImGui::TextColored(ui::kMuted, "%s", v.active(sq->q) ? "on" : "-");
                if (!p->assumed.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("Taken as done (the dialog will tell): %s", p->assumed.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // ── options ──
    section_title("Options");
    std::string chain;
    for (const QKey& c : town->chain) chain += (chain.empty() ? "" : ", ") + g.quest_name(c);
    bool onlyStickers = !k.doKey;                       // (the profile keeps doKey)
    if (town->key.id) {
        toggle_row("Only complete sticker quests (the town's quests, not the key quest itself)", &onlyStickers);
        k.doKey = !onlyStickers;
        ImGui::TextColored(ui::kMuted, "%s", onlyStickers ? "the key quest is left for you: the stickers stay in the bag (never sold)"
                                                          : fmt("then the key quest and what follows it at its NPC: %s", chain.c_str()).c_str());
    } else {
        ImGui::TextColored(ui::kMuted, "this town has no key quest in the data: only its sticker quests");
    }
    toggle_row("Monster quests: back through the success portal (while the quest's timer runs)", &k.usePortal);
    toggle_row("Buy the items a shop sells (quicker than hunting them)", &k.buyItems);
    ImGui::SameLine(0, 16);
    ImGui::BeginDisabled(!k.buyItems);
    slider("##buymax", &k.buyMaxGelder, 1000, 5000000, "at most %d galder a purchase", 280);
    ImGui::EndDisabled();
    toggle_row("Compound the items a compounder makes (its ingredients are gathered like the rest)", &k.compoundItems);
    ui::help("An item the plan needs: bought when a shop sells it (the cheapest one reached from here; never below the Town's "
             "\"galder always kept\"); else an exchange / a compound with what the bag already holds; else the quests that "
             "give it, an NPC, an exchange shop; a compounder last (its rate: more tries than items, the failures leave its "
             "waste item).");

    // ── the plan ──
    section_title(live ? "The plan" : "The plan (a preview: what Start would do now)");
    const std::vector<QJob>& plan = live ? k.jobs() : plan_preview(w, k, bot.book);
    const std::vector<KeyQuestAction::JobInfo>* info = live && k.job_info().size() == plan.size() ? &k.job_info() : nullptr;
    if (plan.empty()) {
        ImGui::TextColored(k.picked.empty() && !k.doKey ? ui::kMuted : ui::kOk, "%s",
                           k.picked.empty() && !k.doKey ? "nothing picked" : "nothing left to do: everything picked is done");
    } else if (begin_table("kqplan", 6)) {
        ImGui::TableSetupColumn("#", 0, 22);
        ImGui::TableSetupColumn("Quest", 0, 260);
        ImGui::TableSetupColumn("Done", 0, 60);
        ImGui::TableSetupColumn("State", 0, 110);
        ImGui::TableSetupColumn("NPC", 0, 260);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < plan.size(); ++i) {
            const QJob& j = plan[i];
            const QuestPlaces* qp = g.places(j.q);
            const int lo = v.hist_lo(j.q), hi = v.hist_hi(j.q);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(((j.q.mon ? "[M] " : "") + g.quest_name(j.q)).c_str());
            ImGui::TableNextColumn();
            if (j.keepActive) ImGui::TextColored(ui::kMuted, "on");
            else ImGui::TextColored(hi < 0 ? ui::kMuted : ui::kText, hi < 0 ? "%d+/%d" : "%d/%d", std::min(lo, j.target), j.target);
            ImGui::TableNextColumn();
            if (info) {
                const KeyQuestAction::JobInfo& f = (*info)[i];
                const ImVec4 col = f.state == KeyQuestAction::JDone ? ui::kOk : f.state == KeyQuestAction::JOn ? ui::kAccent
                                 : f.state == KeyQuestAction::JBlocked || f.state == KeyQuestAction::JWaitItem || f.state == KeyQuestAction::JNoWay ? ui::kWarn : ui::kMuted;
                ImGui::TextColored(col, "%s", KeyQuestAction::state_name(f.state));
            } else {
                ImGui::TextColored(j.blocked ? ui::kWarn : v.active(j.q) ? ui::kAccent : ui::kMuted, "%s",
                                   j.blocked ? "blocked" : v.active(j.q) ? "on" : "to do");
            }
            ImGui::TableNextColumn();
            if (qp && j.accept >= 0 && size_t(j.accept) < qp->accepts.size()) ImGui::TextUnformatted(npc_where(*gd, qp->accepts[size_t(j.accept)].npc).c_str());
            else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            std::string note2 = info ? (*info)[i].note : j.why;
            if (note2.empty() && !j.assumed.empty()) {
                for (const QCond& c : j.assumed) if (c.kind == QCond::HistGE) note2 += (note2.empty() ? "takes as done: " : "; ") + g.cond_text(c);
            }
            if (note2.empty() && j.q.mon)
                if (const MonQuestInfo* mi = gd->monster_quest(j.q.id)) {
                    const int gr = mi->grade_for(w.self.level);
                    if (gr >= 0) note2 = fmt("kill %d x %s in %d min", mi->grades[size_t(gr)].cnt, gd->mob_name(uint32_t(mi->grades[size_t(gr)].monster)).c_str(),
                                             mi->grades[size_t(gr)].timeMin);
                }
            ImGui::TextColored(ui::kMuted, "%s", note2.c_str());
        }
        ImGui::EndTable();
    }
    if (live && !k.gather.list.empty()) {
        std::string items;
        for (const HuntItem& h : k.gather.list)
            items += fmt("%s%s %d/%d", items.empty() ? "" : ", ", gd->item_name(h.item).c_str(), w.count_item(h.item), h.want);
        ImGui::TextColored(ui::kMuted, "items: %s", items.c_str());
        if (k.gather.source().zone) ImGui::TextColored(ui::kMuted, "getting them: %s", k.gather.source().label.c_str());
    }
    (void)now;
    note("Each quest is taken and handed in at its NPC the way you would click it: the bot opens the dialog, follows its options "
         "(each check the NPC makes - a quest done so many times, the quest's own state - answers with the next message) and "
         "learns from the answers what was done before; a quest the dialogs put first is done first (a chain of the NPC's quests, "
         "a monster quest to kill for: its monster is fought in its map, the success portal brings you back). At most 6 quests "
         "can be on at once. The items come from the Item Hunt's planner: the map that gives the most of them at once, killing "
         "where they drop, digging where they are buried (a drill in the bag); they are always picked up and never sold or "
         "stored. Most sticker quests give their sticker at their 5th hand-in. What the server told of each quest's history "
         "is kept per character (profiles\\quests_<id>.txt), so the next run starts where this one stopped.");
}

} // namespace nj
