// Loot: C2S 0x21 [oid][type]. Server rules (ProjectPAULA inventory.cpp / drop.cpp):
//   - a drop belongs to its monster's loot owner (S2C 0x016D, the first attacker) for its first 30 s,
//     and anyone else's pick-up is refused WITHOUT a reply -> only our kills' drops or free items, and
//     an item still on the ground after the walk is left alone for 60 s;
//   - a pick-up ends the running attack -> never while a target is alive;
//   - our own kill's drops are taken between two targets even with other monsters around; free items
//     (someone else's after 30 s, items that were there when we came) only out of a fight;
//   - shadow items (event flag) need the Third Eye (inventory.cpp: refused without it);
//   - galder piles (Type 30) on their own switch; the common gear grades (Old / Strong / Art / Ult.: heavy) left by
//     default, or the checked ones; every other item through the black / whitelist.
// Walls: the server's pick-up walk is a straight line (through walls), the client's avatar path-finds it and
// stays behind when it finds no way (stuck.cpp). So the nearest item is the nearest by the WALK on the map's
// grid (none in another area, none round a long wall); with a wall on the line the bot walks the grid path
// first and asks once the line is open; an item the client could not walk to (StuckGuard) is left for 2 min.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>

namespace nj {
namespace {

constexpr uint64_t kOwnedMs = 30000;
constexpr uint64_t kBanMs = 60000;
constexpr uint64_t kSlackMs = 1500;             // the walk's expected time + this
constexpr double   kDetourRatio = 3.0;          // a walk longer than 3 x the distance + 200: behind a long wall
constexpr double   kDetourSlack = 200.0;
constexpr uint64_t kUnreachBanMs = 120000;      // an item the client could not walk to
constexpr uint64_t kPathMaxMs = 30000;          // the walk round a wall to one item

} // namespace

bool LootAction::lootable(const BotCtx& c, uint16_t oid, bool& own) const
{
    auto it = c.w.groundItems.find(oid);
    if (it == c.w.groundItems.end()) return false;
    const GroundItem& g = it->second;
    auto bad = banned_.find(oid);
    if (bad != banned_.end() && c.now < bad->second) return false;
    if (g.eventFlag && !c.w.third_eye(c.now)) return false;
    own = g.fromDrop && g.owner == c.w.self.eid;
    const bool othersStill = g.fromDrop && g.owner && g.owner != c.w.self.eid && !ft_older(c.now, g.seenAt, kOwnedMs);
    if (othersStill) return false;
    if (c.wanted && std::find(c.wanted->begin(), c.wanted->end(), g.itemId) != c.wanted->end()) return true;   // the hunt's
    if (!enabled) return false;                     // (asked only for the Item Hunt's items)
    const ItemInfo* ii = c.w.data() ? c.w.data()->item(g.itemId) : nullptr;
    if (ii && ii->type == 30) return galder;
    // heavy: nothing but the plan's items (a live report: 90 %+ of the weight in loot while the quests waited)
    if (heavyPct > 0 && heavyPct < 100 && c.w.weight_pct() >= heavyPct) return false;
    if (!equip_allowed(c.w.data(), g.itemId)) return false;
    return items.allows(g.itemId);
}

bool LootAction::equip_allowed(const GameData* gd, uint32_t item) const
{
    const int t = gd ? gd->equip_tier(item) : ET_None;
    if (t == ET_None || items.white.count(item)) return true;
    return !skipEquip && lootTier[t];
}

void LootAction::observe(BotCtx& c)
{
    const StuckGuard::Event& se = c.bot.guard.last();
    if (se.at && se.at != stuckSeen_) {                 // the client could not walk where the server took us
        stuckSeen_ = se.at;
        const uint16_t oid = waiting_ ? waitOid_ : pathOid_;
        if (oid && c.w.groundItems.count(oid)) {
            c.bot.note(c.now, fmt("item #%u: the client could not walk there (a wall): left for 2 min", oid));
            banned_[oid] = c.now + kUnreachBanMs * kFtMs;
        }
        waiting_ = false;
        pathOid_ = 0;
        nav_.interrupted();
    }
    if (!waiting_) return;
    if (!c.w.groundItems.count(waitOid_)) {
        waiting_ = false;                       // picked up (0x22) or gone (0x0F)
    } else if (c.now >= waitUntil_) {
        c.bot.note(c.now, fmt("item #%u stayed on the ground (not ours?): left for 60 s", waitOid_));
        banned_[waitOid_] = c.now + kBanMs * kFtMs;
        waiting_ = false;
    }
}

Step LootAction::step(BotCtx& c)
{
    if (c.travelling) { waiting_ = false; status = "on a trip"; return Step::Pass; }
    if (c.targetMode && !c.targetHere) { waiting_ = false; pathOid_ = 0; status = std::string(c.targetWhat) + ": on the way"; return Step::Pass; }
    if (waiting_) {
        status = fmt("picking up #%u", waitOid_);
        return Step::Wait;
    }
    if (c.bot.attack.engaged(c.w)) { status = "a fight is on"; pathOid_ = 0; return Step::Pass; }
    uint16_t best = 0;
    double bestD = 1e18;
    for (const auto& kv : c.w.groundItems) {
        bool own = false;
        if (!lootable(c, kv.first, own)) continue;
        if (!own && (!othersItems || c.attacked)) continue;
        const double d = botu::dist(kv.second.x, kv.second.y, c.px, c.py);
        if (d > radius) continue;
        const double wd = c.bot.reach.walk(c, kv.second.x, kv.second.y);    // the walk there on the grid
        if (wd < 0 || wd > d * kDetourRatio + kDetourSlack || wd >= bestD) continue;
        bestD = wd;
        best = kv.first;
    }
    if (!best) { status = "nothing to pick up"; pathOid_ = 0; return Step::Pass; }
    if (c.sitting()) return c.act(act::sit(0));
    // a wall on the straight line: walk the grid path first (the server's pick-up walk would cross it)
    const GroundItem& gi = c.w.groundItems.at(best);
    if (!Reach::straight(c, gi.x, gi.y)) {
        if (pathOid_ != best) {
            pathOid_ = best;
            pathSince_ = c.now;
            nav_.interrupted();
        }
        if (ft_older(c.now, pathSince_, kPathMaxMs)) {
            c.bot.note(c.now, fmt("item #%u: no way round the wall: left for 2 min", best));
            banned_[best] = c.now + kUnreachBanMs * kFtMs;
            pathOid_ = 0;
            return Step::Wait;
        }
        const Step s = nav_.walk_to(c, gi.x, gi.y, 24, fmt("walking round a wall to item #%u", best));
        if (s != Step::Pass) { status = nav_.status; return s; }
    }
    pathOid_ = 0;
    waiting_ = true;
    waitOid_ = best;
    const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;
    waitUntil_ = c.now + (kSlackMs + uint64_t(bestD * 1000.0 / speed)) * kFtMs;
    return c.act(act::pick_up(c.w, best, c.bot.run));
}

void LootAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "radius", radius);
    botu::kv(o, key(), "galder", galder);
    botu::kv(o, key(), "heavyPct", heavyPct);
    botu::kv(o, key(), "othersItems", othersItems);
    botu::kv(o, key(), "skipEquip", skipEquip);
    for (int t = 0; t < ET_Count; ++t) botu::kv(o, key(), fmt("equip%d", t).c_str(), lootTier[t]);
    items.save(o, key());
}

void LootAction::load(const std::string& k, const std::string& v)
{
    if (k == "radius") radius = int(botu::num(v));
    else if (k == "galder") galder = botu::flag(v);
    else if (k == "heavyPct") heavyPct = std::clamp(int(botu::num(v)), 0, 100);
    else if (k == "gelderOnly") { if (botu::flag(v)) items.mode = IdFilter::OnlyWhite; }   // (old "galder only")
    else if (k == "othersItems") othersItems = botu::flag(v);
    else if (k == "skipEquip") skipEquip = botu::flag(v);
    else if (k.size() == 6 && k.compare(0, 5, "equip") == 0 && k[5] >= '0' && k[5] < char('0' + ET_Count)) lootTier[k[5] - '0'] = botu::flag(v);
    else items.load(k, v);
}

} // namespace nj
