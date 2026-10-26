#pragma once
// Where an item comes from, from the server's own data and rolls (ProjectPAULA = retail):
//   Monster drops (drop.cpp roll_item_drops / roll_sub_tables / roll_sub_table; world/drop_db.hpp loaders):
//     a kill makes ItemDropCnt rolls of 0..9999 against the monster's MonItemDropInfo bands (items first, then
//     sub-tables; a band's width (int)(rate * 10000), the rates divided by their float sum when it is over 1);
//     a roll in an item band drops it (a rare one: 1, else its Cnt); a roll in a sub-table band fills the loot up
//     to ItemDropCnt from that MonItemTable_DropInfo row (rand() % total against its slot bands); 10 at most.
//     The expected count of every item per kill is worked out exactly over the loot sizes (expect_kill()), with
//     the rolls' own odds over rand() 0..0x7FFF (10000 * rand() / 0x8000; rand() % total favours the low values).
//   Buried items (drill.cpp map_item_pop; world/drill_db.hpp MapItemDb): a find's item is rolled only then —
//     its type by the map's type rates (S / E / N1..N3 tables x (1 + MapItem_Event_Add Rate_Add), and its own
//     Item0..29 as type 5), then the item by the rates within the type: per_find() is that share.
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nj {

class GameData;

struct ItemMobSource { uint32_t mob = 0; double perKill = 0.0; };   // the expected count per kill
struct ItemDigSource { int zone = 0; double perFind = 0.0; };       // the share of a find in that map

class ItemSources {
public:
    // dir = ProjectPAULA\db: mon_item_drop_db / mon_item_table_db (+ mob_db / item_db through gd),
    // map_item_db / map_item_table_db / map_item_event_add_db (the maps with buried items: gd.map_dig).
    bool load(const std::string& dir, const GameData& gd, std::string& err);

    // the monsters that drop it, the most per kill first / the maps where it is buried, the most per find first
    const std::vector<ItemMobSource>& mobs(uint32_t item) const;
    const std::vector<ItemDigSource>& digs(uint32_t item) const;
    double per_kill(uint32_t mob, uint32_t item) const;
    double per_find(int zone, uint32_t item) const;
    // a monster's drops per kill (null = none) / a map's buried items per find (null = none)
    const std::vector<std::pair<uint32_t, double>>* drops(uint32_t mob) const;
    const std::vector<std::pair<uint32_t, double>>* buried(int zone) const;

    // One kill of `mob` rolled as the server rolls it (the selftest checks expect_kill() against many of these).
    void simulate_kill(uint32_t mob, std::mt19937& rng, std::vector<std::pair<uint32_t, int>>& loot) const;

    size_t lists() const { return lists_.size(); }
    size_t tables() const { return tables_.size(); }
    size_t dig_maps() const { return mapDigs_.size(); }

    // the loaders' bands (drop_db.hpp), public for the selftest's simulation
    struct Band { uint32_t id = 0; int lo = 0, hi = 0; int amount = 1; };      // item band (or a table: id = table)
    struct List { std::vector<Band> items, tables; };
    struct SubTable { Band slot[20]; int count = 0, total = 0; };

private:
    void expect_kill(const List& l, int rolls, std::vector<std::pair<uint32_t, double>>& out) const;
    struct Fill { double subRolls = 0.0; double end[11] = {}; };              // a sub-table's fill from m loot
    const Fill& fill(uint32_t table, int rolls, int m) const;

    std::unordered_map<uint32_t, List> lists_;                                   // MonItemDropInfo ID
    std::unordered_map<uint32_t, SubTable> tables_;                              // MonItemTable_DropInfo ID
    std::unordered_map<uint32_t, uint32_t> mobList_;                             // mob -> its list
    std::unordered_map<uint32_t, int> mobRolls_;                                 // mob -> ItemDropCnt
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, double>>> mobDrops_;   // mob -> (item, per kill)
    std::unordered_map<uint32_t, std::vector<ItemMobSource>> byItemMob_;
    std::unordered_map<int, std::vector<std::pair<uint32_t, double>>> mapDigs_;       // zone -> (item, per find)
    std::unordered_map<uint32_t, std::vector<ItemDigSource>> byItemDig_;
    mutable std::unordered_map<uint64_t, Fill> fills_;
};

} // namespace nj
