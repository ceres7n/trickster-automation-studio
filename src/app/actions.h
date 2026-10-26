#pragma once
// C2S actions built from the world model: the bodies the client itself sends for each request.
// Layouts and the server's checks are ProjectPAULA's handler of each opcode (game_session.cpp,
// skill.cpp, item_use.cpp, inventory.cpp); the notes in actions.cpp cite them. The Send tab's quick
// actions use these, and so will the automation (phase 4).
#include <cstdint>
#include <string>
#include <vector>

namespace nj {

class WorldModel;
struct Entity;
struct GroundItem;
struct InvItem;
struct SkillEntry;

struct Action {
    uint16_t             op = 0;
    std::vector<uint8_t> body;
    std::string          label;     // what it does, for the UI / log
    std::string          error;     // set = not built (the reason)
    bool ok() const { return error.empty(); }
};

// Whom a skill goes to. Auto: a buff (Type 3) on ourselves, anything else on the nearest live monster.
struct SkillTarget {
    enum Mode { Auto, Self, NearestMonster, Entity, Point } mode = Auto;
    uint16_t eid = 0;               // Entity
    int      x = 0, y = 0;          // Point
};

namespace act {

// run: the move type the server walks us with (1 walk / 2 run), as the client's sub_6AF5B0 picks it
Action attack(const WorldModel& w, uint16_t eid, bool run = true);   // C2S 0x001D [u16 target][u8 type]
Action pick_up(const WorldModel& w, uint16_t oid, bool run = true);  // C2S 0x0021 [u16 oid][u8 type]
Action sit(uint8_t state);                                        // C2S 0x0040 [u8 0 stand / 2 sit / 13 rest]
Action use_item(const WorldModel& w, uint64_t uid);               // C2S 0x0057 [u64 uid]
// C2S 0x0108 [u64 uid][i16 n][n bytes] (item_use.cpp handle_use_item_custom, client CMD_USE_ITEM_CUSTOM_DATA): an
// Options 0x8000 item with its data — the Portable Port AD: [u32 ZoneIndex] -> S2C 151 + the map change / 566 our own
// zone / 567 no such index / 799 [4] an Option 128 map
Action use_item_custom(const WorldModel& w, uint64_t uid, const std::vector<uint8_t>& data, const std::string& what);
Action use_skill(const WorldModel& w, uint16_t skill, const SkillTarget& t, uint64_t now);  // 0x00EE / 0x00E0
Action move(const WorldModel& w, int x, int y, bool run, uint64_t now);     // C2S 0x0018
// C2S 0x0018 [x][y][x][y][type]: "stand here" — what the client sends for its stop key (sub_6B9F80). The
// server trusts the src, so this puts its position where the client draws us.
Action stop_at(int x, int y, bool run);
Action portal_walk(int x, int y, bool run);                       // C2S 0x01D4
// Guns (gun.cpp): one shot per C2S 0x01DD [u16 target][u16 crc][u16 1][u8 type], crc keyed with the
// last S2C 0x0200 seed; loading bullets = C2S 0x0205 [u64 uid][u8 20]; a seed = C2S 0x01FF [].
Action gun_shot(const WorldModel& w, uint16_t target, bool run);
Action load_bullets(const WorldModel& w, uint64_t uid);
Action gun_seed_request();

// Drilling (drill.cpp): C2S 0x002C [u16 x][u16 y][u8 type] digs at a point — in our cell at once, else the
// server walks us there first (straight, no path) and digs on arrival; S2C 371 then tells the cell's item
// depth and the CRC seed. C2S 0x0031 [u16 cnt][u16 ms][u16 depth][u16 crc] reports the dig (the client's
// c2s_49: crc = seed ^ BarEffArea ^ BarSpeed << 1 ^ ScoopMass << 5 ^ depth << 8 ^ cnt << 12; cnt = the
// gauge's starts = the life it costs, ms = the gauge's time); the server cuts the session for a bad CRC, a
// depth past DigDepth / the map's or a find reported faster than its shortest dig time. C2S 0x0030
// [u16 cnt][u16 ms] cancels (cnt life spent). Equip: C2S 0x0051 [u64 uid][u8 slot] (a drill goes to 7).
Action drill_start(int x, int y, bool run);
Action drill_end(const WorldModel& w, int cnt, int depth, uint64_t now);
Action drill_cancel(const WorldModel& w, int cnt, uint64_t now);
Action equip(const WorldModel& w, uint64_t uid, uint8_t slot);

// Death (game_session.cpp handle_restart): C2S 0x0016 [] "Go to Town" — only while dead; the server
// answers S2C 0x17 [town][0,0][hp 30%] + the field change to the return town.
Action go_to_town();
// NPC talk (npc_talk.cpp): C2S 0x006F [u16 npc] opens the dialog (within 300 units, else S2C 113) ->
// S2C 112; C2S 0x007F [u16 npc][u8 func][u8 n][u16 x n] runs a script function of that NPC (the
// client sends the action of the option clicked, e.g. "Teleport_Start,1" = func 19 [1]); C2S 0x0075 []
// closes it.
Action npc_talk(const WorldModel& w, uint16_t npc);
Action npc_script(uint16_t npc, uint8_t func, const std::vector<uint16_t>& args, const std::string& label);
// C2S 0x0072 [u16 npc][u8 index]: the client asks for the message an option names (npc_talk.cpp handle_npc_next:
// S2C 115 with it, 116 when the NPC has none; another NPC's id drops the player).
Action npc_next(uint16_t npc, int index);
Action npc_end();
namespace npcfn {                       // NpcScript function ids (ExecuteNpcScript sub_7E85A0)
constexpr uint8_t Exit = 1, Warehouse_Start = 16, NeoWarehouse_Start = 17, Teleport_Check = 18, Teleport_Start = 19;
constexpr uint8_t Move_Check = 14, Move_Complete = 15, ExchangeShop_Start = 22, Compound_Start = 20;
// Monster quests (client table 0xFACE78, = ProjectPAULA npc_talk.cpp): the base works on QuestId (slot 0),
// _1.._4 on QuestId1..4; Once_Accept / NH_Complete only on slot 0.
constexpr uint8_t MonQuest_Check = 7, MonQuest_Accept = 8, MonQuest_Complete = 9, MonQuest_NH_Complete = 149,
                  MonQuest_Once_Accept = 185;
inline uint8_t monq_accept(int slot)   { return slot <= 0 ? MonQuest_Accept : uint8_t(241 + slot); }    // 242..245
inline uint8_t monq_complete(int slot) { return slot <= 0 ? MonQuest_Complete : uint8_t(245 + slot); }  // 246..249
}
// The monster quest's success portal (monster_quest.cpp handle_monq_return): C2S 0x0410 [u32 2] -> a running
// quest whose map has a ReturnMapID: S2C 0x0411 and the field change to it in 500 ms, else S2C 0x0412.
Action monq_return();
// Shops (shop.cpp): C2S 0x0046 [u16 shop] opens it (S2C 0xA8, state 5: moves are swallowed until the
// close); C2S 0x00A4 [u16 shop] -> S2C 0x47 its prices; buy C2S 0x0048 [u16 shop][u16 n]{u32 item,
// u16 count} (1..20 lines) -> S2C 0x49 / 0x4A / 0x4B; sell = inner 76 of the 0x039F wrapper
// [u16 76][cstr password][u16 shop][u16 n]{u64 uid, u32 item, u16 count} -> S2C 0x4E / 0x4D;
// C2S 0x004F [] closes it.
struct ShopLine { uint32_t item = 0; uint16_t count = 0; };
struct SellLine { uint64_t uid = 0; uint32_t item = 0; uint16_t count = 0; };
Action shop_open(const WorldModel& w, uint16_t shop);
Action shop_prices(uint16_t shop);
Action shop_buy(const WorldModel& w, uint16_t shop, const std::vector<ShopLine>& lines);
Action shop_sell(const WorldModel& w, uint16_t shop, const std::vector<SellLine>& lines);
Action shop_close();
// The warehouse (warehouse.cpp), open after Warehouse_Start (state 10): C2S 0x008B [u8 tab][u64 uid]
// [u32 item][u16 count] stores -> S2C 140 / 141; C2S 0x008A [] closes it (no answer).
Action wh_add(const WorldModel& w, uint8_t tab, uint64_t uid, uint16_t count);
Action wh_close();
// An exchange shop (exchange_shop.cpp), open after ExchangeShop_Start (S2C 117 + 249 [u8 n]{u16 id}, state 18):
// C2S 0x00FE [u16 id][u8 n]{u64 uid, u32 item, u16 count}[u8 times = 1] -> S2C 255 [u8 n]{bag record} / 191 [-1] /
// 256; C2S 0x0101 [] closes it (S2C 258).
Action exchange(const WorldModel& w, int id, const std::vector<SellLine>& give);
Action exchange_close();
// A compounder (compound.cpp), open after Compound_Start (S2C 117 + 188, state 13): C2S 0x00E3 (227) [u16 id][u8 type 10 =
// exactly the items asked, the KR client's][u8 spins][u8 n]{u64 uid, u32 item, u16 cnt = ReqNum x spins} -> S2C 228
// [u8 rate] (the roll) / 191; then C2S 0x00BA (186) [u8 n]{the same items} -> S2C 190 (made, bag records) / 191;
// C2S 0x00C0 (192) closes it (S2C 193).
Action compound_roll(const WorldModel& w, int id, int spins, const std::vector<SellLine>& give);
Action compound_make(const WorldModel& w, const std::vector<SellLine>& give);
Action compound_close();

} // namespace act

const Entity*     nearest_monster(const WorldModel& w, uint64_t now);
const GroundItem* nearest_item(const WorldModel& w, uint64_t now);
// An item C2S 0x57 can use: in the bag, not worn, ItemParam Options & 2.
bool usable_item(const WorldModel& w, const InvItem& it);
// A learned skill the server would accept from us (an unknown or passive one drops the session).
bool usable_skill(const WorldModel& w, const SkillEntry& sk, std::string* why = nullptr);
// The weapon in slot 1 is a gun (ItemParam SubType 54) -> its Item_GunParam2 row, else null.
struct GunInfo;
const GunInfo* equipped_gun(const WorldModel& w, uint32_t* itemId = nullptr);
// The loaded bullet stack (common slot 20) and how many it holds; 0 = empty.
int loaded_bullets(const WorldModel& w);
// A bullet stack (ItemParam Type 29) in the bag that is not loaded; 0 = none.
uint64_t spare_bullets(const WorldModel& w);
// The server's shortest time between two shots (gun.cpp FastShot), in ms.
int gun_shot_interval_ms(const GunInfo& g);
// The drill worn in slot 7 (and in the bag), null = none; its Item_DrillParam row.
struct DrillInfo;
const InvItem*   equipped_drill(const WorldModel& w);
const DrillInfo* equipped_drill_info(const WorldModel& w);
// The client's gauge (Driller sub_562A90) for a player who never lets it overheat: the bar up to the
// effective area, 600 * (200 - BarEffArea) / BarSpeed ms, then ScoopMass deeper every 120 ms.
int drill_dig_ms(const DrillInfo& d, int depth);
// The server's check of a find (drill.cpp shortest_dig_ms, sub_5E8F20): 0.7 x the same without the round-up.
int drill_shortest_ms(const DrillInfo& d, int depth, int mapDepth);
// C2S 0x31's CRC (the client's c2s_49).
uint16_t drill_crc(uint16_t seed, const DrillInfo& d, int depth, int cnt);
// The selection kinds of C2S 0x00E0 (server sub_521690 / sub_523230).
inline bool esaui_object(int k) { return k == 1 || k == 7 || k == 8; }
inline bool esaui_point(int k)  { return k == 3 || k == 6 || k == 10; }

} // namespace nj
