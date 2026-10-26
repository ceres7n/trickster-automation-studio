// The bot's legs (navigator.h). Server rules (ProjectPAULA game_session.cpp / npc_talk.cpp):
//   C2S 0x18 trusts the client's line (no wall check): every leg is a straight open line of layer 1.
//   C2S 0x1D4 walks from where the server has us with the portal armed; any later 0x18 disarms it.
//   The zone tick takes us when our position AND the 0x1D4 destination are in the rectangle grown by 24.
//   C2S 0x6F needs the NPC within 300 units (else S2C 113); 0x7F runs only in that talk state.
//   Teleport_Start re-checks MinLevel / the money (refused: S2C 116), then 117 + the map change.
#include "app/bot/navigator.h"
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <cmath>

namespace nj {
namespace {

constexpr int      kMaxLeg     = 600;           // units per C2S 0x18
constexpr uint64_t kLegSlackMs = 1500;
constexpr uint64_t kBanMs      = 120000;        // a hop that failed is left out this long
constexpr uint64_t kRetryMs    = 10000;         // no route: plan again after this
constexpr int      kTalkRange  = 200;           // walk this close to an NPC (the server allows 300)

int walk_speed(const BotCtx& c)
{
    const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;
    return c.bot.run ? speed : std::max(40, speed / 2);
}

// The NPC of that species in the map, nearest to where the atlas puts it.
const Entity* find_npc(const WorldModel& w, uint16_t species, int x, int y)
{
    const Entity* best = nullptr;
    double bestD = 1e18;
    for (const auto& kv : w.entities) {
        const Entity& e = kv.second;
        if ((e.kind != EK_NPC && e.kind != EK_SHOP) || e.species != species) continue;
        const double d = botu::dist(e.x, e.y, x, y);
        if (d < bestD) { bestD = d; best = &e; }
    }
    return best && bestD <= 400 ? best : nullptr;
}

std::string first_hop_text(const Atlas& at, const Route& r)
{
    if (r.hops.empty()) return "walking";
    const RouteHop& h = r.hops.front();
    if (h.kind == RouteHop::Teleport) {
        const AtlasNpc* n = at.npc(h.npc);
        return fmt("%s's %s to %s", n ? n->name.c_str() : "an NPC", h.move ? "move" : "teleport", at.map_label(h.toZone).c_str());
    }
    if (h.kind == RouteHop::Item) return std::string(h.advIndex >= 0 ? "the Portable Port AD to " : "a port item to ") + at.map_label(h.toZone);
    return "the portal to " + at.map_label(h.toZone);
}

} // namespace

void Navigator::go(const Goal& g)
{
    const bool same = g.zone == goal_.zone && g.point == goal_.point && g.x == goal_.x && g.y == goal_.y;
    goal_ = g;
    failed = false;
    if (same) return;
    replan_ = true;
    retryAt_ = 0;
    hop_ = HopWalk;
    legs_.clear();
    legSent_ = false;
    walkFails_ = 0;
    walkX_ = walkY_ = -1;
}

void Navigator::stop()
{
    goal_ = Goal{};
    route_ = Route{};
    others_.clear();
    routeZone_ = -1;
    replan_ = true;
    hop_ = HopWalk;
    legs_.clear();
    legSent_ = false;
    failed = false;
    status.clear();
}

void Navigator::interrupted()
{
    if (hop_ == HopScript) return;              // the teleport is the server's now: its map change may come
    hop_ = HopWalk;                             // (a portal walk is sent again from where we are)
    legs_.clear();
    legSent_ = false;
    walkX_ = walkY_ = -1;
}

bool Navigator::client_away(const BotCtx& c, int x, int y, int within) const
{
    if (!c.w.drawn_live(c.now)) return false;
    const SelfState::Drawn& d = c.w.self.drawn;
    return botu::dist(d.x, d.y, x, y) > within;
}

void Navigator::ban(BotCtx& c, const RouteHop& h, const std::string& why, bool moved)
{
    replan_ = true;
    hop_ = HopWalk;
    legs_.clear();
    legSent_ = false;
    if (!moved) {                                       // the client never walked there: the same way again
        c.bot.note(c.now, why + " - the client did not walk there (drawn elsewhere): the same way again");
        return;
    }
    const uint64_t key = Atlas::hop_key(h);
    std::pair<int, uint64_t>& f = hopFails_[key];
    if (f.second && ft_older(c.now, f.second, 300000)) f.first = 0;
    ++f.first;
    f.second = c.now;
    if (f.first < 2) {
        c.bot.note(c.now, why + ": once more");
        return;
    }
    f.first = 0;
    banned_[key] = c.now + kBanMs * kFtMs;
    banTimes_.push_back(c.now);
    banTimes_.erase(std::remove_if(banTimes_.begin(), banTimes_.end(), [&](uint64_t t) { return ft_older(c.now, t, 120000); }),
                    banTimes_.end());
    if (banTimes_.size() >= 3) {                        // the cascade breaker
        banned_.clear();
        banTimes_.clear();
        hopFails_.clear();
        holdUntil_ = c.now + 15000 * kFtMs;
        c.bot.note(c.now, why + " - 3 ways failed within 2 min: the moves are failing, not the ways; no way round, the direct "
                                "one again in 15 s");
        return;
    }
    c.bot.note(c.now, why + ": trying another way");
}

RouteOptions Navigator::base_options(const WorldModel& w)
{
    RouteOptions o;
    o.level = w.self.level;
    o.money = w.self.money;
    o.bagKnown = w.inWorld;
    for (const InvItem& it : w.inventory)
        if (it.equipSlot < 0) o.bag[it.itemId] += std::max<int>(1, it.count);
    o.ftype = w.self.fType;
    return o;
}

RouteOptions Navigator::options(const BotCtx& c) const
{
    RouteOptions o = base_options(c.w);
    o.teleports = teleports;
    o.maxGelder = maxGelder;
    o.portAd = portAd && (!adOffUntil_ || c.now >= adOffUntil_);
    o.speed = walk_speed(c);
    if (goal_.point) { o.goalX = goal_.x; o.goalY = goal_.y; }
    for (const auto& kv : banned_)
        if (c.now < kv.second) o.banned.insert(kv.first);
    return o;
}

// The quickest route from where we are: every way out of this map is tried (Atlas::alternatives), the
// quickest one is taken and the next one is said in the log, so a shorter way never goes unseen.
void Navigator::plan(BotCtx& c)
{
    replan_ = false;
    routeZone_ = c.w.self.zone;
    hop_ = HopWalk;
    legs_.clear();
    legSent_ = false;
    walkX_ = walkY_ = -1;
    others_.clear();
    for (auto it = banned_.begin(); it != banned_.end();) it = c.now >= it->second ? banned_.erase(it) : std::next(it);
    const GameData* gd = c.w.data();
    if (!gd) { route_ = Route{}; route_.why = "no game data"; return; }
    const Atlas& at = gd->atlas();
    const RouteOptions o = options(c);
    route_ = at.route(*gd, c.w.self.zone, c.px, c.py, goal_.zone, o);
    noWay_ = false;
    if (!route_.ok) {
        if (c.w.self.zone == goal_.zone) {       // no grid / no way: walk there straight and see
            route_ = Route{};
            route_.ok = true;
            return;
        }
        noWay_ = true;                          // (not only because a hop that failed is left out for now)
        if (!o.banned.empty()) {
            RouteOptions o2 = o;
            o2.banned.clear();
            noWay_ = !at.route(*gd, c.w.self.zone, c.px, c.py, goal_.zone, o2).ok;
        }
        retryAt_ = c.now + kRetryMs * kFtMs;
        c.bot.note(c.now, "no route: " + route_.why);
        return;
    }
    if (route_.hops.empty()) return;            // a walk in this map
    // ways left out for a while: never a detour much longer than the direct way — that one again instead
    if (!o.banned.empty()) {
        RouteOptions o2 = o;
        o2.banned.clear();
        const Route direct = at.route(*gd, c.w.self.zone, c.px, c.py, goal_.zone, o2);
        if (direct.ok && route_.secs > direct.secs * 1.5 + 30.0) {
            if (!detourNoteAt_ || ft_older(c.now, detourNoteAt_, 30000)) {
                detourNoteAt_ = c.now;
                c.bot.note(c.now, fmt("the way round would take ~%.0f s (%zu maps) against ~%.0f s (%zu) direct: the direct way again",
                                      route_.secs, route_.hops.size(), direct.secs, direct.hops.size()));
            }
            banned_.clear();
            route_ = direct;
        }
    }
    others_ = at.alternatives(*gd, c.w.self.zone, c.px, c.py, goal_.zone, o, 4);
    std::string via;
    for (const RouteHop& h : route_.hops) {
        if (!via.empty()) via += " > ";
        via += at.map_label(h.toZone);
        if (h.kind == RouteHop::Teleport) via += h.move ? std::string(" (NPC move)") : fmt(" (teleport %dg)", h.cost);
        if (h.kind == RouteHop::Item) via += " (" + gd->item_name(h.item) + ")";
    }
    std::string next;
    for (const Route& r : others_) {
        if (r.hops.empty() || Atlas::hop_key(r.hops.front()) == Atlas::hop_key(route_.hops.front())) continue;
        next = fmt("; next best ~%.0f s by %s", r.secs, first_hop_text(at, r).c_str());
        break;
    }
    c.bot.note(c.now, fmt("route to %s: %zu map%s, ~%.0f s: %s%s", at.map_label(goal_.zone).c_str(), route_.hops.size(),
                          route_.hops.size() == 1 ? "" : "s", route_.secs, via.c_str(), next.c_str()));
}

Step Navigator::step(BotCtx& c)
{
    if (!active()) return Step::Pass;
    const WorldModel& w = c.w;
    if (w.changingZone) { status = "changing map"; return Step::Wait; }
    if (routeZone_ != w.self.zone) {            // a new map (the hop worked, or we were moved): plan from here
        replan_ = true;
        retryAt_ = 0;
        talkRefused_ = 0;
    }
    if (c.now < holdUntil_) { status = "waiting: the moves keep failing (the client may be stuck)"; return Step::Wait; }
    if (replan_) {
        if (c.now < retryAt_) { status = "no route: " + route_.why; return Step::Wait; }
        plan(c);
    }
    if (route_.ok && !route_.hops.empty() && route_.hops.front().fromZone == w.self.zone) {
        failed = false;
        const RouteHop& h = route_.hops.front();
        return h.kind == RouteHop::Portal ? portal_hop(c, h) : h.kind == RouteHop::Item ? item_hop(c, h) : teleport_hop(c, h);
    }
    if (w.self.zone == goal_.zone) {
        failed = false;
        if (!goal_.point || botu::dist(c.px, c.py, goal_.x, goal_.y) <= goal_.within) {
            status = "arrived";
            legs_.clear();
            return Step::Pass;
        }
        return walk_to(c, goal_.x, goal_.y, goal_.within, goal_.what.empty() ? "walking there" : goal_.what);
    }
    failed = true;
    status = "no route: " + route_.why;
    if (c.now >= retryAt_) replan_ = true;
    return Step::Wait;
}

// ── a portal: the grid path into its rectangle, the last leg as C2S 0x1D4 ──
Step Navigator::portal_hop(BotCtx& c, const RouteHop& h)
{
    const GameData* gd = c.w.data();
    const AtlasMap* m = gd->atlas().map(h.fromZone);
    if (!m || h.portal < 0 || h.portal >= (int)m->portals.size()) { replan_ = true; return Step::Wait; }
    const AtlasPortal& p = m->portals[size_t(h.portal)];
    const std::string to = gd->atlas().map_label(h.toZone);
    if (hop_ == HopEnter) {
        if (c.now < hopDeadline_) { status = "going through the portal to " + to; return Step::Wait; }
        const int half = std::max(p.x2 - p.x1, p.y2 - p.y1) / 2;
        ban(c, h, "the portal to " + to + " did not take us", !client_away(c, p.cx(), p.cy(), half + 120));
        return Step::Wait;
    }
    WalkTarget t;
    t.portal = true;
    t.rect = p;
    t.x = p.cx();
    t.y = p.cy();
    const Step s = walk(c, t, "walking to the portal to " + to);
    if (s == Step::Act && c.out.op == 0x01D4) {
        hop_ = HopEnter;
        hopAt_ = c.now;
        hopDeadline_ = legEnd_ + 5000 * kFtMs;
        legs_.clear();
        legSent_ = false;
        status = "into the portal to " + to;
    }
    return s;
}

// ── an NPC teleport: to the NPC, talk (0x6F -> 112), Teleport_Start [slot] (0x7F -> 117 + the change) ──
Step Navigator::teleport_hop(BotCtx& c, const RouteHop& h)
{
    const GameData* gd = c.w.data();
    const AtlasNpc* n = gd->atlas().npc(h.npc);
    if (!n) { replan_ = true; return Step::Wait; }
    const std::string to = gd->atlas().map_label(h.toZone);
    const NpcState& ns = c.w.npc;
    if (hop_ == HopTalk) {
        if (ns.talkNpc == hopNpc_ && ns.talkAt >= hopAt_) {
            hop_ = HopScript;
            hopAt_ = c.now;
            hopDeadline_ = c.now + 8000 * kFtMs;
            status = "teleporting to " + to;
            const TeleportSlot* sl = h.slot >= 0 && size_t(h.slot) < n->teleports.size() ? &n->teleports[size_t(h.slot)] : nullptr;
            if (sl && sl->func)                         // Move_Complete (the move 2 s after its message) / Random_Move
                return c.act(act::npc_script(hopNpc_, sl->func, sl->args,
                                             sl->random ? fmt("random move (%.0f %% %s)", sl->chance * 100.0, to.c_str())
                                                        : fmt("NPC move to %s", to.c_str())));
            return c.act(act::npc_script(hopNpc_, act::npcfn::Teleport_Start, { uint16_t(h.slot) },
                                         fmt("teleport to %s (%d galder)", to.c_str(), h.cost)));
        }
        if (ns.talkRefusedAt >= hopAt_) {       // S2C 113: farther than 300 for the server — closer
            hop_ = HopWalk;
            if (++talkRefused_ >= 3) { talkRefused_ = 0; ban(c, h, n->name + " refuses to talk"); }
            status = "too far from " + n->name;
            return Step::Wait;
        }
        if (c.now < hopDeadline_) { status = "talking to " + n->name; return Step::Wait; }
        ban(c, h, n->name + " did not answer", !client_away(c, n->x, n->y, 320));
        return Step::Wait;
    }
    if (hop_ == HopScript) {
        if (ns.talkErrorAt >= hopAt_ || c.now >= hopDeadline_) {
            ban(c, h, fmt("%s did not teleport us to %s", n->name.c_str(), to.c_str()));
            return c.act(act::npc_end());
        }
        status = "teleporting to " + to;
        return Step::Wait;
    }
    const Entity* e = find_npc(c.w, n->species, n->x, n->y);
    const int ex = e ? e->x : n->x, ey = e ? e->y : n->y;
    const int range = talkRefused_ ? 70 : kTalkRange;
    if (botu::dist(c.px, c.py, ex, ey) > range) {
        npcMissingSince_ = 0;
        const Step s = walk_to(c, ex, ey, range - 40, "walking to " + n->name + " (teleport to " + to + ")");
        if (s != Step::Pass) return s;
    }
    if (!e) {
        if (!npcMissingSince_) npcMissingSince_ = c.now;
        if (!ft_older(c.now, npcMissingSince_, 4000)) { status = "looking for " + n->name; return Step::Wait; }
        npcMissingSince_ = 0;
        int npcs = 0;
        for (const auto& kv : c.w.entities) npcs += kv.second.kind == EK_NPC || kv.second.kind == EK_SHOP;
        ban(c, h, fmt("%s is not here (%d NPCs seen in this map)", n->name.c_str(), npcs), !client_away(c, n->x, n->y, 450));
        return Step::Wait;
    }
    if (c.sitting()) return c.act(act::sit(0));
    hop_ = HopTalk;
    hopNpc_ = e->eid;
    hopAt_ = c.now;
    hopDeadline_ = c.now + 4000 * kFtMs;
    status = "talking to " + n->name;
    return c.act(act::npc_talk(c.w, e->eid));
}

// ── a port item: C2S 0x57 where we stand -> S2C 151 + the map change 200 ms later (item_use.cpp use_port_item) ──
Step Navigator::item_hop(BotCtx& c, const RouteHop& h)
{
    const GameData* gd = c.w.data();
    const std::string to = gd->atlas().map_label(h.toZone);
    const std::string name = gd->item_name(h.item);
    if (hop_ == HopScript) {
        if (c.now < hopDeadline_) { status = "porting to " + to; return Step::Wait; }
        if (h.advIndex >= 0) {                          // (S2C 566 / 567 / 799 [4]): not one of its 31 ways, 5 min
            adOffUntil_ = c.now + 300000 * kFtMs;
            hop_ = HopWalk;
            replan_ = true;
            c.bot.note(c.now, name + " did not port us to " + to + ": no Port AD for 5 min");
            return Step::Wait;
        }
        ban(c, h, name + " did not port us to " + to);
        return Step::Wait;
    }
    const InvItem* it = nullptr;
    for (const InvItem& x : c.w.inventory)
        if (x.itemId == h.item && x.equipSlot < 0) { it = &x; break; }
    if (!it) { replan_ = true; return Step::Wait; }
    if (c.sitting()) return c.act(act::sit(0));
    if (c.w.npc.talkNpc) return c.act(act::npc_end());
    Action a;
    if (h.advIndex >= 0) {                              // the Portable Port AD: [u32 ZoneIndex]
        const uint32_t idx = uint32_t(h.advIndex);
        a = act::use_item_custom(c.w, it->uid, { uint8_t(idx), uint8_t(idx >> 8), uint8_t(idx >> 16), uint8_t(idx >> 24) }, "to " + to);
    } else {
        a = act::use_item(c.w, it->uid);
    }
    if (!a.ok()) { ban(c, h, name + ": " + a.error); return Step::Wait; }
    hop_ = HopScript;
    hopAt_ = c.now;
    hopDeadline_ = c.now + 6000 * kFtMs;
    status = "porting to " + to;
    c.bot.note(c.now, h.advIndex >= 0 ? fmt("%s to %s (a shortcut)", name.c_str(), to.c_str())
                                      : fmt("using %s to get to %s (no other way)", name.c_str(), to.c_str()));
    return c.act(a);
}

// ── a walk inside the map: the grid path as straight legs ──
Step Navigator::walk(BotCtx& c, const WalkTarget& t, const std::string& what)
{
    const WorldModel& w = c.w;
    const AtlasPortal& r = t.rect;
    if (t.portal) {
        // already in the rectangle (grown by 16): arm it where we stand
        if (c.px >= r.x1 - 16 && c.px < r.x2 + 16 && c.py >= r.y1 - 16 && c.py < r.y2 + 16) {
            if (c.sitting()) return c.act(act::sit(0));
            legEnd_ = c.now + 1000 * kFtMs;
            return c.act(act::portal_walk(std::clamp(c.px, r.x1, std::max(r.x1, r.x2 - 1)),
                                          std::clamp(c.py, r.y1, std::max(r.y1, r.y2 - 1)), c.bot.run));
        }
    } else if (botu::dist(c.px, c.py, t.x, t.y) <= t.within) {
        legs_.clear();
        legSent_ = false;
        return Step::Pass;
    }
    const bool newTarget = t.x != walkX_ || t.y != walkY_ || t.within != walkWithin_ || w.self.zone != walkZone_ ||
                           t.portal != walkPortal_;
    if (newTarget || legs_.empty()) {
        if (c.now < walkRetryAt_ && !newTarget) { status = what + " (no path yet)"; return Step::Wait; }
        legs_.clear();
        legI_ = 0;
        legSent_ = false;
        walkX_ = t.x; walkY_ = t.y; walkWithin_ = t.within; walkZone_ = w.self.zone; walkPortal_ = t.portal;
        const MapGrid* g = w.data() ? w.data()->map(w.self.zone) : nullptr;
        if (g) {
            std::string why;
            bool noCell = false;
            bool ok = t.portal ? Atlas::path_rect(*g, c.px, c.py, r, kMaxLeg, legs_, &why, &noCell)
                               : Atlas::path(*g, c.px, c.py, t.x, t.y, t.within, kMaxLeg, legs_, &why);
            if (!ok && noCell) {                        // a portal over a wall: as near as the cells go, then into it
                ok = Atlas::path(*g, c.px, c.py, t.x, t.y, 64, kMaxLeg, legs_, &why);
                if (ok) legs_.push_back({ t.x, t.y });
            }
            if (!ok) {
                legs_.clear();
                ++walkFails_;
                walkRetryAt_ = c.now + 3000 * kFtMs;
                status = what + ": " + (why.empty() ? std::string("no path") : why);
                if (walkFails_ == 3) c.bot.note(c.now, status);
                return Step::Wait;
            }
            if (legs_.empty()) {
                if (!t.portal) return Step::Pass;       // as close as the open cells get
                legs_.push_back({ t.x, t.y });
            }
        } else {
            legs_.push_back({ t.x, t.y });              // no map file: one straight leg, blind
        }
    }
    if (legSent_) {
        const auto& L = legs_[legI_];
        const bool there = botu::dist(c.px, c.py, L.first, L.second) < 12.0;
        if (!there && c.now < legEnd_) { status = what; return Step::Wait; }
        if (!there) {                                   // stopped short (a stop, a fight): plan again
            legs_.clear();
            legSent_ = false;
            ++walkFails_;
            return Step::Wait;
        }
        walkFails_ = 0;
        legSent_ = false;
        if (++legI_ >= legs_.size()) { legs_.clear(); return Step::Wait; }
    }
    if (c.sitting()) return c.act(act::sit(0));
    const auto& L = legs_[legI_];
    // from where we are (a few units off the leg's planned start) the line may graze a corner: plan again, once
    if (const MapGrid* g = w.data() ? w.data()->map(w.self.zone) : nullptr) {
        if (!legReplanned_ && !g->line_clear(c.px, c.py, L.first, L.second)) {
            legReplanned_ = true;
            legs_.clear();
            legSent_ = false;
            status = what;
            return Step::Wait;
        }
    }
    legReplanned_ = false;
    const bool last = legI_ + 1 == legs_.size();
    legEnd_ = c.now + (uint64_t(botu::dist(c.px, c.py, L.first, L.second) * 1000.0 / walk_speed(c)) + kLegSlackMs) * kFtMs;
    status = what;
    if (t.portal && last) return c.act(act::portal_walk(L.first, L.second, c.bot.run));   // into the portal
    Action a = act::move(w, L.first, L.second, c.bot.run, c.now);
    if (!a.ok()) {                                      // already on that corner
        if (++legI_ >= legs_.size()) legs_.clear();
        return Step::Wait;
    }
    legSent_ = true;
    return c.act(a);
}

} // namespace nj
