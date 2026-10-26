// Buffs: the checked skills are kept up on ourselves (0xEE, or 0xE0 with our id). Whether one is up
// comes from the effects the model tracks (S2C 224 / 280 / 850); one is cast again when its effect is
// gone or ends within `beforeSec`, once its cool time and MP allow. A cast that fired (S2C 240) but
// never showed an effect (an ES with no 224, e.g. ESAStatUpScroll) counts as up for its row's Seconds,
// else `untrackedSec`.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <sstream>

namespace nj {

bool BuffAction::kept(uint16_t skill) const { return std::find(keep.begin(), keep.end(), skill) != keep.end(); }

void BuffAction::set_kept(uint16_t skill, bool on)
{
    auto it = std::find(keep.begin(), keep.end(), skill);
    if (on && it == keep.end()) keep.push_back(skill);
    else if (!on && it != keep.end()) keep.erase(it);
}

void BuffAction::observe(BotCtx& c)
{
    for (uint16_t sk : keep)
        if (c.w.effect_of(sk, c.now)) seen_[sk] = c.now;
}

Step BuffAction::step(BotCtx& c)
{
    if (keep.empty()) { status = "nothing checked"; return Step::Pass; }
    // an NPC's dialog open: the talk state answers a skill with S2C 239 (npc_talk.cpp) — after it
    if (c.w.npc.talkNpc && !ft_older(c.now, c.w.npc.talkAt, 10000)) { status = "an NPC's dialog is open"; return Step::Pass; }
    const GameData* gd = c.w.data();
    std::string waiting;
    for (uint16_t sk : keep) {
        const std::string name = gd ? gd->skill_name(sk) : fmt("skill %u", sk);
        if (const SkillInfo* si = gd ? gd->skill(sk) : nullptr; si && (si->affectionRel & 32)) continue;   // (an enemy's: the Attack's)
        if (const Effect* e = c.w.effect_of(sk, c.now)) {
            if (!e->endAt || e->endAt > c.now + uint64_t(std::max(0, beforeSec)) * 10000000ull) continue;
        } else {
            auto f = c.w.skillAnswers.firedAt.find(sk);
            const uint64_t fired = f == c.w.skillAnswers.firedAt.end() ? 0 : f->second;
            auto s = seen_.find(sk);
            const uint64_t seen = s == seen_.end() ? 0 : s->second;
            if (fired && seen < fired) {                    // fired, no effect seen since
                if (!ft_older(c.now, fired, 3000)) continue;    // its 224 may still come
                const SkillLevelInfo* li = gd ? gd->skill_level(sk, c.w.skill_level(sk)) : nullptr;
                const int life = li && li->seconds > 0 ? int(li->seconds) : untrackedSec;
                if (!ft_older(c.now, fired, uint64_t(std::max(5, life - beforeSec)) * 1000)) continue;
            }
        }
        std::string why;
        if (!skill_ready(c, sk, &why)) {
            if (waiting.empty()) waiting = name + ": " + why;
            continue;
        }
        if (c.sitting()) return c.act(act::sit(0));
        SkillTarget t;
        t.mode = SkillTarget::Self;
        return c.cast(act::use_skill(c.w, sk, t, c.now), sk);
    }
    status = waiting.empty() ? "all up" : waiting;
    return Step::Pass;
}

void BuffAction::save(std::ostream& o) const
{
    for (uint16_t sk : keep) botu::kv(o, key(), "keep", sk);
    botu::kv(o, key(), "beforeSec", beforeSec);
    botu::kv(o, key(), "untrackedSec", untrackedSec);
}

void BuffAction::load(const std::string& k, const std::string& v)
{
    if (k == "keep" || k == "buff") {               // (buff = the old "skill,interval" lines)
        const uint16_t sk = uint16_t(botu::num(v));
        if (sk) set_kept(sk, true);
    } else if (k == "beforeSec") {
        beforeSec = std::clamp(int(botu::num(v)), 0, 60);
    } else if (k == "untrackedSec") {
        untrackedSec = std::clamp(int(botu::num(v)), 5, 3600);
    }
}

} // namespace nj
