#pragma once
// The quest runner: what the Key Quest and the Story share (quest_runner.cpp). A plan of quests (QuestGraph::expand:
// each one after what its NPC's dialog puts first), each taken and handed in through its NPC's dialog the way a player
// clicks it (DialogWalk), their items from the Item Hunt's planner (kill / dig: `gather`), from other quests' rewards,
// or from an NPC that hands them out (Give_Item); a monster quest's kills hunted in its map. The book (what the server
// told of each quest's history) is the character's: the Bot keeps one for both.
#include "app/bot/bot_actions.h"
#include "app/bot/dialog_walk.h"
#include "app/game_data.h"
#include "app/quest_graph.h"
#include "app/world.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nj {

class Bot;

// What the server told of each quest's history (QKey::v() -> done lo..hi times): the packets (132 / 150 / 210 / 219 /
// 234 / 235 / 829 / 831 / 833) and the dialogs' answers. The app keeps it in its own file per character.
class QuestBook {
public:
    std::map<uint64_t, QuestHist> h;
    int  version() const { return version_; }
    void set(QKey q, int lo, int hi);
    void merge_model(const WorldModel& w);              // the session's exact histories
    // A dialog's answers: what they say of histories. The lines learned go to `notes`. A quest whose history the
    // server told exactly this session (`w`) is left as it is: a check answered before the errand's own take / hand-in
    // speaks of the history before it (a live log: Bean's Request handed in, then "learned done fewer than 1 times").
    void learn(const std::vector<QCond>& cs, std::vector<std::string>* notes = nullptr, const WorldModel* w = nullptr);
    std::string text() const;                           // "q7107:5:5" per line (q / m / s = normal / monster / scenario)
    void load(const std::string& text);                 // merged into what is there
    void clear() { h.clear(); ++version_; }
private:
    int version_ = 0;
};

class QuestRunner : public BotAction {
public:
    QuestRunner() { enabled = false; gather.driven = true; gather.stopWhenDone = false; }
    void reset() override;
    void observe(BotCtx& c) override;
    Step step(BotCtx& c) override;
    void preempted() override;

    bool usePortal = true;              // a monster quest: back through its success portal while its timer runs
    // Getting an item another way (the Key Quest / Story Quest tabs): a shop that sells it is the quickest (bought within
    // the galder below), else an exchange / a compound with what the bag holds, then the quests, the NPCs, the exchange
    // shops; a compounder makes what nothing else gives (its ingredients gathered like the rest).
    bool buyItems = true;
    int  buyMaxGelder = 200000;         // the most one purchase may cost
    bool compoundItems = true;
    int  keepGelder = 0;                // never spent below this (Bot::tick: the Town's "galder always kept")
    void save_acquire(std::ostream& o, const char* key) const;
    bool load_acquire(const std::string& k, const std::string& v);
    // A compound's spins for `made` items (its Probability: the successes expected), 1..255.
    static int compound_spins(const CompoundDef& c, int made);

    // The Story's rule: its scenario quests come one after the other, so one whose history is not known counts as not done
    // (the plan starts at the first and goes in order) - but those of the chapters before the start one count as done.
    struct LinearRule {
        bool on = false;
        std::set<uint64_t> before;
        bool undone(QKey q) const { return on && q.scen && !before.count(q.v()); }
    };
    // The book + the world, as the planner sees it.
    class View : public QStateView {
    public:
        View(const QuestBook& b, const WorldModel& w, const LinearRule* linear = nullptr) : b_(b), w_(w), linear_(linear) {}
        int  hist_lo(QKey q) const override;
        int  hist_hi(QKey q) const override;
        bool active(QKey q) const override;
        bool items(QKey q) const override;
        bool has_item(uint32_t item) const override;
        int  level() const override;
        int  ftype() const override;
        bool monq_busy(QKey q) const override;
        bool gatherable(uint32_t item) const override;
        int  zone() const override { return w_.self.zone; }
        int  job() const override { return w_.self.job; }
        int  model() const override { return w_.self.species; }
        bool assume_undone(QKey q) const override { return linear_ && linear_->undone(q); }
    private:
        QuestHist hist(QKey q) const;
        const QuestBook& b_;
        const WorldModel& w_;
        const LinearRule* linear_;
    };
    // How many runs of a quest the bag holds the items for (the server: one stack of each with its count).
    static int runs_in_bag(const WorldModel& w, const QuestInfo& q);
    // The runs whose items are gathered at a time: as many as weigh 40 % of the max weight (1..runs; 5 when unknown).
    static int batch_runs(const WorldModel& w, const QuestInfo& q, int runs);
    // A monster quest's sure reward at that level (null: none / one at random).
    static const QuestResultRow* monq_reward(const GameData* gd, int qid, int level);
    // The server's level check on a take (quest.cpp quest_accept / squest_accept: MinLevel..MaxLevel, both 0 = any;
    // MonQuest the same on MonsterQuestInfo): 0 = our level may take it, -1 = under it (`lv` = MinLevel: it waits),
    // 1 = over it (`lv` = MaxLevel: never again).
    static int level_gate(const GameData* gd, QKey q, int level, int* lv = nullptr);

    // The plan for the settings now (the UI's preview when the bot is not running): the action's own quests, then
    // the quests and NPCs whose rewards / gifts give an item nothing drops or buries.
    std::vector<QJob> make_plan(const WorldModel& w, const QuestBook& book) const;
    // The quests and NPCs whose rewards / gifts give an item the jobs need that nothing drops / buries (not a sticker,
    // not through a key quest chain).
    void add_providers(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const;
    // A job whose NPC no way reaches from here now: the item that would open one (an NPC move's item, a port item) joins
    // what it needs, so its provider joins the plan. true = one was added.
    bool add_unlocks(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const;
    // The run, before the actions (Bot::tick): the book from the packets, the plan, the gather list.
    void refresh(const WorldModel& w, uint64_t now, bool digOpen, QuestBook& book);
    const std::vector<QJob>& jobs() const { return jobs_; }
    ItemHuntAction gather;              // the plan's items (driven)
    bool gathering() const { return gathering_; }   // the gather has the floor (Bot::tick wires its source)
    bool monq_hunting(const WorldModel& w) const;   // a monster quest of the plan is on and its kills not done
    uint16_t monq_mob(const WorldModel& w) const;
    int  monq_zone(const WorldModel& w) const;
    const std::vector<uint32_t>& items() const { return items_; }   // the plan's items: always picked up, never sold

    // each job now, for the UI (same order as jobs())
    enum JobState { JDone, JOn, JReady, JLater, JWaitItem, JBlocked, JHeld, JNoWay, JLevel };
    struct JobInfo {
        int state = JLater;
        int lo = 0, hi = -1;            // its history as known
        int runsLeft = 0;               // runs still to hand in (a give job: items still to get)
        int haveRuns = 0;               // runs the bag holds the items for
        int batch = 0;                  // the runs gathered at a time (their items weigh at most 40 % of the max weight)
        bool shortItem = false;         // an item nothing drops / buries that the plan will not get enough of
        int needLevel = 0;              // JLevel: its MinLevel
        std::string note;
    };
    const std::vector<JobInfo>& job_info() const { return info_; }
    static const char* state_name(int s);
    std::string plan;                   // what comes next
    int taken = 0, handedIn = 0;        // this run
    uint64_t startedAt = 0;
    bool errand_on() const { return walk_.busy() || walk_.sent() || errand_.shop >= 0; }
    QKey errand_quest() const { return errand_.q; }     // the errand under way (errand_on)
    const std::string& errand_what() const { return errand_.what; }
    const DialogWalk& walk() const { return walk_; }
    // Nothing to do this tick (every job waits): the lower actions may fight / roam meanwhile (Bot::tick).
    bool idle() const { return idle_; }
    // The bot made no progress for a while (Bot's stall watch): the errand under way is dropped and its job left alone
    // `holdMs`, the gather's source given up, every walk planned again.
    void stalled(BotCtx& c, uint64_t holdMs);

protected:
    // the action's own quests (QuestGraph::expand of each, in order)
    virtual void plan_own(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const = 0;
    virtual std::string plan_key() const = 0;                   // the settings the plan depends on
    virtual void keep_items(const WorldModel&, std::vector<uint32_t>&) const {}   // more items never sold
    // taken as soon as it is ready, before its items (a scenario quest: a map / an NPC may wait for it)
    virtual bool take_first(const QJob& j) const { return j.q.scen; }
    virtual std::string done_text() const = 0;                  // the stop when everything is done
    virtual std::string idle_text() const { return "nothing picked"; }   // no plan at all
    virtual bool configured(const WorldModel& w) const = 0;     // its settings name something to do
    virtual const char* label() const = 0;                      // "key quest" / "story" (the log)

private:
    struct Errand {
        QKey q;
        bool complete = false, give = false;
        uint32_t giveItem = 0;
        int haveBefore = 0;
        std::string what;
        // an exchange shop: 0 its list (S2C 249), 1 the exchange's answer (255 / 191 / 256), 2 closed (C2S 257)
        int exchange = 0, exPhase = 0;
        uint64_t exAt = 0;
        bool exOk = false;
        std::string exWhy;
        // a shop (Atlas::npcs()): 0 to it + open (C2S 0x46), 1 S2C 0xA8, 2 buy (0x48), 3 its answer, 4 closed (0x4F)
        int shop = -1, shopPhase = 0, shopWant = 0, shopTries = 0;
        uint16_t shopEid = 0;
        uint64_t shopAt = 0;
        // a compounder: 0 its list (S2C 188), 1 the roll (227 -> 228), 2 made (186 -> 190 / 191), 3 closed (192)
        int compound = 0, cmpPhase = 0, cmpWant = 0, cmpSpins = 0;
        uint64_t cmpAt = 0;
        std::vector<act::SellLine> cmpItems;
        bool cmpOk = false;
        std::string cmpWhy;
    };
    Step begin_errand(BotCtx& c, int job, bool complete);
    Step errand_step(BotCtx& c);
    Step exchange_step(BotCtx& c);
    Step shop_step(BotCtx& c);
    Step compound_step(BotCtx& c);
    void errand_outcome(BotCtx& c, bool ok, const std::string& why, const std::string& okText);
    // its take's NPC stands in a map only a port item opens (used up) and we are not in there: one trip, its items first
    bool one_trip(const WorldModel& w, const QJob& j) const;
    Step monq_step(BotCtx& c, const QJob& j);
    const QGate* best_gate(const QuestGraph& g, const QPlace& p, const View& v) const;
    bool held(const QJob& j, uint64_t now) const;
    void learn(BotCtx& c);

    std::vector<QJob> jobs_;
    std::vector<JobInfo> info_;
protected:
    mutable LinearRule linear_;         // (the Story's plan_own sets it)
private:
    std::vector<uint32_t> items_;
    bool     gathering_ = false;
    uint64_t planAt_ = 0;
    int      planVer_ = -1, planLevel_ = -1;
    int      planRegion_ = -1;          // the map only an item opens that we were in (0 = an open one): planned again on a change
    std::string planKey_;
    DialogWalk walk_;
    Errand   errand_;
    std::map<uint64_t, int> refusals_;
    std::map<uint64_t, uint64_t> holdUntil_;    // job quest -> left alone until
    Navigator nav_;                     // the monster quest's hunt map
    uint64_t portalAt_ = 0, monqTakenSeen_ = 0;
    bool     portalFailed_ = false;
    uint64_t lostSince_ = 0;
    bool     idle_ = false;
    uint64_t noSourceSince_ = 0;        // the gather has had no source since (the stop waits 20 s: a source given up is
                                        //  replaced by the gather's next plan, not in the same tick)
    // job quest -> times the stall watch dropped its errand / left alone until: kept over a reset (the stall watch's
    // restart must not bring it straight back)
    std::map<uint64_t, int> stallDrops_;
    std::map<uint64_t, uint64_t> stallHold_;
    // can we get to that map from here now (the atlas route with our level / galder / bag)? cached 30 s per pair
    struct ReachMemo { uint64_t at = 0; int level = -1; bool ok = false; std::string why; };
    mutable std::map<uint64_t, ReachMemo> reach_;
    mutable std::map<int, uint32_t> unlockFor_;  // the last plan's maps that an item would open -> that item
    bool reachable(const WorldModel& w, uint64_t now, int zone, std::string* why) const;
};

} // namespace nj
