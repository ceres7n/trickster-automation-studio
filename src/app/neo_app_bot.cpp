// The Bot view — the main screen: a toolbar (state, start / stop, profile) over the page of the
// current tab: Overview, then one page per action of the bot (bot/) in its priority order. The
// activity log lives in the bottom panel; the profile is saved per character.
#include "app/neo_app.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/gfx.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace nj {

namespace {

using namespace bui;

std::string duration(uint64_t ft)
{
    const uint64_t sec = ft / 10000000ull;
    return fmt("%llu:%02llu:%02llu", (unsigned long long)(sec / 3600), (unsigned long long)(sec / 60 % 60),
               (unsigned long long)(sec % 60));
}

std::string mmss(double secs)
{
    if (secs < 0) secs = 0;
    const int s = int(std::ceil(secs));
    return s >= 60 ? fmt("%d:%02d", s / 60, s % 60) : fmt("%d s", s);
}

void potion_combo(const char* id, const WorldModel& w, const GameData& gd, bool mp, uint32_t& sel)
{
    const char* autoName = "auto - the smallest that fills the gap";
    const std::string cur = sel ? gd.item_name(sel) : std::string(autoName);
    if (ImGui::BeginCombo(id, cur.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(autoName, sel == 0)) sel = 0;
        std::set<uint32_t> seen;
        for (const InvItem& it : w.inventory) {
            const ItemInfo* ii = gd.item(it.itemId);
            if (!ii || ii->type != 10 || (mp ? ii->mp : ii->hp) <= 0 || !usable_item(w, it)) continue;
            if (!seen.insert(it.itemId).second) continue;
            ImGui::PushID(int(it.itemId));
            const ImVec2 p = ImGui::GetCursorPos();
            if (ImGui::Selectable("##p", sel == it.itemId, 0, ImVec2(0, 24))) sel = it.itemId;
            ImGui::SetCursorPos(p);
            ui::icon(gfx::item(it.itemId), 24);
            ImGui::SameLine();
            ImGui::SetCursorPosY(p.y + (24 - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::Text("%s  (%s +%d)", gd.item_name(it.itemId).c_str(), mp ? "MP" : "HP", mp ? ii->mp : ii->hp);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

} // namespace

int NeoApp::bot_tab_of(const char* actionKey)
{
    const std::string k = actionKey;
    return k == "heal" || k == "buffs" ? BT_HealBuffs : k == "loot" ? BT_Loot
         : k == "attack" ? BT_Attack : k == "drill" ? BT_Drill : k == "quest" ? BT_Quest : k == "keyq" ? BT_KeyQuest
         : k == "story" ? BT_Story : k == "hunt" ? BT_Hunt
         : k == "roam" || k == "town" ? BT_Travel : BT_Overview;
}

std::string NeoApp::bot_profile_path(const Session& s) const
{
    const SelfState& me = s.world->self;
    if (me.charUid) return exe_dir() + fmt("profiles\\bot_%016llX.ini", (unsigned long long)me.charUid);
    return std::string();
}

void NeoApp::bot_load_profile(Session& s, bool quiet)
{
    const std::string path = bot_profile_path(s);
    if (path.empty()) { if (!quiet) botStatus_ = "character not known yet"; return; }
    const bool other = s.botProfileFor != s.world->self.charUid;
    s.botProfileFor = s.world->self.charUid;
    if (other) bot_load_book(s);
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (!quiet) botStatus_ = "no profile saved for this character"; return; }
    std::stringstream ss;
    ss << in.rdbuf();
    s.bot.load_text(ss.str());
    botStatus_ = "profile loaded";
}

std::string NeoApp::bot_book_path(const Session& s) const
{
    const SelfState& me = s.world->self;
    if (me.charUid) return exe_dir() + fmt("profiles\\quests_%016llX.txt", (unsigned long long)me.charUid);
    return std::string();
}

void NeoApp::bot_load_book(Session& s)
{
    s.bot.book.clear();
    const std::string path = bot_book_path(s);
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        s.bot.book.load(ss.str());
    }
    s.bookSaved = s.bot.book.version();
}

void NeoApp::bot_save_book(Session& s)
{
    s.bookSaved = s.bot.book.version();
    const std::string path = bot_book_path(s);
    if (path.empty() || s.bot.book.h.empty()) return;
    CreateDirectoryA((exe_dir() + "profiles").c_str(), nullptr);
    std::ofstream out(path, std::ios::binary);
    out << "# " << s.world->self.name << ": what the server told of each quest's history (q<id> / m<id>:done at least:at most, -1 = ?)\n"
        << s.bot.book.text();
}

void NeoApp::bot_save_profile(Session& s)
{
    const std::string path = bot_profile_path(s);
    if (path.empty()) { botStatus_ = "character not known yet"; return; }
    CreateDirectoryA((exe_dir() + "profiles").c_str(), nullptr);
    std::ofstream out(path, std::ios::binary);
    out << "# " << s.world->self.name << "\n" << s.bot.save_text();
    botStatus_ = out ? "profile saved" : "could not write " + path;
}

void NeoApp::draw_bot()
{
    Session* s = current();
    if (!s) { draw_home(); return; }
    const WorldModel& w = *s->world;
    if (w.self.charUid && s->botProfileFor != w.self.charUid) bot_load_profile(*s, true);
    draw_bot_toolbar(*s);
    ImGui::Dummy(ImVec2(0, 6));
    switch (botTab_) {
    case BT_Overview:  bot_overview(*s); break;
    case BT_HealBuffs: bot_heal_buffs(*s); break;
    case BT_Loot:      bot_loot(*s); break;
    case BT_Attack:    bot_attack(*s); break;
    case BT_Drill:     bot_drill(*s); break;
    case BT_Quest:     bot_quest(*s); break;
    case BT_KeyQuest:  bot_keyquest(*s); break;
    case BT_Story:     bot_story(*s); break;
    case BT_Hunt:      bot_hunt(*s); break;
    case BT_Travel:    bot_roam_travel(*s); break;
    case BT_Supply:    bot_supply(*s); break;
    case BT_Sell:      bot_sell(*s); break;
    case BT_Storage:   bot_storage(*s); break;
    }
    s->bot.sync_modes();                       // a switch flipped on this page: Drill / Attack / Quest / Item Hunt
}

// ── toolbar: state, what it does, Run, profile, Start / Stop ────────────────
void NeoApp::draw_bot_toolbar(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    const uint64_t now = now_filetime();
    const bool live = s.state == Session::State::Live && w.inWorld;
    if (bot.running) ui::pill("RUNNING", ui::kStatusRun);
    else if (live) ui::pill("READY", ui::kStatusLive);
    else ui::pill("OFFLINE", ui::kMuted);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const std::string doing = bot.running ? bot.doing : live ? "in the world - press Start" : "needs a live client in the world";
    const float rightW = 420.0f;
    {
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionMax().x - ImGui::GetCursorPosX() - rightW;
        ImGui::PushClipRect(sp, ImVec2(sp.x + std::max(0.0f, avail), sp.y + 30), true);
        ImGui::TextColored(ui::kMuted, "%s", doing.c_str());
        ImGui::PopClipRect();
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - rightW));
    toggle_row("Run", &bot.run);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The move type of every request: run (2) or walk (1, half the speed).");
    ImGui::SameLine(0, 14);
    if (ImGui::Button("Save profile")) bot_save_profile(s);
    if (ImGui::IsItemHovered() && !botStatus_.empty()) ImGui::SetTooltip("%s", botStatus_.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Load")) bot_load_profile(s, false);
    ImGui::SameLine(0, 14);
    if (bot.running) {
        if (ui::button_primary("Stop", ImVec2(100, 0), ui::kErr)) bot.stop_by_user("by you");
    } else {
        ImGui::BeginDisabled(!live || w.self.dead);
        if (ui::button_primary("Start", ImVec2(100, 0))) bot.start(w, now);
        ImGui::EndDisabled();
    }
    ImGui::Separator();
}

// ── Overview ────────────────────────────────────────────────────────────────
void NeoApp::bot_overview(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    const uint64_t now = now_filetime();
    const bool ran = bot.startedAt != 0;
    const uint64_t up = ran && bot.running && now > bot.startedAt ? now - bot.startedAt : 0;
    const double hours = double(up) / 36e9;
    ui::stat("KILLS", std::to_string(bot.attack.kills).c_str(), 120);
    ImGui::SameLine();
    ui::stat("EXP", ran ? thousands(w.self.expGained - bot.expAtStart).c_str() : "0", 150);
    ImGui::SameLine();
    ui::stat("GALDER", ran ? thousands(w.self.money - bot.moneyAtStart).c_str() : "0", 150);
    ImGui::SameLine();
    ui::stat("KILLS / H", hours > 0.01 ? fmt("%.0f", bot.attack.kills / hours).c_str() : "-", 120);
    ImGui::SameLine();
    ui::stat("TIME", duration(up).c_str(), 120);

    // the target
    section_title("Target");
    if (bot.attack.target && w.entities.count(bot.attack.target)) {
        const Entity& t = w.entities.at(bot.attack.target);
        int px = 0, py = 0;
        w.self_pos_at(now, px, py);
        ImGui::Text("%s", w.entity_label(t.eid).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, "Lv %u  -  %.0f away", t.level, std::hypot(double(t.x - px), double(t.y - py)));
        const int64_t mx = t.maxHp > 0 ? t.maxHp : std::max<int64_t>(t.hp, 1);
        ui::bar(t.hp >= 0 ? float(t.hp) / float(mx) : 1.0f, ImVec4(0.75f, 0.22f, 0.25f, 1), 320,
                t.hp >= 0 ? fmt("%lld / %lld", (long long)t.hp, (long long)mx).c_str() : "HP ?");
    } else {
        ImGui::TextColored(ui::kMuted, "none");
    }

    // where the server has us against where the client draws us (StuckGuard, bot/stuck.cpp)
    section_title("Position");
    {
        int px = 0, py = 0;
        w.self_pos_at(now, px, py);
        ImGui::Text("server (%d,%d)", px, py);
        ImGui::SameLine(0, 18);
        if (w.drawn_live(now)) {
            const double off = std::hypot(double(w.self.drawn.x - px), double(w.self.drawn.y - py));
            ImGui::Text("drawn (%d,%d)", w.self.drawn.x, w.self.drawn.y);
            ImGui::SameLine(0, 18);
            ImGui::TextColored(off >= 48.0 ? ui::kWarn : ui::kMuted, "%.0f apart", off);
        } else {
            ImGui::TextColored(ui::kMuted, "drawn: not read (an older hook DLL, or no avatar yet)");
        }
        ImGui::SameLine(0, 18);
        const StuckGuard::Event& se = bot.guard.last();
        if (bot.guard.pausedUntil > now)
            ImGui::TextColored(ui::kWarn, "stuck guard paused %.0f s (stuck 4 times in a minute)", double(bot.guard.pausedUntil - now) / 1e7);
        else if (se.at)
            ImGui::TextColored(ui::kMuted, "unstuck %d x, the last %.0f s ago at (%d,%d)", bot.guard.count,
                               now > se.at ? double(now - se.at) / 1e7 : 0.0, se.drawnX, se.drawnY);
        else
            ImGui::TextColored(ui::kMuted, "never stuck");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The server walks every chase and pick-up in a straight line, through walls; the client\n"
                              "path-finds it and leaves the character standing when it finds no way round. Drawn 48+\n"
                              "units off while standing still = stuck: the bot sends the client's own 'stand here'\n"
                              "(C2S 0x18 from the drawn spot), which puts the server back there, and leaves that\n"
                              "target / item / cell for a while. Targets and items are also chosen by the walk on the\n"
                              "map's grid, and one behind a wall is reached by walking round it first.");
    }

    // the actions, in order
    section_title("Actions  -  in priority order: the first one with something to do acts");
    if (begin_table("acts", 4)) {
        ImGui::TableSetupColumn("#", 0, 24);
        ImGui::TableSetupColumn("On", 0, 50);
        ImGui::TableSetupColumn("Action", 0, 120);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        int i = 0;
        for (BotAction* a : bot.actions()) {
            ImGui::TableNextRow();
            ImGui::PushID(a->key());
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextColored(ui::kMuted, "%d", i + 1);
            ImGui::TableNextColumn(); ui::toggle("##on", &a->enabled);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(a->title(), false, 0, ImVec2(0, ImGui::GetFrameHeight()))) botTab_ = bot_tab_of(a->key());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ui::kMuted, "%s", !a->enabled ? "off" : bot.running ? a->status.c_str() : "");
            ImGui::PopID();
            ++i;
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(ui::kMuted, "start point (%d, %d)", bot.anchorX, bot.anchorY);
    ImGui::SameLine();
    if (ImGui::SmallButton("here")) w.self_pos_at(now, bot.anchorX, bot.anchorY);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The centre of \"stay\" / \"around start\" (set at Start too).");
}

// ── Heal ────────────────────────────────────────────────────────────────────
void NeoApp::bot_heal(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    HealAction& h = bot.heal;
    const uint64_t now = now_filetime();
    ui::page_header("Heal", bot.running && h.enabled ? h.status.c_str() : "potions (C2S 0x57) and heal skills", &h.enabled);
    ImGui::Dummy(ImVec2(0, 4));
    toggle_row("HP potions", &h.hpPotions);
    ImGui::SameLine(150);
    ImGui::BeginDisabled(!h.hpPotions);
    slider("##hp", &h.hpPct, 5, 95, "HP below %d%%");
    ImGui::SameLine(); ImGui::SetNextItemWidth(320); potion_combo("##hpi", w, gameData_, false, h.hpItem);
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "MP potions");
    ImGui::SameLine(150);
    slider("##mp", &h.mpPct, 5, 95, "MP below %d%%");
    ImGui::SameLine(); ImGui::SetNextItemWidth(320); potion_combo("##mpi", w, gameData_, true, h.mpItem);

    // the heal skills: the checked ones in their order (the first ready one is cast), then the other learned ones
    section_title("Heal skills");
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "  check the skills that heal you; the first ready one goes first, the arrows set the order");
    slider("##skp", &h.skillPct, 5, 99, "cast at or below %d%% HP", 220);
    struct Row { uint16_t skill; int idx; };            // idx in h.skills, -1 = not checked
    std::vector<Row> rows;
    for (size_t i = 0; i < h.skills.size(); ++i) rows.push_back({ h.skills[i], int(i) });
    for (const SkillEntry& sk : w.skills)
        if (HealAction::heal_skill(&gameData_, sk.id) && usable_skill(w, sk) && !h.uses(sk.id)) rows.push_back({ sk.id, -1 });
    int move = -1, dir = 0, del = -1;
    uint16_t add = 0, drop = 0;
    if (rows.empty()) {
        ImGui::TextColored(ui::kMuted, "no heal skill learned (Cure, Catastrophe Healing, Heart's Grace, Chakra Balance)");
    } else if (begin_table("healsk", 8)) {
        ImGui::TableSetupColumn("#", 0, 22);
        ImGui::TableSetupColumn("Use", 0, 34);
        ImGui::TableSetupColumn("Skill", 0, 250);
        ImGui::TableSetupColumn("MP", 0, 50);
        ImGui::TableSetupColumn("Cool time", 0, 72);
        ImGui::TableSetupColumn("Heals", 0, 300);
        ImGui::TableSetupColumn("State", 0, 170);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        BotCtx ctx{ w, now, 0, 0, false, bot, Action{} };
        w.self_pos_at(now, ctx.px, ctx.py);
        for (const Row& row : rows) {
            const bool on = row.idx >= 0;
            const int lv = w.skill_level(row.skill);
            const SkillLevelInfo* li = gameData_.skill_level(row.skill, lv);
            const float midY = (kIcon - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::TableNextRow();
            ImGui::PushID(int(row.skill));
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (on) ImGui::Text("%d", row.idx + 1); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            bool use = on;
            if (ImGui::Checkbox("##use", &use)) (use ? add : drop) = row.skill;
            ImGui::TableNextColumn();
            const uint64_t ready = w.skill_ready_at(row.skill);
            const double cdTotal = li ? std::max(0.5, double(li->coolTime) - 0.5) : 1.0;
            const double cdLeft = ready > now ? double(ready - now) / 1e7 : 0.0;
            const float rowY = ImGui::GetCursorPosY();
            ui::icon_cool(gfx::skill(row.skill), kIcon, float(cdLeft / cdTotal), cdLeft > 0 ? fmt("%.0f", std::ceil(cdLeft)).c_str() : "");
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + midY);
            ImGui::TextColored(lv && on ? ui::kText : ui::kMuted, "%s", gameData_.skill_name(row.skill).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + midY);
            ImGui::TextColored(ui::kMuted, "%s", lv ? fmt("Lv %d", lv).c_str() : "not learned");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (li && li->mpCon > 0) ImGui::Text("%d", li->mpCon); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (li && li->coolTime > 0) ImGui::Text("%.1f s", li->coolTime); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            ImGui::TextColored(ui::kMuted, "%s", HealAction::heal_kind(&gameData_, row.skill));
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            std::string why;
            if (bot.gate.pending() == row.skill) ImGui::TextColored(ui::kAccent, "casting...");
            else if (!on) ImGui::TextColored(ui::kMuted, "off");
            else if (skill_ready(ctx, row.skill, &why)) ImGui::TextColored(ui::kOk, "ready");
            else ImGui::TextColored(why.rfind("cool", 0) == 0 ? ui::kMuted : ui::kWarn, "%s", why.c_str());
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            if (on) {
                ImGui::BeginDisabled(row.idx == 0);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move = row.idx; dir = -1; }
                ImGui::EndDisabled();
                ImGui::SameLine(0, 2);
                ImGui::BeginDisabled(row.idx + 1 == (int)h.skills.size());
                if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) { move = row.idx; dir = 1; }
                ImGui::EndDisabled();
                if (!lv) {                                   // a skill this character does not have (an old profile)
                    ImGui::SameLine(0, 6);
                    if (ImGui::SmallButton("forget")) del = row.idx;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move >= 0) std::swap(h.skills[size_t(move)], h.skills[size_t(move + dir)]);
    if (del >= 0) h.skills.erase(h.skills.begin() + del);
    if (add) h.set_use(add, true);
    if (drop) h.set_use(drop, false);
    note("At or below the % the first ready checked skill is cast on you (one skill request at a time; none while a "
         "dig is open, a skill would end it); then nothing until your HP moves. The HP potion, when on, goes first below "
         "its own %: leave it lower as the emergency, or off for a class that heals itself (MP potions keep going). "
         "Chakra Balance heals over time and ends at the first swing at you, so it is cast only out of a fight.");

    section_title("Potions in the bag");
    std::map<uint32_t, int> count;
    for (const InvItem& it : w.inventory) {
        const ItemInfo* ii = gameData_.item(it.itemId);
        if (ii && ii->type == 10 && (ii->hp > 0 || ii->mp > 0) && usable_item(w, it)) count[it.itemId] += it.count;
    }
    if (count.empty()) { ImGui::TextColored(ui::kWarn, "none: the heal has nothing to use"); return; }
    if (begin_table("pots", 4)) {
        ImGui::TableSetupColumn("Item", 0, 320);
        ImGui::TableSetupColumn("Count", 0, 60);
        ImGui::TableSetupColumn("HP", 0, 60);
        ImGui::TableSetupColumn("MP", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& kv : count) {
            const ItemInfo* ii = gameData_.item(kv.first);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); icon_text(gfx::item(kv.first), gameData_.item_name(kv.first));
            ImGui::TableNextColumn(); mid(); ImGui::Text("%d", kv.second);
            ImGui::TableNextColumn(); mid(); if (ii->hp > 0) ImGui::Text("+%d", ii->hp);
            ImGui::TableNextColumn(); mid(); if (ii->mp > 0) ImGui::Text("+%d", ii->mp);
        }
        ImGui::EndTable();
    }
}

// ── Heal & Buffs: the two actions on one page ────────────────────────────────
void NeoApp::bot_heal_buffs(Session& s)
{
    ImGui::PushID("heal");
    bot_heal(s);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushID("buffs");
    bot_buffs(s);
    ImGui::PopID();
}

// ── Roam & Travel: the Roam, the Town (death, the way back), the map graph ──
void NeoApp::bot_roam_travel(Session& s)
{
    ImGui::PushID("roam");
    bot_roam(s);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushID("town");
    bot_town(s);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 14));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::PushID("travel");
    bot_travel(s);
    ImGui::PopID();
}

// ── Buffs: every buff skill + the effects on us ─────────────────────────────
void NeoApp::bot_buffs(Session& s)
{
    const WorldModel& w = *s.world;
    BuffAction& b = s.bot.buffs;
    const uint64_t now = now_filetime();
    ui::page_header("Buffs", s.bot.running && b.enabled ? b.status.c_str() : "keep the checked ones up", &b.enabled);
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("##before", &b.beforeSec, 0, 30, "recast %d s before the end");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt("##untracked", &b.untrackedSec, 10, 600, "no effect seen: every %d s");
    ui::help("Every effect on you is tracked from the packets: S2C 224 starts it, 280 ends it, 850 says 10 s are "
             "left; its length is the skill's Seconds. A checked buff is cast on you again when it is gone or "
             "about to end, as soon as its cool time and MP allow. A cast that fires but never shows an effect "
             "is cast again after its Seconds (or the second value).");

    // the buff skills: learned Type 3, plus anything checked or up on us
    std::vector<SkillEntry> rows;
    for (const SkillEntry& sk : w.skills) {
        const SkillInfo* si = gameData_.skill(sk.id);
        if (!si || !usable_skill(w, sk)) continue;
        if (si->buff() || ((b.kept(sk.id) || w.effect_of(sk.id, now)) && !(si->affectionRel & 32))) rows.push_back(sk);
    }
    section_title("Buff skills");
    if (rows.empty()) ImGui::TextColored(ui::kMuted, "no buff skill learned");
    else if (begin_table("buffs", 6)) {
        ImGui::TableSetupColumn("Keep up", 0, 60);
        ImGui::TableSetupColumn("Skill", 0, 260);
        ImGui::TableSetupColumn("Effect", 0, 220);
        ImGui::TableSetupColumn("Lasts", 0, 60);
        ImGui::TableSetupColumn("Cool time", 0, 80);
        ImGui::TableSetupColumn("MP", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        BotCtx ctx{ w, now, 0, 0, false, s.bot, Action{} };
        for (const SkillEntry& sk : rows) {
            const SkillLevelInfo* li = gameData_.skill_level(sk.id, sk.level);
            const Effect* e = w.effect_of(sk.id, now);
            ImGui::TableNextRow();
            ImGui::PushID(sk.id);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            bool k = b.kept(sk.id);
            if (ImGui::Checkbox("##keep", &k)) {
                b.set_kept(sk.id, k);
                if (k) b.enabled = true;            // checking one turns the action on
            }
            ImGui::TableNextColumn();
            const uint64_t ready = w.skill_ready_at(sk.id);
            const double cdTotal = li ? std::max(0.5, double(li->coolTime) - 0.5) : 1.0;
            const double cdLeft = ready > now ? double(ready - now) / 1e7 : 0.0;
            const float rowY = ImGui::GetCursorPosY();
            ui::icon_cool(gfx::skill(sk.id), kIcon, float(cdLeft / cdTotal), cdLeft > 0 ? fmt("%.0f", std::ceil(cdLeft)).c_str() : "");
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::Text("%s", gameData_.skill_name(sk.id).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(ui::kMuted, "Lv %u", sk.level);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - 16) * 0.5f);
            if (e && e->endAt) {
                const double left = double(e->endAt > now ? e->endAt - now : 0) / 1e7;
                const double life = std::max(1.0, double(e->endAt - e->startAt) / 1e7);
                ui::bar(float(left / life), e->ending ? ui::kWarn : ui::kOk, 200, mmss(left).c_str());
            } else if (e) {
                ui::bar(1.0f, ui::kOk, 200, "up");
            } else {
                std::string why;
                const bool r = skill_ready(ctx, sk.id, &why);
                ImGui::TextColored(k ? (r ? ui::kAccent : ui::kWarn) : ui::kMuted, "%s", !k ? "off" : r ? "off - will cast" : why.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            if (li && li->seconds > 0) ImGui::Text("%s", mmss(li->seconds).c_str()); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            if (li && li->coolTime > 0) ImGui::Text("%.1f s", li->coolTime); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            if (li && li->mpCon > 0) ImGui::Text("%d", li->mpCon); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    section_title("Effects on you");
    int shown = 0;
    if (begin_table("effects", 4)) {
        ImGui::TableSetupColumn("Effect", 0, 260);
        ImGui::TableSetupColumn("From", 0, 160);
        ImGui::TableSetupColumn("Left", 0, 220);
        ImGui::TableSetupColumn("ES", ImGuiTableColumnFlags_WidthStretch);
        for (const Effect& e : w.effects) {
            if (!WorldModel::effect_up(e, now)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); icon_text(gfx::buff(e.skill), gameData_.skill_name(e.skill));
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(ui::kMuted, "%s", e.caster == w.self.eid ? "you" : w.entity_label(e.caster).c_str());
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - 16) * 0.5f);
            if (e.endAt) {
                const double left = double(e.endAt > now ? e.endAt - now : 0) / 1e7;
                const double life = std::max(1.0, double(e.endAt - e.startAt) / 1e7);
                ui::bar(float(left / life), e.ending ? ui::kWarn : ui::kOk, 200, mmss(left).c_str());
            } else {
                ImGui::TextColored(ui::kMuted, "until it ends");
            }
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextColored(ui::kMuted, "ESAction %u  class %u%s", e.esAction, e.esClass, e.esId ? "" : "  (id lost in a zone change)");
            ++shown;
        }
        ImGui::EndTable();
    }
    if (!shown) ImGui::TextColored(ui::kMuted, "none");
}

// ── Loot ────────────────────────────────────────────────────────────────────
void NeoApp::bot_loot(Session& s)
{
    const WorldModel& w = *s.world;
    LootAction& l = s.bot.loot;
    const uint64_t now = now_filetime();
    const bool eye = w.third_eye(now);
    ui::page_header("Loot", s.bot.running && l.enabled ? l.status.c_str() : "pick up (C2S 0x21)", &l.enabled);
    ImGui::Dummy(ImVec2(0, 4));
    slider("##r", &l.radius, 50, 1500, "radius %d");
    ImGui::SameLine(); toggle_row("galder", &l.galder);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Galder piles (ItemParam Type 30): on their own switch, apart from the lists.");
    ImGui::SameLine(); toggle_row("free items too", &l.othersItems);
    slider("##heavy", &l.heavyPct, 0, 100, l.heavyPct ? "from %d %% of the weight: only quest / hunt items" : "no weight limit", 320);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When the bag weighs this share of your max weight (MaxWT) or more, only the items the Key Quest / Story "
                          "Quest / Item Hunt need (and galder) are picked up. 0 = everything the lists allow, whatever the weight.");
    ImGui::SameLine(); ImGui::TextColored(ui::kMuted, "bag now: %s", w.weight_pct() >= 0 ? fmt("%d %%", w.weight_pct()).c_str() : "?");
    note("Your kills' drops are picked up between two targets, even with monsters around. Free items (someone else's "
         "after their 30 s, items that were already there) only out of a fight. The server refuses someone else's "
         "drop without a reply, so an item still on the ground after the walk is left alone for 60 s. A shadow "
         "monster's drop needs the Third Eye.");

    // the nearby ground, by distance
    int px = 0, py = 0;
    w.self_pos_at(now, px, py);
    struct Row { const GroundItem* g; double d; };
    std::vector<Row> rows;
    for (const auto& kv : w.groundItems) rows.push_back({ &kv.second, std::hypot(double(kv.second.x - px), double(kv.second.y - py)) });
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });

    section_title("On the ground nearby");
    if (rows.empty()) ImGui::TextColored(ui::kMuted, "nothing");
    else if (begin_table("ground", 5)) {
        ImGui::TableSetupColumn("Item", 0, 300);
        ImGui::TableSetupColumn("Count", 0, 56);
        ImGui::TableSetupColumn("Away", 0, 56);
        ImGui::TableSetupColumn("Whose", 0, 160);
        ImGui::TableSetupColumn("Taken?", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int n = 0;
        for (const Row& r : rows) {
            if (++n > 14) break;
            const GroundItem& g = *r.g;
            const ItemInfo* ii = gameData_.item(g.itemId);
            const bool galder = ii && ii->type == 30;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            icon_text(gfx::item(g.itemId), gameData_.item_name(g.itemId), r.d <= l.radius ? ui::kText : ui::kMuted);
            ImGui::TableNextColumn(); mid(); ImGui::Text("%u", g.count);
            ImGui::TableNextColumn(); mid(); ImGui::Text("%.0f", r.d);
            ImGui::TableNextColumn(); mid();
            const bool ours = g.fromDrop && g.owner == w.self.eid;
            const bool fresh = g.fromDrop && now < g.seenAt + 300000000ull;
            ImGui::TextColored(ours ? ui::kOk : g.fromDrop && g.owner && fresh ? ui::kWarn : ui::kMuted, "%s",
                               ours ? "your kill" : !g.fromDrop ? "was there" : !g.owner ? "free"
                               : fresh ? fmt("%s's", w.entity_label(g.owner).c_str()).c_str() : "free (30 s passed)");
            ImGui::TableNextColumn(); mid();
            const int tier = gameData_.equip_tier(g.itemId);
            const std::string gear = fmt("equipment (%s): not looted", GameData::equip_tier_name(tier));
            const char* why = g.eventFlag && !eye ? "shadow: needs the Third Eye"
                            : galder ? (l.galder ? nullptr : "galder off")
                            : !l.equip_allowed(&gameData_, g.itemId) ? gear.c_str()
                            : !l.items.allows(g.itemId) ? (l.items.mode == IdFilter::OnlyWhite ? "not in the whitelist" : "blacklisted")
                            : r.d > l.radius ? "out of the radius" : nullptr;
            ImGui::TextColored(why ? ui::kMuted : ui::kOk, "%s", why ? why : "yes");
        }
        ImGui::EndTable();
    }

    // the common gear grades (heavy): left on the ground unless chosen
    section_title("Equipment");
    toggle_row("Don't loot equipment (Old / Strong / Art / Ult.)", &l.skipEquip);
    if (!l.skipEquip) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, "loot these:");
        for (int t = 0; t < ET_Count; ++t) {
            ImGui::SameLine(0, 14);
            ImGui::Checkbox(fmt("%s##loottier", GameData::equip_tier_name(t)).c_str(), &l.lootTier[t]);
        }
    }
    note("Weapons, helmets and shields named Old, Strong (a sword: Sharp), Art or Ult. - the ones monsters drop and maps "
         "bury by the hundred, and they weigh a lot. Chaos, Morph and the named series follow the Items list below. An "
         "item whitelisted by its id is picked up anyway, and so are the Item Hunt's / Key Quest's items. The Drill's own "
         "finds follow this too. Selling them: Auto-Sell tab.");

    // the black / whitelist: always the last thing of the page
    section_title("Items");
    {
        std::vector<uint32_t> nearby;               // the ground first, then the bag (not galder)
        auto add = [&](uint32_t id) {
            const ItemInfo* ii = gameData_.item(id);
            if (ii && ii->type == 30) return;
            if (std::find(nearby.begin(), nearby.end(), id) == nearby.end()) nearby.push_back(id);
        };
        for (const Row& r : rows) add(r.g->itemId);
        for (const InvItem& it : w.inventory) add(it.itemId);
        ImGui::PushID("items");
        draw_id_list(0, l.items, nearby);
        ImGui::PopID();
    }
}

// ── Attack: target, basic attack, the skill rotation, the monster list ──────
void NeoApp::bot_attack(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    AttackAction& at = bot.attack;
    const uint64_t now = now_filetime();
    ui::page_header("Attack", bot.running && at.enabled ? at.status.c_str() : "hunt monsters", &at.enabled);
    ImGui::Dummy(ImVec2(0, 4));

    section_title("Target");
    slider("##r", &at.radius, 100, 3000, "radius %d");
    ImGui::SameLine(); toggle_row("leave other players' monsters", &at.skipOwned);
    ImGui::SameLine(); toggle_row("fight back whatever hits you", &at.fightBack);
    note("Whoever hits you first, else the nearest monster within the radius (around you when roaming, around the "
         "start point otherwise) that the monster list at the bottom allows. A target whose HP has not dropped for "
         "12 s is left.");
    {
        const bool eye = w.third_eye(now);
        const Effect* e = nullptr;
        for (const Effect& x : w.effects)
            if (x.esAction == 1390 && WorldModel::effect_up(x, now)) e = &x;
        ui::icon(gfx::buff(8001), 22);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (eye) ImGui::TextColored(ui::kOk, "Third Eye up%s", e && e->endAt ? fmt("  -  %s left", mmss(double(e->endAt > now ? e->endAt - now : 0) / 1e7).c_str()).c_str() : "");
        else ImGui::TextColored(ui::kMuted, "Third Eye off");
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, " -  shadow monsters (16000..16999) are hunted only with it: without it the server "
                                       "ignores the attack. Keep Mind's Eye checked in Buffs.");
    }

    section_title("Basic attack");
    toggle_row("use the weapon's basic attack", &at.basic);
    ImGui::SameLine(0, 20);
    uint32_t gunId = 0;
    const GunInfo* gun = equipped_gun(w, &gunId);
    if (!at.basic) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, "off: skills only (the bot keeps within the skills' reach)");
    } else if (gunId) {
        const int loaded = loaded_bullets(w);
        const bool spare = spare_bullets(w) != 0;
        const ImVec4 c = !gun || at.gunOff ? ui::kErr : loaded || spare ? ui::kOk : ui::kWarn;
        ui::icon(gfx::item(gunId), 24);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(c, "%s", gameData_.item_name(gunId).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ui::kMuted, "%s", !gun ? "no gun data"
                           : at.gunOff ? "off for this run (a shot got no answer)"
                           : loaded ? fmt("%d bullets loaded%s, %d ms between shots", loaded, spare ? " + spare" : "",
                                          gun_shot_interval_ms(*gun)).c_str()
                           : spare ? "bullets in the bag (will load them)" : "no bullets: cannot attack");
        ImGui::SameLine(); toggle_row("shoot", &at.useGun);
    } else {
        if (w.self.equip[1]) if (const InvItem* it = w.find_item(w.self.equip[1])) { ui::icon(gfx::item(it->itemId), 24); ImGui::SameLine(); }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, "melee (C2S 0x1D): the server chases and swings until the target dies");
    }

    // the rotation: every attack skill learned (as Buffs lists every buff). The rotation's skills come
    // first in their order (checked = cast); the others under them, unchecked: checking one puts it at
    // the end of the rotation. The first ready checked skill is cast: the order is the priority.
    section_title("Skill rotation");
    ImGui::SameLine();
    ImGui::TextColored(ui::kMuted, "  check the skills to cast; the first ready one goes first, the arrows set the order");
    struct Row { uint16_t skill; int rot; };            // rot = index in at.rotation, -1 = not in it
    std::vector<Row> rows;
    for (size_t i = 0; i < at.rotation.size(); ++i) rows.push_back({ at.rotation[i].skill, int(i) });
    for (const SkillEntry& sk : w.skills) {
        const SkillInfo* si = gameData_.skill(sk.id);
        if (!si || si->buff() || !usable_skill(w, sk) || HealAction::heal_skill(&gameData_, sk.id)) continue;   // (heals: the Heal part)
        bool in = false;
        for (const RotationSkill& r : at.rotation) in = in || r.skill == sk.id;
        if (!in) rows.push_back({ sk.id, -1 });
    }
    int move = -1, dir = 0, del = -1;
    uint16_t addSkill = 0;
    if (rows.empty()) ImGui::TextColored(ui::kMuted, "no attack skill learned");
    else if (begin_table("rot", 9)) {
        ImGui::TableSetupColumn("#", 0, 22);
        ImGui::TableSetupColumn("Use", 0, 34);
        ImGui::TableSetupColumn("Skill", 0, 250);
        ImGui::TableSetupColumn("MP", 0, 50);
        ImGui::TableSetupColumn("Reach", 0, 70);
        ImGui::TableSetupColumn("Area", 0, 210);
        ImGui::TableSetupColumn("Cool time", 0, 72);
        ImGui::TableSetupColumn("State", 0, 200);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        BotCtx ctx{ w, now, 0, 0, false, bot, Action{} };
        w.self_pos_at(now, ctx.px, ctx.py);
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& row = rows[i];
            RotationSkill* r = row.rot >= 0 ? &at.rotation[size_t(row.rot)] : nullptr;
            const bool on = r && r->on;
            const int lv = w.skill_level(row.skill);
            const SkillInfo* si = gameData_.skill(row.skill);
            const SkillLevelInfo* li = gameData_.skill_level(row.skill, lv);
            const float midY = (kIcon - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::TableNextRow();
            ImGui::PushID(int(row.skill));
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (r) ImGui::TextColored(on ? ui::kText : ui::kMuted, "%d", row.rot + 1);
            else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            bool use = on;
            if (ImGui::Checkbox("##use", &use)) {
                if (r) r->on = use;
                else if (use) addSkill = row.skill;
                if (use && !bot.drill.enabled) at.enabled = true;   // checking one turns the action on (not in drill mode)
            }
            ImGui::TableNextColumn();
            const uint64_t ready = w.skill_ready_at(row.skill);
            const double cdTotal = li ? std::max(0.5, double(li->coolTime) - 0.5) : 1.0;
            const double cdLeft = ready > now ? double(ready - now) / 1e7 : 0.0;
            const float rowY = ImGui::GetCursorPosY();
            ui::icon_cool(gfx::skill(row.skill), kIcon, float(cdLeft / cdTotal), cdLeft > 0 ? fmt("%.0f", std::ceil(cdLeft)).c_str() : "");
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + midY);
            ImGui::TextColored(lv && on ? ui::kText : ui::kMuted, "%s", gameData_.skill_name(row.skill).c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowY + midY);
            ImGui::TextColored(ui::kMuted, "%s", lv ? fmt("Lv %d", lv).c_str() : "not learned");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (li && li->mpCon > 0) ImGui::Text("%d", li->mpCon); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (si && si->esaic == 2) ImGui::TextColored(ui::kMuted, "chase");
            else if (si && si->esaui == 0) ImGui::Text("%d (you)", li && li->range > 0 ? li->range : 150);
            else if (li && li->range > 0) ImGui::Text("%d", li->range);
            else ImGui::TextColored(ui::kMuted, "-");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("chase = the server walks into range by itself (ESAIC 2); a number = the bot walks within "
                                  "it first; (you) = around you");
            ImGui::TableNextColumn();
            {
                const float radius = AttackAction::area_radius(w, row.skill);
                if (radius > 0.0f) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("r %.0f", radius);
                    if (r) {
                        ImGui::SameLine(0, 8);
                        ImGui::TextColored(ui::kMuted, "min");
                        ImGui::SameLine(0, 4);
                        ImGui::SetNextItemWidth(96);
                        int mt = std::max(1, r->minTargets);
                        if (ImGui::InputInt("##min", &mt, 1, 1)) r->minTargets = std::clamp(mt, 1, 20);
                        ImGui::SameLine(0, 4);
                        ImGui::TextColored(ui::kMuted, "mobs");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("An area skill: aimed where it takes the most monsters, and cast only when at least "
                                              "this many stand in its area (now: %d where it would go).",
                                              bot.attack.area_spot(ctx, row.skill).count);
                    }
                } else {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
                    ImGui::TextColored(ui::kMuted, "single");
                }
            }
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            if (li && li->coolTime > 0) ImGui::Text("%.1f s", li->coolTime); else ImGui::TextColored(ui::kMuted, "-");
            ImGui::TableNextColumn(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + midY);
            std::string why;
            if (bot.gate.pending() == row.skill) ImGui::TextColored(ui::kAccent, "casting...");
            else if (!on) ImGui::TextColored(ui::kMuted, "off");
            else if (skill_ready(ctx, row.skill, &why)) ImGui::TextColored(ui::kOk, "ready");
            else ImGui::TextColored(why.rfind("cool", 0) == 0 ? ui::kMuted : ui::kWarn, "%s", why.c_str());
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            if (r) {
                ImGui::BeginDisabled(row.rot == 0);
                if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move = row.rot; dir = -1; }
                ImGui::EndDisabled();
                ImGui::SameLine(0, 2);
                ImGui::BeginDisabled(row.rot + 1 == (int)at.rotation.size());
                if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) { move = row.rot; dir = 1; }
                ImGui::EndDisabled();
                if (!lv) {                               // a skill this character does not have (an old profile)
                    ImGui::SameLine(0, 6);
                    if (ImGui::SmallButton("forget")) del = row.rot;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (move >= 0) std::swap(at.rotation[size_t(move)], at.rotation[size_t(move + dir)]);
    if (del >= 0) at.rotation.erase(at.rotation.begin() + del);
    if (addSkill) at.rotation.push_back(RotationSkill{ addSkill, true, AttackAction::area_radius(w, addSkill) > 0.0f ? 2 : 0 });
    ui::help("Each skill's cool time starts when the server says it fired (S2C 240) and lasts its CoolTime at your "
             "level; one request at a time, its answer awaited (240 fired / 239 refused / 243 cancelled). A refused "
             "skill rests a few seconds. With the basic attack on, a skill out of reach is skipped while the swing "
             "closes in; with it off, the bot walks within reach first.");

    // the black / whitelist: always the last thing of the page
    section_title("Monsters");
    {
        int px = 0, py = 0;
        w.self_pos_at(now, px, py);
        std::vector<std::pair<double, uint32_t>> around;
        for (const auto& kv : w.entities)
            if (kv.second.kind == EK_MONSTER && !kv.second.dead)
                around.push_back({ std::hypot(double(kv.second.x - px), double(kv.second.y - py)), kv.second.species });
        std::sort(around.begin(), around.end());
        std::vector<uint32_t> nearby;
        for (auto& n : around)
            if (std::find(nearby.begin(), nearby.end(), n.second) == nearby.end()) nearby.push_back(n.second);
        ImGui::PushID("mobs");
        draw_id_list(1, at.mobs, nearby);
        ImGui::PopID();
    }
}

// ── a black / whitelist editor ──────────────────────────────────────────────
// kind 0 = items (with their icons), 1 = monster species. The mode, the two lists (the one in use
// bright), a name / id search over the game data and the ids nearby, each with + Black / + White.
void NeoApp::draw_id_list(int kind, IdFilter& f, const std::vector<uint32_t>& nearby)
{
    const bool items = kind != 1;
    auto name = [&](uint32_t id) { return items ? gameData_.item_name(id) : gameData_.mob_name(id); };
    auto entry = [&](uint32_t id, float h) {          // icon + name + id, vertically centred on h
        const float y = ImGui::GetCursorPosY();
        if (items) { ui::icon(gfx::item(id), h); ImGui::SameLine(); }
        ImGui::SetCursorPosY(y + (h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextUnformatted(name(id).c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosY(y + (h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextColored(ui::kMuted, "%u", id);
    };
    const char* what = items ? "item" : "monster";
    const char* kModes[] = { "Every one except the blacklist", "Only the whitelist" };
    ui::segmented("##mode", &f.mode, kModes, 2);
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, f.mode == IdFilter::OnlyWhite ? "only the whitelisted %ss" : "every %s but the blacklisted ones", what);

    const float h = items ? 24.0f : ImGui::GetTextLineHeight();
    // the two lists
    if (ImGui::BeginTable("lists", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        for (int li = 0; li < 2; ++li) {
            std::set<uint32_t>& set = li == 0 ? f.black : f.white;
            const bool inUse = (li == 0) == (f.mode == IdFilter::AllButBlack);
            ImGui::TableNextColumn();
            ImGui::PushID(li);
            ImGui::TextColored(inUse ? ui::kText : ui::kMuted, "%s (%zu)%s", li == 0 ? "Blacklist" : "Whitelist", set.size(),
                               inUse ? "" : "  -  not used in this mode");
            ImGui::BeginChild("##l", ImVec2(0, 120), ImGuiChildFlags_Borders);
            uint32_t drop = 0;
            for (uint32_t id : set) {
                ImGui::PushID(int(id));
                if (ImGui::SmallButton("x")) drop = id;
                ImGui::SameLine();
                entry(id, h);
                ImGui::PopID();
            }
            if (set.empty()) ImGui::TextColored(ui::kMuted, "empty");
            if (drop) set.erase(drop);
            ImGui::EndChild();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // add: search + nearby
    auto add_row = [&](uint32_t id) {
        ImGui::PushID(int(id));
        const bool inB = f.black.count(id) != 0, inW = f.white.count(id) != 0;
        ImGui::BeginDisabled(inB);
        if (ImGui::SmallButton("+ Black")) { f.black.insert(id); f.white.erase(id); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(inW);
        if (ImGui::SmallButton("+ White")) { f.white.insert(id); f.black.erase(id); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        entry(id, h);
        ImGui::PopID();
    };
    if (ImGui::BeginTable("adds", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##q", items ? "search the items by name or id..." : "search the monsters by name or id...",
                                 &listQuery_[kind]);
        if (listQuery_[kind] != listQueryFor_[kind]) {
            listQueryFor_[kind] = listQuery_[kind];
            listResults_[kind] = items ? gameData_.search_items(listQuery_[kind], 60) : gameData_.search_mobs(listQuery_[kind], 60);
        }
        ImGui::BeginChild("##res", ImVec2(0, 130), ImGuiChildFlags_Borders);
        if (listQuery_[kind].empty()) ImGui::TextColored(ui::kMuted, "type a name or an id");
        else if (listResults_[kind].empty()) ImGui::TextColored(ui::kMuted, "nothing found");
        for (uint32_t id : listResults_[kind]) add_row(id);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, items ? "Nearby (the ground, then your bag)" : "Nearby");
        ImGui::BeginChild("##near", ImVec2(0, 130), ImGuiChildFlags_Borders);
        if (nearby.empty()) ImGui::TextColored(ui::kMuted, "nothing around");
        for (uint32_t id : nearby) add_row(id);
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

// ── Roam ────────────────────────────────────────────────────────────────────
void NeoApp::bot_roam(Session& s)
{
    const WorldModel& w = *s.world;
    RoamAction& r = s.bot.roam;
    const uint64_t now = now_filetime();
    ui::page_header("Roam", s.bot.running && r.enabled ? r.status.c_str() : "walk when there is nothing to do", &r.enabled);
    ImGui::Dummy(ImVec2(0, 4));
    static const char* kModes[] = { "Stay", "Around start", "Whole map" };
    ui::segmented("##mode", &r.mode, kModes, 3);
    if (r.mode == RoamAction::Around) { ImGui::SameLine(0, 12); slider("##area", &r.areaRadius, 300, 6000, "radius %d", 180); }
    const MapGrid* map = gameData_.map(w.self.zone);
    if (r.mode != RoamAction::Stay && !map) {               // (the sweep needs the map: legs only without it)
        slider("##lmin", &r.legMin, 100, 1500, "legs from %d", 180);
        ImGui::SameLine(); slider("##lmax", &r.legMax, 150, 3000, "to %d", 180);
        r.legMax = std::max(r.legMax, r.legMin + 50);
    }
    ImGui::TextColored(ui::kMuted, "%s", map ? fmt("map %s  -  %d x %d cells, walls avoided", gameData_.map_name(w.self.zone).c_str(),
                                                   map->w, map->h).c_str()
                                             : "no map file for this zone: roaming walks blind");
    ImGui::TextColored(ui::kMuted, "start point (%d, %d)", s.bot.anchorX, s.bot.anchorY);
    ImGui::SameLine();
    if (ImGui::SmallButton("here")) w.self_pos_at(now, s.bot.anchorX, s.bot.anchorY);
    if (map && r.mode != RoamAction::Stay && r.sweepAreas > 0) {
        ui::bar(float(r.sweepDone) / float(r.sweepAreas), ui::kAccent, 260,
                r.sweepRound.n ? fmt("round %d: %d / %d areas", r.sweepRound.n, r.sweepDone, r.sweepAreas).c_str()
                               : fmt("%d areas", r.sweepAreas).c_str());
        if (r.sweepRound.n) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ui::kMuted, "%s", r.sweepRound.sense > 0 ? "clockwise" : "counter-clockwise");
        }
        if (r.active()) { ImGui::SameLine(0, 12); ImGui::TextColored(ui::kMuted, "%s (%d, %d)", r.goalWhy.c_str(), r.goalX, r.goalY); }
    } else if (r.active()) {
        ImGui::TextColored(ui::kMuted, "walking to (%d, %d)", r.destX, r.destY);
    }
    note("With the map file the roam is a sweep in rounds: the map in areas of 256 units, an area counts as seen when "
         "it comes within the Attack's radius of you, and a round goes over every area once. The next area is drawn "
         "among the nearest ones not seen this round, and each round turns the other way round the map (clockwise, "
         "then counter-clockwise), so the route changes from round to round. It walks the grid path without pauses; "
         "an area out of reach is left out for 10 minutes. The server shows every monster of the map, so a monster the "
         "Attack wants but out of its radius is walked to first.");

    section_title("Monster quest: spawn patrol");
    slider("##spause", &r.spotPause, 0, 30, r.spotPause ? "pause about %d s at each spot" : "no pause at the spots", 240);
    if (r.spotZone() == w.self.zone && !r.spots.empty()) {
        int done = 0;
        for (const RoamAction::Spot& sp : r.spots) done += sp.done;
        ImGui::TextColored(ui::kMuted, "%s in %s: %zu spots over %d spawn points", gameData_.mob_name(r.spotMob()).c_str(),
                           gameData_.map_name(r.spotZone()).c_str(), r.spots.size(), r.spotPoints);
        if (r.patrolRound.n) {
            ui::bar(float(done) / float(r.spots.size()), ui::kAccent, 260,
                    fmt("round %d: %d / %zu spots", r.patrolRound.n, done, r.spots.size()).c_str());
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ui::kMuted, "%s%s", r.patrolRound.sense > 0 ? "clockwise" : "counter-clockwise",
                               r.spotGoal() >= 0 ? fmt(", %s spot %d", r.spotPaused() ? "at" : "to", r.spotGoal() + 1).c_str() : "");
        }
    }
    note("In monster quest mode the Attack takes the quest's monster anywhere on the map. With none to take, the bot "
         "patrols the monster's spawn spots: its regeneration points in groups (the point that sees the most others "
         "within 400 units, then the same among the rest). It goes over them in rounds like the sweep, the next spot "
         "drawn among the nearest, each round the other way round, with a short pause at each; a spot passed on the "
         "way counts. This page's switch does not matter there.");
    ImGui::TextColored(ui::kMuted, "The map with the walls, the target, the roam point and the spots: Debug > World.");
}

} // namespace nj
