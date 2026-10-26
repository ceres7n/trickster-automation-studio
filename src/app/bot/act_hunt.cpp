// Item Hunt (bot_actions.h ItemHuntAction): the list's items until the bag holds each one's count.
//   The sources (item_sources.h, the server's data and rolls): Kill = the species of a map that drop an item, per kill
//   the expected count of their mix (each killed as often as the map keeps it: mon_spawn_db); Dig = a map where it is
//   buried, per find its share (a find's item is rolled at the find, whatever its depth).
//   The pace only ranks them (a guess of the bot's pace, not the server's):
//     Kill: a kill takes its fight, 12 / 20 / 40 / 120 s up to 0 / 5 / 10 / more levels over ours, and the walk to
//           the next target, 2 s with 10 or more of them in the map, up to 14 s with one; the bot takes the nearest,
//           so each species of a mix is killed as often as the map keeps it: kills an hour = 3600 / (sum_s n_s/N x
//           fight_s + walk(N)), an item's rate = that x sum_s n_s/N x perKill_s (another species in the mix only
//           takes time from the others, the walks get shorter);
//     Dig:  a probe ~3 s and a find ~8 s more; the cells holding one = ItemPercentage x the share of the depths
//           (10 x [Min/10, Max/10], drill.cpp) the best drill of the bag reaches; no drill = not usable.
//   Smart (make_plan): what is missing of the whole list, as the time to finish it. Each missing item's own rate at
//   each place (a map killing: its best mix of the map's species up to 10 levels over ours; a map digging) and its
//   best rate anywhere (best_i). A place's plan: every missing item it gives, less the ones quicker elsewhere (the
//   way, ~6 min a map, plus left_i / best_i) than the time they add to the farm here, the worst first; the farm =
//   each step the mix of every species dropping one of its items still missing (an item done takes its species out),
//   until all are done; the whole list from here = the way + the farm + the rest one by one. The quickest place is
//   hunted, killing every species of its mix; the current one holds while within 15 % of it (the way costs). The
//   route ahead (for the UI): the same from each place's end. The plan is made again at every count change (and
//   every 20 s).
//   One by one (smart off): the list's items in order, each from its best source (or the chosen one).
//   Kill: the Attack takes only those species, only in that map (Bot::tick targetMode: whatever its lists say,
//   anywhere on the map - one sector); the Roam patrols their spawns. Dig: the Drill works there with its own
//   settings (drill mode). Loot takes the list's items whatever its lists say (even when off) and the Town never
//   sells or stores them. Counted in the bag (count_item: a rare item is one). In a source's map, none of its items
//   for switchMin minutes = left (one by one: for the next source; smart: for 30 minutes). A row's chosen source
//   holds in both modes (smart: that item counts only there). Nothing changes while a dig is open.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/item_sources.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <sstream>

namespace nj {
namespace {

constexpr uint64_t kCacheMs = 30000;            // an item's sources are worked out again after this
constexpr uint64_t kCheckMs = 60000;            // the current source is checked again (drills, level)
constexpr uint64_t kGiveUpMs = 60000;           // no way to the source's map this long: the next source
constexpr uint64_t kReplanMs = 20000;           // smart: the plan is made again this often (and at every count change)
constexpr uint64_t kPenaltyMs = 1800000;        // smart: a map with none for switchMin minutes / no way: left this long
constexpr double   kStay = 1.15;                // smart: the current place holds while its total is within 15 % of the best
constexpr double   kTripHours = 0.1;            // smart: the way to another map (~6 min)
constexpr int      kOverLv = 10;                // (sources(): the note of a species this far over ours; the plan: maxOver)

uint64_t key_of(int kind, int zone) { return uint64_t(uint32_t(kind)) << 32 | uint32_t(zone); }

const DrillInfo* best_drill(const WorldModel& w)
{
    const GameData* gd = w.data();
    const DrillInfo* best = nullptr;
    if (!gd) return nullptr;
    for (const InvItem& it : w.inventory) {
        const DrillInfo* d = gd->drill(it.itemId);
        if (!d || it.count == 0) continue;                  // (a drill's wire count is its life)
        if (!best || d->digDepth > best->digDepth) best = d;
    }
    return best;
}

// the pace (see the top): a kill's fight, the walk to the next target
double fight_secs(int level, int myLv)
{
    const int over = level - myLv;
    return over <= 0 ? 12.0 : over <= 5 ? 20.0 : over <= 10 ? 40.0 : 120.0;
}

double walk_secs(int targets) { return 2.0 + 12.0 * double(std::max(0, 10 - targets)) / 10.0; }

// A mix of species (count, level, per kill of each item): each killed as often as it is there; the items an hour.
struct MixMob { int n = 0, level = 0; std::map<uint32_t, double> per; };
template <typename Get>
void mix_rates(const std::vector<uint16_t>& mix, Get get, int myLv, std::map<uint32_t, double>& out)
{
    out.clear();
    int n = 0;
    double fight = 0.0;
    for (uint16_t s : mix) { const MixMob& x = get(s); n += x.n; fight += double(x.n) * fight_secs(x.level, myLv); }
    if (n <= 0) return;
    const double kills = 3600.0 / (fight / n + walk_secs(n));
    for (uint16_t s : mix) {
        const MixMob& x = get(s);
        for (const auto& q : x.per) out[q.first] += kills * (double(x.n) / n) * q.second;
    }
}

// finds an hour digging there with that drill (0 = it reaches none of the items); reach = the share of the depths
double finds_per_hour(const MapDigInfo& md, const DrillInfo& drill, double* reach = nullptr)
{
    const int lo = md.minDepth / 10, hi = std::max(md.minDepth, md.maxDepth) / 10;
    const double r = hi >= lo ? std::clamp(double(drill.digDepth / 10 - lo + 1) / double(hi - lo + 1), 0.0, 1.0) : 1.0;
    if (reach) *reach = r;
    const double cell = double(md.itemPct) / 100.0 * r;
    return cell > 0.0 ? 3600.0 / (3.0 / cell + 8.0) : 0.0;
}

// An item's own best mix of a map's species that drop it: in order of their per kill, the prefix with the best rate.
void best_mix_for(uint32_t item, const std::map<uint16_t, MixMob>& sps, int myLv, std::vector<uint16_t>& best, double& rate)
{
    std::vector<std::pair<double, uint16_t>> order;
    for (const auto& s : sps) {
        auto q = s.second.per.find(item);
        if (q != s.second.per.end() && q->second > 0.0) order.push_back({ q->second, s.first });
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    best.clear();
    rate = 0.0;
    std::vector<uint16_t> mix;
    std::map<uint32_t, double> r;
    for (const auto& o : order) {
        mix.push_back(o.second);
        mix_rates(mix, [&](uint16_t m) -> const MixMob& { return sps.at(m); }, myLv, r);
        const double v = r.count(item) ? r.at(item) : 0.0;
        if (v > rate * 1.0001 + 1e-12) { rate = v; best = mix; }
    }
}

std::string kill_label(const GameData& gd, const std::vector<uint16_t>& mobs, int zone)
{
    std::string m;
    for (size_t i = 0; i < mobs.size() && i < 3; ++i) m += (i ? ", " : "") + gd.mob_name(mobs[i]);
    if (mobs.size() > 3) m += fmt(" (+%zu)", mobs.size() - 3);
    return "kill " + m + " in " + gd.map_name(zone);
}

} // namespace

std::vector<HuntSource> ItemHuntAction::sources(const WorldModel& w, uint32_t item)
{
    std::vector<HuntSource> out;
    const GameData* gd = w.data();
    if (!gd || !item) return out;
    const ItemSources& is = gd->item_sources();
    const int myLv = std::max(1, w.self.level);

    // Kill: per map, its species that drop it (their best mix for it)
    std::map<int, std::map<uint16_t, MixMob>> zones;
    for (const ItemMobSource& m : is.mobs(item)) {
        if (botu::shadow_monster(uint16_t(m.mob))) continue;            // (only with the Third Eye)
        const MobInfo* mi = gd->mob(m.mob);
        for (const MobSpawn& sp : gd->spawns_of(m.mob)) {
            MixMob& x = zones[sp.zone][uint16_t(m.mob)];
            x.n = sp.count;
            x.level = mi ? mi->level : 0;
            x.per[item] = m.perKill;
        }
    }
    for (const auto& z : zones) {
        HuntSource s;
        s.kind = HuntSource::Kill;
        s.zone = z.first;
        best_mix_for(item, z.second, myLv, s.mobs, s.perHour);
        if (s.mobs.empty()) continue;
        double per = 0.0;
        for (uint16_t m : s.mobs) {
            const MixMob& x = z.second.at(m);
            s.count += x.n;
            s.level = std::max(s.level, x.level);
            per += double(x.n) * x.per.at(item);
        }
        s.perUnit = s.count > 0 ? per / s.count : 0.0;
        if (s.level - myLv > kOverLv) s.why = fmt("Lv %d, %d over yours", s.level, s.level - myLv);
        s.label = kill_label(*gd, s.mobs, s.zone);
        out.push_back(std::move(s));
    }

    // Dig: the maps where it is buried, with the best drill of the bag
    const DrillInfo* drill = best_drill(w);
    for (const ItemDigSource& d : is.digs(item)) {
        const MapDigInfo* md = gd->map_dig(d.zone);
        if (!md || md->itemPct <= 0.0f) continue;
        HuntSource s;
        s.kind = HuntSource::Dig;
        s.zone = d.zone;
        s.perUnit = d.perFind;
        if (!drill) {
            s.usable = false;
            s.why = "no drill in the bag";
        } else {
            double reach = 0.0;
            s.perHour = finds_per_hour(*md, *drill, &reach) * d.perFind;
            if (reach <= 0.0) {
                s.usable = false;
                s.why = fmt("the drill reaches %d, the items lie %d..%d deep", drill->digDepth, md->minDepth, md->maxDepth);
            } else if (reach < 1.0) {
                s.why = fmt("the drill reaches %.0f %% of the depths", reach * 100.0);
            }
        }
        s.label = "dig in " + gd->map_name(s.zone);
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const HuntSource& a, const HuntSource& b) {
        if (a.usable != b.usable) return a.usable;
        return a.perHour > b.perHour;
    });
    return out;
}

// ── the smart plan ──────────────────────────────────────────────────────────
// The places (a map killing / a map digging), each missing item's own rate at each (its best mix of the map's species,
// or the dig) and its best anywhere; then per place the items worth getting there and the time to finish the list.
HuntPlan ItemHuntAction::make_plan(const WorldModel& w, const std::set<uint64_t>& excluded) const
{
    HuntPlan p;
    const GameData* gd = w.data();
    if (!gd) return p;
    for (size_t i = 0; i < list.size(); ++i) {
        const int miss = list[i].want - have(w, i);
        if (miss > 0) p.need[list[i].item] += miss;
    }
    if (p.need.empty()) return p;
    const ItemSources& is = gd->item_sources();
    const int myLv = std::max(1, w.self.level);
    std::map<uint32_t, const HuntItem*> pin;                // a row's chosen source: that item counts only there
    for (const HuntItem& h : list) if (h.srcKind >= 0) pin[h.item] = &h;
    auto counts_at = [&](uint32_t item, int kind, int zone) {
        auto q = pin.find(item);
        return !excluded.count(key_of(kind, zone)) && (q == pin.end() || (q->second->srcKind == kind && q->second->srcZone == zone));
    };

    struct Place {
        int kind = HuntSource::Kill, zone = 0;
        std::map<uint16_t, MixMob> sps;                     // kill: the species (up to kOverLv over us) that drop missing items
        std::map<uint32_t, std::vector<uint16_t>> mixOf;    // kill: each item's own best mix here
        std::map<uint32_t, double> solo;                    // each item's rate here alone (its best mix / the dig)
        std::set<uint16_t> strong;                          // kill: species too strong for us
    };
    std::map<uint64_t, Place> places;
    // a map no way reaches from here now (a teleport's level, an item that opens it) is no place to go
    const RouteOptions ro = Navigator::base_options(w);
    std::map<int, bool> reachMemo;
    auto reachable = [&](int zone) {
        auto it = reachMemo.find(zone);
        if (it != reachMemo.end()) return it->second;
        return reachMemo[zone] = w.self.zone <= 0 || gd->atlas().zone_reachable(w.self.zone, zone, ro);
    };
    // the species up to `limit` levels over ours that drop the item, in maps a way reaches
    auto add_mobs = [&](uint32_t item, int limit) {
        bool any = false;
        for (const ItemMobSource& m : is.mobs(item)) {
            if (botu::shadow_monster(uint16_t(m.mob))) continue;
            const MobInfo* mi = gd->mob(m.mob);
            const int lv = mi ? mi->level : 0;
            for (const MobSpawn& sp : gd->spawns_of(m.mob)) {
                if (!counts_at(item, HuntSource::Kill, sp.zone) || !reachable(sp.zone)) continue;
                Place& pl = places[key_of(HuntSource::Kill, sp.zone)];
                pl.kind = HuntSource::Kill;
                pl.zone = sp.zone;
                if (lv - myLv > limit) { pl.strong.insert(uint16_t(m.mob)); continue; }
                pl.strong.erase(uint16_t(m.mob));
                MixMob& x = pl.sps[uint16_t(m.mob)];
                x.n = sp.count;
                x.level = lv;
                x.per[item] = m.perKill;
                any = true;
            }
        }
        return any;
    };
    for (const auto& nd : p.need) {
        if (add_mobs(nd.first, maxOver)) continue;
        // only stronger ones drop it (a live log: Swamp's quest items, monsters Lv 164..219 for a level-145 character,
        // "no usable source" and the stop): its weakest ones (and those up to 10 levels over them)
        int weakest = INT_MAX;
        for (const ItemMobSource& m : is.mobs(nd.first)) {
            if (botu::shadow_monster(uint16_t(m.mob))) continue;
            const MobInfo* mi = gd->mob(m.mob);
            bool there = false;
            for (const MobSpawn& sp : gd->spawns_of(m.mob)) there = there || (counts_at(nd.first, HuntSource::Kill, sp.zone) && reachable(sp.zone));
            if (there) weakest = std::min(weakest, (mi ? mi->level : 0) - myLv);
        }
        if (weakest != INT_MAX && add_mobs(nd.first, weakest + 10)) p.over[nd.first] = weakest;
    }
    for (auto& kv : places)
        for (const auto& nd : p.need) {
            std::vector<uint16_t> mix;
            double r = 0.0;
            best_mix_for(nd.first, kv.second.sps, myLv, mix, r);
            if (r > 0.0) { kv.second.mixOf[nd.first] = mix; kv.second.solo[nd.first] = r; }
        }
    if (const DrillInfo* drill = best_drill(w)) {
        for (const auto& nd : p.need)
            for (const ItemDigSource& d : is.digs(nd.first)) {
                if (!counts_at(nd.first, HuntSource::Dig, d.zone) || !reachable(d.zone)) continue;
                const MapDigInfo* md = gd->map_dig(d.zone);
                const double fph = md ? finds_per_hour(*md, *drill) : 0.0;
                if (fph <= 0.0) continue;
                Place& pl = places[key_of(HuntSource::Dig, d.zone)];
                pl.kind = HuntSource::Dig;
                pl.zone = d.zone;
                pl.solo[nd.first] = fph * d.perFind;
            }
    }
    for (const auto& nd : p.need) p.best[nd.first] = 0.0;
    for (const auto& kv : places)
        for (const auto& q : kv.second.solo) p.best[q.first] = std::max(p.best[q.first], q.second);

    // The farm at a place: each step the mix of the items still missing there (an item done takes its species out),
    // until each is done. The hours (1e18 = never); the first step's rates and species.
    auto farm = [&](const Place& pl, const std::vector<uint32_t>& items, const std::map<uint32_t, double>& left,
                    std::map<uint32_t, double>* rate0, std::vector<uint16_t>* mix0) {
        std::map<uint32_t, double> l;
        for (uint32_t i : items) {
            auto q = left.find(i);
            if (q != left.end() && q->second > 1e-9) l[i] = q->second;
        }
        double t = 0.0;
        for (int step = 0; step < 16 && !l.empty(); ++step) {
            std::map<uint32_t, double> r;
            if (pl.kind == HuntSource::Dig) {
                for (const auto& q : l) { auto s = pl.solo.find(q.first); r[q.first] = s == pl.solo.end() ? 0.0 : s->second; }
            } else {
                std::vector<uint16_t> mix;
                for (const auto& q : l) {
                    auto mo = pl.mixOf.find(q.first);
                    if (mo == pl.mixOf.end()) continue;
                    for (uint16_t m : mo->second) if (std::find(mix.begin(), mix.end(), m) == mix.end()) mix.push_back(m);
                }
                mix_rates(mix, [&](uint16_t m) -> const MixMob& { return pl.sps.at(m); }, myLv, r);
                if (step == 0 && mix0) *mix0 = mix;
            }
            if (step == 0 && rate0) {
                rate0->clear();
                for (const auto& q : l) { auto v = r.find(q.first); if (v != r.end() && v->second > 0.0) (*rate0)[q.first] = v->second; }
            }
            double dt = 1e18;
            for (const auto& q : l) {
                auto v = r.find(q.first);
                if (v != r.end() && v->second > 0.0) dt = std::min(dt, q.second / v->second);
            }
            if (dt >= 1e17) return 1e18;
            t += dt;
            for (auto it = l.begin(); it != l.end();) {
                auto v = r.find(it->first);
                it->second -= (v == r.end() ? 0.0 : v->second) * dt;
                it = it->second <= 1e-6 ? l.erase(it) : std::next(it);
            }
        }
        return t;
    };
    // an item alone: the way to its best place + its farm there
    auto alone = [&](uint32_t i, const std::map<uint32_t, double>& left) { return kTripHours + left.at(i) / p.best.at(i); };
    // A place's plan from `fromZone`: every missing item it gives, less the ones quicker elsewhere (the way included),
    // the worst first; the farm there; the whole list = the way + the farm + the rest one by one.
    auto evaluate = [&](const Place& pl, const std::map<uint32_t, double>& left, int fromZone, HuntPlan::Cand& c) {
        std::vector<uint32_t> cov;
        for (const auto& q : pl.solo) {
            auto l = left.find(q.first);
            if (l != left.end() && l->second > 1e-9 && q.second > 0.0) cov.push_back(q.first);
        }
        if (cov.empty()) return false;
        while (cov.size() > 1) {
            const double base = farm(pl, cov, left, nullptr, nullptr);
            int worst = -1;
            double worstGain = 0.0;
            for (size_t k = 0; k < cov.size(); ++k) {
                if (pl.solo.at(cov[k]) >= p.best.at(cov[k]) * 0.999) continue;     // its best place: here anyway
                std::vector<uint32_t> less = cov;
                less.erase(less.begin() + long(k));
                const double gain = base - farm(pl, less, left, nullptr, nullptr) - alone(cov[k], left);
                if (gain > worstGain) { worstGain = gain; worst = int(k); }
            }
            if (worst < 0) break;
            cov.erase(cov.begin() + worst);
        }
        c = HuntPlan::Cand{};
        c.src.kind = pl.kind;
        c.src.zone = pl.zone;
        c.covered = cov;
        c.farmHours = farm(pl, cov, left, &c.rate, &c.src.mobs);
        if (c.farmHours >= 1e17) return false;
        double rest = 0.0;
        for (const auto& l : left)
            if (l.second > 1e-9 && p.best.at(l.first) > 0.0 && std::find(cov.begin(), cov.end(), l.first) == cov.end())
                rest += alone(l.first, left);
        c.totalHours = (fromZone == pl.zone ? 0.0 : kTripHours) + c.farmHours + rest;
        return true;
    };

    std::map<uint32_t, double> left;
    for (const auto& nd : p.need) left[nd.first] = nd.second;
    std::vector<uint64_t> keys;                             // cands[i] <-> places[keys[i]]
    for (const auto& kv : places) {
        HuntPlan::Cand c;
        if (!evaluate(kv.second, left, w.self.zone, c)) continue;
        const Place& pl = kv.second;
        if (pl.kind == HuntSource::Kill) {
            for (const auto& s : pl.sps)
                if (std::find(c.src.mobs.begin(), c.src.mobs.end(), s.first) == c.src.mobs.end()) c.skipped.push_back(s.first);
            for (uint16_t s : pl.strong) c.skipped.push_back(s);
            for (uint16_t m : c.src.mobs) {
                c.src.count += pl.sps.at(m).n;
                c.src.level = std::max(c.src.level, pl.sps.at(m).level);
            }
            c.src.label = kill_label(*gd, c.src.mobs, c.src.zone);
        } else {
            c.src.label = "dig in " + gd->map_name(c.src.zone);
        }
        for (const auto& q : c.rate) c.src.perHour += q.second;
        p.cands.push_back(std::move(c));
        keys.push_back(kv.first);
    }
    std::vector<size_t> order(p.cands.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p.cands[a].totalHours < p.cands[b].totalHours; });
    {
        std::vector<HuntPlan::Cand> c2;
        std::vector<uint64_t> k2;
        for (size_t i : order) { c2.push_back(std::move(p.cands[i])); k2.push_back(keys[i]); }
        p.cands = std::move(c2);
        keys = std::move(k2);
    }

    // the route: from where we are, the quickest place each time, its items done there (the 15 best places)
    std::map<uint32_t, double> l = left;
    int zone = w.self.zone;
    const size_t top = std::min<size_t>(15, p.cands.size());
    for (int k = 0; k < 10; ++k) {
        bool any = false;
        for (const auto& q : l) any = any || (q.second > 1e-9 && p.best.at(q.first) > 0.0);
        if (!any) break;
        int bi = -1;
        HuntPlan::Cand bc;
        for (size_t i = 0; i < top; ++i) {
            HuntPlan::Cand c;
            if (!evaluate(places.at(keys[i]), l, zone, c)) continue;
            if (bi < 0 || c.totalHours < bc.totalHours) { bi = int(i); bc = std::move(c); }
        }
        if (bi < 0) break;
        HuntPlan::Step st;
        st.cand = bi;
        st.hours = (zone == bc.src.zone ? 0.0 : kTripHours) + bc.farmHours;
        st.done = bc.covered;
        for (uint32_t i : bc.covered) l[i] = 0.0;
        zone = bc.src.zone;
        p.route.push_back(std::move(st));
    }
    return p;
}

void ItemHuntAction::smart_refresh(const WorldModel& w, uint64_t now, bool digOpen)
{
    std::map<uint32_t, int> need;
    for (size_t i = 0; i < list.size(); ++i) {
        const int miss = list[i].want - have(w, i);
        if (miss > 0) need[list[i].item] += miss;
    }
    if (need.empty()) { cur_ = -1; src_ = HuntSource{}; plan_ = HuntPlan{}; planCand_ = -1; return; }
    if (digOpen && src_.zone) return;                       // (nothing changes while a dig is open)
    if (src_.zone && w.self.zone == src_.zone && !arrivedAt_) arrivedAt_ = now;
    // in its map for switchMin minutes without one of its items: left for a while
    if (src_.zone && switchMin > 0 && arrivedAt_ && w.self.zone == src_.zone &&
        ft_older(now, std::max(arrivedAt_, lastGainAt), uint64_t(switchMin) * 60000)) {
        giveUp_ = fmt("none in %d min: \"%s\" left for 30 min", switchMin, src_.label.c_str());
        penalty_[key_of(src_.kind, src_.zone)] = now + kPenaltyMs * kFtMs;
        src_ = HuntSource{};
        planCand_ = -1;
    }
    if (src_.zone && planCand_ >= 0 && need == planNeed_ && !ft_older(now, planAt_, kReplanMs)) return;
    std::set<uint64_t> excluded;
    for (auto it = penalty_.begin(); it != penalty_.end();) {
        if (now >= it->second) { it = penalty_.erase(it); continue; }
        excluded.insert(it->first);
        ++it;
    }
    plan_ = make_plan(w, excluded);
    if (plan_.cands.empty() && !excluded.empty()) plan_ = make_plan(w);   // (left out for a while: still better than none)
    planAt_ = now;
    planNeed_ = need;
    if (plan_.cands.empty()) { src_ = HuntSource{}; planCand_ = -1; cur_ = -1; return; }
    int pick = 0;                                           // the quickest, unless the current one still holds
    if (src_.zone)
        for (size_t i = 0; i < plan_.cands.size(); ++i)
            if (plan_.cands[i].src.kind == src_.kind && plan_.cands[i].src.zone == src_.zone) {
                if (plan_.cands[i].totalHours <= kStay * plan_.cands[0].totalHours + 0.02) pick = int(i);
                break;
            }
    const HuntPlan::Cand& c = plan_.cands[size_t(pick)];
    if (c.src.kind != src_.kind || c.src.zone != src_.zone) {
        sourceAt = now;
        arrivedAt_ = 0;
        lastGainAt = now;
        srcNote_ = true;
    }
    src_ = c.src;
    planCand_ = pick;
    cur_ = -1;                                              // the list's first missing item it gives
    for (size_t i = 0; i < list.size() && cur_ < 0; ++i)
        if (have(w, i) < list[i].want && c.rate.count(list[i].item)) cur_ = int(i);
    if (cur_ < 0)
        for (size_t i = 0; i < list.size() && cur_ < 0; ++i)
            if (have(w, i) < list[i].want) cur_ = int(i);
}

// ── the run ─────────────────────────────────────────────────────────────────
void ItemHuntAction::reset()
{
    nav_.reset();
    cur_ = -1;
    src_ = HuntSource{};
    plan_ = HuntPlan{};
    planCand_ = -1;
    planAt_ = 0;
    planNeed_.clear();
    penalty_.clear();
    srcNote_ = false;
    giveUp_.clear();
    items_.clear();
    lastHave_.clear();
    tried_.clear();
    lostSince_ = arrivedAt_ = checkAt_ = 0;
    cache_.clear();
    cacheLevel_ = -1;
    gained.clear();
    startedAt = sourceAt = lastGainAt = 0;
    plan.clear();
}

int ItemHuntAction::have(const WorldModel& w, size_t i) const { return i < list.size() ? w.count_item(list[i].item) : 0; }

bool ItemHuntAction::done(const WorldModel& w) const
{
    if (list.empty()) return false;
    for (size_t i = 0; i < list.size(); ++i)
        if (have(w, i) < list[i].want) return false;
    return true;
}

bool ItemHuntAction::serves(uint32_t item) const
{
    if (smart) return planCand_ >= 0 && size_t(planCand_) < plan_.cands.size() && plan_.cands[size_t(planCand_)].rate.count(item);
    return cur_ >= 0 && size_t(cur_) < list.size() && list[size_t(cur_)].item == item;
}

const std::vector<HuntSource>& ItemHuntAction::cached(const WorldModel& w, uint32_t item, uint64_t now)
{
    if (cacheLevel_ != w.self.level) { cache_.clear(); cacheLevel_ = w.self.level; }
    auto& e = cache_[item];
    if (!e.first || ft_older(now, e.first, kCacheMs)) {
        e.first = now ? now : 1;
        e.second = sources(w, item);
    }
    return e.second;
}

void ItemHuntAction::choose(const WorldModel& w, uint64_t now)
{
    checkAt_ = now;
    const HuntItem& hi = list[size_t(cur_)];
    const std::vector<HuntSource>& all = cached(w, hi.item, now);
    const HuntSource* pick = nullptr;
    if (hi.srcKind >= 0)                                    // the chosen one, while it can be used
        for (const HuntSource& s : all)
            if (s.usable && s.kind == hi.srcKind && s.zone == hi.srcZone) { pick = &s; break; }
    if (!pick) {
        for (const HuntSource& s : all)
            if (s.usable && !tried_.count(key_of(s.kind, s.zone))) { pick = &s; break; }
        if (!pick && !tried_.empty()) {                     // every one tried: round again
            tried_.clear();
            for (const HuntSource& s : all) if (s.usable) { pick = &s; break; }
        }
    }
    if (!pick) { src_ = HuntSource{}; return; }
    if (pick->kind != src_.kind || pick->zone != src_.zone) {
        sourceAt = now;
        arrivedAt_ = 0;
        srcNote_ = true;
    }
    src_ = *pick;
}

void ItemHuntAction::refresh(const WorldModel& w, uint64_t now, bool digOpen)
{
    items_.clear();
    for (const HuntItem& h : list) items_.push_back(h.item);
    if (!startedAt) startedAt = now;
    for (const HuntItem& h : list) {                        // the bag: what came in
        const int n = w.count_item(h.item);
        auto it = lastHave_.find(h.item);
        if (it != lastHave_.end() && n > it->second) {
            gained[h.item] += n - it->second;
            if (serves(h.item)) lastGainAt = now;
        }
        lastHave_[h.item] = n;
    }
    if (smart) { smart_refresh(w, now, digOpen); return; }
    plan_ = HuntPlan{};
    planCand_ = -1;
    if (digOpen && cur_ >= 0) return;                       // (nothing changes while a dig is open)
    // one by one: the first item of the list short of its count that has a usable source
    int pick = -1;
    for (size_t i = 0; i < list.size() && pick < 0; ++i) {
        if (have(w, i) >= list[i].want) continue;
        for (const HuntSource& s : cached(w, list[i].item, now))
            if (s.usable) { pick = int(i); break; }
    }
    if (pick != cur_) {
        cur_ = pick;
        tried_.clear();
        src_ = HuntSource{};
        lastGainAt = now;
    }
    if (cur_ < 0) return;
    if (src_.zone && w.self.zone == src_.zone && !arrivedAt_) arrivedAt_ = now;
    // in its map for switchMin minutes without one: the next source
    if (src_.zone && switchMin > 0 && arrivedAt_ && w.self.zone == src_.zone &&
        ft_older(now, std::max(arrivedAt_, lastGainAt), uint64_t(switchMin) * 60000)) {
        giveUp_ = fmt("none in %d min: \"%s\" left for the next source", switchMin, src_.label.c_str());
        tried_.insert(key_of(src_.kind, src_.zone));
        src_ = HuntSource{};
    }
    const HuntItem& hi = list[size_t(cur_)];
    const bool other = hi.srcKind >= 0 && (hi.srcKind != src_.kind || hi.srcZone != src_.zone);
    if (!src_.zone || other || ft_older(now, checkAt_, kCheckMs)) choose(w, now);
}

Step ItemHuntAction::step(BotCtx& c)
{
    const WorldModel& w = c.w;
    if (c.travelling) { status = "a town trip runs"; return Step::Pass; }
    if (!giveUp_.empty()) { c.bot.note(c.now, "item hunt: " + giveUp_); giveUp_.clear(); }
    if (list.empty()) { status = plan = driven ? "nothing to get" : "add the items to hunt (Item Hunt tab)"; return driven ? Step::Pass : Step::Wait; }
    if (done(w)) {
        status = plan = "every item of the list is in the bag";
        if (driven) return Step::Pass;
        if (stopWhenDone) {
            int got = 0;
            for (const auto& g : gained) got += g.second;
            const double mins = startedAt && c.now > startedAt ? double(c.now - startedAt) / 6e8 : 0.0;
            c.bot.stop(fmt("item hunt done: the %zu item(s) of the list are in the bag (%d picked up in %s)", list.size(), got,
                           mins < 60.0 ? fmt("%.0f min", mins).c_str() : fmt("%.1f h", mins / 60.0).c_str()), true);
        }
        return Step::Wait;
    }
    if (cur_ < 0 || !src_.zone) {
        status = plan = "no usable source for the rest of the list";
        if (driven) return Step::Pass;
        c.bot.stop("item hunt: " + plan + " - " + why_none(w));
        return Step::Wait;
    }
    const GameData* gd = w.data();
    const HuntItem& hi = list[size_t(cur_)];
    const std::string name = gd ? gd->item_name(hi.item) : fmt("item %u", hi.item);
    std::string what = name;                                // (smart: every missing item it gives)
    if (smart && planCand_ >= 0) {
        what.clear();
        for (const auto& q : plan_.cands[size_t(planCand_)].rate)
            what += (what.empty() ? "" : ", ") + (gd ? gd->item_name(q.first) : fmt("item %u", q.first));
    }
    if (srcNote_) {
        srcNote_ = false;
        std::string over;                                   // its items only stronger monsters drop
        if (smart && planCand_ >= 0)
            for (const auto& q : plan_.cands[size_t(planCand_)].rate) {
                auto o = plan_.over.find(q.first);
                if (o != plan_.over.end())
                    over += fmt("%s%s: only monsters %d+ levels over yours drop it", over.empty() ? " (" : "; ",
                                gd ? gd->item_name(q.first).c_str() : "?", o->second);
            }
        if (!over.empty()) over += ")";
        if (smart && planCand_ >= 0)
            c.bot.note(c.now, fmt("item hunt: %s for %s (~%.1f h there, the whole list ~%.1f h)%s", src_.label.c_str(), what.c_str(),
                                  plan_.cands[size_t(planCand_)].farmHours, plan_.cands[size_t(planCand_)].totalHours, over.c_str()));
        else
            c.bot.note(c.now, fmt("item hunt: %s -> %s (about %.1f an hour)", name.c_str(), src_.label.c_str(), src_.perHour));
    }
    if (w.self.zone != src_.zone) {
        Navigator::Goal g;
        g.zone = src_.zone;
        g.what = "to " + (gd ? gd->map_name(src_.zone) : fmt("map %d", src_.zone)) + " (" + what + ")";
        nav_.go(g);
        const Step s = nav_.step(c);
        if (s != Step::Pass) {
            status = plan = nav_.status;
            if (nav_.no_route()) {                          // no way between the maps: the next source at once
                abandon_source(c, "no way to " + (gd ? gd->map_name(src_.zone) : std::string("its map")) + " (" + nav_.route().why + ")");
            } else if (nav_.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kGiveUpMs))
                    abandon_source(c, "no way to " + (gd ? gd->map_name(src_.zone) : std::string("its map")));
            } else lostSince_ = 0;
            return s;
        }
    }
    lostSince_ = 0;
    if (smart) status = plan = fmt("%s - for %s", src_.label.c_str(), what.c_str());
    else status = plan = fmt("%s: %d / %d - %s", name.c_str(), have(w, size_t(cur_)), hi.want, src_.label.c_str());
    return Step::Pass;                                      // (the Attack / Loot / Roam, or the Drill, work here)
}

void ItemHuntAction::abandon_source(BotCtx& c, const std::string& why)
{
    if (!src_.zone) return;
    giveUp_ = why + ": another source";
    if (smart) penalty_[key_of(src_.kind, src_.zone)] = c.now + kPenaltyMs * kFtMs;
    else tried_.insert(key_of(src_.kind, src_.zone));
    src_ = HuntSource{};
    planCand_ = -1;
    lostSince_ = 0;
    nav_.reset();
}

std::string ItemHuntAction::why_none(const WorldModel& w) const
{
    const GameData* gd = w.data();
    if (!gd) return "no game data";
    const ItemSources& is = gd->item_sources();
    const int myLv = std::max(1, w.self.level);
    const RouteOptions ro = Navigator::base_options(w);
    const DrillInfo* drill = best_drill(w);
    std::string out;
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].want - have(w, i) <= 0) continue;
        const uint32_t item = list[i].item;
        std::string why;
        int n = 0;
        for (const ItemMobSource& m : is.mobs(item)) {
            if (botu::shadow_monster(uint16_t(m.mob))) continue;
            const MobInfo* mi = gd->mob(m.mob);
            for (const MobSpawn& sp : gd->spawns_of(m.mob)) {
                std::string r;
                if (mi && mi->level - myLv > maxOver) r = fmt("Lv %d, %d over yours (the limit: %d; hunted anyway when nothing weaker drops it)",
                                                              mi->level, mi->level - myLv, maxOver);
                if (w.self.zone > 0 && !gd->atlas().zone_reachable(w.self.zone, sp.zone, ro))
                    r += (r.empty() ? "" : ", ") + std::string("no way there now");
                if (r.empty()) r = "usable, but left out for now (no gain there lately / a pinned source)";
                if (++n == 1) why = fmt("%s in %s: %s", gd->mob_name(uint32_t(m.mob)).c_str(), gd->map_name(sp.zone).c_str(), r.c_str());
            }
        }
        for (const ItemDigSource& d : is.digs(item)) {
            std::string r = !drill ? std::string("no drill in the bag")
                          : (gd->map_dig(d.zone) && drill->digDepth < gd->map_dig(d.zone)->minDepth) ? std::string("the drill does not reach it")
                          : std::string();
            if (w.self.zone > 0 && !gd->atlas().zone_reachable(w.self.zone, d.zone, ro)) r += (r.empty() ? "" : ", ") + std::string("no way there now");
            if (r.empty()) r = "usable, but left out for now";
            if (++n == 1) why = fmt("dug in %s: %s", gd->map_name(d.zone).c_str(), r.c_str());
        }
        if (!n) why = "no monster drops it, no map buries it";
        else if (n > 1) why += fmt(" (+%d more source%s)", n - 1, n == 2 ? "" : "s");
        out += (out.empty() ? "" : "; ") + gd->item_name(item) + ": " + why;
    }
    return out.empty() ? std::string("nothing missing") : out;
}

void ItemHuntAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "smart", smart);
    botu::kv(o, key(), "stopWhenDone", stopWhenDone);
    botu::kv(o, key(), "switchMin", switchMin);
    botu::kv(o, key(), "maxOver", maxOver);
    for (const HuntItem& h : list)
        botu::kv(o, key(), "item", fmt("%u,%d,%d,%d", h.item, h.want, h.srcKind, h.srcZone));
}

void ItemHuntAction::load(const std::string& k, const std::string& v)
{
    if (k == "smart") smart = botu::flag(v);
    else if (k == "stopWhenDone") stopWhenDone = botu::flag(v);
    else if (k == "switchMin") switchMin = std::clamp(int(botu::num(v)), 0, 600);
    else if (k == "maxOver") maxOver = std::clamp(int(botu::num(v)), 0, 300);
    else if (k == "item") {
        HuntItem h;
        long a = 0, b = 1, c = -1, d = 0;
        std::istringstream in(v);
        char sep = 0;
        in >> a >> sep >> b >> sep >> c >> sep >> d;
        if (a <= 0) return;
        h.item = uint32_t(a);
        h.want = std::max(1, int(b));
        h.srcKind = c == 0 || c == 1 ? int(c) : -1;
        h.srcZone = int(d);
        for (const HuntItem& x : list) if (x.item == h.item) return;
        list.push_back(h);
    }
}

} // namespace nj
