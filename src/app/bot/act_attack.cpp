// Attack: pick a monster, then the skill rotation and / or what is in the weapon slot.
//   Rotation: the first skill of the list that is ready (learned, off its cool time from its last
//   S2C 240, MP / HP for its cost, not resting after a refusal) is cast at the target (0xE0 / 0xEE,
//   one request at a time through the skill gate). A Chase skill (ESAIC 2) is sent from anywhere: the
//   server walks into its Range. Any other one needs us within its ESAII Range (a self-centred one,
//   ESAUI 0, within 150 when its row has none): the bot walks there first (0x18).
//   Basic attack off = skills only: with nothing ready the bot stays near the target and waits.
//   An area skill (its ESA row's Radius, skill_magic.cpp: Mana Ring = round the monster it is aimed at, at most 7, the
//   damage less toward the edge; Magic Storm = round a point of the ground; a self-centred one = round us) is aimed where
//   it takes the most monsters (the huntable ones near us), and cast only when that many reach its "min mobs".
//   Target: one that swung at us first, else the nearest within the radius (around us when roaming,
//   else around the start point), among the species the black / whitelist allows (one that hits us is
//   fought anyway when `fightBack` is on). Shadow monsters (16000..16999) only with the Third Eye up:
//   without it the server ignores the attack (game_session.cpp) and a shadow target is dropped when it
//   ends. Skipped if set: monsters another player owns (S2C 0x016D). A target whose HP does not drop
//   for 12 s is left for 60 s. After a kill it waits 700 ms so Loot sees the drops first.
//   Drill mode (DrillAction on): no hunting; only a monster that swings at us, and only with the Drill tab's
//   "fight back" (whatever the lists say) — else nothing at all.
//   Monster quest mode (QuestAction on): only in the quest's hunt map while its kills are still to do, and
//   only the quest's monster (whatever the lists say) plus, with "fight back", whoever hits us there.
//   Melee: C2S 0x1D; the server chases and swings until it dies; sent again when we have not swung for
//   4 s. With a gun in the weapon slot (SubType 54) the server ignores 0x1D (gun.cpp), so:
//   Gun (only with bullets): C2S 0x1DD, ONE shot per request. Each request is keyed with the last seed
//   (S2C 0x0200) and must come at least the FastShot time after the last shot (and after the weapon
//   change / login, when the server starts that timer); a bad one gets NO answer and three in a row
//   cut the player. So: a new seed after every request before the next one, the interval + 250 ms,
//   and the first request that gets no new seed turns the gun off for the run. Empty magazine with
//   bullets in the bag -> C2S 0x205 loads them; no bullets at all -> no attack.
//   Walls: the server's chase is a straight line (through walls), the client's avatar path-finds it and stays
//   behind when it finds no way (stuck.cpp). So a target is chosen by the WALK to it on the map's grid: one in
//   another area, or round a long wall (walk > 3 x its distance + 200), is left alone (an attacker within 96
//   is fought wherever it stands); with a wall on the straight line the bot walks the grid path itself (0x18
//   legs) and attacks once the line is open; a target the client could not walk to (StuckGuard) is left 2 min.
#include "app/bot/bot.h"
#include "app/bot/bot_util.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>

namespace nj {
namespace {

constexpr uint64_t kAttackedMs = 4000;
constexpr uint64_t kResendMs = 4000;
constexpr uint64_t kNoProgressMs = 12000;
constexpr uint64_t kBanMs = 60000;
constexpr uint64_t kLootDelayMs = 700;
constexpr uint64_t kGunMarginMs = 250;
constexpr uint64_t kSeedTimeoutMs = 2500;
constexpr uint64_t kChaseResendMs = 2000;
constexpr int      kNearUnits = 96;             // an attacker this close is fought wherever it stands
constexpr double   kDetourRatio = 3.0;          // a walk longer than 3 x the distance + 200:
constexpr double   kDetourSlack = 200.0;        //   the target lies behind a long wall
constexpr uint64_t kUnreachBanMs = 120000;      // a target the client could not walk to
constexpr uint64_t kPathMaxMs = 30000;          // the walk round a wall to one target

} // namespace

constexpr int kSelfAoeRange = 150;              // a self-centred skill with no Range in its row
constexpr uint64_t kApproachResendMs = 700;

// The distance a skill needs to the target; 0 = from anywhere (the server chases into its Range).
static int skill_reach(const WorldModel& w, uint16_t skill)
{
    const GameData* gd = w.data();
    const SkillInfo* si = gd ? gd->skill(skill) : nullptr;
    if (!si || si->esaic == 2) return 0;
    const SkillLevelInfo* li = gd->skill_level(skill, w.skill_level(skill));
    const int range = li ? li->range : 0;
    if (si->esaui == 0) return range > 0 ? range : kSelfAoeRange;
    return range;
}

float AttackAction::area_radius(const WorldModel& w, uint16_t skill)
{
    const GameData* gd = w.data();
    const SkillLevelInfo* li = gd ? gd->skill_level(skill, w.skill_level(skill)) : nullptr;
    return li && li->radius > 0.0f ? li->radius : 0.0f;
}

AreaSpot AttackAction::area_spot(const BotCtx& c, uint16_t skill) const
{
    AreaSpot best;
    const GameData* gd = c.w.data();
    const SkillInfo* si = gd ? gd->skill(skill) : nullptr;
    const float r = area_radius(c.w, skill);
    if (!si || r <= 0.0f) return best;
    const int reach = skill_reach(c.w, skill);
    // the monsters it may take: huntable (or hitting us), alive, near enough to matter
    std::vector<const Entity*> mobs;
    const double around = (reach > 0 ? reach : 400) + r + 150.0;
    for (const auto& kv : c.w.entities) {
        const Entity& e = kv.second;
        if (e.kind != EK_MONSTER || e.dead || e.hp == 0) continue;
        const bool attacker = e.lastTarget == c.w.self.eid && !ft_older(c.now, e.lastSwingAt, kAttackedMs);
        if (!huntable(c, e.eid, attacker)) continue;
        if (botu::dist(e.x, e.y, c.px, c.py) > around) continue;
        mobs.push_back(&e);
    }
    auto count_at = [&](int x, int y) {
        int n = 0;
        for (const Entity* m : mobs) n += botu::dist(m->x, m->y, x, y) <= r;
        return n;
    };
    if (si->esaui == 0) {                               // round us
        best.x = c.px;
        best.y = c.py;
        best.count = count_at(c.px, c.py);
        return best;
    }
    auto better = [&](int n, double d) { return n > best.count || (n == best.count && d < botu::dist(best.x, best.y, c.px, c.py)); };
    if (esaui_object(si->esaui)) {                      // round the monster it is aimed at
        for (const Entity* m : mobs) {
            const int n = count_at(m->x, m->y);
            const double d = botu::dist(m->x, m->y, c.px, c.py);
            if (best.count == 0 || better(n, d) || (n == best.count && m->eid == target)) {
                best.eid = m->eid; best.x = m->x; best.y = m->y; best.count = n;
            }
        }
        return best;
    }
    // a point of the ground: each monster, and each midpoint of two near ones
    for (size_t i = 0; i < mobs.size(); ++i)
        for (size_t j = i; j < mobs.size(); ++j) {
            if (j != i && botu::dist(mobs[i]->x, mobs[i]->y, mobs[j]->x, mobs[j]->y) > 2.0 * r) continue;
            const int x = (mobs[i]->x + mobs[j]->x) / 2, y = (mobs[i]->y + mobs[j]->y) / 2;
            const int n = count_at(x, y);
            if (best.count == 0 || better(n, botu::dist(x, y, c.px, c.py))) { best.x = x; best.y = y; best.count = n; }
        }
    return best;
}

void AttackAction::reset()
{
    target = 0;
    kills = 0;
    lastKillAt = 0;
    gunOff = false;
    banned_.clear();
    progressAt_ = meleeAt_ = approachAt_ = shotAt_ = loadAt_ = seedAskAt_ = 0;
    seedsAtShot_ = shotsAtShot_ = 0;
    waitSeed_ = false;
    targetHp_ = -1;
    nav_.reset();
    pathing_ = false;
    pathSince_ = pathCapMs_ = stuckSeen_ = 0;
    bestDist_ = 1e18;
}

bool AttackAction::engaged(const WorldModel& w) const
{
    if (!target) return false;
    auto it = w.entities.find(target);
    return it != w.entities.end() && !it->second.dead;
}

bool AttackAction::huntable(const BotCtx& c, uint16_t eid, bool attacker) const
{
    auto it = c.w.entities.find(eid);
    if (it == c.w.entities.end()) return false;
    const Entity& e = it->second;
    if (e.kind != EK_MONSTER || e.dead || e.hp == 0) return false;
    if (botu::shadow_monster(e.species) && !c.w.third_eye(c.now)) return false;
    if (c.targetMode) {                             // the quest's / hunt's monsters (whatever the lists say), and who hits us
        if (!c.target_mob(e.species) && !(attacker && fightBack)) return false;
    } else if (!(attacker && (fightBack || c.drillMode)) && !mobs.allows(e.species)) return false;
    if (skipOwned && e.owner && e.owner != c.w.self.eid) return false;
    auto bad = banned_.find(eid);
    return bad == banned_.end() || c.now >= bad->second;
}

uint16_t AttackAction::choose(const BotCtx& c) const
{
    const bool roaming = c.bot.roam.enabled && c.bot.roam.mode != RoamAction::Stay;
    const int cx = roaming ? c.px : c.bot.anchorX, cy = roaming ? c.py : c.bot.anchorY;
    // the walk there (Reach: the map's grid); < 0 = another area or round a long wall
    auto walk = [&](const Entity& e, bool attacker) {
        const double d = botu::dist(e.x, e.y, c.px, c.py);
        if (attacker && d <= kNearUnits) return d;
        const double wd = c.bot.reach.walk(c, e.x, e.y);
        if (c.targetMode) return wd;                // the quest's / hunt's monster anywhere: the nearest by the walk
        return wd < 0 || wd > d * kDetourRatio + kDetourSlack ? -1.0 : wd;
    };
    uint16_t best = 0;
    double bestD = 1e18;
    for (const auto& kv : c.w.entities) {            // whoever is hitting us, wherever it is
        const Entity& e = kv.second;
        if (e.lastTarget != c.w.self.eid || ft_older(c.now, e.lastSwingAt, kAttackedMs) || !huntable(c, e.eid, true)) continue;
        const double d = walk(e, true);
        if (d >= 0 && d < bestD) { bestD = d; best = e.eid; }
    }
    if (best || c.travelling || c.drillMode) return best;     // on a trip / drilling: only who attacks us
    for (const auto& kv : c.w.entities) {
        const Entity& e = kv.second;
        // (the quest / hunt mode looks over the whole map: the server shows every monster of it — a map is one sector)
        if (!huntable(c, e.eid) || (!c.targetMode && botu::dist(e.x, e.y, cx, cy) > radius)) continue;
        const double d = walk(e, false);
        if (d >= 0 && d < bestD) { bestD = d; best = e.eid; }
    }
    return best;
}

void AttackAction::observe(BotCtx& c)
{
    if (waitSeed_) {
        if (c.w.self.gunSeeds > seedsAtShot_) {
            waitSeed_ = false;
        } else if (ft_older(c.now, shotAt_, kSeedTimeoutMs)) {
            waitSeed_ = false;
            gunOff = true;
            c.bot.note(c.now, "a shot got no answer (no new seed): gun attack off for this run");
        }
    }
    const StuckGuard::Event& se = c.bot.guard.last();
    if (se.at && se.at != stuckSeen_) {                 // the client could not walk where the server took us
        stuckSeen_ = se.at;
        if (target && se.target == target) {
            c.bot.note(c.now, fmt("%s: the client could not walk there (a wall): left for 2 min", c.w.entity_label(target).c_str()));
            banned_[target] = c.now + kUnreachBanMs * kFtMs;
            target = 0;
        }
        pathing_ = false;
        nav_.interrupted();
    }
    if (!target) return;
    auto it = c.w.entities.find(target);
    if (it == c.w.entities.end() || it->second.dead) {
        const bool died = it != c.w.entities.end();
        if (died) { ++kills; lastKillAt = c.now; }
        c.bot.note(c.now, fmt("%s %s", died ? "killed" : "lost", c.w.entity_label(target).c_str()));
        target = 0;
        return;
    }
    const Entity& t = it->second;
    if (t.hp >= 0) {
        if (targetHp_ >= 0 && t.hp < targetHp_) progressAt_ = c.now;
        targetHp_ = t.hp;
    }
    if (t.selfHitAt > progressAt_ && t.selfHitAt <= c.now) progressAt_ = t.selfHitAt;
    const double near = botu::dist(t.x, t.y, c.px, c.py);    // getting nearer is progress too (a far target)
    if (near < bestDist_ - 32.0) { bestDist_ = near; progressAt_ = c.now; }
    if (botu::shadow_monster(t.species) && !c.w.third_eye(c.now)) {
        c.bot.note(c.now, fmt("the Third Eye is off: %s (shadow) left", c.w.entity_label(target).c_str()));
        target = 0;
    } else if (skipOwned && t.owner && t.owner != c.w.self.eid) {
        c.bot.note(c.now, fmt("%s is another player's: left", c.w.entity_label(target).c_str()));
        banned_[target] = c.now + kBanMs * kFtMs;
        target = 0;
    } else if (ft_older(c.now, progressAt_, kNoProgressMs)) {
        c.bot.note(c.now, fmt("no progress on %s for 12 s: left for 60 s", c.w.entity_label(target).c_str()));
        banned_[target] = c.now + kBanMs * kFtMs;
        target = 0;
    }
}

Step AttackAction::step(BotCtx& c)
{
    if (c.drillMode && !c.bot.drill.fightBack) {    // drilling: no fights at all
        status = "drill mode: no fights";
        target = 0;
        return Step::Pass;
    }
    if (c.targetMode && !c.targetHere) {            // monster quest / item hunt: fights only in its map, while there is work
        status = std::string(c.targetWhat) + ": fights only in its map";
        target = 0;
        return Step::Pass;
    }
    if (c.bot.loot.enabled && lastKillAt && !ft_older(c.now, lastKillAt, kLootDelayMs)) {
        status = "waiting for the drops";
        return Step::Wait;
    }

    // what can we hit with? (a gun in the weapon slot makes the server ignore melee)
    uint32_t gunId = 0;
    const GunInfo* gun = equipped_gun(c.w, &gunId);
    const bool gunMode = basic && gunId != 0;
    bool anySkill = false;
    for (const RotationSkill& r : rotation) anySkill |= r.on && r.skill;
    if (!basic && !anySkill) { status = "basic attack off and no skill in the rotation"; target = 0; return Step::Pass; }
    if (gunMode) {
        const char* why = !useGun ? "a gun is in the weapon slot and gun attack is off"
                        : gunOff  ? "gun attack off (a shot got no answer)"
                        : !gun    ? "the gun has no Item_GunParam2 row" : nullptr;
        if (why) { status = why; target = 0; return Step::Pass; }
        if (loaded_bullets(c.w) < 1) {
            const uint64_t spare = spare_bullets(c.w);
            if (!spare) {
                status = "no bullets: the gun cannot attack";
                c.bot.note(c.now, status);
                target = 0;
                return Step::Pass;
            }
            if (ft_older(c.now, loadAt_, 2000)) {
                loadAt_ = c.now;
                return c.act(act::load_bullets(c.w, spare));
            }
            status = "loading bullets";
            return Step::Wait;
        }
        if (!c.w.self.gunSeeds) {
            if (ft_older(c.now, seedAskAt_, 3000)) {
                seedAskAt_ = c.now;
                return c.act(act::gun_seed_request());
            }
            status = "waiting for a gun seed";
            return Step::Wait;
        }
    }

    if (!target) {
        target = choose(c);
        if (!target) { status = "no monster in range"; return Step::Pass; }
        progressAt_ = c.now;
        auto it = c.w.entities.find(target);
        targetHp_ = it != c.w.entities.end() ? it->second.hp : -1;
        meleeAt_ = 0;
        shotAt_ = 0;
        pathing_ = false;
        nav_.interrupted();
        bestDist_ = 1e18;
        c.bot.note(c.now, fmt("target %s", c.w.entity_label(target).c_str()));
    }
    if (c.sitting()) return c.act(act::sit(0));
    const Entity& t = c.w.entities.at(target);
    const std::string name = c.w.entity_label(target);

    // a wall on the straight line: round it first (the server's chase would go through it, the client not)
    if (!waitSeed_) {
        const Step ps = path_step(c, t);
        if (ps != Step::Pass) return ps;
        if (!target) return Step::Wait;
    }

    if (!waitSeed_) {
        const Step rs = rotation_step(c, t);
        if (rs != Step::Pass) return rs;
    }

    if (!basic) {                               // skills only: keep within reach of the nearest-reaching one
        int reach = 0;
        for (const RotationSkill& r : rotation) {
            if (!r.on || !r.skill || !c.w.skill_level(r.skill)) continue;
            const int k = skill_reach(c.w, r.skill);
            if (k > 0 && (reach == 0 || k < reach)) reach = k;
        }
        if (reach > 0 && botu::dist(t.x, t.y, c.px, c.py) > reach) return approach(c, t, reach, "closing in");
        status = fmt("%s: waiting for a skill", name.c_str());
        return Step::Wait;
    }

    if (gunMode) {
        if (waitSeed_) { status = "waiting for the shot's answer"; return Step::Wait; }
        const uint64_t interval = uint64_t(gun_shot_interval_ms(*gun)) + kGunMarginMs;
        const uint64_t timer = std::max(c.w.self.lastShotAt, c.w.self.weaponAt);
        if (!ft_older(c.now, timer, interval)) { status = fmt("shooting %s (reload)", name.c_str()); return Step::Wait; }
        const bool fired = c.w.self.lastShotAt > shotsAtShot_;      // our last request has been shot
        if (shotAt_ && !fired && !ft_older(c.now, shotAt_, kChaseResendMs)) {
            status = fmt("closing in on %s", name.c_str());
            return Step::Wait;
        }
        shotAt_ = c.now;
        seedsAtShot_ = c.w.self.gunSeeds;
        shotsAtShot_ = c.w.self.lastShotAt;
        waitSeed_ = true;
        return c.act(act::gun_shot(c.w, target, c.bot.run));
    }

    // (again right after a skill: the skill took the server's attention off the swing)
    if (!meleeAt_ || c.bot.gate.doneAt > meleeAt_ ||
        (ft_older(c.now, meleeAt_, kResendMs) && ft_older(c.now, t.selfHitAt, kResendMs))) {
        meleeAt_ = c.now;
        return c.act(act::attack(c.w, target, c.bot.run));
    }
    status = fmt("fighting %s", name.c_str());
    return Step::Wait;
}

Step AttackAction::rotation_step(BotCtx& c, const Entity& t)
{
    const double d = botu::dist(t.x, t.y, c.px, c.py);
    for (const RotationSkill& r : rotation) {
        if (!r.on || !r.skill || !skill_ready(c, r.skill)) continue;
        const int reach = skill_reach(c.w, r.skill);
        if (area_radius(c.w, r.skill) > 0.0f) {         // an area skill: where it takes the most, if enough
            const AreaSpot sp = area_spot(c, r.skill);
            if (sp.count < std::max(1, r.minTargets)) continue;
            const double ds = botu::dist(sp.x, sp.y, c.px, c.py);
            const SkillInfo* si = c.w.data()->skill(r.skill);
            if (si && si->esaui != 0 && reach > 0 && ds > reach) {
                if (basic) continue;
                return approach(c, t, reach, c.w.data()->skill_name(r.skill).c_str());
            }
            SkillTarget st;
            if (si && esaui_object(si->esaui)) { st.mode = SkillTarget::Entity; st.eid = sp.eid ? sp.eid : target; }
            else if (si && esaui_point(si->esaui)) { st.mode = SkillTarget::Point; st.x = sp.x; st.y = sp.y; }
            else { st.mode = SkillTarget::Entity; st.eid = target; }
            const Action a = act::use_skill(c.w, r.skill, st, c.now);
            if (!a.ok()) {
                c.bot.note(c.now, fmt("rotation skill %u refused: %s", r.skill, a.error.c_str()));
                continue;
            }
            return c.cast(a, r.skill);
        }
        if (reach > 0 && d > reach) {
            // the first ready skill is out of reach: walk there, unless the basic attack closes in anyway
            if (basic) continue;
            return approach(c, t, reach, c.w.data() ? c.w.data()->skill_name(r.skill).c_str() : "skill");
        }
        SkillTarget st;
        st.mode = SkillTarget::Entity;
        st.eid = target;
        const Action a = act::use_skill(c.w, r.skill, st, c.now);
        if (!a.ok()) {
            c.bot.note(c.now, fmt("rotation skill %u refused: %s", r.skill, a.error.c_str()));
            continue;
        }
        return c.cast(a, r.skill);
    }
    return Step::Pass;
}

// The server chases in a straight line (game_session.cpp player_order), the client's avatar path-finds it and
// stays behind a wall it finds no way round (stuck.cpp). So while the line to the target crosses a wall the bot
// walks the grid path itself — 0x18 legs, each an open line the client walks at once — and attacks from where
// the line is open. No way there / 30 s of walking = the target is left for 2 min.
Step AttackAction::path_step(BotCtx& c, const Entity& t)
{
    // (one hitting us from close by is fought where it stands, as choose() takes it)
    const bool closeAttacker = t.lastTarget == c.w.self.eid && !ft_older(c.now, t.lastSwingAt, kAttackedMs) &&
                               botu::dist(t.x, t.y, c.px, c.py) <= kNearUnits;
    if (closeAttacker || Reach::straight(c, t.x, t.y)) {
        if (pathing_) { pathing_ = false; nav_.interrupted(); }
        return Step::Pass;
    }
    const std::string name = c.w.entity_label(target);
    const double wd = c.bot.reach.walk(c, t.x, t.y);
    if (!pathing_) {
        pathing_ = true;
        pathSince_ = c.now;
        const int speed = c.w.self.speed > 0 ? c.w.self.speed : 160;     // (a long way: twice its time + 10 s)
        pathCapMs_ = std::max<uint64_t>(kPathMaxMs, uint64_t(std::max(0.0, wd) * 2000.0 / speed) + 10000);
        nav_.interrupted();
        c.bot.note(c.now, fmt("%s: a wall between us, walking round it", name.c_str()));
    }
    if (wd < 0 || ft_older(c.now, pathSince_, pathCapMs_)) {
        c.bot.note(c.now, fmt("%s: no way round the wall: left for 2 min", name.c_str()));
        banned_[target] = c.now + kUnreachBanMs * kFtMs;
        target = 0;
        pathing_ = false;
        status = "no way to the target";
        return Step::Wait;
    }
    // the target on a 32-unit grid: a wandering monster does not make a new path every tick
    const int qx = (t.x / 32) * 32 + 16, qy = (t.y / 32) * 32 + 16;
    const Step s = nav_.walk_to(c, qx, qy, 40, name + ": walking round a wall");
    if (s == Step::Pass) return Step::Pass;     // next to it: the server chases the rest
    status = nav_.status;
    if (s == Step::Act) progressAt_ = c.now;    // a leg out is progress (the 12 s watchdog)
    return s;
}

// Walk to a point `range` * 0.75 from the target on our side of it (0x18), at most every 700 ms.
Step AttackAction::approach(BotCtx& c, const Entity& t, int range, const char* why)
{
    status = fmt("%s: %s", c.w.entity_label(target).c_str(), why);
    if (!ft_older(c.now, approachAt_, kApproachResendMs)) return Step::Wait;
    approachAt_ = c.now;
    const double d = botu::dist(t.x, t.y, c.px, c.py);
    const double keep = range * 0.75;
    int x = t.x, y = t.y;
    if (d > 1.0) {
        x = t.x + int((c.px - t.x) * keep / d);
        y = t.y + int((c.py - t.y) * keep / d);
    }
    return c.act(act::move(c.w, x, y, c.bot.run, c.now));
}

void AttackAction::save(std::ostream& o) const
{
    botu::kv(o, key(), "radius", radius);
    botu::kv(o, key(), "skipOwned", skipOwned);
    botu::kv(o, key(), "fightBack", fightBack);
    mobs.save(o, key());
    botu::kv(o, key(), "basic", basic);
    for (const RotationSkill& r : rotation) o << key() << ".rotation=" << r.skill << ',' << int(r.on) << ',' << r.minTargets << '\n';
    botu::kv(o, key(), "useGun", useGun);
}

void AttackAction::load(const std::string& k, const std::string& v)
{
    if (k == "radius") radius = int(botu::num(v));
    else if (k == "skipOwned") skipOwned = botu::flag(v);
    else if (k == "basic") basic = botu::flag(v);
    else if (k == "rotation" || k == "skill") {         // (skill = the old single attack skill)
        RotationSkill r;
        r.skill = uint16_t(botu::num(v));
        const size_t comma = v.find(',');
        r.on = comma == std::string::npos || botu::flag(v.substr(comma + 1));
        const size_t comma2 = comma == std::string::npos ? std::string::npos : v.find(',', comma + 1);
        if (comma2 != std::string::npos) r.minTargets = std::clamp(int(botu::num(v.substr(comma2 + 1))), 0, 20);
        if (r.skill && std::none_of(rotation.begin(), rotation.end(), [&](const RotationSkill& x) { return x.skill == r.skill; }))
            rotation.push_back(r);
    }
    else if (k == "useGun") useGun = botu::flag(v);
    else if (k == "fightBack") fightBack = botu::flag(v);
    else mobs.load(k, v);
}

} // namespace nj
