// Heal: a potion (ItemParam Type 10) when HP / MP drop below the %, and heal skills for HP. The server takes a
// potion only below the max (item_use.cpp sub_6E6120) and answers 0x24 / 0x25 (or 0x5A): we wait for a change.
// The heal skills, by the skill's ESAction (ProjectPAULA; each answers S2C 0x24 [u32 HP] when it healed):
//   1085 Cure (skill.cpp esa_heal; ESAUI 1 = C2S 0xE0 with our id): HP + MaxHP x Ratio.
//   1168 Catastrophe Healing (skill_dragon.cpp esa_first_aid; 0xEE): HP + (MA + LK) x 0.5 x Ratio.
//   1323 Heart's Grace (esa_wide_first_aid; 0xEE): the party within Radius, us included (alone: us), each
//        MaxHP x Ratio + our MA x Ratio x 40.
//   1482 Chakra Balance (skill_power.cpp esa_self_cure; 0xEE): ESASelfCureDo, Heal every Interval for Time, ended by
//        any swing at us, hit or miss (es.cpp 30002) -> only out of a fight, and not while it runs.
// One skill request at a time (the SkillGate: a pending one -> no heal skill, potions still go); a skill ends an
// open dig (drill.cpp) -> none then. After a heal skill no other one goes until HP moves or 1.5 s after its answer.
// Order: an HP potion below hpPct (when on), else a heal skill at or below skillPct, then the MP potion.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>

namespace nj {
namespace {

constexpr uint64_t kAnswerMs = 1500;
constexpr uint64_t kHealHoldMs = 1500;

int heal_esaction(const GameData* gd, uint16_t skill)
{
    const SkillInfo* si = gd ? gd->skill(skill) : nullptr;
    if (!si) return 0;
    switch (si->esAction) {
    case 1085: case 1168: case 1323: case 1482: return si->esAction;
    default: return 0;
    }
}

// Auto: the smallest potion that fills the gap, else the biggest one.
uint64_t pick_potion(const WorldModel& w, bool mp, uint32_t only)
{
    const GameData* gd = w.data();
    if (!gd) return 0;
    const int64_t gap = mp ? w.self.mpMax - w.self.mp : w.self.hpMax - w.self.hp;
    const InvItem* fit = nullptr;
    const InvItem* big = nullptr;
    int fitV = 0, bigV = 0;
    for (const InvItem& it : w.inventory) {
        if (!usable_item(w, it)) continue;
        const ItemInfo* ii = gd->item(it.itemId);
        if (!ii || ii->type != 10) continue;
        if (only && it.itemId != only) continue;
        const int v = mp ? ii->mp : ii->hp;
        if (v <= 0) continue;
        if (ii->minLevel > 0 && w.self.level > 0 && w.self.level < ii->minLevel) continue;
        if (v >= gap && (!fit || v < fitV)) { fit = &it; fitV = v; }
        if (!big || v > bigV) { big = &it; bigV = v; }
    }
    const InvItem* p = fit ? fit : big;
    return p ? p->uid : 0;
}

bool below(int64_t cur, int64_t max, int pct) { return cur >= 0 && max > 0 && cur < max && botu::pct(cur, max) < pct; }

} // namespace

bool HealAction::heal_skill(const GameData* gd, uint16_t skill) { return heal_esaction(gd, skill) != 0; }

const char* HealAction::heal_kind(const GameData* gd, uint16_t skill)
{
    switch (heal_esaction(gd, skill)) {
    case 1085: return "MaxHP x Ratio";
    case 1168: return "(MA + LK) / 2 x Ratio";
    case 1323: return "MaxHP x Ratio + MA x Ratio x 40, the party too";
    case 1482: return "over time, ends when a monster swings at you";
    default:   return "";
    }
}

bool HealAction::uses(uint16_t skill) const { return std::find(skills.begin(), skills.end(), skill) != skills.end(); }

void HealAction::set_use(uint16_t skill, bool on)
{
    auto it = std::find(skills.begin(), skills.end(), skill);
    if (on && it == skills.end()) skills.push_back(skill);
    else if (!on && it != skills.end()) skills.erase(it);
}

// A heal skill at or below skillPct: the first checked one that is ready.
Step HealAction::skill_step(BotCtx& c)
{
    const SelfState& me = c.w.self;
    if (skills.empty() || me.hp < 0 || me.hpMax <= 0 || me.hp >= me.hpMax || botu::pct(me.hp, me.hpMax) > skillPct)
        return Step::Pass;
    if (c.w.npc.talkNpc && !ft_older(c.now, c.w.npc.talkAt, 10000)) return Step::Pass;   // (an NPC's dialog open: potions only)
    if (castAt_) {                                          // the last one: its answer, then the HP it gives
        if (me.hp != castHp_ || ft_older(c.now, std::max(castAt_, c.bot.gate.doneAt), kHealHoldMs)) castAt_ = 0;
        else return Step::Pass;
    }
    if (c.bot.gate.busy()) return Step::Pass;               // (another skill's answer first)
    if (c.drillMode && c.bot.drill.digging(c.w, c.now)) return Step::Pass;   // a skill would end the dig
    const GameData* gd = c.w.data();
    std::string first;
    for (uint16_t sk : skills) {
        const int esa = heal_esaction(gd, sk);
        if (!esa) continue;
        const std::string name = gd ? gd->skill_name(sk) : fmt("skill %u", sk);
        if (esa == 1482 && c.attacked) { if (first.empty()) first = name + ": in a fight"; continue; }
        if (esa == 1482 && c.w.effect_of(sk, c.now)) continue;
        std::string why;
        if (!skill_ready(c, sk, &why)) {
            if (first.empty()) first = name + ": " + why;
            continue;
        }
        if (c.sitting()) return c.act(act::sit(0));
        SkillTarget t;
        t.mode = SkillTarget::Self;
        castAt_ = c.now;
        castHp_ = me.hp;
        return c.cast(act::use_skill(c.w, sk, t, c.now), sk);
    }
    if (!first.empty()) status = "HP low, " + first;
    return Step::Pass;
}

Step HealAction::step(BotCtx& c)
{
    const SelfState& me = c.w.self;
    if (waiting_) {
        if (me.hp != waitHp_ || me.mp != waitMp_ || c.now >= waitUntil_) waiting_ = false;
        else { status = "waiting for the potion"; return Step::Wait; }
    }
    if (c.bot.town.recovering() && !c.attacked) { status = "resting in town"; return Step::Pass; }   // free there
    status = "ok";
    auto potion = [&](bool mp) {
        const uint64_t uid = pick_potion(c.w, mp, mp ? mpItem : hpItem);
        if (!uid) {
            status = mp ? "MP low, no MP potion" : "HP low, no HP potion";
            uint64_t& at = mp ? noMpNoteAt_ : noHpNoteAt_;        // (once a minute: it came every tick, 10 lines a second)
            if (!at || ft_older(c.now, at, 60000)) { at = c.now; c.bot.note(c.now, status); }
            return Step::Pass;
        }
        waiting_ = true;
        waitHp_ = me.hp;
        waitMp_ = me.mp;
        waitUntil_ = c.now + kAnswerMs * kFtMs;
        return c.act(act::use_item(c.w, uid));
    };
    if (hpPotions && below(me.hp, me.hpMax, hpPct)) {
        const Step s = potion(false);
        if (s != Step::Pass) return s;
    }
    const Step s = skill_step(c);
    if (s != Step::Pass) return s;
    if (below(me.mp, me.mpMax, mpPct)) return potion(true);
    return Step::Pass;
}

void HealAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "hpPct", hpPct);
    botu::kv(o, key(), "mpPct", mpPct);
    botu::kv(o, key(), "hpItem", hpItem);
    botu::kv(o, key(), "mpItem", mpItem);
    botu::kv(o, key(), "hpPotions", hpPotions);
    botu::kv(o, key(), "skillPct", skillPct);
    for (uint16_t sk : skills) botu::kv(o, key(), "skill", sk);
}

void HealAction::load(const std::string& k, const std::string& v)
{
    if (k == "hpPct") hpPct = int(botu::num(v));
    else if (k == "mpPct") mpPct = int(botu::num(v));
    else if (k == "hpItem") hpItem = uint32_t(botu::num(v));
    else if (k == "mpItem") mpItem = uint32_t(botu::num(v));
    else if (k == "hpPotions") hpPotions = botu::flag(v);
    else if (k == "skillPct") skillPct = std::clamp(int(botu::num(v)), 5, 99);
    else if (k == "skill") {
        const uint16_t sk = uint16_t(botu::num(v));
        if (sk) set_use(sk, true);
    }
}

} // namespace nj
