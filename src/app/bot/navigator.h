#pragma once
// The bot's legs: getting somewhere. Inside a map it walks the grid path (atlas.h Atlas::path) as legs
// of C2S 0x18, each a straight open line; to another map it follows the atlas route hop by hop — a
// portal (walk near it, then C2S 0x1D4 into its rectangle) or an NPC teleport (walk to the NPC, C2S
// 0x6F, then 0x7F Teleport_Start [slot]) — and plans again at every map change. A hop that does not
// take us (no map change in time, the NPC refuses) is left out of the routes for a while.
#include "app/atlas.h"
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nj {

struct BotCtx;
class WorldModel;
enum class Step;

class Navigator {
public:
    struct Goal {
        int  zone = -1;
        bool point = false;             // a point of that map (else: anywhere in it)
        int  x = 0, y = 0;
        int  within = 48;               // close enough to the point
        std::string what;               // for the status
    };

    void go(const Goal& g);
    void stop();
    bool active() const { return goal_.zone >= 0; }
    const Goal& goal() const { return goal_; }
    // Act = a request in c.out; Wait = on the way (or retrying); Pass = there.
    Step step(BotCtx& c);
    // Something else acted (a fight, a potion): the walk is planned again from where we are.
    void interrupted();
    void reset() { stop(); banned_.clear(); hopFails_.clear(); banTimes_.clear(); holdUntil_ = 0; adOffUntil_ = 0; }

    const Route& route() const { return route_; }
    const std::vector<Route>& others() const { return others_; }   // the other ways out, quickest first
    const std::vector<std::pair<int, int>>& legs() const { return legs_; }
    size_t leg() const { return legI_; }
    std::string status;
    bool        failed = false;         // no route / no path for a while: `status` says why
    // The last plan found no way between the maps (not a walk inside one): waiting does not bring one (the level, the
    // galder, an item would) — the caller gives up at once.
    bool no_route() const { return failed && noWay_ && !route_.ok && routeZone_ >= 0 && goal_.zone >= 0 && routeZone_ != goal_.zone; }
    // The route options for the world now with the default teleport settings (a check before going: QuestRunner).
    static RouteOptions base_options(const WorldModel& w);

    // settings (the Travel tab; profile town.nav*)
    bool teleports = true;              // NPC teleports allowed
    int  maxGelder = 3000;              // the most one teleport may cost
    bool portAd = true;                 // a Portable Port AD of the bag cuts the way

    RouteOptions options(const BotCtx& c) const;   // the route's options now (level, galder, speed, bans)

    // Only the walk inside this map (no route): the grid path to within `within` of (x, y), leg by leg.
    // Act / Wait (on the way, or no path yet: `status` says why) / Pass = there.
    Step walk_to(BotCtx& c, int x, int y, int within, const std::string& what)
    {
        WalkTarget t;
        t.x = x; t.y = y; t.within = within;
        return walk(c, t, what);
    }

private:
    // A walk inside the map: to within `within` of (x, y), or into a portal's rectangle (`portal`), whose
    // last leg is then the C2S 0x1D4.
    struct WalkTarget {
        int  x = 0, y = 0, within = 48;
        bool portal = false;
        AtlasPortal rect;
    };
    Step walk(BotCtx& c, const WalkTarget& t, const std::string& what);
    Step portal_hop(BotCtx& c, const RouteHop& h);
    Step teleport_hop(BotCtx& c, const RouteHop& h);
    Step item_hop(BotCtx& c, const RouteHop& h);
    void plan(BotCtx& c);
    // A hop that did not take us. `moved` false = the client never got there (its drawn avatar far from the hop): the
    // way is not to blame and is tried again. Else the 2nd failure within 5 min leaves it out 2 min; 3 left out within
    // 2 min = the moves are failing, not the ways: every one back in, 15 s of waiting, no detours.
    void ban(BotCtx& c, const RouteHop& h, const std::string& why, bool moved = true);
    // the hook draws our avatar farther than `within` from (x, y) (it never walked there); false = not known / near
    bool client_away(const BotCtx& c, int x, int y, int within) const;

    enum HopPhase { HopWalk, HopEnter, HopTalk, HopScript };
    Goal     goal_;
    Route    route_;
    std::vector<Route> others_;
    int      routeZone_ = -1;           // the map the route was planned in
    bool     replan_ = true;
    bool     noWay_ = false;            // the last plan: no route even with the failed hops back in
    uint64_t retryAt_ = 0;              // no route: plan again then
    HopPhase hop_ = HopWalk;
    uint64_t hopAt_ = 0, hopDeadline_ = 0;
    uint16_t hopNpc_ = 0;
    int      talkRefused_ = 0;          // S2C 113 answers to this hop's talk (walk closer)
    uint64_t npcMissingSince_ = 0;
    std::map<uint64_t, uint64_t> banned_;   // Atlas::hop_key -> until (FILETIME)
    std::map<uint64_t, std::pair<int, uint64_t>> hopFails_;   // hop key -> failures, the last one's time
    std::vector<uint64_t> banTimes_;        // when the recent bans came (the cascade breaker)
    uint64_t holdUntil_ = 0;                // the moves keep failing: no hop before this
    uint64_t detourNoteAt_ = 0;
    uint64_t adOffUntil_ = 0;               // a Portable Port AD use did not port us: none before this
    // the walk inside the map
    std::vector<std::pair<int, int>> legs_;
    size_t   legI_ = 0;
    bool     legSent_ = false;
    bool     legReplanned_ = false;     // this leg's line was not open from where we stood: planned again once
    uint64_t legEnd_ = 0;
    int      walkX_ = -1, walkY_ = -1, walkWithin_ = 0, walkZone_ = -1;
    bool     walkPortal_ = false;
    int      walkFails_ = 0;
    uint64_t walkRetryAt_ = 0;
};

} // namespace nj
