// Roam: reached only when nothing above has anything to do (no monster to take, nothing to loot).
//   Stay     - walk back to the start point when farther than 150 units (or half the attack radius).
//   Around   - sweep the areas within `areaRadius` of the start point.
//   WholeMap - sweep the whole map.
// The sweep (with the map's .md3 layer 1): the map in areas of 256 x 256 units, each with its open centre. An
// area is SEEN when its centre comes within the Attack's radius of us (a monster there would be taken). The
// sweep goes in ROUNDS: every area once a round, then the next round. The next goal is drawn among the near
// areas not seen this round (draw(): by the walk on the grid, a goal against the round's sense round the
// middle counting longer), and each round turns the other way round the map — clockwise, then
// counter-clockwise —, so the bot does not walk the same route over and over; once seen on the way, the next
// goal at once. It walks the grid path (Navigator legs, each an open line, no pause between them); an area it
// cannot get to is left out for 10 minutes.
// The server shows every monster of the map (ProjectPAULA monster_ai.cpp: no SectorWidth in the Paula
// GameServer.ini, a map is one sector), so a monster the Attack wants but out of its radius is walked to
// first — the bot does not search for what it already knows.
// Monster quest mode (in its hunt map, asked even when the Roam is off): the Attack takes the quest's monster
// anywhere on the map; with none to take, the bot PATROLS the monster's spawn spots. Its regeneration points
// (regen.cpp: a Mon_ row's Region R > 0 -> the type-6 points of id R, 0 -> the type-3 points; a monster
// comes at a random one of them) are grouped (group()): the point that sees the most others within 400 units
// is a spot and takes them, then the same among the rest. The spots are visited in rounds as the sweep's
// areas are, with a short pause at each (`spotPause`); a spot passed on the way counts as visited.
// Without the map file it walks random legs (no wall check) and says so.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <climits>
#include <cmath>

namespace nj {
namespace {

constexpr uint64_t kSlackMs = 2000;             // a random leg is over at its expected arrival + this
constexpr uint64_t kRetryMs = 2000;             // no open leg found: try again after this
constexpr double kPi = 3.14159265358979323846;
constexpr int      kArea = 256;                 // an area's side (units)
constexpr uint64_t kBadMs = 600000;             // no way to an area / a spot: left out this long
constexpr uint64_t kSkipMs = 60000;             // a monster walked to that the Attack still does not take
constexpr int      kSpotWithin = 64;            // at a patrol spot (passed on the way: visited)
constexpr int      kSpotRadius[] = { 400, 280, 200 };  // a spot's points: within this (smaller when < 3 spots)
constexpr size_t   kMaxSpots = 12;
constexpr double   kMovedOff = 256.0;           // moved this far between two roam steps (a fight): draw again
// draw(): a goal against the round's sense counts up to this much longer; how wide the next goal is drawn
// (sweep / patrol). Tried with the selftests: map 2's sweep (roam) — with no draw the same-sense rounds share
// ~55 % of their steps, these keep round 1 as quick as the old nearest-first (~46 s) at ~20 % shared; the
// 12 spots of a quest map (quest) — spread 1 shares ~50 %, 2 ~30 % for ~10 % more walk a round (the patrol
// runs only with no monster to take: the change matters more than the walk).
constexpr double   kSenseBias = 0.25;
constexpr double   kSweepSpread = 0.25;
constexpr double   kPatrolSpread = 2.0;

const MapGrid* zone_map(const BotCtx& c) { return c.w.data() ? c.w.data()->map(c.w.self.zone) : nullptr; }

// what counts as seen: the Attack's radius (what it would take around us)
double cover_radius(const BotCtx& c) { return std::clamp(double(c.bot.attack.radius), 160.0, 800.0); }

const char* sense_text(int sense) { return sense > 0 ? "clockwise" : "counter-clockwise"; }

// The next round turns the other way (the first one: either way).
void next_round(RoamAction::Round& r, uint64_t now, std::mt19937& rng)
{
    r.sense = r.n == 0 ? (std::uniform_int_distribution<int>(0, 1)(rng) ? 1 : -1) : -r.sense;
    ++r.n;
    r.at = now;
}

struct Cand { int i, x, y; double walk; };

// The next goal of a round: its score is the walk there, up to kSenseBias more for a goal against the round's
// sense round the middle (mx, my); drawn among the goals within (1 + spread / 2) x the best score + 128 x spread, the
// nearer the likelier (weight e^-(score - best) / (spread x max(96, 0.3 x best))): near goals take turns, far
// ones do not come first. The sweep's spread is small (its areas are many and close: a wide draw leaves holes
// to walk back to), the patrol's wide (a few spots far apart).
int draw(std::mt19937& rng, const std::vector<Cand>& cands, int px, int py, int mx, int my, int sense, double spread)
{
    if (cands.empty()) return -1;
    const double rx = px - mx, ry = py - my, rl = std::hypot(rx, ry);
    std::vector<double> score(cands.size());
    double best = 1e300;
    for (size_t k = 0; k < cands.size(); ++k) {
        const Cand& q = cands[k];
        double s = q.walk;
        const double sx = q.x - px, sy = q.y - py, sl = std::hypot(sx, sy);
        if (rl >= 128.0 && sl >= 1.0) {                     // (at the middle itself every way is the same)
            const double turn = (rx * sy - ry * sx) / (rl * sl);    // > 0: clockwise on the radar (y down)
            if (turn * sense < 0) s *= 1.0 - kSenseBias * turn * sense;
        }
        score[k] = s;
        best = std::min(best, s);
    }
    const double temp = spread * std::max(96.0, 0.3 * best), cap = (1.0 + spread / 2) * best + 128.0 * spread;
    std::vector<double> wt(cands.size(), 0.0);
    double sum = 0;
    for (size_t k = 0; k < cands.size(); ++k)
        if (score[k] <= cap) sum += wt[k] = std::exp(-(score[k] - best) / temp);
    double r = std::uniform_real_distribution<double>(0.0, sum)(rng);
    for (size_t k = 0; k < cands.size(); ++k) {
        if (wt[k] <= 0) continue;
        r -= wt[k];
        if (r <= 0) return cands[k].i;
    }
    for (size_t k = 0; k < cands.size(); ++k) if (score[k] == best) return cands[k].i;     // (rounding)
    return cands.front().i;
}

// The spawn spots: the point that sees the most others (within `radius`, an open straight line) is a spot and
// takes them, then the same among the points left, until the best takes fewer than 2 % of them (at least 1)
// or there are kMaxSpots. A spot stands at that point.
std::vector<RoamAction::Spot> group(const std::vector<std::pair<int, int>>& pts, const MapGrid* g, int radius)
{
    const size_t n = pts.size();
    std::vector<std::vector<int>> sees(n);
    for (size_t i = 0; i < n; ++i) {
        sees[i].push_back(int(i));
        for (size_t j = i + 1; j < n; ++j) {
            if (botu::dist(pts[i].first, pts[i].second, pts[j].first, pts[j].second) > radius) continue;
            if (g && !g->line_clear(pts[i].first, pts[i].second, pts[j].first, pts[j].second)) continue;
            sees[i].push_back(int(j));
            sees[j].push_back(int(i));
        }
    }
    std::vector<char> taken(n, 0);
    const int least = std::max(1, int(n) / 50);
    std::vector<RoamAction::Spot> out;
    while (out.size() < kMaxSpots) {
        int best = -1, bestN = 0;
        double bestSum = 0;
        for (size_t i = 0; i < n; ++i) {
            int k = 0;
            double sum = 0;
            for (int j : sees[i])
                if (!taken[size_t(j)]) {
                    ++k;
                    sum += botu::dist(pts[i].first, pts[i].second, pts[size_t(j)].first, pts[size_t(j)].second);
                }
            if (k > bestN || (k == bestN && k > 0 && sum < bestSum)) { best = int(i); bestN = k; bestSum = sum; }
        }
        if (best < 0 || bestN < least) break;
        for (int j : sees[size_t(best)]) taken[size_t(j)] = 1;
        RoamAction::Spot s;
        s.x = pts[size_t(best)].first;
        s.y = pts[size_t(best)].second;
        s.points = bestN;
        out.push_back(s);
    }
    return out;
}

} // namespace

void RoamAction::reset()
{
    leg_ = false;
    nextAt_ = 0;
    stuckSeen_ = 0;
    nav_.reset();
    hasLast_ = false;
    sweepZone_ = -1;
    areaX_.clear();
    areaY_.clear();
    seenAt_.clear();
    badUntil_.clear();
    area_ = -1;
    sweepRound = Round{};
    spotZone_ = -1;
    spotMobs_.clear();
    spots.clear();
    spotSeen_.clear();
    spotBad_.clear();
    spot_ = -1;
    pauseUntil_ = 0;
    patrolRound = Round{};
    spotPoints = 0;
    hasGoal = false;
    sweepAreas = sweepDone = 0;
    skip_.clear();
}

// The map's areas: the open cell of each nearest its middle (inside the map's bounds).
void RoamAction::build(const BotCtx& c, const MapGrid& g)
{
    if (sweepZone_ == c.w.self.zone && !areaX_.empty()) return;
    sweepZone_ = c.w.self.zone;
    const int per = kArea / 16;
    const int aw = (g.w + per - 1) / per, ah = (g.h + per - 1) / per;
    const size_t n = size_t(aw) * size_t(ah);
    areaX_.assign(n, -1);
    areaY_.assign(n, -1);
    seenAt_.assign(n, 0);
    badUntil_.assign(n, 0);
    area_ = -1;
    sweepRound = Round{};
    const bool bounds = g.bx2 > g.bx1 && g.by2 > g.by1;
    for (int ay = 0; ay < ah; ++ay)
        for (int ax = 0; ax < aw; ++ax) {
            const int mx = ax * per + per / 2, my = ay * per + per / 2;
            int best = INT_MAX;
            for (int cy = ay * per; cy < std::min(g.h, (ay + 1) * per); ++cy)
                for (int cx = ax * per; cx < std::min(g.w, (ax + 1) * per); ++cx) {
                    if (!g.open_cell(cx, cy)) continue;
                    const int x = cx * 16 + 8, y = cy * 16 + 8;
                    if (bounds && (x < g.bx1 || x > g.bx2 || y < g.by1 || y > g.by2)) continue;
                    const int d = (cx - mx) * (cx - mx) + (cy - my) * (cy - my);
                    if (d >= best) continue;
                    best = d;
                    areaX_[size_t(ay) * size_t(aw) + size_t(ax)] = x;
                    areaY_[size_t(ay) * size_t(aw) + size_t(ax)] = y;
                }
        }
    double sx = 0, sy = 0;
    int k = 0;
    for (size_t i = 0; i < n; ++i)
        if (areaX_[i] >= 0) { sx += areaX_[i]; sy += areaY_[i]; ++k; }
    midX_ = k ? int(sx / k) : g.w * 8;
    midY_ = k ? int(sy / k) : g.h * 8;
}

// The spots of the target monsters in this map, their regeneration points together (none: no regeneration data ->
// the target mode sweeps).
void RoamAction::build_spots(BotCtx& c, const std::vector<uint16_t>& mobs)
{
    spotZone_ = c.w.self.zone;
    spotMobs_ = mobs;
    spots.clear();
    spot_ = -1;
    pauseUntil_ = 0;
    patrolRound = Round{};
    spotPoints = 0;
    const GameData* gd = c.w.data();
    const MapGrid* g = zone_map(c);
    std::vector<std::pair<int, int>> pts;
    if (gd)
        for (uint16_t mob : mobs)
            for (const MobSpawn& sp : gd->spawns_of(mob))
                if (sp.zone == c.w.self.zone) {
                    for (const auto& p : gd->atlas().regen_points(sp.zone, sp.regions))
                        if (std::find(pts.begin(), pts.end(), p) == pts.end()) pts.push_back(p);
                    break;
                }
    std::vector<std::pair<int, int>> ok;                    // within reach, on an open cell (a point at a wall's foot: the nearest)
    for (const auto& p : pts) {
        if (c.bot.reach.walk(c, p.first, p.second) < 0) continue;
        int x = p.first, y = p.second;
        if (g && g->blocked(x, y) && !g->nearest_open(x, y, 2, x, y)) continue;
        ok.push_back({ x, y });
    }
    spotPoints = int(ok.size());
    for (int radius : kSpotRadius) {
        std::vector<Spot> got = group(ok, g, radius);
        if (got.size() > spots.size()) spots = std::move(got);
        if (spots.size() >= 3) break;
    }
    spotSeen_.assign(spots.size(), 0);
    spotBad_.assign(spots.size(), 0);
    double sx = 0, sy = 0;
    for (const Spot& s : spots) { sx += s.x; sy += s.y; }
    spotMidX_ = spots.empty() ? 0 : int(sx / double(spots.size()));
    spotMidY_ = spots.empty() ? 0 : int(sy / double(spots.size()));
}

void RoamAction::observe(BotCtx& c)
{
    // the client could not walk a leg (StuckGuard, stuck.cpp): that way is left out
    const StuckGuard::Event& se = c.bot.guard.last();
    if (se.at && se.at != stuckSeen_) {
        stuckSeen_ = se.at;
        if (leg_) { leg_ = false; heading_ += kPi; nextAt_ = 0; }
        if (area_ >= 0 && size_t(area_) < badUntil_.size()) badUntil_[size_t(area_)] = c.now + kBadMs * kFtMs;
        area_ = -1;
        if (spot_ >= 0 && size_t(spot_) < spotBad_.size()) spotBad_[size_t(spot_)] = c.now + kBadMs * kFtMs;
        spot_ = -1;
        pauseUntil_ = 0;
        nav_.interrupted();
    }
    // the patrol's spots passed on the way (the one walked to counts after its pause)
    if (spotZone_ == c.w.self.zone)
        for (size_t i = 0; i < spots.size(); ++i) {
            if (int(i) != spot_ && botu::dist(spots[i].x, spots[i].y, c.px, c.py) <= kSpotWithin) spotSeen_[i] = c.now;
            spots[i].done = patrolRound.n > 0 && spotSeen_[i] >= patrolRound.at;
        }
    // the areas within the Attack's radius are seen now
    const MapGrid* g = zone_map(c);
    if (!g || g->w <= 0) return;
    build(c, *g);
    const double r = cover_radius(c);
    int total = 0, done = 0;
    for (size_t i = 0; i < areaX_.size(); ++i) {
        if (areaX_[i] < 0) continue;
        ++total;
        if (botu::dist(areaX_[i], areaY_[i], c.px, c.py) <= r) seenAt_[i] = c.now;
        done += sweepRound.n > 0 && seenAt_[i] >= sweepRound.at;
    }
    sweepAreas = total;
    sweepDone = done;
}

// The sweep's next goal: drawn among the areas not seen this round; none left = the next round.
int RoamAction::next_area(BotCtx& c)
{
    const double here = cover_radius(c) * 0.5;
    const bool first = sweepRound.n == 0;
    const int mx = mode == Around ? c.bot.anchorX : midX_, my = mode == Around ? c.bot.anchorY : midY_;
    for (int pass = 0; pass < 2; ++pass) {
        const uint64_t since = pass == 0 && !first ? sweepRound.at : c.now;    // (pass 1: as a new round)
        std::vector<Cand> cands;
        for (size_t i = 0; i < areaX_.size(); ++i) {
            if (areaX_[i] < 0 || c.now < badUntil_[i] || seenAt_[i] >= since) continue;
            const int x = areaX_[i], y = areaY_[i];
            if (mode == Around && botu::dist(x, y, c.bot.anchorX, c.bot.anchorY) > areaRadius) continue;
            if (botu::dist(x, y, c.px, c.py) <= here) continue;
            const double wd = c.bot.reach.walk(c, x, y);
            if (wd < 0) continue;                           // another part of the map
            cands.push_back({ int(i), x, y, wd });
        }
        if (!cands.empty()) {
            if (pass == 1 || first) next_round(sweepRound, c.now, c.bot.rng());
            return draw(c.bot.rng(), cands, c.px, c.py, mx, my, sweepRound.sense, kSweepSpread);
        }
        if (first) break;
    }
    return -1;
}

// The patrol's next spot: drawn among the spots not visited this round; none left = the next round.
int RoamAction::next_spot(BotCtx& c)
{
    const bool first = patrolRound.n == 0;
    for (int pass = 0; pass < 2; ++pass) {
        const uint64_t since = pass == 0 && !first ? patrolRound.at : c.now;
        std::vector<Cand> cands;
        for (size_t i = 0; i < spots.size(); ++i) {
            if (c.now < spotBad_[i] || spotSeen_[i] >= since) continue;
            const double wd = c.bot.reach.walk(c, spots[i].x, spots[i].y);
            if (wd < 0) continue;
            cands.push_back({ int(i), spots[i].x, spots[i].y, wd });
        }
        if (!cands.empty()) {
            if (pass == 1 || first) next_round(patrolRound, c.now, c.bot.rng());
            return draw(c.bot.rng(), cands, c.px, c.py, spotMidX_, spotMidY_, patrolRound.sense, kPatrolSpread);
        }
        if (first) break;
    }
    return -1;
}

Step RoamAction::walk(BotCtx& c, int x, int y, int within, const std::string& what)
{
    hasGoal = true;
    goalX = destX = x;
    goalY = destY = y;
    goalWhy = what;
    const Step s = nav_.walk_to(c, x, y, within, what);
    if (s != Step::Pass) status = nav_.status;
    return s;
}

Step RoamAction::sweep_step(BotCtx& c, const MapGrid& g)
{
    build(c, g);
    // a monster the Attack wants, out of its radius: to the nearest one by the walk
    if (c.bot.attack.enabled) {
        const Entity* best = nullptr;
        double bestD = 1e18;
        for (const auto& kv : c.w.entities) {
            const Entity& e = kv.second;
            if (e.kind != EK_MONSTER || e.dead || !c.bot.attack.wanted(c, e.eid)) continue;
            auto sk = skip_.find(e.eid);
            if (sk != skip_.end() && c.now < sk->second) continue;
            if (mode == Around && botu::dist(e.x, e.y, c.bot.anchorX, c.bot.anchorY) > areaRadius) continue;
            const double wd = c.bot.reach.walk(c, e.x, e.y);
            if (wd < 0 || wd >= bestD) continue;
            bestD = wd;
            best = &e;
        }
        if (best) {
            area_ = -1;
            const int within = std::max(48, c.bot.attack.radius / 2);
            const Step s = walk(c, (best->x / 32) * 32 + 16, (best->y / 32) * 32 + 16, within,
                                "toward " + c.w.entity_label(best->eid));
            if (s != Step::Pass) return s;
            skip_[best->eid] = c.now + kSkipMs * kFtMs;     // there, and the Attack still does not take it
        }
    }
    // the sweep
    if (area_ >= 0) {
        const size_t a = size_t(area_);
        if (c.now < badUntil_[a] || seenAt_[a] >= areaAt_) area_ = -1;          // seen on the way: the next one
        else if (ft_older(c.now, areaAt_, areaCapMs_)) {                           // never got there
            badUntil_[a] = c.now + kBadMs * kFtMs;
            area_ = -1;
        }
    }
    for (int round = 0; round < 2; ++round) {
        if (area_ < 0) {
            area_ = next_area(c);
            if (area_ < 0) {
                hasGoal = false;
                status = mode == Around ? "nothing to sweep around the start point" : "nothing to sweep: no area within reach";
                return Step::Wait;
            }
            areaAt_ = c.now;
            const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;
            const double wd = std::max(0.0, c.bot.reach.walk(c, areaX_[size_t(area_)], areaY_[size_t(area_)]));
            areaCapMs_ = uint64_t(wd * 2000.0 / (c.bot.run ? speed : speed / 2.0)) + 15000;
        }
        const Step s = walk(c, areaX_[size_t(area_)], areaY_[size_t(area_)], 48,
                            fmt("sweeping, round %d %s: %d of %d areas", sweepRound.n, sense_text(sweepRound.sense),
                                sweepDone, sweepAreas));
        if (s != Step::Pass) return s;
        seenAt_[size_t(area_)] = c.now;                     // there: the next one at once
        area_ = -1;
    }
    return Step::Wait;
}

// Monster quest / item hunt, in its map with none of its monsters to take: the patrol of their spawn spots.
Step RoamAction::quest_step(BotCtx& c, const MapGrid* g)
{
    if (spotZone_ != c.w.self.zone || spotMobs_ != c.targetMobs) build_spots(c, c.targetMobs);
    const GameData* gd = c.w.data();
    const uint16_t mob = c.targetMobs.empty() ? 0 : c.targetMobs.front();
    std::string mobName = gd ? gd->mob_name(mob) : fmt("monster %u", mob);
    if (c.targetMobs.size() > 1) mobName += fmt(" (+%zu)", c.targetMobs.size() - 1);
    if (spots.empty()) {                                    // no regeneration data: sweep
        if (g && g->w > 0) return sweep_step(c, *g);
        return legs_step(c);
    }
    if (spot_ >= 0 && pauseUntil_) {                        // at the spot: its pause, then the next one
        if (c.now < pauseUntil_) {
            hasGoal = false;
            status = fmt("at spawn spot %d of %zu of %s (%d points), round %d: %.0f s", spot_ + 1, spots.size(),
                         mobName.c_str(), spots[size_t(spot_)].points, patrolRound.n, double(pauseUntil_ - c.now) / 1e7);
            return Step::Wait;
        }
        spotSeen_[size_t(spot_)] = c.now;
        spot_ = -1;
        pauseUntil_ = 0;
    }
    if (spot_ >= 0) {
        const size_t s = size_t(spot_);
        if (c.now < spotBad_[s]) spot_ = -1;
        else if (ft_older(c.now, spotAt_, spotCapMs_)) {                           // never got there
            spotBad_[s] = c.now + kBadMs * kFtMs;
            spot_ = -1;
        }
    }
    if (spot_ < 0) {
        spot_ = next_spot(c);
        if (spot_ < 0) {                                    // one spot (or the others out of reach): wait at it
            int closest = -1;
            double nearD = 1e300;
            for (size_t i = 0; i < spots.size(); ++i) {
                if (c.now < spotBad_[i]) continue;
                const double wd = c.bot.reach.walk(c, spots[i].x, spots[i].y);
                if (wd >= 0 && wd < nearD) { nearD = wd; closest = int(i); }
            }
            if (closest < 0) { hasGoal = false; status = "no spawn spot of " + mobName + " within reach"; return Step::Wait; }
            const Spot& sp = spots[size_t(closest)];
            if (botu::dist(c.px, c.py, sp.x, sp.y) > kSpotWithin)
                return walk(c, sp.x, sp.y, 48, fmt("to the spawn spot of %s (%d points)", mobName.c_str(), sp.points));
            hasGoal = false;
            status = fmt("waiting at the only spawn spot of %s (%d points)", mobName.c_str(), sp.points);
            return Step::Wait;
        }
        spotAt_ = c.now;
        const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;
        const double wd = std::max(0.0, c.bot.reach.walk(c, spots[size_t(spot_)].x, spots[size_t(spot_)].y));
        spotCapMs_ = uint64_t(wd * 2000.0 / (c.bot.run ? speed : speed / 2.0)) + 15000;
    }
    const Spot& sp = spots[size_t(spot_)];
    const Step s = walk(c, sp.x, sp.y, 48, fmt("patrolling the spawns of %s, round %d %s: to spot %d of %zu", mobName.c_str(),
                                               patrolRound.n, sense_text(patrolRound.sense), spot_ + 1, spots.size()));
    if (s != Step::Pass) return s;
    const double pause = spotPause * 1000.0 * std::uniform_real_distribution<double>(0.6, 1.4)(c.bot.rng());
    hasGoal = false;
    if (pause < 100.0) {                                    // no pause: the next one on the next step
        spotSeen_[size_t(spot_)] = c.now;
        spot_ = -1;
        status = "patrolling the spawns of " + mobName;
        return Step::Wait;
    }
    pauseUntil_ = c.now + uint64_t(pause) * kFtMs;
    status = fmt("at spawn spot %d of %zu of %s (%d points), round %d", spot_ + 1, spots.size(), mobName.c_str(), sp.points,
                 patrolRound.n);
    return Step::Wait;
}

bool RoamAction::pick(BotCtx& c, int& x, int& y)
{
    const MapGrid* map = zone_map(c);
    std::mt19937& rng = c.bot.rng();
    std::uniform_real_distribution<double> turn(-kPi / 2, kPi / 2), any(0.0, 2 * kPi);
    for (int pass = 0; pass < 2; ++pass) {
        const int lo = pass == 0 ? legMin : std::max(48, legMin / 3);
        const int hi = pass == 0 ? std::max(legMin + 1, legMax) : std::max(lo + 1, legMin);
        std::uniform_int_distribution<int> len(lo, hi);
        for (int i = 0; i < 80; ++i) {
            const double a = i < 40 ? heading_ + turn(rng) : any(rng);
            const int d = len(rng);
            const int tx = c.px + int(std::cos(a) * d), ty = c.py + int(std::sin(a) * d);
            if (tx < 0 || ty < 0 || tx > 0xFFFF || ty > 0xFFFF) continue;
            if (mode == Around && botu::dist(tx, ty, c.bot.anchorX, c.bot.anchorY) > areaRadius) continue;
            if (map) {
                if (map->bx2 > map->bx1 && (tx < map->bx1 || tx > map->bx2 || ty < map->by1 || ty > map->by2)) continue;
                if (map->blocked(tx, ty) || !map->line_clear(c.px, c.py, tx, ty)) continue;
            }
            heading_ = a;
            x = tx;
            y = ty;
            return true;
        }
    }
    return false;
}

// No map file: random legs that keep roughly the same heading (no wall check).
Step RoamAction::legs_step(BotCtx& c)
{
    if (leg_) {
        const bool there = botu::dist(c.px, c.py, destX, destY) < 12.0;
        if (!there && c.now < legEnd_) { status = fmt("roaming to (%d,%d)", destX, destY); return Step::Wait; }
        leg_ = false;
        std::uniform_int_distribution<int> pause(300, 1200);
        nextAt_ = c.now + uint64_t(pause(c.bot.rng())) * kFtMs;
    }
    if (c.now < nextAt_) { status = "looking around"; return Step::Wait; }
    int x = 0, y = 0;
    if (!pick(c, x, y)) {
        status = "no open way to walk";
        nextAt_ = c.now + kRetryMs * kFtMs;
        return Step::Wait;
    }
    if (c.sitting()) return c.act(act::sit(0));
    if (!zone_map(c)) c.bot.note(c.now, "no map file for this zone: roaming blind");
    leg_ = true;
    destX = x;
    destY = y;
    const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;
    const double d = botu::dist(c.px, c.py, destX, destY);
    legEnd_ = c.now + (uint64_t(d * 1000.0 / (c.bot.run ? speed : speed / 2.0)) + kSlackMs) * kFtMs;
    return c.act(act::move(c.w, destX, destY, c.bot.run, c.now));
}

Step RoamAction::step(BotCtx& c)
{
    if (c.travelling) { leg_ = false; hasGoal = false; hasLast_ = false; status = "on a trip"; return Step::Pass; }
    if (c.drillMode) { leg_ = false; hasGoal = false; hasLast_ = false; status = "drill mode: the drill walks"; return Step::Pass; }
    if (c.targetMode && !c.targetHere) {
        leg_ = false; hasGoal = false; hasLast_ = false;
        status = "monster quest: the quest walks";
        return Step::Pass;
    }
    // a fight / a pick-up since our last step took us elsewhere: the next goal is drawn from here (a spot we
    // were pausing at counts as visited)
    if (hasLast_ && botu::dist(lastX_, lastY_, c.px, c.py) > kMovedOff) {
        area_ = -1;
        if (spot_ >= 0 && pauseUntil_ && size_t(spot_) < spotSeen_.size()) spotSeen_[size_t(spot_)] = c.now;
        spot_ = -1;
        pauseUntil_ = 0;
    }
    lastX_ = c.px;
    lastY_ = c.py;
    hasLast_ = true;
    const MapGrid* g = zone_map(c);
    if (c.targetMode) return quest_step(c, g);
    if (mode == Stay) {
        const int within = std::max(150, c.bot.attack.radius / 2);
        if (botu::dist(c.px, c.py, c.bot.anchorX, c.bot.anchorY) <= within) {
            hasGoal = false;
            status = "at the start point";
            return Step::Pass;
        }
        if (!g || g->w <= 0) {                              // no map file: one straight leg
            if (leg_ && c.now < legEnd_) { status = "walking back"; return Step::Wait; }
            if (c.sitting()) return c.act(act::sit(0));
            leg_ = true;
            destX = c.bot.anchorX;
            destY = c.bot.anchorY;
            const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;
            const double d = botu::dist(c.px, c.py, destX, destY);
            legEnd_ = c.now + (uint64_t(d * 1000.0 / (c.bot.run ? speed : speed / 2.0)) + kSlackMs) * kFtMs;
            return c.act(act::move(c.w, destX, destY, c.bot.run, c.now));
        }
        return walk(c, c.bot.anchorX, c.bot.anchorY, within - 20, "walking back to the start point");
    }
    if (!g || g->w <= 0) return legs_step(c);
    return sweep_step(c, *g);
}

void RoamAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "mode", mode);
    botu::kv(o, key(), "areaRadius", areaRadius);
    botu::kv(o, key(), "legMin", legMin);
    botu::kv(o, key(), "legMax", legMax);
    botu::kv(o, key(), "spotPause", spotPause);
}

void RoamAction::load(const std::string& k, const std::string& v)
{
    if (k == "mode") mode = std::clamp(int(botu::num(v)), 0, 2);
    else if (k == "areaRadius") areaRadius = int(botu::num(v));
    else if (k == "legMin") legMin = int(botu::num(v));
    else if (k == "legMax") legMax = int(botu::num(v));
    else if (k == "spotPause") spotPause = std::clamp(int(botu::num(v)), 0, 60);
}

} // namespace nj
