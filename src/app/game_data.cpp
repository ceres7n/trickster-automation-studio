#include "app/game_data.h"
#include "app/atlas.h"
#include "app/item_sources.h"
#include "app/libconfig.h"
#include "app/quest_graph.h"
#include "app/text_util.h"
#include "app/yml_lite.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <windows.h>

namespace nj {

namespace {
bool g_keepLib = false;
std::map<std::string, std::shared_ptr<LibConfig>> g_libs;   // (keep_libconfig)
}

void GameData::keep_libconfig(bool on)
{
    g_keepLib = on;
    if (!on) g_libs.clear();
}

bool GameData::read_db(const std::string& name, std::vector<YRecord>& out, std::string& err) const
{
    if (lib_ && lib_->loaded()) return libconfig_db(*lib_, name, out, err);
    return yml_read_body_file(dbDir_ + name + ".yml", out, err);
}

bool GameData::load(const std::string& dir, std::string& err)
{
    items_.clear();
    mobs_.clear();
    skills_.clear();
    std::string base = dir;
    if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '\\';
    dbDir_ = base;
    atlas_.reset();
    sources_.reset();
    err.clear();
    std::string e;
    std::vector<YRecord> recs;
    // the client's libconfig (conf\paula_libconfig.xml next to the exe): everything is read from it when it is there
    lib_.reset();
    std::string lp = libPath_;
    if (lp.empty()) {                                       // the first conf\*.xml next to the exe
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((exe_dir() + "conf\\*.xml").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { lp = exe_dir() + "conf\\" + fd.cFileName; FindClose(h); }
    }
    if (!libPath_.empty() && !file_exists(libPath_)) err += "the libconfig XML " + libPath_ + " is missing  ";
    source_ = base.empty() ? std::string("no data") : base + "*.yml";
    if (file_exists(lp)) {
        auto c = g_libs.find(lp);
        if (c != g_libs.end()) lib_ = c->second;
        else {
            auto lc = std::make_shared<LibConfig>();
            std::string le;
            if (lc->load(lp, le)) { lib_ = lc; if (g_keepLib) g_libs[lp] = lc; }
            else err += le + "  ";
        }
        if (lib_) source_ = lp;
    }

    if (read_db("item_db", recs, e)) {
        for (const YRecord& r : recs) {
            ItemInfo i;
            i.name    = r.str("Name");
            i.cls     = (int)r.num("Class");
            i.type    = (int)r.num("Type");
            i.subType = (int)r.num("SubType");
            i.weight  = (int)r.num("Weight");
            i.effect  = (int)r.num("Effect");
            i.options  = (int)r.num("Options");
            i.minLevel = (int)r.num("MinLevel");
            i.hp       = (int)r.num("HP");
            i.ncash    = (int)r.num("Ncash");
            i.value    = (int)r.num("Value");
            i.mp       = (int)r.num("MP");
            static const char* const kPlus[ST_COUNT] = { "APPlus", "ACPlus", "DXPlus", "MaxMPPlus", "MAPlus", "MDPlus",
                                                         "MaxWTPlus", "DAPlus", "LKPlus", "MaxHPPlus", "DPPlus", "HVPlus" };
            for (int k = 0; k < ST_COUNT; ++k) i.plus[k] = int16_t(r.num(kPlus[k]));
            items_[(uint32_t)r.num("ID")] = std::move(i);
        }
    } else err += e + "  ";

    recs.clear();
    if (read_db("mob_db", recs, e)) {
        for (const YRecord& r : recs) {
            MobInfo m;
            m.name  = r.str("Name");
            m.level = (int)r.num("Level");
            m.maxHp = (int)r.num("MaxHp");
            m.maxMp = (int)r.num("MaxMp");
            m.exp   = (int)r.num("Exp");
            m.tm    = (int)r.num("Tm");
            m.itemDropIdx = (int)r.num("ItemDropIdx");
            m.itemDropCnt = (int)r.num("ItemDropCnt");
            mobs_[(uint32_t)r.num("Id")] = std::move(m);
        }
    } else err += e + "  ";

    recs.clear();
    std::unordered_map<uint32_t, std::pair<std::string, std::string>> esaii;   // skill -> ESAIITable, ESATable
    if (read_db("skill_db", recs, e)) {
        for (const YRecord& r : recs) {
            SkillInfo s;
            s.name     = r.str("Name");
            s.type     = (int)r.num("Type");
            s.maxLevel = (int)r.num("MaxSKLV");
            s.esaic    = (int)r.num("ESAIC");
            s.esaui    = (int)r.num("ESAUI");
            s.skillOption = (int)r.num("SkillOption");
            s.affectionRel = (int)r.num("AffectionRel");
            s.esClass  = (int)r.num("ESClass", -1);
            s.esAction = (int)r.num("ESAction");
            esaii[(uint32_t)r.num("ID")] = { r.str("ESAIITable"), r.str("ESATable") };
            skills_[(uint32_t)r.num("ID")] = std::move(s);
        }
    } else err += e + "  ";

    // esa_db.yml: each skill's ESAII rows (Delay, CoolTime, MpCon, HpCon, Range) + its ESAction rows' Seconds
    recs.clear();
    if (!esaii.empty() && read_db("esa_db", recs, e)) {
        std::unordered_map<std::string, std::vector<const YRecord*>> tables;    // name -> rows
        for (const YRecord& r : recs) tables[r.str("Table")].push_back(&r);
        for (auto& kv : esaii) {
            SkillInfo& s = skills_[kv.first];
            auto ii = tables.find(kv.second.first);
            auto ea = tables.find(kv.second.second);
            int top = 0;
            if (ii != tables.end()) for (const YRecord* r : ii->second) top = std::max(top, (int)r->num("Level"));
            if (ea != tables.end()) for (const YRecord* r : ea->second) top = std::max(top, (int)r->num("Level"));
            if (top <= 0 || top > 100) continue;
            s.levels.assign(size_t(top), SkillLevelInfo{});
            if (ii != tables.end())
                for (const YRecord* r : ii->second) {
                    const int lv = (int)r->num("Level");
                    if (lv < 1 || lv > top) continue;
                    SkillLevelInfo& l = s.levels[size_t(lv - 1)];
                    l.delay    = (float)r->fnum("Delay");
                    l.coolTime = (float)r->fnum("CoolTime");
                    l.mpCon    = (int)r->num("MpCon");
                    l.hpCon    = (int)r->num("HpCon");
                    l.range    = (int)r->num("Range");
                }
            if (ea != tables.end())
                for (const YRecord* r : ea->second) {
                    const int lv = (int)r->num("Level");
                    if (lv < 1 || lv > top) continue;
                    SkillLevelInfo& l = s.levels[size_t(lv - 1)];
                    l.seconds    = (float)r->fnum("Seconds");
                    l.radius     = (float)r->fnum("Radius");
                    l.depth      = (float)r->fnum("Depth");
                    l.depthRatio = (float)r->fnum("DepthRatio");
                    l.applyRatio = (float)r->fnum("ApplyRatio");
                }
        }
    } else if (!esaii.empty()) err += e + "  ";

    recs.clear();
    guns_.clear();
    if (read_db("gun_db", recs, e)) {
        for (const YRecord& r : recs) {
            GunInfo g;
            g.gaugeSpeed = (int)r.num("GaugeSpeed");
            g.powRange   = (int)r.num("PowRange");
            g.powStart   = (int)r.num("PowStartPoint");
            g.minRange   = (int)r.num("MinAttackRange");
            g.maxRange   = (int)r.num("MaxAttackRange");
            guns_.emplace((uint32_t)r.num("ID"), g);    // a duplicated ID keeps its FIRST row (retail)
        }
    } else err += e + "  ";

    // drill_db.yml (Item_DrillParam) + drill_grade_exp_db.yml (DrillGradeExpRate, row i = Grade i)
    recs.clear();
    drills_.clear();
    if (read_db("drill_db", recs, e)) {
        for (const YRecord& r : recs) {
            DrillInfo d;
            d.grade        = (int)r.num("Grade");
            d.groundNature = (int)r.num("GroundNature");
            d.digDepth     = (int)r.num("DigDepth");
            d.barEffArea   = (int)r.num("BarEffArea");
            d.barSpeed     = (int)r.num("BarSpeed");
            d.scoopMass    = (int)r.num("ScoopMass");
            d.drillLife    = (int)r.num("DrillLife");
            drills_[(uint32_t)r.num("Id")] = d;
        }
    } else err += e + "  ";
    recs.clear();
    drillExp_.clear();
    if (read_db("drill_grade_exp_db", recs, e)) {
        for (const YRecord& r : recs) drillExp_.push_back(r.fnum("ExpRate"));
    } else err += e + "  ";

    recs.clear();
    mapRows_.clear();
    maps_.clear();
    if (read_db("maps_db", recs, e)) {
        for (const YRecord& r : recs) {
            MapRow m{ r.str("Name"), r.str("FileName"), MapDigInfo{} };
            m.dig.itemPct  = (float)r.fnum("ItemPercentage");
            m.dig.minDepth = (int)r.num("MinItemDepth");
            m.dig.maxDepth = std::max(m.dig.minDepth, (int)r.num("MaxItemDepth"));   // sub_623D20
            m.dig.type     = (int)r.num("Type");
            m.dig.option   = (int)r.num("Option");
            m.monsterTable = r.str("MonsterTable");
            m.monsterAvr   = (int)r.num("MonsterAvrNum");
            mapRows_[(int)r.num("ID")] = std::move(m);
        }
    } else err += e + "  ";
    // the maps: <data dir>\map, else conf\map, else <db dir>\..\data\map
    const std::string dd = !dataDir_.empty() ? (dataDir_.back() == '\\' || dataDir_.back() == '/' ? dataDir_ : dataDir_ + "\\")
                                             : exe_dir() + "conf\\";
    if (mapDir_.empty()) mapDir_ = !dataDir_.empty() || file_exists(dd + "map\\map_sq00.md3") ? dd + "map" : base + "..\\data\\map";
    // map_attr_db.yml (MapAttr): the .md3 layer-3 byte (Type 0..16) -> its ground nature (Attr)
    recs.clear();
    std::fill(std::begin(groundAttr_), std::end(groundAttr_), uint8_t(0));
    if (read_db("map_attr_db", recs, e)) {
        for (const YRecord& r : recs) {
            const long long t = r.num("Type", -1);
            if (t >= 0 && t < 17) groundAttr_[t] = uint8_t(r.num("Attr"));
        }
    } else err += e + "  ";

    // stat_table.fnt2 (gulim13): one line per graph-level, 12 "<gl> 4 <value>" triplets (StatIdx order)
    statRows_.assign(1, std::vector<int>(ST_COUNT, 0));
    {
        // gulim13.fnt2 of the data folder (the client's data\gulim13.fnt2), else conf\stat_table.fnt2, else the db dir's
        std::string st = base + "stat_table.fnt2";
        for (const std::string& c : { dd + "gulim13.fnt2", dd + "stat_table.fnt2", exe_dir() + "conf\\stat_table.fnt2" })
            if (file_exists(c)) { st = c; break; }
        std::ifstream in(st);
        std::string line;
        while (std::getline(in, line)) {
            std::vector<long> tok;
            const char* p = line.c_str();
            char* end = nullptr;
            for (long v = std::strtol(p, &end, 10); end != p; v = std::strtol(p, &end, 10)) {
                tok.push_back(v);
                p = end;
            }
            if (tok.size() < size_t(ST_COUNT) * 3) continue;
            std::vector<int> row(ST_COUNT);
            for (int k = 0; k < ST_COUNT; ++k) row[size_t(k)] = int(tok[size_t(k) * 3 + 2]);
            statRows_.push_back(std::move(row));
        }
        if (statRows_.size() <= 1) err += "stat_table.fnt2 missing  ";
    }

    // monster_quest_db.yml (MonsterQuestInfo): the grades while SrcMonsterN is set, from column 0 on
    monQuests_.clear();
    recs.clear();
    if (read_db("monster_quest_db", recs, e)) {
        for (const YRecord& r : recs) {
            MonQuestInfo q;
            q.id = (int)r.num("Id");
            q.name = r.str("Name");
            q.client = r.str("Client");
            q.pos = r.str("Pos");
            q.comment = r.str("Comment");
            q.minLevel = (int)r.num("MinLevel");
            q.maxLevel = (int)r.num("MaxLevel");
            for (int i = 0; i < 5; ++i) {
                MonQuestGrade g;
                g.monster = (int)r.num(fmt("SrcMonster%d", i).c_str());
                if (!g.monster) break;
                g.cnt = (int)r.num(fmt("Cnt%d", i).c_str());
                g.timeMin = (int)r.num(fmt("Time%d", i).c_str());
                g.minLv = (int)r.num(fmt("MinLv%d", i).c_str());
                g.maxLv = (int)r.num(fmt("MaxLv%d", i).c_str());
                g.exp = (int)r.num(fmt("Exp%d", i).c_str());
                g.tmxp = (int)r.num(fmt("Tmxp%d", i).c_str());
                g.mapId = (int)r.num(fmt("MapId%d", i).c_str());
                q.grades.push_back(g);
            }
            const long long mc = r.num("MaxCount");
            q.maxCount = mc <= 0 ? 25 : int(std::min<long long>(mc, 255));
            q.returnMap = (int)r.num("ReturnMapID");
            q.resultTable = r.str("ResultTable");
            if (q.id) monQuests_.push_back(std::move(q));
        }
        std::sort(monQuests_.begin(), monQuests_.end(), [](const MonQuestInfo& a, const MonQuestInfo& b) { return a.id < b.id; });
        // monster_quest_result_db.yml: each table's rows in order
        std::unordered_map<std::string, std::vector<QuestResultRow>> mt;
        recs.clear();
        if (read_db("monster_quest_result_db", recs, e)) {
            for (const YRecord& r : recs) {
                QuestResultRow row;
                row.type = (int)r.num("Type");
                for (int i = 0; i < 4; ++i) {
                    const uint32_t it = (uint32_t)r.num(fmt("Item%d", i).c_str());
                    if (it) row.items.push_back(QuestNeed{ it, (int)r.num(fmt("Cnt%d", i).c_str()) });
                }
                mt[r.str("Table")].push_back(std::move(row));
            }
        }
        for (MonQuestInfo& q : monQuests_) {
            auto t = mt.find(q.resultTable);
            if (t != mt.end()) q.results = t->second;
        }
    } else err += e + "  ";

    // quest_db.yml (QuestInfo) + quest_result_db.yml (its QuestResult table's rows, in the table's order)
    quests_.clear();
    questGraph_.reset();
    {
        std::unordered_map<std::string, std::vector<QuestResultRow>> tables;
        recs.clear();
        if (read_db("quest_result_db", recs, e)) {
            for (const YRecord& r : recs) {
                QuestResultRow row;
                row.count = (int)r.num("Count");
                row.type = (int)r.num("Type");
                for (int i = 0; i < 4; ++i) {
                    const uint32_t it = (uint32_t)r.num(fmt("Item%d", i).c_str());
                    if (it) row.items.push_back(QuestNeed{ it, (int)r.num(fmt("Cnt%d", i).c_str()) });
                }
                row.gelder = (int)r.num("Gelder");
                row.exp = (int)r.num("Exp");
                row.tmxp = (int)r.num("Tmxp");
                tables[r.str("Table")].push_back(std::move(row));
            }
        } else err += e + "  ";
        // quest_db.yml and scenario_quest_db.yml (ScenarioQuestInfo: the same columns)
        auto read_quests = [&](const char* file, std::vector<QuestInfo>& out) {
        recs.clear();
        out.clear();
        if (read_db(file, recs, e)) {
            for (const YRecord& r : recs) {
                QuestInfo q;
                q.id = (int)r.num("Id");
                q.name = r.str("Name");
                q.client = r.str("Client");
                q.pos = r.str("Pos");
                q.comment = r.str("Comment");
                q.minLevel = (int)r.num("MinLevel");
                q.maxLevel = (int)r.num("MaxLevel");
                q.exp = (int)r.num("Exp");
                q.tmxp = (int)r.num("Tmxp");
                for (int i = 0; i < 6; ++i) {
                    const uint32_t it = (uint32_t)r.num(fmt("SrcItem%d", i).c_str());
                    if (!it) break;
                    q.src.push_back(QuestNeed{ it, (int)r.num(fmt("Cnt%d", i).c_str()) });
                }
                q.srcSkill = (int)r.num("SrcSkill0");
                q.srcSkillLv = (int)r.num("SrcSkillLv0");
                q.srcGelder = (int)r.num("SrcGelder");
                const long long mc = r.num("MaxCount");
                q.maxCount = mc <= 0 ? 25 : int(std::min<long long>(mc, 255));
                q.options = (int)r.num("Options");
                auto t = tables.find(r.str("ResultTable"));
                if (t != tables.end()) q.results = t->second;
                if (q.id) out.push_back(std::move(q));
            }
            std::sort(out.begin(), out.end(), [](const QuestInfo& a, const QuestInfo& b) { return a.id < b.id; });
        } else err += e + "  ";
        };
        read_quests("quest_db", quests_);
        read_quests("scenario_quest_db", squests_);
    }

    // npc_check_lv_msg_db.yml (NpcCheckLvMsg): the PreCheck 5 tables
    lvMsgs_.clear();
    recs.clear();
    if (read_db("npc_check_lv_msg_db", recs, e)) {
        for (const YRecord& r : recs) {
            NpcLvMsg m;
            for (int i = 0; i < 6; ++i) {
                m.min[i] = (int)r.num(fmt("Min%d", i).c_str());
                m.max[i] = (int)r.num(fmt("Max%d", i).c_str());
                m.msg[i] = (int)r.num(fmt("Msg%d", i).c_str());
            }
            m.reject = (int)r.num("RejectMsg");
            lvMsgs_[(int)r.num("ID")] = m;
        }
    } else err += e + "  ";

    // mon_spawn_db.yml (the Mon_<area> tables) through each map's MonsterTable / MonsterAvrNum
    spawns_.clear();
    recs.clear();
    if (read_db("mon_spawn_db", recs, e)) {
        struct Row { uint32_t mob; double ratio; int avr; int region; };
        std::unordered_map<std::string, std::vector<Row>> tables;
        for (const YRecord& r : recs)
            tables[r.str("Table")].push_back(Row{ (uint32_t)r.num("MonsterID"), r.fnum("Ratio"), (int)r.num("Avr"), (int)r.num("Region") });
        for (const auto& kv : mapRows_) {
            auto t = tables.find(kv.second.monsterTable);
            if (kv.second.monsterTable.empty() || t == tables.end()) continue;
            int ratioSum = 0;
            for (const Row& row : t->second) ratioSum += int(row.ratio);
            std::unordered_map<uint32_t, MobSpawn> here;
            for (const Row& row : t->second) {
                const int n = row.avr > 0 ? row.avr
                            : ratioSum > 0 ? int(double(kv.second.monsterAvr) * row.ratio / ratioSum) : 0;
                MobSpawn& h = here[row.mob];
                h.zone = kv.first;
                h.count += std::max(n, 1);
                if (std::find(h.regions.begin(), h.regions.end(), row.region) == h.regions.end()) h.regions.push_back(row.region);
            }
            for (auto& h : here) spawns_[h.first].push_back(std::move(h.second));
        }
        for (auto& kv : spawns_)
            std::sort(kv.second.begin(), kv.second.end(), [](const MobSpawn& a, const MobSpawn& b) {
                return a.count != b.count ? a.count > b.count : a.zone < b.zone;
            });
    } else err += e + "  ";

    // the libconfig's text is big (146 MB): what reads it later is built now, then it goes
    if (lib_) {
        atlas();
        item_sources();
        lib_.reset();
    }
    return err.empty();
}

const MonQuestInfo* GameData::monster_quest(int id) const
{
    auto it = std::lower_bound(monQuests_.begin(), monQuests_.end(), id, [](const MonQuestInfo& q, int v) { return q.id < v; });
    return it != monQuests_.end() && it->id == id ? &*it : nullptr;
}

bool GameData::no_skill_zone(int zone) const
{
    auto it = mapRows_.find(zone);
    if (it == mapRows_.end()) return false;
    const int t = it->second.dig.type;
    return t == 1 || t == 5 || t == 6;
}

int GameData::equip_tier(uint32_t id) const
{
    const ItemInfo* ii = item(id);
    if (!ii || ii->cls != 2 || (ii->type != 2 && ii->type != 3 && ii->type != 8)) return ET_None;
    const std::string& n = ii->name;
    auto starts = [&](const char* w) { const size_t k = strlen(w); return n.size() > k && n.compare(0, k, w) == 0 && n[k] == ' '; };
    if (starts("Old")) return ET_Old;
    if (starts("Strong") || starts("Sharp")) return ET_Strong;
    if (starts("Art")) return ET_Art;
    if (starts("Ult.")) return ET_Ult;
    return ET_None;
}

const char* GameData::equip_tier_name(int tier)
{
    static const char* const k[ET_Count] = { "Old", "Strong", "Art", "Ult." };
    return tier >= 0 && tier < ET_Count ? k[tier] : "";
}

const QuestInfo* GameData::quest(int id) const
{
    auto it = std::lower_bound(quests_.begin(), quests_.end(), id, [](const QuestInfo& q, int v) { return q.id < v; });
    return it != quests_.end() && it->id == id ? &*it : nullptr;
}

const QuestInfo* GameData::scenario_quest(int id) const
{
    auto it = std::lower_bound(squests_.begin(), squests_.end(), id, [](const QuestInfo& q, int v) { return q.id < v; });
    return it != squests_.end() && it->id == id ? &*it : nullptr;
}

// ProjectPAULA npc_talk.cpp npc_precheck case 5: the first range (both ends set) holding the level, else RejectMsg
int GameData::lv_msg(int table, int level) const
{
    auto it = lvMsgs_.find(table);
    if (it == lvMsgs_.end()) return -1;
    const NpcLvMsg& m = it->second;
    for (int i = 0; i < 6; ++i)
        if (m.min[i] && m.max[i] && m.min[i] <= level && m.max[i] >= level) return m.msg[i];
    return m.reject;
}

std::vector<MobSpawn> GameData::spawns_of(uint32_t mob) const
{
    auto it = spawns_.find(mob);
    return it == spawns_.end() ? std::vector<MobSpawn>() : it->second;
}

int GameData::stat_value(int gl, int stat) const
{
    if (statRows_.size() <= 1 || stat < 0 || stat >= ST_COUNT) return 0;
    gl = std::clamp(gl, 1, int(statRows_.size()) - 1);
    return statRows_[size_t(gl)][size_t(stat)];
}

bool MapGrid::blocked(int x, int y) const
{
    if (x < 0 || y < 0) return true;
    const int cx = x / 16, cy = y / 16;
    if (cx >= w || cy >= h) return true;
    return cell[size_t(cy) * size_t(w) + size_t(cx)] == 1;
}

bool MapGrid::line_clear(int x0, int y0, int x1, int y1) const
{
    const double len = std::hypot(double(x1 - x0), double(y1 - y0));
    const int steps = std::max(1, int(len / 8.0));
    for (int i = 1; i <= steps; ++i) {         // (our own cell is not checked: we stand on it)
        const int x = x0 + int((x1 - x0) * double(i) / steps);
        const int y = y0 + int((y1 - y0) * double(i) / steps);
        if (blocked(x, y)) return false;
    }
    return true;
}

int MapGrid::component(int x, int y) const
{
    if (x < 0 || y < 0) return 0;
    const int cx = x / 16, cy = y / 16;
    if (cx >= w || cy >= h || !open_cell(cx, cy)) return 0;
    if (comp_.empty()) {
        comp_.assign(size_t(w) * size_t(h), 0);
        std::vector<int> stack;
        int next = 0;
        for (int sy = 0; sy < h; ++sy)
            for (int sx = 0; sx < w; ++sx) {
                const size_t s = size_t(sy) * size_t(w) + size_t(sx);
                if (comp_[s] || !open_cell(sx, sy)) continue;
                comp_[s] = ++next;
                stack.assign(1, int(s));
                while (!stack.empty()) {
                    const int i = stack.back();
                    stack.pop_back();
                    const int ix = i % w, iy = i / w;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            const int nx = ix + dx, ny = iy + dy;
                            if (!open_cell(nx, ny)) continue;
                            if (dx && dy && (!open_cell(ix + dx, iy) || !open_cell(ix, iy + dy))) continue;
                            int32_t& c = comp_[size_t(ny) * size_t(w) + size_t(nx)];
                            if (c) continue;
                            c = next;
                            stack.push_back(ny * w + nx);
                        }
                }
            }
    }
    return comp_[size_t(cy) * size_t(w) + size_t(cx)];
}

bool MapGrid::nearest_open(int x, int y, int maxCells, int& ox, int& oy) const
{
    const int cx = std::clamp(x / 16, 0, std::max(0, w - 1)), cy = std::clamp(y / 16, 0, std::max(0, h - 1));
    double best = -1.0;
    for (int r = 0; r <= maxCells; ++r) {
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;     // this ring only
                const int nx = cx + dx, ny = cy + dy;
                if (!open_cell(nx, ny)) continue;
                const double d = std::hypot(double(nx * 16 + 8 - x), double(ny * 16 + 8 - y));
                if (best < 0 || d < best) { best = d; ox = nx * 16 + 8; oy = ny * 16 + 8; }
            }
        if (best >= 0) return true;
    }
    return false;
}

const Atlas& GameData::atlas() const
{
    if (!atlas_) {
        atlas_ = std::make_shared<Atlas>();
        atlas_->build(*this, dbDir_, mapDir_);
    }
    return *atlas_;
}

const ItemSources& GameData::item_sources() const
{
    if (!sources_) {
        sources_ = std::make_shared<ItemSources>();
        std::string err;
        sources_->load(dbDir_, *this, err);
    }
    return *sources_;
}

const QuestGraph& GameData::quest_graph() const
{
    if (!questGraph_) {
        questGraph_ = std::make_shared<QuestGraph>();
        questGraph_->build(*this);
    }
    return *questGraph_;
}

const GunInfo* GameData::gun(uint32_t id) const
{
    auto it = guns_.find(id);
    return it == guns_.end() ? nullptr : &it->second;
}

const DrillInfo* GameData::drill(uint32_t id) const
{
    auto it = drills_.find(id);
    return it == drills_.end() ? nullptr : &it->second;
}

double GameData::drill_exp_rate(int grade) const
{
    if (drillExp_.empty()) return 0.0;
    return drillExp_[size_t(std::clamp(grade, 0, int(drillExp_.size()) - 1))];
}

const MapDigInfo* GameData::map_dig(int zone) const
{
    auto it = mapRows_.find(zone);
    return it == mapRows_.end() || it->second.dig.itemPct <= 0.0f ? nullptr : &it->second.dig;
}

std::string GameData::map_name(int zone) const
{
    auto it = mapRows_.find(zone);
    return it == mapRows_.end() ? fmt("zone %d", zone) : it->second.name;
}

// The .md3 (ProjectPAULA map_attr.hpp MapAttrDb::load): header 24 x u32 ([0] 'MDN_', [6..9] the
// bounds, [17] the layer count), then the layers {u32 type, w, h, cellW, cellH, w*h bytes}; type 1 =
// the blocked cells, type 3 = the ground (each byte through MapAttr -> the cell's nature, sub_8209C0).
const MapGrid* GameData::map(int zone) const
{
    auto cached = maps_.find(zone);
    if (cached != maps_.end()) return cached->second.get();
    std::unique_ptr<MapGrid> out;
    auto row = mapRows_.find(zone);
    if (row != mapRows_.end() && !row->second.file.empty()) {
        const std::string& f = row->second.file;
        const size_t cut = f.find_last_of("\\/");
        const std::string path = mapDir_ + "\\" + (cut == std::string::npos ? f : f.substr(cut + 1));
        std::ifstream in(path, std::ios::binary);
        uint32_t hdr[24] = {};
        std::vector<uint8_t> ground;
        if (in && in.read((char*)hdr, sizeof(hdr)) && hdr[0] == 1598964813u) {   // 'MDN_'
            for (uint32_t i = 0; i < hdr[17] && in; ++i) {
                uint32_t lh[5] = {};
                if (!in.read((char*)lh, sizeof(lh))) break;
                const uint64_t bytes = uint64_t(lh[1]) * lh[2];
                const bool sane = lh[1] > 0 && lh[2] > 0 && lh[1] < 0x8000 && lh[2] < 0x8000;
                if (lh[0] == 1 && sane && !out) {
                    auto g = std::make_unique<MapGrid>();
                    g->w = int(lh[1]);
                    g->h = int(lh[2]);
                    g->cell.resize(size_t(bytes));
                    if (!in.read((char*)g->cell.data(), std::streamsize(bytes))) break;
                    g->bx1 = int(int32_t(hdr[6])); g->by1 = int(int32_t(hdr[7]));
                    g->bx2 = int(int32_t(hdr[8])); g->by2 = int(int32_t(hdr[9]));
                    g->file = path;
                    out = std::move(g);
                    continue;
                }
                if (lh[0] == 3 && sane && ground.empty()) {
                    ground.resize(size_t(bytes));
                    if (!in.read((char*)ground.data(), std::streamsize(bytes))) { ground.clear(); break; }
                    continue;
                }
                in.seekg(std::streamoff(bytes), std::ios::cur);
            }
        }
        if (out && !ground.empty()) {           // (a Type past the 17-entry table reads 0, as the retail)
            out->nature.assign(out->cell.size(), 0);
            const size_t n = std::min(ground.size(), out->nature.size());
            for (size_t i = 0; i < n; ++i) out->nature[i] = ground[i] < 17 ? groundAttr_[ground[i]] : 0;
        }
    }
    const MapGrid* p = out.get();
    maps_[zone] = std::move(out);
    return p;
}

const ItemInfo* GameData::item(uint32_t id) const
{
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

const MobInfo* GameData::mob(uint32_t id) const
{
    auto it = mobs_.find(id);
    return it == mobs_.end() ? nullptr : &it->second;
}

const SkillInfo* GameData::skill(uint32_t id) const
{
    auto it = skills_.find(id);
    return it == skills_.end() ? nullptr : &it->second;
}

std::string GameData::item_name(uint32_t id) const
{
    const ItemInfo* i = item(id);
    return i && !i->name.empty() ? i->name : fmt("item %u", id);
}

std::string GameData::mob_name(uint32_t id) const
{
    const MobInfo* m = mob(id);
    return m && !m->name.empty() ? m->name : fmt("monster %u", id);
}

namespace {
std::string lower_ascii(std::string s)
{
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

// ids of `m` whose name holds `needle` (case-insensitive) or whose id is it; sorted, at most `max`
template <typename M>
std::vector<uint32_t> search(const M& m, const std::string& needle, size_t max)
{
    std::vector<uint32_t> out;
    const std::string n = lower_ascii(needle);
    if (n.empty()) return out;
    char* end = nullptr;
    const unsigned long asId = strtoul(n.c_str(), &end, 10);
    const bool numeric = end && *end == 0;
    for (const auto& kv : m)
        if ((numeric && kv.first == asId) || lower_ascii(kv.second.name).find(n) != std::string::npos) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    if (out.size() > max) out.resize(max);
    return out;
}
} // namespace

std::vector<uint32_t> GameData::search_items(const std::string& needle, size_t max) const { return search(items_, needle, max); }
std::vector<uint32_t> GameData::search_mobs(const std::string& needle, size_t max) const { return search(mobs_, needle, max); }

const SkillLevelInfo* GameData::skill_level(uint32_t id, int lv) const
{
    const SkillInfo* s = skill(id);
    if (!s || s->levels.empty()) return nullptr;
    lv = std::clamp(lv, 1, (int)s->levels.size());
    return &s->levels[size_t(lv - 1)];
}

std::string GameData::skill_name(uint32_t id) const
{
    const SkillInfo* s = skill(id);
    return s && !s->name.empty() ? s->name : fmt("skill %u", id);
}

} // namespace nj
