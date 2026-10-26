#pragma once
// The game state as the packets describe it. Fed every captured packet in capture order
// (history replay included), it rebuilds the character, the entities around it, the items on
// the ground, the inventory and the skills. Layouts: ProjectPAULA's writers of each S2C
// (verified against the client there) — see the table in world.cpp.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace nj {

class GameData;
struct CapPacket;
struct EvAvatar;

enum EntityKind : uint16_t {
    EK_PLAYER = 1, EK_NPC = 2, EK_MONSTER = 3, EK_PET = 4, EK_SHOP = 5, EK_SKILL_MASTER = 14,
};
const char* entity_kind_name(uint16_t kind);

struct Entity {
    uint16_t    eid = 0;
    uint16_t    kind = 0;
    uint16_t    species = 0;        // a monster's mob id, a player's species
    uint8_t     job = 0;
    uint16_t    level = 0;
    uint32_t    stateFlags = 0;
    std::string name;               // UTF-8
    int         x = 0, y = 0;       // last known position
    int         dx = 0, dy = 0;     // destination while moving
    bool        moving = false;
    uint64_t    moveAt = 0;         // FILETIME of the last move / stop
    int64_t     hp = -1;            // last known HP (-1 = unknown)
    int64_t     maxHp = 0;          // monsters: mob_db
    bool        dead = false;
    uint16_t    owner = 0;          // a monster's loot owner (S2C 365)
    uint16_t    lastTarget = 0;     // whom it swung at last (S2C 0x1F)
    uint64_t    lastSwingAt = 0;
    uint8_t     sit = 0;
    bool        hitBySelf = false;  // we swung at it (S2C 0x1F with us as the attacker)
    uint64_t    selfHitAt = 0;      // FILETIME of our last swing at it
    uint64_t    seenAt = 0;
};

struct GroundItem {
    uint16_t oid = 0;
    int      x = 0, y = 0;
    uint8_t  kind = 0;
    uint32_t itemId = 0;
    uint64_t uid = 0;
    uint16_t count = 0;
    uint16_t eventFlag = 0;
    uint64_t seenAt = 0;
    // A monster drop (S2C 0x000C / 0x0269): the monster, and its loot owner then (S2C 0x016D). The
    // server keeps a drop for that player for its first 30 s and refuses anyone else silently.
    uint16_t dropper = 0;
    uint16_t owner = 0;
    bool     fromDrop = false;      // false = already on the ground when we came (age / owner unknown)
};

struct InvItem {
    uint64_t uid = 0;
    uint32_t itemId = 0;
    uint16_t count = 0;
    uint8_t  kind = 0;
    int      equipSlot = -1;        // 0..22 when worn
    // a rare item's options (the record's rare extra {u32 type, u32 id | value << 16, u32 extra}): id ->
    // value; 10000 + stat = that stat's bonus when worn (in place of its ItemParam Plus)
    std::vector<std::pair<uint16_t, int16_t>> opts;
};

struct SkillEntry {
    uint16_t id = 0;
    uint8_t  level = 0;
};

// An effect state (ES: a buff, a debuff, an aura) on us. S2C 224 with us as the target starts it,
// S2C 280 [us][esId] ends it, S2C 850 [ESAction] warns that 10 s or less are left (es.cpp). A zone
// change re-makes every carried ES with a new id and no 224 (es_restore_carry), so their ids are
// unknown afterwards (esId 0) and they are matched by time.
struct Effect {
    uint16_t esId = 0;
    uint16_t skill = 0;
    uint16_t esAction = 0;
    uint16_t caster = 0;
    uint8_t  esClass = 0;
    uint64_t startAt = 0;
    uint64_t endAt = 0;             // 0 = lifetime unknown (no Seconds for it)
    bool     ending = false;        // S2C 850 came
};

// The answers to our skill requests (skill.cpp): 240 = it fired (the cool time starts), 239 = refused
// (12 = cool time), 243 = cancelled (a chase that gave up), 242 = our cast started.
struct SkillAnswers {
    std::map<uint16_t, uint64_t> firedAt;   // skill -> FILETIME of its last S2C 240
    uint64_t fired = 0;                     // how many 240 came
    uint64_t refusedAt = 0;
    uint16_t refusedWhy = 0;
    uint64_t cancelAt = 0;
    uint16_t castSkill = 0;
    uint64_t castAt = 0;
};

// NPC dialogs, shops and the warehouse: the answers the bot's town trips wait for (ProjectPAULA
// npc_talk.cpp, shop.cpp, warehouse.cpp). FILETIMEs of the last answer of each kind.
struct NpcState {
    uint16_t    talkNpc = 0;            // S2C 112: whose dialog is open (0 = none)
    uint64_t    talkAt = 0;             // 112 / 115 came
    std::string text;                   // its last message (UTF-8): the text, then its options
    bool        first = false;          // it is the dialog's first message (S2C 112: its number is not on the wire)
    int         msgNo = -1;             // S2C 115: its number (-1 = a 112's)
    uint16_t    msgQid = 0;             // S2C 115: the quest of the function's slot
    int         errMsg = -1;            // S2C 116 [npc][index]: the message it could not send (-1 = an empty 116)
    uint64_t    talkRefusedAt = 0;      // S2C 113: too far / no such NPC
    uint64_t    talkErrorAt = 0;        // S2C 116: a script refused (e.g. a teleport's level / money)
    uint64_t    talkEndAt = 0;          // S2C 117: the dialog ended
    uint16_t    shop = 0;               // S2C 0xA8: the shop whose window opened
    uint64_t    shopAt = 0;
    std::map<uint32_t, uint32_t> prices;   // S2C 0x47 of `pricesFor`: item -> price
    uint16_t    pricesFor = 0;
    uint64_t    pricesAt = 0;
    uint64_t    boughtAt = 0;           // S2C 0x49
    uint64_t    buyFailAt = 0;          // S2C 0x4A (money / lines) / 0x4B (weight)
    uint16_t    buyFail = 0;
    uint64_t    soldAt = 0;             // S2C 0x4E
    uint64_t    sellFailAt = 0;         // S2C 0x4D / 0x355
    uint64_t    whOpenAt = 0;           // S2C 137: a warehouse tab came (the window is open)
    std::map<uint8_t, int> whItems;     // tab -> items in it (137 / 140)
    uint64_t    whAddAt = 0;            // S2C 140
    uint64_t    whFailAt = 0;           // S2C 141 [tab]
    uint8_t     whFailTab = 0;
    // an exchange shop (exchange_shop.cpp): 249 its list (open), 255 done (bag records), 191 [-1] / 256 refused, 258 closed
    uint64_t    exListAt = 0, exDoneAt = 0, exFailAt = 0, exCloseAt = 0;
    std::vector<uint16_t> exIds;
    // a compounder (compound.cpp): 188 its list (open), 228 the rate of the roll, 190 made (bag records), 191 refused,
    // 193 closed
    uint64_t    cmpListAt = 0, cmpRateAt = 0, cmpDoneAt = 0, cmpFailAt = 0, cmpCloseAt = 0;
    std::vector<uint16_t> cmpIds;
    int         cmpRate = 0, cmpMade = 0, cmpFailed = 0, cmpFailCode = 0;
};

// Our monster quest (ProjectPAULA monster_quest.cpp; the wire is the client's, sub_6F0960): one at a time.
// 210 taken [qid][monster][grade][run][kills][record][Name][Client][Pos][Comment], 215 the timer every second
// [remain ms][total ms], 217 [kills][target], 218 the kills done [qid][u8 has a return map], 216 the time ran
// out [qid] (done: it stays to be handed in; else 211 follows), 219 handed in, 211 the entry gone [qid].
// 235 (login, map change) [n]{qid, monster, run, grade, kills, record}: only a done quest survives a relog
// (its char_quest row is written when the kills are done); a map change carries a running one.
struct MonQuestState {
    bool        has = false;            // an entry (210 / 235) until 211
    uint16_t    qid = 0, monster = 0;   // the grade's monster
    uint8_t     grade = 0, run = 0;     // run = the how-manyth time
    int         kills = 0, target = 0;
    bool        success = false;        // 218, or a 235 entry with a record
    bool        portal = false;         // 218: it has a ReturnMapID (the client offers the portal back)
    uint32_t    remainMs = 0, totalMs = 0;
    uint64_t    timerAt = 0;            // the last 215 (0 = stopped: 216)
    uint64_t    takenAt = 0, successAt = 0, timeoutAt = 0, removedAt = 0, doneAt = 0;
    uint16_t    removedQid = 0;
    uint64_t    returnOkAt = 0, returnNoAt = 0;   // S2C 0x411 / 0x412
    int         completed = 0;          // 219 since the session started
    int64_t     lastExp = 0, lastTm = 0, lastGelder = 0;
    std::string name, client, pos;
    // the timer is running (a 215 in the last 2.5 s): the portal back works only then
    bool running(uint64_t now) const { return has && timerAt && !(now > timerAt && now - timerAt > 25000000ull); }
};

// Our normal quests (ProjectPAULA quest.cpp; its wire VERIFIED in the client there): S2C 234 at login [u16 n]{u32 qid,
// u16 run} = the ones on; 132 taken [u32 qid][u16 run][cstr Name][Client][Pos][Comment][u16 qid]; 133 its entry gone
// [u32 qid]; 150 handed in [u16 n]{bag record}[u16 m]{u32 item, u16 count}[u32 EXP][u32 TM EXP][u16 qid][u16 run]
// [u32 gelder][u16 skill][u16 level]. A run is the history + 1. What the packets tell of each quest's history is kept
// for the session (monster quests too: 210 / 235 the run, 219 handed in).
struct QuestHist { int lo = 0, hi = -1; };      // done at least lo times, at most hi (-1 = not known)
struct QuestLog {
    std::map<int, int> active;          // qid -> the run on
    bool        listed = false;         // a 234 came (the list of the ones on is known)
    uint64_t    takenAt = 0, doneAt = 0, removedAt = 0;
    int         takenQid = 0, takenRun = 0, doneQid = 0, doneRun = 0, removedQid = 0;
    int64_t     lastExp = 0, lastTm = 0, lastGelder = 0;
    int         completed = 0;          // 150 since the session started
    std::map<int, QuestHist> hist, monHist;
    std::map<int, std::string> names;   // 132's Name
    // scenario quests (the Story; quest.cpp squest_*, VERIFIED in the client there): S2C 833 at login [u16 n]{u32 qid,
    // u16 run} = on; 829 taken [u32 qid][u16 run][Name][Client][Pos][Comment]; 831 handed in [u16 n]{record}[u16 m]
    // {u32 item, u16}[u32 EXP][u32 TM EXP][u32 qid][u16 the row's Count][u32 gelder]; 830 its entry gone [u32 qid]
    std::map<int, int> sactive;
    bool        slisted = false;
    uint64_t    stakenAt = 0, sdoneAt = 0;
    int         stakenQid = 0, stakenRun = 0, sdoneQid = 0;
    std::map<int, QuestHist> shist;
    // an NPC's Give_Item / Take_Item (npc_talk.cpp): S2C 0x024A [u16 n]{bag record}
    uint64_t    npcItemsAt = 0;
};

// Drilling (ProjectPAULA drill.cpp; client: the avatar's state 5, sub_6B4240 / sub_6B3110, the gauge Driller
// sub_562A90). Our dig as the packets tell it: C2S 0x2C [x][y][type] asks (the server walks us there first
// when the point is in another cell) -> S2C 0x2D (the client enters its state 5) or 0x2E (refused: no drill)
// -> S2C 371 when the walk ends: the cell's map depth, ITS ITEM'S DEPTH (0 = none — sent whatever the sense
// roll says; the client's gauge stops there by itself), the ground nature, the CRC seed | the sense bit and the
// Crazy Drill gauge % -> C2S 0x31 [cnt][ms][depth][crc] (dug) or 0x30 [cnt][ms] (cancelled; the client sends it
// by itself when the sense roll shows an empty cell) -> S2C 0x32 [reason][life][item] ends it. While the Crazy
// Drill Mode runs there is no 371: the dig is instant (0x32 reason 8 / 9 / 4).
struct DrillSession {
    enum Phase : uint8_t { Idle, Asked, Started, Info, Ending };
    uint8_t  phase = Idle;
    int      x = 0, y = 0;              // the point asked (C2S 0x2C); the dig's cell
    uint64_t askedAt = 0, startedAt = 0, infoAt = 0, endAskedAt = 0;
    int      mapDepth = 0, itemDepth = 0;   // 371, in units; itemDepth 0 = no item in the cell
    uint8_t  nature = 0;
    uint16_t seed = 0;                  // as 371 sent it: the seed | the sense bit
    bool     sensed = false;
    bool     premium = false;           // 371's form byte was set (an auto-drill form, not modelled)
    uint16_t endOp = 0;                 // C2S 0x30 / 0x31 seen for this dig (ours or the client's own)
};
// The end of a dig (S2C 0x32): the client's reasons 1 cancelled, 2 / 9 nothing, 3 dug out, 4 too deep for the
// drill, 6 no life left, 7 not diggable ground, 8 / 19 a Crazy Drill find, 10 interrupted by a hit.
struct DrillResult {
    uint8_t  reason = 0;
    uint16_t life = 0;
    uint32_t item = 0;
    uint64_t at = 0;
    int      x = 0, y = 0;
    bool     info = false;              // its S2C 371 came (false: it ended before the dig began)
    int      itemDepth = 0;
};
struct DrillStats {
    int digs = 0, finds = 0, nothing = 0, tooDeep = 0, interrupted = 0, cancelled = 0;
    int lifeUsed = 0;
    std::deque<std::pair<uint64_t, uint32_t>> found;   // FILETIME, item id (newest last)
    std::map<uint32_t, int> foundCount;                // item id -> how many dug up (the BigBang's too)
};

// What we know of a map's buried items, per 16 x 16 cell: our own digs' 371 (the exact depth), the others'
// S2C 47 (the same info for their cell), the ends (a dug-out cell is empty: the server lays the new item
// elsewhere) and the detectors (a found point holds an item no deeper than the reach; the rest of the scanned
// square holds none that shallow).
struct BuriedCell {
    enum State : uint8_t { Unknown, Empty, Item, Detected, Undiggable, Claimed };
    uint8_t  state = Unknown;
    int16_t  depth = 0;                 // Item: its depth; Detected: the detector's reach
    int16_t  noneTo = -1;               // a detector saw no item down to this depth (-1 = none)
    uint64_t at = 0;
};

struct SelfState {
    uint16_t    eid = 0;
    uint64_t    charUid = 0;
    std::string name;
    uint16_t    species = 0;
    uint8_t     hair = 0, fType = 0;
    uint8_t     job = 0;                // the class (JobTypeInfo id 1..33): our appear's job byte / S2C 570
    int         level = 0, tmLevel = 0, tmPoints = -1;
    int64_t     hp = -1, mp = -1;
    // The max HP / MP is not on the wire: the client and the server work it out (ProjectPAULA recompute_stats):
    // gulim13[points / 4 + 1] + what the worn items add (a rare option 10000 + stat, else the item's Plus) +
    // the buffs. hpMaxCalc = that without the buffs (0 = no stat table); hpMax = the larger of it and the
    // highest HP seen since the login (a buff may raise it; a formula that misses something never lowers it).
    int64_t     hpMax = 0, mpMax = 0;
    int64_t     hpMaxCalc = 0, mpMaxCalc = 0;
    int64_t     hpMaxSeen = 0, mpMaxSeen = 0;
    int64_t     exp = 0, tmExp = 0, money = 0;
    int         zone = -1, homeZone = -1;
    int         x = 0, y = 0, dx = 0, dy = 0;
    bool        moving = false;
    uint64_t    moveAt = 0;             // FILETIME the segment (x,y) -> (dx,dy) started
    int         speed = 160;            // units/s: S2C 0x01D8 (ProjectPAULA run 160, walk 80)
    bool        dead = false;
    uint64_t    deadAt = 0;             // FILETIME of the last death
    int         deadZone = -1;          // where it happened
    uint8_t     sit = 0;
    int         whSlots = 80;           // the warehouse's slots (0xE9 state block +57): tabs of 80, ids 2, 21..29
    uint16_t    statPoints[13] = {};    // ap ac dx max_mp ma md max_wt da lk max_hp dp hv attr
    bool        statsKnown = false;     // a stat block came
    uint64_t    equip[23] = {};         // slot 1 = weapon, slot 20 = the loaded bullets (common slot)
    uint16_t    gunSeed = 0;            // the last S2C 0x0200: the next shot's CRC seed
    uint64_t    gunSeeds = 0;           // how many seeds arrived (a new one after each accepted shot)
    uint64_t    lastShotAt = 0;         // FILETIME of our last shot (S2C 0x01E5 with us as the shooter)
    uint64_t    weaponAt = 0;           // FILETIME the weapon slot changed / we logged in (the server's
                                        // shot timer starts then too: gun.cpp WeaponItemHelper)
    uint64_t    attackCancelAt = 0;     // FILETIME of the last S2C 0x0136 (attack request failed)
    // Where the client DRAWS us (hook EV_AVATAR; x / y above are the server's view). The client path-finds each
    // move the server orders; ProjectPAULA walks straight through walls, so a move with no way round leaves the
    // avatar standing while the server has us elsewhere.
    struct Drawn {
        uint32_t actor = 0;             // 0 = no avatar (not in a map) / no feed
        int      x = 0, y = 0, dx = 0, dy = 0;
        bool     moving = false;        // its move type bits (& 7) are set
        uint64_t at = 0;                // FILETIME of the last read
        uint64_t movedAt = 0;           // when x / y last changed (or the avatar appeared)
        uint64_t reads = 0;
    } drawn;
    // since the session started
    int64_t     expGained = 0, tmGained = 0;
    int         kills = 0, deaths = 0;
};

class WorldModel {
public:
    explicit WorldModel(const GameData* gd) : gd_(gd) {}

    void feed(const CapPacket& p);
    void feed_avatar(const EvAvatar& a);    // hook EV_AVATAR -> self.drawn
    // The drawn avatar is being read (an EV_AVATAR in the last 2.5 s with an avatar in it).
    bool drawn_live(uint64_t now) const;

    SelfState                      self;
    std::map<uint16_t, Entity>     entities;
    std::map<uint16_t, GroundItem> groundItems;
    std::vector<InvItem>           inventory;     // bag order
    std::vector<SkillEntry>        skills;
    std::vector<Effect>            effects;       // on us
    SkillAnswers                   skillAnswers;
    NpcState                       npc;
    MonQuestState                  monq;
    QuestLog                       quests;
    bool                           inWorld = false;
    // A map change: S2C 153 (portal / teleport), 0x17 (go to town) or 154 (the relocate) start it, the
    // new link's S2C 437 / 157 (or a login's 0xE6) end it. The client reconnects in between: nothing
    // may be sent then (ProjectPAULA begin_field_change / handle_enter_zone_cmd).
    bool                           changingZone = false;
    uint64_t                       zoneChangeAt = 0;   // when the last one started
    uint64_t                       arrivedAt = 0;      // when we last entered a map
    uint64_t                       overweightAt = 0;   // S2C 0x23: a pick-up refused for the weight

    // drilling
    DrillSession                   drill;
    DrillResult                    drillLast;
    DrillStats                     drillStats;
    uint64_t                       drillRefusedAt = 0;   // S2C 0x2E
    uint64_t                       cdmUntil = 0;         // the Crazy Drill Mode runs until (0 = off)
    int                            cdmGauge = -1;        // its gauge % (371), -1 = not seen
    uint64_t                       detectAt = 0;         // the last detector answer (S2C 224 [1147 / 1170])
    int                            detectFound = -1;     // its points (-1 = the roll failed)
    std::map<int, std::map<uint32_t, BuriedCell>> buried;   // zone -> cell key -> what we know

    std::deque<std::string>        events;        // what happened, newest last
    uint64_t                       applied = 0;   // packets the model understood
    uint64_t                       malformed = 0; // understood opcodes whose body did not fit
    std::string                    lastMalformed;

    const InvItem* find_item(uint64_t uid) const;
    // How many of that item the bag holds (every stack, the loaded one included; a rare item counts one —
    // a drill's count on the wire is its life).
    int            count_item(uint32_t itemId) const;
    // the bag's weight in % of the max weight (MaxWT, as the server counts it); -1 = the max is not known
    int            weight_pct() const;
    // The bag's weight: ItemParam Weight x count of every item (equipped ones too, as the server counts).
    int64_t        bag_weight() const;
    // Where we are at `now` (FILETIME): the move segment interpolated at our speed, else the rest point.
    void           self_pos_at(uint64_t now, int& x, int& y) const;
    std::string    entity_label(uint16_t eid) const;
    // Our learned level of a skill (0 = not learned).
    int            skill_level(uint16_t skill) const;
    // When the skill can fire again (FILETIME): its last S2C 240 + its cool time at our level
    // (CoolTime - 0.5 s, the server's timer) + a little slack; 0 = never fired, ready.
    uint64_t       skill_ready_at(uint16_t skill) const;
    // An effect still up at `now` (its time not over, with 1.5 s of slack for the 280 to come).
    static bool    effect_up(const Effect& e, uint64_t now);
    // The effect of that skill on us that lasts the longest (null = none up).
    const Effect*  effect_of(uint16_t skill, uint64_t now) const;
    // The Third Eye (Player +1628): an ESAThirdEye (ESAction 1390, Mind's Eye 8001) up on us. Without
    // it the server ignores our attacks on shadow monsters (16000..16999) and refuses their drops
    // (an item with the event flag) — ProjectPAULA es.cpp third_eye_on/off, inventory.cpp.
    bool           third_eye(uint64_t now) const;
    // A stat's value without the buffs: gulim13[points / 4 + 1] + what the worn items add (0 = unknown).
    int            stat_base(int stat) const;
    // The cells we know of in our map (keys: cell_key)
    static uint32_t cell_key(int cx, int cy) { return uint32_t(uint16_t(cx)) << 16 | uint16_t(cy); }
    const std::map<uint32_t, BuriedCell>* buried_here() const;
    const BuriedCell* buried_at(int cx, int cy) const;
    bool           cdm_on(uint64_t now) const { return cdmUntil > now; }
    // A dig is open (asked and not ended). A phase that waited far longer than the server ever takes counts
    // as over (a lost S2C 0x32): 4 s asked / ending, 25 s walking there, 200 s digging.
    bool           drill_open(uint64_t now) const;
    const GameData* data() const { return gd_; }

    struct Reader;          // little-endian payload cursor (world.cpp)

private:
    void event(uint64_t seq, const std::string& text);
    void clear_area();
    void set_self_hp(int64_t hp);
    void set_self_mp(int64_t mp);
    void recompute_max();           // hpMaxCalc / mpMaxCalc from the points + the worn items, then hpMax / mpMax
    void apply_stat_block(const uint8_t* b);
    uint8_t apply_bag_record(Reader& r, uint64_t seq, bool toBag = true);
    void arrived(uint64_t now);
    Entity* ent(uint16_t eid);
    void remove_id(uint16_t id, uint64_t seq, const char* why);
    void effects_lose_ids();
    BuriedCell& cell_of(int x, int y);  // our zone's cell holding (x, y) units
    void drill_stop(uint8_t reason, uint16_t life, uint32_t item, uint64_t now, uint64_t seq);
    void detector_result(uint16_t skill, uint16_t esAction, Reader& r, uint64_t now, uint64_t seq);

    struct OtherDig { uint32_t key = 0; int16_t depth = 0; };
    std::map<uint16_t, OtherDig> othersDig_;   // another player's open dig (S2C 47) until its S2C 51
    uint64_t detPointAt_ = 0;                   // our last Item Detector request's point (C2S 0xE0)
    int      detPointX_ = 0, detPointY_ = 0;

    const GameData* gd_;
};

} // namespace nj
