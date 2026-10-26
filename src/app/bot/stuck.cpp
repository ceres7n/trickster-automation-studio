// Stuck: the drawn avatar against the server's position, and the walking distances the actions choose by
// (bot_actions.h StuckGuard / Reach).
//
// The client side (Paula Trickster.bin; the S2 build has the same objects at the same offsets): every S2C 0x18
// for us goes to the avatar (sub_6BEC50 case 0x18). Where it stands at the move's start -> event 30016 MoveTo(to)
// (PathFindingMoveStrategy sub_4030E0); elsewhere -> 30017 (sub_403960): a path from the move's start, joined
// from where the avatar is. Both search with the map's PathFinder (sub_408450: the move strategy's finder, then
// SmartPathFinder — an A* over the .md3 attribute layer, 16-unit cells, 255 nodes, at most 128 points; a goal
// in a wall moves to an open cell within a few steps, sub_409950). No path = the move is dropped and the avatar
// stays where it is (sub_6B3010). ProjectPAULA moves us in straight lines whatever lies between, so a chase
// across a wall, or a long way round, leaves the server's position and the drawn one apart for good.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <cmath>

namespace nj {
namespace {

constexpr double   kDriftUnits = 48.0;          // three cells apart
constexpr uint64_t kStillMs = 900;              // the drawn avatar has not moved for this long: it stands
constexpr uint64_t kStuckMs = 1200;             // apart this long while it stands = stuck
constexpr uint64_t kDetourMs = 6000;            // apart this long while it still walks (a long way round)
constexpr uint64_t kSettleMs = 3000;            // after a map change: the avatar is being built
constexpr uint64_t kGapMs = 3000;               // between two resyncs
constexpr uint64_t kBurstMs = 60000;            // this many resyncs within this long ...
constexpr size_t   kBurstCount = 4;
constexpr uint64_t kPauseMs = 300000;           // ... = the drawn position cannot be right: no resync this long
constexpr uint64_t kFieldMs = 1000;             // the walking field is made again at most this often

} // namespace

void StuckGuard::observe(BotCtx& c)
{
    const WorldModel& w = c.w;
    const SelfState::Drawn& d = w.self.drawn;
    live = w.drawn_live(c.now) && (d.x > 0 || d.y > 0);
    if (live) {                                     // a point of this map (a read mid-change is not)
        const MapGrid* g = w.data() ? w.data()->map(w.self.zone) : nullptr;
        if (g && g->bx2 > g->bx1 && (d.x < g->bx1 - 64 || d.x > g->bx2 + 64 || d.y < g->by1 - 64 || d.y > g->by2 + 64))
            live = false;
    }
    drift = live ? botu::dist(d.x, d.y, c.px, c.py) : 0.0;
    if (!live || w.changingZone || w.self.dead || c.sitting() || !ft_older(c.now, w.arrivedAt, kSettleMs) ||
        c.now < pausedUntil) {
        driftSince_ = 0;
        due_ = false;
        return;
    }
    if (drift < kDriftUnits) { driftSince_ = 0; return; }
    if (!driftSince_) driftSince_ = c.now;
    if (due_ || !ft_older(c.now, resyncAt_, kGapMs)) return;
    const bool still = ft_older(c.now, d.movedAt, kStillMs);
    if (!ft_older(c.now, driftSince_, still ? kStuckMs : kDetourMs)) return;
    // stuck again and again: the drawn position is not to be trusted (an unknown client state) — pause
    while (!recent_.empty() && ft_older(c.now, recent_.front(), kBurstMs)) recent_.pop_front();
    if (recent_.size() >= kBurstCount) {
        pausedUntil = c.now + kPauseMs * kFtMs;
        recent_.clear();
        driftSince_ = 0;
        c.bot.note(c.now, fmt("stuck %zu times in a minute: the stuck guard pauses 5 min (the drawn position may be wrong)", kBurstCount));
        return;
    }
    recent_.push_back(c.now);
    due_ = true;
    ++count;
    last_ = Event{};
    last_.at = c.now;
    last_.zone = w.self.zone;
    last_.drawnX = d.x;
    last_.drawnY = d.y;
    last_.serverX = c.px;
    last_.serverY = c.py;
    last_.target = c.bot.attack.target;
    last_.doing = c.bot.doing;
    c.bot.note(c.now, fmt("stuck: drawn at (%d,%d), the server has us at (%d,%d), %d units off%s (%s)", d.x, d.y, c.px, c.py,
                          int(drift), still ? "" : " for 6 s", c.bot.doing.c_str()));
}

Action StuckGuard::resync(BotCtx& c)
{
    due_ = false;
    driftSince_ = 0;
    resyncAt_ = c.now;
    return act::stop_at(c.w.self.drawn.x, c.w.self.drawn.y, c.bot.run);
}

// (The server may have us inside a wall — its own straight walks go anywhere: the grid cannot judge from
// there, so both fall back to the straight line; the drawn avatar, StuckGuard, still tells.)
double Reach::walk(const BotCtx& c, int x, int y)
{
    const MapGrid* g = c.w.data() ? c.w.data()->map(c.w.self.zone) : nullptr;
    if (!g || g->w <= 0 || g->blocked(c.px, c.py)) return botu::dist(c.px, c.py, x, y);
    const int cell = (c.py / 16) * g->w + c.px / 16;
    if (zone_ != c.w.self.zone || cell != cell_ || ft_older(c.now, at_, kFieldMs) || field_.empty()) {
        Atlas::walk_field(*g, c.px, c.py, field_);
        zone_ = c.w.self.zone;
        cell_ = cell;
        at_ = c.now;
    }
    return Atlas::field_walk(*g, field_, x, y);
}

bool Reach::straight(const BotCtx& c, int x, int y)
{
    const MapGrid* g = c.w.data() ? c.w.data()->map(c.w.self.zone) : nullptr;
    if (!g || g->w <= 0 || g->blocked(c.px, c.py)) return true;
    const double d = botu::dist(c.px, c.py, x, y);
    if (d <= 24.0) return true;
    const int ex = c.px + int((x - c.px) * (d - 24.0) / d), ey = c.py + int((y - c.py) * (d - 24.0) / d);
    return g->line_clear(c.px, c.py, ex, ey);
}

} // namespace nj
