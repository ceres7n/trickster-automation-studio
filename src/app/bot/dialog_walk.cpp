// Talks an NPC through its dialog to a call (dialog_walk.h). Server side: ProjectPAULA npc_talk.cpp — C2S 0x6F within
// 300 units (else S2C 113), the PreCheck's first message in S2C 112; C2S 0x72 [npc][u8 index] -> S2C 115 with that
// message (116 when the NPC has none); C2S 0x7F [npc][func][n][args] -> the function's result r >= 1 = S2C 115 with its
// r-th argument (116 on an error); Exit = func 1 (S2C 117 closes the client's window).
#include "app/bot/dialog_walk.h"
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>

namespace nj {
namespace {

constexpr int      kNpcRange = 150;             // walk this close (the talk takes 300)
constexpr int      kNpcNear = 400;              // the NPC stands within this of its atlas point (the server's spawn point)
constexpr uint64_t kAnswerMs = 4000;            // a dialog answer
constexpr uint64_t kLostMs = 60000;             // no way to the NPC this long: failed

// The NPC of that species nearest to where the atlas puts it, within kNpcNear: the server spawns a talk NPC on the
// type-4 point of its PosId, the atlas reads the same point (game_server.cpp spawn_world_entities) — one of the same look
// farther away is another NPC (a live log: chasing such one, then the walk back to the point, back and forth for minutes).
const Entity* npc_entity(const BotCtx& c, const AtlasNpc& n)
{
    const Entity* best = nullptr;
    double bestD = kNpcNear;
    for (const auto& kv : c.w.entities) {
        const Entity& e = kv.second;
        if ((e.kind != EK_NPC && e.kind != EK_SHOP) || e.species != n.species) continue;
        const double d = botu::dist(e.x, e.y, n.x, n.y);
        if (d <= bestD) { bestD = d; best = &e; }
    }
    return best;
}

// what the model has around the NPC's point and of its look elsewhere (a "not here" in the log says it)
std::string npcs_near(const BotCtx& c, const AtlasNpc& n)
{
    std::string near, look;
    int nn = 0, nl = 0, all = 0;
    for (const auto& kv : c.w.entities) {
        const Entity& e = kv.second;
        all += e.kind == EK_NPC || e.kind == EK_SHOP;
        const double d = botu::dist(e.x, e.y, n.x, n.y);
        if (d <= kNpcNear && e.kind != EK_MONSTER && ++nn <= 6)
            near += fmt("%s%s [kind %u, species %u] at (%d,%d)", near.empty() ? "" : ", ", c.w.entity_label(kv.first).c_str(),
                        e.kind, e.species, e.x, e.y);
        if (e.species == n.species && d > kNpcNear && ++nl <= 4)
            look += fmt("%s%s [kind %u] at (%d,%d)", look.empty() ? "" : ", ", c.w.entity_label(kv.first).c_str(), e.kind, e.x, e.y);
    }
    return fmt("%d NPCs in the map; near its point (%d,%d): %s; the same look elsewhere: %s", all, n.x, n.y,
               near.empty() ? "nothing" : near.c_str(), look.empty() ? "none" : look.c_str());
}

} // namespace

void DialogWalk::reset()
{
    phase_ = Idle;
    npc_ = -1;
    func_ = 0;
    args_.clear();
    path_.clear();
    idx_ = 0;
    waiting_ = false;
    expect_ = -1;
    stepOpt_ = nullptr;
    eid_ = 0;
    at_ = sentAt_ = lostSince_ = 0;
    refused_ = 0;
    failAfterClose_ = false;
    status.clear();
    why.clear();
    learned.clear();
    deviated = lost = absent = false;
    skipped.clear();
    nav_.reset();
}

void DialogWalk::begin(int npc, uint8_t func, const std::vector<uint16_t>& args, const QGate* gate, const std::string& what)
{
    reset();
    npc_ = npc;
    func_ = func;
    args_ = args;
    if (gate) path_ = gate->path;
    what_ = what;
    phase_ = Going;
}

void DialogWalk::learn(const std::vector<QCond>& cs)
{
    for (const QCond& c : cs) {
        bool have = false;
        for (const QCond& x : learned)
            have = have || (x.kind == c.kind && x.q == c.q && x.n == c.n && x.n2 == c.n2);
        if (!have) learned.push_back(c);
    }
}

Step DialogWalk::fail(BotCtx& c, const std::string& w)
{
    why = w;
    status = what_ + ": " + w;
    // the dialog still open: Exit first
    if (eid_ && c.w.npc.talkNpc == eid_) {
        failAfterClose_ = true;
        phase_ = Closing;
        return Step::Wait;
    }
    phase_ = Failed;
    return Step::Pass;
}

int DialogWalk::identify_first(const BotCtx& c) const
{
    const GameData* gd = c.w.data();
    const AtlasNpc* n = gd ? gd->atlas().npc(npc_) : nullptr;
    return n && n->dialog ? n->dialog->identify(c.w.npc.text) : -1;
}

Step DialogWalk::step(BotCtx& c)
{
    const WorldModel& w = c.w;
    const GameData* gd = w.data();
    const AtlasNpc* n = gd ? gd->atlas().npc(npc_) : nullptr;
    const NpcState& ns = w.npc;
    if (phase_ == Idle || phase_ == Done || phase_ == Failed || phase_ == Sent) return Step::Pass;
    if (!n) return fail(c, "no such NPC");
    const QuestGraph& g = gd->quest_graph();
    switch (phase_) {
    case Going: {
        // to the NPC itself once it is seen near its point (else to the point)
        const Entity* seen = w.self.zone == n->zone ? npc_entity(c, *n) : nullptr;
        Navigator::Goal goal;
        goal.zone = n->zone;
        goal.point = true;
        goal.x = seen ? seen->x : n->x;
        goal.y = seen ? seen->y : n->y;
        goal.within = kNpcRange;
        goal.what = "to " + n->name;
        nav_.go(goal);
        const Step s = nav_.step(c);
        if (s != Step::Pass) {
            status = nav_.status;
            if (nav_.no_route()) {                      // no way between the maps: waiting does not bring one
                lost = true;
                return fail(c, fmt("no way to %s in %s now (%s)", n->name.c_str(), gd->map_name(n->zone).c_str(), nav_.route().why.c_str()));
            }
            if (nav_.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kLostMs)) { lost = true; return fail(c, "no way to " + n->name); }
            } else lostSince_ = 0;
            return s;
        }
        lostSince_ = 0;
        const Entity* e = npc_entity(c, *n);
        if (!e) {
            if (!at_) at_ = c.now;
            if (ft_older(c.now, at_, 5000)) {
                absent = true;
                return fail(c, fmt("%s (species %u) is not at its point in %s - %s", n->name.c_str(), n->species,
                                   gd->map_name(n->zone).c_str(), npcs_near(c, *n).c_str()));
            }
            status = "looking for " + n->name;
            return Step::Wait;
        }
        if (c.sitting()) return c.act(act::sit(0));
        eid_ = e->eid;
        phase_ = Opening;
        at_ = c.now;
        status = "talking to " + n->name;
        return c.act(act::npc_talk(w, eid_));
    }
    case Opening:
        if (ns.talkNpc == eid_ && ns.talkAt >= at_ && ns.first) {
            const int m0 = identify_first(c);
            // the PreCheck that picks it: what it means
            std::vector<std::pair<int, std::vector<QCond>>> st = g.starts(*n);
            int picks = 0;
            for (const auto& s : st) picks += s.first == m0;
            if (picks == 1)
                for (const auto& s : st) if (s.first == m0) learn(s.second);
            if (path_.empty()) { phase_ = Calling; return Step::Wait; }
            if (m0 != path_.front()) {
                deviated = true;
                return fail(c, fmt("%s opened with message %d, the way starts at %d", n->name.c_str(), m0, path_.front()));
            }
            idx_ = 0;
            waiting_ = false;
            phase_ = Stepping;
            return Step::Wait;
        }
        if (ns.talkRefusedAt >= at_) {                          // too far: walk closer, talk again
            if (++refused_ >= 3) return fail(c, n->name + " refused the talk (too far?)");
            phase_ = Going;
            at_ = 0;
            return Step::Wait;
        }
        if (ft_older(c.now, at_, kAnswerMs)) return fail(c, n->name + " did not answer");
        return Step::Wait;
    case Stepping: {
        if (waiting_) {
            if (ns.talkErrorAt >= at_) {
                // a check ProjectPAULA does not do (S2C 116 for any player): the call itself, from here
                if (stepOpt_ && QuestGraph::server_lacks(stepOpt_->func)) {
                    skipped = Atlas::func_name(stepOpt_->func);
                    waiting_ = false;
                    phase_ = Calling;
                    return Step::Wait;
                }
                return fail(c, fmt("%s refused message %d", n->name.c_str(), expect_));
            }
            if (ns.talkNpc == eid_ && ns.talkAt >= at_ && !ns.first && ns.msgNo >= 0) {
                const int got = ns.msgNo;
                if (stepOpt_) {                                 // the check's branch: what it means
                    const auto br = g.branches(*n, *stepOpt_);
                    int hits = 0;
                    for (const auto& b : br)
                        if (b.first >= 1 && size_t(b.first) <= stepOpt_->args.size() && stepOpt_->args[size_t(b.first - 1)] == got) ++hits;
                    if (hits == 1)
                        for (const auto& b : br)
                            if (b.first >= 1 && size_t(b.first) <= stepOpt_->args.size() && stepOpt_->args[size_t(b.first - 1)] == got) learn(b.second);
                }
                waiting_ = false;
                if (got != expect_) {
                    deviated = true;
                    return fail(c, fmt("%s answered message %d, the way goes on at %d", n->name.c_str(), got, expect_));
                }
                ++idx_;
                return Step::Wait;
            }
            if (ft_older(c.now, at_, kAnswerMs)) return fail(c, fmt("%s did not answer (message %d)", n->name.c_str(), expect_));
            return Step::Wait;
        }
        if (idx_ + 1 >= path_.size()) { phase_ = Calling; return Step::Wait; }
        const int cur = path_[idx_], next = path_[idx_ + 1];
        if (!n->dialog) return fail(c, "the NPC has no dialog");
        const auto m = n->dialog->msgs.find(cur);
        if (m == n->dialog->msgs.end()) return fail(c, fmt("message %d is not in the dialog", cur));
        const DialogOpt* link = nullptr;
        const DialogOpt* check = nullptr;
        for (const DialogOpt& o : m->second.opts) {
            if (o.link == next) { link = &o; break; }
            if (o.link < 0 && !check)
                for (const auto& b : g.branches(*n, o))
                    if (b.first >= 1 && size_t(b.first) <= o.args.size() && o.args[size_t(b.first - 1)] == next) { check = &o; break; }
        }
        if (!link && !check) return fail(c, fmt("no option of message %d leads to %d", cur, next));
        expect_ = next;
        waiting_ = true;
        at_ = c.now;
        stepOpt_ = link ? nullptr : check;
        status = fmt("%s: message %d", n->name.c_str(), next);
        if (link) return c.act(act::npc_next(eid_, next));
        return c.act(act::npc_script(eid_, check->func, check->args, fmt("%s (%s)", Atlas::func_name(check->func), n->name.c_str())));
    }
    case Calling:
        phase_ = Sent;
        sentAt_ = c.now;
        status = what_;
        return c.act(act::npc_script(eid_, func_, args_, fmt("%s: %s", Atlas::func_name(func_), what_.c_str())));
    case Closing:
        phase_ = failAfterClose_ ? Failed : Done;
        if (!eid_ || ns.talkNpc != eid_) return failAfterClose_ ? Step::Pass : Step::Wait;
        return c.act(act::npc_script(eid_, act::npcfn::Exit, {}, "close the dialog"));
    default:
        return Step::Pass;
    }
}

} // namespace nj
