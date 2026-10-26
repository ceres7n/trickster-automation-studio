#pragma once
// The bot's actions. Each one has its settings (public, edited by the Bot tab and saved in the
// profile), its own state, and two calls per tick: observe() always, step() when it is asked.
#include "app/actions.h"
#include "app/bot/navigator.h"
#include "app/game_data.h"
#include <cstdint>
#include <deque>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace nj {

class Bot;
class GameData;
class WorldModel;
struct Entity;

constexpr uint64_t kFtMs = 10000;   // FILETIME units per millisecond
inline bool ft_older(uint64_t now, uint64_t then, uint64_t ms) { return now < then || now - then >= ms * kFtMs; }

enum class Step { Pass, Wait, Act };

struct BotCtx {
    const WorldModel& w;
    uint64_t now;
    int      px, py;                // where we are now (the move interpolated)
    bool     attacked;              // a monster swung at us in the last 4 s
    Bot&     bot;
    Action   out;
    bool     travelling = false;    // a town trip is on: fight only what attacks us, no loot / rest / roam
    bool     drillMode = false;     // the Drill action is on (or the Item Hunt digs here): no hunting, the Attack
                                    //   only answers who hits us
    // The Monster Quest / the Item Hunt (killing) drive the combat: only these species, only in their map.
    bool     targetMode = false;
    bool     targetHere = false;    //   we are in that map and there is work there
    std::vector<uint16_t> targetMobs;   //   the species to kill
    const char* targetWhat = "";    //   for the statuses ("monster quest", "item hunt")
    bool     huntDig = false;       // the Item Hunt digs, and we are in its map (the Drill is asked, drill mode)
    const std::vector<uint32_t>* wanted = nullptr;  // the Item Hunt's items: always picked up (Loot)
    bool target_mob(uint16_t species) const;

    bool sitting() const;
    Step act(const Action& a) { out = a; return Step::Act; }
    // A skill request: sent through the skill gate (one at a time, its answer awaited).
    Step cast(const Action& a, uint16_t skill);
};

// One skill request at a time, as the client does it (bit 0x20 of player+26512 blocks the next one
// until S2C 239 / 240 / 243 — kto-skill-use): after a request the bot waits for its answer, fired
// (240: the cool time starts), refused (239) or cancelled (243), or a timeout (the server's chase
// gives up after 5 s). A refused or unanswered skill rests a while before it is tried again.
class SkillGate {
public:
    void     reset() { skill_ = 0; rest_.clear(); }
    void     observe(const WorldModel& w, uint64_t now, Bot& bot);
    void     sent(const WorldModel& w, uint16_t skill, uint64_t now);
    bool     busy() const { return skill_ != 0; }
    uint16_t pending() const { return skill_; }
    bool     resting(uint16_t skill, uint64_t now) const;
    uint64_t doneAt = 0;            // when the last request was answered (or given up)
private:
    uint16_t skill_ = 0;
    uint64_t sentAt_ = 0, deadline_ = 0;
    std::map<uint16_t, uint64_t> rest_;
};

// Stuck: the client draws us (hook EV_AVATAR) away from where the server has us. ProjectPAULA walks every
// chase, pick-up walk and move in a straight line, through walls (game_session.cpp player_order: retail's
// anti-stuck warp sub_41DB40 is not ported); the client path-finds each move the server orders (A*, 255
// nodes, 128 points: PathFindingMoveStrategy / SmartPathFinder) and leaves the avatar standing when it finds
// no way. The server then fights and walks on from a place the client never reached. Drawn >= 48 units off
// while it stands still 1.2 s (or 6 s while it walks a detour) = stuck: the bot sends the client's own
// "stand here" (C2S 0x18 [drawn][drawn], its stop key), which puts the server back where we are drawn, and
// the actions leave what they were after (the target, the item, the cell, the leg) for a while.
// Without the feed (an old hook DLL / an unknown client build) it sees nothing.
class StuckGuard {
public:
    struct Event {
        uint64_t    at = 0;             // FILETIME (0 = none yet)
        int         zone = -1;
        int         drawnX = 0, drawnY = 0, serverX = 0, serverY = 0;
        uint16_t    target = 0;         // the Attack's target then
        std::string doing;              // what the bot was doing
    };
    void reset() { last_ = Event{}; due_ = false; driftSince_ = resyncAt_ = pausedUntil = 0; count = 0; live = false; drift = 0; recent_.clear(); }
    void observe(BotCtx& c);            // every tick, before the actions'
    bool due() const { return due_; }
    Action resync(BotCtx& c);           // the stand-here request (due -> false)
    const Event& last() const { return last_; }
    bool   live = false;                // the drawn avatar is being read
    double drift = 0;                   // units between where it is drawn and the server's position
    int    count = 0;                   // stuck events since the start
    uint64_t pausedUntil = 0;           // 4 resyncs within a minute: the drawn position is not trusted until then
private:
    Event    last_;
    bool     due_ = false;
    uint64_t driftSince_ = 0, resyncAt_ = 0;
    std::deque<uint64_t> recent_;       // the last resyncs (the burst check)
};

// Walking distances from where we stand over the map's grid (Atlas::walk_field), kept while we stay in the
// same cell (at most 1 s). No map file = the straight distance.
class Reach {
public:
    void   reset() { zone_ = -1; field_.clear(); }
    double walk(const BotCtx& c, int x, int y);     // units; -1 = no way there
    // The straight walk from us to (x, y) crosses no wall — the line the server's chase / pick-up walk takes.
    // The last 24 units are not looked at (a monster may stand at a wall's foot). No map = true.
    static bool straight(const BotCtx& c, int x, int y);
private:
    int      zone_ = -1, cell_ = -1;
    uint64_t at_ = 0;
    std::vector<int32_t> field_;
};

// Which ids an action takes (item ids for Loot, monster species for Attack): everything except the
// blacklist, or only the whitelist. Profile lines "<action>.listMode=0|1", ".black=<id>", ".white=<id>".
struct IdFilter {
    enum Mode { AllButBlack = 0, OnlyWhite = 1 };
    int mode = AllButBlack;
    std::set<uint32_t> black, white;
    bool allows(uint32_t id) const { return mode == OnlyWhite ? white.count(id) != 0 : black.count(id) == 0; }
    void save(std::ostream& o, const char* key) const;
    bool load(const std::string& k, const std::string& v);      // true = one of its keys
};

// Is the skill castable now from what the model knows: learned + usable, off its cool time, not
// resting after a refusal, MP / HP for its cost. false -> `why` says what is missing.
bool skill_ready(const BotCtx& c, uint16_t skill, std::string* why = nullptr);

class BotAction {
public:
    virtual ~BotAction() = default;
    virtual const char* key() const = 0;        // profile prefix
    virtual const char* title() const = 0;
    virtual void reset() {}                     // the bot (re)starts
    virtual void observe(BotCtx&) {}            // every tick, enabled or asked or not
    virtual Step step(BotCtx& c) = 0;
    virtual void preempted() {}                 // a higher action acted this tick
    virtual void save(std::ostream& o) const = 0;
    virtual void load(const std::string& k, const std::string& v) = 0;

    bool        enabled = true;
    std::string status;                         // what it is doing / waiting for
};

// ── Heal: potions (item_use.cpp: a potion needs HP / MP below the max; answers 0x24 / 0x25 / 0x5A) ──
// ── Heal: potions (ItemParam Type 10) when HP / MP drop below the %, and heal skills for HP: the learned skills
//    whose ESAction heals the caster (act_heal.cpp has the server's formulas). A class with one (the Dragon's
//    Catastrophe Healing / Heart's Grace) can leave the HP potions off and keep only the MP ones. ──
class HealAction : public BotAction {
public:
    const char* key() const override { return "heal"; }
    const char* title() const override { return "Heal"; }
    void reset() override { waiting_ = false; castAt_ = 0; }
    Step step(BotCtx& c) override;
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    int      hpPct = 50, mpPct = 30;
    uint32_t hpItem = 0, mpItem = 0;            // 0 = auto: the smallest potion that fills the gap
    bool     hpPotions = true;                  // HP potions below hpPct (off: only the heal skills heal HP)
    std::vector<uint16_t> skills;               // the heal skills cast, in this order: the first ready one
    int      skillPct = 70;                     // a heal skill at or below this HP %
    bool uses(uint16_t skill) const;
    void set_use(uint16_t skill, bool on);
    // The ESAction heals the caster: 1085 Cure (on us), 1168 Catastrophe Healing, 1323 Heart's Grace (the party
    // within Radius, us too), 1482 Chakra Balance (over time, ends at the first swing at us).
    static bool heal_skill(const GameData* gd, uint16_t skill);
    static const char* heal_kind(const GameData* gd, uint16_t skill);   // for the UI
private:
    uint64_t noHpNoteAt_ = 0, noMpNoteAt_ = 0;   // the last "no potion" line
    Step skill_step(BotCtx& c);
    bool     waiting_ = false;
    uint64_t waitUntil_ = 0;
    int64_t  waitHp_ = 0, waitMp_ = 0;
    uint64_t castAt_ = 0;                       // a heal skill went out: no other one until HP moves
    int64_t  castHp_ = 0;
};


// ── Buffs: keep the checked ones up. The effects on us are tracked from the packets (world.h Effect:
//    S2C 224 / 280 / 850, the lifetime = the ESAction row's Seconds), so a buff is cast again when its
//    effect is gone or about to end, as soon as its cool time allows. ──
class BuffAction : public BotAction {
public:
    BuffAction() { enabled = false; }
    const char* key() const override { return "buffs"; }
    const char* title() const override { return "Buffs"; }
    void reset() override { seen_.clear(); }
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    bool kept(uint16_t skill) const;
    void set_kept(uint16_t skill, bool on);

    std::vector<uint16_t> keep;                 // the skills kept up, in this order
    int beforeSec = 3;                          // cast again this long before the effect ends
    int untrackedSec = 60;                      // fired but no effect ever seen: again after its Seconds, else this
private:
    std::map<uint16_t, uint64_t> seen_;         // skill -> last time its effect was up on us
};

// ── Loot: C2S 0x21. A drop is its monster's loot owner's for 30 s and the server refuses anyone
//    else WITHOUT a reply (inventory.cpp do_pickup); a pick-up ends the running attack. So: our own
//    kills' drops between two targets (even with monsters around), free items only out of a fight. ──
class LootAction : public BotAction {
public:
    const char* key() const override { return "loot"; }
    const char* title() const override { return "Loot"; }
    void reset() override { waiting_ = false; banned_.clear(); pathOid_ = 0; nav_.reset(); stuckSeen_ = 0; }
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void preempted() override { if (pathOid_) nav_.interrupted(); }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    int  radius = 400;
    bool galder = true;                         // galder piles (ItemParam Type 30): apart from the lists
    int  heavyPct = 80;                         // the bag weighs this % of the max or more: only the quests' / hunt's items
    IdFilter items;                             // every other item, by item id
    bool othersItems = true;                    // also free items (not only our own kills' drops)
    // The common gear grades (game_data.h EquipTier: Old / Strong / Art / Ult.), heavy: left on the ground by default;
    // off, the checked grades are picked up. An item whitelisted by its id is picked up anyway; the Item Hunt's / Key
    // Quest's items too. The Drill's own finds follow it as well.
    bool skipEquip = true;
    bool lootTier[ET_Count] = { true, true, true, true };
    bool equip_allowed(const GameData* gd, uint32_t item) const;
private:
    bool lootable(const BotCtx& c, uint16_t oid, bool& own) const;
    bool     waiting_ = false;
    uint16_t waitOid_ = 0;
    uint64_t waitUntil_ = 0;
    std::map<uint16_t, uint64_t> banned_;       // oid -> left alone until
    // an item behind a wall: the grid path walked first (the server's pick-up walk is a straight line)
    Navigator nav_;
    uint16_t pathOid_ = 0;
    uint64_t pathSince_ = 0;
    uint64_t stuckSeen_ = 0;                    // the last StuckGuard event handled
};

// ── Attack: a target, then the skill rotation (the first skill of the list that is ready, so the list
//    order is the priority) and, when on, the weapon's basic attack: melee (0x1D, the server chases +
//    swings) or the gun (0x1DD, one shot per request) when a gun (SubType 54) is in the weapon slot and
//    there are bullets. A skill whose ESAIC is not Chase is only cast within its ESAII Range: the bot
//    walks there first (the server walks a Chase skill there by itself). ──
struct RotationSkill {
    uint16_t skill = 0;
    bool     on = true;
    // an area skill (its ESA row's Radius): cast only when at least this many monsters stand in its area (0 / 1 = any)
    int      minTargets = 0;
};
// An area skill's best spot now (AttackAction::area_spot): the monster at its centre (a skill aimed at one: Mana Ring),
// the point (a ground skill: Magic Storm), or us (a self-centred one); how many monsters the area takes there.
struct AreaSpot { uint16_t eid = 0; int x = 0, y = 0; int count = 0; };
class AttackAction : public BotAction {
public:
    const char* key() const override { return "attack"; }
    const char* title() const override { return "Attack"; }
    void reset() override;
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void preempted() override { if (pathing_) nav_.interrupted(); }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    bool engaged(const WorldModel& w) const;    // a live target
    // The idle watchdog (Bot::tick): the target left alone for `ms`, the walk round a wall forgotten.
    void drop_target(uint64_t now, uint64_t ms) { if (target) banned_[target] = now + ms * kFtMs; target = 0; pathing_ = false; nav_.interrupted(); }
    // A monster it would hunt (the lists / quest monster, shadows, owners, bans) — the Roam walks toward one
    bool wanted(const BotCtx& c, uint16_t eid) const { return huntable(c, eid); }

    int      radius = 600;                      // around us when roaming, else around the start point
    bool     skipOwned = true;                  // leave monsters another player fights
    IdFilter mobs;                              // which monsters to hunt, by species (mob id)
    bool     fightBack = true;                  // whoever hits us is fought, whatever the lists say
    bool     basic = true;                      // the weapon's own attack; off = skills only
    std::vector<RotationSkill> rotation;        // priority order
    bool     useGun = true;                     // shoot when a gun + bullets are there

    uint16_t target = 0;
    int      kills = 0;
    uint64_t lastKillAt = 0;
    bool     gunOff = false;                    // a shot went unanswered: no more shots this run
private:
    bool     huntable(const BotCtx& c, uint16_t eid, bool attacker = false) const;
    uint16_t choose(const BotCtx& c) const;
    Step     gun_step(BotCtx& c);
    Step     rotation_step(BotCtx& c, const Entity& t);
public:
    // The radius of a skill's area at our level (its ESA row; 0 = a single target).
    static float area_radius(const WorldModel& w, uint16_t skill);
    // Where an area skill takes the most monsters now (the huntable ones near us); count 0 = none.
    AreaSpot area_spot(const BotCtx& c, uint16_t skill) const;
private:
    Step     approach(BotCtx& c, const Entity& t, int range, const char* why);
    Step     path_step(BotCtx& c, const Entity& t);
    // a target behind a wall: the grid path walked first (the server's chase is a straight line)
    Navigator nav_;
    bool     pathing_ = false;
    uint64_t pathSince_ = 0, pathCapMs_ = 0;
    uint64_t stuckSeen_ = 0;                    // the last StuckGuard event handled
    double   bestDist_ = 1e18;                  // the nearest we got to the target (nearer = progress)
    uint64_t progressAt_ = 0;
    int64_t  targetHp_ = -1;
    uint64_t meleeAt_ = 0, approachAt_ = 0;
    uint64_t shotAt_ = 0, loadAt_ = 0, seedAskAt_ = 0;
    uint64_t seedsAtShot_ = 0;
    uint64_t shotsAtShot_ = 0;                  // w.self.lastShotAt when we asked
    bool     waitSeed_ = false;
    std::map<uint16_t, uint64_t> banned_;
};

// ── Monster Quest (act_quest.cpp; ProjectPAULA monster_quest.cpp): a repeatable quest of an NPC — take it
//    (its dialog's MonQuest_Accept), kill its grade's monster in the chosen map within the time, come back
//    (the success portal C2S 0x410, else walking), hand it in (MonQuest_Complete), take it again. On, it
//    turns the Drill off and the Attack on; the Attack then fights only in that map and only that monster
//    (and whoever hits us there, with "fight back"). ──
struct MonQuestInfo;
struct AtlasNpc;
class QuestAction : public BotAction {
public:
    QuestAction() { enabled = false; }
    const char* key() const override { return "quest"; }
    const char* title() const override { return "Monster Quest"; }
    void reset() override;
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void preempted() override { nav_.interrupted(); }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    int  questId = 0;               // monster_quest_db Id
    std::string npcKey;             // the NPC that gives it (atlas key; empty = the first that does)
    int  huntZone = 0;              // the map to hunt in (0 = where the monster is most)
    bool usePortal = true;          // come back through the success portal (only while the quest's timer runs)
    bool repeat = true;             // take it again after each hand-in
    int  maxRuns = 0;               // stop the bot after this many hand-ins (0 = no limit)

    // for the bot and the UI
    const MonQuestInfo* info(const WorldModel& w) const;
    const AtlasNpc* giver(const WorldModel& w, int* slot = nullptr) const;
    uint16_t mob(const WorldModel& w) const;            // the monster to kill now (the entry's, else our grade's)
    int      hunt_zone(const WorldModel& w) const;      // the map (0 = none known)
    bool     hunting(const WorldModel& w) const;        // in that map with the kills still to do
    int      runs = 0;                                  // hand-ins this run
    int64_t  expGained = 0, gelderGained = 0;
    uint64_t lastDoneAt = 0;
    std::string plan;                                   // what comes next (UI)
private:
    Step npc_step(BotCtx& c, bool complete);
    Navigator nav_;
    int      sub_ = 0;              // the NPC errand: 0 walk + talk, 1 the dialog, 2 the answer, 3 exit
    uint64_t subAt_ = 0;
    uint16_t npcEid_ = 0;
    bool     subComplete_ = false;  // the errand hands in (else takes)
    uint64_t portalAt_ = 0;         // the portal asked (0 = not yet for this quest)
    bool     portalFailed_ = false;
    uint64_t waitUntil_ = 0;        // after a refusal: ask again then
    int      refusals_ = 0;
    uint64_t seenDone_ = UINT64_MAX, seenTaken_ = UINT64_MAX;   // the last 219 / 210 counted (MAX = not yet)
    uint64_t lostSince_ = 0;        // the navigator has had no way since
    int      noteZone_ = 0;         // the hunt map a "not there" note was given for
};

// ── Item Hunt (act_hunt.cpp): a list of items and how many of each. The bot goes where they come from — the
//    monsters that drop them (item_sources.h: the expected count per kill, worked out from the server's own
//    rolls) or the maps where they are buried (the share of a find) — and works there until the bag holds them
//    all: Kill = the Attack only on those species in that map (the Roam patrols their spawns), Dig = the Drill
//    there. The list's items are always picked up and never sold or stored. Smart (the default): the whole list
//    is planned at once — the map (kill / dig) that does the most of what is missing, killing every species there
//    that drops a missing item (the mix that pays) —, else one by one in the list's order. A source with none of
//    its items for `switchMin` minutes gives way to the next one. ──
struct HuntSource {
    enum Kind { Kill = 0, Dig = 1 };
    int    kind = Kill;
    int    zone = 0;
    std::vector<uint16_t> mobs;     // Kill: the species of that map that drop it
    int    count = 0;               // Kill: how many of them the map keeps
    int    level = 0;               // Kill: the highest level among them
    double perUnit = 0.0;           // per kill (their mix) / per find
    double perHour = 0.0;           // the estimate
    bool   usable = true;
    std::string why;                // why not usable / a warning
    std::string label;              // "kill Sea Scorpion in Coral Beach" / "dig in Gate of Caballa Relics"
};

struct HuntItem {
    uint32_t item = 0;
    int      want = 1;
    int      srcKind = -1, srcZone = 0;     // the chosen source (srcKind -1 = auto: the best one)
};

// The smart plan (act_hunt.cpp make_plan): every map that gives missing items, with the items worth getting there
// and the estimated time to finish the whole list starting with it; the route ahead (the same, step by step).
struct HuntPlan {
    struct Cand {
        HuntSource src;                         // kind, zone, the species to kill now (every one that pays), label
        std::vector<uint32_t> covered;          // the missing items worth getting there (faster than elsewhere + the way)
        std::map<uint32_t, double> rate;        // covered item -> an hour there now
        double farmHours = 0.0;                 // until the covered ones are all done there
        double totalHours = 0.0;                // the whole list starting here: the way, the farm, the rest one by one
        std::vector<uint16_t> skipped;          // species there that drop missing items, left out (too strong / not worth it)
    };
    struct Step {
        int      cand = -1;                     // index into cands (its covered items done there)
        double   hours = 0.0;                   // the way there + the farm
        std::vector<uint32_t> done;
    };
    std::vector<Cand> cands;                    // the quickest first
    std::vector<Step> route;
    std::map<uint32_t, double> best;            // each missing item's best rate anywhere (0 = no usable source)
    std::map<uint32_t, int>    need;            // missing item -> how many
    // missing item -> its weakest monster's level over ours, when every monster that drops it is past maxOver (hunted
    // anyway: the limit picks among sources, it never leaves an item with none)
    std::map<uint32_t, int>    over;
};

class ItemHuntAction : public BotAction {
public:
    ItemHuntAction() { enabled = false; }
    const char* key() const override { return "hunt"; }
    const char* title() const override { return "Item Hunt"; }
    void reset() override;
    Step step(BotCtx& c) override;
    void preempted() override { nav_.interrupted(); }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    std::vector<HuntItem> list;
    bool smart = true;              // plan the whole list (several items per map), else one by one in order
    bool stopWhenDone = true;
    bool driven = false;            // run by another action (the Key Quest's items): never stops the bot, only passes
    int  switchMin = 30;            // minutes in a source without one of its items: the next source (0 = never)
    int  maxOver = 10;              // smart: species at most this many levels over ours are hunted (the Item Hunt tab;
                                    //  an item only stronger ones drop: its weakest ones, HuntPlan::over)
    // Why no source is usable for what the list still misses: per item, its monsters' level over ours, a map no way
    // reaches now, the drill (the stop's reason).
    std::string why_none(const WorldModel& w) const;

    // Every source of an item for this character, the usable ones first, the best estimate first.
    static std::vector<HuntSource> sources(const WorldModel& w, uint32_t item);
    // The smart plan for what the list misses now (`excluded`: candidate keys left out, kind << 32 | zone).
    HuntPlan make_plan(const WorldModel& w, const std::set<uint64_t>& excluded = {}) const;
    const HuntPlan& smart_plan() const { return plan_; }   // the one the run follows (empty when not smart)
    // The run, before the actions (Bot::tick): the bag's counts, the current item and its source.
    void refresh(const WorldModel& w, uint64_t now, bool digOpen);
    int  current() const { return cur_; }               // index into the list (-1 = none: all done / no source)
    const HuntSource& source() const { return src_; }
    bool killing() const { return cur_ >= 0 && src_.zone > 0 && src_.kind == HuntSource::Kill; }
    bool digging() const { return cur_ >= 0 && src_.zone > 0 && src_.kind == HuntSource::Dig; }
    const std::vector<uint32_t>& items() const { return items_; }   // the list's ids (Loot, Town)
    int  have(const WorldModel& w, size_t i) const;
    bool done(const WorldModel& w) const;               // every item at its count
    // for the UI
    std::map<uint32_t, int> gained;                     // this run, per item
    uint64_t startedAt = 0, sourceAt = 0, lastGainAt = 0;
    std::string plan;
    bool serves(uint32_t item) const;                   // the current source gives it (smart: its plan's rates)
    // The current source given up (no way there, no progress there): left out a while, the next one chosen.
    void abandon_source(BotCtx& c, const std::string& why);
private:
    void choose(const WorldModel& w, uint64_t now);
    void smart_refresh(const WorldModel& w, uint64_t now, bool digOpen);
    const std::vector<HuntSource>& cached(const WorldModel& w, uint32_t item, uint64_t now);
    Navigator nav_;
    int      cur_ = -1;
    HuntSource src_;
    HuntPlan plan_;                 // smart: the last plan (plan_.cands[planCand_] = the source)
    int      planCand_ = -1;
    uint64_t planAt_ = 0;
    std::map<uint32_t, int> planNeed_;          // the needs it was made for
    std::map<uint64_t, uint64_t> penalty_;      // smart: candidate key -> left out until (no gain there / no way)
    bool     srcNote_ = false;      // a new source: noted in the log at the next step
    std::string giveUp_;            // a source given up: why (noted at the next step)
    std::vector<uint32_t> items_;
    std::map<uint32_t, int> lastHave_;
    std::set<uint64_t> tried_;      // (kind << 32 | zone) of the current item, given up
    uint64_t lostSince_ = 0;
    uint64_t arrivedAt_ = 0;        // in the source's map since (the switchMin clock)
    uint64_t checkAt_ = 0;          // the source is checked again now and then (level, drills)
    std::map<uint32_t, std::pair<uint64_t, std::vector<HuntSource>>> cache_;
    int      cacheLevel_ = -1;
};

// ── Drill: dig the map's buried items (ProjectPAULA drill.cpp, act_drill.cpp has the rules). One dig = C2S
//    0x2C at a cell -> S2C 371 tells whether an item lies there and how deep -> the find (C2S 0x31 at that
//    depth once the client's gauge would have got there) or a free cancel (0x30). While a dig is open only
//    potions may go out: a skill, a sit or an attack ends it. Drill mode means no hunting: the Attack action
//    answers only who hits us, and only with `fightBack`. The map is worked block by block — a block is the
//    detector's square (else `lineCells` wide) — row by row in a snake from its nearest corner; a detector is
//    cast once, when a block starts; a block that keeps failing is left for another area. ──
struct DrillInfo;
struct MapDigInfo;
struct MapGrid;
class DrillAction : public BotAction {
public:
    DrillAction() { enabled = false; }
    const char* key() const override { return "drill"; }
    const char* title() const override { return "Drill"; }
    void reset() override;
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void preempted() override { walking_ = false; }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    enum Area { AroundStart = 0, WholeMap = 1 };
    enum Ground { AnyGround = 0, PreferGround = 1, OnlyGround = 2 };
    bool fightBack = false;         // kill what attacks us while drilling (the Attack tab's weapon / skills)
    int  area = AroundStart;
    int  radius = 320;              // AroundStart: cells within this of the start point
    bool moveOn = true;             // AroundStart: when the area runs dry or keeps failing, the start point moves on
    int  lineCells = 11;            // a block's side without a detector (with one: its square)
    int  ground = PreferGround;     // the drill's GroundNature: a dig on another ground costs twice the life
    bool forExp = false;            // a cell with nothing to find is dug to the bottom anyway (the dig's EXP)
    bool pickUp = true;             // pick up our finds (ours for 30 s), whatever the Loot lists say
    bool swap = true;               // a worn-out / useless drill -> the best other one of the bag
    std::vector<uint16_t> detectors;            // detection skills: one scan when a block starts (3013 / 3102)
    bool detects(uint16_t skill) const;
    void set_detect(uint16_t skill, bool on);
    // A dig is open (the model's): nothing but potions may go out until it ends (Bot::tick).
    bool digging(const WorldModel& w, uint64_t now) const;

    // for the UI
    bool hasTarget = false;
    int  targetX = 0, targetY = 0;  // the cell being walked to / dug (units, its centre)
    std::string plan;               // what the last S2C 371 decided
    int  probes = 0;                // digs asked this run
    int  runFinds = 0;              // finds this run
    uint64_t lastFindAt = 0;
    // the block being worked (cells [blockCellX, +blockSide) x [blockCellY, +blockSide) of zone blockZone)
    bool hasBlock = false;
    int  blockZone = -1, blockCellX = 0, blockCellY = 0, blockSide = 0;
    int  blockLeft = 0;             // its cells still to dig / probe
    int  blockFails = 0, failLimit = 0;        // probes in a row with nothing there / the limit that leaves it
    int  blockProbes = 0, blockFinds = 0;
    uint16_t blockScan = 0;         // the detector it was scanned with (0 = none)
    const std::map<uint64_t, uint64_t>& poor() const { return poor_; }   // block key -> left alone until
    // block keys: zone | the block's first cell
    static uint64_t block_key(int zone, int cx, int cy) { return uint64_t(uint16_t(zone)) << 32 | uint32_t(uint16_t(cx)) << 16 | uint16_t(cy); }
    // The reach (deepest item a scan shows) of a detection skill at our level; 0 = unknown.
    static int detector_reach(const WorldModel& w, uint16_t skill);

private:
    Step dig_step(BotCtx& c);
    Step pickup_step(BotCtx& c);
    Step swap_step(BotCtx& c, const MapDigInfo& md);
    Step block_step(BotCtx& c, const DrillInfo& d, const MapDigInfo& md, const MapGrid& g, int need);
    Step scan_step(BotCtx& c, const MapGrid& g);
    Step target_step(BotCtx& c, const DrillInfo& d, const MapGrid& g, int need);   // the Crazy Drill: nearest first
    Step go_cell(BotCtx& c, const MapGrid& g, int cell, bool known);
    Step walk_cell(BotCtx& c, const MapGrid& g, int cell, int x, int y, const char* what);
    void decide(BotCtx& c);         // a new S2C 371: dig to which depth, or cancel
    void bfs(const BotCtx& c, const MapGrid& g, bool nearArea);
    bool valid_cell(const BotCtx& c, const DrillInfo& d, const MapGrid& g, int need, int cx, int cy, bool cdm,
                    bool ignoreArea, bool* known) const;
    int  next_in_block(const BotCtx& c, const DrillInfo& d, const MapGrid& g, int need, int* left) const;
    int  nearest_cell(const BotCtx& c, const DrillInfo& d, const MapGrid& g, int need, bool ignoreArea, bool awayFromPoor) const;
    uint16_t block_detector(const BotCtx& c, const MapDigInfo& md, int* side) const;
    void start_block(BotCtx& c, const DrillInfo& d, const MapDigInfo& md, const MapGrid& g, int cell, int need);
    void leave_block(BotCtx& c, bool poor, const char* why);
    bool in_block(int cx, int cy) const;
    int  radius_area() const { return radius; }
    int  unknown_cells(const BotCtx& c, const MapGrid& g, int x, int y, int radius, int need, int* total) const;
    int  nature_share(const BotCtx& c, const MapGrid& g, int nature);

    int      planDepth_ = -1;       // the open dig: -1 undecided, 0 cancel, > 0 dig that deep
    uint64_t planAt_ = 0;           // when its C2S 0x31 may go
    uint64_t planFor_ = 0;          // the 371 (infoAt) the plan is for
    uint64_t sentAt_ = 0;           // our last drill C2S
    uint64_t resultAt_ = 0;         // the last S2C 0x32 seen
    int      silent_ = 0;           // asks in a row with no answer
    uint64_t pauseUntil_ = 0;
    std::map<uint32_t, uint64_t> skip_;        // cell key -> left alone until
    // the same cell asked again and again without its dig starting (a refusal S2C 0x2E, no answer, a walk that never
    // ends there): cell key -> asks without a 371; 2 -> the cell is left 10 min. A block where 4 cells went that way
    // is left too (another area).
    std::map<uint32_t, int> tries_;
    uint32_t askedCell_ = 0;                   // the cell of our last 0x2C
    uint64_t refusedSeen_ = 0;                 // the last S2C 0x2E handled
    uint64_t infoSeen_ = 0;                    // the last 371 handled
    std::vector<uint64_t> refusals_;           // when the recent refusals came
    int      blockMisses_ = 0;                 // cells of this block whose dig never started
    void     missed_cell(BotCtx& c, uint32_t cell, const std::string& why);
    bool     walking_ = false;      // a 0x18 leg toward a far cell
    uint64_t walkUntil_ = 0;
    uint64_t equipUid_ = 0, equipAt_ = 0;
    uint16_t pickOid_ = 0;
    uint64_t pickAt_ = 0;
    std::map<uint16_t, uint64_t> pickBan_;
    std::set<uint64_t> badDrill_;              // drills whose equip got no answer (S2C 0x53)
    int      shareZone_ = -1;
    int      shareCount_[8] = {}, shareTotal_ = 0;
    // the block
    int      blockStep_ = 0;        // the block grid's step (side, or side - 1 for an odd side: starts stay odd)
    int      corner_ = 0;           // its snake's corner: bit 0 = from the right, bit 1 = from the bottom
    bool     scanWanted_ = false;   // the block's scan is still to cast
    uint64_t scanWait_ = 0;         // the detector's cool time: wait for it until this, then work without
    int      scanWalks_ = 0;        // walks toward the scan point (a few, then the block goes without it)
    std::map<uint64_t, uint64_t> poor_, scanned_;  // block key -> left alone until / scanned at
    uint64_t stuckSeen_ = 0;        // the last StuckGuard event handled
    bool     hasPoor_ = false;      // the last block left for failing (a new area keeps away from it)
    int      poorX_ = 0, poorY_ = 0, poorZone_ = -1;
    // the BFS over the map's cells (reused: a stamp per search)
    std::vector<uint32_t> seen_;
    std::vector<int32_t>  from_;
    std::vector<uint16_t> steps_;
    std::vector<int32_t>  order_;   // the cells in the order the BFS reached them
    uint32_t stamp_ = 0;
    int      bfsStart_ = -1;
};

// ── Roam: nothing to fight or loot -> walk. Stay = back to the start point; Around = within a radius of it;
//    Map = the whole map. With the map's grid it is a SWEEP (act_roam.cpp): the map in areas, each one
//    "seen" when its centre comes within the Attack's radius of us, gone over in ROUNDS (each area once a
//    round); the next goal is drawn among the near ones not seen this round, and each round turns the other
//    way round the map, so the route changes from round to round. It walks the grid path (Navigator legs,
//    no pauses); an area with no way to it is left out for 10 minutes. A monster the Attack wants but out
//    of its radius goes first: the server shows every monster of the map (a map is one sector), so the bot
//    walks to the nearest one instead of searching. Monster quest mode: the Roam is asked even when off,
//    and with none of the quest's monster to take it PATROLS its spawn spots (its regeneration points in
//    groups) in rounds the same way, with a short pause at each. ──
class RoamAction : public BotAction {
public:
    enum Mode { Stay = 0, Around = 1, WholeMap = 2 };
    const char* key() const override { return "roam"; }
    const char* title() const override { return "Roam"; }
    void reset() override;
    void observe(BotCtx& c) override;           // the areas seen, the spots passed; a stuck leg
    Step step(BotCtx& c) override;
    void preempted() override { leg_ = false; nav_.interrupted(); }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    int mode = WholeMap;
    int areaRadius = 1500;                      // Around
    int legMin = 250, legMax = 700;             // (no map file: random legs)
    int spotPause = 3;                          // monster quest patrol: seconds at each spawn spot (0.6..1.4 x)
    bool active() const { return leg_ || hasGoal; }      // walking somewhere (destX / destY: the radar)
    int destX = 0, destY = 0;
    // A round over goals (the sweep's areas, the patrol's spots): each one once, then the next round, which
    // turns the other way round the middle.
    struct Round {
        int      n = 0;                         // 1 = the first (0 = not started)
        uint64_t at = 0;                        // its start: a goal seen / visited since then is done
        int      sense = 1;                     // +1 clockwise, -1 counter-clockwise (as the radar shows it)
    };
    // A patrol spot: a group of the quest monster's regeneration points (done = visited this round).
    struct Spot { int x = 0, y = 0, points = 0; bool done = false; };
    // the sweep, for the UI
    int    sweepAreas = 0, sweepDone = 0;       // the areas with an open centre / seen this round
    Round  sweepRound;
    bool   hasGoal = false;
    int    goalX = 0, goalY = 0;
    std::string goalWhy;
    // the monster quest's patrol, for the UI (spots of map spotZone(), empty = none)
    std::vector<Spot> spots;
    Round  patrolRound;
    int    spotPoints = 0;                      // the regeneration points within reach they group
    int    spotZone() const { return spotZone_; }
    uint16_t spotMob() const { return spotMobs_.empty() ? 0 : spotMobs_.front(); }
    const std::vector<uint16_t>& spotMobs() const { return spotMobs_; }
    int    spotGoal() const { return spot_; }   // the spot walked to / paused at (-1 = none)
    bool   spotPaused() const { return spot_ >= 0 && pauseUntil_ != 0; }
private:
    bool pick(BotCtx& c, int& x, int& y);
    Step legs_step(BotCtx& c);                  // the old random legs (no map file)
    Step sweep_step(BotCtx& c, const MapGrid& g);
    Step quest_step(BotCtx& c, const MapGrid* g);
    void build(const BotCtx& c, const MapGrid& g);
    void build_spots(BotCtx& c, const std::vector<uint16_t>& mobs);
    int  next_area(BotCtx& c);
    int  next_spot(BotCtx& c);
    Step walk(BotCtx& c, int x, int y, int within, const std::string& what);
    bool     leg_ = false;
    uint64_t legEnd_ = 0;                       // expected arrival + slack
    uint64_t nextAt_ = 0;                       // a short pause between legs
    double   heading_ = 0.0;
    uint64_t stuckSeen_ = 0;
    Navigator nav_;
    // where the last sweep / patrol step stood: a fight or a pick-up in between took us elsewhere -> the goal
    // is drawn again from there
    int      lastX_ = 0, lastY_ = 0;
    bool     hasLast_ = false;
    // the areas of the map (sweepZone_): an open centre each (-1 = none), when it was last seen, left out until
    int      sweepZone_ = -1;
    std::vector<int32_t>  areaX_, areaY_;
    std::vector<uint64_t> seenAt_, badUntil_;
    int      midX_ = 0, midY_ = 0;              // the middle of the areas (the rounds turn round it)
    int      area_ = -1;                        // the area walked to
    uint64_t areaAt_ = 0, areaCapMs_ = 0;
    // monster quest / item hunt: the spots of spotMobs_ in spotZone_, when each was last visited / passed, left out until
    int      spotZone_ = -1;
    std::vector<uint16_t> spotMobs_;
    std::vector<uint64_t> spotSeen_, spotBad_;
    int      spotMidX_ = 0, spotMidY_ = 0;
    int      spot_ = -1;
    uint64_t spotAt_ = 0, spotCapMs_ = 0;
    uint64_t pauseUntil_ = 0;                   // at the spot: the next one after this (0 = not there yet)
    std::map<uint16_t, uint64_t> skip_;         // monsters walked to that the Attack still did not take
};

// ── Town: the trips away from the hunt. Dead -> C2S 0x16 Go to Town (the server: 30 % HP, the return
//    town); in town rest (0x40 13) up to the set HP / MP; then the supply errands; then back to the
//    map it left (or a chosen one) and its start point. A supply trip starts when a refilled item is
//    running out or the bag is full / too heavy: the warehouse (Warehouse_Start, 0x8B per item), the
//    shop (0x46, sell 0x39F/76, prices 0xA4, buy 0x48, close 0x4F), then back. The way between maps is
//    the Navigator's (portals + NPC teleports). ──
struct BuyRow {
    uint32_t item = 0;
    int      below = 10;            // a trip starts when the bag holds this many or fewer
    int      upTo = 100;            // and buys up to this many
    bool     on = true;
};

class TownAction : public BotAction {
public:
    TownAction();
    const char* key() const override { return "town"; }
    const char* title() const override { return "Town"; }
    void reset() override;
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void preempted() override { nav.interrupted(); }
    void save(std::ostream& o) const override;
    void load(const std::string& k, const std::string& v) override;

    // ── the Town tab: death, recovery, the way back ──
    bool onDeath = true;            // dead -> go to town (off: the bot stops, as before)
    int  deathDelaySec = 5;
    int  recoverHp = 90, recoverMp = 50;   // rest in town up to these (%)
    enum ReturnMode { ReturnWhereLeft = 0, ReturnChosen = 1, ReturnStay = 2 };
    int  returnMode = ReturnWhereLeft;
    int  returnZone = 0;            // ReturnChosen
    bool supplyAfterDeath = true;   // run the supply errands after a death too

    // ── the Auto-Supply / Auto-Sell / Auto-Storage tabs: a trip runs for any of the three ──
    bool supply = false;            // Auto-Supply: buy the refill rows
    std::string shopKey;            // AtlasNpc::key() of the shop
    std::vector<BuyRow> buy;
    bool sell = false;              // Auto-Sell: every item but the kept ones (sellList.black) and the protected ones
    IdFilter sellList;              // always AllButBlack: black = the items checked "keep" (the whitelist is gone)
    bool sellRare = false;          // rare items (kind 2 / 3) sold too
    // the common gear grades (EquipTier: Old / Strong / Art / Ult.) are sold whatever the list's mode (not worn, not
    // blacklisted by id, not kept): all four by default
    bool sellTier[ET_Count] = { true, true, true, true };
    bool store = false;             // Auto-Storage: every item but the kept ones (storeList.black) and the protected ones
    std::string whKey;              // AtlasNpc::key() of the warehouse keeper
    IdFilter storeList;             // always AllButBlack (as sellList)
    bool errands() const { return supply || sell || store; }   // a trip may run
    // Never sold or stored whatever the lists say: what the bot itself uses (the potions the Heal drinks, the port items /
    // the Portable Port AD the routes take) and the cash items (Ncash > 0)
    static bool protected_item(const ItemInfo& ii);
    // An old profile's whitelist (only those sold / stored): replaced by "every one but the checked ones"; that switch
    // is turned off until the user checks what to keep (the UI says so). Called after a profile load.
    void after_load();
    std::string sellMigrated, storeMigrated;   // the notice (empty = none)
    std::set<uint32_t> keep;        // never sold or stored, whatever the lists say (the Item Hunt's items; Bot::tick)
    int  bagFull = 270;             // items in the bag that start a trip (the bag holds 300)
    bool onOverweight = true;       // a pick-up refused for the weight (S2C 0x23) starts one
    int  weightPct = 0;             // the bag weighs this % of the max (MaxWT) or more: a trip (0 = off)
    int  keepGelder = 0;            // never spend below this

    // what it does (for the UI)
    enum Phase { Idle, Dead, Recover, Store, Shop, Return };
    int         phase = Idle;
    std::string why;                // what started the trip
    int  backZone = -1, backX = 0, backY = 0;   // where the trip ends
    bool backPoint = false;
    bool manual = false;            // a trip asked for in the UI (Travel: that map, whatever the return mode)
    Navigator nav;
    bool trip() const { return phase != Idle; }
    // back in town after a death, before its rest is over (the heal leaves the potions alone then)
    bool recovering() const { return (phase == Dead || phase == Recover) && fromDeath_ && (recoverHp > 0 || recoverMp > 0); }
    static const char* phase_name(int p);
    // the Supply tab's "go now" / the Travel tab's "go to that map"
    void start_supply(const WorldModel& w, uint64_t now, int anchorZone, int ax, int ay, const std::string& reason);
    void travel_to(int zone, uint64_t now);
    // the refill rows the shop sells / what would be sold or stored now
    bool sellable(const WorldModel& w, const InvItem& it) const;
    bool storable(const WorldModel& w, const InvItem& it) const;
    std::string need(const WorldModel& w) const;     // why a supply trip is due ("" = not)

private:
    Step dead_step(BotCtx& c);
    Step recover_step(BotCtx& c);
    Step store_step(BotCtx& c);
    Step shop_step(BotCtx& c);
    Step return_step(BotCtx& c);
    void next_errand(BotCtx& c);    // Store -> Shop -> Return, skipping what has nothing to do
    void finish(BotCtx& c, const std::string& text);
    bool in_buy_list(uint32_t item) const;
    const Entity* npc_entity(const BotCtx& c, const AtlasNpc& n) const;

    int npc_index(const WorldModel& w, const std::string& key) const;   // the atlas NPC of a profile key (cached)

    bool     fromDeath_ = false;
    // the rest after a death: HP / MP when they last changed, when; since when it rests
    int64_t  restHp_ = -1, restMp_ = -1;
    uint64_t restChangeAt_ = 0, restSince_ = 0;
    int      sub_ = 0;              // the phase's own step
    uint64_t subAt_ = 0;
    uint16_t npcEid_ = 0;
    uint64_t townSentAt_ = 0;
    uint64_t lastTripEnd_ = 0, lastOverweight_ = 0;
    uint64_t noTripUntil_ = 0;      // a trip did not fix its cause: no new one before this
    uint64_t lostSince_ = 0;        // the navigator has had no way since
    // the warehouse
    uint64_t storeUid_ = 0;         // the item out (0 = none)
    uint8_t  storeTab_ = 2;
    std::set<uint64_t> storeSkip_;
    std::map<uint64_t, std::set<uint8_t>> storeTried_;   // uid -> tabs that refused it
    // the shop
    int      sellRounds_ = 0, buyTries_ = 0;
    double   buyScale_ = 1.0;
    bool     sellDone_ = false, buyDone_ = false;
    mutable std::map<std::string, int> npcCache_;
};

} // namespace nj
