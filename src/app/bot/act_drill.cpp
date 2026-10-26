// Drill: dig the map's buried items. The rules are ProjectPAULA drill.cpp's (the retail GameServer's) and the
// KR client's gauge (Driller sub_562A90, the avatar's check sub_6B3110, its sender c2s_49):
//   - C2S 0x2C at a point: in our cell the dig starts at once; in another cell the server walks us there in a
//     straight line (it only checks that the end cell is not a wall) and the dig starts on arrival;
//   - S2C 371 then tells the cell's map depth and THE DEPTH OF ITS ITEM (0 = none), whatever the sense roll
//     (rand() % 100 < DA + 10): the roll only decides what the client shows (a sensed empty cell makes it cancel
//     by itself) and gives the probe's EXP for a sensed empty cell other than the last one dug;
//   - C2S 0x31 [cnt][ms][depth][crc] reports the dig: the item's depth = the find (it pops up, ours for 30 s, TM
//     EXP + the dig's EXP); shallower = "LowDrillGrade" (+ the dig's EXP); no item = "Nothing" (+ the dig's EXP).
//     The drill loses cnt life (twice on a ground of another nature than its GroundNature). A find reported
//     sooner than 0.7 x the gauge's shortest time, a bad CRC or a depth past DigDepth / the map's depth cuts the
//     session (actions.cpp drill_end refuses those);
//   - C2S 0x30 cancels: cnt life (0 when the gauge never started);
//   - a monster's swing (hit or miss) ends the dig (reason 10) unless we have PSAbility 6 (Invincible Drill,
//     3001); a skill, a sit / stand or an attack request ends it too.
// So the bot plays the gauge as a player who never lets it overheat (one start = cnt 1) and:
//   - digs a cell whose item its drill reaches and reports the item's depth once the gauge would have got there
//     (600 * (200 - BarEffArea) / BarSpeed ms, then 120 ms per ScoopMass) plus a little;
//   - cancels at once (cnt 0: no life) a cell with nothing it can get, unless `forExp` digs it to the bottom for
//     the dig's EXP (one life);
//   - works the map BLOCK BY BLOCK, as players do: a block is the detector's square (skill.cpp detect_buried:
//     (2 * Radius) / 16 cells, starting at an odd cell), else `lineCells` wide; its cells go row by row in a snake
//     from the corner nearest to us (the drill's ground first when preferred). A detector is cast ONCE, when a block
//     starts and its square still has a quarter to learn: Basic Detection from the block's centre (standing there
//     its square is the block), the Item Detector at the centre from within its Range. Then only its points (and
//     what it could not see) are dug. Cells known empty (probed, dug, scanned, another player's dig) are never
//     tried again;
//   - a block where `failLimit` probes in a row find nothing (4 / the chance a cell holds an item this drill gets)
//     is left for 10 min, and so is the area: the next block is the nearest one at least a block away from it.
//     Around the start point, when nothing is left there, the start point moves on to the nearest block to dig;
//   - never asks the same spot again and again: a cell whose dig does not start (S2C 0x2E — no drill in slot 7, a
//     condition such as Berserker, a wall where the server looks — no answer, a walk that never ends there, the client
//     stuck on the way) is left 60 s, 10 min the second time; a block with 4 such cells is left (another area); 3
//     refusals in a minute pause the drill 30 s;
//   - picks up its finds; swaps a worn-out drill (or one too short for this map's items) for the best of the bag;
//   - in the Crazy Drill Mode (S2C 0x32 [11]) a dig is instant (no 371, one life each, 200 ms apart): the nearest
//     known item cells first, any depth — the drill must reach the map's depth, else every dig is "LowDrillGrade".
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>

namespace nj {
namespace {

constexpr uint64_t kAskMs = 3000;           // a 0x2C with no 0x2D / 0x2E: swallowed by a state -> clear it
constexpr uint64_t kWalkSlackMs = 3000;     // the walk to the cell: its time + this
constexpr uint64_t kSensedMs = 1500;        // a sensed empty cell: the client's own cancel comes within this
constexpr uint64_t kPickMs = 4000;
constexpr uint64_t kSkipMs = 60000;
constexpr uint64_t kMissMs = 600000;        // a cell asked twice without its dig starting: left this long
constexpr int      kMaxTries = 2;           // asks of one cell without a 371
constexpr int      kBlockMisses = 4;        // cells of a block that went that way: the block is left (another area)
constexpr uint64_t kClaimMs = 120000;       // another player's dig: their cell is theirs this long
constexpr uint64_t kOwnMs = 28000;          // a find is ours 30 s (drop.cpp): after that it is Loot's business
constexpr uint64_t kFightCutMs = 1500;      // attacked mid-dig with fightBack: drop the dig unless this near done
// After a dig's end (S2C 0x32) the client stays in its drill state 5 for 700 ms (the avatar's case 5 tick, +26644 = 255)
// and only then leaves it; a new S2C 0x2D before that changes nothing (sub_49EC70 ignores the same state), so its gauge
// would not restart and the 700 ms timer would close it in the middle of the next dig. The next dig waits that out.
constexpr uint64_t kSettleMs = 800;
constexpr uint64_t kCdmSettleMs = 250;      // the Crazy Drill Mode: its 200 ms between digs (the client's view matters less)
constexpr uint64_t kPoorMs = 600000;        // a block that kept failing is left this long
constexpr uint64_t kRescanMs = 300000;      // a block scanned this recently is not scanned again
constexpr uint64_t kScanWaitMs = 3000;      // a block's detector still cooling: wait this long, then work without it
constexpr uint64_t kSearchWaitMs = 17000;   // ... but up to this more when the last block was dry (the Item Detector: 15 s)
constexpr int      kDirectMax = 400;        // a dig request walks at most this far (else 0x18 legs first)
constexpr int      kKnownBonus = 160;       // a known item is worth this much more walking (units)
constexpr int      kNaturePenalty = 64;     // another ground than the drill's (twice the life)
constexpr size_t   kMaxNodes = 60000;       // cells the BFS looks at

int centre(int cell) { return cell * 16 + 8; }
int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// skill.cpp detect_buried's square: (x - radius) put back to 16 mod 32, (2 * radius) / 16 cells each way.
void detector_square(int x, int y, int radius, int& sx, int& sy, int& steps)
{
    int16_t ax = int16_t(x - radius), ay = int16_t(y - radius);
    ax = int16_t(ax - int16_t(ax % 32) + 16);
    ay = int16_t(ay - int16_t(ay % 32) + 16);
    sx = ax / 16;
    sy = ay / 16;
    steps = int16_t(2 * radius / 16);
}

} // namespace

bool DrillAction::detects(uint16_t skill) const
{
    return std::find(detectors.begin(), detectors.end(), skill) != detectors.end();
}

void DrillAction::set_detect(uint16_t skill, bool on)
{
    auto it = std::find(detectors.begin(), detectors.end(), skill);
    if (on && it == detectors.end()) detectors.push_back(skill);
    else if (!on && it != detectors.end()) detectors.erase(it);
}

int DrillAction::detector_reach(const WorldModel& w, uint16_t skill)
{
    const GameData* gd = w.data();
    const SkillInfo* si = gd ? gd->skill(skill) : nullptr;
    const int lv = w.skill_level(skill);
    const SkillLevelInfo* li = gd ? gd->skill_level(skill, lv ? lv : 1) : nullptr;
    if (!si || !li) return 0;
    if (si->esAction == 1170) return int(li->depth);                     // Basic Detection: its row's Depth
    if (si->esAction == 1147)                                            // Item Detector: DA * DepthRatio / 10
        return li->applyRatio > 0.9f ? 300 : int(double(w.stat_base(ST_DA)) * double(li->depthRatio) * 0.1);
    return 0;
}

void DrillAction::reset()
{
    planDepth_ = -1;
    planAt_ = planFor_ = sentAt_ = 0;
    resultAt_ = 0;
    silent_ = 0;
    pauseUntil_ = 0;
    skip_.clear();
    tries_.clear();
    askedCell_ = 0;
    refusedSeen_ = infoSeen_ = 0;
    refusals_.clear();
    blockMisses_ = 0;
    walking_ = false;
    walkUntil_ = 0;
    stuckSeen_ = 0;
    equipUid_ = equipAt_ = 0;
    pickOid_ = 0;
    pickAt_ = 0;
    pickBan_.clear();
    badDrill_.clear();
    hasTarget = false;
    plan.clear();
    probes = 0;
    runFinds = 0;
    lastFindAt = 0;
    hasBlock = false;
    blockZone = -1;
    blockLeft = blockFails = failLimit = blockProbes = blockFinds = 0;
    blockScan = 0;
    scanWanted_ = false;
    poor_.clear();
    scanned_.clear();
    hasPoor_ = false;
}

bool DrillAction::digging(const WorldModel& w, uint64_t now) const
{
    return w.drill_open(now);
}

bool DrillAction::in_block(int cx, int cy) const
{
    return hasBlock && cx >= blockCellX && cy >= blockCellY && cx < blockCellX + blockSide && cy < blockCellY + blockSide;
}

void DrillAction::observe(BotCtx& c)
{
    const WorldModel& w = c.w;
    // the client could not walk to the cell (StuckGuard, stuck.cpp): that cell is left alone for a while
    const StuckGuard::Event& se = c.bot.guard.last();
    if (se.at && se.at != stuckSeen_) {
        stuckSeen_ = se.at;
        if (hasTarget && (walking_ || pickOid_))
            missed_cell(c, WorldModel::cell_key(targetX / 16, targetY / 16), "the client could not walk there (a wall?)");
        walking_ = false;
    }
    // a dig started (S2C 371): its cell is fine
    if (w.drill.infoAt && w.drill.infoAt != infoSeen_) {
        infoSeen_ = w.drill.infoAt;
        tries_.erase(WorldModel::cell_key(w.drill.x / 16, w.drill.y / 16));
        tries_.erase(askedCell_);
    }
    // a refusal (S2C 0x2E: no drill in slot 7, a condition such as Berserker, or the server sees a wall at the point)
    if (!refusedSeen_) refusedSeen_ = std::max<uint64_t>(1, w.drillRefusedAt);
    if (w.drillRefusedAt > refusedSeen_) {
        refusedSeen_ = w.drillRefusedAt;
        refusals_.push_back(c.now);
        refusals_.erase(std::remove_if(refusals_.begin(), refusals_.end(), [&](uint64_t t) { return ft_older(c.now, t, 60000); }),
                        refusals_.end());
        if (askedCell_) missed_cell(c, askedCell_, "the server refused the dig (S2C 0x2E)");
        if (refusals_.size() >= 3) {
            pauseUntil_ = c.now + 30000 * kFtMs;
            refusals_.clear();
            c.bot.note(c.now, "3 digs refused in a minute (no drill in slot 7? a condition such as Berserker?): the drill pauses 30 s");
        }
    }
    if (!resultAt_) resultAt_ = std::max<uint64_t>(1, w.drillLast.at);     // (an older result is not ours)
    if (w.drillLast.at <= resultAt_) return;
    resultAt_ = w.drillLast.at;
    const DrillResult& r = w.drillLast;
    const GameData* gd = w.data();
    planDepth_ = -1;
    silent_ = 0;
    const bool find = r.reason == 3 || r.reason == 8 || r.reason == 19;
    switch (r.reason) {
    case 3: case 8: case 19:
        lastFindAt = c.now;
        ++runFinds;
        c.bot.note(c.now, fmt("found %s%s", gd ? gd->item_name(r.item).c_str() : fmt("item %u", r.item).c_str(),
                              r.itemDepth ? fmt(" (%d deep)", r.itemDepth).c_str() : ""));
        break;
    case 4:
        if (w.cdm_on(c.now)) c.bot.note(c.now, "Crazy Drill: LowDrillGrade (the drill does not reach the map's depth)");
        break;
    case 6:  c.bot.note(c.now, "the drill is worn out"); break;
    case 7:  c.bot.note(c.now, fmt("(%d,%d): ground that cannot be dug", r.x, r.y)); break;
    case 10: c.bot.note(c.now, "the dig was interrupted by a monster's swing"); break;
    case 15: c.bot.note(c.now, "Crazy Drill Mode: not on this map"); break;
    case 1:
        if (!r.info)                            // it ended before the dig began: we never got to that cell
            missed_cell(c, WorldModel::cell_key(r.x / 16, r.y / 16), fmt("never got to dig at (%d,%d)", r.x, r.y));
        break;
    default: break;
    }
    // the block's tally: its own probes (a 371 came) — a find resets the failures, nothing / too deep counts one
    if (hasBlock && w.self.zone == blockZone && r.info && in_block(r.x / 16, r.y / 16)) {
        ++blockProbes;
        if (find) {
            ++blockFinds;
            blockFails = 0;
            hasPoor_ = false;
        } else if ((r.reason == 1 || r.reason == 2 || r.reason == 4) && ++blockFails >= failLimit && failLimit > 0) {
            leave_block(c, true, fmt("%d probes in a row found nothing", blockFails).c_str());
        }
    }
}

// A cell whose dig did not start: left 60 s, 10 min once that happened kMaxTries times (no more asks at the same spot);
// kBlockMisses such cells in the block leave the block (poor: another area).
void DrillAction::missed_cell(BotCtx& c, uint32_t cell, const std::string& why)
{
    const int n = ++tries_[cell];
    const bool done = n >= kMaxTries;
    skip_[cell] = c.now + (done ? kMissMs : kSkipMs) * kFtMs;
    const int cx = int(int16_t(cell >> 16)), cy = int(int16_t(cell & 0xFFFF));
    c.bot.note(c.now, fmt("%s - cell (%d,%d) left for %s", why.c_str(), cx, cy, done ? "10 min (tried twice)" : "60 s"));
    if (hasBlock && in_block(cx, cy) && ++blockMisses_ >= kBlockMisses)
        leave_block(c, true, fmt("%d cells whose dig never started", blockMisses_).c_str());
}

// ── a dig is open ───────────────────────────────────────────────────────────
void DrillAction::decide(BotCtx& c)
{
    const WorldModel& w = c.w;
    const DrillSession& s = w.drill;
    planFor_ = s.infoAt;
    planDepth_ = 0;
    const DrillInfo* d = equipped_drill_info(w);
    if (!d) { plan = "no drill row: cancelled"; return; }
    if (s.premium) { plan = "an auto-drill dig (not modelled): cancelled"; return; }
    const int need = std::min(d->digDepth, s.mapDepth);    // where the client's gauge stops without an item
    const int item = s.itemDepth;
    if (item > 0 && item <= d->digDepth) planDepth_ = item;
    else if (forExp && need > 0 && !(s.sensed && !item)) planDepth_ = need;   // (a sensed empty one: the client cancels)
    if (!planDepth_) {
        plan = item ? fmt("an item %d deep, the drill reaches %d: next cell", item, d->digDepth)
                    : std::string(s.sensed ? "nothing here (sensed): next cell" : "nothing here: next cell");
        return;
    }
    std::uniform_int_distribution<int> extra(150, 450);
    const int ms = drill_dig_ms(*d, planDepth_) + extra(c.bot.rng());
    planAt_ = s.infoAt + uint64_t(ms) * kFtMs;
    plan = planDepth_ == item ? fmt("an item %d deep: digging (%.1f s)", item, ms / 1000.0)
                              : fmt("%s: digging %d deep for the EXP (%.1f s)", item ? "out of reach" : "nothing here",
                                    planDepth_, ms / 1000.0);
    c.bot.note(c.now, plan);
}

Step DrillAction::dig_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const DrillSession& s = w.drill;
    walking_ = false;
    switch (s.phase) {
    case DrillSession::Asked:
        status = "asking to dig";
        if (ft_older(c.now, s.askedAt, kAskMs) && ft_older(c.now, sentAt_, kAskMs)) {
            // swallowed (a state the server is in) or stuck: a cancel clears the server's dig state
            if (++silent_ >= 3) {
                pauseUntil_ = c.now + 5000 * kFtMs;
                status = "the dig requests get no answer: pausing 5 s";
                c.bot.note(c.now, status);
                silent_ = 0;
            }
            if (askedCell_) missed_cell(c, askedCell_, "no answer to the dig request");
            sentAt_ = c.now;
            return c.act(act::drill_cancel(w, 0, c.now));
        }
        return Step::Wait;
    case DrillSession::Started: {
        status = w.cdm_on(c.now) ? std::string("Crazy Drill: digging") : fmt("walking to (%d,%d) to dig", s.x, s.y);
        const int speed = std::max(40, w.self.speed > 0 ? w.self.speed : 160);
        const uint64_t walkMs = uint64_t(kDirectMax) * 1000 / uint64_t(speed) + kWalkSlackMs;
        if (ft_older(c.now, s.startedAt, walkMs) && ft_older(c.now, sentAt_, kAskMs)) {
            missed_cell(c, WorldModel::cell_key(s.x / 16, s.y / 16), fmt("no dig at (%d,%d) after the walk: cancelled", s.x, s.y));
            sentAt_ = c.now;
            return c.act(act::drill_cancel(w, 0, c.now));
        }
        return Step::Wait;
    }
    case DrillSession::Info: {
        if (planFor_ != s.infoAt) decide(c);
        if (s.endOp) { status = "waiting for the result"; return Step::Wait; }
        if (planDepth_ <= 0) {
            status = plan;
            if (s.sensed && !s.itemDepth && !ft_older(c.now, s.infoAt, kSensedMs)) {
                status = "nothing here (sensed): the client cancels it";
                return Step::Wait;
            }
            sentAt_ = c.now;
            return c.act(act::drill_cancel(w, 0, c.now));
        }
        // a hit with PSAbility 6 keeps the dig going: with fightBack, drop it unless it is nearly done
        if (c.attacked && fightBack && planAt_ > c.now + kFightCutMs * kFtMs) {
            c.bot.note(c.now, "attacked while digging: the dig is dropped to fight back");
            sentAt_ = c.now;
            return c.act(act::drill_cancel(w, 1, c.now));
        }
        if (c.now < planAt_) {
            status = fmt("%s - %.1f s left", plan.c_str(), double(planAt_ - c.now) / 1e7);
            return Step::Wait;
        }
        const Action a = act::drill_end(w, 1, planDepth_, c.now);
        sentAt_ = c.now;
        if (!a.ok()) {
            c.bot.note(c.now, "the dig report was refused (" + a.error + "): cancelled");
            return c.act(act::drill_cancel(w, 1, c.now));
        }
        return c.act(a);
    }
    default:
        status = "waiting for the result";
        return Step::Wait;
    }
}

// ── between digs ────────────────────────────────────────────────────────────
Step DrillAction::pickup_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    if (pickOid_) {
        if (!w.groundItems.count(pickOid_)) {
            pickOid_ = 0;                       // picked up (0x22) or gone
        } else if (!ft_older(c.now, pickAt_, kPickMs)) {
            status = "picking up the find";
            return Step::Wait;
        } else {
            pickBan_[pickOid_] = c.now + kSkipMs * kFtMs;
            c.bot.note(c.now, "a find stayed on the ground (the bag full or too heavy?)");
            pickOid_ = 0;
        }
    }
    uint16_t best = 0;
    double bestD = 1e18;
    for (const auto& kv : w.groundItems) {
        const GroundItem& g = kv.second;
        if (!g.fromDrop || g.dropper != w.self.eid || g.owner != w.self.eid) continue;
        if (ft_older(c.now, g.seenAt, kOwnMs)) continue;
        const bool wanted = c.wanted && std::find(c.wanted->begin(), c.wanted->end(), g.itemId) != c.wanted->end();
        if (!wanted && !c.bot.loot.equip_allowed(w.data(), g.itemId)) continue;     // (the Loot tab's gear grades)
        auto b = pickBan_.find(kv.first);
        if (b != pickBan_.end() && c.now < b->second) continue;
        const double d = botu::dist(g.x, g.y, c.px, c.py);
        if (d < 320 && d < bestD) { bestD = d; best = kv.first; }
    }
    if (!best) return Step::Pass;
    if (c.sitting()) return c.act(act::sit(0));
    pickOid_ = best;
    pickAt_ = c.now;
    return c.act(act::pick_up(w, best, c.bot.run));
}

// The share (%) of this map's diggable cells whose ground is `nature`.
int DrillAction::nature_share(const BotCtx& c, const MapGrid& g, int nature)
{
    if (shareZone_ != c.w.self.zone) {
        shareZone_ = c.w.self.zone;
        std::fill(std::begin(shareCount_), std::end(shareCount_), 0);
        shareTotal_ = 0;
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                if (!g.diggable(x, y)) continue;
                ++shareTotal_;
                ++shareCount_[g.nature_at(x, y) & 7];
            }
    }
    return shareTotal_ ? shareCount_[nature & 7] * 100 / shareTotal_ : 0;
}

Step DrillAction::swap_step(BotCtx& c, const MapDigInfo& md)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    if (equipUid_) {
        if (w.self.equip[7] == equipUid_) {
            equipUid_ = 0;                      // S2C 0x52: on
        } else if (!ft_older(c.now, equipAt_, 2500)) {
            status = "equipping a drill";
            return Step::Wait;
        } else {
            badDrill_.insert(equipUid_);
            c.bot.note(c.now, "the drill would not go on (S2C 0x53?): left out");
            equipUid_ = 0;
        }
    }
    const InvItem* di = equipped_drill(w);
    const DrillInfo* d = equipped_drill_info(w);
    const bool worn = !di || !d || di->count == 0;
    const bool useless = d && d->digDepth < md.minDepth && !forExp;
    if (!worn && !useless) return Step::Pass;
    if (!worn && w.cdm_on(c.now)) return Step::Pass;      // another drill would end the Crazy Drill Mode
    const MapGrid* g = gd->map(w.self.zone);
    const InvItem* best = nullptr;
    double bestScore = -1e18;
    for (const InvItem& it : w.inventory) {
        if (it.uid == w.self.equip[7] || it.count == 0 || badDrill_.count(it.uid)) continue;
        const ItemInfo* ii = gd->item(it.itemId);
        const DrillInfo* x = gd->drill(it.itemId);
        if (!ii || ii->type != 6 || !x) continue;
        if (ii->minLevel > w.self.level && w.self.level > 0) continue;
        if (x->digDepth < md.minDepth && !forExp) continue;          // as useless here
        // reaching every item of the map first, then the share of the map on its ground, then its Grade (EXP)
        double score = x->digDepth >= md.maxDepth ? 1e6 : x->digDepth * 1000.0;
        score += (x->groundNature == 0 ? 100 : g ? nature_share(c, *g, x->groundNature) : 0) * 100.0;   // 0 = any ground
        score += x->grade;
        if (score > bestScore) { bestScore = score; best = &it; }
    }
    if (!best) return Step::Pass;
    if (c.sitting()) return c.act(act::sit(0));
    equipUid_ = best->uid;
    equipAt_ = c.now;
    c.bot.note(c.now, fmt("%s: putting on %s (life %u)", worn ? "no drill with life" : "the drill does not reach this map's items",
                          gd->item_name(best->itemId).c_str(), best->count));
    return c.act(act::equip(w, best->uid, 7));
}

// Open cells of a detector's square at (x, y) we would learn something about (none known, or only "nothing
// down to a shallower depth than `need`"), inside our area. `total` = the open cells of the square in the area.
int DrillAction::unknown_cells(const BotCtx& c, const MapGrid& g, int x, int y, int radius, int need, int* total) const
{
    int sx, sy, steps;
    detector_square(x, y, radius, sx, sy, steps);
    int n = 0, t = 0;
    for (int i = 0; i < steps; ++i)
        for (int j = 0; j < steps; ++j) {
            const int cx = sx + j, cy = sy + i;
            if (!(g.raw(cx, cy) == 0 && g.nature_at(cx, cy) != 6)) continue;      // the scan skips these
            if (area == AroundStart && botu::dist(centre(cx), centre(cy), c.bot.anchorX, c.bot.anchorY) > radius_area()) continue;
            ++t;
            const BuriedCell* b = c.w.buried_at(cx, cy);
            if (!b || (b->state == BuriedCell::Unknown && b->noneTo < need)) ++n;
        }
    if (total) *total = t;
    return n;
}

// ── the cells ───────────────────────────────────────────────────────────────
// The BFS from our cell over the open cells (8-way, a diagonal only past two open sides, as the path finder
// walks): steps_ / from_ for each cell reached, order_ = the order it reached them. `nearArea` keeps it within the
// start point's area (+ 800 units) when we work around it.
void DrillAction::bfs(const BotCtx& c, const MapGrid& g, bool nearArea)
{
    const size_t cells = size_t(g.w) * size_t(g.h);
    if (seen_.size() != cells) { seen_.assign(cells, 0); from_.assign(cells, -1); steps_.assign(cells, 0); stamp_ = 0; }
    if (++stamp_ == 0) { std::fill(seen_.begin(), seen_.end(), 0); stamp_ = 1; }
    order_.clear();
    bfsStart_ = -1;
    int scx = c.px / 16, scy = c.py / 16;
    if (!g.open_cell(scx, scy)) {
        int ox = 0, oy = 0;
        if (!g.nearest_open(c.px, c.py, 4, ox, oy)) return;
        scx = ox / 16;
        scy = oy / 16;
    }
    const int start = scy * g.w + scx;
    bfsStart_ = start;
    seen_[size_t(start)] = stamp_;
    from_[size_t(start)] = -1;
    steps_[size_t(start)] = 0;
    order_.push_back(start);
    const bool limit = nearArea && area == AroundStart;
    const int areaR = radius_area() + 800;
    for (size_t head = 0; head < order_.size() && order_.size() < kMaxNodes; ++head) {
        const int i = order_[head];
        const int cx = i % g.w, cy = i / g.w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dy) continue;
                const int nx = cx + dx, ny = cy + dy;
                if (!g.open_cell(nx, ny)) continue;
                if (dx && dy && (!g.open_cell(cx + dx, cy) || !g.open_cell(cx, cy + dy))) continue;
                const int j = ny * g.w + nx;
                if (seen_[size_t(j)] == stamp_) continue;
                if (limit && botu::dist(centre(nx), centre(ny), c.bot.anchorX, c.bot.anchorY) > areaR) continue;
                seen_[size_t(j)] = stamp_;
                from_[size_t(j)] = i;
                steps_[size_t(j)] = uint16_t(std::min<int>(steps_[size_t(i)] + 1, 0xFFFF));
                order_.push_back(j);
            }
    }
}

// A cell worth a dig: diggable, in the area (unless `ignoreArea`), not left alone, the drill's ground when only that
// is dug, and nothing known against it: unknown (or a scan saw nothing only shallower than `need`), a known item the
// drill reaches (any in the Crazy Drill Mode), a detected one, another player's dig long ago.
bool DrillAction::valid_cell(const BotCtx& c, const DrillInfo& d, const MapGrid& g, int need, int cx, int cy, bool cdm,
                             bool ignoreArea, bool* known) const
{
    if (known) *known = false;
    if (!g.diggable(cx, cy)) return false;
    if (!ignoreArea && area == AroundStart && botu::dist(centre(cx), centre(cy), c.bot.anchorX, c.bot.anchorY) > radius_area())
        return false;
    auto sk = skip_.find(WorldModel::cell_key(cx, cy));
    if (sk != skip_.end() && c.now < sk->second) return false;
    if (ground == OnlyGround && d.groundNature && g.nature_at(cx, cy) != d.groundNature) return false;
    const BuriedCell* b = c.w.buried_at(cx, cy);
    if (!b) return true;
    switch (b->state) {
    case BuriedCell::Undiggable:
    case BuriedCell::Empty:    return false;
    case BuriedCell::Claimed:  return ft_older(c.now, b->at, kClaimMs);
    case BuriedCell::Item:
        if (!cdm && b->depth > d.digDepth) return false;
        if (known) *known = true;
        return true;
    case BuriedCell::Detected:
        if (known) *known = true;
        return true;
    default:
        return cdm || b->noneTo < need;
    }
}

// The block's next cell: its cells row by row in a snake from the corner it started at — the drill's ground first
// when that is preferred — the first one still to dig that the BFS reached. `left` = how many are still to dig.
int DrillAction::next_in_block(const BotCtx& c, const DrillInfo& d, const MapGrid& g, int need, int* left) const
{
    int first = -1, n = 0;
    const int S = blockSide;
    const bool twoPasses = ground == PreferGround && d.groundNature;
    for (int pass = 0; pass < (twoPasses ? 2 : 1); ++pass)
        for (int r = 0; r < S; ++r) {
            const int cy = (corner_ & 2) ? blockCellY + S - 1 - r : blockCellY + r;
            const bool rightward = ((corner_ & 1) == 0) == (r % 2 == 0);
            for (int k = 0; k < S; ++k) {
                const int cx = rightward ? blockCellX + k : blockCellX + S - 1 - k;
                if (!g.inside(cx, cy)) continue;
                if (twoPasses && (g.nature_at(cx, cy) != d.groundNature) != (pass == 1)) continue;
                if (!valid_cell(c, d, g, need, cx, cy, false, false, nullptr)) continue;
                const int i = cy * g.w + cx;
                if (seen_[size_t(i)] != stamp_) continue;           // not reachable from here
                ++n;
                if (first < 0) first = i;
            }
        }
    if (left) *left = n;
    return first;
}

// The nearest cell to dig by walking (the BFS order): a known item worth kKnownBonus units, another ground
// kNaturePenalty more; none in a block left for failing — nor, `awayFromPoor`, a block next to the last one (a new area).
int DrillAction::nearest_cell(const BotCtx& c, const DrillInfo& d, const MapGrid& g, int need, bool ignoreArea,
                              bool awayFromPoor) const
{
    const bool cdm = c.w.cdm_on(c.now);
    int best = -1, bestCost = INT_MAX;
    for (int i : order_) {
        const int walk = int(steps_[size_t(i)]) * 16;
        if (best >= 0 && walk - kKnownBonus > bestCost) break;        // nothing farther can win
        const int cx = i % g.w, cy = i / g.w;
        bool known = false;
        if (!valid_cell(c, d, g, need, cx, cy, cdm, ignoreArea, &known)) continue;
        if (blockStep_ > 0 && !cdm) {
            const int bx = 1 + floordiv(cx - 1, blockStep_) * blockStep_, by = 1 + floordiv(cy - 1, blockStep_) * blockStep_;
            auto p = poor_.find(block_key(c.w.self.zone, bx, by));
            if (p != poor_.end() && c.now < p->second) continue;
            if (awayFromPoor && hasPoor_ && poorZone_ == c.w.self.zone &&
                std::max(std::abs(bx - poorX_), std::abs(by - poorY_)) <= blockStep_)
                continue;
        }
        const bool other = d.groundNature && g.nature_at(cx, cy) != d.groundNature;
        const int cost = walk - (known ? kKnownBonus : 0) + (other && ground == PreferGround ? kNaturePenalty : 0);
        if (cost < bestCost) { bestCost = cost; best = i; }
    }
    return best;
}

// ── the blocks ──────────────────────────────────────────────────────────────
// The detector a block is scanned with: Basic Detection (around us, cheap) when it shows this map's items, else the
// Item Detector (at a point); its square ((2 * Radius) / 16 cells) is the block. 0 = none: blocks of `lineCells`.
uint16_t DrillAction::block_detector(const BotCtx& c, const MapDigInfo& md, int* side) const
{
    const GameData* gd = c.w.data();
    uint16_t best = 0;
    int bestSide = 0, bestRank = 9;
    for (uint16_t sk : detectors) {
        const SkillInfo* si = gd ? gd->skill(sk) : nullptr;
        const int lv = c.w.skill_level(sk);
        const SkillLevelInfo* li = si && lv ? gd->skill_level(sk, lv) : nullptr;
        if (!li || li->radius <= 0.0f || (si->esAction != 1170 && si->esAction != 1147)) continue;
        if (!usable_skill(c.w, SkillEntry{ sk, uint8_t(lv) })) continue;
        if (detector_reach(c.w, sk) < md.minDepth) continue;           // it shows none of this map's items
        const int rank = si->esAction == 1170 ? 0 : 1;
        if (rank < bestRank) {
            bestRank = rank;
            best = sk;
            bestSide = std::max(3, int(int16_t(2 * int(li->radius) / 16)));
        }
    }
    if (side) *side = best ? bestSide : std::clamp(lineCells, 3, 31);
    return best;
}

// A new block: the one holding `cell` (the block grid starts at the odd cells, as the scan's square does). Its
// snake starts at the corner nearest to us; its fail limit comes from the chance that a cell holds an item this
// drill gets; its scan is due when its detector shows this map's items and a quarter of it is still to learn.
void DrillAction::start_block(BotCtx& c, const DrillInfo& d, const MapDigInfo& md, const MapGrid& g, int cell, int need)
{
    const int cx = cell % g.w, cy = cell / g.w;
    hasBlock = true;
    blockZone = c.w.self.zone;
    blockCellX = 1 + floordiv(cx - 1, blockStep_) * blockStep_;
    blockCellY = 1 + floordiv(cy - 1, blockStep_) * blockStep_;
    blockFails = blockProbes = blockFinds = 0;
    blockMisses_ = 0;
    int bestD = INT_MAX;
    for (int k = 0; k < 4; ++k) {
        const int qx = (k & 1) ? blockCellX + blockSide - 1 : blockCellX;
        const int qy = (k & 2) ? blockCellY + blockSide - 1 : blockCellY;
        const int i = qy * g.w + qx;
        const int dd = g.inside(qx, qy) && seen_[size_t(i)] == stamp_ ? int(steps_[size_t(i)]) * 16
                                                                      : 1000 + int(botu::dist(centre(qx), centre(qy), c.px, c.py));
        if (dd < bestD) { bestD = dd; corner_ = k; }
    }
    // 4 / q probes in a row with nothing: q = ItemPercentage x the share of the depths (10 x [Min/10, Max/10]) it reaches
    const int k0 = md.minDepth / 10, k1 = md.maxDepth / 10;
    int n = 0, ok = 0;
    for (int k = k0; k <= k1; ++k, ++n) ok += 10 * k <= d.digDepth;
    const double q = n ? double(md.itemPct) / 100.0 * ok / n : 0.0;
    failLimit = q > 0.0 ? std::clamp(int(std::ceil(4.0 / q)), 6, 30) : 30;
    // its scan
    blockScan = 0;
    scanWanted_ = false;
    int side = 0;
    const uint16_t det = block_detector(c, md, &side);
    std::string how = "in lines";
    if (det) {
        auto s = scanned_.find(block_key(blockZone, blockCellX, blockCellY));
        const bool recent = s != scanned_.end() && !ft_older(c.now, s->second, kRescanMs);
        const SkillLevelInfo* li = c.w.data()->skill_level(det, c.w.skill_level(det));
        int open = 0;
        const int u = li ? unknown_cells(c, g, 16 * blockCellX + 8 * blockSide, 16 * blockCellY + 8 * blockSide, int(li->radius),
                                         std::min(detector_reach(c.w, det), need), &open)
                         : 0;
        if (!recent && u >= std::max(4, open / 4)) {
            scanWanted_ = true;
            blockScan = det;
            scanWait_ = c.now + kScanWaitMs * kFtMs;
            scanWalks_ = 0;
            how = "a scan with " + c.w.data()->skill_name(det) + " first";
        }
    }
    static const char* const kCorner[] = { "top left", "top right", "bottom left", "bottom right" };
    c.bot.note(c.now, fmt("block at (%d,%d), %d x %d cells: %s, from its %s corner", 16 * blockCellX, 16 * blockCellY, blockSide,
                          blockSide, how.c_str(), kCorner[corner_ & 3]));
}

void DrillAction::leave_block(BotCtx& c, bool poor, const char* why)
{
    if (!hasBlock) return;
    // a block left without a find is a dry area: the next one keeps away from it (and a failing one is left alone)
    if (poor) poor_[block_key(blockZone, blockCellX, blockCellY)] = c.now + kPoorMs * kFtMs;
    if (poor || blockFinds == 0) {
        hasPoor_ = true;
        poorX_ = blockCellX;
        poorY_ = blockCellY;
        poorZone_ = blockZone;
    }
    c.bot.note(c.now, fmt("block at (%d,%d) %s: %d find%s in %d probe%s%s", 16 * blockCellX, 16 * blockCellY, why, blockFinds,
                          blockFinds == 1 ? "" : "s", blockProbes, blockProbes == 1 ? "" : "s",
                          poor ? " - left for 10 min, another area next" : blockFinds == 0 ? " - another area next" : ""));
    hasBlock = false;
    scanWanted_ = false;
    blockLeft = 0;
}

// The block's scan, once, when it starts: Basic Detection from the block's centre (standing within half a cell of it
// its square is the block), the Item Detector at the centre from within its Range. A detector still cooling is
// waited for kScanWaitMs; past that the block is worked in lines without it.
Step DrillAction::scan_step(BotCtx& c, const MapGrid& g)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const uint16_t sk = blockScan;
    const SkillInfo* si = gd->skill(sk);
    const int lv = w.skill_level(sk);
    const SkillLevelInfo* li = si && lv ? gd->skill_level(sk, lv) : nullptr;
    if (!li) { scanWanted_ = false; return Step::Pass; }
    const std::string name = gd->skill_name(sk);
    // in position first (the cool time runs meanwhile): Basic Detection on the centre, the Item Detector within reach
    const int mx = 16 * blockCellX + 8 * blockSide, my = 16 * blockCellY + 8 * blockSide;   // the block's centre
    SkillTarget t;
    if (si->esAction == 1170) {
        int sx = mx, sy = my;
        if (g.blocked(sx, sy) && !g.nearest_open(mx, my, 2, sx, sy)) { scanWanted_ = false; return Step::Pass; }
        if (std::abs(c.px - sx) > 8 || std::abs(c.py - sy) > 8) {
            if (walking_ && c.now < walkUntil_) { status = "walking to the block's centre to scan it"; return Step::Wait; }
            if (++scanWalks_ > 6) { scanWanted_ = false; c.bot.note(c.now, "the block's centre is out of reach: no scan"); return Step::Pass; }
            scanWait_ = std::max(scanWait_, c.now + kScanWaitMs * kFtMs);     // (the walk does not use up the wait)
            return walk_cell(c, g, (sy / 16) * g.w + sx / 16, sx, sy, "to the block's centre to scan it");
        }
        t.mode = SkillTarget::Self;
    } else {
        const int range = li->range > 0 ? li->range : 200;
        if (botu::dist(c.px, c.py, mx, my) > range * 0.9) {
            if (walking_ && c.now < walkUntil_) { status = "walking within reach of the block's centre"; return Step::Wait; }
            if (++scanWalks_ > 6) { scanWanted_ = false; c.bot.note(c.now, "the block's centre is out of reach: no scan"); return Step::Pass; }
            scanWait_ = std::max(scanWait_, c.now + kScanWaitMs * kFtMs);
            int cx = mx / 16, cy = my / 16;
            if (!g.open_cell(cx, cy)) {
                int ox = 0, oy = 0;
                if (!g.nearest_open(mx, my, 4, ox, oy)) { scanWanted_ = false; return Step::Pass; }
                cx = ox / 16;
                cy = oy / 16;
            }
            return walk_cell(c, g, cy * g.w + cx, centre(cx), centre(cy), "within reach of the block's centre");
        }
        t.mode = SkillTarget::Point;
        t.x = mx;
        t.y = my;
    }
    // then its cool time: waited a few seconds — up to kSearchWaitMs when looking for a new area after a dry block
    std::string why;
    if (!skill_ready(c, sk, &why)) {
        const uint64_t limit = scanWait_ + (hasPoor_ ? kSearchWaitMs : 0) * kFtMs;
        if (c.now < limit) {
            status = fmt("%s for the block: %s", name.c_str(), why.c_str());
            return Step::Wait;
        }
        scanWanted_ = false;
        c.bot.note(c.now, fmt("%s not ready (%s): the block in lines", name.c_str(), why.c_str()));
        return Step::Pass;
    }
    if (c.sitting()) return c.act(act::sit(0));
    scanWanted_ = false;
    scanned_[block_key(blockZone, blockCellX, blockCellY)] = c.now;
    status = fmt("%s over the block", name.c_str());
    return c.cast(act::use_skill(w, sk, t, c.now), sk);
}

// Walk toward (x, y) in the cell `cell`: straight there when the line is open, else along the BFS path to its farthest
// point in a straight open line (0x18 legs; the server trusts the client's path).
Step DrillAction::walk_cell(BotCtx& c, const MapGrid& g, int cell, int x, int y, const char* what)
{
    int wx = -1, wy = -1;
    if (botu::dist(c.px, c.py, x, y) <= 600 && g.line_clear(c.px, c.py, x, y)) {
        wx = x;
        wy = y;
    } else if (cell >= 0 && size_t(cell) < seen_.size() && seen_[size_t(cell)] == stamp_) {
        std::vector<int> path;
        for (int i = cell; i >= 0 && path.size() < 4096; i = from_[size_t(i)]) path.push_back(i);
        std::reverse(path.begin(), path.end());
        for (size_t k = path.size(); k-- > 1;) {
            const int px = centre(path[k] % g.w), py = centre(path[k] / g.w);
            if (botu::dist(c.px, c.py, px, py) <= 600 && g.line_clear(c.px, c.py, px, py)) { wx = px; wy = py; break; }
        }
        if (wx < 0 && path.size() > 1) { wx = centre(path[1] % g.w); wy = centre(path[1] / g.w); }
    }
    if (wx < 0) {
        status = "no way there";
        if (cell >= 0) skip_[WorldModel::cell_key(cell % g.w, cell / g.w)] = c.now + kSkipMs * kFtMs;
        return Step::Wait;
    }
    if (c.sitting()) return c.act(act::sit(0));
    const int speed = std::max(40, c.w.self.speed > 0 ? c.w.self.speed : 160);
    const double secs = botu::dist(c.px, c.py, wx, wy) / (c.bot.run ? speed : speed / 2.0);
    walking_ = true;
    walkUntil_ = c.now + uint64_t(secs * 1000.0 + 600.0) * kFtMs;
    status = fmt("walking %s", what);
    return c.act(act::move(c.w, wx, wy, c.bot.run, c.now));
}

// Dig `cell`: our own cell at once, one within kDirectMax in a straight open line by asking it (the server walks us
// there), a farther one by walking first.
Step DrillAction::go_cell(BotCtx& c, const MapGrid& g, int cell, bool known)
{
    const int tcx = cell % g.w, tcy = cell / g.w;
    const int tx = centre(tcx), ty = centre(tcy);
    hasTarget = true;
    targetX = tx;
    targetY = ty;
    const bool here = cell == bfsStart_;
    if (here || (botu::dist(c.px, c.py, tx, ty) <= kDirectMax && g.line_clear(c.px, c.py, tx, ty))) {
        if (c.sitting()) return c.act(act::sit(0));
        const uint32_t key = here ? WorldModel::cell_key(c.px / 16, c.py / 16) : WorldModel::cell_key(tcx, tcy);
        auto t = tries_.find(key);
        if (t != tries_.end() && t->second >= kMaxTries) {      // (asked twice already, never started: not a third time)
            skip_[key] = c.now + kMissMs * kFtMs;
            skip_[WorldModel::cell_key(tcx, tcy)] = c.now + kMissMs * kFtMs;
            status = "that cell's dig never starts: another one";
            return Step::Wait;
        }
        askedCell_ = key;
        ++probes;
        sentAt_ = c.now;
        status = fmt("%s (%d,%d)", known ? "digging the item at" : "probing", tx, ty);
        // our own cell: the point where we stand (the server digs where IT has us when the cell is the same)
        return c.act(here ? act::drill_start(c.px, c.py, c.bot.run) : act::drill_start(tx, ty, c.bot.run));
    }
    return walk_cell(c, g, cell, tx, ty, fmt("toward (%d,%d)", tx, ty).c_str());
}

Step DrillAction::block_step(BotCtx& c, const DrillInfo& d, const MapDigInfo& md, const MapGrid& g, int need)
{
    const WorldModel& w = c.w;
    int side = 0;
    block_detector(c, md, &side);
    if (hasBlock && (blockZone != w.self.zone || side != blockSide)) {   // another map / another detector: a new grid
        hasBlock = false;
        scanWanted_ = false;
    }
    blockSide = side;
    blockStep_ = std::max(2, side % 2 ? side - 1 : side);
    bfs(c, g, true);
    if (bfsStart_ < 0) { status = "standing in a wall?"; return Step::Wait; }
    if (hasBlock) {
        if (scanWanted_) {
            const Step s = scan_step(c, g);
            if (s != Step::Pass) return s;
        }
        const int next = next_in_block(c, d, g, need, &blockLeft);
        if (next >= 0) {
            bool known = false;
            valid_cell(c, d, g, need, next % g.w, next / g.w, false, false, &known);
            return go_cell(c, g, next, known);
        }
        leave_block(c, false, "done");
    }
    // a new block: the one of the nearest cell to dig — after a dry block, not next to it (another area)
    int cell = nearest_cell(c, d, g, need, false, true);
    if (cell < 0 && hasPoor_) cell = nearest_cell(c, d, g, need, false, false);
    if (cell < 0 && area == AroundStart && moveOn) {
        bfs(c, g, false);                       // past the area: the start point moves on
        cell = nearest_cell(c, d, g, need, true, true);
        if (cell < 0 && hasPoor_) cell = nearest_cell(c, d, g, need, true, false);
        if (cell >= 0) {
            const int cx = cell % g.w, cy = cell / g.w;
            c.bot.anchorX = 16 * (1 + floordiv(cx - 1, blockStep_) * blockStep_) + 8 * blockSide;
            c.bot.anchorY = 16 * (1 + floordiv(cy - 1, blockStep_) * blockStep_) + 8 * blockSide;
            c.bot.note(c.now, fmt("nothing left around the start point: it moves on to (%d,%d)", c.bot.anchorX, c.bot.anchorY));
            bfs(c, g, true);
            cell = nearest_cell(c, d, g, need, false, false);
        }
    }
    if (cell < 0) {
        hasTarget = false;
        status = area == AroundStart ? "nothing left to dig around the start point" : "no cell left to dig within reach";
        pauseUntil_ = c.now + 5000 * kFtMs;
        return Step::Pass;
    }
    start_block(c, d, md, g, cell, need);
    if (scanWanted_) {
        const Step s = scan_step(c, g);
        if (s != Step::Pass) return s;
    }
    const int next = next_in_block(c, d, g, need, &blockLeft);
    if (next < 0) { leave_block(c, false, "done"); return Step::Wait; }
    bool known = false;
    valid_cell(c, d, g, need, next % g.w, next / g.w, false, false, &known);
    return go_cell(c, g, next, known);
}

// The Crazy Drill Mode: instant digs, so simply the nearest cell (known items first, any depth).
Step DrillAction::target_step(BotCtx& c, const DrillInfo& d, const MapGrid& g, int need)
{
    bfs(c, g, true);
    if (bfsStart_ < 0) { status = "standing in a wall?"; return Step::Wait; }
    const int cell = nearest_cell(c, d, g, need, false, false);
    if (cell < 0) {
        hasTarget = false;
        status = "Crazy Drill Mode: no cell left to dig within reach";
        pauseUntil_ = c.now + 2000 * kFtMs;
        return Step::Pass;
    }
    bool known = false;
    valid_cell(c, d, g, need, cell % g.w, cell / g.w, true, false, &known);
    return go_cell(c, g, cell, known);
}

Step DrillAction::step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    if (c.travelling) { walking_ = false; hasTarget = false; status = "on a trip"; return Step::Pass; }
    if (!gd) { status = "no game data"; return Step::Pass; }
    if (w.drill_open(c.now)) return dig_step(c);
    planDepth_ = -1;
    if (pickUp) {
        const Step s = pickup_step(c);
        if (s != Step::Pass) return s;
    }
    const MapDigInfo* md = gd->map_dig(w.self.zone);
    if (!md) { hasTarget = false; status = "nothing is buried in this map"; return Step::Pass; }
    if (swap) {
        const Step s = swap_step(c, *md);
        if (s != Step::Pass) return s;
    }
    const InvItem* di = equipped_drill(w);
    const DrillInfo* d = equipped_drill_info(w);
    if (!di) { status = "no drill in slot 7"; return Step::Pass; }
    if (!d) { status = "the drill has no Item_DrillParam row"; return Step::Pass; }
    if (di->count == 0) { status = "the drill is worn out (no other in the bag)"; return Step::Pass; }
    const MapGrid* g = gd->map(w.self.zone);
    if (!g) { status = "no map file for this zone"; return Step::Pass; }
    const bool cdm = w.cdm_on(c.now);
    if (cdm && (md->option & 512)) { status = "Crazy Drill Mode is off in this map: waiting for it to end"; return Step::Wait; }
    if (cdm && d->digDepth < md->maxDepth) {
        status = fmt("Crazy Drill Mode needs a drill reaching %d (this one: %d): waiting for it to end", md->maxDepth, d->digDepth);
        return Step::Wait;
    }
    const int need = std::min(d->digDepth, md->maxDepth);
    if (!cdm && d->digDepth < md->minDepth && !forExp) {
        status = fmt("the drill reaches %d, this map's items lie %d..%d deep", d->digDepth, md->minDepth, md->maxDepth);
        return Step::Pass;
    }
    if (c.now < pauseUntil_) return Step::Pass;
    if (walking_) {
        if (c.now < walkUntil_) return Step::Wait;
        walking_ = false;
    }
    if (w.drillLast.at && !ft_older(c.now, w.drillLast.at, cdm ? kCdmSettleMs : kSettleMs)) {
        status = "the client leaves its dig state";
        return Step::Wait;
    }
    return cdm ? target_step(c, *d, *g, need) : block_step(c, *d, *md, *g, need);
}

void DrillAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "fightBack", fightBack);
    botu::kv(o, key(), "area", area);
    botu::kv(o, key(), "radius", radius);
    botu::kv(o, key(), "moveOn", moveOn);
    botu::kv(o, key(), "lineCells", lineCells);
    botu::kv(o, key(), "ground", ground);
    botu::kv(o, key(), "forExp", forExp);
    botu::kv(o, key(), "pickUp", pickUp);
    botu::kv(o, key(), "swap", swap);
    for (uint16_t sk : detectors) botu::kv(o, key(), "detect", sk);
}

void DrillAction::load(const std::string& k, const std::string& v)
{
    if (k == "fightBack") fightBack = botu::flag(v);
    else if (k == "area") area = std::clamp(int(botu::num(v)), 0, 1);
    else if (k == "radius") radius = std::clamp(int(botu::num(v)), 32, 20000);
    else if (k == "moveOn") moveOn = botu::flag(v);
    else if (k == "lineCells") lineCells = std::clamp(int(botu::num(v)), 3, 31);
    else if (k == "ground") ground = std::clamp(int(botu::num(v)), 0, 2);
    else if (k == "forExp") forExp = botu::flag(v);
    else if (k == "pickUp") pickUp = botu::flag(v);
    else if (k == "swap") swap = botu::flag(v);
    else if (k == "detect") { const uint16_t sk = uint16_t(botu::num(v)); if (sk) set_detect(sk, true); }
}

} // namespace nj
