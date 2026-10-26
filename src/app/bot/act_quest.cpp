// Monster Quest: one repeatable monster quest, round and round (bot_actions.h QuestAction). Server rules
// (ProjectPAULA monster_quest.cpp + npc_talk.cpp, = retail; the script ids and the argument syntax checked in
// the client: the function table 0xFACE78, sub_C033A0):
//   - One monster quest at a time, run by its NPC's dialog: C2S 0x6F (within 300) opens it (S2C 112), C2S 0x7F
//     [npc][func][n][u16 x n] runs a script function of THAT NPC. Take = MonQuest_Accept (8 for its QuestId,
//     242..245 for QuestId1..4; Once_Accept 185) -> S2C 210, the timer 215, the kills 217 [0][target], then
//     the message of the 1st argument (115). Refused (level out of the grades / MinLevel..MaxLevel, done
//     MaxCount times, once a day, another one on) = a message or S2C 116, no 210.
//   - A kill counts for the monster's OWNER (the first one to hurt it) and only the grade's monster -> 217;
//     the last one -> 218 [qid][has a return map]. The timer (Time minutes) running out first -> 216 + 211;
//     dying before the end -> 211 too.
//   - Back: C2S 0x410 [u32 2] while the quest's timer runs and it has a ReturnMapID -> S2C 0x411 and the map
//     change (a relocate) in 500 ms; else 0x412 -> walk.
//   - Hand in = MonQuest_Complete (9 / 246..249; NH_Complete 149) -> S2C 219 [records][items][EXP][TM EXP]
//     [qid][galder], 115, 211. Sent before the kills are done the server DROPS the player (CompleteMonQuest:
//     bSuccess false -> close), so it goes only after a 218 (or a 235 entry with its record). A full bag /
//     too heavy for the reward = a message, no 219.
//   - The arguments are message numbers of the NPC's dialog: the bot sends those of the NPC's own action
//     (npc_msg_db, converted as the client does: m<n> = 256 + n ...), then Exit (func 1: S2C 117 closes the
//     client's window and its talk state).
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/atlas.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"

namespace nj {
namespace {

constexpr int      kNpcRange = 150;             // walk this close to the NPC (the talk takes 300)
constexpr uint64_t kAnswerMs = 4000;            // a dialog / script answer
constexpr uint64_t kRefusedWaitMs = 60000;      // a refused take / hand-in: asked again after this
constexpr int      kMaxRefusals = 3;            // this many in a row: the bot stops
constexpr uint64_t kPortalMs = 3000;            // the portal's answer, then its map change
constexpr uint64_t kGiveUpMs = 60000;           // no way to the NPC / the map this long: the bot stops

const Entity* npc_here(const BotCtx& c, const AtlasNpc& n)
{
    const Entity* best = nullptr;
    double bestD = 1e18;
    for (const auto& kv : c.w.entities) {
        const Entity& e = kv.second;
        if (e.kind != EK_NPC || e.species != n.species) continue;
        const double d = botu::dist(e.x, e.y, n.x, n.y);
        if (d < bestD) { bestD = d; best = &e; }
    }
    return best && bestD <= 400 ? best : nullptr;
}

std::string clock_text(uint32_t ms) { return fmt("%u:%02u", ms / 60000, (ms / 1000) % 60); }

} // namespace

void QuestAction::reset()
{
    nav_.reset();
    sub_ = 0;
    subAt_ = 0;
    npcEid_ = 0;
    subComplete_ = false;
    portalAt_ = 0;
    portalFailed_ = false;
    waitUntil_ = 0;
    refusals_ = 0;
    seenDone_ = seenTaken_ = UINT64_MAX;
    lostSince_ = 0;
    noteZone_ = 0;
    runs = 0;
    expGained = gelderGained = 0;
    lastDoneAt = 0;
    plan.clear();
}

const MonQuestInfo* QuestAction::info(const WorldModel& w) const
{
    return w.data() ? w.data()->monster_quest(questId) : nullptr;
}

const AtlasNpc* QuestAction::giver(const WorldModel& w, int* slot) const
{
    if (!w.data() || !questId) return nullptr;
    const Atlas& at = w.data()->atlas();
    const AtlasNpc* best = nullptr;
    if (!npcKey.empty()) {
        const AtlasNpc* n = at.npc(at.find_npc(npcKey));
        if (n && n->quest_slot(questId) >= 0) best = n;
    }
    if (!best) {                                // the first one whose dialog can take it, in its return map first
        const MonQuestInfo* q = info(w);
        for (int i : at.quest_givers(questId)) {
            const AtlasNpc* n = at.npc(i);
            const int s = n->quest_slot(questId);
            if (!n->call(act::npcfn::monq_accept(s)) && !(s == 0 && n->call(act::npcfn::MonQuest_Once_Accept))) continue;
            if (!best || (q && n->zone == q->returnMap && best->zone != q->returnMap)) best = n;
        }
    }
    if (best && slot) *slot = best->quest_slot(questId);
    return best;
}

uint16_t QuestAction::mob(const WorldModel& w) const
{
    if (w.monq.has && w.monq.qid == questId && w.monq.monster) return w.monq.monster;
    const MonQuestInfo* q = info(w);
    const int g = q ? q->grade_for(w.self.level) : -1;
    return g >= 0 ? uint16_t(q->grades[size_t(g)].monster) : 0;
}

// The chosen map while the monster lives there (or nothing is known of where it lives), else the map where
// most of it is regenerated, else the grade's MapId.
int QuestAction::hunt_zone(const WorldModel& w) const
{
    const uint16_t m = mob(w);
    if (!m || !w.data()) return huntZone > 0 ? huntZone : 0;
    const std::vector<MobSpawn> sp = w.data()->spawns_of(m);
    if (huntZone > 0) {
        if (sp.empty()) return huntZone;
        for (const MobSpawn& s : sp) if (s.zone == huntZone) return huntZone;
    }
    if (!sp.empty()) return sp.front().zone;
    const MonQuestInfo* q = info(w);
    const int g = q ? q->grade_for(w.self.level) : -1;
    return g >= 0 ? q->grades[size_t(g)].mapId : 0;
}

bool QuestAction::hunting(const WorldModel& w) const
{
    const MonQuestState& m = w.monq;
    return m.has && m.qid == questId && !m.success && w.self.zone == hunt_zone(w);
}

void QuestAction::observe(BotCtx& c)
{
    const MonQuestState& m = c.w.monq;
    if (seenDone_ == UINT64_MAX) seenDone_ = m.doneAt;          // (what came before the start is not ours)
    if (seenTaken_ == UINT64_MAX) seenTaken_ = m.takenAt;
    if (m.takenAt > seenTaken_) {                               // a new run: the portal may be used again
        seenTaken_ = m.takenAt;
        portalAt_ = 0;
        portalFailed_ = false;
    }
    if (m.doneAt > seenDone_) {
        seenDone_ = m.doneAt;
        ++runs;
        expGained += m.lastExp;
        gelderGained += m.lastGelder;
        lastDoneAt = m.doneAt;
        c.bot.note(c.now, fmt("monster quest handed in (%d this run): %lld EXP, %lld galder", runs, (long long)m.lastExp,
                              (long long)m.lastGelder));
    }
}

Step QuestAction::step(BotCtx& c)
{
    const WorldModel& w = c.w;
    if (c.travelling) { status = "a town trip runs"; return Step::Pass; }
    const MonQuestInfo* q = info(w);
    if (!q) { status = plan = "choose a monster quest (Monster Quest tab)"; return Step::Wait; }
    const AtlasNpc* n = giver(w);
    if (!n) { status = plan = "no NPC of the atlas gives this quest"; return Step::Wait; }
    const MonQuestState& m = w.monq;
    const bool mine = m.has && m.qid == questId;
    if (sub_ > 0) return npc_step(c, subComplete_);     // an errand at the NPC goes on to its exit
    if (m.has && !mine) {
        status = plan = fmt("another monster quest (%u) is on: hand it in or let it run out first", m.qid);
        return Step::Wait;
    }

    if (!mine) {                                        // take it
        if (maxRuns > 0 && runs >= maxRuns) { c.bot.stop(fmt("monster quest: %d runs done", runs), true); return Step::Wait; }
        if (!repeat && runs >= 1) { c.bot.stop("monster quest handed in (repeat is off)", true); return Step::Wait; }
        if (q->grade_for(w.self.level) < 0 || !q->level_ok(w.self.level)) {
            c.bot.stop(fmt("monster quest: level %d is out of its range", w.self.level));
            return Step::Wait;
        }
        plan = "take it from " + n->name;
        return npc_step(c, false);
    }

    if (m.success) {                                    // hand it in: through the portal while the timer runs
        if (usePortal && q->returnMap > 0 && m.portal && !portalFailed_ && w.self.zone != q->returnMap) {
            if (!portalAt_) {
                if (m.running(c.now)) {
                    portalAt_ = c.now;
                    status = plan = "through the portal back";
                    return c.act(act::monq_return());
                }
                portalFailed_ = true;                   // its time is up: no portal any more
            } else if (m.returnOkAt >= portalAt_) {
                if (!ft_older(c.now, m.returnOkAt, kPortalMs)) { status = "through the portal back"; return Step::Wait; }
                portalFailed_ = true;                   // no map change came
                c.bot.note(c.now, "monster quest: the portal took us nowhere: walking");
            } else if (m.returnNoAt >= portalAt_ || ft_older(c.now, portalAt_, kPortalMs)) {
                portalFailed_ = true;
                c.bot.note(c.now, "monster quest: the portal back was refused: walking");
            } else {
                status = "through the portal back";
                return Step::Wait;
            }
        }
        plan = "hand it in to " + n->name;
        return npc_step(c, true);
    }

    // the kills: in the hunt map the Attack / Loot / Roam work (BotCtx targetHere)
    const int zone = hunt_zone(w);
    const std::string mobName = w.data()->mob_name(m.monster);
    if (!zone) { status = plan = "no map known for " + mobName; return Step::Wait; }
    if (huntZone > 0 && zone != huntZone && noteZone_ != huntZone) {
        noteZone_ = huntZone;
        c.bot.note(c.now, fmt("%s does not live in %s: hunting in %s", mobName.c_str(), w.data()->map_name(huntZone).c_str(),
                              w.data()->map_name(zone).c_str()));
    }
    if (w.self.zone != zone) {
        Navigator::Goal g;
        g.zone = zone;
        g.what = "to " + w.data()->map_name(zone) + " (" + mobName + ")";
        nav_.go(g);
        const Step s = nav_.step(c);
        if (s != Step::Pass) {
            status = plan = nav_.status;
            if (nav_.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kGiveUpMs)) c.bot.stop("monster quest: no way to " + w.data()->map_name(zone));
            } else lostSince_ = 0;
            return s;
        }
    }
    lostSince_ = 0;
    status = plan = fmt("hunting %s: %d / %d, %s left", mobName.c_str(), m.kills, m.target, clock_text(m.remainMs).c_str());
    return Step::Pass;
}

// The NPC errand: walk + talk (0x6F) -> its dialog (112) -> the script (0x7F) -> its answer (210 / 219, or a
// message = refused) -> Exit.
Step QuestAction::npc_step(BotCtx& c, bool complete)
{
    const WorldModel& w = c.w;
    const NpcState& ns = w.npc;
    int slot = -1;
    const AtlasNpc* n = giver(w, &slot);
    if (!n) { sub_ = 0; return Step::Wait; }
    uint8_t fn = complete ? act::npcfn::monq_complete(slot) : act::npcfn::monq_accept(slot);
    if (!n->call(fn) && slot == 0) fn = complete ? act::npcfn::MonQuest_NH_Complete : act::npcfn::MonQuest_Once_Accept;
    const NpcScriptCall* call = n->call(fn);
    if (!call) {
        sub_ = 0;
        status = plan = fmt("%s's dialog has no %s for this quest", n->name.c_str(), complete ? "hand-in" : "take");
        return Step::Wait;
    }
    if (sub_ == 0 && c.now < waitUntil_) {
        status = fmt("asking %s again in %.0f s", n->name.c_str(), double(waitUntil_ - c.now) / 1e7);
        return Step::Wait;
    }
    subComplete_ = complete;
    switch (sub_) {
    case 0: {                                   // to the NPC, then talk
        Navigator::Goal g;
        g.zone = n->zone;
        g.point = true;
        g.x = n->x;
        g.y = n->y;
        g.within = kNpcRange;
        g.what = "to " + n->name;
        nav_.go(g);
        const Step s = nav_.step(c);
        if (s != Step::Pass) {
            status = nav_.status;
            if (nav_.failed) {
                if (!lostSince_) lostSince_ = c.now;
                if (ft_older(c.now, lostSince_, kGiveUpMs)) c.bot.stop("monster quest: no way to " + n->name);
            } else lostSince_ = 0;
            return s;
        }
        lostSince_ = 0;
        const Entity* e = npc_here(c, *n);
        if (!e) {
            if (!subAt_) subAt_ = c.now;
            if (ft_older(c.now, subAt_, 5000)) {
                c.bot.note(c.now, n->name + " is not here");
                waitUntil_ = c.now + 10000 * kFtMs;
                subAt_ = 0;
            }
            status = "looking for " + n->name;
            return Step::Wait;
        }
        if (c.sitting()) return c.act(act::sit(0));
        npcEid_ = e->eid;
        sub_ = 1;
        subAt_ = c.now;
        status = "talking to " + n->name;
        return c.act(act::npc_talk(w, npcEid_));
    }
    case 1:                                     // its dialog (112) -> the script
        if (ns.talkNpc == npcEid_ && ns.talkAt >= subAt_) {
            const MonQuestState& m = w.monq;
            if (complete && !(m.has && m.qid == questId && m.success)) {   // (never: the server would drop us)
                sub_ = 3;
                return Step::Wait;
            }
            sub_ = 2;
            subAt_ = c.now;
            status = complete ? "handing it in" : "taking the quest";
            return c.act(act::npc_script(npcEid_, fn, call->args, complete ? "hand in the monster quest" : "take the monster quest"));
        }
        if (ns.talkRefusedAt >= subAt_) { sub_ = 0; subAt_ = 0; status = "too far from " + n->name; return Step::Wait; }
        if (ft_older(c.now, subAt_, kAnswerMs)) {
            c.bot.note(c.now, n->name + " did not answer");
            sub_ = 0;
            subAt_ = 0;
            waitUntil_ = c.now + 10000 * kFtMs;
            return Step::Wait;
        }
        status = "talking to " + n->name;
        return Step::Wait;
    case 2: {                                   // 210 / 219 come before the NPC's message (115)
        const MonQuestState& m = w.monq;
        if (complete ? m.doneAt >= subAt_ : (m.takenAt >= subAt_ && m.qid == questId)) {
            refusals_ = 0;
            sub_ = 3;
            return Step::Wait;
        }
        if (ns.talkAt > subAt_ || ns.talkErrorAt >= subAt_ || ft_older(c.now, subAt_, kAnswerMs)) {
            const MonQuestInfo* q = info(w);
            std::string why = complete ? "the bag full or too heavy for the reward?" : "level / done its MaxCount times / once a day?";
            if (!complete && q && q->maxCount < 99 && m.run >= q->maxCount) why = fmt("done %d times, its MaxCount", q->maxCount);
            c.bot.note(c.now, fmt("monster quest: %s refused the %s (%s)", n->name.c_str(), complete ? "hand-in" : "take", why.c_str()));
            waitUntil_ = c.now + kRefusedWaitMs * kFtMs;
            sub_ = 3;
            if (++refusals_ >= kMaxRefusals) c.bot.stop(fmt("monster quest: refused %d times", refusals_));
            return Step::Wait;
        }
        status = complete ? "handing it in" : "taking the quest";
        return Step::Wait;
    }
    default:                                    // Exit: S2C 117 closes the client's window
        sub_ = 0;
        subAt_ = 0;
        if (!npcEid_ || ns.talkNpc != npcEid_) return Step::Wait;
        return c.act(act::npc_script(npcEid_, act::npcfn::Exit, {}, "close the dialog"));
    }
}

void QuestAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "questId", questId);
    botu::kv(o, key(), "npcKey", npcKey);
    botu::kv(o, key(), "huntZone", huntZone);
    botu::kv(o, key(), "usePortal", usePortal);
    botu::kv(o, key(), "repeat", repeat);
    botu::kv(o, key(), "maxRuns", maxRuns);
}

void QuestAction::load(const std::string& k, const std::string& v)
{
    if (k == "questId") questId = int(botu::num(v));
    else if (k == "npcKey") npcKey = v;
    else if (k == "huntZone") huntZone = int(botu::num(v));
    else if (k == "usePortal") usePortal = botu::flag(v);
    else if (k == "repeat") repeat = botu::flag(v);
    else if (k == "maxRuns") maxRuns = int(botu::num(v));
}

} // namespace nj
