// The Bot view's trip pages (bot/act_town.cpp, bot/navigator.cpp): Town (death, the rest in town, the
// way back), Supply (the auto-shop: refill, sell, store) and Travel (the map graph, a trip to a map).
#include "app/neo_app.h"
#include "app/atlas.h"
#include "app/bot_ui.h"
#include "app/game_data.h"
#include "app/gfx.h"
#include "app/text_util.h"
#include "app/ui_theme.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace nj {
namespace {

using namespace bui;

std::string lower_s(std::string s)
{
    for (char& ch : s) ch = char(tolower((unsigned char)ch));
    return s;
}

bool matches(const std::string& q, const std::string& text, int id)
{
    return q.empty() || lower_s(text).find(q) != std::string::npos || std::to_string(id) == q;
}

// one line per hop: "1  portal (x, y)  ->  Map (id)"
void route_table(const Atlas& at, const Route& r, const char* id)
{
    if (!r.ok) { ImGui::TextColored(ui::kWarn, "%s", r.why.empty() ? "no route" : r.why.c_str()); return; }
    if (r.hops.empty()) { ImGui::TextColored(ui::kMuted, "already there"); return; }
    ImGui::TextColored(ui::kMuted, "%zu map change%s, about %.0f s%s", r.hops.size(), r.hops.size() == 1 ? "" : "s", r.secs,
                       r.gelder ? fmt(", %d galder", r.gelder).c_str() : "");
    if (!begin_table(id, 4)) return;
    ImGui::TableSetupColumn("#", 0, 24);
    ImGui::TableSetupColumn("From", 0, 260);
    ImGui::TableSetupColumn("By", 0, 260);
    ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int i = 0;
    for (const RouteHop& h : r.hops) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", ++i);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(at.map_label(h.fromZone).c_str());
        ImGui::TableNextColumn();
        if (h.kind == RouteHop::Portal) ImGui::Text("portal at (%d, %d)", h.x, h.y);
        else if (h.kind == RouteHop::Item) ImGui::Text(h.advIndex >= 0 ? "Portable Port AD (#%d)" : "port item %u",
                                                       h.advIndex >= 0 ? unsigned(h.advIndex) : h.item);
        else {
            const AtlasNpc* n = at.npc(h.npc);
            ImGui::Text("%s, %d galder", n ? n->name.c_str() : "teleport", h.cost);
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(at.map_label(h.toZone).c_str());
    }
    ImGui::EndTable();
}

// Every way out of this map with its best route, the quickest first: the first one is what the bot takes.
void ways_table(const Atlas& at, const std::vector<Route>& ways, const char* id)
{
    if (ways.empty()) { ImGui::TextColored(ui::kWarn, "no way out of this map gets there"); return; }
    if (!begin_table(id, 5)) return;
    ImGui::TableSetupColumn("", 0, 70);
    ImGui::TableSetupColumn("Time", 0, 70);
    ImGui::TableSetupColumn("Galder", 0, 60);
    ImGui::TableSetupColumn("Out by", 0, 300);
    ImGui::TableSetupColumn("Then", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < ways.size(); ++i) {
        const Route& r = ways[i];
        const RouteHop& h = r.hops.front();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (i == 0) ImGui::TextColored(ui::kOk, "taken");
        else ImGui::TextColored(ui::kMuted, "+%.0f s", r.secs - ways[0].secs);
        ImGui::TableNextColumn(); ImGui::Text("~%.0f s", r.secs);
        ImGui::TableNextColumn(); ImGui::Text("%d", r.gelder);
        ImGui::TableNextColumn();
        if (h.kind == RouteHop::Portal) ImGui::Text("portal at (%d, %d) to %s", h.x, h.y, at.map_label(h.toZone).c_str());
        else if (h.kind == RouteHop::Item)
            ImGui::Text("%s to %s", h.advIndex >= 0 ? "the Portable Port AD" : "a port item", at.map_label(h.toZone).c_str());
        else {
            const AtlasNpc* n = at.npc(h.npc);
            ImGui::Text("%s: teleport to %s", n ? n->name.c_str() : "NPC", at.map_label(h.toZone).c_str());
        }
        ImGui::TableNextColumn();
        std::string via;
        for (size_t k = 1; k < r.hops.size(); ++k) via += (k > 1 ? " > " : "") + at.map_label(r.hops[k].toZone);
        ImGui::TextColored(ui::kMuted, "%s", via.empty() ? "(there)" : via.c_str());
    }
    ImGui::EndTable();
}

} // namespace

// ── pickers ─────────────────────────────────────────────────────────────────
bool NeoApp::map_picker(const char* id, int& zone, std::string& query, float width)
{
    const Atlas& at = gameData_.atlas();
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    const std::string cur = zone > 0 ? at.map_label(zone) : std::string("choose a map...");
    if (ImGui::BeginCombo(id, cur.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##q", "search a map by name or id...", &query);
        const std::string q = lower_s(query);
        std::vector<int> ids;
        for (const auto& kv : at.maps()) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        int shown = 0;
        for (int z : ids) {
            const AtlasMap* m = at.map(z);
            if (!m || !matches(q, m->name, z)) continue;
            if (++shown > 400) break;
            ImGui::PushID(z);
            const std::string text = fmt("%s (%d)%s", m->name.c_str(), z, m->isTown() ? "   town" : "");
            if (ImGui::Selectable(text.c_str(), zone == z)) { zone = z; changed = true; }
            ImGui::PopID();
        }
        if (!shown) ImGui::TextColored(ui::kMuted, "nothing found");
        ImGui::EndCombo();
    }
    return changed;
}

bool NeoApp::npc_picker(const char* id, std::string& key, bool shops, std::string& query, float width)
{
    const Atlas& at = gameData_.atlas();
    bool changed = false;
    const int cur = at.find_npc(key);
    const AtlasNpc* cn = at.npc(cur);
    ImGui::SetNextItemWidth(width);
    const std::string label = cn ? fmt("%s  -  %s", cn->name.c_str(), at.map_label(cn->zone).c_str())
                                 : std::string(shops ? "choose a shop..." : "choose a warehouse keeper...");
    if (ImGui::BeginCombo(id, label.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##q", "search by map or name...", &query);
        const std::string q = lower_s(query);
        if (ImGui::Selectable("(none)", cur < 0)) { key.clear(); changed = true; }
        int shown = 0;
        for (int i : shops ? at.shops() : at.warehouses()) {
            const AtlasNpc* n = at.npc(i);
            const AtlasMap* m = at.map(n->zone);
            const std::string text = fmt("%s  -  %s (%d)%s", n->name.c_str(), m ? m->name.c_str() : "?", n->zone,
                                         shops ? fmt("   %zu items", n->items.size()).c_str() : "");
            if (!q.empty() && lower_s(text).find(q) == std::string::npos) continue;
            if (++shown > 400) break;
            ImGui::PushID(i);
            if (ImGui::Selectable(text.c_str(), i == cur)) { key = n->key(); changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// ── Town ────────────────────────────────────────────────────────────────────
void NeoApp::bot_town(Session& s)
{
    const WorldModel& w = *s.world;
    TownAction& t = s.bot.town;
    const Atlas& at = gameData_.atlas();
    ui::page_header("Town", s.bot.running && (t.enabled || t.trip()) ? t.status.c_str() : "death, the rest in town, the way back",
                    &t.enabled);
    ImGui::Dummy(ImVec2(0, 4));

    section_title("When you die");
    toggle_row("go to town", &t.onDeath);
    ui::help("C2S 0x16 Go to Town: the server brings you back in your return town (the last town you entered) "
             "with 30% of your HP. Off = the bot stops when you die.");
    ImGui::SameLine(0, 16);
    slider("##dd", &t.deathDelaySec, 0, 30, "after %d s", 160);

    section_title("In town");
    slider("##rhp", &t.recoverHp, 0, 100, "rest up to HP %d%%", 200);
    ImGui::SameLine();
    slider("##rmp", &t.recoverMp, 0, 100, "and MP %d%%", 200);
    ui::help("C2S 0x40 [13] = rest (state 15) until both are reached, then stand up. 0 = no rest.");
    toggle_row("then the town errands (Auto-Supply / Auto-Sell / Auto-Storage)", &t.supplyAfterDeath);

    section_title("Then");
    static const char* kModes[] = { "Back where it died", "To a chosen map", "Stay in town (stop)" };
    ui::segmented("##ret", &t.returnMode, kModes, 3);
    if (t.returnMode == TownAction::ReturnChosen) {
        ImGui::SameLine(0, 12);
        map_picker("##rz", t.returnZone, townMapQuery_, 340);
    }
    note("Back where it died = the map it left and its start point there; a supply trip comes back the same way. "
         "The way between maps is the Travel part's (below): the portals of every map and the NPC teleports.");

    section_title("Now");
    if (!t.trip()) {
        ImGui::TextColored(ui::kMuted, "no trip%s", t.status.empty() ? "" : fmt("  -  %s", t.status.c_str()).c_str());
    } else {
        ui::pill(TownAction::phase_name(t.phase), ui::kStatusRun);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", t.why.c_str());
        if (t.backZone >= 0)
            ImGui::TextColored(ui::kMuted, "then back to %s%s", at.map_label(t.backZone).c_str(),
                               t.backPoint ? fmt(" (%d, %d)", t.backX, t.backY).c_str() : "");
        if (!t.nav.status.empty()) ImGui::TextColored(t.nav.failed ? ui::kWarn : ui::kMuted, "%s", t.nav.status.c_str());
        if (t.nav.active() && !t.nav.route().hops.empty()) route_table(at, t.nav.route(), "troute");
        if (ImGui::Button("Cancel the trip")) {
            t.reset();
            s.bot.note(now_filetime(), "trip cancelled by you");
        }
    }
}

// ── Supply ──────────────────────────────────────────────────────────────────
void NeoApp::bot_supply(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    TownAction& t = bot.town;
    const Atlas& at = gameData_.atlas();
    const uint64_t now = now_filetime();
    ui::page_header("Auto-Supply", bot.running && t.trip() && t.phase != TownAction::Dead ? t.status.c_str()
                                                                                          : "buy the refill items at a shop",
                    &t.supply);
    if (!t.enabled) ImGui::TextColored(ui::kWarn, "The Town is off (Roam & Travel tab): no trip will run.");
    ImGui::Dummy(ImVec2(0, 4));

    // ── the shop ──
    section_title("Shop");
    npc_picker("##shop", t.shopKey, true, shopQuery_, 460);
    const AtlasNpc* shop = at.npc(at.find_npc(t.shopKey));
    if (shop) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, "%zu items, at (%d, %d)%s", shop->items.size(), shop->x, shop->y,
                           shop->hasPoint ? "" : " (no point in the map file)");
    }

    // ── refill ──
    section_title("Refill");
    note("A trip starts when an item drops to its \"trip at\" count; at the shop it buys up to \"buy up to\" (as the "
         "galder allows; never below the galder kept). Ammo and potions alike. The trip also sells (Auto-Sell) and stores "
         "(Auto-Storage) when those are on.");
    int drop = -1;
    if (!t.buy.empty() && begin_table("buy", 6)) {
        ImGui::TableSetupColumn("Item", 0, 300);
        ImGui::TableSetupColumn("Have", 0, 56);
        ImGui::TableSetupColumn("Trip at", 0, 110);
        ImGui::TableSetupColumn("Buy up to", 0, 110);
        ImGui::TableSetupColumn("On", 0, 50);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.buy.size(); ++i) {
            BuyRow& r = t.buy[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            const bool sold = !shop || std::find(shop->items.begin(), shop->items.end(), r.item) != shop->items.end();
            ImGui::TableNextColumn(); icon_text(gfx::item(r.item), gameData_.item_name(r.item), sold ? ui::kText : ui::kWarn);
            if (!sold && ImGui::IsItemHovered()) ImGui::SetTooltip("this shop does not sell it");
            const int have = w.count_item(r.item);
            ImGui::TableNextColumn(); mid(); ImGui::TextColored(have <= r.below ? ui::kWarn : ui::kText, "%d", have);
            ImGui::TableNextColumn(); mid(); ImGui::SetNextItemWidth(100); ImGui::InputInt("##b", &r.below, 1, 10);
            ImGui::TableNextColumn(); mid(); ImGui::SetNextItemWidth(100); ImGui::InputInt("##u", &r.upTo, 1, 10);
            r.below = std::clamp(r.below, 0, 30000);
            r.upTo = std::clamp(r.upTo, r.below + 1, 30000);
            ImGui::TableNextColumn(); mid(); ui::toggle("##on", &r.on);
            ImGui::TableNextColumn(); mid(); if (ImGui::SmallButton("remove")) drop = int(i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (drop >= 0) t.buy.erase(t.buy.begin() + drop);
    {
        // add: what the shop sells, the bag's usable items first
        ImGui::SetNextItemWidth(360);
        const std::string cur = buyAdd_ ? gameData_.item_name(buyAdd_) : std::string(shop ? "add an item the shop sells..." : "choose a shop first");
        ImGui::BeginDisabled(!shop);
        if (ImGui::BeginCombo("##add", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (shop)
                for (uint32_t id : shop->items) {
                    bool there = false;
                    for (const BuyRow& r : t.buy) there = there || r.item == id;
                    if (there) continue;
                    ImGui::PushID(int(id));
                    const ImVec2 p = ImGui::GetCursorPos();
                    if (ImGui::Selectable("##i", buyAdd_ == id, 0, ImVec2(0, 24))) buyAdd_ = id;
                    ImGui::SetCursorPos(p);
                    ui::icon(gfx::item(id), 24);
                    ImGui::SameLine();
                    ImGui::SetCursorPosY(p.y + (24 - ImGui::GetTextLineHeight()) * 0.5f);
                    ImGui::Text("%s", gameData_.item_name(id).c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(ui::kMuted, "%u", id);
                    ImGui::PopID();
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!buyAdd_);
        if (ImGui::Button("+ Add")) {
            BuyRow r;
            r.item = buyAdd_;
            const ItemInfo* ii = gameData_.item(buyAdd_);
            if (ii && ii->type == 29) { r.below = 100; r.upTo = 1000; }     // bullets go by hundreds
            t.buy.push_back(r);
            buyAdd_ = 0;
        }
        ImGui::EndDisabled();
    }

    // ── when ──
    section_title("When");
    slider("##bf", &t.bagFull, 50, 300, "bag full at %d items", 220);
    ImGui::SameLine();
    toggle_row("too heavy to pick up (S2C 0x23)", &t.onOverweight);
    slider("##wp", &t.weightPct, 0, 100, t.weightPct ? "the bag weighs %d %% or more" : "by weight: off", 220);
    ImGui::SameLine();
    ui::help("A trip when the bag weighs that share of your max weight (the MaxWT stat, as the server counts it: "
             "a rare item weighs once, a stack weight x count). 0 = off. The Drill tab sets it too.");
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("galder always kept", &t.keepGelder, 100, 1000);
    t.keepGelder = std::max(0, t.keepGelder);
    const int maxWt = w.stat_base(ST_MAXWT);
    ImGui::TextColored(ui::kMuted, "now: %zu items in the bag (300 max), weight %lld%s, %s galder", w.inventory.size(),
                       (long long)w.bag_weight(), maxWt > 0 ? fmt(" / %d", maxWt).c_str() : "", thousands(w.self.money).c_str());
    const std::string need = t.need(w);
    ImGui::TextColored(need.empty() ? ui::kMuted : ui::kWarn, "%s", need.empty() ? "no trip due" : ("due: " + need).c_str());
    ImGui::BeginDisabled(!bot.running || t.trip() || !t.errands());
    if (ImGui::Button("Town trip now")) {
        t.start_supply(w, now, bot.anchorZone, bot.anchorX, bot.anchorY, "asked by you");
        bot.note(now, "supply trip: asked by you");
    }
    ImGui::EndDisabled();
    if (!bot.running) { ImGui::SameLine(); ImGui::TextColored(ui::kMuted, "(start the bot first)"); }
}

// ── Auto-Sell ───────────────────────────────────────────────────────────────
void NeoApp::bot_sell(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    TownAction& t = bot.town;
    const Atlas& at = gameData_.atlas();
    ui::page_header("Auto-Sell", bot.running && t.trip() && t.phase == TownAction::Shop ? t.status.c_str()
                                                                                       : "sell every item but the ones you keep",
                    &t.sell);
    if (!t.sellMigrated.empty()) ImGui::TextColored(ui::kWarn, "%s", t.sellMigrated.c_str());
    if (!t.enabled) ImGui::TextColored(ui::kWarn, "The Town is off (Roam & Travel tab): no trip will run.");
    ImGui::Dummy(ImVec2(0, 4));
    const AtlasNpc* shop = at.npc(at.find_npc(t.shopKey));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "at");
    ImGui::SameLine();
    npc_picker("##shop", t.shopKey, true, shopQuery_, 460);
    if (!shop) { ImGui::SameLine(); ImGui::TextColored(ui::kWarn, "choose a shop"); }
    toggle_row("rare items too", &t.sellRare);
    {
        int n = 0;
        for (const InvItem& it : w.inventory) n += t.sellable(w, it);
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::kMuted, "%d item%s in the bag would be sold now", n, n == 1 ? "" : "s");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "the equipment grades:");
    for (int k = 0; k < ET_Count; ++k) {
        ImGui::SameLine(0, 14);
        ImGui::Checkbox(fmt("%s##selltier", GameData::equip_tier_name(k)).c_str(), &t.sellTier[k]);
    }
    note("Every item is sold except the ones checked \"keep\" below. Gear of the grades above (weapons, helmets and shields "
         "named Old, Strong / Sharp, Art, Ult.) goes by its grade's checkbox. Never sold: what you wear, the refill items "
         "(Auto-Supply), the Item Hunt's / quests' items, the potions and port items the bot uses, cash items, rare items "
         "unless \"rare items too\", items the server does not buy (Options 0x200). A trip runs when the bag is full / too "
         "heavy (Auto-Supply: \"When\").");
    // the list: always the last thing of the page
    section_title("Keep (never sold)");
    ImGui::PushID("sell");
    draw_keep_list(s, 2, t.sellList);
    ImGui::PopID();
}

// ── Auto-Storage ────────────────────────────────────────────────────────────
void NeoApp::bot_storage(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    TownAction& t = bot.town;
    ui::page_header("Auto-Storage", bot.running && t.trip() && t.phase == TownAction::Store ? t.status.c_str()
                                                                                           : "store every item but the ones you keep",
                    &t.store);
    if (!t.storeMigrated.empty()) ImGui::TextColored(ui::kWarn, "%s", t.storeMigrated.c_str());
    if (!t.enabled) ImGui::TextColored(ui::kWarn, "The Town is off (Roam & Travel tab): no trip will run.");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui::kMuted, "at");
    ImGui::SameLine();
    npc_picker("##wh", t.whKey, false, whQuery_, 420);
    {
        int n = 0;
        for (const InvItem& it : w.inventory) n += t.storable(w, it);
        ImGui::TextColored(ui::kMuted, "%d item%s in the bag would be stored now  -  warehouse %d slots", n, n == 1 ? "" : "s",
                           w.self.whSlots);
    }
    note("Every item is stored except the ones checked \"keep\" below. Warehouse_Start costs 50 galder and needs level 5. "
         "Never stored: what you wear, the refill items, the loaded bullets' kind, the Item Hunt's / quests' items, the potions "
         "and port items the bot uses, cash items, items the warehouse refuses (Options 0x10000). The errands run warehouse "
         "first, then the shop.");
    section_title("Keep (never stored)");
    ImGui::PushID("store");
    draw_keep_list(s, 3, t.storeList);
    ImGui::PopID();
}

// The bag's items with a "keep" checkbox each (+ the kept ones not in the bag), what each would do now, a search to keep
// another one.
void NeoApp::draw_keep_list(Session& s, int kind, IdFilter& f)
{
    const WorldModel& w = *s.world;
    TownAction& t = s.bot.town;
    const bool sell = kind == 2;
    std::vector<uint32_t> ids;
    std::map<uint32_t, const InvItem*> first;
    for (const InvItem& it : w.inventory) {
        if (it.equipSlot >= 0) continue;
        if (!first.count(it.itemId)) { first[it.itemId] = &it; ids.push_back(it.itemId); }
    }
    for (uint32_t id : f.black)
        if (!first.count(id)) ids.push_back(id);
    int kept = 0;
    for (uint32_t id : ids) kept += f.black.count(id) != 0;
    ImGui::TextColored(ui::kMuted, "%d kept; the bag's items first, then the kept ones not in it", kept);
    if (ids.empty()) ImGui::TextColored(ui::kMuted, "the bag is empty and nothing is kept");
    else if (begin_table("keep", 4)) {
        ImGui::TableSetupColumn("Keep", 0, 44);
        ImGui::TableSetupColumn("Item", 0, 320);
        ImGui::TableSetupColumn("In the bag", 0, 80);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (uint32_t id : ids) {
            ImGui::TableNextRow();
            ImGui::PushID(int(id));
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kIcon - ImGui::GetFrameHeight()) * 0.5f);
            bool keep = f.black.count(id) != 0;
            if (ImGui::Checkbox("##keep", &keep)) { if (keep) f.black.insert(id); else f.black.erase(id); }
            ImGui::TableNextColumn(); icon_text(gfx::item(id), gameData_.item_name(id));
            ImGui::TableNextColumn(); mid(); ImGui::Text("%d", w.count_item(id));
            ImGui::TableNextColumn(); mid();
            auto fi = first.find(id);
            if (fi == first.end()) ImGui::TextColored(ui::kMuted, "not in the bag");
            else if (sell ? t.sellable(w, *fi->second) : t.storable(w, *fi->second))
                ImGui::TextColored(ui::kWarn, sell ? "would be sold" : "would be stored");
            else if (keep) ImGui::TextColored(ui::kOk, "kept");
            else {
                const ItemInfo* ii = gameData_.item(id);
                if (ii && TownAction::protected_item(*ii))
                    ImGui::TextColored(ui::kMuted, ii->ncash > 0 ? "never: a cash item" : "never: the bot uses it (potion / port item)");
                else ImGui::TextColored(ui::kMuted, sell ? "never sold (refill / a quest's / rare / not buyable / its grade)"
                                                         : "never stored (refill / a quest's / loaded bullets / refused)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // keep an item that is not in the bag
    ImGui::SetNextItemWidth(360);
    ImGui::InputTextWithHint("##q", "keep another item: search by name or id...", &listQuery_[kind]);
    if (listQuery_[kind] != listQueryFor_[kind]) {
        listQueryFor_[kind] = listQuery_[kind];
        listResults_[kind] = gameData_.search_items(listQuery_[kind], 40);
    }
    if (!listQuery_[kind].empty()) {
        ImGui::BeginChild("##res", ImVec2(0, 130), ImGuiChildFlags_Borders);
        if (listResults_[kind].empty()) ImGui::TextColored(ui::kMuted, "nothing found");
        for (uint32_t id : listResults_[kind]) {
            ImGui::PushID(int(id));
            const bool in = f.black.count(id) != 0;
            ImGui::BeginDisabled(in);
            if (ImGui::SmallButton(in ? "kept" : "+ Keep")) f.black.insert(id);
            ImGui::EndDisabled();
            ImGui::SameLine();
            icon_text(gfx::item(id), gameData_.item_name(id));
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

// ── Travel ──────────────────────────────────────────────────────────────────
void NeoApp::bot_travel(Session& s)
{
    const WorldModel& w = *s.world;
    Bot& bot = s.bot;
    TownAction& t = bot.town;
    Navigator& nav = t.nav;
    const Atlas& at = gameData_.atlas();
    const uint64_t now = now_filetime();
    ui::page_header("Travel", t.trip() && !nav.status.empty() ? nav.status.c_str() : "the map graph: portals + NPC teleports", nullptr);
    ImGui::TextColored(ui::kMuted, "%s", at.load_report().c_str());
    ImGui::Dummy(ImVec2(0, 4));

    section_title("Options");
    toggle_row("NPC teleports", &nav.teleports);
    ui::help("Teleport_Start of the NPCs with a teleport table (talk C2S 0x6F, then 0x7F func 19 [slot]); the price is "
             "paid; your level must reach its MinLevel.");
    ImGui::SameLine(0, 16);
    slider("##mg", &nav.maxGelder, 0, 20000, "at most %d galder each", 240);
    toggle_row("Portable Port AD", &nav.portAd);
    ui::help("With a Portable Port AD (19501 / 19502) in the bag the route may start with it: C2S 0x0108 [ZoneIndex] "
             "to any town or gate of its list (teleport_adv_db), from any map but the Option 128 ones. One item each use: "
             "taken when it saves more than ~15 s of the way. A use that does not port us = none for 5 min.");
    {
        const uint32_t adItem = at.port_ad_item(Navigator::base_options(w));
        ImGui::SameLine(0, 16);
        if (adItem) ImGui::TextColored(ui::kMuted, "%d in the bag", w.count_item(adItem));
        else ImGui::TextColored(ui::kMuted, "none in the bag");
    }

    int px = 0, py = 0;
    w.self_pos_at(now, px, py);
    const AtlasMap* here = at.map(w.self.zone);
    section_title("Here");
    if (!here) {
        ImGui::TextColored(ui::kMuted, "not in a known map");
    } else {
        ImGui::Text("%s  at (%d, %d)%s", at.map_label(here->id).c_str(), px, py, here->isTown() ? "  -  a town" : "");
        if (begin_table("here", 3)) {
            ImGui::TableSetupColumn("Way", 0, 300);
            ImGui::TableSetupColumn("Where", 0, 160);
            ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const AtlasPortal& p : here->portals) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("portal #%u", p.id);
                ImGui::TableNextColumn(); ImGui::Text("(%d, %d)", p.cx(), p.cy());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(at.map_label(p.toZone).c_str());
            }
            for (int ni : here->npcs) {
                const AtlasNpc* n = at.npc(ni);
                if (!n || n->teleports.empty()) continue;
                for (const TeleportSlot& sl : n->teleports) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%s  (Lv %d+, %d galder)", n->name.c_str(), n->teleportMinLevel, sl.cost);
                    ImGui::TableNextColumn(); ImGui::Text("(%d, %d)", n->x, n->y);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(at.map_label(sl.zone).c_str());
                }
            }
            ImGui::EndTable();
        }
    }

    section_title("Go to a map");
    map_picker("##tz", travelZone_, travelMapQuery_, 360);
    ImGui::SameLine();
    const bool can = s.state == Session::State::Live && w.inWorld && !w.self.dead && travelZone_ > 0 && !t.trip();
    ImGui::BeginDisabled(!can);
    if (ui::button_primary("Go", ImVec2(80, 0))) {
        if (!bot.running) {
            bot.start(w, now);
            bot.travelOnly = true;              // stops when it is there
        }
        t.travel_to(travelZone_, now);
        bot.note(now, "travel to " + at.map_label(travelZone_));
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With the bot stopped: it starts, travels and stops there. Running: it hunts there afterwards.");
    if (travelZone_ > 0 && here && travelZone_ != w.self.zone) {
        // every way out of this map with its best route (planned again as you move 200 units)
        RouteOptions o = Navigator::base_options(w);
        o.portAd = nav.portAd;
        const std::string key = fmt("%d,%d,%d>%d:%d:%d:%d:%lld:%d:%u", w.self.zone, px / 200, py / 200, travelZone_, nav.teleports,
                                    nav.maxGelder, w.self.level, (long long)w.self.money / 100, nav.portAd,
                                    at.port_ad_item(o));
        if (key != travelPreviewKey_) {
            travelPreviewKey_ = key;
            o.teleports = nav.teleports;
            o.maxGelder = nav.maxGelder;
            o.level = w.self.level;
            o.money = w.self.money;
            o.speed = w.self.speed > 0 ? w.self.speed : 160;
            travelPreview_ = at.alternatives(gameData_, w.self.zone, px, py, travelZone_, o, 8);
        }
        ImGui::TextColored(ui::kMuted, "Every way out of this map, the quickest first (time = the walks on the map's grid, "
                                       "~4 s per map change, a teleport's talk and price):");
        ways_table(at, travelPreview_, "ways");
        if (!travelPreview_.empty()) route_table(at, travelPreview_.front(), "preview");
    }
    if (t.trip()) {
        section_title("On the way");
        ImGui::TextColored(nav.failed ? ui::kWarn : ui::kMuted, "%s", nav.status.c_str());
        if (nav.active() && !nav.route().hops.empty()) {
            route_table(at, nav.route(), "live");
            if (nav.others().size() > 1) {
                ImGui::TextColored(ui::kMuted, "the ways out it compared:");
                ways_table(at, nav.others(), "liveways");
            }
        }
        if (ImGui::Button("Cancel")) {
            t.reset();
            if (bot.travelOnly) bot.stop_by_user("travel cancelled");
        }
    }
}

} // namespace nj
