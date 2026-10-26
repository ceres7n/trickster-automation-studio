#pragma once
// The quests as the NPCs' dialogs offer them. Server rules (ProjectPAULA npc_talk.cpp / quest.cpp /
// monster_quest.cpp, = retail; the function ids are the client's table 0xFACE78):
//   - C2S 0x6F opens an NPC's dialog: its PreCheck (npc_db PreCheckType / Arg / Yes / No) picks the first message
//     (1 the quest Arg done once, 2 in progress, 3 an item in the bag, 4 our FType, 5 a level table, 8..10 none).
//   - A message's options link to other messages (C2S 0x72) or run a script function of the NPC (C2S 0x7F); a check
//     answers with the n-th of its arguments as the next message (S2C 115 carries its number):
//       Quest_Check(_n) on the slot's normal quest: 1 it can be taken, 2 on with its items, 3 on without them, 4 done
//         its MaxCount times (5 the money);  MonQuest_Check(_n): 1 can be taken, 2 killed, 3 not yet, 4 done its
//         MaxCount times (5 the level, 6 another one on);  CheckQuestState(_1/_2): 1 CheckQuestId(n) done
//         CheckQuestNum(n) times, else 2;  CheckMonQuestState: the same on CheckMonQuestId / Num;
//         ActiveQuestID_n: 1 ActiveQuestIDn on, else 2;  FType_X_Only: 1 that FType, else 2;  CheckHaveItem.
//   - Quest_Accept(_n) / Quest_Continue(_n) / Quest_Once_Accept(_n) take the slot's quest, Quest_Complete(_n) /
//     Quest_NH_Complete hand it in (the MonQuest_ ones for a monster quest). The server checks the level, the history
//     (MaxCount) and the items — NOT the way through the dialog: the way is how the game orders its quests (a chain:
//     "Survey 4" only after "Survey 3" was done five times), so the bot follows it.
// A gate = one way from a first message to a take: the conditions its checks need. The bot makes them true by doing
// the quests they name first (QuestGraph::expand), the way a player is led.
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nj {

class GameData;
struct AtlasNpc;
struct DialogOpt;
struct QuestInfo;

// A quest: a normal one (QuestInfo), a monster quest (MonsterQuestInfo) or a scenario quest (ScenarioQuestInfo, the
// Story). The same id can be two of them; an NPC's slot is one or the other by the functions its dialog calls on it.
struct QKey {
    bool mon = false;
    int  id = 0;
    bool scen = false;
    uint64_t v() const { return uint64_t(scen ? 2 : mon ? 1 : 0) << 32 | uint32_t(id); }
    static QKey from(uint64_t v) { QKey k; k.id = int(uint32_t(v)); k.mon = (v >> 32) == 1; k.scen = (v >> 32) == 2; return k; }
    bool operator<(const QKey& o) const { return v() < o.v(); }
    bool operator==(const QKey& o) const { return v() == o.v(); }
    bool operator!=(const QKey& o) const { return !(*this == o); }
    char tag() const { return scen ? 's' : mon ? 'm' : 'q'; }     // the book's / logs' prefix
};

// A condition of a gate.
struct QCond {
    enum Kind : uint8_t {
        HistGE,         // q done at least n times
        HistLT,         // q done fewer than n times
        Active,         // q on
        NotActive,
        Items,          // q on with its items in the bag (a monster quest: its kills done)
        NoItems,        // q on without them
        FType,          // our FType is n (0 P, 1 M, 2 S, 3 C)
        NotFType,
        HasItem,        // item n in the bag
        NoItem,
        Level,          // our level within n..n2
        MonqFree,       // no other monster quest on
        Opaque,         // a check the bot cannot judge (n = its function: CheckDailyValue's time, a random message ...): the
                        //   walk asks it and sees
        Never,          // a party / guild check the bot cannot follow
        Enter,          // the NPC's map is opened only by item n (a port item / an NPC move's item; n2 = the map it
                        //   opens): in the bag, or we are in there already
        Job,            // Check_Job: our job's bit in JobTypeFlag1 n (jobs 1..16) / JobTypeFlag2 n2 (17..33)
        NotJob,
        Gender,         // CheckGender: our model's parity is n (0 even -> its 1st answer, 1 odd -> its 2nd)
    };
    uint8_t kind = Never;
    QKey    q;
    int     n = 0, n2 = 0;
};

struct QGate {
    std::vector<QCond> conds;
    std::vector<int>   path;        // the message numbers on the way (the first one first)
};

// A call of an NPC's dialog on one of its slots: where a quest is taken / handed in / checked.
struct QPlace {
    int npc = -1;                   // Atlas::npcs() index
    int slot = 0;
    uint8_t func = 0;
    std::vector<uint16_t> args;     // as the client sends them
    std::vector<QGate> gates;       // a take / hand-in: the ways to it (empty = none from a first message)
};

struct QuestPlaces {
    QKey key;
    std::vector<QPlace> accepts, completes, checks;
};

// An exchange shop's exchange that gives an item (exchange_shop.cpp): the NPC's ExchangeShop_Start (func 22) and the
// ways to it, then C2S 254 with the exchange's ReqIDs.
struct QExchange {
    QPlace place;
    int    id = 0;                  // Atlas::exchange
};

// A compounder's compound that makes an item (compound.cpp): the NPC's Compound_Start (func 20) and the ways to it, then
// C2S 227 / 186 with the compound's ReqIDs.
struct QCompound {
    QPlace place;
    int    id = 0;                  // Atlas::compound
};

// What a quest's state is now, for the conditions (the bot's book + the world model). The history is known as a
// range: done at least lo times, at most hi (-1 = no upper bound known: a quest the bot never saw).
class QStateView {
public:
    virtual ~QStateView() = default;
    virtual int  hist_lo(QKey q) const = 0;
    virtual int  hist_hi(QKey q) const = 0;
    int  hist(QKey q) const { return hist_lo(q); }
    bool hist_known(QKey q) const { return hist_hi(q) >= 0 && hist_hi(q) == hist_lo(q); }
    virtual bool active(QKey q) const = 0;
    virtual bool items(QKey q) const = 0;           // on with its items / its kills done
    virtual bool has_item(uint32_t item) const = 0;
    virtual int  level() const = 0;
    virtual int  ftype() const = 0;
    virtual bool monq_busy(QKey q) const = 0;       // another monster quest than q is on
    // an item the bot can get by itself (a monster drops it / a map buries it)
    virtual bool gatherable(uint32_t item) const = 0;
    virtual int  zone() const { return -1; }        // the map we are in (-1 = not known)
    virtual int  job() const { return 0; }          // our class (0 = not known)
    virtual int  model() const { return 0; }        // our model (species, 0 = not known)
    // A quest whose unknown history is taken as "not done" (the Story's chapters: linear, done in order from the start
    // chapter) instead of "the walk will tell".
    virtual bool assume_undone(QKey) const { return false; }
};

// One step of a plan: q done `target` times. Its take and hand-in, the gate it goes through, what comes first.
struct QJob {
    QKey q;                         // (a give job: q.id = -item, unique per item)
    int  target = 0;
    uint32_t giveItem = 0;          // a give job: the item an NPC's Give_Item hands out (accept = into give_places)
    int  exchange = 0;              // a give job by an exchange shop: its exchange id (accept = into exchanges(giveItem))
    int  compound = 0;              // a give job by a compounder: its compound id (accept = into compounds(giveItem))
    int  shop = -1;                 // a give job by a shop: the shop (Atlas::npcs()) that sells giveItem
    int  accept = -1, complete = -1, check = -1;    // into QuestPlaces accepts / completes / checks (-1 = none)
    int  gate = -1;                 // into accepts[accept].gates
    std::vector<QKey> after;        // jobs that come first (each to its own target)
    std::vector<uint32_t> needItems;   // items the gate wants in the bag (CheckHaveItem / PreCheck 3)
    std::vector<QCond> assumed;     // conditions of the gate not known yet, taken as true (the walk will tell)
    bool keepActive = false;        // only to be on (an Active condition of another job's gate)
    bool blocked = false;
    bool notForUs = false;          // blocked by what the character is (its job / FType / gender): another class's variant
    std::string why;                // blocked: why
    int  runs = 0;                  // runs still to do (with its prerequisites')
};

// A quest whose result gives a town's sticker.
struct StickerQuest {
    QKey   q;                       // a normal quest
    int    runs = 0;                // its last run whose result gives the sticker (the target)
    double stickers = 0.0;          // expected stickers over runs 1..runs (a Type 1 row: one item at random)
    int    firstRun = 0;            // the first run that gives one
};

struct KeyTown {
    uint32_t    sticker = 0;
    std::string name;               // the sticker's name without " Sticker"
    int         need = 0;           // how many the key quest asks for (0 = no key quest in the data)
    QKey        key;                // the key quest (id 0 = none)
    std::vector<QKey> chain;        // the key quest, then its follow-ups of the same NPC (same MinLevel)
    std::vector<StickerQuest> quests;
};

class QuestGraph {
public:
    void build(const GameData& gd);
    const GameData* data() const { return gd_; }

    const QuestPlaces* places(QKey q) const;
    // the normal / scenario / monster quests whose result rows (all their items: Type 0) give that item
    const std::vector<QKey>& reward_quests(uint32_t item) const;
    const std::vector<KeyTown>& towns() const { return towns_; }
    const KeyTown* town(uint32_t sticker) const;
    std::string quest_name(QKey q) const;           // "Survey - 4" / "monster quest 266 (Electric Snail)"
    int  max_count(QKey q) const;                   // MaxCount (25 when unknown)

    // The function of a script id and the slot it works on (-1 = none / not a quest one).
    enum class Fn : uint8_t { None, Exit, Check, Accept, Continue, OnceAccept, Complete, NHComplete, Quit,
                              MCheck, MAccept, MContinue, MOnceAccept, MComplete, MNHComplete, MQuit, MRank,
                              CheckState, MCheckState, ActiveId, FTypeOnly, HaveItem,
                              SCheck, SAccept, SContinue, SComplete, SActiveId, SCheckState, GiveItem, TakeItem,
                              CheckJob, CheckGender, Opaque, Other };
    // ProjectPAULA's npc_script does not do these (S2C 116): CheckDailyValue 87, EventTalkStart 223 ... (a walk that meets
    // one goes on to its call directly)
    static bool server_lacks(uint8_t func);
    static Fn decode(uint8_t func, int& slot);
    // a normal or scenario quest's QuestInfo (null for a monster quest)
    const QuestInfo* info(QKey q) const;

    // An NPC's dialog call that gives its GiveItemID (Give_Item, func 119: one, no other check than the bag's room —
    // npc_talk.cpp npc_give_item): where an item no monster drops comes from (the Story's). Its gates as a take's.
    const std::vector<QPlace>& give_places(uint32_t item) const;
    // The exchange shops' exchanges that give that item (the Story's Approved Memory Kits).
    const std::vector<QExchange>& exchanges(uint32_t item) const;
    // The compounders' compounds that make that item.
    const std::vector<QCompound>& compounds(uint32_t item) const;
    // A map only an item opens: the item and the map it opens first (0 = an open map / none).
    uint32_t entry_item(int zone, int* root = nullptr) const;

    // The Story (libconfig ScenarioQuestEpisode: each chapter's first and last scenario quest; episodes 0..6) and the
    // scenario quests each chapter takes (the plan of its last one for a new character, less the earlier chapters').
    struct Chapter {
        int episode = 0, chapter = 0;
        std::string name;               // its title (English)
        int first = 0, last = 0;        // scenario quest ids
        std::vector<int> quests;        // the scenario quests it takes, in plan order
        int minLevel = 0;               // its last quest's MinLevel
    };
    const std::vector<Chapter>& chapters() const { return chapters_; }
    int chapter_of(int scenarioQuest) const;    // index into chapters() (-1 = none)

    // The ways from an NPC's first messages to a call (func + args) of its dialog.
    std::vector<QGate> gates_to(const AtlasNpc& n, uint8_t func, const std::vector<uint16_t>& args) const;
    // A dialog call's branches: (the argument whose message comes next, 1-based) -> the conditions that branch means.
    std::vector<std::pair<int, std::vector<QCond>>> branches(const AtlasNpc& n, const DialogOpt& o) const;
    // The NPC's first messages (its PreCheck): message -> the conditions that pick it.
    std::vector<std::pair<int, std::vector<QCond>>> starts(const AtlasNpc& n) const;

    // Is the condition true now / could the bot make it true (by doing quests, getting an item)? Unknown = the book
    // cannot tell (a history never seen): the planner takes it as true, the walk through the dialog finds out.
    enum class Truth { Yes, No, Fixable, Unknown };
    Truth eval(const QCond& c, const QStateView& v) const;

    // The jobs to get q done `target` times, its prerequisites first (they come before it in `jobs`); an existing
    // job of the same quest is raised to the larger target. false = q itself has no way (its job says why).
    bool expand(QKey q, int target, const QStateView& v, std::vector<QJob>& jobs) const;
    std::string cond_text(const QCond& c) const;
    // Why q cannot be taken now (down its plainest way: what stops it, through the quests it waits for).
    std::string explain(QKey q, const QStateView& v, int depth = 0) const;

private:
    bool expand_rec(QKey q, int target, const QStateView& v, std::vector<QJob>& jobs, std::set<uint64_t>& visiting,
                    int depth) const;
    // the cheapest way (runs of other quests) to take q through one of its places / gates; -1 = none
    double take_cost(QKey q, const QStateView& v, std::set<uint64_t>& visiting, int depth, int* place, int* gate) const;
    double gate_cost(QKey self, const QGate& g, const QStateView& v, std::set<uint64_t>& visiting, int depth) const;
    void   build_towns();

    const GameData* gd_ = nullptr;
    std::map<QKey, QuestPlaces> places_;
    std::vector<KeyTown> towns_;
    std::map<uint32_t, std::vector<QKey>> rewards_;  // item -> the quests that give it
    std::map<QKey, int> unreach_;                    // a quest whose NPCs all stand in maps no way leads to -> one of them
    std::map<uint32_t, std::vector<QPlace>> gives_;  // item -> the NPC calls that give it
    std::map<uint32_t, std::vector<QExchange>> exch_;   // item -> the exchanges that give it
    std::map<uint32_t, std::vector<QCompound>> comp_;   // item -> the compounds that make it
    struct Entry { uint32_t item = 0; int root = 0; };
    std::map<int, Entry> entry_;                     // a map only an item opens -> the item, the map it opens first
    std::vector<Chapter> chapters_;
    std::map<int, int> chapterOf_;
    void build_chapters();
    // take_cost's results for one state (reset by expand / explain and when the view changes)
    struct Memo { double cost = std::numeric_limits<double>::infinity(); int place = -1, gate = -1; };
    mutable std::map<uint64_t, Memo> memo_;
    mutable const QStateView* memoFor_ = nullptr;
    mutable int cuts_ = 0;
};

} // namespace nj
