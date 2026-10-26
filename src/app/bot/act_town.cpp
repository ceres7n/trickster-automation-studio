// Town: death -> town -> rest -> the supply errands -> back (bot_actions.h). Server rules (ProjectPAULA):
//   death     - C2S 0x16 only while dead (game_session.cpp handle_restart): HP = 30 % of the max, S2C 0x17
//               [town][0,0][hp] + the field change to the return town (or maps_db Town, restart_town: zone).
//   rest      - C2S 0x40 [13] -> state 15, which swallows moves; [0] stands up.
//   warehouse - Warehouse_Start (NpcScript 16, warehouse.cpp): level >= 5 and 50 galder, else its 2nd / 3rd
//               argument's message (S2C 115); OK = S2C 117 + 137 per tab (state 10). C2S 0x8B [tab][uid][item]
//               [count]: refused (141) for a worn item, Options & 0x10000, the loaded bullets' id, a tab at 80
//               unless the item stacks there; a uid already in the tab cuts the session (we send one item
//               at a time, straight from the bag, so it cannot happen).
//   shop      - state 5 swallows 0x18 / 0x40 until C2S 0x4F; sell (shop.cpp): not Options & 0x200, a count
//               the bag has (we never sell a worn item); buy: the weight (0x4B), the money (0x4A), 1..20 lines.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace nj {
namespace {

constexpr uint64_t kAnswerMs = 4000;            // an NPC / shop / warehouse answer
constexpr int      kNpcRange = 150;             // walk this close to a shop / warehouse keeper (talk: 300)
constexpr uint64_t kGiveUpMs = 90000;           // no way to an errand for this long: skip it
constexpr uint64_t kNoTripMs = 600000;          // a trip that did not fix its cause: no new one for 10 min
constexpr uint64_t kUnset    = ~0ull;

std::vector<uint8_t> wh_tabs(int slots)          // tabs of 80: ids 2, 21..29 (warehouse.cpp wh_build)
{
    std::vector<uint8_t> t{ 2 };
    for (int i = 1; i < std::clamp(slots / 80, 1, 10); ++i) t.push_back(uint8_t(20 + i));
    return t;
}

// the arguments of `name` in an NPC message ("...\Warehouse_Start,1,2,3")
bool script_args_in(const std::string& text, const char* name, std::vector<uint16_t>& out)
{
    size_t at = text.find(name);
    while (at != std::string::npos && at >= 3 && text.compare(at - 3, 3, "Neo") == 0) at = text.find(name, at + 1);
    if (at == std::string::npos) return false;
    out.clear();
    size_t p = at + strlen(name);
    while (p < text.size() && text[p] == ',') {
        ++p;
        size_t e = p;
        while (e < text.size() && text[e] >= '0' && text[e] <= '9') ++e;
        if (e == p) break;
        out.push_back(uint16_t(std::strtoul(text.substr(p, e - p).c_str(), nullptr, 10)));
        p = e;
    }
    return true;
}

} // namespace

const char* TownAction::phase_name(int p)
{
    switch (p) {
    case Idle:    return "idle";
    case Dead:    return "dead";
    case Recover: return "recovering";
    case Store:   return "warehouse";
    case Shop:    return "shop";
    case Return:  return "on the way";
    }
    return "?";
}

TownAction::TownAction()
{
    sellList.mode = IdFilter::AllButBlack;
    storeList.mode = IdFilter::AllButBlack;
}

void TownAction::after_load()
{
    auto migrate = [](IdFilter& f, bool& on, std::string& notice, const char* verb) {
        if (f.mode != IdFilter::OnlyWhite) return;
        notice = fmt("Your old list (only %zu whitelisted item%s %s) is gone: now every item is %s except the ones checked "
                     "\"keep\" below. The switch was turned off: check what to keep, then turn it on.", f.white.size(),
                     f.white.size() == 1 ? "" : "s", verb, verb);
        f.mode = IdFilter::AllButBlack;
        f.white.clear();
        on = false;
    };
    migrate(sellList, sell, sellMigrated, "sold");
    migrate(storeList, store, storeMigrated, "stored");
}

void TownAction::reset()
{
    phase = Idle;
    why.clear();
    backZone = -1;
    backPoint = false;
    manual = false;
    nav.reset();
    fromDeath_ = false;
    sub_ = 0;
    townSentAt_ = 0;
    lastOverweight_ = kUnset;
    noTripUntil_ = 0;
    lostSince_ = 0;
}

void TownAction::observe(BotCtx& c)
{
    if (lastOverweight_ == kUnset) lastOverweight_ = c.w.overweightAt;   // only what happens from now on
}

bool TownAction::in_buy_list(uint32_t item) const
{
    for (const BuyRow& r : buy)
        if (r.item == item) return true;
    return false;
}

bool TownAction::protected_item(const ItemInfo& ii)
{
    if (ii.ncash > 0) return true;                                          // a cash item
    if (ii.type == 15 && (ii.effect == 20 || ii.effect == 28)) return true; // the Portable Port AD / a port item (routes)
    return (ii.options & 2) && (ii.hp > 0 || ii.mp > 0);                    // a potion (the Heal)
}

bool TownAction::sellable(const WorldModel& w, const InvItem& it) const
{
    if (it.equipSlot >= 0 || in_buy_list(it.itemId) || keep.count(it.itemId)) return false;
    const ItemInfo* ii = w.data() ? w.data()->item(it.itemId) : nullptr;
    if (!ii || (ii->options & 0x200) || ii->type == 30 || protected_item(*ii)) return false;   // not sellable / galder / ours
    if (sellList.black.count(it.itemId)) return false;                     // checked "keep"
    const int tier = w.data()->equip_tier(it.itemId);                       // a common gear grade: its own switch
    if (tier != ET_None) return sellTier[tier];
    return sellRare || !(it.kind == 2 || it.kind == 3);
}

bool TownAction::storable(const WorldModel& w, const InvItem& it) const
{
    if (it.equipSlot >= 0 || in_buy_list(it.itemId) || keep.count(it.itemId)) return false;
    const ItemInfo* ii = w.data() ? w.data()->item(it.itemId) : nullptr;
    if (!ii || (ii->options & 0x10000) || ii->type == 30 || protected_item(*ii)) return false;   // NotAddableToWarehouse / ours
    const InvItem* loaded = w.self.equip[20] ? w.find_item(w.self.equip[20]) : nullptr;
    if (loaded && loaded->itemId == it.itemId) return false;              // the loaded bullets' id (wh_ammo_item)
    return !storeList.black.count(it.itemId);                               // (not checked "keep")
}

int TownAction::npc_index(const WorldModel& w, const std::string& key) const
{
    if (key.empty() || !w.data()) return -1;
    const Atlas& at = w.data()->atlas();
    auto it = npcCache_.find(key);
    if (it != npcCache_.end() && (it->second < 0 || (at.npc(it->second) && at.npc(it->second)->key() == key))) return it->second;
    const int i = at.find_npc(key);
    npcCache_[key] = i;
    return i;
}

std::string TownAction::need(const WorldModel& w) const
{
    if (!errands() || !w.data()) return std::string();
    const bool shopOk = npc_index(w, shopKey) >= 0, whOk = npc_index(w, whKey) >= 0;
    if (shopOk && supply)
        for (const BuyRow& r : buy) {
            if (!r.on || !r.item) continue;
            const int have = w.count_item(r.item);
            if (have <= r.below) return fmt("%s is running out (%d left)", w.data()->item_name(r.item).c_str(), have);
        }
    bool any = false;
    for (const InvItem& it : w.inventory)
        if ((sell && shopOk && sellable(w, it)) || (store && whOk && storable(w, it))) {
            any = true;
            break;
        }
    if (!any) return std::string();
    if ((int)w.inventory.size() >= bagFull) return fmt("the bag is full (%zu items)", w.inventory.size());
    if (onOverweight && lastOverweight_ != kUnset && w.overweightAt > lastOverweight_) return "too heavy to pick up more";
    if (weightPct == 0 && w.weight_pct() >= 92)   // (nothing set: 92 %, so a heavy bag is never left as it is)
        return fmt("the bag weighs %d %% of the max", w.weight_pct());
    if (weightPct > 0) {                        // the server's limit: the MaxWT stat (inventory.cpp, our base + items)
        const int maxWt = w.stat_base(ST_MAXWT);
        const int64_t wt = w.bag_weight();
        if (maxWt > 0 && wt * 100 >= int64_t(weightPct) * maxWt)
            return fmt("the bag weighs %lld / %d (%lld %%)", (long long)wt, maxWt, (long long)(wt * 100 / maxWt));
    }
    return std::string();
}

void TownAction::start_supply(const WorldModel& w, uint64_t now, int anchorZone, int ax, int ay, const std::string& reason)
{
    (void)now;
    phase = Recover;
    fromDeath_ = false;
    manual = false;
    why = reason;
    sub_ = 0;
    backZone = w.self.zone;
    backPoint = anchorZone == backZone;
    backX = ax;
    backY = ay;
    nav.stop();
}

void TownAction::travel_to(int zone, uint64_t now)
{
    (void)now;
    phase = Return;
    fromDeath_ = false;
    manual = true;
    why = "travel";
    sub_ = 0;
    backZone = zone;
    backPoint = false;
    nav.stop();
}

const Entity* TownAction::npc_entity(const BotCtx& c, const AtlasNpc& n) const
{
    const Entity* best = nullptr;
    double bestD = 1e18;
    for (const auto& kv : c.w.entities) {
        const Entity& e = kv.second;
        if ((e.kind != EK_NPC && e.kind != EK_SHOP) || e.species != n.species) continue;
        const double d = botu::dist(e.x, e.y, n.x, n.y);
        if (d < bestD) { bestD = d; best = &e; }
    }
    return best && bestD <= 400 ? best : nullptr;
}

// Store -> Shop -> Return, skipping the errands with nothing to do.
void TownAction::next_errand(BotCtx& c)
{
    const WorldModel& w = c.w;
    const Atlas& at = w.data()->atlas();
    const bool errands = this->errands() && (!fromDeath_ || supplyAfterDeath);
    sub_ = 0;
    subAt_ = 0;
    npcEid_ = 0;
    lostSince_ = 0;
    nav.stop();
    if (phase < Store && errands && store && npc_index(w, whKey) >= 0) {
        bool any = false;
        for (const InvItem& it : w.inventory) any = any || storable(w, it);
        if (any) {
            phase = Store;
            storeUid_ = 0;
            storeSkip_.clear();
            storeTried_.clear();
            return;
        }
    }
    if (phase < Shop && errands && npc_index(w, shopKey) >= 0) {
        bool sales = false, buys = false;
        if (sell) for (const InvItem& it : w.inventory) sales = sales || sellable(w, it);
        if (supply) for (const BuyRow& r : buy) buys = buys || (r.on && r.item && w.count_item(r.item) < r.upTo);
        if (sales || buys) {
            phase = Shop;
            sellRounds_ = buyTries_ = 0;
            buyScale_ = 1.0;
            sellDone_ = !sales;
            buyDone_ = !buys;
            return;
        }
    }
    phase = Return;
}

void TownAction::finish(BotCtx& c, const std::string& text)
{
    const WorldModel& w = c.w;
    const bool atBack = backPoint && w.self.zone == backZone;
    c.bot.anchorZone = w.self.zone;
    c.bot.anchorX = atBack ? backX : c.px;
    c.bot.anchorY = atBack ? backY : c.py;
    phase = Idle;
    nav.stop();
    fromDeath_ = false;
    backZone = -1;
    lastTripEnd_ = c.now;
    lastOverweight_ = w.overweightAt;
    c.bot.note(c.now, text);
    const bool wasManual = manual;
    manual = false;
    const std::string still = need(w);
    if (!still.empty()) {
        noTripUntil_ = c.now + kNoTripMs * kFtMs;
        c.bot.note(c.now, "still " + still + ": no new supply trip for 10 min");
    }
    if (wasManual && c.bot.travelOnly) c.bot.stop("arrived", true);
}

// ── the step ────────────────────────────────────────────────────────────────
Step TownAction::step(BotCtx& c)
{
    const WorldModel& w = c.w;
    if (!w.data()) { status = "no game data"; return Step::Pass; }
    if (w.self.dead) return dead_step(c);
    if (phase == Dead) {                         // alive again: S2C 0x17 + the change put us in town
        phase = Recover;
        sub_ = 0;
        subAt_ = 0;
        restHp_ = restMp_ = -1;
        restChangeAt_ = restSince_ = 0;
        c.bot.note(c.now, fmt("in %s with %d%% HP", w.data()->atlas().map_label(w.self.zone).c_str(),
                              botu::pct(w.self.hp, w.self.hpMax)));
    }
    if (phase == Idle) {
        if (!enabled) return Step::Pass;
        if (c.now < noTripUntil_) { status = "waiting before another supply trip"; return Step::Pass; }
        const std::string n = need(w);
        if (n.empty()) { status = errands() ? "supplies ok" : "ready"; return Step::Pass; }
        if (c.attacked || c.bot.attack.engaged(w)) { status = n + " - after this fight"; return Step::Pass; }
        start_supply(w, c.now, c.bot.anchorZone, c.bot.anchorX, c.bot.anchorY, n);
        c.bot.note(c.now, "supply trip: " + n);
    }
    // a fight on the way (not in a dialog): the attack action defends us (only who attacks, c.travelling)
    if ((phase == Store || phase == Shop || phase == Return) && sub_ == 0 && (c.attacked || c.bot.attack.engaged(w))) {
        status = "fighting on the way";
        nav.interrupted();
        return Step::Pass;
    }
    switch (phase) {
    case Recover: return recover_step(c);
    case Store:   return store_step(c);
    case Shop:    return shop_step(c);
    case Return:  return return_step(c);
    default:      break;
    }
    return Step::Pass;
}

Step TownAction::dead_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    if (phase != Dead) {
        if (backZone < 0 || !trip()) {           // a trip on its way keeps its way back
            backZone = w.self.deadZone >= 0 ? w.self.deadZone : w.self.zone;
            backPoint = c.bot.anchorZone == backZone;
            backX = c.bot.anchorX;
            backY = c.bot.anchorY;
        }
        phase = Dead;
        fromDeath_ = true;
        why = "died";
        townSentAt_ = 0;
        nav.stop();
        sub_ = 0;
        c.bot.note(c.now, fmt("died in %s: going to town in %d s", w.data()->atlas().map_label(w.self.zone).c_str(),
                              deathDelaySec));
    }
    const uint64_t at = w.self.deadAt ? w.self.deadAt : c.now;
    const uint64_t ready = at + uint64_t(std::max(0, deathDelaySec)) * 1000 * kFtMs;
    if (c.now < ready) {
        status = fmt("dead - to town in %.0f s", double(ready - c.now) / 1e7);
        return Step::Wait;
    }
    if (!townSentAt_ || ft_older(c.now, townSentAt_, 10000)) {
        townSentAt_ = c.now;
        status = "going to town";
        return c.act(act::go_to_town());
    }
    status = "waiting for the town";
    return Step::Wait;
}

Step TownAction::recover_step(BotCtx& c)
{
    const SelfState& me = c.w.self;
    const int hpP = botu::pct(me.hp, me.hpMax), mpP = botu::pct(me.mp, me.mpMax);
    // HP / MP that stop rising while we rest are full (the server's 2 s ticks stop at the top): the max the bot knows can
    // be one a buff gave before the death (a live log: 72 min resting at "26%", never up to 90% of a buffed max)
    if (!restSince_) restSince_ = c.now;
    if (me.hp != restHp_ || me.mp != restMp_) { restHp_ = me.hp; restMp_ = me.mp; restChangeAt_ = c.now; }
    const bool still = c.sitting() && restChangeAt_ && ft_older(c.now, restChangeAt_, 15000);
    const bool tooLong = ft_older(c.now, restSince_, 300000);
    if (!fromDeath_ || (hpP >= recoverHp && mpP >= recoverMp) || still || tooLong || sub_ == 1) {   // (sub_ 1: decided)
        if (fromDeath_ && (still || tooLong) && !(hpP >= recoverHp && mpP >= recoverMp) && sub_ == 0) {
            sub_ = 1;
            c.bot.note(c.now, still ? fmt("resting: HP %d%% / MP %d%% have not risen for 15 s - full (the max known had a buff), on", hpP, mpP)
                                    : fmt("resting 5 min: HP %d%% / MP %d%% - on", hpP, mpP));
        }
        if (c.sitting()) return c.act(act::sit(0));
        next_errand(c);
        status = phase_name(phase);
        return Step::Wait;
    }
    if (!c.sitting() && (!subAt_ || ft_older(c.now, subAt_, 3000))) {
        subAt_ = c.now;
        c.bot.note(c.now, fmt("resting in town: HP %d%%, MP %d%%", hpP, mpP));
        return c.act(act::sit(13));
    }
    status = fmt("resting in town: HP %d%% (to %d%%), MP %d%% (to %d%%)", hpP, recoverHp, mpP, recoverMp);
    return Step::Wait;
}

// ── the warehouse ───────────────────────────────────────────────────────────
Step TownAction::store_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const NpcState& ns = w.npc;
    const Atlas& at = w.data()->atlas();
    const AtlasNpc* n = at.npc(npc_index(w, whKey));
    if (!n) { next_errand(c); return Step::Wait; }
    auto give_up = [&](const std::string& text) {
        c.bot.note(c.now, "warehouse: " + text);
        next_errand(c);
        return Step::Wait;
    };
    switch (sub_) {
    case 0: {                                    // to the keeper
        Navigator::Goal g;
        g.zone = n->zone; g.point = true; g.x = n->x; g.y = n->y; g.within = kNpcRange;
        g.what = "to the warehouse (" + n->name + ")";
        nav.go(g);
        const Step s = nav.step(c);
        if (s != Step::Pass) {
            status = nav.status;
            if (nav.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kGiveUpMs)) return give_up("no way to " + n->name);
            } else lostSince_ = 0;
            return s;
        }
        const Entity* e = npc_entity(c, *n);
        if (!e) {
            if (!subAt_) subAt_ = c.now;
            if (ft_older(c.now, subAt_, 5000)) return give_up(n->name + " is not here");
            status = "looking for " + n->name;
            return Step::Wait;
        }
        if (c.sitting()) return c.act(act::sit(0));
        npcEid_ = e->eid;
        sub_ = 1;
        subAt_ = c.now;
        status = "talking to " + n->name;
        return c.act(act::npc_talk(w, npcEid_));
    }
    case 1:                                      // the dialog (112) -> Warehouse_Start
        if (ns.talkNpc == npcEid_ && ns.talkAt >= subAt_) {
            std::vector<uint16_t> args;
            if (!script_args_in(ns.text, "Warehouse_Start", args)) args = n->warehouseArgs;
            if (args.empty()) args = { 1, 2, 3 };
            sub_ = 2;
            subAt_ = c.now;
            status = "opening the warehouse";
            return c.act(act::npc_script(npcEid_, act::npcfn::Warehouse_Start, args, "open the warehouse"));
        }
        if (ns.talkRefusedAt >= subAt_) { sub_ = 0; subAt_ = 0; status = "too far"; return Step::Wait; }
        if (ft_older(c.now, subAt_, kAnswerMs)) return give_up(n->name + " did not answer");
        status = "talking to " + n->name;
        return Step::Wait;
    case 2:                                      // S2C 137 (open), or a message (115: level 5 / 50 galder)
        if (ns.whOpenAt >= subAt_) {
            sub_ = 3;
            subAt_ = c.now;
            storeUid_ = 0;
            return Step::Wait;
        }
        if ((ns.talkNpc == npcEid_ && ns.talkAt > subAt_) || ns.talkErrorAt >= subAt_ || ft_older(c.now, subAt_, kAnswerMs)) {
            c.bot.note(c.now, "warehouse: it did not open (level 5 and 50 galder needed)");
            next_errand(c);
            return c.act(act::npc_end());
        }
        status = "opening the warehouse";
        return Step::Wait;
    case 3: {                                    // one item at a time: 0x8B -> 140 / 141
        const std::vector<uint8_t> tabs = wh_tabs(w.self.whSlots);
        if (storeUid_) {
            if (ns.whAddAt >= subAt_) {
                storeUid_ = 0;
            } else if (ns.whFailAt >= subAt_ || ft_older(c.now, subAt_, kAnswerMs)) {
                storeTried_[storeUid_].insert(storeTab_);        // the next tab, else leave it
                if (storeTried_[storeUid_].size() >= tabs.size()) storeSkip_.insert(storeUid_);
                storeUid_ = 0;
            } else {
                status = "storing";
                return Step::Wait;
            }
        }
        for (const InvItem& it : w.inventory) {
            if (storeSkip_.count(it.uid) || !storable(w, it)) continue;
            const std::set<uint8_t>& tried = storeTried_[it.uid];
            uint8_t tab = 0;
            for (uint8_t t : tabs) {
                if (tried.count(t)) continue;
                auto cnt = ns.whItems.find(t);
                if (cnt != ns.whItems.end() && cnt->second >= 80 && it.kind != 1) continue;   // full (a stack may join)
                tab = t;
                break;
            }
            if (!tab) { storeSkip_.insert(it.uid); continue; }
            storeUid_ = it.uid;
            storeTab_ = tab;
            subAt_ = c.now;
            return c.act(act::wh_add(w, tab, it.uid, it.count));
        }
        sub_ = 4;
        return c.act(act::wh_close());
    }
    default:
        c.bot.note(c.now, fmt("warehouse: done (%zu left in the bag)", storeSkip_.size()));
        next_errand(c);
        return Step::Wait;
    }
}

// ── the shop: sell, then buy ────────────────────────────────────────────────
Step TownAction::shop_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const NpcState& ns = w.npc;
    const Atlas& at = w.data()->atlas();
    const AtlasNpc* n = at.npc(npc_index(w, shopKey));
    if (!n) { next_errand(c); return Step::Wait; }
    auto give_up = [&](const std::string& text) {
        c.bot.note(c.now, "shop: " + text);
        next_errand(c);
        return Step::Wait;
    };
    switch (sub_) {
    case 0: {                                    // to the shop
        Navigator::Goal g;
        g.zone = n->zone; g.point = true; g.x = n->x; g.y = n->y; g.within = kNpcRange;
        g.what = "to the shop (" + n->name + ")";
        nav.go(g);
        const Step s = nav.step(c);
        if (s != Step::Pass) {
            status = nav.status;
            if (nav.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kGiveUpMs)) return give_up("no way to " + n->name);
            } else lostSince_ = 0;
            return s;
        }
        const Entity* e = npc_entity(c, *n);
        if (!e) {
            if (!subAt_) subAt_ = c.now;
            if (ft_older(c.now, subAt_, 5000)) return give_up(n->name + " is not here");
            status = "looking for " + n->name;
            return Step::Wait;
        }
        if (c.sitting()) return c.act(act::sit(0));
        npcEid_ = e->eid;
        sub_ = 1;
        subAt_ = c.now;
        status = "opening " + n->name;
        return c.act(act::shop_open(w, npcEid_));
    }
    case 1:                                      // S2C 0xA8
        if (ns.shop == npcEid_ && ns.shopAt >= subAt_) { sub_ = 2; subAt_ = 0; return Step::Wait; }
        if (ft_older(c.now, subAt_, kAnswerMs)) return give_up(n->name + " did not open");
        status = "opening " + n->name;
        return Step::Wait;
    case 2: {                                    // sell, up to 20 lines a request
        if (subAt_) {                            // a sale is out
            if (ns.soldAt >= subAt_) { subAt_ = 0; }
            else if (ns.sellFailAt >= subAt_ || ft_older(c.now, subAt_, kAnswerMs)) {
                c.bot.note(c.now, "shop: a sale was refused");
                sellDone_ = true;
                subAt_ = 0;
            } else { status = "selling"; return Step::Wait; }
        }
        if (!sellDone_ && ++sellRounds_ <= 15) {
            std::vector<act::SellLine> lines;
            for (const InvItem& it : w.inventory) {
                if (!sellable(w, it) || !it.count) continue;
                lines.push_back({ it.uid, it.itemId, it.count });
                if (lines.size() == 20) break;
            }
            if (!lines.empty()) {
                subAt_ = c.now;
                return c.act(act::shop_sell(w, npcEid_, lines));
            }
        }
        sellDone_ = true;
        sub_ = buyDone_ ? 7 : 4;
        return Step::Wait;
    }
    case 4:                                      // the prices: C2S 0xA4 -> S2C 0x47
        sub_ = 5;
        subAt_ = c.now;
        return c.act(act::shop_prices(npcEid_));
    case 5:
        if ((ns.pricesAt >= subAt_ && ns.pricesFor == npcEid_) || ft_older(c.now, subAt_, kAnswerMs)) { sub_ = 6; subAt_ = 0; }
        status = "reading the prices";
        return Step::Wait;
    case 6: {                                    // buy up to the refill counts
        const bool priced = ns.pricesFor == npcEid_ && !ns.prices.empty();
        if (subAt_) {                            // a purchase is out
            if (ns.boughtAt >= subAt_) { subAt_ = 0; buyTries_ = 0; }
            else if (ns.buyFailAt >= subAt_ || ft_older(c.now, subAt_, kAnswerMs)) {
                subAt_ = 0;
                buyScale_ *= 0.5;                // too heavy (0x4B) / too dear (0x4A): half of it
                if (++buyTries_ >= 4) {
                    c.bot.note(c.now, ns.buyFail == 0x004B ? "shop: too heavy to buy more" : "shop: the purchase was refused");
                    buyDone_ = true;
                }
            } else { status = "buying"; return Step::Wait; }
        }
        if (!buyDone_) {
            int64_t money = w.self.money - keepGelder;
            std::vector<act::ShopLine> lines;
            std::string poor;
            for (const BuyRow& r : buy) {
                if (!r.on || !r.item) continue;
                const bool sold = priced ? ns.prices.count(r.item) != 0
                                         : std::find(n->items.begin(), n->items.end(), r.item) != n->items.end();
                if (!sold) continue;
                const int needN = r.upTo - w.count_item(r.item);
                if (needN <= 0) continue;
                int cnt = std::max(1, int(needN * buyScale_));
                cnt = std::min(cnt, 30000);
                const int64_t price = priced ? ns.prices.at(r.item) : 0;
                if (price > 0) cnt = int(std::min<int64_t>(cnt, std::max<int64_t>(0, money / price)));
                if (cnt <= 0) { poor = w.data()->item_name(r.item); continue; }
                money -= price * cnt;
                lines.push_back({ r.item, uint16_t(cnt) });
                if (lines.size() == 20) break;
            }
            if (!poor.empty() && lines.empty()) c.bot.note(c.now, "shop: not enough galder for " + poor);
            if (!lines.empty()) {
                subAt_ = c.now;
                return c.act(act::shop_buy(w, npcEid_, lines));
            }
        }
        buyDone_ = true;
        sub_ = 7;
        return Step::Wait;
    }
    case 7:                                      // close: back to state 0
        sub_ = 8;
        return c.act(act::shop_close());
    default:
        c.bot.note(c.now, "shop: done");
        next_errand(c);
        return Step::Wait;
    }
}

// ── back ────────────────────────────────────────────────────────────────────
Step TownAction::return_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const Atlas& at = w.data()->atlas();
    if (!manual && (c.bot.quest.enabled || c.bot.hunt.enabled)) {   // they know where to go (an NPC / a map)
        finish(c, fmt("the trip is over: the %s leads the way", c.bot.quest.enabled ? "monster quest" : "item hunt"));
        return Step::Pass;
    }
    if (!manual && returnMode == ReturnStay) {
        finish(c, "the trip is over: staying in town");
        c.bot.stop("stays in town after the trip", true);
        return Step::Wait;
    }
    int zone = backZone >= 0 ? backZone : w.self.zone;
    bool point = backPoint;
    if (!manual && returnMode == ReturnChosen && returnZone > 0 && at.map(returnZone)) {
        point = point && zone == returnZone;
        zone = returnZone;
    }
    Navigator::Goal g;
    g.zone = zone;
    g.point = point;
    g.x = backX;
    g.y = backY;
    g.within = 120;
    g.what = manual ? "travelling" : "back to the hunt";
    nav.go(g);
    const Step s = nav.step(c);
    if (s == Step::Pass) {
        finish(c, fmt("%s %s", manual ? "arrived in" : "back in", at.map_label(w.self.zone).c_str()));
        return Step::Pass;
    }
    status = nav.status;
    return s;
}

// ── the profile ─────────────────────────────────────────────────────────────
void TownAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "onDeath", onDeath);
    botu::kv(o, key(), "deathDelaySec", deathDelaySec);
    botu::kv(o, key(), "recoverHp", recoverHp);
    botu::kv(o, key(), "recoverMp", recoverMp);
    botu::kv(o, key(), "returnMode", returnMode);
    botu::kv(o, key(), "returnZone", returnZone);
    botu::kv(o, key(), "supplyAfterDeath", supplyAfterDeath);
    botu::kv(o, key(), "supply", supply);
    botu::kv(o, key(), "shop", shopKey);
    for (const BuyRow& r : buy) o << key() << ".buy=" << r.item << ',' << r.below << ',' << r.upTo << ',' << r.on << '\n';
    botu::kv(o, key(), "sell", sell);
    sellList.save(o, "town.sellList");
    botu::kv(o, key(), "sellRare", sellRare);
    for (int t = 0; t < ET_Count; ++t) botu::kv(o, key(), fmt("sellEquip%d", t).c_str(), sellTier[t]);
    botu::kv(o, key(), "store", store);
    botu::kv(o, key(), "warehouse", whKey);
    storeList.save(o, "town.storeList");
    botu::kv(o, key(), "bagFull", bagFull);
    botu::kv(o, key(), "onOverweight", onOverweight);
    botu::kv(o, key(), "weightPct", weightPct);
    botu::kv(o, key(), "keepGelder", keepGelder);
    botu::kv(o, key(), "navTeleports", nav.teleports);
    botu::kv(o, key(), "navMaxGelder", nav.maxGelder);
    botu::kv(o, key(), "navPortAd", nav.portAd);
}

void TownAction::load(const std::string& k, const std::string& v)
{
    if (k.compare(0, 9, "sellList.") == 0) { sellList.load(k.substr(9), v); return; }
    if (k.compare(0, 10, "storeList.") == 0) { storeList.load(k.substr(10), v); return; }
    if (k == "onDeath") onDeath = botu::flag(v);
    else if (k == "deathDelaySec") deathDelaySec = std::clamp(int(botu::num(v)), 0, 60);
    else if (k == "recoverHp") recoverHp = std::clamp(int(botu::num(v)), 0, 100);
    else if (k == "recoverMp") recoverMp = std::clamp(int(botu::num(v)), 0, 100);
    else if (k == "returnMode") returnMode = std::clamp(int(botu::num(v)), 0, 2);
    else if (k == "returnZone") returnZone = int(botu::num(v));
    else if (k == "supplyAfterDeath") supplyAfterDeath = botu::flag(v);
    else if (k == "supply") supply = botu::flag(v);
    else if (k == "shop") shopKey = v;
    else if (k == "buy") {
        BuyRow r;
        std::istringstream in(v);
        char comma = 0;
        int on = 1;
        in >> r.item >> comma >> r.below >> comma >> r.upTo >> comma >> on;
        r.on = on != 0;
        if (r.item) buy.push_back(r);
    }
    else if (k == "sell") sell = botu::flag(v);
    else if (k == "sellRare") sellRare = botu::flag(v);
    else if (k.size() == 10 && k.compare(0, 9, "sellEquip") == 0 && k[9] >= '0' && k[9] < char('0' + ET_Count)) sellTier[k[9] - '0'] = botu::flag(v);
    else if (k == "store") store = botu::flag(v);
    else if (k == "warehouse") whKey = v;
    else if (k == "bagFull") bagFull = std::clamp(int(botu::num(v)), 1, 300);
    else if (k == "onOverweight") onOverweight = botu::flag(v);
    else if (k == "weightPct") weightPct = std::clamp(int(botu::num(v)), 0, 100);
    else if (k == "keepGelder") keepGelder = std::max(0, int(botu::num(v)));
    else if (k == "navTeleports") nav.teleports = botu::flag(v);
    else if (k == "navMaxGelder") nav.maxGelder = std::max(0, int(botu::num(v)));
    else if (k == "navPortAd") nav.portAd = botu::flag(v);
}

} // namespace nj
