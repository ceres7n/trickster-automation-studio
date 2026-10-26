#pragma once
// Static game data from ProjectPAULA/db (the server's tables): names and the numbers the
// world model needs (a monster's max HP, an item's type / weight).
#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nj {

class LibConfig;
struct YRecord;

struct ItemInfo {
    std::string name;
    int cls = 0;                    // Class: 1 common (stacks), 2 rare, 3 (drills; the server reads it as 2)
    int type = 0, subType = 0, weight = 0, effect = 0;
    int options = 0, minLevel = 0, hp = 0, mp = 0;   // Options & 2 = usable (C2S 0x57)
    int ncash = 0;                  // the cash shop's price (> 0 = a cash item)
    int value = 0;                  // ItemParam Value: a shop's price = (int)(map ShopPriceRate x Value x rate + 0.5)
    int16_t plus[12] = {};          // APPlus..HVPlus: what it adds to each stat when worn (StatIdx order)
};

// The common gear grades that monsters drop and maps bury by the hundred (heavy, rarely worth carrying): a weapon /
// helmet / shield (ItemParam Class 2, Type 2 / 3 / 8) whose name starts with Old, Strong (a sword's: Sharp), Art or
// Ult. — the item tables keep each kind in a row of eight: Old, plain, Strong / Sharp, Edge / Focus / Aegis ..., Morph,
// Art, Ult., Chaos. Chaos, Morph, the others and the named series (Requiem, Shadow, Ultimate Amadas ...) are not these.
enum EquipTier { ET_None = -1, ET_Old = 0, ET_Strong, ET_Art, ET_Ult, ET_Count };

// The stats in the engine's order (ProjectPAULA stat_table.hpp StatIdx; the 33-byte stat block's points).
enum StatIdx { ST_AP = 0, ST_AC, ST_DX, ST_MAXMP, ST_MA, ST_MD, ST_MAXWT, ST_DA, ST_LK, ST_MAXHP, ST_DP, ST_HV, ST_COUNT };

struct MobInfo {
    std::string name;
    int level = 0, maxHp = 0, maxMp = 0, exp = 0, tm = 0;
    int itemDropIdx = 0, itemDropCnt = 0;   // its MonItemDropInfo row / the rolls per kill (drop.cpp)
};

// One level of a skill: its ESAII row (the invocation conditions, esa_db.yml) and its ESAction row's
// lifetime. ProjectPAULA skill.cpp: the cast waits Delay, fires (S2C 240) and starts the cool time
// then: CoolTime - 0.5 s (sub_88F740); an ES lives 1000 * Seconds ms (es.cpp).
struct SkillLevelInfo {
    float delay = 0.0f;             // cast time (s)
    float coolTime = 0.0f;          // s
    int   mpCon = 0, hpCon = 0;
    int   range = 0;                // 0 = none in the row
    float seconds = 0.0f;           // the effect's lifetime (0 = none / not a timed effect)
    // the ESAction row's own columns the bot reads (0 = not in the row): the detectors' Radius, Depth
    // (Basic Detection's reach), DepthRatio (Item Detector: reach = DA * DepthRatio / 10) and ApplyRatio
    float radius = 0.0f, depth = 0.0f, depthRatio = 0.0f, applyRatio = 0.0f;
};

// A drill (Item_DrillParam, drill_db.yml; ProjectPAULA drill.cpp): an ItemParam Type 6 item, worn in slot 7,
// whose count is its life. The client's dig gauge (Driller sub_562A90) fills a bar at BarSpeed / 600 per ms up
// to its effective area (200 - BarEffArea) and digs ScoopMass deeper every 120 ms there; DigDepth is the
// deepest it can report. Another ground nature than GroundNature costs twice the life.
struct DrillInfo {
    int grade = 0, groundNature = 0, digDepth = 0;
    int barEffArea = 0, barSpeed = 0, scoopMass = 0, drillLife = 0;
};

// A map's buried items (MapInfoEx, maps_db.yml): ItemPercentage % of its diggable cells hold one, at a depth
// of 10 * [Min / 10, Max / 10 + 1); MaxItemDepth (raised to the min) is the map's depth (drill.cpp).
struct MapDigInfo {
    float itemPct = 0.0f;
    int   minDepth = 0, maxDepth = 0;
    int   type = 0, option = 0;     // Option & 512: no Crazy Drill Mode here
};

struct SkillInfo {
    std::string name;
    int type = 0, maxLevel = 0;     // Type 1 passive, 2 active, 3 an effect state (a buff, a debuff, a field)
    int esaic = 0, esaui = 0;       // invocation condition (2 = Chase) / selection UI kind (C2S 0x00E0 uiKind)
    int skillOption = 0;            // & 0x10 = never usable through C2S 0x00EE
    int affectionRel = 0;           // whom it works on: 1 self, 16 party, 32 enemies, 64 the partner
    int esClass = -1, esAction = 0;
    std::vector<SkillLevelInfo> levels;   // [lv - 1]
    // A buff: an effect state on us / our side. Type 3 on enemies (AffectionRel 32) is the attack's: a debuff (Armor
    // Breaker, Mana Web) or a field on the ground (Scorching Earth, Deadly Fen).
    bool buff() const { return type == 3 && !(affectionRel & 32); }
};

struct GunInfo {                    // Item_GunParam2 (gun_db.yml): the shot's CRC key and timing
    int gaugeSpeed = 0, powRange = 0, powStart = 0, minRange = 0, maxRange = 0;
};

// A monster quest (monster_quest_db.yml = MonsterQuestInfo; ProjectPAULA quest_db.hpp MonsterQuestDef): the
// grade whose MinLv..MaxLv holds our level says what to kill — Cnt of SrcMonster in Time minutes.
struct MonQuestGrade {
    int monster = 0, cnt = 0, timeMin = 0, minLv = 0, maxLv = 0, exp = 0, tmxp = 0, mapId = 0;
};
struct QuestNeed { uint32_t item = 0; int cnt = 0; };
struct QuestResultRow {
    int count = 0, type = 0;
    std::vector<QuestNeed> items;               // Item0..3 / Cnt0..3
    int gelder = 0, exp = 0, tmxp = 0;
};
struct MonQuestInfo {
    int id = 0;
    std::string name, client, pos, comment;     // client = the NPC's name, pos = where
    int minLevel = 0, maxLevel = 0;             // both 0 = any level
    std::vector<MonQuestGrade> grades;          // SrcMonster0.. while set (read from column 0)
    int maxCount = 25;                          // MaxCount (<= 0 -> 25; 99 and more: no limit)
    int returnMap = 0;                          // ReturnMapID: the success portal's map (0 = none)
    // its MonsterQuestResult table's rows in order (monster_quest_result_db.yml): a grade's reward is the row of its index
    // (the last one past them: quest_db.hpp result_of); a row with Type 1 gives one of its items at random
    std::vector<QuestResultRow> results;
    std::string resultTable;
    int grade_for(int level) const              // sub_7EB790 (-1 = none)
    {
        for (size_t i = 0; i < grades.size(); ++i)
            if (grades[i].minLv <= level && level <= grades[i].maxLv) return int(i);
        return -1;
    }
    bool level_ok(int level) const { return (minLevel <= 0 && maxLevel <= 0) || (level >= minLevel && level <= maxLevel); }
};

// A normal quest (quest_db.yml = QuestInfo, its QuestResult table in quest_result_db.yml; ProjectPAULA quest.cpp):
// SrcItem0..5 / Cnt are brought to the NPC (the server checks one bag stack of each holding its count), the run is
// the history + 1, the result row of a run = the one with the largest Count up to it (the first when none); a Type 1
// row gives ONE of its items at random.
struct QuestInfo {
    int id = 0;
    std::string name, client, pos, comment;
    int minLevel = 0, maxLevel = 0;             // both 0 = any level
    int exp = 0, tmxp = 0;
    std::vector<QuestNeed> src;                 // SrcItem0..5 while set
    int srcSkill = 0, srcSkillLv = 0, srcGelder = 0;
    int maxCount = 25;                          // MaxCount (<= 0 -> 25; 99 and more: no limit)
    int options = 0;                            // 2 = the reward tier follows how many of SrcItem0 are brought
    std::vector<QuestResultRow> results;        // its table's rows, in order
    bool level_ok(int level) const { return (minLevel <= 0 && maxLevel <= 0) || (level >= minLevel && level <= maxLevel); }
    const QuestResultRow* result_for(int run) const   // sub_6D8680
    {
        const QuestResultRow* best = nullptr;
        for (const QuestResultRow& r : results)
            if (r.count <= run && (!best || r.count > best->count)) best = &r;
        return best ? best : results.empty() ? nullptr : &results.front();
    }
};

// NpcCheckLvMsg (npc_check_lv_msg_db.yml): an NPC's PreCheck 5 — the first range holding our level picks its message.
struct NpcLvMsg { int min[6] = {}, max[6] = {}, msg[6] = {}; int reject = 0; };

// Where a monster lives: a map whose MonsterTable (maps_db) holds it (mon_spawn_db), and how many the
// regeneration keeps there (each row's Avr, else MonsterAvrNum * Ratio / the table's sum of (int)Ratio).
struct MobSpawn { int zone = 0; int count = 0; std::vector<int> regions; };   // regions: its rows' Region (0 = type-3 points)

// A map's .md3 layer 1 (ProjectPAULA map_attr.hpp): one byte per 16x16-unit cell, 1 = blocked. Layer 3 is
// the ground, whose MapAttr Attr (map_attr_db.yml) is the cell's ground nature: 6 cannot be dug.
struct MapGrid {
    int w = 0, h = 0;                   // cells
    std::vector<uint8_t> cell;
    std::vector<uint8_t> nature;        // per cell (0 when the map has no ground layer)
    int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;   // the map's bounds in units (.md3 header [6..9])
    std::string file;

    bool inside(int cx, int cy) const { return cx >= 0 && cy >= 0 && cx < w && cy < h; }
    uint8_t raw(int cx, int cy) const { return inside(cx, cy) ? cell[size_t(cy) * size_t(w) + size_t(cx)] : 1; }
    uint8_t nature_at(int cx, int cy) const
    {
        return inside(cx, cy) && !nature.empty() ? nature[size_t(cy) * size_t(w) + size_t(cx)] : 0;
    }
    // drill.cpp diggable_cell: not blocked and not a nature-6 ground (the buried items lie only there)
    bool diggable(int cx, int cy) const { return inside(cx, cy) && raw(cx, cy) != 1 && nature_at(cx, cy) != 6; }
    bool blocked(int x, int y) const;   // units; outside the grid = blocked
    // Every cell the straight segment crosses (sampled each 8 units, the start excluded) is open.
    bool line_clear(int x0, int y0, int x1, int y1) const;
    bool open_cell(int cx, int cy) const { return cx >= 0 && cy >= 0 && cx < w && cy < h && cell[size_t(cy) * size_t(w) + size_t(cx)] != 1; }
    // The open areas: cells joined by 8-way steps (a diagonal only when both of its sides are open, as
    // the path finder walks), numbered from 1 on first use; 0 = blocked or outside. Units in.
    int  component(int x, int y) const;
    // The open cell nearest to (x, y) within `maxCells` rings — its centre in units; false = none.
    bool nearest_open(int x, int y, int maxCells, int& ox, int& oy) const;
    mutable std::vector<int32_t> comp_;     // component() labels (empty = not built yet)
};

class Atlas;
class ItemSources;
class QuestGraph;

class GameData {
public:
    // dir = ProjectPAULA\db (item_db.yml, mob_db.yml, skill_db.yml). Missing files are reported
    // in `err` but do not stop the others from loading.
    bool load(const std::string& dir, std::string& err);

    const ItemInfo*  item(uint32_t id) const;
    int              equip_tier(uint32_t id) const;           // EquipTier of an item (ET_None for anything else)
    static const char* equip_tier_name(int tier);           // "Old" / "Strong" / "Art" / "Ult."
    const MobInfo*   mob(uint32_t id) const;
    const SkillInfo* skill(uint32_t id) const;
    // The row of that level (clamped to the levels there are); null = the skill has no ESAII rows.
    const SkillLevelInfo* skill_level(uint32_t id, int lv) const;
    const GunInfo*   gun(uint32_t id) const;
    const DrillInfo* drill(uint32_t id) const;
    // DrillGradeExpRate: a dig's EXP = rate * depth * (Lv + 400) * 0.0005 (a Grade past the rows reads the last)
    double           drill_exp_rate(int grade) const;
    // the zone's buried items (null = the map has none: ItemPercentage 0)
    const MapDigInfo* map_dig(int zone) const;
    const MonQuestInfo* monster_quest(int id) const;
    const std::vector<MonQuestInfo>& monster_quests() const { return monQuests_; }   // by id
    const QuestInfo* quest(int id) const;
    const std::vector<QuestInfo>& quests() const { return quests_; }                 // by id
    // scenario_quest_db.yml (ScenarioQuestInfo, the Story): the same layout as QuestInfo
    const QuestInfo* scenario_quest(int id) const;
    const std::vector<QuestInfo>& scenario_quests() const { return squests_; }       // by id
    // PreCheck 5: the message for that level (-1 = no such table)
    int lv_msg(int table, int level) const;
    // the maps a monster is regenerated in, the most first
    std::vector<MobSpawn> spawns_of(uint32_t mob) const;
    // gulim13 (db/stat_table.fnt2): a stat's naked value at graph-level gl = its points / 4 + 1
    // (ProjectPAULA StatTable::value, clamped to the rows there are); 0 = no table.
    int              stat_value(int gl, int stat) const;
    bool             has_stat_table() const { return statRows_.size() > 1; }
    // The zone's map (maps_db.yml FileName in mapDir), read on first use; null when missing.
    const MapGrid*   map(int zone) const;
    std::string      map_name(int zone) const;
    // A map whose Type is 1, 5 or 6 (the towns, their shops, inns ...): a skill with SkillOption 0x200 (most buffs) is
    // refused there (ProjectPAULA skill.cpp: S2C 239 [15] + system message 15).
    bool             no_skill_zone(int zone) const;
    void set_map_dir(const std::string& dir) { mapDir_ = dir; }
    // The client's libconfig XML the data is read from (empty = <exe dir>\conf\paula_libconfig.xml); without that file the
    // ProjectPAULA db/*.yml files of the load dir are read instead.
    void set_libconfig(const std::string& path) { libPath_ = path; }
    // The server's data folder (map\*.md3, gulim13.fnt2); empty = conf\ next to the exe.
    void set_data_dir(const std::string& dir) { dataDir_ = dir; }
    const std::string& map_dir() const { return mapDir_; }
    // One db's records: from the libconfig (libconfig_db) when it is there, else <db dir>\<name>.yml.
    bool read_db(const std::string& name, std::vector<YRecord>& out, std::string& err) const;
    const std::string& source() const { return source_; }          // what the data was read from (the status line)
    // keep the parsed libconfig for the process (the selftest loads the data many times)
    static void keep_libconfig(bool on);
    // Every map's portals, NPCs, shops and warehouses + the routes between maps (atlas.h). Built on
    // first use from the same db dir and map dir (a few hundred ms: every .md3's regions / points).
    const Atlas&     atlas() const;
    // Where each item comes from: the monsters' drops per kill, the maps' buried items per find (item_sources.h).
    // Built on first use (the drop and dig tables, ~0.1 s).
    const ItemSources& item_sources() const;
    // The NPC dialogs as a graph, the quests' givers and what each one waits for, the Key Quests (quest_graph.h).
    // Built on first use from the atlas and the quest tables.
    const QuestGraph& quest_graph() const;
    const std::unordered_map<uint32_t, MobInfo>& mob_table() const { return mobs_; }
    std::string item_name(uint32_t id) const;
    std::string mob_name(uint32_t id) const;
    // ids whose name holds `needle` (case-insensitive) or whose id it is; sorted by id, at most `max`
    std::vector<uint32_t> search_items(const std::string& needle, size_t max) const;
    std::vector<uint32_t> search_mobs(const std::string& needle, size_t max) const;
    std::string skill_name(uint32_t id) const;

    size_t items() const { return items_.size(); }
    size_t mobs() const { return mobs_.size(); }
    size_t skills() const { return skills_.size(); }
    size_t maps() const { return mapRows_.size(); }

private:
    std::unordered_map<uint32_t, ItemInfo>  items_;
    std::unordered_map<uint32_t, MobInfo>   mobs_;
    std::unordered_map<uint32_t, SkillInfo> skills_;
    std::unordered_map<uint32_t, GunInfo>   guns_;
    std::unordered_map<uint32_t, DrillInfo> drills_;
    std::vector<double>                     drillExp_;      // by Grade
    std::vector<std::vector<int>>           statRows_;      // [graph-level][stat], row 0 unused
    std::vector<MonQuestInfo>               monQuests_;     // by id
    std::vector<QuestInfo>                  quests_;        // by id
    std::vector<QuestInfo>                  squests_;       // by id
    std::unordered_map<int, NpcLvMsg>       lvMsgs_;
    std::unordered_map<uint32_t, std::vector<MobSpawn>> spawns_;   // mob -> maps, the most first
    uint8_t                                 groundAttr_[17] = {};   // .md3 layer-3 byte -> ground nature
    struct MapRow { std::string name, file; MapDigInfo dig; std::string monsterTable; int monsterAvr = 0; };
    std::unordered_map<int, MapRow>          mapRows_;
    std::string                              mapDir_;
    std::string                              dbDir_;
    std::string                              libPath_, source_, dataDir_;
    std::shared_ptr<LibConfig>               lib_;     // while loading (then released unless kept)
    mutable std::unordered_map<int, std::unique_ptr<MapGrid>> maps_;   // null = tried, missing
    mutable std::shared_ptr<Atlas>           atlas_;
    mutable std::shared_ptr<ItemSources>     sources_;
    mutable std::shared_ptr<QuestGraph>      questGraph_;
};

} // namespace nj
