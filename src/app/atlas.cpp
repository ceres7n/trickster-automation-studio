// The atlas (atlas.h): the maps, their portals and NPCs, a path finder and the route planner.
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/yml_lite.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <queue>

namespace nj {
namespace {

constexpr double kChangeSecs = 4.0;     // a map change: leave, the new link, the map loads (154 -> 156)
constexpr double kTalkSecs   = 2.0;     // a teleport NPC's talk (0x6F -> 112 -> 0x7F)
constexpr double kGelderSecs = 1.0 / 200.0;   // a teleport's price weighs like this much walking
constexpr double kAdPenaltySecs = 15.0;   // a Portable Port AD use (one item) weighs like this much walking

std::string base_name(const std::string& f)
{
    const size_t cut = f.find_last_of("\\/");
    return cut == std::string::npos ? f : f.substr(cut + 1);
}

const AtlasPoint* first_point(const AtlasMap& m, uint32_t type, uint32_t id)
{
    for (const AtlasPoint& p : m.points)
        if (p.type == type && p.id == id) return &p;
    return nullptr;
}

// The script functions of the client's table 0xFACE78 ({u32 id, char* name}, ids 1..275 in order; read off
// Client_Janus\Trickster.bin) — the id the client sends in C2S 0x7F for an action's name (sub_77C720).
const char* const kFuncNames[] = {
    "", "Exit", "Quest_Check", "Quest_Accept", "Quest_Complete", "Quest_Continue", "Quest_Quit", "MonQuest_Check",
    "MonQuest_Accept", "MonQuest_Complete", "MonQuest_Continue", "MonQuest_Quit", "MonQuest_AllRank", "MonQuest_ZoneRank",
    "Move_Check", "Move_Complete", "Warehouse_Start", "NeoWarehouse_Start", "Teleport_Check", "Teleport_Start",
    "Compound_Start", "NotifyCardBattleRank", "ExchangeShop_Start", "SkillList_Check", "SkillQuest_Check",
    "SkillQuest_Accept", "SkillQuest_Complete", "SkillMonQuest_Check", "SkillMonQuest_Accept", "SkillMonQuest_Complete",
    "SkillGQuest_Check", "SkillGQuest_Accept", "SkillGQuest_Complete", "NotifyCertainSkillRank", "NotifyCertainStatusRank",
    "PurStorage_Start", "SkillShop_Buy", "SkillShop_Sell", "SkillShop_Exit", "SkillMaster_Check", "TuCourse_CheckOne",
    "TuCourse_CheckTwo", "TuCourse_CheckThree", "TuCourse_Complete", "TuQuest_Check", "TuQuest_Accept", "TuQuest_Complete",
    "TuMonQuest_Check", "TuMonQuest_Accept", "TuMonQuest_Complete", "Tutorial_Clear", "End_Tutorial", "Tutorial_Teleport",
    "GuildSetup_Ckeck", "GuildDissolve_Check", "GuildTotalGuild_Process", "GuildRecruitBulletin_Process",
    "GuildApplyCancel_Process", "GuildWithDraw_Process", "SkillMasterUI", "MagicElementUI", "CJQuest_Check",
    "CJQuest_Accept", "CJQuest_Complete", "CJUIOpen", "EventIssueItem_Check", "RetrieveUnitedItem_Start",
    "GuildGame_ShowLobby", "GuildGame_ShowLadder", "DisguiseChar", "LevelGame_ShowLobby", "Item_Refine", "Item_Growth",
    "Reset_Mix", "FType_P_Only", "FType_M_Only", "FType_S_Only", "FType_C_Only", "Item_ChargeTimer", "GuildChangeMaster",
    "GuildChangePassword", "GuildChangeGuildName", "CheckQuestState", "CheckMonQuestState", "CheckHaveItem",
    "EventGame_ShowLobby", "CheckGender", "CheckDailyValue", "Limit_Teleport", "EquipComposing", "Quest_Check_1",
    "Quest_Check_2", "Quest_Check_3", "Quest_Check_4", "Quest_Check_5", "Quest_Check_6", "Quest_Accept_1", "Quest_Accept_2",
    "Quest_Accept_3", "Quest_Accept_4", "Quest_Complete_1", "Quest_Complete_2", "Quest_Complete_3", "Quest_Complete_4",
    "CheckQuestState_1", "CheckQuestState_2", "Quest_Continue_1", "Quest_Continue_2", "Quest_Continue_3",
    "Quest_Continue_4", "Limit_Teleport_50", "CJQuest2_Check", "CJQuest2_Accept", "CJQuest2_Complete", "ActiveQuestID_0",
    "ActiveQuestID_1", "ActiveQuestID_2", "WarpGateSkill", "Check_Job", "Give_Item", "Take_Item", "Random_Move",
    "S_Quest_Check", "S_Quest_Check_1", "S_Quest_Check_2", "S_Quest_Check_3", "S_Quest_Check_4", "S_Quest_Accept",
    "S_Quest_Accept_1", "S_Quest_Accept_2", "S_Quest_Accept_3", "S_Quest_Accept_4", "S_Quest_Complete",
    "S_Quest_Complete_1", "S_Quest_Complete_2", "S_Quest_Complete_3", "S_Quest_Complete_4", "S_Quest_Continue",
    "S_Quest_Continue_1", "S_Quest_Continue_2", "S_Quest_Continue_3", "S_Quest_Continue_4", "S_ActiveQuestID_0",
    "S_ActiveQuestID_1", "S_ActiveQuestID_2", "S_CheckQuestState", "S_CheckQuestState_1", "S_CheckQuestState_2",
    "Img_Teleport_Start", "MonQuest_NH_Complete", "DisguiseCharUI", "Random_Msg", "NewEquipComposing", "P_Quest_Check",
    "P_Quest_Check_1", "P_Quest_Check_2", "P_Quest_Check_3", "P_Quest_Check_4", "P_Quest_Accept", "P_Quest_Accept_1",
    "P_Quest_Accept_2", "P_Quest_Accept_3", "P_Quest_Accept_4", "P_Quest_Complete", "P_Quest_Complete_1",
    "P_Quest_Complete_2", "P_Quest_Complete_3", "P_Quest_Complete_4", "P_Quest_Success", "P_Quest_Success_1",
    "P_Quest_Success_2", "P_Quest_Success_3", "P_Quest_Success_4", "PetLevelUp", "GuildWeekRank_Process", "VIP_Shop",
    "EquipReform", "PetRetrain", "ItemStatCheck", "ItemStatCheck1", "ItemStatCheck2", "Quest_NH_Complete", "Take_Gelder",
    "Give_Gelder", "Quest_Once_Accept", "MonQuest_Once_Accept", "Skill_Check_0", "Skill_Check_1", "Skill_Check_2",
    "Skill_Check_3", "Skill_Check_4", "Skill_Check_5", "Skill_Check_6", "Skill_Check_7", "Skill_Check_8", "Skill_Check_9",
    "Skill_Check", "CheckItemCnt", "CheckItemCnt1", "PetReinforce", "PetReinforce_Reset", "EnterFortuneHouse",
    "Quest_Once_Accept_1", "Quest_Once_Accept_2", "Quest_Once_Accept_3", "Quest_Once_Accept_4", "InvenItemRecycle",
    "MyCampItemRecycle", "RecycleCouponExchange", "ReformIni", "Skill_Check2_1", "Skill_Check2_2", "PetComposing",
    "T_Quest_Check", "T_Quest_Accept", "T_Quest_Complete", "T_Quest_Continue", "T_Quest_Quit", "CheckEnterPartyZone",
    "MonsterSummon", "NpcOpenWeb", "AwakenComplete", "AwakenReset", "EventTalkStart", "CheckSympathizingState",
    "MakeCouple", "BrokenCouple", "Limit_Teleport_30", "CheckMarriageState", "ExchangeMarriage", "OpenMarriageRoom",
    "OpenMarriageBoard", "BrokenMarriage", "CheckQuestStateLover", "CheckCoupleSkill", "CheckMarriageOption",
    "Check_Awaken", "PetMerge", "MonQuest_Check_1", "MonQuest_Check_2", "MonQuest_Check_3", "MonQuest_Check_4",
    "MonQuest_Accept_1", "MonQuest_Accept_2", "MonQuest_Accept_3", "MonQuest_Accept_4", "MonQuest_Complete_1",
    "MonQuest_Complete_2", "MonQuest_Complete_3", "MonQuest_Complete_4", "MonQuest_Continue_1", "MonQuest_Continue_2",
    "MonQuest_Continue_3", "MonQuest_Continue_4", "EventSceneStart", "DecompoundItem", "EquipMerge", "WarpGateSkill_CM",
    "NpcSkillStart", "HarconDefEnter", "ChkJumping", "AskJumping", "CreateHarconCannon", "EquipMergeCM", "HarconSkill",
    "HarconCannonCheck", "MeltingStart", "SlotExtend", "GhostExchange_MC_Start", "GhostExchange_Neo_Start",
    "Item_GhostRefine", "Mission_Check", "Harcon_Shop", "ChargeSoulWeapon", "SoulWeaponPlus", "SoulEquipmentCraftInfo",
};
constexpr int kFuncCount = int(sizeof(kFuncNames) / sizeof(kFuncNames[0]));   // 276 (0 unused)
static_assert(kFuncCount == 276, "the client's table holds ids 1..275");

// An action of a dialog option: a message number, or a script call with its ','-separated arguments as the client
// sends them (sub_C033A0: n, q<n> = 128 + n, m<n> = 256 + n, s<n> = 512 + n; at most 10); a leading "(condition)"
// is skipped. false = neither (an empty / '*' / '@' action).
bool parse_action(std::string a, DialogOpt& o)
{
    while (!a.empty() && (a.back() == '\'' || a.back() == ' ')) a.pop_back();
    while (!a.empty() && a.front() == ' ') a.erase(a.begin());
    if (!a.empty() && a[0] == '(') {
        const size_t close = a.find(')');
        if (close == std::string::npos) return false;
        a = a.substr(close + 1);
    }
    if (a.empty()) return false;
    auto tagged_num = [](const std::string& v, int& out) {
        if (v.empty()) return false;
        const char k = char(v[0] | 0x20);
        const bool tagged = (k == 'q' || k == 'm' || k == 's') && v.size() > 1 && v[1] >= '0' && v[1] <= '9';
        if (!tagged && (v[0] < '0' || v[0] > '9')) return false;
        const int n = std::atoi(v.c_str() + (tagged ? 1 : 0));
        out = n + (!tagged ? 0 : k == 'q' ? 128 : k == 'm' ? 256 : 512);
        return true;
    };
    const size_t comma = a.find(',');
    int n = 0;
    if (comma == std::string::npos && tagged_num(a, n)) { o.link = n; return true; }
    const std::string name = a.substr(0, comma);
    if (name.empty() || name[0] == '*' || name[0] == '@') return false;
    o.link = -1;
    o.func = Atlas::func_id(name);
    o.args.clear();
    size_t s = comma;
    while (s != std::string::npos && o.args.size() < 10) {
        const size_t e = a.find(',', s + 1);
        const std::string v = a.substr(s + 1, e == std::string::npos ? std::string::npos : e - s - 1);
        s = e;
        if (tagged_num(v, n)) o.args.push_back(uint16_t(n));
    }
    return true;
}

// a message's options: tok[0] = the text, then label, action, label, action ...
std::vector<DialogOpt> options_of(const std::string& message)
{
    std::vector<DialogOpt> out;
    std::vector<std::string> tok;
    size_t p = 0;
    for (size_t q = message.find('\\'); ; q = message.find('\\', p)) {
        tok.push_back(message.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    for (size_t i = 2; i < tok.size(); i += 2) {
        DialogOpt o;
        if (parse_action(tok[i], o)) out.push_back(std::move(o));
    }
    return out;
}

// The monster quest functions (client table 0xFACE78 = ProjectPAULA npc_talk.cpp; slots _1.._4 after the base).
uint8_t monq_func(const std::string& name)
{
    static const struct { const char* name; uint8_t base, slot1; } kF[] = {
        { "MonQuest_Accept", 8, 242 }, { "MonQuest_Complete", 9, 246 },
        { "MonQuest_Once_Accept", 185, 0 }, { "MonQuest_NH_Complete", 149, 0 },
    };
    for (const auto& f : kF) {
        if (name == f.name) return f.base;
        const size_t n = strlen(f.name);
        if (f.slot1 && name.size() == n + 2 && name.compare(0, n, f.name) == 0 && name[n] == '_' && name[n + 1] >= '1' &&
            name[n + 1] <= '4')
            return uint8_t(f.slot1 + (name[n + 1] - '1'));
    }
    return 0;
}

// A message's script actions of those functions: the text, then '\'-separated label / action pairs; an
// action may lead with a "(condition)" (sub_C033A0 skips it).
void monq_calls(const std::string& message, int msgNo, std::vector<NpcScriptCall>& out)
{
    std::vector<std::string> tok;
    size_t p = 0;
    for (size_t q = message.find('\\'); ; q = message.find('\\', p)) {
        tok.push_back(message.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    for (size_t i = 2; i < tok.size(); i += 2) {                 // tok[0] = the text, then label, action, ...
        std::string a = tok[i];
        while (!a.empty() && (a.back() == '\'' || a.back() == ' ')) a.pop_back();
        if (!a.empty() && a[0] == '(') {
            const size_t close = a.find(')');
            if (close == std::string::npos) continue;
            a = a.substr(close + 1);
        }
        const size_t comma = a.find(',');
        const uint8_t f = monq_func(a.substr(0, comma));
        if (!f) continue;
        NpcScriptCall c;
        c.func = f;
        c.msgNo = msgNo;
        size_t s = comma;
        while (s != std::string::npos && c.args.size() < 10) {
            const size_t e = a.find(',', s + 1);
            const std::string v = a.substr(s + 1, e == std::string::npos ? std::string::npos : e - s - 1);
            s = e;
            if (v.empty()) continue;
            const char k = char(v[0] | 0x20);
            const bool tagged = (k == 'q' || k == 'm' || k == 's') && v.size() > 1 && v[1] >= '0' && v[1] <= '9';
            if (!tagged && (v[0] < '0' || v[0] > '9')) continue;
            const int n = std::atoi(v.c_str() + (tagged ? 1 : 0));
            c.args.push_back(uint16_t(n + (!tagged ? 0 : k == 'q' ? 128 : k == 'm' ? 256 : 512)));
        }
        out.push_back(std::move(c));
    }
}

// the comma-separated u16 arguments that follow `name,` up to the next '\' (the action's end)
std::vector<uint16_t> script_args(const std::string& text, size_t at, size_t nameLen)
{
    std::vector<uint16_t> out;
    size_t p = at + nameLen;
    while (p < text.size() && text[p] == ',') {
        ++p;
        size_t e = p;
        while (e < text.size() && text[e] >= '0' && text[e] <= '9') ++e;
        if (e == p) break;
        out.push_back(uint16_t(std::strtoul(text.substr(p, e - p).c_str(), nullptr, 10)));
        p = e;
    }
    return out;
}

} // namespace

std::string AtlasNpc::key() const { return fmt("%d:%u:%d", zone, species, posId); }

// ── loading ─────────────────────────────────────────────────────────────────
// The .md3 (ProjectPAULA map_attr.hpp): header 24 x u32 ([0] 'MDN_', [1] version, [17] layers,
// [18] regions, [19] points), the layers {u32 type, w, h, cellW, cellH, w*h bytes}, then the regions
// (0x1C bytes each, 0x20 from version 301: i32 id, type, param, x1, y1, x2, y2) and the points
// (0x14 / 0x18: u32 id, type, param, x, y).
void Atlas::read_md3(AtlasMap& m, const std::string& mapDir)
{
    if (m.file.empty()) return;
    std::ifstream in(mapDir + "\\" + base_name(m.file), std::ios::binary);
    uint32_t hdr[24] = {};
    if (!in || !in.read((char*)hdr, sizeof(hdr)) || hdr[0] != 1598964813u) return;
    for (uint32_t i = 0; i < hdr[17]; ++i) {
        uint32_t lh[5] = {};
        if (!in.read((char*)lh, sizeof(lh))) return;
        in.seekg(std::streamoff(uint64_t(lh[1]) * lh[2]), std::ios::cur);
    }
    const size_t regionSz = hdr[1] < 301 ? 0x1C : 0x20, pointSz = hdr[1] < 301 ? 0x14 : 0x18;
    if (hdr[18] > 100000 || hdr[19] > 100000) return;
    std::vector<uint8_t> rec(std::max(regionSz, pointSz));
    for (uint32_t i = 0; i < hdr[18]; ++i) {
        if (!in.read((char*)rec.data(), std::streamsize(regionSz))) return;
        int32_t r[7];
        memcpy(r, rec.data(), sizeof(r));
        if (r[1] != 1) continue;                            // BuildRegions case 1: a portal
        AtlasPortal p;
        p.id = uint32_t(r[0]);
        p.toZone = r[2];
        p.x1 = int16_t(r[3]); p.y1 = int16_t(r[4]); p.x2 = int16_t(r[5]); p.y2 = int16_t(r[6]);
        m.portals.push_back(p);
    }
    for (uint32_t i = 0; i < hdr[19]; ++i) {
        if (!in.read((char*)rec.data(), std::streamsize(pointSz))) break;
        uint32_t v[5];
        memcpy(v, rec.data(), sizeof(v));
        if (v[1] == 3 || v[1] == 6) {                   // the monsters' regeneration points (regen.cpp)
            m.regen.push_back(AtlasPoint{ v[0], v[1], v[2], int(int16_t(v[3])), int(int16_t(v[4])) });
            continue;
        }
        if (v[1] != 1 && v[1] != 2 && v[1] != 4 && v[1] != 5 && v[1] != 7 && v[1] != 9) continue;
        m.points.push_back(AtlasPoint{ v[0], v[1], v[2], int(int16_t(v[3])), int(int16_t(v[4])) });
    }
    m.hasFile = true;
}

void Atlas::build(const GameData& gd, const std::string& dbDir, const std::string& mapDir)
{
    gd_ = &gd;
    maps_.clear();
    npcs_.clear();
    dialogs_.clear();
    exitCache_.clear();
    goalField_.clear();
    goalZone_ = -1;
    std::string err, e;
    std::vector<YRecord> recs;

    std::unordered_map<int, std::pair<std::string, std::string>> tables;   // zone -> NpcTable, ShopTable
    if (gd.read_db("maps_db", recs, e)) {
        for (const YRecord& r : recs) {
            AtlasMap m;
            m.id    = (int)r.num("ID");
            m.name  = r.str("Name");
            m.file  = r.str("FileName");
            m.type  = (int)r.num("Type");
            m.cls   = (int)r.num("Class");
            m.group = (int)r.num("Group");
            m.town  = (int)r.num("Town");
            m.option = (int)r.num("Option");
            m.shopPriceRate = float(r.fnum("ShopPriceRate"));
            if (m.shopPriceRate <= 0.0f) m.shopPriceRate = 1.0f;
            tables[m.id] = { r.str("NpcTable"), r.str("ShopTable") };
            maps_[m.id] = std::move(m);
        }
    } else err += e + "  ";
    size_t files = 0, portals = 0;
    for (auto& kv : maps_) {
        read_md3(kv.second, mapDir);
        files += kv.second.hasFile;
        portals += kv.second.portals.size();
    }

    // which message tables run the warehouse / the bank (and with which arguments)
    struct Wh { bool wh = false, bank = false; std::vector<uint16_t> args; };
    std::unordered_map<std::string, Wh> whTables;
    std::unordered_map<std::string, std::vector<NpcScriptCall>> monqTables;   // the monster quest calls
    {
        // the libconfig: its NpcMsg_ records -> "Message: text" lines, as the yml file's (one line each)
        std::vector<std::string> libLines;
        if (gd.source().find(".xml") != std::string::npos) {
            std::vector<YRecord> msgs;
            std::string me;
            gd.read_db("npc_msg_db", msgs, me);
            std::string tbl;
            for (const YRecord& r : msgs) {
                if (r.str("Table") != tbl) { tbl = r.str("Table"); libLines.push_back("  - Table: " + tbl); }
                libLines.push_back("    No: " + std::to_string(r.num("No")));
                libLines.push_back("    Message: " + r.str("Message"));
            }
        }
        std::ifstream in;
        if (libLines.empty()) in.open(dbDir + "npc_msg_db.yml", std::ios::binary);
        std::string line, table;
        int msgNo = 0;
        size_t li = 0;
        while (libLines.empty() ? bool(std::getline(in, line)) : li < libLines.size()) {
            if (!libLines.empty()) line = libLines[li++];
            const size_t t = line.find("- Table: ");
            if (t != std::string::npos) { table = yml_trim(line.substr(t + 9)); continue; }
            const size_t no = line.find("No: ");
            if (no != std::string::npos && line.find("Message") == std::string::npos) { msgNo = std::atoi(line.c_str() + no + 4); continue; }
            if (line.find("Message:") != std::string::npos) {
                DialogMsg& dm = dialogs_[table].msgs[msgNo];
                dm.opts = options_of(line);
                dm.text = yml_trim(line.substr(line.find("Message:") + 8));
                if (dm.text.size() >= 2 && dm.text.front() == '\'' && dm.text.back() == '\'') {      // 'single quoted', '' = '
                    std::string u;
                    for (size_t k = 1; k + 1 < dm.text.size(); ++k) {
                        u += dm.text[k];
                        if (dm.text[k] == '\'' && dm.text[k + 1] == '\'') ++k;
                    }
                    dm.text = std::move(u);
                }
            }
            if (line.find("MonQuest_") != std::string::npos) monq_calls(line, msgNo, monqTables[table]);
            if (line.find("Warehouse_Start") == std::string::npos) continue;
            const size_t neo = line.find("NeoWarehouse_Start");
            Wh& w = whTables[table];
            if (neo != std::string::npos) w.bank = true;
            size_t at = line.find("Warehouse_Start");
            while (at != std::string::npos) {
                const bool isNeo = at >= 3 && line.compare(at - 3, 3, "Neo") == 0;
                if (!isNeo) {
                    w.wh = true;
                    if (w.args.empty()) w.args = script_args(line, at, strlen("Warehouse_Start"));
                }
                at = line.find("Warehouse_Start", at + 1);
            }
        }
        if (libLines.empty() && !in.eof() && whTables.empty()) err += "npc_msg_db.yml unreadable  ";
    }

    struct Tp { int minLevel = 0; std::vector<TeleportSlot> slots; };
    std::unordered_map<int, Tp> teleports;
    recs.clear();
    if (gd.read_db("teleport_db", recs, e)) {
        for (const YRecord& r : recs) {
            Tp t;
            t.minLevel = (int)r.num("MinLevel");
            for (int i = 0; i < 7; ++i) {                  // only a non-zero zone is a slot (npc_script_db.hpp)
                const int z = (int)r.num(fmt("Zone%d", i).c_str());
                if (!z) continue;
                t.slots.push_back(TeleportSlot{ int(t.slots.size()), z, (int)r.num(fmt("Cost%d", i).c_str()) });
            }
            teleports[(int)r.num("Id")] = std::move(t);
        }
    } else err += e + "  ";

    // the NPC moves (npc_move_db; GameInfoMgr sub_6288A0: a negative level -> 0, Min > Max -> both 0)
    std::unordered_map<int, TeleportSlot> moves;
    recs.clear();
    if (gd.read_db("npc_move_db", recs, e)) {
        for (const YRecord& r : recs) {
            TeleportSlot s;
            s.move = true;
            s.zone = (int)r.num("Zone");
            s.destId = (int)r.num("DestId");
            for (int i = 0; i < 4; ++i) {
                const uint32_t it = (uint32_t)r.num(fmt("SrcItem%d", i).c_str());
                if (it) s.items.push_back({ it, std::max(1, (int)r.num(fmt("Cnt%d", i).c_str())) });
            }
            s.minLevel = std::max(0, (int)r.num("MinLevel"));
            s.maxLevel = std::max(0, (int)r.num("MaxLevel"));
            if (s.minLevel > s.maxLevel) s.minLevel = s.maxLevel = 0;
            s.ftype = (int)r.num("FType", -1);
            s.chrType = (int)r.num("ChrType");
            s.takes = r.num("TakeItem") == 1;
            if (s.zone > 0 && s.zone < 1000) moves[(int)r.num("Id")] = std::move(s);
        }
    } else err += e + "  ";
    // the port items with one destination (random_port_db: only a ZoneID 1..999 counts)
    portItems_.clear();
    recs.clear();
    if (gd.read_db("random_port_db", recs, e)) {
        for (const YRecord& r : recs) {
            std::set<int> zones;
            for (int i = 0; i < 20; ++i) {
                const int z = (int)r.num(fmt("ZoneID_%d", i).c_str());
                if (z >= 1 && z < 1000 && r.num(fmt("Ratio_%d", i).c_str()) > 0) zones.insert(z);
            }
            if (zones.size() == 1) portItems_[(uint32_t)r.num("ID")] = *zones.begin();
        }
    } else err += e + "  ";
    // the Portable Port AD's destinations (teleport_adv_db)
    adv_.clear();
    recs.clear();
    if (gd.read_db("teleport_adv_db", recs, e))
        for (const YRecord& r : recs) {
            const int z = (int)r.num("ZoneID");
            if (z > 0 && z < 1000) adv_.push_back({ (int)r.num("ZoneIndex"), z });
        }
    // the exchange shops (exchange_shop_db: the first row of an id; exchange_shop_table_db: a table's ids in order)
    exchanges_.clear();
    std::unordered_map<std::string, std::vector<int>> excTables;
    recs.clear();
    if (gd.read_db("exchange_shop_db", recs, e)) {
        for (const YRecord& r : recs) {
            ExchangeDef x;
            x.id = (int)r.num("ExchangeID");
            if (exchanges_.count(x.id)) continue;
            x.fee = (int)r.num("Fee");
            for (int i = 1; i <= 3; ++i) {
                const uint32_t it = (uint32_t)r.num(fmt("ResID%d", i).c_str());
                if (!it) break;
                x.res.push_back(it);
            }
            for (int i = 1; i <= 5; ++i) {
                const uint32_t it = (uint32_t)r.num(fmt("ReqID%d", i).c_str());
                if (!it) break;
                x.req.push_back({ it, (int)r.num(fmt("ReqNum%d", i).c_str()) });
            }
            exchanges_[x.id] = std::move(x);
        }
    }
    recs.clear();
    if (gd.read_db("exchange_shop_table_db", recs, e))
        for (const YRecord& r : recs) excTables[r.str("Table")].push_back((int)r.num("ExchangeID"));
    const uint8_t fnMove = func_id("Move_Complete"), fnExchange = func_id("ExchangeShop_Start"), fnRandom = func_id("Random_Move");
    const uint8_t fnCompound = func_id("Compound_Start");

    std::unordered_map<std::string, std::vector<YRecord>> npcRows, shopRows;
    recs.clear();
    if (gd.read_db("npc_db", recs, e)) {
        for (YRecord& r : recs) npcRows[r.str("Table")].push_back(std::move(r));
    } else err += e + "  ";
    recs.clear();
    if (gd.read_db("shop_db", recs, e)) {
        for (YRecord& r : recs) shopRows[r.str("Table")].push_back(std::move(r));
    } else err += e + "  ";
    std::unordered_map<std::string, std::vector<uint32_t>> shopItems;
    std::unordered_map<std::string, std::vector<float>> shopRates;
    recs.clear();
    if (gd.read_db("shop_item_db", recs, e)) {
        for (const YRecord& r : recs) {
            if (shopItems[r.str("Table")].size() >= 128) continue;     // (sub_62A0A0: the first 128 rows)
            // OutOfStock = not in the buy list (S2C 0x47): a row only for its SellingRate - what the shop BUYS from you
            // (a live report: Snow Hill's droppable items shown as "to buy" from R_ShopItem_Snow's sell rows)
            if (r.num("OutOfStock") == 1) continue;
            shopItems[r.str("Table")].push_back((uint32_t)r.num("ItemId"));
            shopRates[r.str("Table")].push_back(float(r.fnum("PurchaseRate")));
        }
    } else err += e + "  ";
    // the compounds and the compounders' lists (compound_db / compounder_db: the first list of a name)
    compounds_.clear();
    recs.clear();
    if (gd.read_db("compound_db", recs, e))
        for (const YRecord& r : recs) {
            CompoundDef c;
            c.id = (int)r.num("CompoundID");
            const std::string t = r.str("Table");
            c.type = t == "Compound_Rare" ? 1 : t == "Compound_Throw" ? 2 : 0;
            c.probability = (int)r.num("Probability");
            c.fee = (int)r.num("Fee");
            c.waste = (uint32_t)r.num("WasteItem");
            for (int i = 1; i <= 3; ++i) { const uint32_t it = (uint32_t)r.num(fmt("ResID%d", i).c_str()); if (!it) break; c.res.push_back(it); }
            for (int i = 1; i <= 5; ++i) {
                const uint32_t it = (uint32_t)r.num(fmt("ReqID%d", i).c_str());
                if (!it) break;
                c.req.push_back({ it, std::max(1, (int)r.num(fmt("ReqNum%d", i).c_str())) });
            }
            if (c.id && !c.res.empty() && !compounds_.count(c.id)) compounds_[c.id] = std::move(c);
        }
    std::unordered_map<std::string, std::vector<int>> cmpLists;   // list -> its shown compounds
    recs.clear();
    if (gd.read_db("compounder_db", recs, e)) {
        std::set<std::string> done;                 // (a list named twice: the first one)
        std::string cur;
        for (const YRecord& r : recs) {
            const std::string ln = r.str("ListName");
            if (ln != cur) { if (!cur.empty()) done.insert(cur); cur = ln; }
            if (done.count(ln) || r.num("Hidden") == 1) continue;
            const int id = (int)r.num("CompoundID");
            if (id) cmpLists[ln].push_back(id);
        }
    }

    for (auto& kv : maps_) {
        AtlasMap& m = kv.second;
        const auto& tb = tables[m.id];
        auto nt = npcRows.find(tb.first);
        if (!tb.first.empty() && nt != npcRows.end())
            for (const YRecord& r : nt->second) {
                const int type = (int)r.num("Type");
                if ((type < 1000 || type >= 1450) && (type < 17000 || type >= 480000)) continue;   // CreateNpc refuses it
                AtlasNpc n;
                n.kind = AtlasNpc::Talk;
                n.zone = m.id;
                n.species = uint16_t(type);
                n.name = r.str("Name");
                n.posId = (int)r.num("PosId");
                if (const AtlasPoint* p = first_point(m, 4, uint32_t(n.posId))) { n.x = p->x; n.y = p->y; n.hasPoint = true; }
                n.msgTable = r.str("MsgTable");
                auto w = whTables.find(n.msgTable);
                if (w != whTables.end()) {
                    n.warehouse = w->second.wh;
                    n.bank = w->second.bank;
                    n.warehouseArgs = w->second.args;
                }
                n.quests[0] = (int)r.num("QuestId");
                for (int q = 1; q < 5; ++q) n.quests[q] = (int)r.num(fmt("QuestId%d", q).c_str());
                auto mq = monqTables.find(n.msgTable);
                if (mq != monqTables.end()) n.monq = mq->second;
                n.preType = (int)r.num("PreCheckType");
                n.preArg = (int)r.num("PreCheckArg");
                n.preYes = (int)r.num("PreCheckYes");
                n.preNo = (int)r.num("PreCheckNo");
                static const char* const kCq[3] = { "CheckQuestId", "CheckQuestId1", "CheckQuestId2" };
                static const char* const kCn[3] = { "CheckQuestNum", "CheckQuestNum1", "CheckQuestNum2" };
                for (int k = 0; k < 3; ++k) {
                    n.checkQuest[k] = int(int16_t(r.num(kCq[k])));
                    n.checkNum[k] = int(int16_t(r.num(kCn[k])));
                    n.activeQuest[k] = (int)r.num(fmt("ActiveQuestID%d", k).c_str());
                }
                n.checkMonQuest = (int)r.num("CheckMonQuestId");
                n.checkMonNum = (int)r.num("CheckMonQuestNum");
                n.checkItem[0] = (uint32_t)r.num("CheckItemId0");
                n.checkItem[1] = (uint32_t)r.num("CheckItemId1");
                n.giveItem = (uint32_t)r.num("GiveItemID");
                n.jobFlag1 = (uint32_t)r.num("JobTypeFlag1");
                n.jobFlag2 = (uint32_t)r.num("JobTypeFlag2");
                n.takeItem = (uint32_t)r.num("TakeItemID");
                auto dl = dialogs_.find(n.msgTable);
                if (dl != dialogs_.end()) n.dialog = &dl->second;
                n.teleportId = (int)r.num("TeleportId");
                auto t = teleports.find(n.teleportId);
                if (n.teleportId && t != teleports.end()) {
                    n.teleportMinLevel = t->second.minLevel;
                    n.teleports = t->second.slots;
                }
                // its move: the Move_Complete its dialog sends (the first one; npc_move_complete answers with the
                // first argument's message)
                n.moveId = (int)r.num("NpcMoveId");
                auto mv = moves.find(n.moveId);
                if (n.moveId && mv != moves.end() && n.dialog) {
                    const DialogOpt* call = nullptr;
                    for (const auto& dm : n.dialog->msgs) {
                        for (const DialogOpt& o : dm.second.opts)
                            if (o.link < 0 && o.func == fnMove) { call = &o; break; }
                        if (call) break;
                    }
                    if (call) {
                        TeleportSlot s = mv->second;
                        s.slot = int(n.teleports.size());
                        s.func = fnMove;
                        s.args = call->args;
                        n.teleports.push_back(std::move(s));
                    }
                }
                // its Random_Move: one slot per zone among its five arguments
                if (n.dialog) {
                    const DialogOpt* rnd = nullptr;
                    for (const auto& dm : n.dialog->msgs) {
                        for (const DialogOpt& o : dm.second.opts)
                            if (o.link < 0 && o.func == fnRandom && o.args.size() >= 5) { rnd = &o; break; }
                        if (rnd) break;
                    }
                    if (rnd) {
                        std::map<int, int> count;
                        for (size_t k = 0; k < 5; ++k) ++count[rnd->args[k]];
                        for (const auto& kv : count) {
                            if (kv.first <= 0 || kv.first >= 1000) continue;
                            TeleportSlot s;
                            s.slot = int(n.teleports.size());
                            s.zone = kv.first;
                            s.func = fnRandom;
                            s.random = true;
                            s.chance = kv.second / 5.0;
                            s.args = rnd->args;
                            n.teleports.push_back(std::move(s));
                        }
                    }
                }
                auto xt = excTables.find(r.str("ExcShopTable"));
                if (xt != excTables.end() && n.dialog) {
                    for (const auto& dm : n.dialog->msgs)
                        for (const DialogOpt& o : dm.second.opts)
                            if (o.link < 0 && o.func == fnExchange && n.exchanges.empty()) {
                                n.exchanges = xt->second;
                                n.exchangeArgs = o.args;
                            }
                }
                auto ct = cmpLists.find(r.str("CompounderTable"));
                if (ct != cmpLists.end() && n.dialog) {
                    for (const auto& dm : n.dialog->msgs)
                        for (const DialogOpt& o : dm.second.opts)
                            if (o.link < 0 && o.func == fnCompound && n.compounds.empty()) {
                                n.compounds = ct->second;
                                n.compoundArgs = o.args;
                            }
                }
                if (!n.teleports.empty()) m.teleporters.push_back(int(npcs_.size()));
                m.npcs.push_back(int(npcs_.size()));
                npcs_.push_back(std::move(n));
            }
        auto st = shopRows.find(tb.second);
        if (!tb.second.empty() && st != shopRows.end())
            for (const YRecord& r : st->second) {
                AtlasNpc n;
                n.kind = AtlasNpc::Shop;
                n.zone = m.id;
                n.species = uint16_t(r.num("NpcType"));
                n.name = r.str("Name");
                n.posId = (int)r.num("PosId");
                if (const AtlasPoint* p = first_point(m, 5, uint32_t(n.posId))) { n.x = p->x; n.y = p->y; n.hasPoint = true; }
                n.itemTable = r.str("ItemTable");
                n.purchaseRate = float(r.fnum("PurchaseRate"));
                if (n.purchaseRate <= 0.0f) n.purchaseRate = 1.0f;
                auto it = shopItems.find(n.itemTable);
                if (it != shopItems.end()) { n.items = it->second; n.itemRates = shopRates[n.itemTable]; }
                m.npcs.push_back(int(npcs_.size()));
                npcs_.push_back(std::move(n));
            }
    }
    size_t tps = 0, whs = 0, shops = 0, mvs = 0, exs = 0, cps = 0;
    sellers_.clear();
    for (size_t i = 0; i < npcs_.size(); ++i)
        if (npcs_[i].kind == AtlasNpc::Shop)
            for (uint32_t it : npcs_[i].items) {
                std::vector<int>& v = sellers_[it];
                if (std::find(v.begin(), v.end(), int(i)) == v.end()) v.push_back(int(i));
            }
    for (const AtlasNpc& n : npcs_) {
        tps += !n.teleports.empty();
        whs += n.warehouse;
        shops += n.kind == AtlasNpc::Shop;
        exs += !n.exchanges.empty();
        cps += !n.compounds.empty();
        for (const TeleportSlot& s : n.teleports) mvs += s.move;
    }
    unlocks_.clear();
    for (const auto& kv : portItems_) unlocks_.push_back(kv.first);
    for (const AtlasNpc& n : npcs_)
        for (const TeleportSlot& s : n.teleports)
            if (s.move) for (const auto& it : s.items) unlocks_.push_back(it.first);
    std::sort(unlocks_.begin(), unlocks_.end());
    unlocks_.erase(std::unique(unlocks_.begin(), unlocks_.end()), unlocks_.end());
    report_ = fmt("atlas: %zu maps (%zu .md3), %zu portals, %zu NPCs (%zu shops, %zu warehouses, %zu teleporters, %zu moves,"
                  " %zu exchange shops, %zu compounders), %zu port items",
                  maps_.size(), files, portals, npcs_.size(), shops, whs, tps, mvs, exs, cps, portItems_.size());
    if (!err.empty()) report_ += "  (" + err + ")";
}

// ── lookups ─────────────────────────────────────────────────────────────────
const AtlasMap* Atlas::map(int zone) const
{
    auto it = maps_.find(zone);
    return it == maps_.end() ? nullptr : &it->second;
}

void Atlas::parse_monq_calls(const std::string& message, int msgNo, std::vector<NpcScriptCall>& out)
{
    monq_calls(message, msgNo, out);
}

std::vector<DialogOpt> Atlas::parse_options(const std::string& message) { return options_of(message); }

int Dialog::identify(const std::string& text) const
{
    if (text.empty()) return -1;
    const std::vector<DialogOpt> o = options_of(text);
    auto same_opts = [&](const std::vector<DialogOpt>& b) {
        if (b.size() != o.size()) return false;
        for (size_t i = 0; i < b.size(); ++i)
            if (b[i].link != o[i].link || b[i].func != o[i].func || b[i].args != o[i].args) return false;
        return true;
    };
    int best = -1;
    size_t bestLen = 0;
    for (const auto& m : msgs) {
        if (m.second.text == text) return m.first;
        if (!same_opts(m.second.opts)) continue;
        size_t k = 0;
        while (k < text.size() && k < m.second.text.size() && text[k] == m.second.text[k]) ++k;
        if (best < 0 || k > bestLen) { best = m.first; bestLen = k; }
    }
    return best;
}

uint8_t Atlas::func_id(const std::string& name)
{
    static const std::unordered_map<std::string, uint8_t> ids = [] {
        std::unordered_map<std::string, uint8_t> m;
        for (int i = 1; i < kFuncCount; ++i) m[kFuncNames[i]] = uint8_t(i);
        return m;
    }();
    auto it = ids.find(name);
    return it == ids.end() ? 0 : it->second;
}

const char* Atlas::func_name(uint8_t id) { return id > 0 && id < kFuncCount ? kFuncNames[id] : "?"; }

std::vector<std::pair<int, int>> Atlas::regen_points(int zone, const std::vector<int>& regions) const
{
    std::vector<std::pair<int, int>> out;
    const AtlasMap* m = map(zone);
    if (!m) return out;
    bool type3 = regions.empty();
    for (int r : regions) {
        bool found = false;
        if (r > 0)
            for (const AtlasPoint& p : m->regen)
                if (p.type == 6 && int(p.id) == r) { out.push_back({ p.x, p.y }); found = true; }
        if (!found) type3 = true;
    }
    if (type3) for (const AtlasPoint& p : m->regen) if (p.type == 3) out.push_back({ p.x, p.y });
    if (out.empty()) for (const AtlasPoint& p : m->points) if (p.type == 1) { out.push_back({ p.x, p.y }); break; }
    return out;
}

std::vector<int> Atlas::quest_givers(int qid) const
{
    std::vector<int> out;
    for (size_t i = 0; i < npcs_.size(); ++i)
        if (npcs_[i].kind == AtlasNpc::Talk && npcs_[i].quest_slot(qid) >= 0) out.push_back(int(i));
    return out;
}

int Atlas::find_npc(const std::string& key) const
{
    if (key.empty()) return -1;
    for (size_t i = 0; i < npcs_.size(); ++i)
        if (npcs_[i].key() == key) return int(i);
    return -1;
}

std::string Atlas::map_label(int zone) const
{
    const AtlasMap* m = map(zone);
    return m ? fmt("%s (%d)", m->name.c_str(), zone) : fmt("map %d", zone);
}

std::vector<int> Atlas::shops() const
{
    std::vector<int> out;
    for (size_t i = 0; i < npcs_.size(); ++i)
        if (npcs_[i].kind == AtlasNpc::Shop && !npcs_[i].items.empty()) out.push_back(int(i));
    std::sort(out.begin(), out.end(), [&](int a, int b) {
        const AtlasNpc &x = npcs_[size_t(a)], &y = npcs_[size_t(b)];
        return x.zone != y.zone ? x.zone < y.zone : x.posId < y.posId;
    });
    return out;
}

std::vector<int> Atlas::warehouses(bool bank) const
{
    std::vector<int> out;
    for (size_t i = 0; i < npcs_.size(); ++i)
        if (bank ? npcs_[i].bank : npcs_[i].warehouse) out.push_back(int(i));
    std::sort(out.begin(), out.end(), [&](int a, int b) {
        const AtlasNpc &x = npcs_[size_t(a)], &y = npcs_[size_t(b)];
        return x.zone != y.zone ? x.zone < y.zone : x.posId < y.posId;
    });
    return out;
}

// ── the grid: A*, string pulling, distance fields ───────────────────────────
namespace {

// 8-way steps, a diagonal only when both of its sides are open (the client walks no corners); cost 10 /
// 14 per step (16 units per 10).
template <class Visit>
void neighbours(const MapGrid& g, int i, Visit visit)
{
    const int w = g.w, ix = i % w, iy = i / w;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const int nx = ix + dx, ny = iy + dy;
            if (!g.open_cell(nx, ny)) continue;
            if (dx && dy && (!g.open_cell(ix + dx, iy) || !g.open_cell(ix, iy + dy))) continue;
            visit(ny * w + nx, dx && dy ? 14 : 10);
        }
}

// A* from `start` to the first cell `goal` accepts; `hcost` = an octile lower bound to it. The cell
// path (start excluded) in `cells`, as cell centres. false = none.
template <class Goal, class H>
bool astar(const MapGrid& g, int start, Goal goal, H hcost, std::vector<std::pair<int, int>>& cells)
{
    cells.clear();
    const int w = g.w;
    const size_t n = size_t(g.w) * size_t(g.h);
    std::vector<int32_t> cost(n, INT32_MAX), from(n, -1);
    using QE = std::pair<int32_t, int32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    cost[size_t(start)] = 0;
    open.push({ hcost(start), start });
    int found = -1;
    while (!open.empty()) {
        const QE top = open.top();
        open.pop();
        const int i = top.second;
        const int32_t c = cost[size_t(i)];
        if (top.first - hcost(i) > c) continue;         // a stale entry
        if (goal(i)) { found = i; break; }
        neighbours(g, i, [&](int j, int step) {
            const int32_t nc = c + step;
            if (nc >= cost[size_t(j)]) return;
            cost[size_t(j)] = nc;
            from[size_t(j)] = i;
            open.push({ nc + hcost(j), j });
        });
    }
    if (found < 0) return false;
    for (int i = found; i != start && i >= 0; i = from[size_t(i)]) cells.push_back({ (i % w) * 16 + 8, (i / w) * 16 + 8 });
    std::reverse(cells.begin(), cells.end());
    return true;
}

int octile(int dx, int dy)
{
    dx = std::abs(dx);
    dy = std::abs(dy);
    return 10 * std::max(dx, dy) + 4 * std::min(dx, dy);
}

// String pulling: from (sx, sy), the farthest corner of the cell path in a straight open line, legs <= maxLeg.
void pull(const MapGrid& g, int sx, int sy, const std::vector<std::pair<int, int>>& cells, int maxLeg,
          std::vector<std::pair<int, int>>& legs)
{
    legs.clear();
    int cx = sx, cy = sy;
    size_t i = 0;
    while (i < cells.size()) {
        size_t best = i;
        for (size_t k = i; k < cells.size(); ++k) {
            if (std::hypot(double(cells[k].first - cx), double(cells[k].second - cy)) > maxLeg) break;
            if (g.line_clear(cx, cy, cells[k].first, cells[k].second)) best = k;
        }
        legs.push_back(cells[best]);
        cx = cells[best].first;
        cy = cells[best].second;
        i = best + 1;
    }
}

// The cell to start from: ours, or the nearest open one (we may stand on the edge of a wall). -1 = none.
int start_cell(const MapGrid& g, int x, int y)
{
    int ox = x, oy = y;
    if (g.blocked(x, y) && !g.nearest_open(x, y, 6, ox, oy)) return -1;
    return (oy / 16) * g.w + ox / 16;
}

// Every cell's walking cost from (x, y) (10 per 16 units; INT32_MAX = no way): Dial's buckets, the steps
// being 10 or 14.
void distance_field(const MapGrid& g, int x, int y, std::vector<int32_t>& dist)
{
    dist.assign(size_t(g.w) * size_t(g.h), INT32_MAX);
    const int s = start_cell(g, x, y);
    if (s < 0) return;
    std::vector<int> bucket[15];
    dist[size_t(s)] = 0;
    bucket[0].push_back(s);
    size_t pending = 1;
    for (int32_t cur = 0; pending; ++cur) {
        std::vector<int>& b = bucket[cur % 15];
        for (size_t k = 0; k < b.size(); ++k) {           // (a step is >= 10: nothing joins this bucket now)
            const int i = b[k];
            --pending;
            if (dist[size_t(i)] != cur) continue;
            neighbours(g, i, [&](int j, int step) {
                const int32_t nd = cur + step;
                if (nd >= dist[size_t(j)]) return;
                dist[size_t(j)] = nd;
                bucket[nd % 15].push_back(j);
                ++pending;
            });
        }
        b.clear();
    }
}

// The same from every open cell within `radius` units of (x, y), each starting at its straight distance: the walk to
// NEAR the point. An NPC may stand on a pocket of a few open cells walled in on the grid (Indiana John in Relics Field 2:
// 8 cells) — a field from its own cell alone reaches nothing, and every route to it failed.
void distance_field_near(const MapGrid& g, int x, int y, int radius, std::vector<int32_t>& dist)
{
    dist.assign(size_t(g.w) * size_t(g.h), INT32_MAX);
    using QE = std::pair<int32_t, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    const int r = std::max(1, radius / 16 + 1);
    for (int cy = std::max(0, y / 16 - r); cy <= std::min(g.h - 1, y / 16 + r); ++cy)
        for (int cx = std::max(0, x / 16 - r); cx <= std::min(g.w - 1, x / 16 + r); ++cx) {
            if (!g.open_cell(cx, cy)) continue;
            const double d = std::hypot(double(cx * 16 + 8 - x), double(cy * 16 + 8 - y));
            if (d > radius) continue;
            const int i = cy * g.w + cx;
            const int32_t c = int32_t(d * 10.0 / 16.0);
            if (c < dist[size_t(i)]) { dist[size_t(i)] = c; q.push({ c, i }); }
        }
    while (!q.empty()) {
        const QE top = q.top();
        q.pop();
        if (top.first != dist[size_t(top.second)]) continue;
        neighbours(g, top.second, [&](int j, int step) {
            const int32_t nd = top.first + step;
            if (nd >= dist[size_t(j)]) return;
            dist[size_t(j)] = nd;
            q.push({ nd, j });
        });
    }
}

// The least field value over the open cells whose centre lies in the box (units); INT32_MAX = none reached,
// `any` = the box holds an open cell at all.
int32_t field_min_box(const MapGrid& g, const std::vector<int32_t>& f, int x1, int y1, int x2, int y2, bool* any = nullptr)
{
    int32_t best = INT32_MAX;
    for (int cy = std::max(0, y1 / 16); cy <= std::min(g.h - 1, y2 / 16); ++cy)
        for (int cx = std::max(0, x1 / 16); cx <= std::min(g.w - 1, x2 / 16); ++cx) {
            const int px = cx * 16 + 8, py = cy * 16 + 8;
            if (px < x1 || px >= x2 || py < y1 || py >= y2 || !g.open_cell(cx, cy)) continue;
            if (any) *any = true;
            best = std::min(best, f[size_t(cy) * size_t(g.w) + size_t(cx)]);
        }
    return best;
}

int32_t field_min_circle(const MapGrid& g, const std::vector<int32_t>& f, int x, int y, int r)
{
    int32_t best = INT32_MAX;
    for (int cy = std::max(0, (y - r) / 16); cy <= std::min(g.h - 1, (y + r) / 16); ++cy)
        for (int cx = std::max(0, (x - r) / 16); cx <= std::min(g.w - 1, (x + r) / 16); ++cx) {
            const int px = cx * 16 + 8, py = cy * 16 + 8;
            if (std::hypot(double(px - x), double(py - y)) > r || !g.open_cell(cx, cy)) continue;
            best = std::min(best, f[size_t(cy) * size_t(g.w) + size_t(cx)]);
        }
    return best;
}

float units(int32_t v) { return v == INT32_MAX ? -1.0f : float(v) * 1.6f; }   // 10 per 16 units

} // namespace

void Atlas::walk_field(const MapGrid& g, int x, int y, std::vector<int32_t>& field)
{
    if (g.w <= 0 || g.h <= 0) { field.clear(); return; }
    distance_field(g, x, y, field);
}

double Atlas::field_walk(const MapGrid& g, const std::vector<int32_t>& field, int x, int y, int ring)
{
    if (field.size() != size_t(g.w) * size_t(g.h) || x < 0 || y < 0) return -1.0;
    const int cx = x / 16, cy = y / 16;
    if (!g.inside(cx, cy)) return -1.0;
    const int32_t own = field[size_t(cy) * size_t(g.w) + size_t(cx)];
    if (g.open_cell(cx, cy)) return own == INT32_MAX ? -1.0 : double(own) * 1.6;
    double best = -1.0;
    for (int dy = -ring; dy <= ring; ++dy)
        for (int dx = -ring; dx <= ring; ++dx) {
            const int nx = cx + dx, ny = cy + dy;
            if (!g.open_cell(nx, ny)) continue;
            const int32_t v = field[size_t(ny) * size_t(g.w) + size_t(nx)];
            if (v == INT32_MAX) continue;
            const double d = double(v) * 1.6 + std::hypot(double(nx * 16 + 8 - x), double(ny * 16 + 8 - y));
            if (best < 0 || d < best) best = d;
        }
    return best;
}

bool Atlas::path(const MapGrid& g, int sx, int sy, int tx, int ty, int within, int maxLeg,
                 std::vector<std::pair<int, int>>& legs, std::string* why)
{
    legs.clear();
    auto no = [&](const char* s) { if (why) *why = s; return false; };
    if (g.w <= 0 || g.h <= 0) return no("no grid");
    const int start = start_cell(g, sx, sy);
    if (start < 0) return no("we stand inside a wall");
    // the end: the target's cell, or the nearest open one to it
    int ex = tx, ey = ty;
    if (g.blocked(tx, ty) && !g.nearest_open(tx, ty, std::max(4, within / 16 + 2), ex, ey)) return no("the target is inside a wall");
    const int w = g.w, goalCell = (ey / 16) * w + ex / 16;
    const double reach = std::max(8.0, double(within));
    std::vector<std::pair<int, int>> cells;
    const bool ok = astar(g, start,
        [&](int i) { return i == goalCell || std::hypot(double((i % w) * 16 + 8 - ex), double((i / w) * 16 + 8 - ey)) <= reach; },
        [&](int i) { return octile(i % w - ex / 16, i / w - ey / 16); }, cells);
    if (!ok) return no("no open way there");
    // the exact target when it is open and in reach from the last cell
    if (!g.blocked(tx, ty) && std::hypot(double(tx - ex), double(ty - ey)) < 1.0 &&
        (cells.empty() || g.line_clear(cells.back().first, cells.back().second, tx, ty)))
        cells.push_back({ tx, ty });
    pull(g, sx, sy, cells, maxLeg, legs);
    return true;
}

bool Atlas::path_rect(const MapGrid& g, int sx, int sy, const AtlasPortal& p, int maxLeg,
                      std::vector<std::pair<int, int>>& legs, std::string* why, bool* noCell)
{
    legs.clear();
    if (noCell) *noCell = false;
    auto no = [&](const char* s) { if (why) *why = s; return false; };
    if (g.w <= 0 || g.h <= 0) return no("no grid");
    const int start = start_cell(g, sx, sy);
    if (start < 0) return no("we stand inside a wall");
    const int x1 = p.x1 - 16, y1 = p.y1 - 16, x2 = p.x2 + 16, y2 = p.y2 + 16;
    const int w = g.w;
    auto inside = [&](int i) {
        const int px = (i % w) * 16 + 8, py = (i / w) * 16 + 8;
        return px >= x1 && px < x2 && py >= y1 && py < y2;
    };
    bool any = false;
    for (int cy = std::max(0, y1 / 16); cy <= std::min(g.h - 1, y2 / 16) && !any; ++cy)
        for (int cx = std::max(0, x1 / 16); cx <= std::min(g.w - 1, x2 / 16) && !any; ++cx)
            any = g.open_cell(cx, cy) && inside(cy * w + cx);
    if (!any) { if (noCell) *noCell = true; return no("the portal has no open cell"); }
    const int cx1 = x1 / 16, cy1 = y1 / 16, cx2 = (x2 - 1) / 16, cy2 = (y2 - 1) / 16;
    std::vector<std::pair<int, int>> cells;
    const bool ok = astar(g, start, inside, [&](int i) {
        const int cx = i % w, cy = i / w;
        return octile(cx < cx1 ? cx1 - cx : cx > cx2 ? cx - cx2 : 0, cy < cy1 ? cy1 - cy : cy > cy2 ? cy - cy2 : 0);
    }, cells);
    if (!ok) return no("no open way to the portal");
    if (cells.empty()) {                                // already inside: a point of the rectangle near us
        legs.push_back({ std::clamp(sx, p.x1, std::max(p.x1, p.x2 - 1)), std::clamp(sy, p.y1, std::max(p.y1, p.y2 - 1)) });
        return true;
    }
    pull(g, sx, sy, cells, maxLeg, legs);
    return true;
}

const std::vector<int>& Atlas::shops_selling(uint32_t item) const
{
    static const std::vector<int> none;
    auto it = sellers_.find(item);
    return it == sellers_.end() ? none : it->second;
}

int Atlas::shop_price(const GameData& gd, const AtlasNpc& shop, uint32_t item) const
{
    const ItemInfo* ii = gd.item(item);
    const AtlasMap* m = map(shop.zone);
    if (!ii) return -1;
    for (size_t i = 0; i < shop.items.size(); ++i) {
        if (shop.items[i] != item) continue;
        const float r = i < shop.itemRates.size() && shop.itemRates[i] > 0.0f ? shop.itemRates[i] : shop.purchaseRate;
        return int(double(m ? m->shopPriceRate : 1.0f) * double(ii->value) * double(r) + 0.5);
    }
    return -1;
}

uint32_t Atlas::port_ad_item(const RouteOptions& o) const
{
    if (!o.portAd || !o.bagKnown || !gd_ || adv_.empty()) return 0;
    for (const auto& kv : o.bag) {
        if (kv.second <= 0) continue;
        const ItemInfo* ii = gd_->item(kv.first);
        if (ii && ii->type == 15 && ii->effect == 20 && (ii->options & 0x8000) && (ii->options & 2)) return kv.first;
    }
    return 0;
}

uint64_t Atlas::hop_key(const RouteHop& h)
{
    if (h.kind == RouteHop::Portal) return uint64_t(uint32_t(h.fromZone)) << 32 | uint32_t(h.portal);
    if (h.kind == RouteHop::Item && h.advIndex >= 0)
        return uint64_t(uint32_t(h.fromZone)) << 32 | 0x40000000u | (h.item & 0xFFFFFu) << 6 | uint32_t(h.advIndex & 63);
    if (h.kind == RouteHop::Item) return uint64_t(uint32_t(h.fromZone)) << 32 | 0x40000000u | (h.item & 0x3FFFFFFFu);
    return uint64_t(uint32_t(h.fromZone)) << 32 | 0x80000000u | uint32_t(h.npc & 0xFFFFF) << 8 | uint32_t(h.slot & 0xFF);
}

// ── walking distances inside a map ──────────────────────────────────────────
const std::vector<float>& Atlas::exit_dists(const GameData& gd, const AtlasMap& m, int x, int y) const
{
    const uint64_t key = uint64_t(uint32_t(m.id)) << 32 | uint64_t(uint16_t(x)) << 16 | uint16_t(y);
    auto it = exitCache_.find(key);
    if (it != exitCache_.end()) return it->second;
    if (exitCache_.size() > 50000) exitCache_.clear();
    std::vector<float> d(m.portals.size() + m.teleporters.size(), -1.0f);
    const MapGrid* g = gd.map(m.id);
    if (!g) {                                           // no map file: the straight line, a third longer
        for (size_t i = 0; i < m.portals.size(); ++i)
            d[i] = float(std::hypot(double(m.portals[i].cx() - x), double(m.portals[i].cy() - y)) * 1.3);
        for (size_t i = 0; i < m.teleporters.size(); ++i) {
            const AtlasNpc& n = npcs_[size_t(m.teleporters[i])];
            d[m.portals.size() + i] = float(std::hypot(double(n.x - x), double(n.y - y)) * 1.3);
        }
    } else {
        std::vector<int32_t> f;
        distance_field(*g, x, y, f);
        for (size_t i = 0; i < m.portals.size(); ++i) {
            const AtlasPortal& p = m.portals[i];
            bool any = false;
            int32_t v = field_min_box(*g, f, p.x1 - 16, p.y1 - 16, p.x2 + 16, p.y2 + 16, &any);
            if (!any) v = field_min_box(*g, f, p.x1 - 64, p.y1 - 64, p.x2 + 64, p.y2 + 64);   // a portal over a wall
            d[i] = units(v);
        }
        for (size_t i = 0; i < m.teleporters.size(); ++i) {
            const AtlasNpc& n = npcs_[size_t(m.teleporters[i])];
            int32_t v = field_min_circle(*g, f, n.x, n.y, 150);
            if (v == INT32_MAX) v = field_min_circle(*g, f, n.x, n.y, 280);   // it stands in a wall: talk from farther
            d[m.portals.size() + i] = units(v);
        }
    }
    return exitCache_.emplace(key, std::move(d)).first->second;
}

float Atlas::goal_dist(const GameData& gd, int zone, int gx, int gy, int x, int y) const
{
    const MapGrid* g = gd.map(zone);
    if (!g) return float(std::hypot(double(gx - x), double(gy - y)) * 1.3);
    if (goalZone_ != zone || goalX_ != gx || goalY_ != gy || goalField_.empty()) {
        distance_field_near(*g, gx, gy, 128, goalField_); // the walk is the same both ways; to within 128 of the point
        goalZone_ = zone;
        goalX_ = gx;
        goalY_ = gy;
    }
    const int c = start_cell(*g, x, y);                 // (the point's cell, or the open one next to it)
    return c < 0 ? -1.0f : units(goalField_[size_t(c)]);
}

// ── the route between maps ──────────────────────────────────────────────────
// An A* over the arrival points: a node is where a hop leaves us in a map — the start, one point (a
// portal's matching type-1 point), or a random one of a set (a portal without one: the type-1 points;
// a teleport: the type-7 points, else the default arrival). From a node every exit of its map is an
// edge: the walk there on the grid (a random node: the mean over its points) + the map change (+ the
// teleport's talk and price), to the node the exit leaves us on. Reaching the goal map adds the walk
// to the goal point. The heuristic: the fewest map changes still needed x the change's time.
// The hops that use an item up (a port item, an NPC move that takes its items) only when no other way gets there.
Route Atlas::route(const GameData& gd, int fromZone, int fx, int fy, int toZone, const RouteOptions& o, int firstExit) const
{
    // the same map: a walk, never a way round through others (a live log: 7 maps to get back into the map it was in)
    if (fromZone == toZone && firstExit < 0) {
        Route w;
        w.ok = map(fromZone) != nullptr;
        if (!w.ok) { w.why = fmt("map %d is unknown", fromZone); return w; }
        if (o.goalX >= 0 && o.goalY >= 0) {
            const float d = goal_dist(gd, toZone, o.goalX, o.goalY, fx, fy);
            w.secs = w.lastWalkSecs = d >= 0 ? d / std::max(40, o.speed) : 0.0;
        }
        return w;
    }
    Route r = route_pass(gd, fromZone, fx, fy, toZone, o, firstExit, false);
    if (r.ok || !o.bagKnown || firstExit >= 0) return r;
    // an item used up (a port item) only when no way exists at all — not because ways are left out for a while
    // (a live log: the Hologram Port spent after a few portals had failed)
    if (!o.banned.empty()) {
        RouteOptions o2 = o;
        o2.banned.clear();
        if (route_pass(gd, fromZone, fx, fy, toZone, o2, firstExit, false).ok) return r;
    }
    Route r2 = route_pass(gd, fromZone, fx, fy, toZone, o, firstExit, true);
    return r2.ok ? r2 : r;
}

Route Atlas::route_pass(const GameData& gd, int fromZone, int fx, int fy, int toZone, const RouteOptions& o, int firstExit,
                        bool consume) const
{
    Route out;
    const AtlasMap* src = map(fromZone);
    if (!src) { out.why = fmt("map %d is unknown", fromZone); return out; }
    if (!map(toZone)) { out.why = fmt("map %d is unknown", toZone); return out; }
    const double speed = std::max(40, o.speed);
    const bool goalPoint = o.goalX >= 0 && o.goalY >= 0;
    auto has = [&](const std::vector<std::pair<uint32_t, int>>& items) {
        if (items.empty()) return true;
        if (!o.bagKnown) return false;
        for (const auto& it : items) {
            auto b = o.bag.find(it.first);
            if (b == o.bag.end() || b->second < it.second) return false;
        }
        return true;
    };
    auto tp_ok = [&](const AtlasNpc& n, const TeleportSlot& sl) {
        if (!map(sl.zone)) return false;
        if (sl.random) return true;                     // (free)
        if (sl.move)                                    // (free: not the teleports switch's)
            return (consume || !sl.takes) && has(sl.items) && (sl.chrType == 0) &&
                   (!(sl.minLevel || sl.maxLevel) || o.level <= 0 || (o.level >= sl.minLevel && o.level <= sl.maxLevel)) &&
                   (sl.ftype < 0 || o.ftype < 0 || sl.ftype == o.ftype);
        return o.teleports && (o.level <= 0 || o.level >= n.teleportMinLevel) && sl.cost <= o.maxGelder &&
               (o.money < 0 || sl.cost <= o.money);
    };
    std::vector<std::pair<uint32_t, int>> ports;       // consume: the bag's port items -> their zone
    if (consume)
        for (const auto& kv : o.bag)
            if (kv.second > 0)
                if (const int z = port_zone(kv.first)) if (map(z)) ports.push_back({ kv.first, z });

    // the heuristic: map changes left (a breadth-first search on the reversed map graph)
    std::unordered_map<int, int> hopsLeft;
    {
        std::unordered_map<int, std::vector<int>> rev;
        for (const auto& kv : maps_) {
            const AtlasMap& m = kv.second;
            for (const AtlasPortal& p : m.portals) if (p.toZone != m.id) rev[p.toZone].push_back(m.id);
            for (int t : m.teleporters)
                for (const TeleportSlot& sl : npcs_[size_t(t)].teleports)
                    if (sl.zone != m.id && tp_ok(npcs_[size_t(t)], sl)) rev[sl.zone].push_back(m.id);
        }
        std::vector<int> q{ toZone };
        hopsLeft[toZone] = 0;
        for (size_t i = 0; i < q.size(); ++i) {
            const int z = q[i];
            for (int y : rev[z])
                if (!hopsLeft.count(y)) { hopsLeft[y] = hopsLeft[z] + 1; q.push_back(y); }
        }
    }
    // a port item / the Port AD: from any map that allows it, its zones are one change away
    const uint32_t adItem = port_ad_item(o);
    int portCap = -1;
    for (const auto& p : ports) {
        auto it = hopsLeft.find(p.second);
        if (it != hopsLeft.end() && (portCap < 0 || it->second + 1 < portCap)) portCap = it->second + 1;
    }
    if (adItem)
        for (const auto& ad : adv_) {
            auto it = hopsLeft.find(ad.second);
            if (it != hopsLeft.end() && (portCap < 0 || it->second + 1 < portCap)) portCap = it->second + 1;
        }
    auto h = [&](int zone) {
        auto it = hopsLeft.find(zone);
        int v = it == hopsLeft.end() ? -1 : it->second;
        if (portCap >= 0) {
            const AtlasMap* m = map(zone);
            const int viaPort = portCap + (m && m->noPort() ? 1 : 0);
            if (v < 0 || viaPort < v) v = viaPort;
        }
        return v < 0 ? -1.0 : v * kChangeSecs;
    };
    if (h(fromZone) < 0) {
        out.why = fmt("no way from %s to %s", map_label(fromZone).c_str(), map_label(toZone).c_str());
        return out;
    }

    enum Kind { kStart = 0, kPoint = 1, kRandom1 = 2, kRandom7 = 3, kDefault = 4, kGoal = 5 };
    auto key = [](int zone, int kind, int idx) { return uint64_t(uint32_t(zone)) << 32 | uint64_t(kind) << 24 | uint32_t(idx & 0xFFFFFF); };
    auto members = [&](int zone, int kind, int idx) {
        std::vector<std::pair<int, int>> pts;
        const AtlasMap* m = map(zone);
        if (kind == kStart) pts.push_back({ fx, fy });
        else if (kind == kPoint && m && idx < (int)m->points.size()) pts.push_back({ m->points[size_t(idx)].x, m->points[size_t(idx)].y });
        else if (m && (kind == kRandom1 || kind == kRandom7)) {
            const uint32_t order7[] = { 7, 2, 1 }, order1[] = { 1 };
            const uint32_t* order = kind == kRandom7 ? order7 : order1;
            const int count = kind == kRandom7 ? 3 : 1;
            for (int t = 0; t < count && pts.empty(); ++t)
                for (const AtlasPoint& p : m->points) if (p.type == order[t]) pts.push_back({ p.x, p.y });
        }
        if (pts.empty()) pts.push_back({ 500, 400 });     // the server's last default
        return pts;
    };
    // where a portal of `zone` puts us in its destination
    auto portal_arrival = [&](int zone, const AtlasPortal& p, RouteHop& hop) {
        const AtlasMap* d = map(p.toZone);
        for (size_t i = 0; i < d->points.size(); ++i) {
            const AtlasPoint& a = d->points[i];
            if (a.type == 1 && a.param == uint32_t(zone) && a.id == (p.id & 0xFF)) {
                hop.arriveX = a.x; hop.arriveY = a.y;
                return key(p.toZone, kPoint, int(i));
            }
        }
        hop.randomArrival = true;
        for (const AtlasPoint& a : d->points)
            if (a.type == 1) { hop.arriveX = a.x; hop.arriveY = a.y; return key(p.toZone, kRandom1, 0); }
        hop.arriveX = 500; hop.arriveY = 400;
        return key(p.toZone, kDefault, 0);
    };

    struct Rec { double g = 1e18; uint64_t parent = 0; RouteHop hop; bool hasHop = false; bool done = false; };
    std::unordered_map<uint64_t, Rec> rec;
    using QE = std::pair<double, uint64_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    const uint64_t startKey = key(fromZone, kStart, 0), goalKey = key(toZone, kGoal, 0);
    rec[startKey].g = 0;
    q.push({ h(fromZone), startKey });
    auto relax = [&](uint64_t to, double g, uint64_t from, const RouteHop* hop) {
        const double hz = h(int(to >> 32));
        if (hz < 0) return;                              // that map never gets there
        Rec& r = rec[to];
        if (r.done || g >= r.g) return;
        r.g = g;
        r.parent = from;
        r.hasHop = hop != nullptr;
        if (hop) r.hop = *hop;
        q.push({ g + hz, to });
    };
    while (!q.empty()) {
        const QE top = q.top();
        q.pop();
        const uint64_t k = top.second;
        Rec& r = rec[k];
        if (r.done) continue;
        r.done = true;
        if (k == goalKey) break;
        const double g0 = r.g;
        const int zone = int(k >> 32), kind = int((k >> 24) & 0xFF), idx = int(k & 0xFFFFFF);
        const AtlasMap* m = map(zone);
        if (!m) continue;
        const std::vector<std::pair<int, int>> pts = members(zone, kind, idx);
        if (zone == toZone) {                            // the goal: the walk to its point
            double walk = 0;
            if (goalPoint) {
                double sum = 0;
                int n = 0;
                for (const auto& p : pts) {
                    const float d = goal_dist(gd, zone, o.goalX, o.goalY, p.first, p.second);
                    if (d >= 0) { sum += d; ++n; }
                }
                walk = n ? sum / n : -1.0;
            }
            if (walk >= 0) relax(goalKey, g0 + walk / speed, k, nullptr);
        }
        // the mean walking distance to each exit over the node's points
        const size_t nx = m->portals.size() + m->teleporters.size();
        std::vector<double> sum(nx, 0.0);
        std::vector<int> cnt(nx, 0);
        for (const auto& p : pts) {
            const std::vector<float>& d = exit_dists(gd, *m, p.first, p.second);
            for (size_t e = 0; e < nx && e < d.size(); ++e)
                if (d[e] >= 0) { sum[e] += d[e]; ++cnt[e]; }
        }
        int exitNo = 0;                                  // the flattened exit number (firstExit)
        for (size_t pi = 0; pi < m->portals.size(); ++pi, ++exitNo) {
            const AtlasPortal& p = m->portals[pi];
            if (k == startKey && firstExit >= 0 && exitNo != firstExit) continue;
            if (p.toZone == zone || !map(p.toZone) || !cnt[pi]) continue;
            RouteHop hop;
            hop.kind = RouteHop::Portal;
            hop.fromZone = zone;
            hop.toZone = p.toZone;
            hop.portal = int(pi);
            hop.x = p.cx();
            hop.y = p.cy();
            if (o.banned.count(hop_key(hop))) continue;
            const uint64_t to = portal_arrival(zone, p, hop);
            hop.secs = sum[pi] / cnt[pi] / speed + kChangeSecs;
            relax(to, g0 + hop.secs, k, &hop);
        }
        for (size_t ti = 0; ti < m->teleporters.size(); ++ti) {
            const size_t e = m->portals.size() + ti;
            const AtlasNpc& n = npcs_[size_t(m->teleporters[ti])];
            for (const TeleportSlot& sl : n.teleports) {
                const int thisExit = exitNo++;
                if (k == startKey && firstExit >= 0 && thisExit != firstExit) continue;
                if (sl.zone == zone || !tp_ok(n, sl) || !cnt[e]) continue;
                RouteHop hop;
                hop.kind = RouteHop::Teleport;
                hop.fromZone = zone;
                hop.toZone = sl.zone;
                hop.npc = m->teleporters[ti];
                hop.slot = sl.slot;
                hop.cost = sl.cost;
                hop.move = sl.move;
                hop.x = n.x;
                hop.y = n.y;
                if (o.banned.count(hop_key(hop))) continue;
                uint64_t to = key(sl.zone, kRandom7, 0);
                const AtlasMap* d = map(sl.zone);
                bool fixed = false;
                for (size_t i = 0; sl.move && d && i < d->points.size(); ++i)     // a move: its type-9 point DestId
                    if (d->points[i].type == 9 && d->points[i].id == uint32_t(sl.destId)) {
                        to = key(sl.zone, kPoint, int(i));
                        hop.arriveX = d->points[i].x;
                        hop.arriveY = d->points[i].y;
                        fixed = true;
                        break;
                    }
                if (!fixed) {
                    const std::vector<std::pair<int, int>> arr = members(sl.zone, kRandom7, 0);
                    hop.arriveX = arr[0].first;
                    hop.arriveY = arr[0].second;
                    hop.randomArrival = arr.size() > 1;
                }
                hop.secs = sum[e] / cnt[e] / speed + kTalkSecs + kChangeSecs + sl.cost * kGelderSecs + (sl.move ? 2.0 : 0.0);
                if (sl.random && sl.chance > 0.0) hop.secs += (1.0 / sl.chance - 1.0) * 2.0 * (kChangeSecs + kTalkSecs);   // the misses
                relax(to, g0 + hop.secs, k, &hop);
            }
        }
        // the Portable Port AD, used where we stand: any zone of its list (a few seconds' worth of walking first: one item)
        if (adItem && !m->noPort() && !(k == startKey && firstExit >= 0))
            for (const auto& ad : adv_) {
                if (ad.second == zone || !map(ad.second)) continue;
                RouteHop hop;
                hop.kind = RouteHop::Item;
                hop.fromZone = zone;
                hop.toZone = ad.second;
                hop.item = adItem;
                hop.advIndex = ad.first;
                hop.x = pts.front().first;
                hop.y = pts.front().second;
                if (o.banned.count(hop_key(hop))) continue;
                const std::vector<std::pair<int, int>> arr = members(ad.second, kRandom7, 0);
                hop.arriveX = arr[0].first;
                hop.arriveY = arr[0].second;
                hop.randomArrival = arr.size() > 1;
                hop.secs = 1.0 + kChangeSecs + kAdPenaltySecs;
                relax(key(ad.second, kRandom7, 0), g0 + hop.secs, k, &hop);
            }
        // a port item of the bag, used where we stand
        if (!ports.empty() && !m->noPort() && !(k == startKey && firstExit >= 0))
            for (const auto& p : ports) {
                if (p.second == zone) continue;
                RouteHop hop;
                hop.kind = RouteHop::Item;
                hop.fromZone = zone;
                hop.toZone = p.second;
                hop.item = p.first;
                hop.x = pts.front().first;
                hop.y = pts.front().second;
                if (o.banned.count(hop_key(hop))) continue;
                const std::vector<std::pair<int, int>> arr = members(p.second, kRandom7, 0);
                hop.arriveX = arr[0].first;
                hop.arriveY = arr[0].second;
                hop.randomArrival = arr.size() > 1;
                hop.secs = 1.0 + kChangeSecs;
                relax(key(p.second, kRandom7, 0), g0 + hop.secs, k, &hop);
            }
    }
    auto goal = rec.find(goalKey);
    if (goal == rec.end() || !goal->second.done) {
        out.why = fmt("no way from %s to %s", map_label(fromZone).c_str(), map_label(toZone).c_str());
        return out;
    }
    out.secs = goal->second.g;
    const uint64_t last = goal->second.parent;
    out.lastWalkSecs = goal->second.g - rec[last].g;
    for (uint64_t k = last; k != startKey;) {
        const Rec& r = rec[k];
        if (!r.hasHop) { out.hops.clear(); out.why = "broken route"; return out; }
        out.hops.push_back(r.hop);
        k = r.parent;
    }
    std::reverse(out.hops.begin(), out.hops.end());
    for (const RouteHop& hop : out.hops) out.gelder += hop.cost;
    out.ok = true;
    return out;
}

bool Atlas::zone_reachable(int fromZone, int toZone, const RouteOptions& o) const
{
    if (fromZone == toZone) return true;
    if (!map(fromZone) || !map(toZone)) return false;
    auto has = [&](const std::vector<std::pair<uint32_t, int>>& items) {
        for (const auto& it : items) {
            auto b = o.bag.find(it.first);
            if (!o.bagKnown || b == o.bag.end() || b->second < it.second) return false;
        }
        return true;
    };
    std::vector<int> ports;
    if (o.bagKnown)
        for (const auto& kv : o.bag)
            if (kv.second > 0) if (const int z = port_zone(kv.first)) ports.push_back(z);
    if (port_ad_item(o)) for (const auto& ad : adv_) ports.push_back(ad.second);
    std::set<int> seen{ fromZone };
    std::vector<int> todo{ fromZone };
    while (!todo.empty()) {
        const int z = todo.back();
        todo.pop_back();
        const AtlasMap* m = map(z);
        if (!m) continue;
        auto go = [&](int to) { if (to > 0 && map(to) && seen.insert(to).second) todo.push_back(to); };
        for (const AtlasPortal& p : m->portals) go(p.toZone);
        for (int t : m->teleporters) {
            const AtlasNpc& n = npcs_[size_t(t)];
            for (const TeleportSlot& sl : n.teleports) {
                if (sl.random) {
                    go(sl.zone);
                } else if (sl.move) {
                    if (has(sl.items) && sl.chrType == 0 && (sl.ftype < 0 || o.ftype < 0 || sl.ftype == o.ftype) &&
                        (!(sl.minLevel || sl.maxLevel) || o.level <= 0 || (o.level >= sl.minLevel && o.level <= sl.maxLevel)))
                        go(sl.zone);
                } else if (o.teleports && (o.level <= 0 || o.level >= n.teleportMinLevel) && sl.cost <= o.maxGelder &&
                           (o.money < 0 || sl.cost <= o.money)) {
                    go(sl.zone);
                }
            }
        }
        if (!m->noPort()) for (int pz : ports) go(pz);
        if (seen.count(toZone)) return true;
    }
    return seen.count(toZone) != 0;
}

std::vector<Route> Atlas::alternatives(const GameData& gd, int fromZone, int fx, int fy, int toZone, const RouteOptions& o,
                                       size_t max) const
{
    std::vector<Route> out;
    const AtlasMap* m = map(fromZone);
    if (!m) return out;
    int exits = int(m->portals.size());
    for (int t : m->teleporters) exits += int(npcs_[size_t(t)].teleports.size());
    for (int e = 0; e < exits; ++e) {
        Route r = route(gd, fromZone, fx, fy, toZone, o, e);
        if (!r.ok || r.hops.empty()) continue;
        // one per first hop's destination and way (two portals to the same map are two ways)
        out.push_back(std::move(r));
    }
    // an item used where we stand (the Portable Port AD) is no exit of the map: the best route when it starts with one
    Route best = route(gd, fromZone, fx, fy, toZone, o);
    if (best.ok && !best.hops.empty() && best.hops.front().kind == RouteHop::Item) out.push_back(std::move(best));
    std::sort(out.begin(), out.end(), [](const Route& a, const Route& b) { return a.secs < b.secs; });
    if (out.size() > max) out.resize(max);
    return out;
}

} // namespace nj
