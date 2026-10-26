#include "app/item_sources.h"
#include "app/game_data.h"
#include "app/yml_lite.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nj {
namespace {

constexpr int kMaxDrops = 10;                   // drop.cpp kMaxDrops

// The server's two rolls over rand() (0..0x7FFF), exactly (b <= a: none):
// scaled: n * rand() / 0x8000 in [a, b) — drop.cpp roll10000 (n = 10000), drill.cpp rand_n;
double band(int a, int b, int n)
{
    if (n <= 0) return 0.0;
    const int lo = std::clamp(a, 0, n), hi = std::clamp(b, 0, n);
    if (hi <= lo) return 0.0;
    auto under = [n](int64_t h) { return (32768LL * h + n - 1) / n; };   // the rand() values whose roll is < h
    return double(under(hi) - under(lo)) / 32768.0;
}

// modulo: rand() % n in [a, b) — drop.cpp roll_sub_table (n = the sub-table's total; the low values come once more).
double mod_band(int a, int b, int n)
{
    if (n <= 0) return 0.0;
    const int lo = std::clamp(a, 0, n), hi = std::clamp(b, 0, n);
    if (hi <= lo) return 0.0;
    const int64_t full = 32768 / n, rem = 32768 % n;
    const int64_t extra = std::max<int64_t>(0, std::min<int64_t>(hi, rem) - lo);
    return double(int64_t(hi - lo) * full + extra) / 32768.0;
}

// "Item12" -> 12 below `limit`, else -1
int slot_of(const std::string& k, const char* prefix, int limit)
{
    const size_t n = std::strlen(prefix);
    if (k.size() <= n || k.compare(0, n, prefix) != 0) return -1;
    for (size_t i = n; i < k.size(); ++i) if (k[i] < '0' || k[i] > '9') return -1;
    const int idx = std::atoi(k.c_str() + n);
    return idx < limit ? idx : -1;
}

int amount_of(const GameData& gd, uint32_t item, int cnt)      // drop.cpp drop_amount: a rare item drops one
{
    const ItemInfo* ii = gd.item(item);
    return ii && ii->cls >= 2 ? 1 : std::max(1, cnt);
}

void sort_desc(std::vector<std::pair<uint32_t, double>>& v)
{
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
}

const std::vector<ItemMobSource> kNoMobs;
const std::vector<ItemDigSource> kNoDigs;

} // namespace

bool ItemSources::load(const std::string& dir, const GameData& gd, std::string& err)
{
    const std::string base = dir.empty() || dir.back() == '\\' || dir.back() == '/' ? dir : dir + "\\";
    std::vector<YRecord> recs;
    std::string e;

    // MonItemDropInfo (drop_db.hpp load_lists): items 0..9 then tables 0..4, float sum, bands of (int)(rate * 10000)
    if (gd.read_db("mon_item_drop_db", recs, e)) {
        for (const YRecord& r : recs) {
            double rate[10] = {}, trate[5] = {};
            uint32_t item[10] = {}, table[5] = {};
            int cnt[10] = {};
            for (const auto& kv : r.scalars) {
                int i;
                const char* v = kv.second.c_str();
                if ((i = slot_of(kv.first, "TableRate", 5)) >= 0) trate[i] = std::strtod(v, nullptr);
                else if ((i = slot_of(kv.first, "Table", 5)) >= 0) table[i] = uint32_t(std::strtoul(v, nullptr, 10));
                else if ((i = slot_of(kv.first, "Item", 10)) >= 0) item[i] = uint32_t(std::strtoul(v, nullptr, 10));
                else if ((i = slot_of(kv.first, "Rate", 10)) >= 0) rate[i] = std::strtod(v, nullptr);
                else if ((i = slot_of(kv.first, "Cnt", 10)) >= 0) cnt[i] = int16_t(std::atoi(v));
            }
            List l;
            std::vector<float> ir, tr;
            float sum = 0.0f;
            for (int k = 0; k < 10; ++k) {
                if (!item[k]) continue;
                Band b;
                b.id = item[k];
                b.amount = amount_of(gd, item[k], cnt[k] ? cnt[k] : 1);
                ir.push_back(float(rate[k]));
                sum = sum + ir.back();
                l.items.push_back(b);
            }
            for (int k = 0; k < 5; ++k) {
                if (!table[k]) continue;
                Band b;
                b.id = table[k];
                tr.push_back(float(trate[k]));
                sum = sum + tr.back();
                l.tables.push_back(b);
            }
            if (sum > 1.0) {
                for (float& x : ir) x = x / sum;
                for (float& x : tr) x = x / sum;
            }
            int acc = 0;
            for (size_t k = 0; k < l.items.size(); ++k) { l.items[k].lo = acc; l.items[k].hi = acc + int(double(ir[k]) * 10000.0); acc = l.items[k].hi; }
            for (size_t k = 0; k < l.tables.size(); ++k) { l.tables[k].lo = acc; l.tables[k].hi = acc + int(double(tr[k]) * 10000.0); acc = l.tables[k].hi; }
            lists_[uint32_t(r.num("ID"))] = std::move(l);
        }
    } else err += e + "  ";

    // MonItemTable_DropInfo (load_tables, retail sub_6368E0): file order through ONE 20-slot buffer that is never
    // cleared (an empty slot keeps the row before's), bands over slots 0..count-1, total (int)(sum * 10000)
    recs.clear();
    if (gd.read_db("mon_item_table_db", recs, e)) {
        Band buf[20];
        float bufRate[20] = {};
        for (const YRecord& r : recs) {
            double rate[20] = {};
            uint32_t item[20] = {};
            int cnt[20] = {};
            for (const auto& kv : r.scalars) {
                int i;
                const char* v = kv.second.c_str();
                if ((i = slot_of(kv.first, "Item", 20)) >= 0) item[i] = uint32_t(std::strtoul(v, nullptr, 10));
                else if ((i = slot_of(kv.first, "Rate", 20)) >= 0) rate[i] = std::strtod(v, nullptr);
                else if ((i = slot_of(kv.first, "Cnt", 20)) >= 0) cnt[i] = int16_t(std::atoi(v));
            }
            int count = 0;
            float sum = 0.0f;
            for (int k = 0; k < 20; ++k) {
                if (!item[k]) continue;
                buf[k].id = item[k];
                bufRate[k] = float(rate[k]);
                buf[k].amount = amount_of(gd, item[k], cnt[k] ? cnt[k] : 1);
                sum = sum + bufRate[k];
                ++count;
            }
            int total = int(double(sum) * 10000.0);
            if (sum > 1.0) {
                total = 10000;
                for (int i = 0; i < count; ++i) bufRate[i] = bufRate[i] / sum;
            }
            int acc = 0;
            for (int j = 0; j < count; ++j) { buf[j].lo = acc; buf[j].hi = acc + int(double(bufRate[j]) * 10000.0); acc = buf[j].hi; }
            SubTable& t = tables_[uint32_t(r.num("ID"))];
            for (int k = 0; k < 20; ++k) t.slot[k] = buf[k];
            t.count = count;
            t.total = total;
        }
    } else err += e + "  ";

    // every monster with a drop list: its items per kill
    for (const auto& kv : gd.mob_table()) {
        const uint32_t id = kv.first;
        const MobInfo& m = kv.second;
        if (m.itemDropIdx <= 0 || m.itemDropCnt <= 0) continue;
        auto l = lists_.find(uint32_t(m.itemDropIdx));
        if (l == lists_.end()) continue;
        mobList_[id] = uint32_t(m.itemDropIdx);
        mobRolls_[id] = m.itemDropCnt;
        std::vector<std::pair<uint32_t, double>> per;
        expect_kill(l->second, m.itemDropCnt, per);
        if (per.empty()) continue;
        for (const auto& p : per) byItemMob_[p.first].push_back({ id, p.second });
        mobDrops_[id] = std::move(per);
    }
    for (auto& kv : byItemMob_)
        std::sort(kv.second.begin(), kv.second.end(), [](const ItemMobSource& a, const ItemMobSource& b) {
            return a.perKill != b.perKill ? a.perKill > b.perKill : a.mob < b.mob;
        });

    // MapItem_Event_Add, MapItem_ETC_Table (every row of a name, in order), MapItem_All (the first row per map)
    float rateAdd[4] = {};
    recs.clear();
    if (gd.read_db("map_item_event_add_db", recs, e)) {
        for (const YRecord& r : recs) {
            const int t = int(r.num("Type", -1));
            if (t >= 0 && t < 4) rateAdd[t] = float(r.fnum("Rate_Add"));
        }
    } else err += e + "  ";
    struct Entry { uint32_t item; uint16_t rate; };
    std::unordered_map<std::string, std::vector<Entry>> named;
    recs.clear();
    if (gd.read_db("map_item_table_db", recs, e)) {
        for (const YRecord& r : recs) {
            uint32_t item[30] = {}, rate[30] = {};
            for (const auto& kv : r.scalars) {
                int i;
                if ((i = slot_of(kv.first, "Item", 30)) >= 0) item[i] = uint32_t(std::strtoul(kv.second.c_str(), nullptr, 10));
                else if ((i = slot_of(kv.first, "Rate", 30)) >= 0) rate[i] = uint32_t(std::strtoul(kv.second.c_str(), nullptr, 10));
            }
            std::vector<Entry>& list = named[r.str("Id")];
            for (int i = 0; i < 30; ++i)
                if (item[i] && uint16_t(rate[i])) list.push_back({ item[i], uint16_t(rate[i]) });
        }
    } else err += e + "  ";
    recs.clear();
    if (gd.read_db("map_item_db", recs, e)) {
        static const char* kT[5] = { "SpecialT", "EquipT", "Normal1T", "Normal2T", "Normal3T" };
        static const char* kR[5] = { "S_Rate", "E_Rate", "N1_Rate", "N2_Rate", "N3_Rate" };
        for (const YRecord& r : recs) {
            const int zone = int(r.num("Id"));
            if (zone <= 0 || mapDigs_.count(zone)) continue;
            const MapDigInfo* dig = gd.map_dig(zone);
            int total = 0, typeRate[6] = {}, itemSum[6] = {};
            std::vector<Entry> items[6];
            for (int t = 0; t < 5; ++t) {
                const std::string name = r.str(kT[t]);
                if (name.empty()) continue;
                const double rr = double(int16_t(r.num(kR[t])));
                const int16_t scaled = int16_t(int(double(rateAdd[t < 2 ? t : 2]) * rr + rr));
                auto n = named.find(name);
                if (n != named.end())
                    for (const Entry& en : n->second) { items[t].push_back(en); itemSum[t] += int16_t(en.rate); }
                total += scaled;
                typeRate[t] = scaled;
            }
            for (int i = 0; i < 30; ++i) {
                const uint32_t it = uint32_t(r.num(("Item" + std::to_string(i)).c_str()));
                const uint16_t r16 = uint16_t(r.num(("Rate" + std::to_string(i)).c_str()));
                if (!it || !r16) continue;
                items[5].push_back({ it, r16 });
                typeRate[5] += int16_t(r16);
                itemSum[5] += int16_t(r16);
                total += int16_t(r16);
            }
            mapDigs_[zone];                                 // (first row only, even when it buries nothing)
            if (total <= 0 || !dig || dig->itemPct <= 0.0f) continue;
            std::unordered_map<uint32_t, double> per;
            int acc = 0;
            for (int t = 0; t < 6; ++t) {
                const double pt = band(acc, acc + typeRate[t], total);   // the type's [acc, acc + rate) of 0..total-1
                acc += typeRate[t];
                if (pt <= 0.0 || itemSum[t] <= 0) continue;
                int prev = 0;
                for (const Entry& en : items[t]) {
                    per[en.item] += pt * band(prev, prev + en.rate, itemSum[t]);
                    prev += en.rate;
                }
            }
            std::vector<std::pair<uint32_t, double>>& out = mapDigs_[zone];
            for (const auto& p : per) if (p.second > 0.0) out.push_back(p);
            sort_desc(out);
            for (const auto& p : out) byItemDig_[p.first].push_back({ zone, p.second });
        }
    } else err += e + "  ";
    for (auto& kv : byItemDig_)
        std::sort(kv.second.begin(), kv.second.end(), [](const ItemDigSource& a, const ItemDigSource& b) {
            return a.perFind != b.perFind ? a.perFind > b.perFind : a.zone < b.zone;
        });
    return err.empty();
}

// A sub-table's fill (drop.cpp roll_sub_tables): (rolls - m) times, (rolls - loot) sub-rolls of rand() % total
// against its slot bands; each lands an item with the slots' covered share. The expected sub-rolls and where the
// loot size ends.
const ItemSources::Fill& ItemSources::fill(uint32_t table, int rolls, int m) const
{
    const uint64_t key = uint64_t(table) << 16 | uint64_t(rolls) << 8 | uint64_t(m);
    auto it = fills_.find(key);
    if (it != fills_.end()) return it->second;
    Fill f;
    double cov = 0.0;
    if (auto t = tables_.find(table); t != tables_.end()) {
        const SubTable& st = t->second;
        if (st.total <= 0) cov = st.count > 0 && st.slot[0].hi > 0 ? 1.0 : 0.0;   // (rand() % 0 is not rolled: r = 0)
        else {
            int cut = 0;
            for (int j = 0; j < st.count; ++j) {
                if (!st.slot[j].hi) break;
                cov += mod_band(cut, st.slot[j].hi, st.total);
                cut = std::max(cut, st.slot[j].hi);
            }
        }
    }
    double dist[kMaxDrops + 1] = {};
    dist[std::clamp(m, 0, kMaxDrops)] = 1.0;
    for (int i = 0; i < rolls - m; ++i) {
        double nd[kMaxDrops + 1] = {};
        for (int m2 = 0; m2 <= kMaxDrops; ++m2) {
            const double p = dist[m2];
            if (p <= 0.0) continue;
            const int s = rolls - m2;
            if (m2 >= kMaxDrops || s <= 0) { nd[m2] += p; continue; }
            f.subRolls += p * s;
            double pmf = std::pow(1.0 - cov, s);            // the hits: Binomial(s, cov)
            for (int h = 0; h <= s; ++h) {
                nd[std::min(m2 + h, kMaxDrops)] += p * pmf;
                if (h < s) pmf = cov >= 1.0 ? (h + 1 == s ? 1.0 : 0.0) : pmf * double(s - h) / double(h + 1) * cov / (1.0 - cov);
            }
        }
        std::memcpy(dist, nd, sizeof dist);
    }
    std::memcpy(f.end, dist, sizeof dist);
    return fills_.emplace(key, f).first->second;
}

// drop.cpp roll_item_drops, worked out over the loot size m (0..10): each roll lands in an item band (+1 item),
// in a sub-table band while m < rolls (the fill), or nowhere.
void ItemSources::expect_kill(const List& l, int rolls, std::vector<std::pair<uint32_t, double>>& out) const
{
    const int R = std::clamp(rolls, 0, kMaxDrops);
    std::vector<double> qi(l.items.size(), 0.0), qt(l.tables.size(), 0.0);
    int cut = 0;
    for (size_t k = 0; k < l.items.size(); ++k) {          // the first band the roll (0..9999) is under; hi 0 ends it
        if (!l.items[k].hi) break;
        qi[k] = band(cut, l.items[k].hi, 10000);
        cut = std::max(cut, l.items[k].hi);
    }
    for (size_t k = 0; k < l.tables.size(); ++k) {         // (only the rolls the items missed get here)
        if (!l.tables[k].hi) break;
        qt[k] = band(cut, l.tables[k].hi, 10000);
        cut = std::max(cut, l.tables[k].hi);
    }
    double q0 = 1.0;
    for (double q : qi) q0 -= q;
    for (double q : qt) q0 -= q;
    q0 = std::max(0.0, q0);
    std::unordered_map<uint32_t, double> acc;
    double dist[kMaxDrops + 1] = {};
    dist[0] = 1.0;
    for (int i = 0; i < R; ++i) {
        double nd[kMaxDrops + 1] = {};
        for (int m = 0; m <= kMaxDrops; ++m) {
            const double p = dist[m];
            if (p <= 0.0) continue;
            if (m >= kMaxDrops) { nd[m] += p; continue; }
            for (size_t k = 0; k < qi.size(); ++k) {
                if (qi[k] <= 0.0) continue;
                nd[m + 1] += p * qi[k];
                acc[l.items[k].id] += p * qi[k] * l.items[k].amount;
            }
            for (size_t k = 0; k < qt.size(); ++k) {
                if (qt[k] <= 0.0) continue;
                const double w = p * qt[k];
                auto t = tables_.find(l.tables[k].id);
                if (m >= R || t == tables_.end()) { nd[m] += w; continue; }
                const Fill& f = fill(l.tables[k].id, R, m);
                const SubTable& st = t->second;
                if (st.total <= 0) {
                    if (st.count > 0 && st.slot[0].hi > 0) acc[st.slot[0].id] += w * f.subRolls * st.slot[0].amount;
                } else {
                    int c2 = 0;
                    for (int j = 0; j < st.count; ++j) {
                        if (!st.slot[j].hi) break;
                        const double pj = mod_band(c2, st.slot[j].hi, st.total);
                        c2 = std::max(c2, st.slot[j].hi);
                        if (pj > 0.0) acc[st.slot[j].id] += w * f.subRolls * pj * st.slot[j].amount;
                    }
                }
                for (int m2 = 0; m2 <= kMaxDrops; ++m2) nd[m2] += w * f.end[m2];
            }
            nd[m] += p * q0;
        }
        std::memcpy(dist, nd, sizeof dist);
    }
    out.clear();
    for (const auto& kv : acc) if (kv.second > 0.0) out.push_back(kv);
    sort_desc(out);
}

void ItemSources::simulate_kill(uint32_t mob, std::mt19937& rng, std::vector<std::pair<uint32_t, int>>& loot) const
{
    loot.clear();
    auto ml = mobList_.find(mob);
    if (ml == mobList_.end()) return;
    const List& l = lists_.at(ml->second);
    const int R = mobRolls_.at(mob);
    auto rand15 = [&]() { return int(rng() & 0x7FFF); };              // rand(): 0..0x7FFF
    for (int i = 0; i < R; ++i) {
        const int roll = 10000 * rand15() / 0x8000;                   // drop.cpp roll10000
        bool hit = false;
        for (const Band& b : l.items) {
            if (!b.hi) break;
            if (roll < b.hi) { loot.push_back({ b.id, b.amount }); hit = true; break; }
        }
        if (int(loot.size()) >= kMaxDrops) break;
        if (hit) continue;
        const int n = R - int(loot.size());                           // roll_sub_tables
        for (int k = 0; k < n; ++k) {
            for (const Band& t : l.tables) {
                if (!t.hi) break;
                if (roll < t.hi) {
                    if (auto st = tables_.find(t.id); st != tables_.end()) {
                        const int n2 = R - int(loot.size());          // roll_sub_table
                        for (int j = 0; j < n2; ++j) {
                            const int r = st->second.total > 0 ? rand15() % st->second.total : 0;
                            for (int s = 0; s < st->second.count; ++s) {
                                const Band& e = st->second.slot[s];
                                if (!e.hi) break;
                                if (r < e.hi) { loot.push_back({ e.id, e.amount }); break; }
                            }
                            if (int(loot.size()) >= kMaxDrops) break;
                        }
                    }
                    break;
                }
            }
            if (int(loot.size()) >= kMaxDrops) break;
        }
    }
}

const std::vector<ItemMobSource>& ItemSources::mobs(uint32_t item) const
{
    auto it = byItemMob_.find(item);
    return it == byItemMob_.end() ? kNoMobs : it->second;
}

const std::vector<ItemDigSource>& ItemSources::digs(uint32_t item) const
{
    auto it = byItemDig_.find(item);
    return it == byItemDig_.end() ? kNoDigs : it->second;
}

double ItemSources::per_kill(uint32_t mob, uint32_t item) const
{
    auto it = mobDrops_.find(mob);
    if (it == mobDrops_.end()) return 0.0;
    for (const auto& p : it->second) if (p.first == item) return p.second;
    return 0.0;
}

double ItemSources::per_find(int zone, uint32_t item) const
{
    auto it = mapDigs_.find(zone);
    if (it == mapDigs_.end()) return 0.0;
    for (const auto& p : it->second) if (p.first == item) return p.second;
    return 0.0;
}

const std::vector<std::pair<uint32_t, double>>* ItemSources::drops(uint32_t mob) const
{
    auto it = mobDrops_.find(mob);
    return it == mobDrops_.end() ? nullptr : &it->second;
}

const std::vector<std::pair<uint32_t, double>>* ItemSources::buried(int zone) const
{
    auto it = mapDigs_.find(zone);
    return it == mapDigs_.end() || it->second.empty() ? nullptr : &it->second;
}

} // namespace nj
