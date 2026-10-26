// The bot's loop: observe, ask the actions in order, pace the output (bot.h).
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace nj {
namespace {

constexpr uint64_t kTickMs = 100;               // decisions at most every 100 ms
constexpr uint64_t kPaceMs = 300;               // one request per 300 ms
constexpr uint64_t kAttackedMs = 4000;          // a monster that swung at us this recently = a fight
constexpr uint64_t kChangeMs = 60000;           // a map change that never ended: stop waiting for it

bool attacked_now(const WorldModel& w, uint64_t now)
{
    for (const auto& kv : w.entities) {
        const Entity& e = kv.second;
        if (e.kind == EK_MONSTER && !e.dead && e.lastTarget == w.self.eid && !ft_older(now, e.lastSwingAt, kAttackedMs))
            return true;
    }
    return false;
}

} // namespace

bool BotCtx::sitting() const { return w.self.sit != 0; }

bool BotCtx::target_mob(uint16_t species) const
{
    return std::find(targetMobs.begin(), targetMobs.end(), species) != targetMobs.end();
}

Step BotCtx::cast(const Action& a, uint16_t skill)
{
    if (a.ok()) bot.gate.sent(w, skill, now);
    return act(a);
}

// ── black / whitelists ──────────────────────────────────────────────────────
void IdFilter::save(std::ostream& o, const char* key) const
{
    o << key << ".listMode=" << mode << '\n';
    for (uint32_t id : black) o << key << ".black=" << id << '\n';
    for (uint32_t id : white) o << key << ".white=" << id << '\n';
}

bool IdFilter::load(const std::string& k, const std::string& v)
{
    const uint32_t id = uint32_t(botu::num(v));
    if (k == "listMode") mode = botu::num(v) == OnlyWhite ? OnlyWhite : AllButBlack;
    else if (k == "black") { if (id) black.insert(id); }
    else if (k == "white") { if (id) white.insert(id); }
    else return false;
    return true;
}

// ── the skill gate ──────────────────────────────────────────────────────────
void SkillGate::sent(const WorldModel& w, uint16_t skill, uint64_t now)
{
    skill_ = skill;
    sentAt_ = now;
    // the answer comes after the cast Delay; a Chase skill may walk up to 5 s first (skill.cpp)
    const GameData* gd = w.data();
    const SkillInfo* si = gd ? gd->skill(skill) : nullptr;
    const SkillLevelInfo* li = gd ? gd->skill_level(skill, w.skill_level(skill)) : nullptr;
    const double secs = (li ? li->delay : 0.0) + (si && si->esaic == 2 ? 5.0 : 0.0) + 2.5;
    deadline_ = now + uint64_t(secs * 1e7);
}

bool SkillGate::resting(uint16_t skill, uint64_t now) const
{
    auto it = rest_.find(skill);
    return it != rest_.end() && now < it->second;
}

void SkillGate::observe(const WorldModel& w, uint64_t now, Bot& bot)
{
    if (!skill_) return;
    const SkillAnswers& a = w.skillAnswers;
    const std::string name = w.data() ? w.data()->skill_name(skill_) : fmt("skill %u", skill_);
    auto f = a.firedAt.find(skill_);
    uint64_t restMs = 0;
    if (f != a.firedAt.end() && f->second >= sentAt_) {
        restMs = 0;
    } else if (a.refusedAt >= sentAt_) {
        restMs = a.refusedWhy == 12 ? 1000 : 4000;
        bot.note(now, fmt("%s refused (%u%s)", name.c_str(), a.refusedWhy, a.refusedWhy == 12 ? ": cool time" : ""));
    } else if (a.cancelAt >= sentAt_) {
        restMs = 3000;
        bot.note(now, fmt("%s cancelled (out of reach?)", name.c_str()));
    } else if (now >= deadline_) {
        restMs = 5000;
        bot.note(now, fmt("%s: no answer, rests 5 s", name.c_str()));
    } else {
        return;                                 // still waiting
    }
    if (restMs) rest_[skill_] = now + restMs * kFtMs;
    skill_ = 0;
    doneAt = now;
}

bool skill_ready(const BotCtx& c, uint16_t skill, std::string* why)
{
    auto no = [&](const std::string& s) { if (why) *why = s; return false; };
    const int lv = c.w.skill_level(skill);
    if (!lv) return no("not learned");
    SkillEntry se{ skill, uint8_t(lv) };
    std::string u;
    if (!usable_skill(c.w, se, &u)) return no(u);
    // most buffs (SkillOption 0x200) are refused in a town / shop / inn map (skill.cpp: S2C 239 [15])
    if (const GameData* gd = c.w.data()) {
        const SkillInfo* si = gd->skill(skill);
        if (si && (si->skillOption & 0x200) && gd->no_skill_zone(c.w.self.zone)) return no("not in a town (SkillOption 0x200)");
    }
    if (c.bot.gate.resting(skill, c.now)) return no("resting after a refusal");
    const uint64_t ready = c.w.skill_ready_at(skill);
    if (ready && c.now < ready) return no(fmt("cool time %.1f s", double(ready - c.now) / 1e7));
    const SkillLevelInfo* li = c.w.data() ? c.w.data()->skill_level(skill, lv) : nullptr;
    if (li && li->mpCon > 0 && c.w.self.mp >= 0 && c.w.self.mp < li->mpCon) return no(fmt("needs %d MP", li->mpCon));
    if (li && li->hpCon > 0 && c.w.self.hp >= 0 && c.w.self.hp <= li->hpCon) return no(fmt("needs %d HP", li->hpCon));
    return true;
}

Bot::Bot() : rng_(std::random_device{}())
{
    order_ = { &heal, &town, &buffs, &keyq, &story, &quest, &hunt, &loot, &attack, &drill, &roam };
}

void Bot::sync_modes()
{
    if (drill.enabled && !wasDrill_) { attack.enabled = false; quest.enabled = hunt.enabled = keyq.enabled = story.enabled = false; }
    else if (keyq.enabled && !wasKeyq_) { drill.enabled = false; attack.enabled = true; quest.enabled = hunt.enabled = story.enabled = false; }
    else if (story.enabled && !wasStory_) { drill.enabled = false; attack.enabled = true; quest.enabled = hunt.enabled = keyq.enabled = false; }
    else if (quest.enabled && !wasQuest_) { drill.enabled = false; attack.enabled = true; hunt.enabled = keyq.enabled = story.enabled = false; }
    else if (hunt.enabled && !wasHunt_) { drill.enabled = false; attack.enabled = true; quest.enabled = keyq.enabled = story.enabled = false; }
    else if (attack.enabled && !wasAttack_) drill.enabled = false;
    else if (!attack.enabled && wasAttack_) quest.enabled = hunt.enabled = keyq.enabled = story.enabled = false;
    // several at once (a profile): the drill keeps its mode, then the key quest, the story, the quest, the hunt; they
    // bring the Attack
    if (drill.enabled) attack.enabled = quest.enabled = hunt.enabled = keyq.enabled = story.enabled = false;
    if (keyq.enabled) quest.enabled = hunt.enabled = story.enabled = false;
    if (story.enabled) quest.enabled = hunt.enabled = false;
    if (quest.enabled) hunt.enabled = false;
    if (quest.enabled || hunt.enabled || keyq.enabled || story.enabled) attack.enabled = true;
    wasDrill_ = drill.enabled;
    wasAttack_ = attack.enabled;
    wasQuest_ = quest.enabled;
    wasHunt_ = hunt.enabled;
    wasKeyq_ = keyq.enabled;
    wasStory_ = story.enabled;
}

void Bot::note(uint64_t now, const std::string& text)
{
    // the same line again: counted, not written — the count goes out at 10 / 100 / 1000 and before the next other line
    auto push = [&](const std::string& line) {
        log.push_back(filetime_local_hms(now) + line);
        ++notes;
    };
    auto shown = [](int n) { return n == 10 || n == 100 || n % 1000 == 0; };
    if (text == lastNote_) {
        if (shown(++noteRepeats_)) push(fmt(" (the line above again: %d times)", noteRepeats_));
    } else {
        if (noteRepeats_ && !shown(noteRepeats_)) push(fmt(" (the line above again: %d times)", noteRepeats_));
        lastNote_ = text;
        noteRepeats_ = 0;
        push(" " + text);
    }
    while (log.size() > 400) log.pop_front();
}

// Nothing sent for 30 s while it could act (not sitting, dead, changing map, digging): what each action says goes to
// the log (the file keeps it), then a light reset — the target left alone 2 min, every walk planned again, the stuck
// guard back on if it paused. (Live reports of "stands there and does not hunt" need that snapshot.)
void Bot::idle_watch(const BotCtx& c, bool digging)
{
    constexpr uint64_t kIdleMs = 30000;
    const WorldModel& w = c.w;
    if (w.self.dead || w.changingZone || c.sitting() || digging || town.recovering() || gate.busy()) return;
    if (!ft_older(c.now, std::max(lastAction_, startedAt), kIdleMs) || (idleNoteAt_ && !ft_older(c.now, idleNoteAt_, kIdleMs * 2))) return;
    idleNoteAt_ = c.now;
    std::string st;
    for (const BotAction* a : order_)
        if (a->enabled || !a->status.empty()) st += fmt("%s%s=%s%s", st.empty() ? "" : "; ", a->key(), a->enabled ? "" : "(off)", a->status.c_str());
    const SelfState::Drawn& d = w.self.drawn;
    note(c.now, fmt("idle 30 s (%s) at (%d,%d)%s, %zu monsters around, target %u: %s", doing.c_str(), c.px, c.py,
                    w.drawn_live(c.now) ? fmt(", drawn (%d,%d)", d.x, d.y).c_str() : "", w.entities.size(), attack.target, st.c_str()));
    attack.drop_target(c.now, 120000);
    for (BotAction* a : order_) a->preempted();
    town.nav.interrupted();
    if (guard.pausedUntil > c.now) { guard.pausedUntil = 0; note(c.now, "the stuck guard is back on"); }
}

uint64_t Bot::progress_sig(const WorldModel& w) const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(uint64_t(w.self.zone));
    mix(uint64_t(w.self.kills));
    mix(uint64_t(w.self.expGained));
    mix(uint64_t(w.self.tmGained));
    mix(uint64_t(w.self.money));
    for (const InvItem& it : w.inventory) mix(it.uid * 31 + it.count);
    mix(w.quests.takenAt); mix(w.quests.doneAt); mix(w.quests.stakenAt); mix(w.quests.sdoneAt); mix(w.quests.npcItemsAt);
    mix(w.monq.takenAt); mix(w.monq.doneAt); mix(uint64_t(w.monq.kills));
    mix(w.npc.exDoneAt);
    mix(uint64_t(w.drillStats.digs));
    return h;
}

// No progress for a minute while a goal mode runs (the drill's too: a dig is progress) and it could act (not sitting, dead, changing map, digging): what it
// is doing and what each action says go to the log, then a remedy that grows each time —
//   60 s  the errand under way dropped (its job left alone 5 min), the target left 2 min, an NPC dialog left open closed,
//         every walk planned again;
//   2 min a step aside: a short walk to an open point (the client and the server agree again, a corner is left) and the
//         item hunt's source given up;
//   3.5 min every action started over (the plans made again);
//   5 min the bot stops and says why.
// (A live log: 7 and 9 minutes without a line in it while requests went out.)
bool Bot::stall_watch(BotCtx& c, bool digging, Action& out)
{
    const WorldModel& w = c.w;
    const bool goal = keyq.enabled || story.enabled || quest.enabled || hunt.enabled || drill.enabled || town.trip();
    const uint64_t sig = progress_sig(w);
    bool newGround = w.self.zone == stallZone_;
    for (const auto& p : stallSeen_) newGround = newGround && botu::dist(c.px, c.py, p.first, p.second) > 150.0;
    if (!goal || sig != stallSig_ || w.self.zone != stallZone_ || !stallSince_ || w.self.dead || w.changingZone || c.sitting() ||
        digging || town.recovering()) {
        if (stallLevel_ && (sig != stallSig_ || w.self.zone != stallZone_)) note(c.now, "progress again");
        stallSig_ = sig;
        stallSince_ = c.now;
        stallSeen_.assign(1, { c.px, c.py });
        stallZone_ = w.self.zone;
        stallLevel_ = 0;
        return false;
    }
    if (newGround) {                                    // somewhere not stood on during this stall: the walk goes on
        if (stallLevel_) note(c.now, "progress again (new ground)");
        stallSeen_.push_back({ c.px, c.py });
        if (stallSeen_.size() > 16) stallSeen_.erase(stallSeen_.begin());
        stallSince_ = c.now;
        stallLevel_ = 0;
        return false;
    }
    static const uint64_t kAt[4] = { 60000, 120000, 210000, 300000 };
    if (stallLevel_ >= 4 || !ft_older(c.now, stallSince_, kAt[stallLevel_])) return false;
    const int level = ++stallLevel_;
    ++stalls;
    std::string st;
    for (const BotAction* a : order_)
        if (a->enabled || !a->status.empty()) st += fmt("%s%s=%s", st.empty() ? "" : "; ", a->key(), a->status.c_str());
    note(c.now, fmt("no progress %.0f s (%s) at (%d,%d) in %s, %zu entities around: %s", double(c.now - stallSince_) / 1e7, doing.c_str(),
                    c.px, c.py, w.data() ? w.data()->map_name(w.self.zone).c_str() : "?", w.entities.size(), st.c_str()));
    QuestRunner* runner = keyq.enabled ? static_cast<QuestRunner*>(&keyq) : story.enabled ? static_cast<QuestRunner*>(&story) : nullptr;
    if (level == 4) {
        stop(fmt("no progress for 5 min (%s)", doing.c_str()));
        return false;
    }
    for (BotAction* a : order_) a->preempted();
    town.nav.interrupted();
    attack.drop_target(c.now, 120000);
    if (level == 1) {
        if (runner) runner->stalled(c, 300000);
        note(c.now, "remedy 1: the errand / the target left alone, every walk planned again");
        if (w.npc.talkNpc) {                            // a dialog left open: the server's talk state swallows moves / skills
            out = act::npc_end();
            return true;
        }
        return false;
    }
    if (level == 2) {
        if (hunt.enabled) hunt.abandon_source(c, "no progress there");
        if (runner) runner->stalled(c, 300000);
        // a step aside: an open point 150..260 units away on an open line
        const MapGrid* g = w.data() ? w.data()->map(w.self.zone) : nullptr;
        std::uniform_real_distribution<double> ang(0.0, 6.283185307179586);
        for (int k = 0; k < 24; ++k) {
            const double a = ang(rng_), r = 150.0 + 110.0 * (k % 3) / 2.0;
            const int x = c.px + int(std::cos(a) * r), y = c.py + int(std::sin(a) * r);
            if (x < 0 || y < 0 || (g && (g->blocked(x, y) || !g->line_clear(c.px, c.py, x, y)))) continue;
            if (c.sitting()) { out = act::sit(0); return true; }
            out = act::move(w, x, y, run, c.now);
            if (!out.ok()) continue;
            stallSeen_.push_back({ x, y });             // (its own walk is no progress)
            note(c.now, fmt("remedy 2: a step aside to (%d,%d)", x, y));
            return true;
        }
        note(c.now, "remedy 2: no open point around to step to");
        return false;
    }
    // level 3: every action started over (the book, the switches and the settings stay)
    for (BotAction* a : order_) if (a != &town) a->reset();
    guard.reset();
    note(c.now, "remedy 3: every action started over");
    return false;
}

void Bot::start(const WorldModel& w, uint64_t now)
{
    running = true;
    travelOnly = false;
    w.self_pos_at(now, anchorX, anchorY);
    anchorZone = w.self.zone;
    for (BotAction* a : order_) { a->reset(); a->status.clear(); }
    gate.reset();
    guard.reset();
    reach.reset();
    lastAction_ = lastTick_ = idleNoteAt_ = 0;
    stallSince_ = 0;
    stallLevel_ = 0;
    stalls = 0;
    startedAt = now;
    expAtStart = w.self.expGained;
    moneyAtStart = w.self.money;
    doing = "starting";
    note(now, fmt("started at (%d,%d)", anchorX, anchorY));
}

void Bot::stop(const std::string& why, bool finished)
{
    if (!running) return;
    running = false;
    ++stopCount;
    stopWhy = why;
    stopFinished = finished;
    stopByUser = false;
    attack.target = 0;
    doing = "stopped: " + why;
    note(now_filetime(), "stopped: " + why);
}

bool Bot::tick(const WorldModel& w, uint64_t now, Action& out)
{
    sync_modes();
    if (!running) return false;
    if (!ft_older(now, lastTick_, kTickMs)) return false;
    lastTick_ = now;
    if (!w.inWorld || !w.self.eid) { doing = "waiting for the world"; return false; }
    // a map change: the client reconnects, nothing may go out until the new link's 437 / 157
    if (w.changingZone && !ft_older(now, w.zoneChangeAt, kChangeMs)) { doing = "changing map"; return false; }
    const bool townOn = town.enabled || town.trip();
    if (w.self.dead && !(townOn && town.onDeath)) { stop("you died"); return false; }

    BotCtx c{ w, now, 0, 0, attacked_now(w, now), *this, Action{} };
    w.self_pos_at(now, c.px, c.py);
    c.travelling = town.trip();
    if (quest.enabled && !c.travelling) {       // the quest's monster, in its map
        c.targetMode = true;
        c.targetHere = quest.hunting(w);
        c.targetMobs.assign(1, quest.mob(w));
        c.targetWhat = "monster quest";
    }
    town.keep.clear();
    if (hunt.enabled) {                         // the item hunt: its source's species in its map, or a dig there
        town.keep.insert(hunt.items().begin(), hunt.items().end());
        if (!c.travelling) {
            hunt.refresh(w, now, drill.digging(w, now));
            c.wanted = &hunt.items();
            const bool there = w.self.zone == hunt.source().zone;
            if (hunt.killing()) {
                c.targetMode = true;
                c.targetHere = there;
                c.targetMobs = hunt.source().mobs;
                c.targetWhat = "item hunt";
            } else if (hunt.digging()) {
                c.huntDig = there;
                c.targetMode = !there;              // (on the way: no fights, no loot)
                c.targetWhat = "item hunt";
            }
        }
    }
    // the key quest / the story: their monster quest's monster in its map, or their items' source
    QuestRunner* runner = keyq.enabled ? static_cast<QuestRunner*>(&keyq) : story.enabled ? static_cast<QuestRunner*>(&story) : nullptr;
    if (runner) {
        QuestRunner& qr = *runner;
        qr.gather.maxOver = hunt.maxOver;           // (the Item Hunt tab's "Monsters" limit)
        qr.keepGelder = town.keepGelder;            // (never spent below the Town's galder kept)
        qr.refresh(w, now, drill.digging(w, now), book);
        town.keep.insert(qr.items().begin(), qr.items().end());
        if (!c.travelling) {
            c.wanted = &qr.items();
            c.targetMode = !qr.idle();          // (on the way to its NPCs: only who hits us is fought; every job waiting:
                                                //  what is around is fought meanwhile)
            c.targetWhat = runner == &keyq ? "key quest" : "story";
            if (qr.monq_hunting(w)) {
                c.targetHere = w.self.zone == qr.monq_zone(w);
                c.targetMobs.assign(1, qr.monq_mob(w));
                c.targetWhat = runner == &keyq ? "key quest (monster quest)" : "story (monster quest)";
            } else if (qr.gathering()) {
                const ItemHuntAction& g = qr.gather;
                const bool there = w.self.zone == g.source().zone;
                if (g.killing()) {
                    c.targetHere = there;
                    c.targetMobs = g.source().mobs;
                } else if (g.digging()) {
                    c.huntDig = there;
                    c.targetMode = !there;
                }
            }
        }
    }
    c.drillMode = (drill.enabled || c.huntDig) && !c.travelling;
    // moved to another map without a trip (by hand, a warp): the start point is here now
    if (!c.travelling && anchorZone != w.self.zone) {
        anchorZone = w.self.zone;
        anchorX = c.px;
        anchorY = c.py;
    }
    // drill mode: the Attack answers who hits us (drill.fightBack) even when its own switch is off; the quest /
    // the hunt patrol their monsters' spawn spots (Roam), the hunt digs (Drill) and picks up its items (Loot)
    auto asked = [&](const BotAction* a) {
        return a->enabled || (a == &town && townOn) || (a == &attack && c.drillMode && drill.fightBack) ||
               (a == &roam && c.targetMode && c.targetHere) || (a == &drill && c.huntDig) ||
               (a == &loot && c.wanted && !c.travelling);
    };
    gate.observe(w, now, *this);
    guard.observe(c);                           // (before the actions: they see its event this tick)
    for (BotAction* a : order_)
        if (asked(a)) a->observe(c);
    if (!ft_older(now, lastAction_, kPaceMs)) return false;
    // a dig is open: a skill, a sit / stand, an attack or a walk would end it — only potions and the drill
    const bool digging = c.drillMode && drill.digging(w, now) && !w.self.dead;
    idle_watch(c, digging);
    if (stall_watch(c, digging, out)) {
        lastAction_ = now;
        doing = out.label;
        return true;
    }

    // stuck: the server is put back where the client draws us before anything else goes out
    if (guard.due() && !digging && !gate.busy()) {
        const Action a = guard.resync(c);
        for (BotAction* x : order_) x->preempted();
        town.nav.interrupted();
        lastAction_ = now;
        doing = a.label;
        if (!a.ok()) return false;
        out = a;
        return true;
    }

    for (size_t i = 0; i < order_.size(); ++i) {
        BotAction* a = order_[i];
        if (!asked(a)) continue;
        if (w.self.dead && a != &town) continue;  // dead: only the way to town
        if (gate.busy() && a != &heal && !w.self.dead) {   // a skill request out: only potions until its answer
            doing = "casting " + (w.data() ? w.data()->skill_name(gate.pending()) : fmt("skill %u", gate.pending()));
            return false;
        }
        if (digging && a != &heal && a != &drill) continue;
        const Step s = a->step(c);
        if (s == Step::Pass) continue;
        if (s == Step::Wait) {
            doing = a->status;
            return false;
        }
        for (size_t j = i + 1; j < order_.size(); ++j) order_[j]->preempted();
        lastAction_ = now;
        if (!c.out.ok()) {
            note(now, fmt("%s: 0x%04X refused: %s", a->title(), c.out.op, c.out.error.c_str()));
            return false;
        }
        out = c.out;
        doing = c.out.label;
        a->status = c.out.label;
        return true;
    }
    doing = "idle";
    return false;
}

std::string Bot::save_text() const
{
    std::ostringstream o;
    o << "# Trickster Automation Studio bot profile\n";
    o << "bot.run=" << run << "\n";
    for (const BotAction* a : order_) {
        o << a->key() << ".enabled=" << a->enabled << "\n";
        a->save(o);
    }
    return o.str();
}

void Bot::load_text(const std::string& text)
{
    // defaults first, so a missing key keeps its default
    heal = HealAction(); town = TownAction(); buffs = BuffAction(); quest = QuestAction();
    hunt = ItemHuntAction();
    keyq = KeyQuestAction();
    story = StoryAction();                      // (the book is the character's: kept; the app loads it per character)
    loot = LootAction(); attack = AttackAction(); drill = DrillAction(); roam = RoamAction();
    run = true;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('='), dot = line.find('.');
        if (eq == std::string::npos || dot == std::string::npos || dot > eq) continue;
        const std::string who = line.substr(0, dot), k = line.substr(dot + 1, eq - dot - 1), v = line.substr(eq + 1);
        if (who == "bot") {
            if (k == "run") run = botu::flag(v);
            continue;
        }
        for (BotAction* a : order_) {
            if (who != a->key()) continue;
            if (k == "enabled") a->enabled = botu::flag(v);
            else a->load(k, v);
        }
    }
    town.after_load();
    wasDrill_ = wasQuest_ = wasKeyq_ = wasStory_ = false;   // (as if switched on now: the drill wins, the quests bring the Attack)
    wasAttack_ = true;
    sync_modes();
}

} // namespace nj
