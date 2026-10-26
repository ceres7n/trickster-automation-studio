#pragma once
// The bot: the main tool of Trickster Automation Studio. It is a list of ACTIONS in priority order — heal, town, rest,
// buffs, loot, attack, drill, roam — each with its own settings and state (bot_actions.h, one .cpp each).
// Every tick each enabled action first observes the world, then they are asked in order what to do:
//   Pass = nothing for me, ask the next one;  Wait = I am busy (lower ones wait);  Act = send `out`.
// At most one C2S goes out per tick (actions.cpp builds it). Everything the bot knows comes from the
// packets (world.cpp); the server rules it follows are ProjectPAULA's, noted in each action.
#include "app/actions.h"
#include "app/bot/act_keyquest.h"
#include "app/bot/act_story.h"
#include "app/bot/bot_actions.h"
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace nj {

class WorldModel;

class Bot {
public:
    Bot();
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;

    // the actions, in priority order (order_ points at them)
    HealAction   heal;
    TownAction   town;              // death / supply trips (asked even when off while a trip it was given runs)
    BuffAction   buffs;
    KeyQuestAction keyq;            // on = key quest mode: a town's sticker quests + its key quest (their items, their monsters)
    StoryAction  story;             // on = story mode: the scenario quests, chapter by chapter
    QuestAction  quest;             // on = monster quest mode: combat only in its map, on its monster
    ItemHuntAction hunt;            // on = item hunt mode: where the list's items come from (kill / dig there)
    LootAction   loot;
    AttackAction attack;
    DrillAction  drill;             // on = drill mode: no hunting (the Attack only answers hits, with drill.fightBack)
    RoamAction   roam;
    const std::vector<BotAction*>& actions() const { return order_; }
    // The Drill and the combat exclude each other — turning one on turns the other off (in drill mode only
    // the Drill's "fight back" fights); the Key Quest, the Monster Quest and the Item Hunt need the Attack (on:
    // Attack on, Drill off, the other two off; Attack off: all three off too). Call it after the switches may have
    // changed (the UI, a profile).
    void sync_modes();

    bool run = true;                // move type of every request: 2 run / 1 walk
    SkillGate gate;                 // one skill request at a time (bot_actions.h)
    StuckGuard guard;               // the client draws us somewhere else: stand there again (bot_actions.h)
    QuestBook book;                 // what the server told of each quest's history (the Key Quest's and the Story's)
    Reach reach;                    // walking distances from where we stand (target / item choice)

    bool        running = false;
    std::string doing;              // what it is doing right now
    std::deque<std::string> log;    // newest last
    int anchorX = 0, anchorY = 0;   // where it started (the "stay" / "roam around" centre)
    int anchorZone = -1;            // the map of that point
    bool travelOnly = false;        // started by the Travel tab: stops when the trip ends

    // session stats (since start)
    uint64_t startedAt = 0;
    int64_t  expAtStart = 0, moneyAtStart = 0;

    void start(const WorldModel& w, uint64_t now);
    // finished = the job is done (the item hunt's list, the quest's runs), else it stopped on a problem / by you
    void stop(const std::string& why, bool finished = false);
    void stop_by_user(const std::string& why) { stop(why); stopByUser = true; }   // (no notice)
    // the last stop (the app notifies each new one but "by you": stopCount moves at every stop)
    int         stopCount = 0;
    std::string stopWhy;
    bool        stopFinished = false;
    bool        stopByUser = false;
    // One decision. true = `out` should be sent now. Call it often; it paces itself.
    bool tick(const WorldModel& w, uint64_t now, Action& out);

    void note(uint64_t now, const std::string& text);
    uint64_t last_request_at() const { return lastAction_; }   // the last C2S the bot sent (FILETIME, 0 = none yet)
    uint64_t notes = 0;             // lines noted since the bot was made (the app appends the new ones to its log file)
    int      stalls = 0;            // the stall watch's remedies this run (Overview)
    std::mt19937& rng() { return rng_; }

    // profile (one per character): "action.key=value" lines
    std::string save_text() const;
    void        load_text(const std::string& text);

private:
    std::vector<BotAction*> order_;
    bool wasDrill_ = false, wasAttack_ = true, wasQuest_ = false, wasHunt_ = false, wasKeyq_ = false, wasStory_ = false;   // last seen
    uint64_t lastTick_ = 0;
    uint64_t lastAction_ = 0;
    uint64_t idleNoteAt_ = 0;       // the idle watchdog's last snapshot
    std::string lastNote_;          // note(): the last line and how many times it came again since
    int      noteRepeats_ = 0;
    void idle_watch(const BotCtx& c, bool digging);
    // the stall watch: no progress (new ground: 150+ units from every spot of the stall, a map change, a kill, EXP,
    // galder, an item, a quest step) while a goal mode runs (key quest, story, monster quest, item hunt, a town trip)
    uint64_t stallSig_ = 0, stallSince_ = 0;
    int      stallZone_ = -1, stallLevel_ = 0;
    std::vector<std::pair<int, int>> stallSeen_;   // the spots stood on since the last progress (a walk back and forth is none)
    uint64_t progress_sig(const WorldModel& w) const;
    bool     stall_watch(BotCtx& c, bool digging, Action& out);
    std::mt19937 rng_;
};

} // namespace nj
