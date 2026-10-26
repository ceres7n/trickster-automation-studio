// The quest runner (quest_runner.h): the Key Quest's and the Story's executor. Server rules (ProjectPAULA quest.cpp /
// monster_quest.cpp / npc_talk.cpp, = retail):
//   - A normal quest: at most 6 on at once (uspm_quest_take answers 1002 past it: S2C 116); taken = S2C 132 [qid][run]
//     then the take's message (115); handed in = S2C 150 (the bag records: the items taken, the rewards) then 115 and
//     133; its result = the row with the largest Count up to the run. A refusal is a message (the take: 2 no free
//     entry, 3 the level; the hand-in: 2 the bag full / too heavy) or 116.
//   - A scenario quest (the Story): the same with S_Quest_* and S2C 829 / 831 / 830, its own 6 on, MaxCount 1.
//   - A monster quest: one at a time; 210 taken, 217 / 218 the kills (only the monster's owner's count), 219 handed in;
//     back through the success portal (C2S 0x410) while its timer runs. Handed in before 218 = the server drops us.
//   - Give_Item: the NPC's GiveItemID, one, whenever there is room (S2C 0x024A + the yes message).
//   - An exchange shop (exchange_shop.cpp): ExchangeShop_Start (S2C 117 + 249, the player's state 18 swallows moves),
//     C2S 254 with the exchange's ReqIDs (each one bag stack with at least ReqNum) -> 255 the records / 191 / 256; C2S
//     257 closes it in any state (258).
//   - A map only an item opens (quest_graph.h QCond::Enter): the Navigator uses the port item / the NPC move; a port
//     item is used up, so a quest whose NPC stands in there is taken with its items already in the bag (one trip).
//   - The dialogs order the quests (quest_graph.h). The bot walks them (DialogWalk) and keeps what the answers tell of
//     each quest's history in the book (the run of a take = the history + 1; a Quest_Check's 4th argument = done
//     MaxCount times; a CheckQuestState's branch ...), so a quest done before is not done again.
//   The plan (made again every 5 s and whenever the book changes): the action's own quests (plan_own), then the quests
//   and NPCs that give an item nothing drops or buries. Each tick, in order: a monster quest of the plan on -> its hunt
//   (the Attack on its monster in its map) or its hand-in; a quest on whose items for every run left are in the bag (or
//   that can get no more of them) -> hand it in; a quest ready -> take it (a scenario quest at once: a map / an NPC may
//   wait for it; another with its items in the bag, or ahead of them while there is room: its history is learned and a
//   dig finds its items more often); an item an NPC hands out -> ask for it; else the gather (the Item Hunt's planner on
//   every item the plan still needs that a monster drops or a map buries).
#include "app/bot/quest_runner.h"
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/item_sources.h"
#include "app/text_util.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace nj {
namespace {

constexpr uint64_t kReplanMs = 5000;
constexpr uint64_t kAnswerMs = 4000;
constexpr uint64_t kHoldMs = 30000;             // a refused take / hand-in / a failed walk: that quest waits this long
constexpr uint64_t kLongHoldMs = 600000;        //   three in a row: 10 min
constexpr int      kMaxRefusals = 3;
constexpr int      kMaxOn = 6;                  // normal / scenario quests on at once, each (uspm_quest_take)
constexpr int      kAheadNoItems = 3;           // normal quests taken before their items are in the bag
constexpr uint64_t kPortalMs = 3000;
constexpr uint64_t kLostMs = 60000;

std::string clock_text(uint32_t ms) { return fmt("%u:%02u", ms / 60000, (ms / 1000) % 60); }

const std::map<int, QuestHist>& model_hist(const WorldModel& w, QKey q)
{
    return q.scen ? w.quests.shist : q.mon ? w.quests.monHist : w.quests.hist;
}

} // namespace

// ── the book ──────────────────────────────────────────────────────────────────
void QuestBook::set(QKey q, int lo, int hi)
{
    QuestHist& x = h[q.v()];
    if (x.lo == lo && x.hi == hi) return;
    x.lo = lo;
    x.hi = hi;
    ++version_;
}

void QuestBook::merge_model(const WorldModel& w)
{
    for (int k = 0; k < 3; ++k)
        for (const auto& kv : k == 2 ? w.quests.shist : k == 1 ? w.quests.monHist : w.quests.hist) {
            if (kv.second.hi < 0 || kv.second.hi != kv.second.lo) continue;
            set(QKey{ k == 1, kv.first, k == 2 }, kv.second.lo, kv.second.hi);
        }
}

void QuestBook::learn(const std::vector<QCond>& cs, std::vector<std::string>* notes, const WorldModel* w)
{
    for (const QCond& c : cs) {
        if (c.kind != QCond::HistGE && c.kind != QCond::HistLT) continue;
        if (w) {                                            // the server's exact word wins
            const std::map<int, QuestHist>& mh = model_hist(*w, c.q);
            auto m = mh.find(c.q.id);
            if (m != mh.end() && m->second.hi >= 0 && m->second.lo == m->second.hi) continue;
        }
        QuestHist x;
        auto it = h.find(c.q.v());
        if (it != h.end()) x = it->second;
        const QuestHist was = x;
        if (c.kind == QCond::HistGE) {
            x.lo = std::max(x.lo, c.n);
            if (x.hi >= 0 && x.hi < x.lo) x.hi = -1;        // (the book was wrong: the server says more)
        } else {
            x.hi = x.hi < 0 ? c.n - 1 : std::min(x.hi, c.n - 1);
            if (x.lo > x.hi) x.lo = x.hi;
        }
        if (x.lo == was.lo && x.hi == was.hi) continue;
        set(c.q, x.lo, x.hi);
        if (notes)
            notes->push_back(c.kind == QCond::HistGE ? fmt("learned %c%d done %d+ times", c.q.tag(), c.q.id, c.n)
                                                     : fmt("learned %c%d done fewer than %d times", c.q.tag(), c.q.id, c.n));
    }
}

std::string QuestBook::text() const
{
    std::ostringstream o;
    for (const auto& kv : h) {
        const QKey q = QKey::from(kv.first);
        o << q.tag() << q.id << ':' << kv.second.lo << ':' << kv.second.hi << '\n';
    }
    return o.str();
}

void QuestBook::load(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 4 || (line[0] != 'q' && line[0] != 'm' && line[0] != 's')) continue;
        int id = 0, lo = 0, hi = -1;
        if (sscanf(line.c_str() + 1, "%d:%d:%d", &id, &lo, &hi) != 3 || id <= 0) continue;
        QuestHist& x = h[QKey{ line[0] == 'm', id, line[0] == 's' }.v()];
        x.lo = std::max(x.lo, lo);
        if (hi >= 0) x.hi = x.hi < 0 ? hi : std::min(x.hi, hi);
        if (x.hi >= 0 && x.lo > x.hi) x.lo = x.hi;
    }
    ++version_;
}

// ── the view ──────────────────────────────────────────────────────────────────
QuestHist QuestRunner::View::hist(QKey q) const
{
    if (q.scen) {                                           // on: its run is the history + 1
        auto on = w_.quests.sactive.find(q.id);
        if (on != w_.quests.sactive.end() && on->second >= 1) return QuestHist{ on->second - 1, on->second - 1 };
    } else if (!q.mon) {
        auto on = w_.quests.active.find(q.id);
        if (on != w_.quests.active.end() && on->second >= 1) return QuestHist{ on->second - 1, on->second - 1 };
    } else if (w_.monq.has && w_.monq.qid == q.id && w_.monq.run >= 1) {
        return QuestHist{ w_.monq.run - 1, w_.monq.run - 1 };
    }
    QuestHist h;
    auto b = b_.h.find(q.v());
    if (b != b_.h.end()) h = b->second;
    const auto& m = model_hist(w_, q);
    auto it = m.find(q.id);
    if (it != m.end()) {
        if (it->second.hi >= 0 && it->second.hi == it->second.lo) return it->second;   // this session's packets
        h.lo = std::max(h.lo, it->second.lo);
    }
    return h;
}

int  QuestRunner::View::hist_lo(QKey q) const { return hist(q).lo; }
int  QuestRunner::View::hist_hi(QKey q) const { return hist(q).hi; }
bool QuestRunner::View::active(QKey q) const
{
    if (q.scen) return w_.quests.sactive.count(q.id) != 0;
    return q.mon ? (w_.monq.has && w_.monq.qid == q.id) : w_.quests.active.count(q.id) != 0;
}
bool QuestRunner::View::items(QKey q) const
{
    if (q.mon) return active(q) && w_.monq.success;
    const GameData* gd = w_.data();
    const QuestInfo* qi = gd ? (q.scen ? gd->scenario_quest(q.id) : gd->quest(q.id)) : nullptr;
    return qi && runs_in_bag(w_, *qi) >= 1;
}
bool QuestRunner::View::has_item(uint32_t item) const { return w_.count_item(item) > 0; }
int  QuestRunner::View::level() const { return w_.self.level; }
int  QuestRunner::View::ftype() const { return w_.self.fType; }
bool QuestRunner::View::monq_busy(QKey q) const { return w_.monq.has && w_.monq.qid != q.id; }
bool QuestRunner::View::gatherable(uint32_t item) const
{
    const GameData* gd = w_.data();
    if (!gd) return false;
    const ItemSources& is = gd->item_sources();
    return !is.mobs(item).empty() || !is.digs(item).empty();
}

int QuestRunner::runs_in_bag(const WorldModel& w, const QuestInfo& q)
{
    if (q.src.empty()) return 99;                           // nothing to bring
    if (q.options == 2) {                                   // a tier: any one of them
        for (const QuestNeed& n : q.src) if (n.cnt > 0 && w.count_item(n.item) >= n.cnt) return 1;
        return 0;
    }
    int runs = 99;
    for (const QuestNeed& n : q.src)
        if (n.cnt > 0) runs = std::min(runs, w.count_item(n.item) / n.cnt);
    return runs;
}

int QuestRunner::batch_runs(const WorldModel& w, const QuestInfo& q, int runs)
{
    if (runs <= 1) return std::max(runs, 1);
    const GameData* gd = w.data();
    const int maxWt = w.stat_base(ST_MAXWT);
    double per = 0.0;                                       // one run's items (a stack weighs weight x count)
    for (const QuestNeed& n : q.src) {
        const ItemInfo* ii = gd ? gd->item(n.item) : nullptr;
        per += double(std::max(1, ii ? ii->weight : 1)) * std::max(1, n.cnt);
    }
    if (maxWt <= 0 || per <= 0.0) return std::min(runs, 5);
    return std::clamp(int(maxWt * 0.40 / per), 1, runs);
}

int QuestRunner::level_gate(const GameData* gd, QKey q, int level, int* lv)
{
    if (!gd || q.id <= 0) return 0;
    int lo = 0, hi = 0;
    if (q.mon) {
        const MonQuestInfo* mi = gd->monster_quest(q.id);
        if (!mi) return 0;
        lo = mi->minLevel;
        hi = mi->maxLevel;
    } else {
        const QuestInfo* qi = gd->quest_graph().info(q);
        if (!qi) return 0;
        lo = qi->minLevel;
        hi = qi->maxLevel;
    }
    if (lo <= 0 && hi <= 0) return 0;
    if (level < lo) { if (lv) *lv = lo; return -1; }
    if (level > hi) { if (lv) *lv = hi; return 1; }
    return 0;
}

int QuestRunner::compound_spins(const CompoundDef& c, int made)
{
    const int p = std::clamp(c.probability, 1, 100);
    return std::clamp((std::max(1, made) * 100 + p - 1) / p, 1, 255);
}

void QuestRunner::save_acquire(std::ostream& o, const char* key) const
{
    botu::kv(o, key, "buyItems", buyItems);
    botu::kv(o, key, "buyMaxGelder", buyMaxGelder);
    botu::kv(o, key, "compoundItems", compoundItems);
}

bool QuestRunner::load_acquire(const std::string& k, const std::string& v)
{
    if (k == "buyItems") buyItems = botu::flag(v);
    else if (k == "buyMaxGelder") buyMaxGelder = std::max(0, int(botu::num(v)));
    else if (k == "compoundItems") compoundItems = botu::flag(v);
    else return false;
    return true;
}

const char* QuestRunner::state_name(int s)
{
    switch (s) {
    case JDone: return "done";
    case JOn: return "on";
    case JReady: return "ready";
    case JLater: return "later";
    case JWaitItem: return "needs an item";
    case JBlocked: return "blocked";
    case JNoWay: return "no way now";
    case JHeld: return "refused, again soon";
    case JLevel: return "waits for its level";
    default: return "?";
    }
}

// A monster quest's reward at our level (quest_db.hpp result_of: the row of the grade's index, the last past them); a
// random row (Type 1) is no sure reward: null.
const QuestResultRow* QuestRunner::monq_reward(const GameData* gd, int qid, int level)
{
    const MonQuestInfo* mi = gd ? gd->monster_quest(qid) : nullptr;
    if (!mi || mi->results.empty()) return nullptr;
    const int gr = std::max(0, mi->grade_for(level));
    const QuestResultRow& row = mi->results[std::min<size_t>(size_t(gr), mi->results.size() - 1)];
    return row.type == 1 ? nullptr : &row;
}

// ── the plan ──────────────────────────────────────────────────────────────────
std::vector<QJob> QuestRunner::make_plan(const WorldModel& w, const QuestBook& book) const
{
    std::vector<QJob> jobs;
    if (!w.data()) return jobs;
    const View v(book, w, &linear_);
    plan_own(w, v, jobs);
    add_providers(w, v, jobs);
    if (add_unlocks(w, v, jobs)) add_providers(w, v, jobs);
    return jobs;
}

// The Secret Garden of Rosetta (King Edward, Queen Cecilia) is entered only by Prince Sebastian's move with the Garden key
// in the bag, and the Vamp Castle (Butler of Vamp Castle) lies behind its Secret Room (the same key) — or a teleport of a
// level far above. The key is the reward of Alfredo's monster quest: that is how a player gets there, so the bot does too.
bool QuestRunner::add_unlocks(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const
{
    const GameData* gd = w.data();
    const QuestGraph& g = gd->quest_graph();
    const Atlas& at = gd->atlas();
    unlockFor_.clear();
    if (w.self.zone <= 0 || !at.map(w.self.zone)) return false;
    const RouteOptions base = Navigator::base_options(w);
    std::map<int, uint32_t> memo;                       // map -> the item that opens it (0: open, or none)
    bool added = false;
    for (QJob& j : jobs) {
        if (j.blocked || j.q.mon) continue;
        if (!j.giveItem && !j.keepActive && v.hist_lo(j.q) >= j.target) continue;
        int npc = -1;
        if (j.shop >= 0) {
            npc = j.shop;
        } else if (j.compound) {
            const std::vector<QCompound>& cp = g.compounds(j.giveItem);
            if (j.accept >= 0 && size_t(j.accept) < cp.size()) npc = cp[size_t(j.accept)].place.npc;
        } else if (j.exchange) {
            const std::vector<QExchange>& ex = g.exchanges(j.giveItem);
            if (j.accept >= 0 && size_t(j.accept) < ex.size()) npc = ex[size_t(j.accept)].place.npc;
        } else if (j.giveItem) {
            const std::vector<QPlace>& gp = g.give_places(j.giveItem);
            if (j.accept >= 0 && size_t(j.accept) < gp.size()) npc = gp[size_t(j.accept)].npc;
        } else if (const QuestPlaces* qp = g.places(j.q)) {
            if (j.accept >= 0 && size_t(j.accept) < qp->accepts.size()) npc = qp->accepts[size_t(j.accept)].npc;
        }
        const AtlasNpc* n = at.npc(npc);
        if (!n) continue;
        auto mm = memo.find(n->zone);
        uint32_t item = 0;
        if (mm != memo.end()) {
            item = mm->second;
        } else {
            if (!at.zone_reachable(w.self.zone, n->zone, base))
                for (uint32_t it : at.unlock_items()) {
                    if (w.count_item(it) > 0) continue;
                    if (!v.gatherable(it) && g.reward_quests(it).empty() && g.give_places(it).empty() && g.exchanges(it).empty() &&
                        g.compounds(it).empty() && at.shops_selling(it).empty()) continue;
                    RouteOptions o = base;
                    o.bag[it] += 1;
                    if (at.zone_reachable(w.self.zone, n->zone, o)) { item = it; break; }
                }
            memo[n->zone] = item;
        }
        if (!item) continue;
        unlockFor_[n->zone] = item;
        if (std::find(j.needItems.begin(), j.needItems.end(), item) == j.needItems.end()) {
            j.needItems.push_back(item);
            added = true;
        }
    }
    return added;
}

// An item the plan needs that no monster drops and no map buries, and that its quests do not give: the quest whose
// results give it joins the plan, as many runs as it takes (254's Water Bucket for 19, whose Filled Water Bucket goes to
// 18) — the way a player is sent there —, else an NPC that hands it out (Give_Item). A few passes: a provider may need
// such an item too. Never for a sticker (the picks say which quests give them) nor through a town's key quest chain.
void QuestRunner::add_providers(const WorldModel& w, const View& v, std::vector<QJob>& jobs) const
{
    const GameData* gd = w.data();
    const QuestGraph& g = gd->quest_graph();
    std::set<uint32_t> stickers;
    std::set<int> keyChains;
    for (const KeyTown& kt : g.towns()) {
        stickers.insert(kt.sticker);
        for (const QKey& k : kt.chain) keyChains.insert(k.id);
    }
    const Atlas& at = gd->atlas();
    const RouteOptions base = Navigator::base_options(w);
    // a shop that sells it, reached from here: as many as the galder allows (the cheapest shop; this map's first)
    auto try_buy = [&](uint32_t item, int deficit) {
        if (!buyItems || deficit <= 0) return false;
        for (const QJob& j : jobs) if (j.giveItem == item && j.shop >= 0) return false;
        const int64_t budget = std::min<int64_t>(buyMaxGelder, w.self.money - keepGelder);
        int best = -1, bestPrice = 0;
        for (int si : at.shops_selling(item)) {
            const AtlasNpc* sn = at.npc(si);
            const int price = sn ? at.shop_price(*gd, *sn, item) : -1;
            if (price < 0) continue;
            if (w.self.zone > 0 && sn->zone != w.self.zone && !at.zone_reachable(w.self.zone, sn->zone, base)) continue;
            const AtlasNpc* bn = at.npc(best);
            if (best < 0 || price < bestPrice || (price == bestPrice && sn->zone == w.self.zone && bn && bn->zone != w.self.zone)) {
                best = si;
                bestPrice = price;
            }
        }
        if (best < 0) return false;
        const int cnt = bestPrice > 0 ? int(std::min<int64_t>(deficit, std::max<int64_t>(0, budget) / bestPrice)) : deficit;
        if (cnt <= 0) return false;
        QJob bj;
        bj.q = QKey{ false, -int(item) - 20000000 };
        bj.giveItem = item;
        bj.shop = best;
        bj.target = cnt;
        bj.runs = 1;
        jobs.push_back(std::move(bj));
        return true;
    };
    // a provider job of an exchange / a compound through the NPC's dialog (accept / gate: the first way open)
    auto open_way = [&](const QPlace& p, QJob& j, int k) {
        for (size_t gi = 0; gi < p.gates.size(); ++gi) {
            bool ok = true;
            for (const QCond& c : p.gates[gi].conds) ok = ok && g.eval(c, v) != QuestGraph::Truth::No;
            if (ok) { j.accept = k; j.gate = int(gi); return true; }
        }
        return false;
    };
    // what the bag already holds: an exchange whose items are all in it, a compound whose ingredients are (one spin)
    auto try_bag = [&](uint32_t item, int deficit) {
        if (deficit <= 0) return false;
        for (const QJob& j : jobs) if (j.giveItem == item) return false;
        const std::vector<QExchange>& ex = g.exchanges(item);
        for (size_t k = 0; k < ex.size(); ++k) {
            const ExchangeDef* x = at.exchange(ex[k].id);
            if (!x || x->req.empty() || (x->fee > 0 && w.self.money - keepGelder < x->fee)) continue;
            int times = deficit;
            for (const auto& rq : x->req) times = std::min(times, w.count_item(rq.first) / std::max(1, rq.second));
            if (times <= 0) continue;
            QJob xj;
            xj.q = QKey{ false, -int(item) };
            xj.giveItem = item;
            xj.exchange = ex[k].id;
            xj.target = times;
            xj.runs = times;
            if (!open_way(ex[k].place, xj, int(k))) continue;
            jobs.push_back(std::move(xj));
            return true;
        }
        if (!compoundItems) return false;
        const std::vector<QCompound>& cp = g.compounds(item);
        for (size_t k = 0; k < cp.size(); ++k) {
            const CompoundDef* cd = at.compound(cp[k].id);
            if (!cd || cd->req.empty() || (cd->fee > 0 && w.self.money - keepGelder < cd->fee)) continue;
            bool one = true;
            for (const auto& rq : cd->req) one = one && w.count_item(rq.first) >= rq.second;
            if (!one) continue;
            QJob cj;
            cj.q = QKey{ false, -int(item) - 40000000 };
            cj.giveItem = item;
            cj.compound = cp[k].id;
            cj.target = deficit;
            cj.runs = 1;
            if (!open_way(cp[k].place, cj, int(k))) continue;
            jobs.push_back(std::move(cj));
            return true;
        }
        return false;
    };
    for (int pass = 0; pass < 4; ++pass) {
        std::map<uint32_t, int> need, got, needG;          // needG: the items a monster drops / a map buries
        for (const QJob& j : jobs) {
            if (j.blocked) continue;
            if (j.giveItem) {
                got[j.giveItem] += j.target;
                if (const ExchangeDef* x = j.exchange ? gd->atlas().exchange(j.exchange) : nullptr)   // what it is given
                    for (const auto& rq : x->req)
                        (v.gatherable(rq.first) ? needG : need)[rq.first] += rq.second * j.target;
                if (const CompoundDef* cd = j.compound ? at.compound(j.compound) : nullptr)          // its ingredients
                    for (const auto& rq : cd->req)
                        (v.gatherable(rq.first) ? needG : need)[rq.first] += rq.second * compound_spins(*cd, j.target);
                continue;
            }
            if (j.q.mon) {                                  // a monster quest: its grade's row each run
                if (const QuestResultRow* row = monq_reward(gd, j.q.id, w.self.level))
                    for (const QuestNeed& it : row->items) got[it.item] += std::max(1, it.cnt) * std::max(0, j.target - v.hist_lo(j.q));
                continue;
            }
            const QuestInfo* qi = g.info(j.q);
            if (!qi) continue;
            const int lo = v.hist_lo(j.q);
            for (int r = lo + 1; r <= j.target; ++r)
                if (const QuestResultRow* row = qi->result_for(r))
                    if (row->type != 1) for (const QuestNeed& it : row->items) got[it.item] += std::max(1, it.cnt);
            if (j.keepActive) continue;
            for (const QuestNeed& n : qi->src)
                (v.gatherable(n.item) ? needG : need)[n.item] += n.cnt * std::max(0, j.target - lo);
            for (uint32_t it : j.needItems)                 // (an item its dialog wants to see in the bag)
                if (v.hist_lo(j.q) < j.target) { auto& m = v.gatherable(it) ? needG : need; m[it] = std::max(m[it], 1); }
        }
        bool added = false;
        // an item a monster drops / a map buries that a shop sells: bought (quicker than the hunt), or made from the bag
        for (const auto& nd : needG) {
            const int deficit = nd.second - w.count_item(nd.first) - got[nd.first];
            if (deficit <= 0 || stickers.count(nd.first)) continue;
            if (try_buy(nd.first, deficit) || try_bag(nd.first, deficit)) added = true;
        }
        for (const auto& nd : need) {
            const int deficit = nd.second - w.count_item(nd.first) - got[nd.first];
            if (deficit <= 0 || stickers.count(nd.first)) continue;
            if (try_buy(nd.first, deficit) || try_bag(nd.first, deficit)) { added = true; continue; }
            // the quests (normal, scenario, monster) whose results give it: the one that gives enough with the fewest runs;
            // when none does alone, several, the cheapest first (Meet the Natives! wants 8 Heart Gems: one from each of
            // 8 scenario quests)
            struct Cand { QKey k; int runs = 0, sum = 0, extra = 0; double cost = 0; };
            std::vector<Cand> cands;
            for (const QKey& pk : g.reward_quests(nd.first)) {
                const QuestPlaces* pp = g.places(pk);
                if (!pp || pp->accepts.empty() || pp->completes.empty() || (!pk.scen && !pk.mon && keyChains.count(pk.id))) continue;
                int target = 0;
                for (const QJob& j : jobs) if (j.q == pk) target = j.target;
                const int lo = std::max(v.hist_lo(pk), target);    // (its runs already planned give theirs)
                int sum = 0, r = lo;
                if (pk.mon) {
                    const MonQuestInfo* mi = gd->monster_quest(pk.id);
                    const QuestResultRow* row = monq_reward(gd, pk.id, w.self.level);
                    if (!mi || !row || !mi->level_ok(w.self.level)) continue;      // (its level / grade: not now)
                    int per = 0;
                    for (const QuestNeed& it : row->items) if (it.item == nd.first) per += std::max(1, it.cnt);
                    while (per > 0 && sum < deficit && r < std::min(mi->maxCount, 25)) { ++r; sum += per; }
                } else {
                    const QuestInfo* pi = g.info(pk);
                    if (!pi) continue;
                    bool stickerSrc = false;
                    for (const QuestNeed& n : pi->src) stickerSrc = stickerSrc || stickers.count(n.item);
                    if (stickerSrc) continue;
                    while (sum < deficit && r < std::min(pi->maxCount, 25)) {
                        ++r;
                        if (const QuestResultRow* row = pi->result_for(r))
                            if (row->type != 1) for (const QuestNeed& it : row->items) if (it.item == nd.first) sum += std::max(1, it.cnt);
                    }
                }
                if (sum <= 0) continue;
                cands.push_back(Cand{ pk, r, sum, r - lo, 0.0 });
            }
            const Cand* best = nullptr;
            for (const Cand& cd : cands)
                if (cd.sum >= deficit && (!best || cd.extra < best->extra)) best = &cd;
            if (best) {
                g.expand(best->k, best->runs, v, jobs);
                added = true;
                continue;
            }
            if (!cands.empty()) {
                for (Cand& cd : cands) {                    // its own plan's size (its chain) per item: the cheapest first
                    std::vector<QJob> tmp;
                    const bool ok = g.expand(cd.k, cd.runs, v, tmp);
                    cd.cost = ok ? double(tmp.size() + size_t(cd.extra)) / double(cd.sum) : 1e9;
                }
                std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
                int left = deficit;
                for (const Cand& cd : cands) {
                    if (left <= 0 || cd.cost >= 1e9) break;
                    g.expand(cd.k, cd.runs, v, jobs);
                    left -= cd.sum;
                }
                added = true;
                continue;
            }
            // an NPC that hands it out: the cheapest way through its dialog
            const std::vector<QPlace>& gp = g.give_places(nd.first);
            bool have = false;
            for (const QJob& j : jobs) have = have || j.giveItem == nd.first;
            if (have) continue;
            if (gp.empty()) {
                // an exchange shop that gives it (the Story's Approved Memory Kit for the Memory Kit a quest gave)
                const std::vector<QExchange>& ex = g.exchanges(nd.first);
                if (ex.empty()) {
                    // a compounder that makes it: its ingredients join the plan (gathered, bought, given ...)
                    const std::vector<QCompound>& cp = g.compounds(nd.first);
                    if (!compoundItems || cp.empty()) continue;
                    QJob cj;
                    cj.q = QKey{ false, -int(nd.first) - 40000000 };
                    cj.giveItem = nd.first;
                    cj.target = deficit;
                    cj.runs = 1;
                    for (size_t k = 0; k < cp.size() && cj.accept < 0; ++k)
                        if (at.compound(cp[k].id) && open_way(cp[k].place, cj, int(k))) cj.compound = cp[k].id;
                    if (cj.accept < 0) { cj.blocked = true; cj.why = "the compounder's dialog does not open now"; cj.compound = cp.front().id; }
                    jobs.push_back(std::move(cj));
                    added = true;
                    continue;
                }
                QJob xj;
                xj.q = QKey{ false, -int(nd.first) };
                xj.giveItem = nd.first;
                xj.target = deficit;
                xj.runs = deficit;
                for (size_t k = 0; k < ex.size() && xj.accept < 0; ++k) {
                    if (!gd->atlas().exchange(ex[k].id)) continue;
                    for (size_t gi = 0; gi < ex[k].place.gates.size(); ++gi) {
                        bool ok = true;
                        for (const QCond& c : ex[k].place.gates[gi].conds) ok = ok && g.eval(c, v) != QuestGraph::Truth::No;
                        if (ok) { xj.accept = int(k); xj.gate = int(gi); xj.exchange = ex[k].id; break; }
                    }
                }
                if (xj.accept < 0) { xj.blocked = true; xj.why = "the exchange shop's dialog does not open now"; }
                jobs.push_back(std::move(xj));
                added = true;
                continue;
            }
            QJob gj;
            gj.q = QKey{ false, -int(nd.first) };
            gj.giveItem = nd.first;
            gj.target = deficit;
            gj.runs = deficit;
            for (size_t k = 0; k < gp.size() && gj.accept < 0; ++k)
                for (size_t gi = 0; gi < gp[k].gates.size(); ++gi) {
                    bool ok = true;
                    for (const QCond& c : gp[k].gates[gi].conds) ok = ok && g.eval(c, v) != QuestGraph::Truth::No;
                    if (ok) { gj.accept = int(k); gj.gate = int(gi); break; }
                }
            if (gj.accept < 0) { gj.blocked = true; gj.why = "its NPC's dialog does not hand it out now"; }
            jobs.push_back(std::move(gj));
            added = true;
        }
        if (!added) break;
    }
}

bool QuestRunner::held(const QJob& j, uint64_t now) const
{
    auto it = holdUntil_.find(j.q.v());
    auto st = stallHold_.find(j.q.v());
    return (it != holdUntil_.end() && now < it->second) || (st != stallHold_.end() && now < st->second);
}

void QuestRunner::refresh(const WorldModel& w, uint64_t now, bool digOpen, QuestBook& book)
{
    const GameData* gd = w.data();
    if (!gd) return;
    if (!startedAt) startedAt = now;
    book.merge_model(w);
    const std::string key = plan_key();
    const QuestGraph& g = gd->quest_graph();
    int region = 0;
    if (!g.entry_item(w.self.zone, &region)) region = 0;
    if (key != planKey_ || book.version() != planVer_ || w.self.level != planLevel_ || region != planRegion_ ||
        ft_older(now, planAt_, kReplanMs)) {
        jobs_ = make_plan(w, book);
        planKey_ = key;
        planVer_ = book.version();
        planLevel_ = w.self.level;
        planRegion_ = region;
        planAt_ = now ? now : 1;
    }
    const View v(book, w, &linear_);
    // An item nothing drops / buries (a sticker, a quest's reward): how many the jobs still to do will give (a Type 1
    // row: its share) against how many they need — short = the jobs needing it wait.
    std::map<uint32_t, double> got, needNg, needAll;    // needAll: every item the jobs left want (a bought one's "done")
    for (const QJob& j : jobs_) {
        if (j.blocked) continue;
        if (j.giveItem) {
            got[j.giveItem] += j.target;
            if (const ExchangeDef* x = j.exchange ? gd->atlas().exchange(j.exchange) : nullptr)
                for (const auto& rq : x->req) needAll[rq.first] += rq.second * j.target;
            if (const CompoundDef* cd = j.compound ? gd->atlas().compound(j.compound) : nullptr)
                for (const auto& rq : cd->req) needAll[rq.first] += rq.second * compound_spins(*cd, j.target);
            continue;
        }
        if (!v.active(j.q) && level_gate(gd, j.q, w.self.level) > 0) continue;      // (never taken again)
        if (j.q.mon) {                                      // a monster quest's sure reward each run left
            if (const QuestResultRow* row = monq_reward(gd, j.q.id, w.self.level))
                for (const QuestNeed& it : row->items) got[it.item] += std::max(1, it.cnt) * std::max(0, j.target - v.hist_lo(j.q));
            continue;
        }
        const QuestInfo* qi = g.info(j.q);
        if (!qi) continue;
        const int lo = v.hist_lo(j.q);
        for (int r = lo + 1; r <= j.target; ++r)
            if (const QuestResultRow* row = qi->result_for(r))
                for (const QuestNeed& it : row->items)
                    got[it.item] += std::max(1, it.cnt) * (row->type == 1 && !row->items.empty() ? 1.0 / double(row->items.size()) : 1.0);
        if (j.keepActive) continue;
        for (size_t k = 0; k < qi->src.size(); ++k) {
            if (qi->options == 2 && k > 0) break;
            needAll[qi->src[k].item] += qi->src[k].cnt * std::max(0, j.target - lo);
            if (!v.gatherable(qi->src[k].item)) needNg[qi->src[k].item] += qi->src[k].cnt * std::max(0, j.target - lo);
        }
        if (lo < j.target)                                  // an item its dialog / its map wants in the bag
            for (uint32_t it : j.needItems) {
                needAll[it] = std::max(needAll[it], 1.0);
                if (!v.gatherable(it)) needNg[it] = std::max(needNg[it], 1.0);
            }
    }
    std::map<uint32_t, int> need, needLater;          // later: the jobs waiting for their level
    items_.clear();
    keep_items(w, items_);
    info_.assign(jobs_.size(), JobInfo{});
    for (size_t i = 0; i < jobs_.size(); ++i) {
        const QJob& j = jobs_[i];
        JobInfo& f = info_[i];
        if (j.giveItem) {                                   // an item an NPC hands out
            const int have = w.count_item(j.giveItem);
            items_.push_back(j.giveItem);
            f.runsLeft = std::max(0, j.target - have);
            if (j.blocked) { f.state = JBlocked; f.note = j.why; continue; }
            if (std::max(needNg[j.giveItem], needAll[j.giveItem]) <= have) { f.state = JDone; continue; }
            if (j.shop >= 0) {                              // a shop: the price of one at least
                const AtlasNpc* sn = gd->atlas().npc(j.shop);
                const int price = sn ? gd->atlas().shop_price(*gd, *sn, j.giveItem) : -1;
                f.runsLeft = std::max(1, int(std::max(needNg[j.giveItem], needAll[j.giveItem])) - have);
                if (price > 0 && w.self.money - keepGelder < price) {
                    f.state = JLater;
                    f.note = fmt("one costs %d galder (%lld to spend)", price, (long long)std::max<int64_t>(0, w.self.money - keepGelder));
                    continue;
                }
                f.note = sn ? fmt("%d at %s (%s), ~%d galder each", f.runsLeft, sn->name.c_str(), gd->map_name(sn->zone).c_str(), price) : "";
            }
            if (const CompoundDef* cd = j.compound ? gd->atlas().compound(j.compound) : nullptr) {   // its ingredients, its fee
                f.runsLeft = std::max(1, int(std::max(needNg[j.giveItem], needAll[j.giveItem])) - have);
                bool ok = true;
                const int spins = compound_spins(*cd, f.runsLeft);
                for (const auto& rq : cd->req) {
                    items_.push_back(rq.first);
                    if (v.gatherable(rq.first)) need[rq.first] += rq.second * spins;
                    if (w.count_item(rq.first) >= rq.second) continue;
                    ok = false;
                    f.note = fmt("needs %s x%d for the compound (%d%% a try)", gd->item_name(rq.first).c_str(), rq.second, cd->probability);
                }
                if (cd->fee > 0 && w.self.money - keepGelder < cd->fee) { ok = false; f.note = fmt("the compound costs %d galder", cd->fee); }
                if (!ok) { f.state = JLater; continue; }
            }
            if (const ExchangeDef* x = j.exchange ? gd->atlas().exchange(j.exchange) : nullptr) {   // what it is given
                bool ok = true;
                for (const auto& rq : x->req) {
                    items_.push_back(rq.first);
                    if (w.count_item(rq.first) >= rq.second) continue;
                    ok = false;
                    f.note = fmt("needs %s x%d for the exchange", gd->item_name(rq.first).c_str(), rq.second);
                    if (v.gatherable(rq.first)) need[rq.first] += rq.second * f.runsLeft;
                }
                if (x->fee > 0 && w.self.money < x->fee) { ok = false; f.note = fmt("the exchange costs %d galder", x->fee); }
                if (!ok) { f.state = JLater; continue; }
            }
            f.state = held(j, now) ? JHeld : JReady;
            continue;
        }
        f.lo = v.hist_lo(j.q);
        f.hi = v.hist_hi(j.q);
        const bool on = v.active(j.q);
        const QuestInfo* qi = g.info(j.q);
        f.runsLeft = j.keepActive ? (on ? 0 : 1) : std::max(0, j.target - f.lo);
        f.haveRuns = qi ? runs_in_bag(w, *qi) : 0;
        if (j.blocked && j.notForUs) { f.state = JDone; f.note = j.why; continue; }   // (another class's variant)
        if (qi) for (const QuestNeed& n : qi->src) items_.push_back(n.item);
        for (uint32_t it : j.needItems) items_.push_back(it);
        if (j.blocked) { f.state = JBlocked; f.note = j.why; continue; }
        if (!j.keepActive && f.runsLeft <= 0 && !on) { f.state = JDone; continue; }
        if (j.keepActive && on) { f.state = JOn; continue; }
        int gateLv = 0;
        const int gate = on ? 0 : level_gate(gd, j.q, w.self.level, &gateLv);
        if (held(j, now)) { f.state = JHeld; f.note = "refused / failed: asked again soon"; }
        else if (on) f.state = JOn;
        else if (gate > 0) { f.state = JBlocked; f.note = fmt("only up to level %d (now %d)", gateLv, w.self.level); }
        else if (gate < 0) {                                // the server refuses the take (3): not before its level
            f.state = JLevel;
            f.needLevel = gateLv;
            f.note = fmt("from level %d (now %d)", gateLv, w.self.level);
        }
        else {
            bool ready = true;
            int afterLv = 0;                                // a quest that comes first waits for its level: this one too
            for (const QKey& a : j.after)
                for (size_t xi = 0; xi < i; ++xi) {
                    const QJob& x = jobs_[xi];
                    if (x.q != a) continue;
                    const bool done = x.keepActive ? v.active(x.q) : v.hist_lo(x.q) >= x.target;
                    ready = ready && done;
                    if (!done && info_[xi].state == JLevel) afterLv = std::max(afterLv, info_[xi].needLevel);
                }
            if (afterLv) {
                f.state = JLevel;
                f.needLevel = afterLv;
                f.note = fmt("after a quest of level %d (now %d)", afterLv, w.self.level);
                ready = false;
            }
            const QuestPlaces* qp = g.places(j.q);
            if (ready && qp && j.accept >= 0 && j.gate >= 0) {
                const QGate& gt = qp->accepts[size_t(j.accept)].gates[size_t(j.gate)];
                for (const QCond& c : gt.conds) {
                    const QuestGraph::Truth t = g.eval(c, v);
                    if (t == QuestGraph::Truth::Yes || t == QuestGraph::Truth::Unknown) continue;
                    if (c.kind == QCond::MonqFree) f.note = "waits for the monster quest on";
                    else if (c.kind == QCond::HasItem || c.kind == QCond::Enter) {
                        f.note = "needs " + g.cond_text(c);
                        // who gives it (a monster quest's level: the Garden key is Alfredo's, level 120)
                        for (const QKey& pk : g.reward_quests(uint32_t(c.n))) {
                            if (pk.mon) {
                                const MonQuestInfo* mi = gd->monster_quest(pk.id);
                                if (mi) f.note += fmt(" - %s (%s) gives it%s", mi->name.c_str(), mi->client.c_str(),
                                                      mi->minLevel > w.self.level ? fmt(", from level %d", mi->minLevel).c_str() : "");
                            } else {
                                f.note += " - " + g.quest_name(pk) + " gives it";
                            }
                            break;
                        }
                    }
                    ready = false;
                }
            }
            if (f.state != JLevel) f.state = ready ? JReady : JLater;
        }
        // its items: the gather gets those a monster drops / a map buries; the others come from a quest of the plan, an
        // NPC, the bag — or nobody gives them
        std::map<uint32_t, int>& nd = f.state == JLevel ? needLater : need;
        if (qi && !(f.state == JBlocked && gate > 0)) {
            f.batch = batch_runs(w, *qi, std::max(f.runsLeft, 1));
            const int runs = std::min(std::max(f.runsLeft, on ? 1 : 0), f.batch);   // (a batch at a time: the weight)
            for (size_t k = 0; k < qi->src.size(); ++k) {
                const QuestNeed& n = qi->src[k];
                if (qi->options == 2 && k > 0) break;       // (a tier: the first one)
                const int want = n.cnt * runs;
                if (v.gatherable(n.item)) { nd[n.item] += want; continue; }
                const double shortBy = needNg[n.item] - w.count_item(n.item) - got[n.item];
                if (shortBy > 0.01 && w.count_item(n.item) < want) {
                    const bool isSticker = g.town(n.item) != nullptr;
                    f.note = isSticker ? fmt("needs %.0f more %s: pick more of the town's quests", std::ceil(shortBy), gd->item_name(n.item).c_str())
                                       : "needs " + gd->item_name(n.item) + " (no monster drops it, no map buries it, no quest or NPC of the plan gives enough"
                                         " - compounded / an exchange shop?)";
                    f.shortItem = true;
                    if (f.state == JReady || f.state == JLater) f.state = JWaitItem;
                }
            }
        }
        for (uint32_t it : j.needItems)
            if (v.gatherable(it)) nd[it] = std::max(nd[it], 1);
    }
    // the quests of our level first: the items of those waiting for theirs only once the others' are in the bag (a live
    // log: Swamp's level-150 quests asked for at 140, refused, again and again)
    bool nowInBag = true;
    for (const auto& kv : need) nowInBag = nowInBag && w.count_item(kv.first) >= kv.second;
    if (nowInBag && w.weight_pct() < 50) for (const auto& kv : needLater) need[kv.first] += kv.second;   // (not into a heavy bag)
    // the NPC each job ready to go would walk to: in a map no route gets to from here now (a teleport's level, the
    // galder, an item) the job waits — the bot does not stand at the route planner (a live log: 3 min at Rose Field 1)
    for (size_t i = 0; i < jobs_.size(); ++i) {
        const QJob& j = jobs_[i];
        JobInfo& f = info_[i];
        const bool take = f.state == JReady, handIn = f.state == JOn && f.haveRuns >= 1 && !j.keepActive && !j.q.mon;
        if (!take && !handIn) continue;
        int npc = -1;
        if (j.shop >= 0) {
            npc = j.shop;
        } else if (j.compound) {
            const std::vector<QCompound>& cp = g.compounds(j.giveItem);
            if (j.accept >= 0 && size_t(j.accept) < cp.size()) npc = cp[size_t(j.accept)].place.npc;
        } else if (j.exchange) {
            const std::vector<QExchange>& ex = g.exchanges(j.giveItem);
            if (j.accept >= 0 && size_t(j.accept) < ex.size()) npc = ex[size_t(j.accept)].place.npc;
        } else if (j.giveItem) {
            const std::vector<QPlace>& gp = g.give_places(j.giveItem);
            if (j.accept >= 0 && size_t(j.accept) < gp.size()) npc = gp[size_t(j.accept)].npc;
        } else if (const QuestPlaces* qp = g.places(j.q)) {
            const std::vector<QPlace>& pl = handIn ? qp->completes : qp->accepts;
            const int k = handIn ? j.complete : j.accept;
            if (k >= 0 && size_t(k) < pl.size()) npc = pl[size_t(k)].npc;
        }
        const AtlasNpc* n = gd->atlas().npc(npc);
        std::string why;
        if (n && !reachable(w, now, n->zone, &why)) {
            f.state = JNoWay;
            auto u = unlockFor_.find(n->zone);
            f.note = u != unlockFor_.end()
                   ? fmt("no way to %s in %s until %s is in the bag (its provider is in the plan)", n->name.c_str(),
                         gd->map_name(n->zone).c_str(), gd->item_name(u->second).c_str())
                   : fmt("no way to %s in %s from here now (%s)", n->name.c_str(), gd->map_name(n->zone).c_str(), why.c_str());
        }
    }
    std::sort(items_.begin(), items_.end());
    items_.erase(std::unique(items_.begin(), items_.end()), items_.end());
    // the gather's list (its plan is made again when the counts change)
    std::vector<HuntItem> list;
    for (const auto& kv : need)
        if (kv.second > 0) {
            HuntItem h;
            h.item = kv.first;
            h.want = kv.second;
            list.push_back(h);
        }
    bool same = list.size() == gather.list.size();
    for (size_t i = 0; same && i < list.size(); ++i) same = list[i].item == gather.list[i].item && list[i].want == gather.list[i].want;
    if (!same) gather.list = std::move(list);
    gather.driven = true;
    gather.stopWhenDone = false;
    if (!gather.list.empty()) gather.refresh(w, now, digOpen);
}

bool QuestRunner::reachable(const WorldModel& w, uint64_t now, int zone, std::string* why) const
{
    const GameData* gd = w.data();
    if (!gd || zone <= 0 || zone == w.self.zone) return true;
    const uint64_t key = uint64_t(uint32_t(w.self.zone)) << 32 | uint32_t(zone);
    ReachMemo& m = reach_[key];
    if (!m.at || m.level != w.self.level || ft_older(now, m.at, 30000)) {
        const Route r = gd->atlas().route(*gd, w.self.zone, w.self.x, w.self.y, zone, Navigator::base_options(w));
        m.at = now ? now : 1;
        m.level = w.self.level;
        m.ok = r.ok;
        m.why = r.why;
    }
    if (!m.ok && why) *why = m.why;
    return m.ok;
}

void QuestRunner::stalled(BotCtx& c, uint64_t holdMs)
{
    if (errand_on() || walk_.done() || walk_.failed()) {
        const int n = ++stallDrops_[errand_.q.v()];         // again the same one: 5, 20, then 60 min
        const uint64_t ms = n <= 1 ? holdMs : n == 2 ? holdMs * 4 : holdMs * 12;
        stallHold_[errand_.q.v()] = c.now + ms * kFtMs;
        c.bot.note(c.now, fmt("%s: %s dropped (no progress%s) - left alone %d min", label(), errand_.what.c_str(),
                              n > 1 ? fmt(", the %d%s time", n, n == 2 ? "nd" : n == 3 ? "rd" : "th").c_str() : "", int(ms / 60000)));
        walk_.reset();                                      // (a dialog left open: the Bot's stall watch closes it)
        errand_ = Errand{};
    }
    if (gathering_) gather.abandon_source(c, "no progress there");
    nav_.interrupted();
    planAt_ = 0;
    reach_.clear();
}

// ── the monster quest of the plan ─────────────────────────────────────────────
uint16_t QuestRunner::monq_mob(const WorldModel& w) const
{
    if (w.monq.has && w.monq.monster) return w.monq.monster;
    return 0;
}

int QuestRunner::monq_zone(const WorldModel& w) const
{
    const GameData* gd = w.data();
    const uint16_t m = monq_mob(w);
    if (!gd || !m) return 0;
    const std::vector<MobSpawn> sp = gd->spawns_of(m);
    if (!sp.empty()) return sp.front().zone;
    const MonQuestInfo* q = gd->monster_quest(w.monq.qid);
    const int g = q ? q->grade_for(w.self.level) : -1;
    return g >= 0 ? q->grades[size_t(g)].mapId : 0;
}

bool QuestRunner::monq_hunting(const WorldModel& w) const
{
    if (!w.monq.has || w.monq.success) return false;
    for (const QJob& j : jobs_) if (j.q.mon && j.q.id == w.monq.qid) return true;
    return false;
}

Step QuestRunner::monq_step(BotCtx& c, const QJob& j)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const MonQuestState& m = w.monq;
    if (m.takenAt != monqTakenSeen_) { monqTakenSeen_ = m.takenAt; portalAt_ = 0; portalFailed_ = false; }
    if (m.success) {                                        // back through the portal while its timer runs, then hand in
        const MonQuestInfo* qi = gd->monster_quest(j.q.id);
        if (usePortal && qi && qi->returnMap > 0 && m.portal && !portalFailed_ && w.self.zone != qi->returnMap) {
            if (!portalAt_) {
                if (m.running(c.now)) {
                    portalAt_ = c.now;
                    status = plan = "monster quest: through the portal back";
                    return c.act(act::monq_return());
                }
                portalFailed_ = true;
            } else if (m.returnOkAt >= portalAt_) {
                if (!ft_older(c.now, m.returnOkAt, kPortalMs)) { status = "monster quest: through the portal back"; return Step::Wait; }
                portalFailed_ = true;
            } else if (m.returnNoAt >= portalAt_ || ft_older(c.now, portalAt_, kPortalMs)) {
                portalFailed_ = true;
                c.bot.note(c.now, fmt("%s: the portal back was refused: walking", label()));
            } else {
                status = "monster quest: through the portal back";
                return Step::Wait;
            }
        }
        for (size_t i = 0; i < jobs_.size(); ++i)
            if (jobs_[i].q == j.q) return begin_errand(c, int(i), true);
        return Step::Wait;
    }
    const int zone = monq_zone(w);
    const std::string mob = gd->mob_name(m.monster);
    if (!zone) { status = plan = "monster quest: no map known for " + mob; return Step::Wait; }
    if (w.self.zone != zone) {
        Navigator::Goal goal;
        goal.zone = zone;
        goal.what = "to " + gd->map_name(zone) + " (" + mob + ")";
        nav_.go(goal);
        const Step s = nav_.step(c);
        if (s != Step::Pass) {
            status = plan = nav_.status;
            if (nav_.no_route()) {                          // no way between the maps: waiting does not bring one
                c.bot.stop(fmt("%s: no way to %s for the monster quest (%s)", label(), gd->map_name(zone).c_str(), nav_.route().why.c_str()));
                return Step::Wait;
            }
            if (nav_.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kLostMs)) c.bot.stop(fmt("%s: no way to %s", label(), gd->map_name(zone).c_str()));
            } else lostSince_ = 0;
            return s;
        }
    }
    lostSince_ = 0;
    status = plan = fmt("monster quest: %s %d / %d, %s left", mob.c_str(), m.kills, m.target, clock_text(m.remainMs).c_str());
    return Step::Pass;                                      // (the Attack / Loot / Roam work here: BotCtx targetMode)
}

// ── the errands at the NPCs ───────────────────────────────────────────────────
// the way through the dialog that holds now (every condition true or not known), the fewest unknown first
const QGate* QuestRunner::best_gate(const QuestGraph& g, const QPlace& p, const View& v) const
{
    const QGate* best = nullptr;
    int bestUnknown = 1 << 30;
    for (const QGate& gt : p.gates) {
        int unknown = 0;
        bool ok = true;
        for (const QCond& c : gt.conds) {
            const QuestGraph::Truth t = g.eval(c, v);
            if (t == QuestGraph::Truth::Unknown) ++unknown;
            else if (t != QuestGraph::Truth::Yes) ok = false;
        }
        if (ok && unknown < bestUnknown) { best = &gt; bestUnknown = unknown; }
    }
    return best;
}

Step QuestRunner::begin_errand(BotCtx& c, int job, bool complete)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const QJob& j = jobs_[size_t(job)];
    const QuestGraph& g = gd->quest_graph();
    const View v(c.bot.book, w, &linear_);
    const std::string name = g.quest_name(j.q);
    if (j.shop >= 0) {                                      // a shop: walk there, open it, buy, close (shop_step)
        const AtlasNpc* sn = gd->atlas().npc(j.shop);
        errand_ = Errand{};
        errand_.q = j.q;
        errand_.give = true;
        errand_.giveItem = j.giveItem;
        errand_.haveBefore = w.count_item(j.giveItem);
        errand_.shop = j.shop;
        errand_.shopWant = size_t(job) < info_.size() ? std::max(1, info_[size_t(job)].runsLeft) : std::max(1, j.target);
        errand_.what = fmt("buy %d %s", errand_.shopWant, gd->item_name(j.giveItem).c_str());
        plan = errand_.what + " (" + (sn ? sn->name : std::string("?")) + ")";
        return Step::Wait;
    }
    const QPlace* p = nullptr;
    if (j.compound) {
        const std::vector<QCompound>& cp = g.compounds(j.giveItem);
        if (j.accept >= 0 && size_t(j.accept) < cp.size()) p = &cp[size_t(j.accept)].place;
    } else if (j.exchange) {
        const std::vector<QExchange>& ex = g.exchanges(j.giveItem);
        if (j.accept >= 0 && size_t(j.accept) < ex.size()) p = &ex[size_t(j.accept)].place;
    } else if (j.giveItem) {
        const std::vector<QPlace>& gp = g.give_places(j.giveItem);
        if (j.accept >= 0 && size_t(j.accept) < gp.size()) p = &gp[size_t(j.accept)];
    } else {
        const QuestPlaces* qp = g.places(j.q);
        const int pi = complete ? j.complete : j.accept;
        const std::vector<QPlace>* pl = qp ? (complete ? &qp->completes : &qp->accepts) : nullptr;
        if (pl && pi >= 0 && size_t(pi) < pl->size()) p = &(*pl)[size_t(pi)];
    }
    if (!p) {
        holdUntil_[j.q.v()] = c.now + kLongHoldMs * kFtMs;
        c.bot.note(c.now, fmt("%s: %s has no NPC to %s it", label(), name.c_str(), complete ? "hand" : "take"));
        return Step::Wait;
    }
    const QGate* gate = best_gate(g, *p, v);
    if (!complete && !gate && j.gate >= 0 && size_t(j.gate) < p->gates.size()) gate = &p->gates[size_t(j.gate)];
    const AtlasNpc* npc = gd->atlas().npc(p->npc);
    if (!gate) {
        holdUntil_[j.q.v()] = c.now + kHoldMs * kFtMs;
        c.bot.note(c.now, fmt("%s: no way through %s's dialog to %s %s now", label(), npc ? npc->name.c_str() : "?",
                              j.giveItem ? "get" : complete ? "hand in" : "take", name.c_str()));
        return Step::Wait;
    }
    errand_ = Errand{};
    errand_.q = j.q;
    errand_.complete = complete;
    errand_.give = j.giveItem != 0;
    errand_.giveItem = j.giveItem;
    errand_.haveBefore = j.giveItem ? w.count_item(j.giveItem) : 0;
    errand_.exchange = j.exchange;
    errand_.compound = j.compound;
    errand_.cmpWant = size_t(job) < info_.size() ? std::max(1, info_[size_t(job)].runsLeft) : std::max(1, j.target);
    errand_.what = j.compound ? "compound " + gd->item_name(j.giveItem)
                 : j.exchange ? "exchange for " + gd->item_name(j.giveItem)
                 : j.giveItem ? "get " + gd->item_name(j.giveItem) : (complete ? "hand in " : "take ") + name;
    walk_.begin(p->npc, p->func, p->args, gate, errand_.what);
    plan = errand_.what + " (" + (npc ? npc->name : std::string("?")) + ")";
    const Step s = walk_.step(c);
    status = walk_.status;
    return s == Step::Pass ? Step::Wait : s;
}

void QuestRunner::learn(BotCtx& c)
{
    std::vector<std::string> notes;
    c.bot.book.learn(walk_.learned, &notes, &c.w);
    for (const std::string& n : notes) c.bot.note(c.now, std::string(label()) + ": " + n);
}

Step QuestRunner::errand_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    if (errand_.shop >= 0) return shop_step(c);
    if (walk_.busy() && !walk_.sent()) {
        const Step s = walk_.step(c);
        status = walk_.status;
        if (s != Step::Pass) return s;
    }
    if (walk_.sent() && errand_.exchange) return exchange_step(c);
    if (walk_.sent() && errand_.compound) return compound_step(c);
    if (walk_.sent()) {
        const uint64_t at = walk_.sentAt();
        const QKey q = errand_.q;
        bool ok = false, refused = false;
        std::string why;
        if (errand_.give) ok = w.count_item(errand_.giveItem) > errand_.haveBefore;
        else if (q.mon) ok = errand_.complete ? w.monq.doneAt >= at : (w.monq.takenAt >= at && w.monq.qid == q.id);
        else if (q.scen) ok = errand_.complete ? (w.quests.sdoneAt >= at && w.quests.sdoneQid == q.id)
                                               : (w.quests.stakenAt >= at && w.quests.stakenQid == q.id);
        else ok = errand_.complete ? (w.quests.doneAt >= at && w.quests.doneQid == q.id) : (w.quests.takenAt >= at && w.quests.takenQid == q.id);
        if (!ok) {
            if (w.npc.talkErrorAt >= at) { refused = true; why = "refused (116)"; }
            else if (w.npc.talkAt > at && w.npc.msgNo >= 0 && !errand_.give) {   // a message and no 132 / 150 / ... before it
                refused = true;
                why = fmt("answered with message %d", w.npc.msgNo);
                const QuestPlaces* qp = gd->quest_graph().places(q);
                for (const QJob& j : jobs_) {
                    if (j.q != q || !qp) continue;
                    const std::vector<QPlace>& pl = errand_.complete ? qp->completes : qp->accepts;
                    const int k = errand_.complete ? j.complete : j.accept;
                    if (k < 0 || size_t(k) >= pl.size()) break;
                    const std::vector<uint16_t>& a = pl[size_t(k)].args;
                    if (errand_.complete && a.size() >= 2 && a[1] == w.npc.msgNo) why = "the bag is full or too heavy for the reward";
                    if (!errand_.complete && a.size() >= 2 && a[1] == w.npc.msgNo) why = "no free quest entry (6 on?)";
                    if (!errand_.complete && a.size() >= 3 && a[2] == w.npc.msgNo) why = "the level";
                    break;
                }
            } else if (ft_older(c.now, at, kAnswerMs)) { refused = true; why = errand_.give ? "nothing came (the bag full?)" : "no answer"; }
        }
        if (!ok && !refused) { status = errand_.what + ": waiting for the answer"; return Step::Wait; }
        const std::string name = errand_.give ? gd->item_name(errand_.giveItem) : gd->quest_graph().quest_name(q);
        if (ok) {
            refusals_.erase(q.v());
            if (errand_.give) c.bot.note(c.now, fmt("%s: got %s from the NPC", label(), name.c_str()));
            else if (errand_.complete) {
                ++handedIn;
                c.bot.note(c.now, fmt("%s: %s handed in", label(), name.c_str()));
            } else {
                ++taken;
                c.bot.note(c.now, fmt("%s: %s taken", label(), name.c_str()));
            }
            planAt_ = 0;                                    // (the plan again with the new state)
        } else {
            const int n = ++refusals_[q.v()];
            holdUntil_[q.v()] = c.now + (n >= kMaxRefusals ? kLongHoldMs : kHoldMs) * kFtMs;
            c.bot.note(c.now, fmt("%s: %s - %s%s", label(), errand_.what.c_str(), why.c_str(), n >= kMaxRefusals ? " (3 times: left for 10 min)" : ""));
        }
        walk_.close();
    }
    if (walk_.busy()) {                                     // Closing: Exit
        const Step s = walk_.step(c);
        if (s != Step::Pass) return s;
    }
    if (walk_.done() || walk_.failed()) {
        const int ver = c.bot.book.version();
        learn(c);
        if (!walk_.skipped.empty())
            c.bot.note(c.now, fmt("%s: %s - the server does not do %s (S2C 116): went on to the call itself", label(),
                                  errand_.what.c_str(), walk_.skipped.c_str()));
        if (walk_.failed()) {
            c.bot.note(c.now, std::string(label()) + ": " + walk_.status);
            if (walk_.deviated && c.bot.book.version() != ver) {
                planAt_ = 0;                                // what was learned changes the plan
            } else {                                        // nothing new: not again at once (3 times, no way, no NPC: 10 min)
                const int n = ++refusals_[errand_.q.v()];
                holdUntil_[errand_.q.v()] = c.now + (walk_.lost || walk_.absent || n >= kMaxRefusals ? kLongHoldMs : kHoldMs) * kFtMs;
                if (walk_.lost) reach_.clear();
            }
        }
        walk_.reset();
        errand_ = Errand{};
    }
    return Step::Wait;
}

// An exchange shop, after ExchangeShop_Start was sent: its list (S2C 249), the exchange (C2S 254 -> 255 / 191 / 256),
// then always the close (C2S 257: state 18 swallows every move).
Step QuestRunner::exchange_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const ExchangeDef* x = gd->atlas().exchange(errand_.exchange);
    auto close = [&](bool ok, const std::string& why) {
        errand_.exOk = ok;
        errand_.exWhy = why;
        errand_.exPhase = 2;
        return c.act(act::exchange_close());
    };
    if (errand_.exPhase == 0) {
        const uint64_t at = walk_.sentAt();
        if (w.npc.exListAt >= at) {
            if (!x) return close(false, "no such exchange");
            std::vector<act::SellLine> give;
            for (const auto& rq : x->req) {                 // one bag stack with at least ReqNum (sub_6DB270)
                const InvItem* best = nullptr;
                for (const InvItem& it : w.inventory)
                    if (it.itemId == rq.first && it.equipSlot < 0 && it.count >= rq.second) { best = &it; break; }
                if (!best) return close(false, fmt("no stack of %d %s", rq.second, gd->item_name(rq.first).c_str()));
                give.push_back(act::SellLine{ best->uid, rq.first, uint16_t(rq.second) });
            }
            const Action a = act::exchange(w, x->id, give);
            if (!a.ok()) return close(false, a.error);
            errand_.exPhase = 1;
            errand_.exAt = c.now;
            status = errand_.what;
            return c.act(a);
        }
        if (w.npc.talkErrorAt >= at) return close(false, "refused (116)");
        if (ft_older(c.now, at, kAnswerMs)) return close(false, "the exchange shop did not open");
        status = errand_.what + ": waiting for the exchange shop";
        return Step::Wait;
    }
    if (errand_.exPhase == 1) {
        if (w.npc.exDoneAt >= errand_.exAt) return close(true, "");
        if (w.npc.exFailAt >= errand_.exAt) return close(false, "refused (the items / the money / the bag's room)");
        if (ft_older(c.now, errand_.exAt, kAnswerMs)) return close(false, "no answer");
        status = errand_.what + ": waiting for the answer";
        return Step::Wait;
    }
    // closed: the outcome
    const QKey q = errand_.q;
    if (errand_.exOk) {
        refusals_.erase(q.v());
        c.bot.note(c.now, fmt("%s: got %s at the exchange shop", label(), gd->item_name(errand_.giveItem).c_str()));
        planAt_ = 0;
    } else {
        const int n = ++refusals_[q.v()];
        holdUntil_[q.v()] = c.now + (n >= kMaxRefusals ? kLongHoldMs : kHoldMs) * kFtMs;
        c.bot.note(c.now, fmt("%s: %s - %s%s", label(), errand_.what.c_str(), errand_.exWhy.c_str(),
                              n >= kMaxRefusals ? " (3 times: left for 10 min)" : ""));
    }
    walk_.reset();
    errand_ = Errand{};
    return Step::Wait;
}

// The end of a shop / a compounder errand: the plan again (the bag changed) or this job left alone a while.
void QuestRunner::errand_outcome(BotCtx& c, bool ok, const std::string& why, const std::string& okText)
{
    const QKey q = errand_.q;
    if (ok) {
        refusals_.erase(q.v());
        c.bot.note(c.now, std::string(label()) + ": " + okText);
        planAt_ = 0;
    } else {
        const int n = ++refusals_[q.v()];
        holdUntil_[q.v()] = c.now + (n >= kMaxRefusals ? kLongHoldMs : kHoldMs) * kFtMs;
        c.bot.note(c.now, fmt("%s: %s - %s%s", label(), errand_.what.c_str(), why.c_str(), n >= kMaxRefusals ? " (3 times: left for 10 min)" : ""));
    }
    walk_.reset();
    errand_ = Errand{};
}

// A shop (shop.cpp): walk to it, C2S 0x46 -> S2C 0xA8 (state 5), C2S 0x48 [shop][1]{item, count} -> 0x49 / 0x4A (the money)
// / 0x4B (the weight: half as many, three times), then always C2S 0x4F (state 5 swallows every move).
Step QuestRunner::shop_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const Atlas& at = gd->atlas();
    const AtlasNpc* n = at.npc(errand_.shop);
    const NpcState& ns = w.npc;
    if (!n) { errand_outcome(c, false, "no such shop", ""); return Step::Wait; }
    auto close = [&](bool ok, const std::string& why) {
        errand_.exOk = ok;
        errand_.exWhy = why;
        errand_.shopPhase = 4;
        return c.act(act::shop_close());
    };
    switch (errand_.shopPhase) {
    case 0: {                                               // to it, then open it
        Navigator::Goal goal;
        goal.zone = n->zone; goal.point = true; goal.x = n->x; goal.y = n->y; goal.within = 120;
        goal.what = "to " + n->name + " (" + errand_.what + ")";
        nav_.go(goal);
        const Step s = nav_.step(c);
        if (s != Step::Pass) {
            status = nav_.status;
            if (nav_.no_route()) { errand_outcome(c, false, "no way to " + n->name, ""); return Step::Wait; }
            return s;
        }
        const Entity* e = nullptr;
        double bestD = 1e18;
        for (const auto& kv : w.entities) {                 // the ShopNpc (kind 5) of its species near its point
            const Entity& x = kv.second;
            if ((x.kind != EK_SHOP && x.kind != EK_NPC) || x.species != n->species) continue;
            const double d = botu::dist(x.x, x.y, n->x, n->y);
            if (d < bestD) { bestD = d; e = &x; }
        }
        if (!e || bestD > 400) {
            if (!errand_.shopAt) errand_.shopAt = c.now;
            if (ft_older(c.now, errand_.shopAt, 6000)) { errand_outcome(c, false, n->name + " is not here", ""); return Step::Wait; }
            status = "looking for " + n->name;
            return Step::Wait;
        }
        if (c.sitting()) return c.act(act::sit(0));
        if (w.npc.talkNpc) return c.act(act::npc_end());
        errand_.shopEid = e->eid;
        errand_.shopPhase = 1;
        errand_.shopAt = c.now;
        status = "opening " + n->name;
        return c.act(act::shop_open(w, errand_.shopEid));
    }
    case 1:                                                 // S2C 0xA8
        if (ns.shop == errand_.shopEid && ns.shopAt >= errand_.shopAt) { errand_.shopPhase = 2; return Step::Wait; }
        if (ft_older(c.now, errand_.shopAt, kAnswerMs)) return close(false, n->name + " did not open");
        status = "opening " + n->name;
        return Step::Wait;
    case 2: {                                               // buy, within the galder kept
        const int price = std::max(0, at.shop_price(*gd, *n, errand_.giveItem));
        const int64_t budget = std::min<int64_t>(buyMaxGelder, w.self.money - keepGelder);
        int cnt = errand_.shopWant >> errand_.shopTries;    // (each refusal: half of it)
        if (price > 0) cnt = int(std::min<int64_t>(cnt, std::max<int64_t>(0, budget) / price));
        cnt = std::min(cnt, 30000);
        if (cnt <= 0) return close(false, price > 0 ? fmt("not enough galder (%d each)", price) : "nothing to buy");
        errand_.shopPhase = 3;
        errand_.shopAt = c.now;
        status = errand_.what;
        return c.act(act::shop_buy(w, errand_.shopEid, { act::ShopLine{ errand_.giveItem, uint16_t(cnt) } }));
    }
    case 3:                                                 // its answer
        if (ns.boughtAt >= errand_.shopAt) return close(true, "");
        if (ns.buyFailAt >= errand_.shopAt || ft_older(c.now, errand_.shopAt, kAnswerMs)) {
            if (ns.buyFailAt >= errand_.shopAt && ++errand_.shopTries < 3 && (errand_.shopWant >> errand_.shopTries) > 0) {
                errand_.shopPhase = 2;                      // too heavy / too dear: half as many
                return Step::Wait;
            }
            return close(false, ns.buyFail == 0x004B ? "too heavy to carry them" : ns.buyFailAt >= errand_.shopAt ? "the shop refused (the galder?)"
                                                                                                     : "no answer");
        }
        status = errand_.what + ": waiting for the shop";
        return Step::Wait;
    default: {                                              // closed
        const int got = w.count_item(errand_.giveItem) - errand_.haveBefore;
        errand_outcome(c, errand_.exOk, errand_.exWhy, fmt("bought %d %s at %s", std::max(0, got), gd->item_name(errand_.giveItem).c_str(),
                                                           n->name.c_str()));
        return Step::Wait;
    }
    }
}

// A compounder (compound.cpp), after Compound_Start was sent: its list (S2C 188), the roll (C2S 227 type 10 -> S2C 228), the
// compound (C2S 186 -> S2C 190 / 191), then always the close (C2S 192: state 13 swallows every move).
Step QuestRunner::compound_step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const CompoundDef* cd = gd->atlas().compound(errand_.compound);
    const NpcState& ns = w.npc;
    auto close = [&](bool ok, const std::string& why) {
        errand_.cmpOk = ok;
        errand_.cmpWhy = why;
        errand_.cmpPhase = 3;
        return c.act(act::compound_close());
    };
    switch (errand_.cmpPhase) {
    case 0: {                                               // the list came: the roll
        const uint64_t at = walk_.sentAt();
        if (ns.cmpListAt >= at) {
            if (!cd) return close(false, "no such compound");
            // the spins: the successes wanted at its rate, as many as one bag stack of each ingredient, the galder allow
            int spins = cd->type == 1 ? 1 : compound_spins(*cd, errand_.cmpWant);
            for (const auto& rq : cd->req) {
                int best = 0;
                for (const InvItem& it : w.inventory)
                    if (it.itemId == rq.first && it.equipSlot < 0) best = std::max(best, int(it.count));
                spins = std::min(spins, best / std::max(1, rq.second));
            }
            if (cd->fee > 0) spins = int(std::min<int64_t>(spins, std::max<int64_t>(0, w.self.money - keepGelder) / cd->fee));
            if (spins <= 0) return close(false, "not enough of its ingredients / galder for one try");
            errand_.cmpItems.clear();
            for (const auto& rq : cd->req) {
                const InvItem* best = nullptr;
                for (const InvItem& it : w.inventory)
                    if (it.itemId == rq.first && it.equipSlot < 0 && (!best || it.count > best->count)) best = &it;
                errand_.cmpItems.push_back(act::SellLine{ best->uid, rq.first, uint16_t(rq.second * spins) });
            }
            const Action a = act::compound_roll(w, cd->id, spins, errand_.cmpItems);
            if (!a.ok()) return close(false, a.error);
            errand_.cmpSpins = spins;
            errand_.cmpPhase = 1;
            errand_.cmpAt = c.now;
            status = errand_.what;
            return c.act(a);
        }
        if (w.npc.talkErrorAt >= at) return close(false, "refused (116)");
        if (ft_older(c.now, at, kAnswerMs)) return close(false, "the compounder did not open");
        status = errand_.what + ": waiting for the compounder";
        return Step::Wait;
    }
    case 1:                                                 // S2C 228: the compound itself
        if (ns.cmpRateAt >= errand_.cmpAt) {
            const Action a = act::compound_make(w, errand_.cmpItems);
            if (!a.ok()) return close(false, a.error);
            errand_.cmpPhase = 2;
            errand_.cmpAt = c.now;
            return c.act(a);
        }
        if (ns.cmpFailAt >= errand_.cmpAt) return close(false, "refused (the ingredients)");
        if (ft_older(c.now, errand_.cmpAt, kAnswerMs)) return close(false, "no answer to the roll");
        status = errand_.what + ": the roll";
        return Step::Wait;
    case 2:                                                 // S2C 190 / 191
        if (ns.cmpDoneAt >= errand_.cmpAt) return close(true, "");
        if (ns.cmpFailAt >= errand_.cmpAt) return close(false, ns.cmpFailCode == 0 ? "no room in the bag" : "refused (the galder / the ingredients)");
        if (ft_older(c.now, errand_.cmpAt, kAnswerMs)) return close(false, "no answer");
        status = errand_.what + ": compounding";
        return Step::Wait;
    default:                                                // closed
        errand_outcome(c, errand_.cmpOk, errand_.cmpWhy,
                       fmt("compound %s x%d at %d%%: %d made, %d failed", gd->item_name(errand_.giveItem).c_str(), errand_.cmpSpins, ns.cmpRate,
                           ns.cmpMade, ns.cmpFailed));
        return Step::Wait;
    }
}

bool QuestRunner::one_trip(const WorldModel& w, const QJob& j) const
{
    const GameData* gd = w.data();
    const QuestGraph& g = gd->quest_graph();
    const QuestInfo* qi = g.info(j.q);
    const QuestPlaces* qp = g.places(j.q);
    if (!qi || qi->src.empty() || !qp || j.accept < 0 || size_t(j.accept) >= qp->accepts.size()) return false;
    const AtlasNpc* n = gd->atlas().npc(qp->accepts[size_t(j.accept)].npc);
    int root = 0;
    const uint32_t it = n ? g.entry_item(n->zone, &root) : 0;
    if (!it || !gd->atlas().port_zone(it)) return false;
    auto inside = [&](int z) { int r = 0; return g.entry_item(z, &r) && r == root; };
    if (inside(w.self.zone)) return false;
    // an item of it found only in there too (Rosaline's Memory buries its Voice Chips): the trip in is needed anyway
    const ItemSources& is = gd->item_sources();
    for (const QuestNeed& nd : qi->src) {
        if (w.count_item(nd.item) >= nd.cnt) continue;
        for (const ItemDigSource& d : is.digs(nd.item)) if (inside(d.zone)) return false;
        for (const ItemMobSource& m : is.mobs(nd.item))
            for (const MobSpawn& sp : gd->spawns_of(uint16_t(m.mob))) if (inside(sp.zone)) return false;
    }
    return true;
}

// ── the tick ──────────────────────────────────────────────────────────────────
void QuestRunner::reset()
{
    walk_.reset();
    errand_ = Errand{};
    nav_.reset();
    jobs_.clear();
    info_.clear();
    planAt_ = 0;
    planVer_ = -1;
    planKey_.clear();
    refusals_.clear();
    holdUntil_.clear();
    portalAt_ = monqTakenSeen_ = lostSince_ = noSourceSince_ = 0;
    portalFailed_ = false;
    gathering_ = false;
    gather.reset();
    taken = handedIn = 0;
    startedAt = 0;
    plan.clear();
}

void QuestRunner::preempted()
{
    walk_.interrupted();
    nav_.interrupted();
    gather.preempted();
}

void QuestRunner::observe(BotCtx& c)
{
    c.bot.book.merge_model(c.w);
}

Step QuestRunner::step(BotCtx& c)
{
    const WorldModel& w = c.w;
    gathering_ = false;
    idle_ = false;
    if (c.travelling) { status = "a town trip runs"; return Step::Pass; }
    const GameData* gd = w.data();
    if (!gd) { status = "no game data"; return Step::Wait; }
    if (walk_.busy() || walk_.sent() || walk_.done() || walk_.failed() || errand_.shop >= 0) return errand_step(c);
    const QuestGraph& g = gd->quest_graph();
    if (!configured(w)) { status = plan = idle_text(); return Step::Wait; }
    bool allDone = true;
    for (const JobInfo& f : info_) allDone = allDone && f.state == JDone;
    if (jobs_.empty() || allDone) {
        if (planAt_ && info_.size() == jobs_.size()) {
            c.bot.stop(fmt("%s (%d taken, %d handed in)", done_text().c_str(), taken, handedIn), true);
            return Step::Wait;
        }
        status = "planning";
        return Step::Wait;
    }
    if (info_.size() != jobs_.size()) { status = "planning"; return Step::Wait; }
    const View v(c.bot.book, w, &linear_);
    // 1. a monster quest of the plan on: its hunt / its hand-in
    for (const QJob& j : jobs_)
        if (j.q.mon && v.active(j.q)) return monq_step(c, j);
    // 2. a quest on with the items for every run left (or no more to get for it): hand it in
    for (size_t i = 0; i < jobs_.size(); ++i) {
        const QJob& j = jobs_[i];
        const JobInfo& f = info_[i];
        if (j.giveItem || j.q.mon || j.keepActive || f.state != JOn || f.haveRuns < 1) continue;
        // the batch in the bag (or a heavy bag): hand it in now, not after the items of every run left (a live report: the bag
        // at 90 %+ while the quests waited for all their runs' items)
        bool more = false;
        const int batch = std::min(f.runsLeft, std::max(1, f.batch));
        if (f.haveRuns < batch && w.weight_pct() < 70)
            if (const QuestInfo* qi = g.info(j.q))
                for (const QuestNeed& n : qi->src)
                    if (n.cnt > 0 && v.gatherable(n.item) && w.count_item(n.item) < n.cnt * batch) more = true;
        if (more && !gather.done(w)) continue;
        return begin_errand(c, int(i), true);
    }
    // 3. a quest ready: take it; an item an NPC hands out: ask for it
    const int onNormal = int(w.quests.active.size()), onScen = int(w.quests.sactive.size());
    int ahead = 0;
    for (size_t i = 0; i < jobs_.size(); ++i)
        if (!jobs_[i].q.mon && !jobs_[i].q.scen && !jobs_[i].giveItem && info_[i].state == JOn && info_[i].haveRuns < 1) ++ahead;
    std::string why;
    for (size_t i = 0; i < jobs_.size(); ++i) {
        const QJob& j = jobs_[i];
        const JobInfo& f = info_[i];
        if (f.state != JReady) continue;
        if (j.giveItem) return begin_errand(c, int(i), false);
        if (j.q.mon) {
            if (w.monq.has) { why = "another monster quest is on"; continue; }
            return begin_errand(c, int(i), false);
        }
        const int on = j.q.scen ? onScen : onNormal;
        if (on >= kMaxOn) { why = j.q.scen ? "6 scenario quests on: none more can be taken" : "6 quests on: none more can be taken"; continue; }
        const bool withItems = f.haveRuns >= 1 || j.keepActive;
        const bool unknown = f.hi < 0;
        if (!withItems && one_trip(w, j)) { why = g.quest_name(j.q) + ": its items first (its NPC's map takes a port item)"; continue; }
        if (take_first(j) || withItems || ((unknown || ahead < kAheadNoItems) && on < kMaxOn - 1)) return begin_errand(c, int(i), false);
    }
    // 4. the items
    if (!gather.list.empty() && !gather.done(w) && gather.current() >= 0 && gather.source().zone) {
        gathering_ = true;
        const Step s = gather.step(c);
        status = plan = "items: " + gather.status;
        return s;                                           // (Pass in its map: the Attack / Loot / Roam / Drill work)
    }
    // 5. nothing to do now: why
    std::string wait;
    for (size_t i = 0; i < jobs_.size() && wait.empty(); ++i)
        if (info_[i].state != JDone && !info_[i].note.empty()) wait = g.quest_name(jobs_[i].q) + ": " + info_[i].note;
    if (wait.empty()) wait = why.empty() ? "nothing to do now" : why;
    bool stuck = true, held = false;                        // every job left blocked / waiting for an item nobody gives
    int lvWait = 0, lvNeed = 0;                             // ... or for its level
    for (const JobInfo& f : info_) {
        stuck = stuck && (f.state == JDone || f.state == JBlocked || f.state == JWaitItem || f.state == JNoWay ||
                          f.state == JLevel || (f.state == JOn && f.shortItem && f.haveRuns < 1));
        held = held || f.state == JHeld;
        if (f.state == JLevel) { ++lvWait; lvNeed = lvNeed ? std::min(lvNeed, f.needLevel) : f.needLevel; }
    }
    if (lvWait)
        wait = fmt("everything our level takes is done; %d quest%s wait%s for level %d (now %d) - level up, then start again",
                   lvWait, lvWait == 1 ? "" : "s", lvWait == 1 ? "s" : "", lvNeed, w.self.level);
    // the items still missing have no usable source (a drill that reaches them, ...) and nothing else can move on
    // (a live log: the stall watch's remedy gave the source up and this stopped the bot 7 ms later, before the gather's
    // next plan; now only a lack that lasts 20 s stops it)
    if (!stuck && !held && !gather.list.empty() && !gather.done(w) && (gather.current() < 0 || !gather.source().zone)) {
        std::string miss;
        for (const HuntItem& h : gather.list)
            if (w.count_item(h.item) < h.want) miss += (miss.empty() ? "" : ", ") + gd->item_name(h.item);
        if (!noSourceSince_) noSourceSince_ = c.now;
        if (ft_older(c.now, noSourceSince_, 20000)) {
            wait = "no usable source for " + miss + " - " + gather.why_none(w);
            stuck = true;
        } else {
            wait = "looking for a source for " + miss;
        }
    } else {
        noSourceSince_ = 0;
    }
    if (stuck) { c.bot.stop(std::string(label()) + ": " + wait); return Step::Wait; }
    status = plan = wait;
    idle_ = true;                                           // (meanwhile the Attack / Roam may fight here: Bot::tick)
    return Step::Pass;
}

} // namespace nj
