// The quests' dialog graph, their places and gates, the Key Quest towns, the prerequisite expansion (quest_graph.h).
#include "app/quest_graph.h"
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nj {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr int    kMaxDepth = 24;            // prerequisites of prerequisites ... (a chain longer than this is refused)
constexpr size_t kMaxGates = 24;            // ways kept per call
constexpr int    kMaxSteps = 20000;         // messages visited per search
constexpr size_t kMaxPath = 32;

bool cond_less(const QCond& a, const QCond& b)
{
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.q != b.q) return a.q < b.q;
    if (a.n != b.n) return a.n < b.n;
    return a.n2 < b.n2;
}

bool cond_eq(const QCond& a, const QCond& b) { return a.kind == b.kind && a.q == b.q && a.n == b.n && a.n2 == b.n2; }

// no two conditions that cannot both hold
bool consistent(const std::vector<QCond>& cs)
{
    for (size_t i = 0; i < cs.size(); ++i)
        for (size_t j = 0; j < cs.size(); ++j) {
            if (i == j) continue;
            const QCond& a = cs[i];
            const QCond& b = cs[j];
            if (a.kind == QCond::Never) return false;
            if (a.kind == QCond::HistGE && b.kind == QCond::HistLT && a.q == b.q && b.n <= a.n) return false;
            if (a.kind == QCond::Active && b.kind == QCond::NotActive && a.q == b.q) return false;
            if (a.kind == QCond::Items && b.kind == QCond::NoItems && a.q == b.q) return false;
            if (a.kind == QCond::FType && b.kind == QCond::FType && a.n != b.n) return false;
            if (a.kind == QCond::FType && b.kind == QCond::NotFType && a.n == b.n) return false;
            if (a.kind == QCond::HasItem && b.kind == QCond::NoItem && a.n == b.n) return false;
            if (a.kind == QCond::Level && b.kind == QCond::Level && (a.n2 < b.n || b.n2 < a.n)) return false;
        }
    return true;
}

QCond cond(uint8_t kind, QKey q = {}, int n = 0, int n2 = 0)
{
    QCond c;
    c.kind = kind;
    c.q = q;
    c.n = n;
    c.n2 = n2;
    return c;
}

// The depth-first walk over an NPC's messages to a call (gates_to).
struct Walk {
    const GameData& gd;
    const AtlasNpc& n;
    uint8_t func;
    const std::vector<uint16_t>& args;
    std::vector<QGate>& out;
    std::vector<int> path;
    std::vector<QCond> conds;
    int steps = 0;

    // the branches of a call: (the argument whose message comes next, 1-based) -> the conditions it needs
    void branches(const DialogOpt& o, std::vector<std::pair<int, std::vector<QCond>>>& br) const
    {
        br.clear();
        int slot = 0;
        const QuestGraph::Fn fn = QuestGraph::decode(o.func, slot);
        using Fn = QuestGraph::Fn;
        switch (fn) {
        case Fn::Check: {                       // quest.cpp quest_check
            if (slot > 4) return;
            const QuestInfo* qi = gd.quest(n.quests[slot]);
            if (!qi) return;
            const QKey q{ false, qi->id };
            const bool unlimited = qi->maxCount >= 99;      // (no limit: never "done")
            if (unlimited) br.push_back({ 1, { cond(QCond::NotActive, q) } });
            else br.push_back({ 1, { cond(QCond::NotActive, q), cond(QCond::HistLT, q, qi->maxCount) } });
            br.push_back({ 2, { cond(QCond::Active, q), cond(QCond::Items, q) } });
            br.push_back({ 3, { cond(QCond::Active, q), cond(QCond::NoItems, q) } });
            if (!unlimited) br.push_back({ 4, { cond(QCond::NotActive, q), cond(QCond::HistGE, q, qi->maxCount) } });
            return;
        }
        case Fn::MCheck: {                      // monster_quest.cpp monq_check
            if (slot > 4) return;
            const MonQuestInfo* mi = gd.monster_quest(n.quests[slot]);
            if (!mi) return;
            const QKey q{ true, mi->id };
            const bool unlimited = mi->maxCount >= 99;
            if (unlimited) br.push_back({ 1, { cond(QCond::MonqFree, q), cond(QCond::NotActive, q) } });
            else br.push_back({ 1, { cond(QCond::MonqFree, q), cond(QCond::NotActive, q), cond(QCond::HistLT, q, mi->maxCount) } });
            br.push_back({ 2, { cond(QCond::Active, q), cond(QCond::Items, q) } });
            br.push_back({ 3, { cond(QCond::Active, q), cond(QCond::NoItems, q) } });
            if (!unlimited) br.push_back({ 4, { cond(QCond::MonqFree, q), cond(QCond::NotActive, q), cond(QCond::HistGE, q, mi->maxCount) } });
            return;
        }
        case Fn::CheckState: {                  // quest_check_state: two arguments, CheckQuestId(n) done CheckQuestNum(n) times
            if (slot > 2 || o.args.size() != 2 || n.checkNum[slot] <= 0) return;
            const QKey q{ false, n.checkQuest[slot] };
            br.push_back({ 1, { cond(QCond::HistGE, q, n.checkNum[slot]) } });
            br.push_back({ 2, { cond(QCond::HistLT, q, n.checkNum[slot]) } });
            return;
        }
        case Fn::MCheckState: {                 // monq_check_state: three arguments
            if (o.args.size() != 3) return;
            const QKey q{ true, n.checkMonQuest };
            if (n.checkMonNum > 0) {
                br.push_back({ 2, { cond(QCond::HistGE, q, n.checkMonNum) } });
                br.push_back({ 3, { cond(QCond::HistLT, q, n.checkMonNum) } });
            } else {
                br.push_back({ 1, { cond(QCond::Active, q), cond(QCond::Items, q) } });
                br.push_back({ 3, { cond(QCond::NoItems, q) } });
            }
            return;
        }
        case Fn::ActiveId: {                    // quest_active: 1 on, else 2
            const QKey q{ false, n.activeQuest[slot] };
            if (!gd.quest(q.id)) { br.push_back({ 2, {} }); return; }
            br.push_back({ 1, { cond(QCond::Active, q) } });
            br.push_back({ 2, { cond(QCond::NotActive, q) } });
            return;
        }
        case Fn::FTypeOnly:
            br.push_back({ 1, { cond(QCond::FType, {}, slot) } });
            br.push_back({ 2, { cond(QCond::NotFType, {}, slot) } });
            return;
        case Fn::HaveItem: {                    // CheckHaveItem: 2 arguments -> CheckItemId0, 4 -> both
            const int i0 = int(n.checkItem[0]), i1 = int(n.checkItem[1]);
            if (o.args.size() == 2) {
                br.push_back({ 1, { cond(QCond::HasItem, {}, i0) } });
                br.push_back({ 2, { cond(QCond::NoItem, {}, i0) } });
            } else if (o.args.size() == 4) {
                br.push_back({ 1, { cond(QCond::HasItem, {}, i0), cond(QCond::HasItem, {}, i1) } });
                br.push_back({ 2, { cond(QCond::HasItem, {}, i0), cond(QCond::NoItem, {}, i1) } });
                br.push_back({ 3, { cond(QCond::NoItem, {}, i0), cond(QCond::HasItem, {}, i1) } });
                br.push_back({ 4, { cond(QCond::NoItem, {}, i0), cond(QCond::NoItem, {}, i1) } });
            }
            return;
        }
        case Fn::SCheck: {                      // quest.cpp squest_check: as Quest_Check (no skill), on the history of type 6
            if (slot > 4) return;
            const QuestInfo* qi = gd.scenario_quest(n.quests[slot]);
            if (!qi) return;
            const QKey q{ false, qi->id, true };
            const bool unlimited = qi->maxCount >= 99;
            if (unlimited) br.push_back({ 1, { cond(QCond::NotActive, q) } });
            else br.push_back({ 1, { cond(QCond::NotActive, q), cond(QCond::HistLT, q, qi->maxCount) } });
            br.push_back({ 2, { cond(QCond::Active, q), cond(QCond::Items, q) } });
            br.push_back({ 3, { cond(QCond::Active, q), cond(QCond::NoItems, q) } });
            if (!unlimited) br.push_back({ 4, { cond(QCond::NotActive, q), cond(QCond::HistGE, q, qi->maxCount) } });
            return;
        }
        case Fn::SActiveId: {                   // S_ActiveQuestID_n: 1 the scenario quest ActiveQuestIDn on, else 2
            const QKey q{ false, n.activeQuest[slot], true };
            if (!gd.scenario_quest(q.id)) { br.push_back({ 2, {} }); return; }
            br.push_back({ 1, { cond(QCond::Active, q) } });
            br.push_back({ 2, { cond(QCond::NotActive, q) } });
            return;
        }
        case Fn::SCheckState: {                 // S_CheckQuestState(_1/_2): CheckQuestId(n) done CheckQuestNum(n) times (type 6)
            if (slot > 2 || o.args.size() != 2 || n.checkNum[slot] <= 0) return;
            const QKey q{ false, n.checkQuest[slot], true };
            br.push_back({ 1, { cond(QCond::HistGE, q, n.checkNum[slot]) } });
            br.push_back({ 2, { cond(QCond::HistLT, q, n.checkNum[slot]) } });
            return;
        }
        case Fn::GiveItem:                      // npc_give_item: the NPC's GiveItemID handed over, then the 1st argument
            br.push_back({ 1, {} });
            return;
        case Fn::TakeItem:                      // npc_take_item: TakeItemID out of the bag -> the 1st argument; none there -> 2
            if (!n.takeItem) { br.push_back({ 2, {} }); return; }
            br.push_back({ 1, { cond(QCond::HasItem, {}, int(n.takeItem)) } });
            br.push_back({ 2, { cond(QCond::NoItem, {}, int(n.takeItem)) } });
            return;
        case Fn::CheckJob:                      // npc_talk.cpp Check_Job: our job's bit in the NPC's JobTypeFlag1/2 -> 1, else 2
            br.push_back({ 1, { cond(QCond::Job, {}, int(n.jobFlag1), int(n.jobFlag2)) } });
            br.push_back({ 2, { cond(QCond::NotJob, {}, int(n.jobFlag1), int(n.jobFlag2)) } });
            return;
        case Fn::CheckGender:                   // CheckGender: the model even -> 1, odd -> 2
            br.push_back({ 1, { cond(QCond::Gender, {}, 0) } });
            br.push_back({ 2, { cond(QCond::Gender, {}, 1) } });
            return;
        case Fn::Opaque:                        // CheckDailyValue (the time of day?), EventTalkStart, Random_Msg: any of its
                                                // message arguments, the walk sees which
            for (size_t k = 0; k < o.args.size() && k < 5; ++k)
                if (o.args[k] < 128) br.push_back({ int(k + 1), { cond(QCond::Opaque, {}, o.func) } });
            return;
        case Fn::Quit:
        case Fn::MQuit:
        case Fn::MRank:                         // the first argument's message, nothing else
            br.push_back({ 1, {} });
            return;
        default:                                // a take / hand-in of another quest, a shop, a teleport, Exit ...: the end
            return;
        }
    }

    void record()
    {
        std::vector<QCond> cs = conds;
        std::sort(cs.begin(), cs.end(), cond_less);
        cs.erase(std::unique(cs.begin(), cs.end(), cond_eq), cs.end());
        if (!consistent(cs)) return;
        for (QGate& g : out) {
            if (g.conds.size() != cs.size() || !std::equal(cs.begin(), cs.end(), g.conds.begin(), cond_eq)) continue;
            if (path.size() < g.path.size()) g.path = path;
            return;
        }
        if (out.size() < kMaxGates) out.push_back(QGate{ std::move(cs), path });
    }

    void go(int msg)
    {
        if (msg < 0 || msg >= 128 || ++steps > kMaxSteps || path.size() >= kMaxPath) return;
        if (std::find(path.begin(), path.end(), msg) != path.end()) return;
        auto it = n.dialog->msgs.find(msg);
        if (it == n.dialog->msgs.end()) return;
        if (!consistent(conds)) return;
        path.push_back(msg);
        for (const DialogOpt& o : it->second.opts)
            if (o.link < 0 && o.func == func && o.args == args) { record(); break; }
        std::vector<std::pair<int, std::vector<QCond>>> br;
        for (const DialogOpt& o : it->second.opts) {
            if (o.link >= 0) { go(o.link); continue; }
            branches(o, br);
            for (const auto& b : br) {
                if (b.first < 1 || size_t(b.first) > o.args.size()) continue;
                const size_t mark = conds.size();
                conds.insert(conds.end(), b.second.begin(), b.second.end());
                go(o.args[size_t(b.first - 1)]);
                conds.resize(mark);
            }
        }
        path.pop_back();
    }
};

} // namespace

QuestGraph::Fn QuestGraph::decode(uint8_t f, int& slot)
{
    slot = 0;
    switch (f) {
    case 1: return Fn::Exit;
    case 2: return Fn::Check;
    case 3: return Fn::Accept;
    case 4: return Fn::Complete;
    case 5: return Fn::Continue;
    case 6: return Fn::Quit;
    case 7: return Fn::MCheck;
    case 8: return Fn::MAccept;
    case 9: return Fn::MComplete;
    case 10: return Fn::MContinue;
    case 11: return Fn::MQuit;
    case 12: case 13: return Fn::MRank;
    case 82: return Fn::CheckState;
    case 83: return Fn::MCheckState;
    case 84: return Fn::HaveItem;
    case 104: slot = 1; return Fn::CheckState;
    case 105: slot = 2; return Fn::CheckState;
    case 149: return Fn::MNHComplete;
    case 181: return Fn::NHComplete;
    case 184: return Fn::OnceAccept;
    case 185: return Fn::MOnceAccept;
    default: break;
    }
    if (f >= 90 && f <= 95)   { slot = f - 89;  return Fn::Check; }
    if (f >= 96 && f <= 99)   { slot = f - 95;  return Fn::Accept; }
    if (f >= 100 && f <= 103) { slot = f - 99;  return Fn::Complete; }
    if (f >= 106 && f <= 109) { slot = f - 105; return Fn::Continue; }
    if (f >= 114 && f <= 116) { slot = f - 114; return Fn::ActiveId; }
    if (f >= 74 && f <= 77)   { slot = f - 74;  return Fn::FTypeOnly; }
    if (f >= 202 && f <= 205) { slot = f - 201; return Fn::OnceAccept; }
    if (f >= 238 && f <= 241) { slot = f - 237; return Fn::MCheck; }
    if (f >= 242 && f <= 245) { slot = f - 241; return Fn::MAccept; }
    if (f >= 246 && f <= 249) { slot = f - 245; return Fn::MComplete; }
    if (f >= 250 && f <= 253) { slot = f - 249; return Fn::MContinue; }
    // scenario quests (npc_talk.cpp: the slot = func - the first one)
    if (f >= 122 && f <= 126) { slot = f - 122; return Fn::SCheck; }
    if (f >= 127 && f <= 131) { slot = f - 127; return Fn::SAccept; }
    if (f >= 132 && f <= 136) { slot = f - 132; return Fn::SComplete; }
    if (f >= 137 && f <= 141) { slot = f - 137; return Fn::SContinue; }
    if (f >= 142 && f <= 144) { slot = f - 142; return Fn::SActiveId; }
    if (f >= 145 && f <= 147) { slot = f - 145; return Fn::SCheckState; }
    if (f == 119) return Fn::GiveItem;
    if (f == 120) return Fn::TakeItem;
    if (f == 118) return Fn::CheckJob;
    if (f == 86) return Fn::CheckGender;
    if (f == 87 || f == 223 || f == 151) return Fn::Opaque;
    return f ? Fn::Other : Fn::None;
}

bool QuestGraph::server_lacks(uint8_t func)
{
    return func == 87 || func == 223;
}

std::vector<std::pair<int, std::vector<QCond>>> QuestGraph::branches(const AtlasNpc& n, const DialogOpt& o) const
{
    std::vector<std::pair<int, std::vector<QCond>>> br;
    if (!gd_) return br;
    std::vector<QGate> none;
    const std::vector<uint16_t> noArgs;
    const Walk wk{ *gd_, n, 0, noArgs, none, {}, {}, 0 };
    wk.branches(o, br);
    return br;
}

std::vector<QGate> QuestGraph::gates_to(const AtlasNpc& n, uint8_t func, const std::vector<uint16_t>& args) const
{
    std::vector<QGate> out;
    if (!n.dialog || !gd_) return out;
    Walk wk{ *gd_, n, func, args, out, {}, {}, 0 };
    for (const auto& s : starts(n)) {
        wk.conds = s.second;
        wk.go(s.first);
    }
    // the fewest conditions first (the plainest way)
    std::stable_sort(out.begin(), out.end(), [](const QGate& a, const QGate& b) { return a.conds.size() < b.conds.size(); });
    return out;
}

// the first message (npc_talk.cpp npc_precheck)
std::vector<std::pair<int, std::vector<QCond>>> QuestGraph::starts(const AtlasNpc& n) const
{
    std::vector<std::pair<int, std::vector<QCond>>> starts;
    if (!gd_) return starts;
    const QKey arg{ false, n.preArg };
    switch (n.preType) {
    case 1:
        if (n.preArg) starts.push_back({ n.preYes, { cond(QCond::HistGE, arg, 1) } });
        starts.push_back({ n.preNo, n.preArg ? std::vector<QCond>{ cond(QCond::HistLT, arg, 1) } : std::vector<QCond>{} });
        break;
    case 2:
        if (n.preArg && gd_->quest(n.preArg)) starts.push_back({ n.preYes, { cond(QCond::Active, arg) } });
        starts.push_back({ n.preNo, n.preArg && gd_->quest(n.preArg) ? std::vector<QCond>{ cond(QCond::NotActive, arg) }
                                                                     : std::vector<QCond>{} });
        break;
    case 3:
        if (n.preArg) starts.push_back({ n.preYes, { cond(QCond::HasItem, {}, n.preArg) } });
        starts.push_back({ n.preNo, n.preArg ? std::vector<QCond>{ cond(QCond::NoItem, {}, n.preArg) } : std::vector<QCond>{} });
        break;
    case 4:
        if (n.preArg) starts.push_back({ n.preYes, { cond(QCond::FType, {}, n.preArg) } });
        starts.push_back({ n.preNo, n.preArg ? std::vector<QCond>{ cond(QCond::NotFType, {}, n.preArg) } : std::vector<QCond>{} });
        break;
    case 5: {                                   // the level table: each of its ranges' message
        for (int lv = 1; lv <= 400;) {
            const int m = gd_->lv_msg(n.preArg, lv);
            int hi = lv;
            while (hi < 400 && gd_->lv_msg(n.preArg, hi + 1) == m) ++hi;
            if (m >= 0) starts.push_back({ m, { cond(QCond::Level, {}, lv, hi) } });
            lv = hi + 1;
        }
        break;
    }
    case 6: {                                   // the scenario quest Arg done once (history type 6)
        const QKey s{ false, n.preArg, true };
        if (n.preArg) starts.push_back({ n.preYes, { cond(QCond::HistGE, s, 1) } });
        starts.push_back({ n.preNo, n.preArg ? std::vector<QCond>{ cond(QCond::HistLT, s, 1) } : std::vector<QCond>{} });
        break;
    }
    case 7: {                                   // ScenarioQuestInfo has Arg and it is on
        const QKey s{ false, n.preArg, true };
        const bool known = n.preArg && gd_->scenario_quest(n.preArg);
        if (known) starts.push_back({ n.preYes, { cond(QCond::Active, s) } });
        starts.push_back({ n.preNo, known ? std::vector<QCond>{ cond(QCond::NotActive, s) } : std::vector<QCond>{} });
        break;
    }
    case 8: case 9: case 10:                    // the NPC answers nothing
        break;
    default:
        starts.push_back({ 0, {} });
        break;
    }
    return starts;
}

void QuestGraph::build(const GameData& gd)
{
    gd_ = &gd;
    places_.clear();
    towns_.clear();
    const Atlas& at = gd.atlas();
    auto add = [](std::vector<QPlace>& v, int npc, int slot, const DialogOpt& o) {
        for (const QPlace& p : v)
            if (p.npc == npc && p.func == o.func && p.args == o.args) return (QPlace*)nullptr;
        QPlace p;
        p.npc = npc;
        p.slot = slot;
        p.func = o.func;
        p.args = o.args;
        v.push_back(std::move(p));
        return &v.back();
    };
    // the maps a player gets to from the start town (Coral Town 148) through portals and NPC teleports: an NPC elsewhere
    // (a test map, an unlinked event map) is left out
    std::set<int> reach;
    {
        std::vector<int> todo;
        if (at.map(148)) todo.push_back(148);
        else for (const auto& kv : at.maps()) if (kv.second.isTown()) todo.push_back(kv.first);
        for (int z : todo) reach.insert(z);
        while (!todo.empty()) {
            const int z = todo.back();
            todo.pop_back();
            const AtlasMap* m = at.map(z);
            if (!m) continue;
            auto go = [&](int to) { if (to > 0 && at.map(to) && reach.insert(to).second) todo.push_back(to); };
            for (const AtlasPortal& p : m->portals) go(p.toZone);
            for (int ti : m->teleporters)
                if (const AtlasNpc* tn = at.npc(ti))
                    for (const TeleportSlot& s : tn->teleports) if (!s.move || (s.items.empty() && !s.takes)) go(s.zone);
        }
    }
    // the maps only an item opens: a port item (random_port_db, one zone), an NPC move that wants an item in the bag
    // (Mirage Island's Ocean's End: the Contract of Secrecy) — and the maps their portals / free moves lead on to
    entry_.clear();
    gives_.clear();
    exch_.clear();
    comp_.clear();
    {
        std::vector<std::pair<int, uint32_t>> seeds;    // map, item
        for (int z : reach)
            if (const AtlasMap* m = at.map(z))
                for (int ti : m->teleporters)
                    if (const AtlasNpc* tn = at.npc(ti))
                        for (const TeleportSlot& s : tn->teleports)
                            if (s.move && !s.items.empty()) seeds.push_back({ s.zone, s.items.front().first });
        for (const auto& kv : at.port_items()) seeds.push_back({ kv.second, kv.first });
        for (const auto& sd : seeds) {
            if (reach.count(sd.first) || entry_.count(sd.first) || !at.map(sd.first)) continue;
            entry_[sd.first] = Entry{ sd.second, sd.first };
            std::vector<int> todo{ sd.first };
            while (!todo.empty()) {
                const AtlasMap* m = at.map(todo.back());
                todo.pop_back();
                if (!m) continue;
                auto go = [&](int to) {
                    if (to <= 0 || !at.map(to) || reach.count(to) || entry_.count(to)) return;
                    entry_[to] = Entry{ sd.second, sd.first };
                    todo.push_back(to);
                };
                for (const AtlasPortal& p : m->portals) go(p.toZone);
                for (int ti : m->teleporters)
                    if (const AtlasNpc* tn = at.npc(ti))
                        for (const TeleportSlot& s : tn->teleports) if (!s.move || (s.items.empty() && !s.takes)) go(s.zone);
            }
        }
    }
    unreach_.clear();
    for (size_t i = 0; i < at.npcs().size(); ++i) {
        const AtlasNpc& n = at.npcs()[i];
        if (n.kind != AtlasNpc::Talk || !n.dialog) continue;
        if (!reach.count(n.zone) && !entry_.count(n.zone)) {    // (kept to say why)
            for (int k = 0; k < 5; ++k)
                if (n.quests[k]) {
                    unreach_.emplace(QKey{ false, n.quests[k], true }, n.zone);
                    unreach_.emplace(QKey{ false, n.quests[k], false }, n.zone);
                    unreach_.emplace(QKey{ true, n.quests[k], false }, n.zone);
                }
            continue;
        }
        for (const auto& m : n.dialog->msgs)
            for (const DialogOpt& o : m.second.opts) {
                if (o.link >= 0 || !o.func) continue;
                int slot = 0;
                const Fn fn = decode(o.func, slot);
                if (fn == Fn::GiveItem) {               // an item the NPC hands out
                    if (n.giveItem && gd.item(n.giveItem)) add(gives_[n.giveItem], int(i), 0, o);
                    continue;
                }
                if (slot > 4 || !n.quests[slot]) continue;
                const int qid = n.quests[slot];
                const bool normal = fn == Fn::Check || fn == Fn::Accept || fn == Fn::Continue || fn == Fn::OnceAccept ||
                                    fn == Fn::Complete || fn == Fn::NHComplete;
                const bool monster = fn == Fn::MCheck || fn == Fn::MAccept || fn == Fn::MContinue || fn == Fn::MOnceAccept ||
                                     fn == Fn::MComplete || fn == Fn::MNHComplete;
                const bool scenario = fn == Fn::SCheck || fn == Fn::SAccept || fn == Fn::SContinue || fn == Fn::SComplete;
                if (!(normal && gd.quest(qid)) && !(monster && gd.monster_quest(qid)) && !(scenario && gd.scenario_quest(qid))) continue;
                const QKey key{ monster, qid, scenario };
                QuestPlaces& qp = places_[key];
                qp.key = key;
                if (fn == Fn::Check || fn == Fn::MCheck || fn == Fn::SCheck) add(qp.checks, int(i), slot, o);
                else if (fn == Fn::Complete || fn == Fn::NHComplete || fn == Fn::MComplete || fn == Fn::MNHComplete || fn == Fn::SComplete)
                    add(qp.completes, int(i), slot, o);
                else add(qp.accepts, int(i), slot, o);
            }
    }
    for (auto& kv : places_) {
        // the plain take first (Quest_Accept, then _Continue, then _Once_Accept), the plain hand-in first (with history)
        auto rank = [](uint8_t f) {
            int s = 0;
            const Fn fn = decode(f, s);
            return fn == Fn::Accept || fn == Fn::MAccept || fn == Fn::SAccept ? 0
                 : fn == Fn::Continue || fn == Fn::MContinue || fn == Fn::SContinue ? 1
                 : fn == Fn::Complete || fn == Fn::MComplete || fn == Fn::SComplete ? 0 : 2;
        };
        auto byRank = [&](const QPlace& a, const QPlace& b) { return rank(a.func) < rank(b.func); };
        std::stable_sort(kv.second.accepts.begin(), kv.second.accepts.end(), byRank);
        std::stable_sort(kv.second.completes.begin(), kv.second.completes.end(), byRank);
        for (QPlace& p : kv.second.accepts) p.gates = gates_to(*at.npc(p.npc), p.func, p.args);
        for (QPlace& p : kv.second.completes) p.gates = gates_to(*at.npc(p.npc), p.func, p.args);
    }
    for (auto& kv : gives_)
        for (QPlace& p : kv.second) p.gates = gates_to(*at.npc(p.npc), p.func, p.args);
    // the exchange shops: each exchange under the items it gives
    for (size_t i = 0; i < at.npcs().size(); ++i) {
        const AtlasNpc& n = at.npcs()[i];
        if (n.exchanges.empty() || (!reach.count(n.zone) && !entry_.count(n.zone))) continue;
        QPlace p;
        p.npc = int(i);
        p.func = 22;                                    // ExchangeShop_Start
        p.args = n.exchangeArgs;
        p.gates = gates_to(n, p.func, p.args);
        for (int id : n.exchanges)
            if (const ExchangeDef* x = at.exchange(id))
                for (uint32_t it : x->res) {
                    std::vector<QExchange>& v = exch_[it];
                    bool have = false;
                    for (const QExchange& e : v) have = have || (e.id == id && e.place.npc == p.npc);
                    if (!have) v.push_back(QExchange{ p, id });
                }
    }
    // the compounders: each compound under the items it makes
    for (size_t i = 0; i < at.npcs().size(); ++i) {
        const AtlasNpc& n = at.npcs()[i];
        if (n.compounds.empty() || (!reach.count(n.zone) && !entry_.count(n.zone))) continue;
        QPlace p;
        p.npc = int(i);
        p.func = 20;                                    // Compound_Start
        p.args = n.compoundArgs;
        p.gates = gates_to(n, p.func, p.args);
        for (int id : n.compounds)
            if (const CompoundDef* c = at.compound(id))
                for (uint32_t it : c->res) {
                    std::vector<QCompound>& v = comp_[it];
                    bool have = false;
                    for (const QCompound& e : v) have = have || (e.id == id && e.place.npc == p.npc);
                    if (!have) v.push_back(QCompound{ p, id });
                }
    }
    // a place in a map only an item opens: that item first (or being in there)
    auto entry_cond = [&](QPlace& p) {
        const AtlasNpc* n = at.npc(p.npc);
        auto e = n ? entry_.find(n->zone) : entry_.end();
        if (e == entry_.end()) return;
        QCond c;
        c.kind = QCond::Enter;
        c.n = int(e->second.item);
        c.n2 = e->second.root;
        for (QGate& g : p.gates) g.conds.push_back(c);
    };
    for (auto& kv : places_) {
        for (QPlace& p : kv.second.accepts) entry_cond(p);
        for (QPlace& p : kv.second.completes) entry_cond(p);
    }
    for (auto& kv : gives_) for (QPlace& p : kv.second) entry_cond(p);
    for (auto& kv : exch_) for (QExchange& x : kv.second) entry_cond(x.place);
    for (auto& kv : comp_) for (QCompound& x : kv.second) entry_cond(x.place);
    rewards_.clear();
    for (int scen = 0; scen < 2; ++scen)
        for (const QuestInfo& q : scen ? gd.scenario_quests() : gd.quests())
            for (const QuestResultRow& r : q.results) {
                if (r.type == 1) continue;                  // (one item at random: not counted on)
                const QKey k{ false, q.id, scen != 0 };
                for (const QuestNeed& it : r.items) {
                    std::vector<QKey>& v = rewards_[it.item];
                    if (v.empty() || v.back() != k) v.push_back(k);
                }
            }
    // the monster quests' rewards (the Garden key of Rose Garden: Alfredo's monster quest) — any grade's row
    for (const MonQuestInfo& m : gd.monster_quests())
        for (const QuestResultRow& r : m.results) {
            if (r.type == 1) continue;
            const QKey k{ true, m.id, false };
            for (const QuestNeed& it : r.items) {
                std::vector<QKey>& v = rewards_[it.item];
                if (std::find(v.begin(), v.end(), k) == v.end()) v.push_back(k);
            }
        }
    build_towns();
    build_chapters();
}

const std::vector<QKey>& QuestGraph::reward_quests(uint32_t item) const
{
    static const std::vector<QKey> none;
    auto it = rewards_.find(item);
    return it == rewards_.end() ? none : it->second;
}

const std::vector<QPlace>& QuestGraph::give_places(uint32_t item) const
{
    static const std::vector<QPlace> none;
    auto it = gives_.find(item);
    return it == gives_.end() ? none : it->second;
}

const std::vector<QCompound>& QuestGraph::compounds(uint32_t item) const
{
    static const std::vector<QCompound> none;
    auto it = comp_.find(item);
    return it == comp_.end() ? none : it->second;
}

const std::vector<QExchange>& QuestGraph::exchanges(uint32_t item) const
{
    static const std::vector<QExchange> none;
    auto it = exch_.find(item);
    return it == exch_.end() ? none : it->second;
}

uint32_t QuestGraph::entry_item(int zone, int* root) const
{
    auto it = entry_.find(zone);
    if (it == entry_.end()) return 0;
    if (root) *root = it->second.root;
    return it->second.item;
}

const QuestInfo* QuestGraph::info(QKey q) const
{
    if (!gd_ || q.mon || q.id <= 0) return nullptr;
    return q.scen ? gd_->scenario_quest(q.id) : gd_->quest(q.id);
}

int QuestGraph::chapter_of(int scenarioQuest) const
{
    auto it = chapterOf_.find(scenarioQuest);
    return it == chapterOf_.end() ? -1 : it->second;
}

// libconfig ScenarioQuestEpisode (42 rows: ID, SignOption 1 = a chapter's first quest / 2 = its last, EpisodeNo,
// ChapterNo, ChapterName in Korean; translated here). A chapter's quests: its last quest's plan for a character who has
// done nothing, less what the earlier chapters already take.
void QuestGraph::build_chapters()
{
    chapters_.clear();
    chapterOf_.clear();
    static const struct { int ep, ch, first, last; const char* name; } kCh[] = {
        { 0, 1, 1233, 1239, "The Adventure Begins" },
        { 0, 2, 1112, 1116, "Don Giuvanni's Forced March" },
        { 0, 3, 1136, 1136, "Our Prologue" },
        { 1, 1, 1000, 1009, "Nefertiti's Necklace" },
        { 1, 2, 1015, 1138, "The Mysterious Ancient Box" },
        { 1, 3, 1019, 1140, "Those Who Seek the Harkon" },
        { 2, 1, 1077, 1085, "The Sword of Vengeance" },
        { 2, 2, 1087, 1092, "The Song of Memory" },
        { 2, 3, 1093, 1105, "Prelude to a Cursed Fate" },
        { 3, 1, 1141, 1141, "Building the Empire" },
        { 3, 2, 1147, 1153, "The Rise of the Hero" },
        { 3, 3, 1158, 1168, "The Message of Spirit Nefertiti" },
        { 3, 4, 1169, 1169, "Bonus: Find the Lost Blueprints" },
        { 4, 1, 1175, 1180, "Megalo Company's Invitation" },
        { 4, 2, 1181, 1185, "The Game Development Diary" },
        { 4, 3, 1187, 1197, "The Dev Room Ghost Story" },
        { 5, 1, 1200, 1208, "Replaying Memories" },
        { 5, 2, 1209, 1215, "Don Cavalier's Room" },
        { 5, 3, 1216, 1224, "The Mysterious Sarcophagus" },
        { 6, 1, 1242, 1247, "Alteo's Last Day" },
        { 6, 2, 1248, 1256, "Nefertiti and Enkicladus" },
        { 6, 3, 1259, 1263, "Rosaline and Don Danihen" },
        { 6, 4, 1264, 1264, "Bonus: The Story of Tau and Chagan" },
    };
    // a character who has done nothing: every history known as 0
    struct Fresh : QStateView {
        const GameData& gd;
        explicit Fresh(const GameData& g) : gd(g) {}
        int  hist_lo(QKey) const override { return 0; }
        int  hist_hi(QKey) const override { return 0; }
        bool active(QKey) const override { return false; }
        bool items(QKey) const override { return false; }
        bool has_item(uint32_t) const override { return false; }
        int  level() const override { return 400; }
        int  ftype() const override { return 0; }
        bool monq_busy(QKey) const override { return false; }
        bool gatherable(uint32_t) const override { return true; }
    } fresh(*gd_);
    std::set<int> taken;
    for (const auto& c : kCh) {
        Chapter ch;
        ch.episode = c.ep;
        ch.chapter = c.ch;
        ch.name = c.name;
        ch.first = c.first;
        ch.last = c.last;
        if (const QuestInfo* q = gd_->scenario_quest(c.last)) ch.minLevel = q->minLevel;
        std::vector<QJob> jobs;
        expand(QKey{ false, c.last, true }, 1, fresh, jobs);
        for (const QJob& j : jobs)
            if (j.q.scen && !taken.count(j.q.id)) {
                ch.quests.push_back(j.q.id);
                taken.insert(j.q.id);
                chapterOf_[j.q.id] = int(chapters_.size());
            }
        if (!chapterOf_.count(c.first) && gd_->scenario_quest(c.first)) chapterOf_[c.first] = int(chapters_.size());
        chapters_.push_back(std::move(ch));
    }
}

const QuestPlaces* QuestGraph::places(QKey q) const
{
    auto it = places_.find(q);
    return it == places_.end() ? nullptr : &it->second;
}

const KeyTown* QuestGraph::town(uint32_t sticker) const
{
    for (const KeyTown& t : towns_) if (t.sticker == sticker) return &t;
    return nullptr;
}

std::string QuestGraph::quest_name(QKey q) const
{
    if (!gd_) return fmt("quest %d", q.id);
    if (q.mon) {
        const MonQuestInfo* m = gd_->monster_quest(q.id);
        std::string mob;
        if (m && !m->grades.empty()) mob = gd_->mob_name(uint32_t(m->grades.front().monster));
        return fmt("monster quest #%d%s", q.id, mob.empty() ? "" : (" (" + mob + ")").c_str());
    }
    if (q.id <= -40000000) return "compound: " + gd_->item_name(uint32_t(-q.id - 40000000));   // (a compounder's job)
    if (q.id <= -20000000) return "buy: " + gd_->item_name(uint32_t(-q.id - 20000000));        // (a shop's job)
    if (q.id < 0) return "from an NPC: " + gd_->item_name(uint32_t(-q.id));       // (a give job)
    const QuestInfo* qi = info(q);
    return qi ? qi->name : fmt("%s #%d", q.scen ? "scenario quest" : "quest", q.id);
}

int QuestGraph::max_count(QKey q) const
{
    if (!gd_) return 25;
    if (q.mon) { const MonQuestInfo* m = gd_->monster_quest(q.id); return m ? m->maxCount : 25; }
    const QuestInfo* qi = info(q);
    return qi ? qi->maxCount : 25;
}

void QuestGraph::build_towns()
{
    std::vector<uint32_t> stickers;
    for (uint32_t id : gd_->search_items("Sticker", 200)) {
        const std::string nm = gd_->item_name(id);
        if (nm.size() > 8 && nm.compare(nm.size() - 8, 8, " Sticker") == 0) stickers.push_back(id);
    }
    // Megalopolis has no sticker: its key chain (Scared Poppuri's Concern > Look for Toys > Fix Toys > Poppuri Redemption)
    // wants 4 Instant Adhesive at Fix Toys, and the Poppuri Dungeon's quests give one each at their 5th run — the same
    // shape as a town's stickers
    uint32_t adhesive = 0;
    for (uint32_t id : gd_->search_items("Instant Adhesive", 20))
        if (gd_->item_name(id) == "Instant Adhesive") { adhesive = id; stickers.push_back(id); break; }
    const Atlas& at = gd_->atlas();
    for (uint32_t s : stickers) {
        KeyTown t;
        t.sticker = s;
        const std::string nm = gd_->item_name(s);
        t.name = s == adhesive ? std::string("Megalopolis (Poppuri Dungeon)") : nm.substr(0, nm.size() - 8);
        for (const QuestInfo& q : gd_->quests())               // the key quest: asks for the most of it
            for (const QuestNeed& n : q.src)
                if (n.item == s && n.cnt > t.need) { t.need = n.cnt; t.key = QKey{ false, q.id }; }
        if (t.key.id) {
            t.chain.push_back(t.key);
            const QuestInfo* kq = gd_->quest(t.key.id);
            const QuestPlaces* kp = places(t.key);
            if (kq && kp && !kp->accepts.empty()) {
                const AtlasNpc* n = at.npc(kp->accepts.front().npc);
                for (int k = kp->accepts.front().slot + 1; n && k < 5; ++k) {
                    const QuestInfo* f = gd_->quest(n->quests[k]);
                    if (!f || f->minLevel != kq->minLevel) break;
                    t.chain.push_back(QKey{ false, f->id });
                }
            }
            // then the quest of the same level that takes what the last step gives (Fix Toys' Mended Toy Box goes to
            // Stern Poppuri: Poppuri Redemption)
            for (int guard = 0; kq && guard < 4; ++guard) {
                const QuestInfo* last = gd_->quest(t.chain.back().id);
                std::set<uint32_t> gives;
                if (last) for (const QuestResultRow& r : last->results) if (r.type != 1) for (const QuestNeed& it : r.items) gives.insert(it.item);
                const QuestInfo* next = nullptr;
                for (const QuestInfo& q : gd_->quests()) {
                    if (q.minLevel != kq->minLevel || q.maxCount != 1) continue;
                    bool inChain = false;
                    for (const QKey& c : t.chain) inChain = inChain || c.id == q.id;
                    if (inChain) continue;
                    for (const QuestNeed& nd : q.src) if (gives.count(nd.item)) next = &q;
                    if (next) break;
                }
                if (!next) break;
                t.chain.push_back(QKey{ false, next->id });
            }
        }
        for (const QuestInfo& q : gd_->quests()) {
            if (QKey{ false, q.id } == t.key) continue;
            StickerQuest sq;
            sq.q = QKey{ false, q.id };
            const int last = std::min(q.maxCount, 25);
            for (int r = 1; r <= last; ++r) {
                const QuestResultRow* row = q.result_for(r);
                if (!row) break;
                for (const QuestNeed& it : row->items) {
                    if (it.item != s) continue;
                    const double chance = row->type == 1 && !row->items.empty() ? 1.0 / double(row->items.size()) : 1.0;
                    sq.stickers += chance * std::max(1, it.cnt);
                    sq.runs = r;
                    if (!sq.firstRun) sq.firstRun = r;
                }
            }
            if (sq.runs > 0) t.quests.push_back(sq);
        }
        if (t.quests.empty() && !t.key.id) continue;
        towns_.push_back(std::move(t));
    }
    auto lv = [&](const KeyTown& t) {
        const QuestInfo* q = t.key.id ? gd_->quest(t.key.id) : nullptr;
        if (q) return q->minLevel;
        int m = 1000;
        for (const StickerQuest& sq : t.quests)
            if (const QuestInfo* x = gd_->quest(sq.q.id)) m = std::min(m, x->minLevel);
        return m;
    };
    std::stable_sort(towns_.begin(), towns_.end(), [&](const KeyTown& a, const KeyTown& b) { return lv(a) < lv(b); });
}

// ── the conditions against the state ──────────────────────────────────────────
QuestGraph::Truth QuestGraph::eval(const QCond& c, const QStateView& v) const
{
    const int hi = v.hist_hi(c.q);
    switch (c.kind) {
    case QCond::HistGE:    return v.hist_lo(c.q) >= c.n ? Truth::Yes : (hi >= 0 && hi < c.n) || v.assume_undone(c.q) ? Truth::Fixable : Truth::Unknown;
    case QCond::HistLT:    return (hi >= 0 && hi < c.n) || (v.assume_undone(c.q) && v.hist_lo(c.q) < c.n) ? Truth::Yes
                                : v.hist_lo(c.q) >= c.n ? Truth::No : Truth::Unknown;
    case QCond::Job:
    case QCond::NotJob: {
        const int j = v.job();
        if (j <= 0 || j >= 34) return Truth::Unknown;
        const bool has = j >= 17 ? ((uint32_t(c.n2) >> (j - 17)) & 1) != 0 : ((uint32_t(c.n) >> (j - 1)) & 1) != 0;
        return has == (c.kind == QCond::Job) ? Truth::Yes : Truth::No;
    }
    case QCond::Gender: {
        const int m = v.model();
        if (m <= 0 || m > 24) return Truth::Unknown;
        return (m & 1) == c.n ? Truth::Yes : Truth::No;
    }
    case QCond::Active:    return v.active(c.q) ? Truth::Yes : Truth::Fixable;
    case QCond::NotActive: return !v.active(c.q) ? Truth::Yes : Truth::Fixable;
    case QCond::Items:     return v.active(c.q) && v.items(c.q) ? Truth::Yes : Truth::No;
    case QCond::NoItems:   return !(v.active(c.q) && v.items(c.q)) ? Truth::Yes : Truth::No;
    case QCond::FType:     return v.ftype() == c.n ? Truth::Yes : Truth::No;
    case QCond::NotFType:  return v.ftype() != c.n ? Truth::Yes : Truth::No;
    case QCond::HasItem:
    case QCond::Enter: {
        if (v.has_item(uint32_t(c.n))) return Truth::Yes;
        if (c.kind == QCond::Enter) {                   // in there already
            auto e = entry_.find(v.zone());
            if (e != entry_.end() && e->second.root == c.n2) return Truth::Yes;
        }
        const uint32_t it = uint32_t(c.n);
        return v.gatherable(it) || !reward_quests(it).empty() || !give_places(it).empty() || !exchanges(it).empty() ||
               !compounds(it).empty() || (gd_ && !gd_->atlas().shops_selling(it).empty())
             ? Truth::Fixable : Truth::No;
    }
    case QCond::Opaque:    return Truth::Unknown;
    case QCond::NoItem:    return !v.has_item(uint32_t(c.n)) ? Truth::Yes : Truth::No;
    case QCond::Level:     return v.level() >= c.n && v.level() <= c.n2 ? Truth::Yes : Truth::No;
    case QCond::MonqFree:  return !v.monq_busy(c.q) ? Truth::Yes : Truth::Fixable;
    default:               return Truth::No;
    }
}

std::string QuestGraph::cond_text(const QCond& c) const
{
    const std::string q = quest_name(c.q);
    switch (c.kind) {
    case QCond::HistGE:    return fmt("%s done %d time(s)", q.c_str(), c.n);
    case QCond::HistLT:    return fmt("%s done fewer than %d times", q.c_str(), c.n);
    case QCond::Active:    return q + " on";
    case QCond::NotActive: return q + " not on";
    case QCond::Items:     return q + " on with its items";
    case QCond::NoItems:   return q + " on without its items";
    case QCond::FType:     return fmt("FType %d", c.n);
    case QCond::NotFType:  return fmt("not FType %d", c.n);
    case QCond::HasItem:   return gd_ ? gd_->item_name(uint32_t(c.n)) + " in the bag" : fmt("item %d in the bag", c.n);
    case QCond::NoItem:    return gd_ ? "no " + gd_->item_name(uint32_t(c.n)) + " in the bag" : fmt("no item %d", c.n);
    case QCond::Level:     return fmt("level %d-%d", c.n, c.n2);
    case QCond::Job:       return "a job of this NPC's (Check_Job)";
    case QCond::NotJob:    return "not a job of this NPC's (Check_Job)";
    case QCond::Gender:    return c.n ? "an odd model (CheckGender)" : "an even model (CheckGender)";
    case QCond::MonqFree:  return "no other monster quest on";
    case QCond::Opaque:    return fmt("%s answering this way", Atlas::func_name(uint8_t(c.n)));
    case QCond::Enter:     return gd_ ? fmt("%s in the bag (to get into %s)", gd_->item_name(uint32_t(c.n)).c_str(), gd_->map_name(c.n2).c_str())
                                      : fmt("item %d (to get into map %d)", c.n, c.n2);
    default:               return "a check the bot cannot follow (scenario / party / guild)";
    }
}

// ── the expansion ─────────────────────────────────────────────────────────────
double QuestGraph::gate_cost(QKey self, const QGate& g, const QStateView& v, std::set<uint64_t>& visiting, int depth) const
{
    double cost = 0.0;
    for (const QCond& c : g.conds) {
        const Truth t = eval(c, v);
        if (t == Truth::Yes) continue;
        if (t == Truth::No) return kInf;
        if (t == Truth::Unknown) { cost += 0.2; continue; }     // (taken as true; a known way is preferred)
        switch (c.kind) {
        case QCond::HistGE: {
            if (c.q == self) return kInf;               // (its own history on the way to its own take)
            const double tc = take_cost(c.q, v, visiting, depth + 1, nullptr, nullptr);
            if (!std::isfinite(tc)) return kInf;
            cost += double(c.n - v.hist(c.q)) + tc;
            break;
        }
        case QCond::Active: {
            if (c.q == self) return kInf;
            const double tc = take_cost(c.q, v, visiting, depth + 1, nullptr, nullptr);
            if (!std::isfinite(tc)) return kInf;
            cost += 0.5 + tc;
            break;
        }
        case QCond::NotActive: cost += c.q == self ? 0.0 : 1.0; break;   // (its hand-in / its end)
        case QCond::HasItem:
        case QCond::Enter:     cost += 1.0; break;
        case QCond::MonqFree:  cost += 0.5; break;
        default:               return kInf;
        }
    }
    return cost;
}

double QuestGraph::take_cost(QKey q, const QStateView& v, std::set<uint64_t>& visiting, int depth, int* place, int* gate) const
{
    if (memoFor_ != &v) { memo_.clear(); memoFor_ = &v; }
    if (depth > kMaxDepth || visiting.count(q.v())) { ++cuts_; return kInf; }
    auto m = memo_.find(q.v());
    if (m != memo_.end()) {
        if (place) *place = m->second.place;
        if (gate) *gate = m->second.gate;
        return m->second.cost;
    }
    const QuestPlaces* qp = places(q);
    if (!qp) return kInf;
    const int cutsBefore = cuts_;
    visiting.insert(q.v());
    Memo best;
    for (size_t a = 0; a < qp->accepts.size(); ++a)
        for (size_t g = 0; g < qp->accepts[a].gates.size(); ++g) {
            const double c = gate_cost(q, qp->accepts[a].gates[g], v, visiting, depth);
            if (c < best.cost) best = Memo{ c, int(a), int(g) };
        }
    visiting.erase(q.v());
    // kept: a way found, or none without a cut (a cycle / the depth) on the way — those depend on where it was asked from
    if (std::isfinite(best.cost) || cuts_ == cutsBefore) memo_[q.v()] = best;
    if (place) *place = best.place;
    if (gate) *gate = best.gate;
    return best.cost;
}

std::string QuestGraph::explain(QKey q, const QStateView& v, int depth) const
{
    if (depth == 0) { memo_.clear(); memoFor_ = &v; }
    const std::string name = quest_name(q);
    const QuestPlaces* qp = places(q);
    if (!qp || qp->accepts.empty()) {
        auto u = unreach_.find(q);
        return name + (u != unreach_.end() ? ": its NPC stands in " + gd_->map_name(u->second) + ", which no portal or NPC teleport leads to"
                                           : std::string(": no NPC of the atlas takes it in its dialog"));
    }
    if (depth > 4) return name + ": a long chain";
    // the way that stops the least: its conditions that do not hold and cannot be made to
    std::string best;
    size_t bestBad = SIZE_MAX;
    for (const QPlace& p : qp->accepts)
        for (const QGate& g : p.gates) {
            std::vector<std::string> bad;
            for (const QCond& c : g.conds) {
                const Truth t = eval(c, v);
                if (t == Truth::Yes || t == Truth::Unknown) continue;
                if (t == Truth::No) { bad.push_back(cond_text(c)); continue; }
                if ((c.kind == QCond::HistGE || c.kind == QCond::Active) && c.q == q) { bad.push_back(cond_text(c) + " (itself)"); continue; }
                if (c.kind == QCond::HistGE || c.kind == QCond::Active) {
                    std::set<uint64_t> vis;
                    if (!std::isfinite(take_cost(c.q, v, vis, 1, nullptr, nullptr))) bad.push_back(cond_text(c) + " <- " + explain(c.q, v, depth + 1));
                }
            }
            if (bad.size() < bestBad) {
                bestBad = bad.size();
                best.clear();
                for (const std::string& b : bad) best += (best.empty() ? "" : "; ") + b;
            }
        }
    if (bestBad == SIZE_MAX) return name + ": its take is not reached from the NPC's first messages";
    return name + ": " + (best.empty() ? std::string("(nothing stops it)") : best);
}

bool QuestGraph::expand(QKey q, int target, const QStateView& v, std::vector<QJob>& jobs) const
{
    std::set<uint64_t> visiting;
    memo_.clear();
    memoFor_ = &v;
    return expand_rec(q, target, v, jobs, visiting, 0);
}

bool QuestGraph::expand_rec(QKey q, int target, const QStateView& v, std::vector<QJob>& jobs, std::set<uint64_t>& visiting,
                            int depth) const
{
    for (QJob& j : jobs)
        if (j.q == q) {
            if (target > j.target) { j.runs += target - j.target; j.target = target; j.keepActive = false; }
            return !j.blocked;
        }
    QJob j;
    j.q = q;
    j.target = target;
    const int max = max_count(q);
    if (max < 99 && target > max) j.target = max;
    if (v.hist(q) >= j.target) return true;
    const QuestPlaces* qp = places(q);
    if (depth > kMaxDepth) { j.blocked = true; j.why = "the chain of quests before it is too long"; }
    else if (visiting.count(q.v())) { j.blocked = true; j.why = "its quests wait for each other"; }
    else if (!qp || qp->accepts.empty()) {
        j.blocked = true;
        auto u = unreach_.find(q);
        j.why = u != unreach_.end() ? "its NPC stands in " + gd_->map_name(u->second) + ", which no portal or NPC teleport leads to"
                                    : "no NPC of the atlas takes it in its dialog";
    }
    else if (qp->completes.empty()) { j.blocked = true; j.why = "no NPC of the atlas hands it in"; }
    if (!j.blocked) {
        const double c = take_cost(q, v, visiting, depth, &j.accept, &j.gate);
        if (!std::isfinite(c)) {
            j.blocked = true;
            // the plainest way, and what stops it
            const QPlace* p = nullptr;
            for (const QPlace& x : qp->accepts) if (!x.gates.empty()) { p = &x; break; }
            bool fixedNo = true;                        // (each gate fails on the job / FType / gender: another class's)
            for (const QPlace& x : qp->accepts)
                for (const QGate& gt : x.gates) {
                    bool no = false;
                    for (const QCond& cd : gt.conds) {
                        const bool fixed = cd.kind == QCond::Job || cd.kind == QCond::NotJob || cd.kind == QCond::Gender ||
                                           cd.kind == QCond::FType || cd.kind == QCond::NotFType;
                        no = no || (fixed && eval(cd, v) == Truth::No);
                    }
                    fixedNo = fixedNo && no;
                }
            j.notForUs = fixedNo && p;
            if (!p) j.why = "its take is not reached from the NPC's first messages";
            else if (j.notForUs) j.why = "not for this character (another job's / FType's / gender's variant)";
            else {
                std::string w;
                for (const QCond& cd : p->gates.front().conds)
                    if (eval(cd, v) != Truth::Yes) w += (w.empty() ? "" : ", ") + cond_text(cd);
                j.why = "its NPC's dialog wants " + (w.empty() ? std::string("a way the bot cannot take") : w);
            }
        }
    }
    if (!j.blocked) {
        visiting.insert(q.v());
        const QGate& g = qp->accepts[size_t(j.accept)].gates[size_t(j.gate)];
        for (const QCond& c : g.conds) {
            const Truth t = eval(c, v);
            if (t == Truth::Yes) continue;
            if (t == Truth::Unknown) { j.assumed.push_back(c); continue; }
            if (c.kind == QCond::HistGE) {
                if (!expand_rec(c.q, c.n, v, jobs, visiting, depth + 1)) { j.blocked = true; j.why = "needs " + cond_text(c) + " (no way)"; }
                j.after.push_back(c.q);
            } else if (c.kind == QCond::Active) {
                bool found = false;
                for (QJob& x : jobs) if (x.q == c.q) { found = true; break; }
                if (!found) {
                    QJob k;
                    k.q = c.q;
                    k.target = v.hist(c.q);
                    k.keepActive = true;
                    const double tc = take_cost(c.q, v, visiting, depth + 1, &k.accept, &k.gate);
                    if (!std::isfinite(tc)) { k.blocked = true; k.why = "cannot be taken"; j.blocked = true; j.why = "needs " + cond_text(c); }
                    if (const QuestPlaces* kp = places(c.q)) k.complete = kp->completes.empty() ? -1 : 0;
                    jobs.push_back(std::move(k));
                }
                j.after.push_back(c.q);
            } else if (c.kind == QCond::HasItem || c.kind == QCond::Enter) {
                j.needItems.push_back(uint32_t(c.n));
            }
        }
        visiting.erase(q.v());
        // the hand-in: at the same NPC as the take when it can, else the first one; its check likewise
        const int npc = qp->accepts[size_t(j.accept)].npc;
        for (size_t k = 0; k < qp->completes.size() && j.complete < 0; ++k)
            if (qp->completes[k].npc == npc && !qp->completes[k].gates.empty()) j.complete = int(k);
        for (size_t k = 0; k < qp->completes.size() && j.complete < 0; ++k) if (!qp->completes[k].gates.empty()) j.complete = int(k);
        if (j.complete < 0) j.complete = 0;
        for (size_t k = 0; k < qp->checks.size() && j.check < 0; ++k) if (qp->checks[k].npc == npc) j.check = int(k);
    }
    j.runs = std::max(0, j.target - v.hist(q));
    for (const QKey& a : j.after)
        for (const QJob& x : jobs) if (x.q == a) j.runs += x.runs;
    jobs.push_back(std::move(j));
    return !jobs.back().blocked;
}

} // namespace nj
