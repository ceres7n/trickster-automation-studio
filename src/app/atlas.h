#pragma once
// The atlas: what the bot needs to get around the world — every map's portals and points (its .md3),
// the NPCs, shops, warehouses and NPC teleports of each map (ProjectPAULA db: maps_db, npc_db,
// shop_db, shop_item_db, teleport_db, npc_msg_db), a path finder inside a map (its .md3 layer 1) and
// a route planner between maps. Server rules (ProjectPAULA, = retail):
//   portal    - a .md3 region of type 1 {id, type, param = the map it leads to, x1, y1, x2, y2}; C2S
//               0x1D4 [dst][type] walks there with the portal armed and each zone tick takes a player
//               whose position AND destination lie in the rectangle grown by 24 (game_session.cpp
//               portal_tick). Arrival: the type-1 point of the new map whose Param is the old map and
//               whose Id is the region id (& 0xFF), else a random type-1 point (take_portal).
//   teleport  - an NPC with a TeleportId (npc_db): its teleport_db row's Zone0..6 (the non-zero ones,
//               in order) are the slots of NpcScript Teleport_Start (func 19, arg = the slot); MinLevel,
//               the slot's Cost paid; arrival on a random type-7 point (npc_talk.cpp).
//   NPC move  - an NPC with a NpcMoveId (npc_move_db): Move_Complete (func 15) with its items in the bag, its
//               level / FType; arrival on the zone's type-9 point DestId (the Hologram Chambers' way out).
//   port item - a Type 15 Effect 28 item whose random_port_db row has one zone: C2S 0x57 sends there (not from
//               an Option 128 map); used up, so a route takes one only when nothing else gets there.
//   Port AD   - the Portable Port AD (Type 15, Effect 20, Options 0x8000): C2S 0x0108 [u32 ZoneIndex] to any zone of
//               teleport_adv_db (the region gates and towns), not from an Option 128 map, one item each: a shortcut
//               taken whenever it saves more than a few seconds (the user's call: "use it freely").
//   NPCs      - a talk NPC stands on the type-4 point of its PosId, a shop on the type-5 point; none ->
//               (600,600) (game_server.cpp spawn_world_entities / spawn_shops).
//   warehouse - an NPC whose messages hold the script Warehouse_Start (func 16); the bank NeoWarehouse_Start.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace nj {

class GameData;
struct MapGrid;

struct AtlasPoint {
    uint32_t id = 0, type = 0, param = 0;
    int      x = 0, y = 0;
};

struct AtlasPortal {
    uint32_t id = 0;                // the region id (the arrival point's Id is its low byte)
    int      toZone = 0;
    int      x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int cx() const { return (x1 + x2) / 2; }
    int cy() const { return (y1 + y2) / 2; }
};

struct TeleportSlot {
    int slot = 0;                   // Teleport_Start's argument (a move: its index in the NPC's slots)
    int zone = 0;
    int cost = 0;
    // An NPC move (npc_move_db, the NPC's NpcMoveId; npc_talk.cpp npc_move_check / npc_move_complete): Move_Complete
    // (func 15) with its dialog's arguments; the items in the bag (taken when `takes`), the level within
    // minLevel..maxLevel (both 0 = any), our FType (-1 = any), the ChrType (0 = any); arrival on the zone's type-9
    // point DestId 2 s later.
    bool move = false;
    // A script call other than Teleport_Start (0): Move_Complete 15, Random_Move 121 (npc_talk.cpp npc_random_move:
    // one of its five zone arguments at random — the Phantom School's classrooms by Peach of mischievous); `chance` =
    // the share of the arguments that are this zone
    uint8_t func = 0;
    bool random = false;
    double chance = 1.0;
    std::vector<uint16_t> args;
    std::vector<std::pair<uint32_t, int>> items;
    int  minLevel = 0, maxLevel = 0, ftype = -1, chrType = 0, destId = 0;
    bool takes = false;
};

// An exchange shop's exchange (exchange_shop_db, exchange_shop.cpp): ReqIDs x ReqNum + Fee -> one of each ResID.
struct ExchangeDef {
    int id = 0;
    int fee = 0;
    std::vector<uint32_t> res;
    std::vector<std::pair<uint32_t, int>> req;
};

// A compound (compound_db: Compound_Potion type 0 / Compound_Rare 1 / Compound_Throw 2; ProjectPAULA compound.cpp): the
// ReqIDs x ReqNum x spins and Fee x spins (x 0.9 from 10 spins) give the ResIDs for each successful spin (Probability %
// + a DA bonus), the WasteItem for each failed one. A rare compound takes one spin at a time.
struct CompoundDef {
    int id = 0, type = 0;
    int probability = 0, fee = 0;
    uint32_t waste = 0;
    std::vector<uint32_t> res;
    std::vector<std::pair<uint32_t, int>> req;
};

// A script action of an NPC's dialog (npc_msg_db: "label\Name,arg,arg"): the function id the client looks the
// name up to (sub_77C720) and its arguments as the client sends them (sub_C033A0: n, q<n> = 128 + n,
// m<n> = 256 + n, s<n> = 512 + n; at most 10).
struct NpcScriptCall {
    uint8_t               func = 0;
    std::vector<uint16_t> args;
    int                   msgNo = 0;        // the message it is in
};

// A dialog as the client builds it from each message's text (sub_C04220): the options, each a message number (the
// client asks for it: C2S 0x72) or a script call (C2S 0x7F [npc][func][n][args]). quest_graph.h walks it.
struct DialogOpt {
    int                   link = -1;        // a message number (q<n> / m<n> / s<n> = 128 / 256 / 512 + n); -1 = a call
    uint8_t               func = 0;         // a call: its function id (client table 0xFACE78; 0 = a name it does not know)
    std::vector<uint16_t> args;
};
struct DialogMsg {
    std::string            text;            // the whole message as the server sends it (UTF-8: the text, then the options)
    std::vector<DialogOpt> opts;
};
struct Dialog {
    std::map<int, DialogMsg> msgs;          // message number -> it
    // The message whose text is `text` (a S2C 112 / 115's, UTF-8): the same text, else the same options and the
    // longest common start of the text. -1 = none.
    int identify(const std::string& text) const;
};

struct AtlasNpc {
    enum Kind : uint8_t { Talk = 2, Shop = 5 };
    uint8_t     kind = Talk;
    int         zone = 0;
    uint16_t    species = 0;        // its CharacterInfo type (the appear's species): npc_db Type / shop NpcType
    std::string name;
    int         posId = 0;
    int         x = 600, y = 600;   // its point (none -> the server's (600,600))
    bool        hasPoint = false;
    // a shop
    std::string itemTable;
    std::vector<uint32_t> items;    // what it sells (shop_item_db)
    std::vector<float> itemRates;   // each one's PurchaseRate (> 0 replaces the shop's)
    float       purchaseRate = 1.0f;   // the shop's (<= 0 -> 1.0)
    // a talk NPC
    std::string msgTable;
    bool        warehouse = false, bank = false;
    std::vector<uint16_t> warehouseArgs;   // the arguments of its Warehouse_Start (e.g. 1,2,3)
    int         teleportId = 0, teleportMinLevel = 0;
    std::vector<TeleportSlot> teleports;    // its Teleport_Start slots, then its NPC move (if any)
    int         moveId = 0;                 // npc_db NpcMoveId
    std::vector<int> exchanges;             // its ExcShopTable's exchanges (ExchangeShop_Start, func 22)
    std::vector<uint16_t> exchangeArgs;     // the arguments of its dialog's ExchangeShop_Start
    std::vector<int> compounds;             // its CompounderTable's compounds shown to the client (Compound_Start, func 20)
    std::vector<uint16_t> compoundArgs;     // the arguments of its dialog's Compound_Start
    int         quests[5] = {};             // npc_db QuestId, QuestId1..4: the quest of each script slot
    std::vector<NpcScriptCall> monq;        // its dialog's MonQuest_Accept / _Once_Accept / _Complete / _NH_Complete
    // The npc_db columns its dialog's checks read (ProjectPAULA npc_talk.cpp): PreCheckType / Arg / Yes / No pick
    // the first message; CheckQuestState(_1/_2) read CheckQuestId(1/2) + CheckQuestNum(1/2), CheckMonQuestState
    // CheckMonQuestId / Num, ActiveQuestID_0..2 ActiveQuestID0..2, CheckHaveItem CheckItemId0..1.
    int         preType = 0, preArg = 0, preYes = 0, preNo = 0;
    int         checkQuest[3] = {}, checkNum[3] = {};
    int         checkMonQuest = 0, checkMonNum = 0;
    int         activeQuest[3] = {};
    uint32_t    checkItem[2] = {};
    uint32_t    giveItem = 0, takeItem = 0; // npc_db GiveItemID / TakeItemID: what its Give_Item / Take_Item hand over
    uint32_t    jobFlag1 = 0, jobFlag2 = 0; // npc_db JobTypeFlag1 / 2: Check_Job's jobs (1..16 bit job-1, 17..33 bit job-17)
    const Dialog* dialog = nullptr;         // its MsgTable's messages (null = none)
    // that function's call in its dialog (the first one), null = none
    const NpcScriptCall* call(uint8_t func) const
    {
        for (const NpcScriptCall& c : monq) if (c.func == func) return &c;
        return nullptr;
    }
    int quest_slot(int qid) const               // -1 = it does not give that quest
    {
        for (int i = 0; i < 5; ++i) if (qid && quests[i] == qid) return i;
        return -1;
    }

    // "zone:species:posId" — the profile's way to name it
    std::string key() const;
};

struct AtlasMap {
    int         id = 0;
    std::string name, file;
    int         type = 0, cls = 0, group = 0, town = 0, option = 0;
    float       shopPriceRate = 1.0f;    // MapInfoEx ShopPriceRate (<= 0 -> 1.0)
    bool        isTown() const { return (type == 1 || type == 2) && cls != 3; }   // the return-town rule
    bool        noPort() const { return (option & 128) != 0; }   // no item port from here (item_use.cpp zone_no_port)
    std::vector<AtlasPortal> portals;
    std::vector<AtlasPoint>  points;     // types 1 (portal arrivals), 2 (default arrivals), 4, 5, 7, 9
    // the monsters' regeneration points (ProjectPAULA regen.cpp, Zone::BuildPoints): a Mon_ row's Region R > 0
    // -> a random type-6 point whose id is R; Region 0 (or a region without points) -> a random type-3 point
    std::vector<AtlasPoint>  regen;
    std::vector<int>         npcs;       // into Atlas::npcs()
    std::vector<int>         teleporters;   // the npcs with teleport slots: the map's exits after its portals
    bool        hasFile = false;         // its .md3 was read
};

// One step between two maps.
struct RouteHop {
    // Item: a port item of the bag used where we stand (random_port_db, one zone: item_use.cpp use_port_item) —
    // only when no other way gets there (it is used up)
    enum Kind { Portal, Teleport, Item } kind = Portal;
    int    fromZone = 0, toZone = 0;
    int    portal = -1;             // Portal: index into the map's portals
    int    npc = -1;                // Teleport: index into Atlas::npcs()
    int    slot = 0, cost = 0;      // Teleport (a slot with `move`: the NPC's Move_Complete)
    bool   move = false;            // Teleport: an NPC move
    uint32_t item = 0;              // Item: the port item
    int    advIndex = -1;           // Item: a Portable Port AD's ZoneIndex (C2S 0x0108 [u32 index]); -1 = a port item
    int    x = 0, y = 0;            // where it is taken: the portal's centre / the NPC
    int    arriveX = 0, arriveY = 0;   // where it should put us (a guess when the arrival is random)
    bool   randomArrival = false;
    double secs = 0;                // estimated: the walk to it + the map change
};

struct Route {
    bool                  ok = false;
    std::vector<RouteHop> hops;
    double                secs = 0;       // everything: the walks, the changes, the teleports' price
    double                lastWalkSecs = 0;   // the walk in the last map to the goal point
    int                   gelder = 0;
    std::string           why;      // !ok: what is missing
};

struct RouteOptions {
    bool    teleports = true;       // NPC teleports allowed
    int     maxGelder = 3000;       // the most one teleport may cost
    int     level = 0;
    int64_t money = -1;             // -1 = unknown (no money check)
    int     speed = 160;            // units / s
    int     goalX = -1, goalY = -1; // a point of the goal map to reach (-1 = anywhere in it)
    std::set<uint64_t> banned;      // Atlas::hop_key of the hops to leave out (they failed)
    // the bag (item -> count) for the hops that want an item: NPC moves and port items (not known = none of them)
    bool    bagKnown = false;
    std::map<uint32_t, int> bag;
    bool    portAd = true;          // a Portable Port AD of the bag may cut the way
    int     ftype = -1;             // our FType (-1 = not known: an NPC move's FType is not checked)
};

class Atlas {
public:
    void build(const GameData& gd, const std::string& dbDir, const std::string& mapDir);
    const std::string& load_report() const { return report_; }

    const AtlasMap* map(int zone) const;
    const std::unordered_map<int, AtlasMap>& maps() const { return maps_; }
    const std::vector<AtlasNpc>& npcs() const { return npcs_; }
    const AtlasNpc* npc(int index) const { return index >= 0 && index < (int)npcs_.size() ? &npcs_[size_t(index)] : nullptr; }
    int  find_npc(const std::string& key) const;          // -1 = none
    std::vector<int> quest_givers(int qid) const;         // the NPCs with that quest in a QuestId slot
    // Where the Mon_ rows of `zone` with these Regions regenerate (regen.cpp): R > 0 -> its type-6 points of id R
    // (none -> as 0), 0 -> the type-3 points; none at all -> the first type-1 point.
    std::vector<std::pair<int, int>> regen_points(int zone, const std::vector<int>& regions) const;
    // A dialog message's MonQuest_Accept / _Once_Accept / _Complete / _NH_Complete actions (what build() keeps).
    static void parse_monq_calls(const std::string& message, int msgNo, std::vector<NpcScriptCall>& out);
    // Every option of a dialog message (the text, then '\'-separated label / action pairs).
    static std::vector<DialogOpt> parse_options(const std::string& message);
    // A script function's id from its name (the client's table 0xFACE78: 1 Exit .. 275), 0 = unknown; its name.
    static uint8_t     func_id(const std::string& name);
    static const char* func_name(uint8_t id);
    std::vector<int> shops() const;                       // every shop, by map then name
    std::vector<int> warehouses(bool bank = false) const; // every warehouse (bank) keeper
    std::string map_label(int zone) const;                // "Coral Town - Blooming Cora (148)"

    // A walk inside one map on its grid (8-way A*, no corner cutting) from (sx, sy) to within `within`
    // units of (tx, ty), as legs: each one a straight open line (MapGrid::line_clear) of at most `maxLeg`
    // units. false = no way (why says it).
    static bool path(const MapGrid& g, int sx, int sy, int tx, int ty, int within, int maxLeg,
                     std::vector<std::pair<int, int>>& legs, std::string* why = nullptr);
    // Every cell's walking cost from (x, y) on the grid (10 per 16 units, INT32_MAX = no way there).
    static void walk_field(const MapGrid& g, int x, int y, std::vector<int32_t>& field);
    // The walk to (x, y) on such a field, in units: its cell, else (a point inside a wall, where a monster may
    // stand) the cheapest open cell around it within `ring` cells plus the step in. -1 = no way.
    static double field_walk(const MapGrid& g, const std::vector<int32_t>& field, int x, int y, int ring = 2);
    // The same into a portal: the nearest (by the walk) open cell of its rectangle grown by 16 — the
    // last leg ends inside, so it can be the C2S 0x1D4. false + noCell = the rectangle has no open cell.
    static bool path_rect(const MapGrid& g, int sx, int sy, const AtlasPortal& p, int maxLeg,
                          std::vector<std::pair<int, int>>& legs, std::string* why = nullptr, bool* noCell = nullptr);
    // The quickest route from (fromZone, fx, fy) to toZone (and o.goalX / goalY in it) over portals and
    // NPC teleports: an A* over the arrival points of the maps, each walk measured on the map's grid.
    // firstExit >= 0 forces the first hop: that exit of the first map (its portals, then each slot of its
    // teleporters, in order).
    Route route(const GameData& gd, int fromZone, int fx, int fy, int toZone, const RouteOptions& o, int firstExit = -1) const;
    // The best route through each way out of the first map, the quickest first (at most `max`).
    std::vector<Route> alternatives(const GameData& gd, int fromZone, int fx, int fy, int toZone, const RouteOptions& o,
                                    size_t max) const;
    // The key of a hop for RouteOptions::banned.
    static uint64_t hop_key(const RouteHop& h);
    // Is there any way from one map to another with these options (portals, teleports, NPC moves, the bag's port
    // items)? A breadth-first search over the maps, no walking distances: quick enough to ask for many items.
    bool zone_reachable(int fromZone, int toZone, const RouteOptions& o) const;
    // The items that open a way: every NPC move's items and every port item (one zone).
    const std::vector<uint32_t>& unlock_items() const { return unlocks_; }

    // The port items that send to one zone (random_port_db rows with a single destination): item -> zone.
    const std::map<uint32_t, int>& port_items() const { return portItems_; }
    int  port_zone(uint32_t item) const { auto it = portItems_.find(item); return it == portItems_.end() ? 0 : it->second; }
    const ExchangeDef* exchange(int id) const { auto it = exchanges_.find(id); return it == exchanges_.end() ? nullptr : &it->second; }
    const CompoundDef* compound(int id) const { auto it = compounds_.find(id); return it == compounds_.end() ? nullptr : &it->second; }
    // The shops that sell an item (into npcs()) and one's price for it (shop.cpp sub_720160: (int)(the map's ShopPriceRate x
    // the item's Value x the rate + 0.5), the item row's PurchaseRate > 0, else the shop's); -1 = it does not sell it.
    const std::vector<int>& shops_selling(uint32_t item) const;
    int shop_price(const GameData& gd, const AtlasNpc& shop, uint32_t item) const;
    // The Portable Port AD's destinations (teleport_adv_db: ZoneIndex -> zone) and the bag's Port AD item (0 = none).
    const std::vector<std::pair<int, int>>& adv_ports() const { return adv_; }
    uint32_t port_ad_item(const RouteOptions& o) const;

private:
    // One A* (route()): `consume` = the hops that use an item up (port items, NPC moves that take theirs) too.
    Route route_pass(const GameData& gd, int fromZone, int fx, int fy, int toZone, const RouteOptions& o, int firstExit,
                     bool consume) const;
    void read_md3(AtlasMap& m, const std::string& mapDir);
    // Walking distances (units) from (x, y) to each exit of the map: its portals (their rectangle grown
    // by 16), then its teleporters (within 150 of them); < 0 = no way. Cached by (map, x, y).
    const std::vector<float>& exit_dists(const GameData& gd, const AtlasMap& m, int x, int y) const;
    // Walking distance from (x, y) to within 128 of the goal point of that map (a grid field from the open
    // cells around the goal, kept for the last goal: an NPC on a walled-in pocket is still reached); < 0 = no way.
    float goal_dist(const GameData& gd, int zone, int gx, int gy, int x, int y) const;
    std::unordered_map<int, AtlasMap> maps_;
    std::vector<AtlasNpc>             npcs_;
    std::unordered_map<std::string, Dialog> dialogs_;    // MsgTable -> its messages (AtlasNpc::dialog points in)
    std::map<uint32_t, int>           portItems_;
    std::vector<uint32_t>             unlocks_;
    std::vector<std::pair<int, int>>  adv_;      // Port AD: ZoneIndex -> zone
    const GameData*                   gd_ = nullptr;
    std::map<int, ExchangeDef>        exchanges_;
    std::map<int, CompoundDef>        compounds_;
    std::unordered_map<uint32_t, std::vector<int>> sellers_;   // item -> the shops selling it
    std::string                       report_;
    mutable std::unordered_map<uint64_t, std::vector<float>> exitCache_;
    mutable std::vector<int32_t> goalField_;
    mutable int goalZone_ = -1, goalX_ = -1, goalY_ = -1;
};

} // namespace nj
