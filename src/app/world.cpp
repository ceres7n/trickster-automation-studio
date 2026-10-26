// World model: the S2C (and a few C2S) packets applied to the game state.
//
// Layouts come from ProjectPAULA's writer of each packet (verified against the client there;
// file noted per case). "inferred" marks a rule taken from the server side that the client
// applies on its own and still needs a live check.
//
//   S2C  0x02A7  [u64 char uid][..][+17 name 20][+37 u16 species][+39 hair][+40 f_type]  (69 B)
//        0x00E6  [u32][u16 self eid][u16 x][u16 y]                       enter batch header
//        0x00E7  [u16 n] n*{u8 kind, item data, rare extra if kind 2}    item list
//        0x00E9  278 B: 23*u64 equip | 33 B stat block | 61 B state block (zone, money, HP, MP, EXP)
//        0x0007/8/9  [u16 eid][u16 kind][u16 species][u8 hair][u8 job][u16 level][u32 flags]
//                    [u16 x][u16 y][u16 dx][u16 dy][cstr name] + class blob      appear
//        0x000A/B  [u16 oid][u16 x][u16 y][u16 flag][u8 0 x3][u8 kind][item data]  items on enter
//        0x000C / 0x0269  [u16 dropper][u16 oid][u16 x][u16 y][u16 ev][u8 0 x3][u8 kind][item data]
//                  drop animation: 0x000C item, 0x0269 (617) gelder (THE common in-play drop);
//                  owned 30 s by the dropper's loot owner (0x016D) — ProjectPAULA drop.cpp place_drops
//        0x000D  [u16 oid][u16 x][u16 y][u16 fx][u16 fy][u16 0][item body]  thrown item lands
//        0x000F  [u16 id][u8]  remove     0x0006 / 0x0015 [u16 eid]  leaves
//        0x0018  [u16 eid][u16 sx][u16 sy][u16 dx][u16 dy][u8 type]  move
//        0x0019  [u16 eid][u16 x][u16 y][u8]  stop        0x001B [u16 id][u16 x][u16 y][u8]  swing
//        0x001F  [u16 att][u16 1][u16 tgt][u16 ax][u16 ay][u16 tx][u16 ty][i16 dmg][u8 res]
//        0x01E5  the same with [u16 2]: a gun shot (one bullet off the loaded stack, client-side)
//        0x0200  [u16 seed] gun CRC seed   0x0136 [u32 why] attack failed
//        0x0206 / 0x0209  [u64 uid][u8 slot]  common slot (20 = bullets) loaded / released
//        0x002A  [u16 tgt][i32 dmg][i32 death][i32 res][i32 el][i32 fl][i32][i32]  (client: HP -= dmg)
//        0x011E  [u16 tgt][i32 hp][u16][u8]  target HP   0x002B [u16 eid][u8 death]  dies
//        0x016D  [u16 monster][u16 owner]   0x0040 [u16 eid][u8 state]  sit / rest
//        0x0022  [u16 picker][u16 oid][u8 kind]  picked up
//        0x0024 [u32 hp]  0x0025 [u32 mp]  0x0026 [u32 exp]  0x0406 [u32 gain]  0x0029 [u32 money]
//        0x0027  [33 B stat block][u32 hp][u32 mp][u32 exp][u8]  level up
//        0x0275  [33 B stat block]   0x011D [u8][u32 tm exp]   0x00F5 [u8][u16 tm lv][u32][u16 pts]
//        0x010F  [u16 n] n*{u16 skill, u8 level, u16}   0x020B / 0x020C [u16 skill][u8 lv][u16 tm pts]
//        0x00A0  bag record + [u8 flag]   0x0044 bag record   0x0049 / 0x004E [u32 money][u16 n] n*record
//        0x0052  [u64 uid][u8 slot] equipped   0x0055 [u64 uid][u8 slot] released
//        0x0097  [u64 uid][effect]  used (count - 1: inferred, the client decrements it)
//        0x017C  [u16 map][u16 x][u16 y]   0x0099 [u16 zone][u16][u16]   0x0017 [u16 zone][u16][u16][u32 hp]
//        0x03A5  [u16 self eid]   0x01D8  [u16 eid][u8 speed/4]  current speed (sent before a move)
//        0x00F0  [u16 skill][u8 lv]  our skill fired: the cool time starts (skill.cpp skill_execute)
//        0x00EF  [u16 reason]  skill refused (12 = cool time)   0x00F3 []  cancelled   0x00F2 [u16 id][u8 2][u16 skill][u8 lv]..
//        0x00E0  [u16 caster][u16 target][u16 esId][u16 ESAction][u8 2][u8 ESClass][u16 skill][u16 EState]+tail
//                an ES starts (es.cpp; a strike's opener has esId 0)   0x0118 [u16 target][u16 esId]  it ends
//        0x0352  [u16 ESAction]  that ES has 10 s or less left (sent to its target)
//        drilling (drill.cpp):  0x002D []  dig accepted (the client's state 5)   0x002E []  refused (no drill)
//        0x0173 (371)  [u16 me][u16 x][u16 y][u8 form 0][u8 mapDepth/5][u8 itemDepth/5][u8 nature][u16 seed|sensed][u8 CDM %]
//        0x002F (47)   the same to the area up to the nature, then [u8 has PSAbility 6]
//        0x0032 (50)   [u8 reason][u16 drill life (CDM 11..14: seconds)][u32 item]   0x0033 (51) [u16 who][u8 reason]
//        0x000E  [u16 digger][u16 oid][u16 x][u16 y][u16 fromX][u16 fromY][u16 0][item body]  a find pops up (ours 30 s)
//        0x0432 (1074) [u32 n][u16 digger] n*{[u16 oid][u16 fx][u16 fy][u16 x][u16 y][item body]}[u8]  the BigBang
//        0x00E0 with ESAction 1147 / 1170 and esId 0 (the detectors, to the caster): tail [u8 applied]
//                [u16 n]{u16 x, u16 y}[u16 seconds when n]  (skill.cpp detect_buried)
//   C2S  0x0018  [u16 sx][u16 sy][u16 dx][u16 dy][u8 type]  our own move
//        0x002C  [u16 x][u16 y][u8 type] dig there   0x0030 [u16 cnt][u16 ms] cancel   0x0031 [u16 cnt][u16 ms][u16 depth][u16 crc]
//        0x00E0  [u8 2][u16 skill][u8 lv][u16 ui][u16 size][payload]: an Item Detector's point
//
//   bag record = [u8 type][u8 kind][item data] + rare extra when (type 2|4) and kind 2;
//                type 1 = uid now has count, 2/4 = new item, 3 = removed
//   item data  = [u32 id][u64 uid][u16 count] + 4 rare bytes when kind 2|3
//   rare extra = [u8 n] n*{u32 type, u32 id|value<<16, u32 extra}
#include "app/world.h"
#include "app/game_data.h"
#include "app/session.h"
#include "app/text_util.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

namespace nj {

const char* entity_kind_name(uint16_t kind)
{
    switch (kind) {
    case EK_PLAYER:       return "player";
    case EK_NPC:          return "npc";
    case EK_MONSTER:      return "monster";
    case EK_PET:          return "pet";
    case EK_SHOP:         return "shop";
    case EK_SKILL_MASTER: return "skill master";
    }
    return "?";
}

struct WorldModel::Reader {
    const uint8_t* p;
    size_t n;
    size_t off = 0;
    bool ok = true;

    bool need(size_t k)
    {
        if (ok && off + k <= n) return true;
        ok = false;
        return false;
    }
    uint64_t le(int k)
    {
        if (!need(k)) return 0;
        uint64_t v = 0;
        for (int i = k - 1; i >= 0; --i) v = (v << 8) | p[off + i];
        off += k;
        return v;
    }
    uint8_t  u8()  { return (uint8_t)le(1); }
    uint16_t u16() { return (uint16_t)le(2); }
    uint32_t u32() { return (uint32_t)le(4); }
    uint64_t u64() { return le(8); }
    int16_t  i16() { return (int16_t)le(2); }
    int32_t  i32() { return (int32_t)le(4); }
    void skip(size_t k) { if (need(k)) off += k; }
    std::string cstr()
    {
        if (!ok) return {};
        const void* z = memchr(p + off, 0, n - off);
        if (!z) { ok = false; return {}; }
        size_t len = (const uint8_t*)z - (p + off);
        std::string s = cp949_to_utf8((const char*)p + off, len);
        off += len + 1;
        return s;
    }
    size_t left() const { return n - off; }
};

namespace {

struct ItemData {
    uint8_t  kind = 0;
    uint32_t id = 0;
    uint64_t uid = 0;
    uint16_t count = 0;
};

constexpr size_t kMaxEvents = 600;

void read_item_data(WorldModel::Reader& r, ItemData& d)
{
    d.id = r.u32();
    d.uid = r.u64();
    d.count = r.u16();
    if (d.kind == 2 || d.kind == 3) r.skip(4);
}

void skip_rare_extra(WorldModel::Reader& r)
{
    uint8_t n = r.u8();
    r.skip(size_t(n) * 12);
}

// [u8 n] n*{u32 type, u32 id | value << 16, u32 extra}: the options by id (inventory.cpp write_rare_extra)
std::vector<std::pair<uint16_t, int16_t>> read_rare_extra(WorldModel::Reader& r)
{
    std::vector<std::pair<uint16_t, int16_t>> out;
    const uint8_t n = r.u8();
    for (uint8_t i = 0; i < n && r.ok; ++i) {
        r.u32();
        const uint32_t idv = r.u32();
        r.u32();
        out.push_back({ uint16_t(idv & 0xFFFF), int16_t(idv >> 16) });
    }
    return out;
}
} // namespace

// ── helpers ─────────────────────────────────────────────────────────────────
void WorldModel::event(uint64_t seq, const std::string& text)
{
    events.push_back(fmt("#%llu  ", (unsigned long long)seq) + text);
    while (events.size() > kMaxEvents) events.pop_front();
}

void WorldModel::clear_area()
{
    entities.clear();
    groundItems.clear();
    drill = DrillSession{};                     // a new link: the server's drill state is gone
    othersDig_.clear();
}

void WorldModel::arrived(uint64_t now)
{
    changingZone = false;
    arrivedAt = now;
    npc.talkNpc = 0;                            // a new link: no dialog / shop state survives it
    npc.shop = 0;
}

Entity* WorldModel::ent(uint16_t eid)
{
    auto it = entities.find(eid);
    return it == entities.end() ? nullptr : &it->second;
}

std::string WorldModel::entity_label(uint16_t eid) const
{
    if (eid && eid == self.eid) return "you";
    auto it = entities.find(eid);
    if (it == entities.end()) return fmt("#%u", eid);
    return it->second.name.empty() ? fmt("%s #%u", entity_kind_name(it->second.kind), eid)
                                   : fmt("%s #%u", it->second.name.c_str(), eid);
}

const InvItem* WorldModel::find_item(uint64_t uid) const
{
    for (const InvItem& i : inventory)
        if (i.uid == uid) return &i;
    return nullptr;
}

int WorldModel::skill_level(uint16_t skill) const
{
    for (const SkillEntry& e : skills)
        if (e.id == skill) return e.level;
    return 0;
}

uint64_t WorldModel::skill_ready_at(uint16_t skill) const
{
    auto it = skillAnswers.firedAt.find(skill);
    if (it == skillAnswers.firedAt.end()) return 0;
    const SkillLevelInfo* l = gd_ ? gd_->skill_level(skill, skill_level(skill)) : nullptr;
    const double secs = l ? std::max(0.0, double(l->coolTime) - 0.5) : 0.0;
    return it->second + uint64_t(secs * 1e7) + 2000000ull;     // + 200 ms
}

bool WorldModel::effect_up(const Effect& e, uint64_t now)
{
    return !e.endAt || now < e.endAt + 15000000ull;
}

const Effect* WorldModel::effect_of(uint16_t skill, uint64_t now) const
{
    const Effect* best = nullptr;
    for (const Effect& e : effects) {
        if (e.skill != skill || !effect_up(e, now)) continue;
        if (!best || (best->endAt && (!e.endAt || e.endAt > best->endAt))) best = &e;
    }
    return best;
}

bool WorldModel::third_eye(uint64_t now) const
{
    for (const Effect& e : effects)
        if (e.esAction == 1390 && effect_up(e, now)) return true;
    return false;
}

void WorldModel::effects_lose_ids()
{
    for (Effect& e : effects) e.esId = 0;
}

int WorldModel::stat_base(int stat) const
{
    if (!gd_ || !gd_->has_stat_table() || !self.statsKnown || stat < 0 || stat >= ST_COUNT) return 0;
    int v = gd_->stat_value(self.statPoints[stat] / 4 + 1, stat);
    for (int sl = 0; sl < 23; ++sl) {
        const InvItem* it = self.equip[sl] ? find_item(self.equip[sl]) : nullptr;
        if (!it) continue;
        const ItemInfo* ii = gd_->item(it->itemId);
        int add = ii ? ii->plus[stat] : 0;
        int sum = 0;
        bool have = false;
        for (const auto& o : it->opts)
            if (o.first == 10000 + stat) { sum += o.second; have = true; }
        if (have) add = int16_t(sum);
        v += add;
    }
    return v;
}

bool WorldModel::drill_open(uint64_t now) const
{
    auto within = [&](uint64_t t, uint64_t ms) { return t && (now < t || now - t < ms * 10000ull); };
    switch (drill.phase) {
    case DrillSession::Asked:   return within(drill.askedAt, 4000);
    case DrillSession::Started: return within(drill.startedAt, 25000);
    case DrillSession::Info:    return within(drill.infoAt, 200000);
    case DrillSession::Ending:  return within(drill.endAskedAt, 5000);
    default:                    return false;
    }
}

const std::map<uint32_t, BuriedCell>* WorldModel::buried_here() const
{
    auto it = buried.find(self.zone);
    return it == buried.end() ? nullptr : &it->second;
}

const BuriedCell* WorldModel::buried_at(int cx, int cy) const
{
    const auto* z = buried_here();
    if (!z) return nullptr;
    auto it = z->find(cell_key(cx, cy));
    return it == z->end() ? nullptr : &it->second;
}

BuriedCell& WorldModel::cell_of(int x, int y)
{
    return buried[self.zone][cell_key(x / 16, y / 16)];
}

// S2C 0x32 (drill.cpp drill_stop / the Crazy Drill notices). The [u16] is the drill's life after the dig (the
// reasons 11..14 carry seconds there instead); a dug-out cell is empty now (the server lays the new item at a
// random cell), "nothing" too; 7 = a ground that cannot be dug.
void WorldModel::drill_stop(uint8_t reason, uint16_t life, uint32_t item, uint64_t now, uint64_t seq)
{
    if (reason >= 11 && reason <= 18 && reason != 15) {
        if (reason <= 14) {                     // 11 started [secs], 12 / 13 / 14 [secs] left
            cdmUntil = now + uint64_t(life) * 10000000ull;
            if (reason == 11) event(seq, fmt("Crazy Drill Mode on for %u s", life));
        } else {                                // 16 map move, 17 another drill, 18 the auto-drill pet: over
            cdmUntil = 0;
            event(seq, "Crazy Drill Mode over");
        }
        return;
    }
    const bool open = drill.phase != DrillSession::Idle;
    const int x = drill.x, y = drill.y;
    drillLast = DrillResult{ reason, life, item, now, x, y, drill.infoAt != 0, drill.itemDepth };
    if (reason != 15)
        for (InvItem& it : inventory)
            if (self.equip[7] && it.uid == self.equip[7]) {
                if (life < it.count) drillStats.lifeUsed += it.count - life;
                it.count = life;
            }
    if (open) {
        BuriedCell& c = cell_of(x, y);
        switch (reason) {
        case 3: case 8: case 19: c.state = BuriedCell::Empty; ++drillStats.finds; break;
        case 2: case 9:          c.state = BuriedCell::Empty; ++drillStats.nothing; break;
        case 4:                  ++drillStats.tooDeep; break;
        case 7:                  c.state = BuriedCell::Undiggable; break;
        case 10:                 ++drillStats.interrupted; break;
        case 1:                  ++drillStats.cancelled; break;
        default: break;
        }
        c.at = now;
        if (reason != 1 && reason != 15) ++drillStats.digs;
    }
    if ((reason == 3 || reason == 8 || reason == 19) && item) {
        drillStats.found.emplace_back(now, item);
        while (drillStats.found.size() > 200) drillStats.found.pop_front();
        ++drillStats.foundCount[item];
    }
    if (reason == 6) event(seq, "the drill is worn out");
    drill = DrillSession{};
}

// The detectors' answer (skill.cpp detect_buried): the cells of the square around the scan point whose item lies no
// deeper than the reach — at most 50, rows of increasing x then the next y. Around us for Basic Detection (1170),
// around the point we asked for the Item Detector (1147). Every other open cell (byte 0 = 0, nature != 6) the scan
// looked at holds no item that shallow.
void WorldModel::detector_result(uint16_t skill, uint16_t esAction, Reader& r, uint64_t now, uint64_t seq)
{
    const uint8_t applied = r.u8();
    if (!r.ok) return;
    detectAt = now;
    if (!applied) {
        detectFound = -1;
        event(seq, "detection failed (the roll)");
        return;
    }
    const uint16_t n = r.u16();
    std::vector<std::pair<int, int>> pts;
    for (uint16_t i = 0; i < n && r.ok; ++i) {
        const int x = r.u16(), y = r.u16();
        pts.emplace_back(x, y);
    }
    if (!r.ok) return;
    detectFound = n;
    event(seq, fmt("detection: %u buried item%s", n, n == 1 ? "" : "s"));
    const int lv = skill_level(skill);
    const SkillLevelInfo* li = gd_ ? gd_->skill_level(skill, lv ? lv : 1) : nullptr;
    int reach = 0;
    if (li) {
        if (esAction == 1170) reach = int(li->depth);
        else if (li->applyRatio > 0.9f) reach = 300;
        else reach = int(double(stat_base(ST_DA)) * double(li->depthRatio) * 0.1);   // (the buffs' DA left out)
    }
    auto& zone = buried[self.zone];
    std::set<uint32_t> hit;
    for (const auto& p : pts) {
        const uint32_t key = cell_key(p.first / 16, p.second / 16);
        hit.insert(key);
        BuriedCell& c = zone[key];
        if (c.state != BuriedCell::Item || c.depth > reach) {
            c.state = BuriedCell::Detected;
            c.depth = int16_t(reach > 0 ? reach : 9999);
        }
        c.at = now;
    }
    // the rest of the square
    int ox = 0, oy = 0;
    if (esAction == 1170) self_pos_at(now, ox, oy);
    else if (detPointAt_ && now >= detPointAt_ && now - detPointAt_ < 100000000ull) { ox = detPointX_; oy = detPointY_; }
    else return;
    if (!li || li->radius <= 0.0f || reach <= 0) return;
    const MapGrid* g = gd_->map(self.zone);
    const int radius = int(li->radius);
    int16_t sx = int16_t(ox - radius), sy = int16_t(oy - radius);
    sx = int16_t(sx - int16_t(sx % 32) + 16);
    sy = int16_t(sy - int16_t(sy % 32) + 16);
    const int steps = int16_t(2 * radius / 16);
    size_t last = SIZE_MAX;                     // the 50-point cap: the scan stopped at the last point
    if (n >= 50) {
        last = 0;
        for (const auto& p : pts) {
            const int i = (p.second - sy) / 16, j = (p.first - sx) / 16;
            if (i >= 0 && j >= 0) last = std::max(last, size_t(i) * size_t(steps) + size_t(j));
        }
    }
    for (int i = 0; i < steps; ++i)
        for (int j = 0; j < steps; ++j) {
            if (size_t(i) * size_t(steps) + size_t(j) > last) return;
            const int cx = (sx + 16 * j) / 16, cy = (sy + 16 * i) / 16;
            const uint32_t key = cell_key(cx, cy);
            if (hit.count(key)) continue;
            if (g && !(g->raw(cx, cy) == 0 && g->nature_at(cx, cy) != 6)) continue;   // not looked at
            BuriedCell& c = zone[key];
            c.noneTo = int16_t(std::max<int>(c.noneTo, reach));
            if ((c.state == BuriedCell::Item || c.state == BuriedCell::Detected) && c.depth <= reach)
                c.state = BuriedCell::Empty;   // dug out since
            c.at = now;
        }
}

void WorldModel::self_pos_at(uint64_t now, int& x, int& y) const
{
    x = self.x;
    y = self.y;
    if (!self.moving) return;
    const double len = std::hypot(double(self.dx - self.x), double(self.dy - self.y));
    const double secs = now > self.moveAt ? double(now - self.moveAt) / 1e7 : 0.0;   // FILETIME = 100 ns
    const double run = secs * (self.speed > 0 ? self.speed : 160);
    if (len <= 0.0 || run >= len) { x = self.dx; y = self.dy; return; }
    x = self.x + int((self.dx - self.x) * run / len);
    y = self.y + int((self.dy - self.y) * run / len);
}

void WorldModel::feed_avatar(const EvAvatar& a)
{
    SelfState::Drawn& d = self.drawn;
    const bool moved = a.actor != d.actor || a.x != d.x || a.y != d.y;
    d.actor = a.actor;
    d.x = a.x; d.y = a.y; d.dx = a.dx; d.dy = a.dy;
    d.moving = (a.flags & 7) != 0;
    d.at = a.fileTime;
    if (moved) d.movedAt = a.fileTime;
    ++d.reads;
}

bool WorldModel::drawn_live(uint64_t now) const
{
    return self.drawn.actor && self.drawn.at && !(now > self.drawn.at && now - self.drawn.at > 2500ull * 10000ull);
}

void WorldModel::set_self_hp(int64_t hp)
{
    self.hp = hp < 0 ? 0 : hp;
    if (self.hp > self.hpMaxSeen) self.hpMaxSeen = self.hp;
    self.hpMax = std::max(self.hpMaxCalc, self.hpMaxSeen);
    if (self.hp > 0) self.dead = false;
}

void WorldModel::set_self_mp(int64_t mp)
{
    self.mp = mp < 0 ? 0 : mp;
    if (self.mp > self.mpMaxSeen) self.mpMaxSeen = self.mp;
    self.mpMax = std::max(self.mpMaxCalc, self.mpMaxSeen);
}

// ProjectPAULA recompute_stats (sub_716680) without the buffs, for MaxHP / MaxMP: the base
// gulim13[points / 4 + 1] (base_stat) + each worn item's bonus: the sum of its options 10000 + stat when it
// has any, else its ItemParam Plus (equip_bonus, sub_718750). Checked against the server: Leao, 236 MaxHP
// points -> graph-level 60 -> 1890; 59 MaxMP points -> 15 -> 500.
void WorldModel::recompute_max()
{
    self.hpMaxCalc = self.mpMaxCalc = 0;
    if (gd_ && gd_->has_stat_table() && self.statsKnown) {
        int bonus[ST_COUNT] = {};
        for (int sl = 0; sl < 23; ++sl) {
            const InvItem* it = self.equip[sl] ? find_item(self.equip[sl]) : nullptr;
            if (!it) continue;
            const ItemInfo* ii = gd_->item(it->itemId);
            for (int s : { int(ST_MAXHP), int(ST_MAXMP) }) {
                int v = ii ? ii->plus[s] : 0;
                int sum = 0;
                bool have = false;
                for (const auto& o : it->opts)
                    if (o.first == 10000 + s) { sum += o.second; have = true; }
                if (have) v = int16_t(sum);
                bonus[s] += v;
            }
        }
        self.hpMaxCalc = gd_->stat_value(self.statPoints[ST_MAXHP] / 4 + 1, ST_MAXHP) + bonus[ST_MAXHP];
        self.mpMaxCalc = gd_->stat_value(self.statPoints[ST_MAXMP] / 4 + 1, ST_MAXMP) + bonus[ST_MAXMP];
        if (self.hpMaxCalc < 1) self.hpMaxCalc = 0;
        if (self.mpMaxCalc < 1) self.mpMaxCalc = 0;
    }
    self.hpMax = std::max(self.hpMaxCalc, self.hpMaxSeen);
    self.mpMax = std::max(self.mpMaxCalc, self.mpMaxSeen);
}

// 33 bytes: u16 points[13] (12 accumulated + the unspent pool), u16 TM points, u16 TM level, u16 level, u8.
void WorldModel::apply_stat_block(const uint8_t* b)
{
    for (int i = 0; i < 13; ++i) self.statPoints[i] = uint16_t(b[i * 2] | b[i * 2 + 1] << 8);
    self.tmLevel = b[28] | b[29] << 8;
    self.level   = b[30] | b[31] << 8;
    self.statsKnown = true;
    recompute_max();
}

void WorldModel::remove_id(uint16_t id, uint64_t seq, const char* why)
{
    auto e = entities.find(id);
    if (e != entities.end()) {
        if (e->second.kind == EK_PLAYER) event(seq, entity_label(id) + " " + why);
        entities.erase(e);
    }
    groundItems.erase(id);
}

int WorldModel::weight_pct() const
{
    const int maxWt = stat_base(ST_MAXWT);
    return maxWt > 0 ? int(bag_weight() * 100 / maxWt) : -1;
}

int WorldModel::count_item(uint32_t itemId) const
{
    int n = 0;
    for (const InvItem& i : inventory)            // a rare item is one (a drill's count is its life: GetCount = 1)
        if (i.itemId == itemId) n += i.kind == 1 ? i.count : 1;
    return n;
}

int64_t WorldModel::bag_weight() const
{
    int64_t sum = 0;
    for (const InvItem& i : inventory) {
        const ItemInfo* ii = gd_ ? gd_->item(i.itemId) : nullptr;
        if (ii) sum += int64_t(ii->weight) * (i.kind == 1 ? i.count : 1);
    }
    return sum;
}

// A bag record (inventory.cpp write_bag_record). The warehouse renames its types (warehouse.cpp):
// 3 -> 7 (the whole item left) and 2 -> 8 (a new item; no rare extra then). toBag false = only read
// (the warehouse side of S2C 140); returns the record's type.
uint8_t WorldModel::apply_bag_record(Reader& r, uint64_t seq, bool toBag)
{
    const uint8_t type = r.u8();
    ItemData d;
    d.kind = r.u8();
    read_item_data(r, d);
    std::vector<std::pair<uint16_t, int16_t>> opts;
    if ((type == 2 || type == 4) && d.kind == 2) opts = read_rare_extra(r);
    if (!r.ok || !toBag) return type;

    auto it = std::find_if(inventory.begin(), inventory.end(), [&](const InvItem& i) { return i.uid == d.uid; });
    const std::string name = gd_ ? gd_->item_name(d.id) : fmt("item %u", d.id);
    switch (type) {
    case 1:
        if (it == inventory.end()) break;
        if (d.count == 0) inventory.erase(it);
        else it->count = d.count;
        event(seq, fmt("%s -> x%u", name.c_str(), d.count));
        break;
    case 2:
    case 4:
    case 8:
        if (it != inventory.end()) {
            it->count = d.count;
            if (!opts.empty()) it->opts = std::move(opts);
        } else {
            InvItem n;
            n.uid = d.uid;
            n.itemId = d.id;
            n.count = d.count;
            n.kind = d.kind;
            n.opts = std::move(opts);
            inventory.push_back(n);
        }
        event(seq, fmt("got %s x%u", name.c_str(), d.count));
        break;
    case 3:
    case 7:
        if (it != inventory.end()) inventory.erase(it);
        event(seq, fmt("lost %s", name.c_str()));
        break;
    default:
        event(seq, fmt("bag record type %u for %s (not modelled)", type, name.c_str()));
        break;
    }
    return type;
}

// ── the packets ─────────────────────────────────────────────────────────────
void WorldModel::feed(const CapPacket& p)
{
    const EvPacket& h = p.hdr;
    if (h.flags & PF_TRUNCATED) return;
    Reader r{ p.data.data(), p.data.size() };
    const uint64_t seq = h.seq;
    const uint64_t now = h.fileTime;

    if (h.dir == DIR_C2S) {
        if (h.opcode == 0x0018) {               // our move request
            int sx = r.u16(), sy = r.u16(), dx = r.u16(), dy = r.u16();
            if (r.ok) {
                self.x = sx; self.y = sy; self.dx = dx; self.dy = dy;
                self.moving = sx != dx || sy != dy;
                self.moveAt = now;
                ++applied;
            }
        } else if (h.opcode == 0x002C) {        // dig there (the server swallows it while a dig is open)
            const int x = r.u16(), y = r.u16();
            if (r.ok && !drill_open(now)) {
                drill = DrillSession{};
                drill.phase = DrillSession::Asked;
                drill.x = x;
                drill.y = y;
                drill.askedAt = now;
                ++applied;
            }
        } else if (h.opcode == 0x0030 || h.opcode == 0x0031) {     // cancelled / dug (ours or the client's own)
            if (drill.phase != DrillSession::Idle && !drill.endOp) {
                drill.endOp = h.opcode;
                drill.endAskedAt = now;
                if (drill.phase == DrillSession::Info) drill.phase = DrillSession::Ending;
                ++applied;
            }
        } else if (h.opcode == 0x00E0) {        // a skill at a point: remember an Item Detector's
            r.u8();
            const uint16_t sk = r.u16();
            r.u8();
            const uint16_t ui = r.u16(), size = r.u16();
            const int x = r.u16(), y = r.u16();
            const SkillInfo* si = gd_ ? gd_->skill(sk) : nullptr;
            if (r.ok && si && si->esAction == 1147 && size >= 4 && ui) {
                detPointX_ = x;
                detPointY_ = y;
                detPointAt_ = now;
            }
        }
        return;
    }

    bool known = true;
    switch (h.opcode) {
    case 0x0005:                                // map enter ack
        r.u8();
        if (r.ok) inWorld = true;
        break;

    case 0x02A7: {                              // char data blob (69 bytes)
        if (!r.need(69)) break;
        const uint8_t* b = p.data.data();
        memcpy(&self.charUid, b, 8);
        size_t nl = strnlen((const char*)b + 17, 20);
        self.name = cp949_to_utf8((const char*)b + 17, nl);
        self.species = uint16_t(b[37] | b[38] << 8);
        self.hair = b[39];
        self.fType = b[40];
        effects.clear();                        // a login: no ES is carried over, the cool times start over
        skillAnswers = SkillAnswers{};
        break;
    }

    case 0x00E6: {                              // enter batch header: a new world entry
        r.u32();
        uint16_t eid = r.u16();
        int x = r.u16(), y = r.u16();
        if (!r.ok) break;
        clear_area();
        effects_lose_ids();
        self.eid = eid;
        self.x = self.dx = x;
        self.y = self.dy = y;
        self.moving = false;
        inWorld = true;
        arrived(now);
        event(seq, fmt("entered the world as #%u at (%d,%d)", eid, x, y));
        break;
    }

    case 0x03A5: {                              // our entity id
        uint16_t eid = r.u16();
        if (r.ok) self.eid = eid;
        break;
    }

    case 0x00E9: {                              // login block (278 bytes)
        if (!r.need(278)) break;
        const uint8_t* b = p.data.data();
        memcpy(self.equip, b, sizeof(self.equip));
        self.weaponAt = now;
        apply_stat_block(b + 184);
        const uint8_t* s = b + 217;
        auto u16at = [&](int o) { return int(s[o] | s[o + 1] << 8); };
        auto u32at = [&](int o) { uint32_t v; memcpy(&v, s + o, 4); return (int64_t)v; };
        self.zone = u16at(2);
        self.homeZone = u16at(12);
        if (u16at(57) >= 80 && u16at(57) <= 800) self.whSlots = u16at(57);
        self.money = u32at(14);
        self.hpMaxSeen = self.mpMaxSeen = 0;
        set_self_hp(u32at(18));
        set_self_mp(u32at(22));
        self.tmExp = u32at(26);
        self.exp = u32at(30);
        for (InvItem& i : inventory) i.equipSlot = -1;
        for (int sl = 0; sl < 23; ++sl)
            for (InvItem& i : inventory)
                if (self.equip[sl] && i.uid == self.equip[sl]) i.equipSlot = sl;
        recompute_max();
        event(seq, fmt("character: Lv %d, TM %d, zone %d, HP %lld, MP %lld", self.level, self.tmLevel, self.zone,
                       (long long)self.hp, (long long)self.mp));
        break;
    }

    case 0x00E7: {                              // the whole item list
        uint16_t n = r.u16();
        std::vector<InvItem> list;
        for (uint16_t i = 0; i < n && r.ok; ++i) {
            ItemData d;
            d.kind = r.u8();
            read_item_data(r, d);
            InvItem it;
            if (d.kind == 2) it.opts = read_rare_extra(r);
            it.uid = d.uid;
            it.itemId = d.id;
            it.count = d.count;
            it.kind = d.kind;
            for (int sl = 0; sl < 23; ++sl)
                if (self.equip[sl] && self.equip[sl] == d.uid) it.equipSlot = sl;
            list.push_back(it);
        }
        if (!r.ok) break;
        inventory = std::move(list);
        recompute_max();
        event(seq, fmt("inventory: %u items", n));
        break;
    }

    case 0x0007:
    case 0x0008:
    case 0x0009: {                              // appear
        Entity e;
        e.eid = r.u16();
        e.kind = r.u16();
        e.species = r.u16();
        r.u8();                                 // hair (game_session.cpp: species, hair, job)
        e.job = r.u8();
        e.level = r.u16();
        e.stateFlags = r.u32();
        e.x = r.u16(); e.y = r.u16();
        e.dx = r.u16(); e.dy = r.u16();
        e.name = r.cstr();
        if (!r.ok) break;
        e.moving = e.x != e.dx || e.y != e.dy;
        e.moveAt = e.seenAt = now;
        if (e.eid == self.eid && e.kind == EK_PLAYER) {      // ourselves (zone enter / revive)
            if (e.job) self.job = e.job;
            self.x = e.x; self.y = e.y; self.dx = e.dx; self.dy = e.dy;
            self.moving = e.moving;
            self.moveAt = now;
            self.level = e.level ? e.level : self.level;
            if (self.dead) event(seq, "you are back");
            self.dead = false;
            break;
        }
        if (e.kind == EK_MONSTER && gd_) {
            if (const MobInfo* m = gd_->mob(e.species)) {
                e.maxHp = m->maxHp;
                if (e.name.empty()) e.name = m->name;
            }
        }
        Entity* old = ent(e.eid);
        if (old && old->kind == e.kind) {        // keep what the packets told us since
            e.hp = old->hp;
            e.owner = old->owner;
            e.hitBySelf = old->hitBySelf;
        }
        if (e.kind == EK_PLAYER && !old) event(seq, fmt("%s (Lv %u) appeared", e.name.c_str(), e.level));
        entities[e.eid] = std::move(e);
        break;
    }

    case 0x000C:                                // drop animation: item  (S2C 12)
    case 0x0269:                                // drop animation: gelder (S2C 617)
    case 0x000A:                                // items already lying in the zone (op 10, on enter)
    case 0x000B:
    case 0x000D: {                              // a thrown item landing
        // 0x000C / 0x0269 lead with the dropper eid (the monster); 0x000A/0x000B/0x000D do not.
        // Then [u16 oid][u16 x][u16 y], then one u16 event flag (0x000D instead has fx,fy,0),
        // then [3 zero bytes][u8 kind][item data]. (ProjectPAULA drop.cpp place_drops / write_item_body)
        GroundItem g;
        const bool drop = h.opcode == 0x000C || h.opcode == 0x0269;
        if (drop) g.dropper = r.u16();          // dropper eid
        g.oid = r.u16();
        g.x = r.u16(); g.y = r.u16();
        if (h.opcode == 0x000D) { r.u16(); r.u16(); r.u16(); }
        else g.eventFlag = r.u16();
        r.u8(); r.u8(); r.u8();
        ItemData d;
        d.kind = r.u8();
        read_item_data(r, d);
        if (!r.ok) break;
        g.kind = d.kind;
        g.itemId = d.id;
        g.uid = d.uid;
        g.count = d.count;
        g.seenAt = now;
        g.fromDrop = drop;
        if (drop)
            if (const Entity* m = ent(g.dropper)) g.owner = m->owner;
        groundItems[g.oid] = g;
        if (drop) event(seq, fmt("dropped %s x%u", gd_ ? gd_->item_name(d.id).c_str() : "?", d.count));
        break;
    }

    case 0x000F: {
        uint16_t id = r.u16();
        r.u8();
        if (r.ok) remove_id(id, seq, "left");
        break;
    }
    case 0x0006:
    case 0x0015: {
        uint16_t id = r.u16();
        if (r.ok && id != self.eid) remove_id(id, seq, h.opcode == 0x0006 ? "logged out" : "changed field");
        break;
    }

    case 0x0018: {                              // move
        uint16_t eid = r.u16();
        int sx = r.u16(), sy = r.u16(), dx = r.u16(), dy = r.u16();
        r.u8();
        if (!r.ok) break;
        if (eid == self.eid) {
            self.x = sx; self.y = sy; self.dx = dx; self.dy = dy;
            self.moving = sx != dx || sy != dy;
            self.moveAt = now;
        } else if (Entity* e = ent(eid)) {
            e->x = sx; e->y = sy; e->dx = dx; e->dy = dy;
            e->moving = sx != dx || sy != dy;
            e->moveAt = now;
        }
        break;
    }
    case 0x01D8: {                              // current speed: [u16 eid][u8 speed/4]
        uint16_t eid = r.u16();
        uint8_t q = r.u8();
        if (!r.ok) break;
        if (eid == self.eid && q) {
            int x, y;                               // the running segment goes on at the new speed
            self_pos_at(now, x, y);
            if (self.moving) { self.x = x; self.y = y; self.moveAt = now; }
            self.speed = 4 * q;
        }
        break;
    }
    case 0x0019: {                              // stop at
        uint16_t eid = r.u16();
        int x = r.u16(), y = r.u16();
        r.u8();
        if (!r.ok) break;
        if (eid == self.eid) {
            self.x = self.dx = x; self.y = self.dy = y; self.moving = false;
        } else if (Entity* e = ent(eid)) {
            e->x = e->dx = x; e->y = e->dy = y; e->moving = false;
            e->moveAt = now;
        }
        break;
    }
    case 0x001B: {                              // a monster's swing announcement
        uint16_t eid = r.u16();
        int x = r.u16(), y = r.u16();
        r.u8();
        if (!r.ok) break;
        if (Entity* e = ent(eid)) { e->x = e->dx = x; e->y = e->dy = y; e->moving = false; }
        break;
    }

    case 0x01E5:                                // a gun shot (same layout; the shooter's u16 is 2)
    case 0x001F: {                              // a melee swing
        uint16_t att = r.u16();
        r.u16();
        uint16_t tgt = r.u16();
        int ax = r.u16(), ay = r.u16(), tx = r.u16(), ty = r.u16();
        r.i16();
        r.u8();
        if (!r.ok) break;
        auto place = [&](uint16_t id, int x, int y) {
            if (id == self.eid) { self.x = self.dx = x; self.y = self.dy = y; self.moving = false; }
            else if (Entity* e = ent(id)) { e->x = e->dx = x; e->y = e->dy = y; e->moving = false; }
        };
        place(att, ax, ay);
        place(tgt, tx, ty);
        if (Entity* a = ent(att)) { a->lastTarget = tgt; a->lastSwingAt = now; }
        if (att == self.eid)
            if (Entity* t = ent(tgt)) { t->hitBySelf = true; t->selfHitAt = now; }
        if (h.opcode == 0x01E5 && att == self.eid) {
            // the client takes the shot's bullet off its own loaded stack (sub_6AEA00; inferred)
            self.lastShotAt = now;
            const uint64_t mag = self.equip[20];
            auto it = std::find_if(inventory.begin(), inventory.end(), [&](const InvItem& i) { return i.uid == mag; });
            if (mag && it != inventory.end()) {
                if (it->count > 1) --it->count;
                else { inventory.erase(it); self.equip[20] = 0; event(seq, "out of bullets"); }
            }
        }
        break;
    }

    case 0x002A: {                              // damage
        uint16_t tgt = r.u16();
        int32_t dmg = r.i32();
        r.skip(24);
        if (!r.ok) break;
        if (tgt == self.eid) {
            if (self.hp >= 0) set_self_hp(self.hp - dmg);
        } else if (Entity* e = ent(tgt)) {
            int64_t base = e->hp >= 0 ? e->hp : e->maxHp;
            if (base > 0) e->hp = std::max<int64_t>(0, base - dmg);
        }
        break;
    }
    case 0x011E: {                              // the target's HP after a hit
        uint16_t tgt = r.u16();
        int32_t hp = r.i32();
        if (!r.ok) break;
        if (tgt == self.eid) set_self_hp(hp);
        else if (Entity* e = ent(tgt)) e->hp = hp < 0 ? 0 : hp;
        break;
    }
    case 0x002B: {                              // dies
        uint16_t eid = r.u16();
        r.u8();
        if (!r.ok) break;
        if (eid == self.eid) {
            self.dead = true;
            self.hp = 0;
            ++self.deaths;
            self.deadAt = now;
            self.deadZone = self.zone;
            drill = DrillSession{};             // (the server leaves the drill state with no 0x32)
            cdmUntil = 0;                       // and the Crazy Drill Mode ends (sub_7B0230)
            event(seq, "you died");
        } else if (Entity* e = ent(eid)) {
            e->dead = true;
            e->hp = 0;
            e->moving = false;
            if (e->kind == EK_MONSTER && (e->owner == self.eid || e->hitBySelf)) {
                ++self.kills;
                event(seq, fmt("killed %s", entity_label(eid).c_str()));
            }
        }
        break;
    }
    case 0x016D: {                              // a monster's owner
        uint16_t mob = r.u16(), owner = r.u16();
        if (!r.ok) break;
        if (Entity* e = ent(mob)) e->owner = owner;
        break;
    }
    case 0x0040: {                              // sit / rest / stand
        uint16_t eid = r.u16();
        uint8_t st = r.u8();
        if (!r.ok) break;
        if (eid == self.eid) self.sit = st;
        else if (Entity* e = ent(eid)) e->sit = st;
        break;
    }

    case 0x0022: {                              // picked up
        uint16_t who = r.u16(), oid = r.u16();
        r.u8();
        if (!r.ok) break;
        auto g = groundItems.find(oid);
        if (who == self.eid && g != groundItems.end())
            event(seq, fmt("picked up %s x%u", gd_ ? gd_->item_name(g->second.itemId).c_str() : "?", g->second.count));
        groundItems.erase(oid);
        break;
    }

    case 0x0024: { uint32_t v = r.u32(); if (r.ok) set_self_hp(v); break; }
    case 0x0025: { uint32_t v = r.u32(); if (r.ok) set_self_mp(v); break; }
    case 0x0029: { uint32_t v = r.u32(); if (r.ok) self.money = v; break; }
    case 0x0026: { uint32_t v = r.u32(); if (r.ok) self.exp = v; break; }
    case 0x0406: {
        uint32_t v = r.u32();
        if (!r.ok) break;
        self.exp += v;
        self.expGained += v;
        break;
    }
    case 0x0027: {                              // level up
        if (!r.need(33 + 12)) break;
        apply_stat_block(p.data.data());
        r.skip(33);
        uint32_t hp = r.u32(), mp = r.u32(), exp = r.u32();
        set_self_hp(hp);
        set_self_mp(mp);
        self.exp = exp;
        event(seq, fmt("LEVEL UP -> %d", self.level));
        break;
    }
    case 0x0275:
        if (r.need(33)) apply_stat_block(p.data.data());
        break;
    case 0x006D: {                              // [u16 unspent][12 x u16 points]: the points changed
        const uint16_t unspent = r.u16();
        uint16_t pts[12];
        for (int i = 0; i < 12; ++i) pts[i] = r.u16();
        if (!r.ok) break;
        for (int i = 0; i < 12; ++i) self.statPoints[i] = pts[i];
        self.statPoints[12] = unspent;
        self.statsKnown = true;
        recompute_max();
        break;
    }
    case 0x011D: {
        r.u8();
        uint32_t v = r.u32();
        if (!r.ok) break;
        if (v > self.tmExp) self.tmGained += v - self.tmExp;
        self.tmExp = v;
        break;
    }
    case 0x00F5: {
        r.u8();
        uint16_t lv = r.u16();
        uint32_t ex = r.u32();
        uint16_t pts = r.u16();
        if (!r.ok) break;
        self.tmLevel = lv;
        self.tmExp = ex;
        self.tmPoints = pts;
        event(seq, fmt("TM LEVEL UP -> %u", lv));
        break;
    }

    case 0x010F: {                              // learned skills
        uint16_t n = r.u16();
        std::vector<SkillEntry> list;
        for (uint16_t i = 0; i < n && r.ok; ++i) {
            SkillEntry s;
            s.id = r.u16();
            s.level = r.u8();
            r.u16();
            list.push_back(s);
        }
        if (r.ok) skills = std::move(list);
        break;
    }
    case 0x020B:
    case 0x020C: {                              // learned / leveled a skill
        uint16_t id = r.u16();
        uint8_t lv = r.u8();
        uint16_t pts = r.u16();
        if (!r.ok) break;
        self.tmPoints = pts;
        auto it = std::find_if(skills.begin(), skills.end(), [&](const SkillEntry& s) { return s.id == id; });
        if (it == skills.end()) skills.push_back({ id, lv });
        else it->level = lv;
        event(seq, fmt("%s %s -> Lv %u", h.opcode == 0x020B ? "learned" : "raised",
                       gd_ ? gd_->skill_name(id).c_str() : "skill", lv));
        break;
    }

    case 0x00F0: {                              // our skill fired
        uint16_t sk = r.u16();
        r.u8();
        if (!r.ok) break;
        skillAnswers.firedAt[sk] = now;
        ++skillAnswers.fired;
        break;
    }
    case 0x00EF: {                              // skill refused
        uint16_t why = r.u16();
        if (!r.ok) break;
        skillAnswers.refusedAt = now;
        skillAnswers.refusedWhy = why;
        if (why != 15) event(seq, fmt("skill refused (%u%s)", why, why == 12 ? ": cool time" : ""));
        break;
    }
    case 0x00F3:                                // skill cancelled
        skillAnswers.cancelAt = now;
        break;
    case 0x00F2: {                              // a cast starts
        uint16_t id = r.u16();
        r.u8();
        uint16_t sk = r.u16();
        if (!r.ok) break;
        if (id == self.eid) { skillAnswers.castSkill = sk; skillAnswers.castAt = now; }
        break;
    }
    case 0x00E0: {                              // an ES starts
        uint16_t caster = r.u16(), target = r.u16(), esId = r.u16(), esAction = r.u16();
        r.u8();
        uint8_t esClass = r.u8();
        uint16_t sk = r.u16();
        r.u16();
        if (!r.ok) break;
        if (!esId && caster == self.eid && (esAction == 1147 || esAction == 1170))
            detector_result(sk, esAction, r, now, seq);     // (the detectors' tail: the points found)
        r.off = r.n;                            // (the other classes' tails are not modelled)
        if (!esId || !target || target != self.eid) break;
        // the limiter ends an ES of the same ESAction first (PlayerESLimiter admit): drop a leftover
        effects.erase(std::remove_if(effects.begin(), effects.end(), [&](const Effect& e) {
            return e.esId == esId || (e.esAction == esAction && e.skill == sk); }), effects.end());
        Effect e;
        e.esId = esId;
        e.skill = sk;
        e.esAction = esAction;
        e.caster = caster;
        e.esClass = esClass;
        e.startAt = now;
        // its lifetime: 1000 * Seconds of the row at the caster's level (ours when we know the skill)
        const int lv = skill_level(sk);
        if (const SkillLevelInfo* l = gd_ ? gd_->skill_level(sk, lv ? lv : 1) : nullptr)
            if (l->seconds > 0) e.endAt = now + uint64_t(double(l->seconds) * 1e7);
        effects.push_back(e);
        event(seq, fmt("effect on you: %s%s", gd_ ? gd_->skill_name(sk).c_str() : fmt("skill %u", sk).c_str(),
                       caster == self.eid ? "" : fmt(" (from %s)", entity_label(caster).c_str()).c_str()));
        break;
    }
    case 0x0118: {                              // an ES ends
        uint16_t target = r.u16(), esId = r.u16();
        if (!r.ok || target != self.eid) break;
        auto it = std::find_if(effects.begin(), effects.end(), [&](const Effect& e) { return e.esId == esId; });
        if (it == effects.end()) {
            // an id lost in a zone change: the one of those whose time is nearest
            uint64_t bestGap = UINT64_MAX;
            for (auto e = effects.begin(); e != effects.end(); ++e) {
                if (e->esId) continue;
                const uint64_t gap = e->endAt ? (e->endAt > now ? e->endAt - now : now - e->endAt) : UINT64_MAX - 1;
                if (gap < bestGap) { bestGap = gap; it = e; }
            }
        }
        if (it != effects.end()) {
            event(seq, fmt("effect over: %s", gd_ ? gd_->skill_name(it->skill).c_str() : "?"));
            effects.erase(it);
        }
        break;
    }
    case 0x0352: {                              // 10 s left
        uint16_t esAction = r.u16();
        if (!r.ok) break;
        for (Effect& e : effects) {
            if (e.esAction != esAction) continue;
            e.ending = true;
            if (!e.endAt || e.endAt > now + 100000000ull) e.endAt = now + 100000000ull;
        }
        break;
    }

    // ── drilling (drill.cpp) ──
    case 0x002D:                                // []: the dig is on (the client's state 5); a walk may come first
        if (drill.phase == DrillSession::Asked || !drill_open(now)) {
            if (drill.phase != DrillSession::Asked) { drill = DrillSession{}; drill.askedAt = now; }
            drill.phase = DrillSession::Started;
            drill.startedAt = now;
        }
        break;
    case 0x002E:                                // []: refused (no drill in slot 7, a condition)
        drill = DrillSession{};
        drillRefusedAt = now;
        event(seq, "drill refused (no drill equipped?)");
        break;
    case 0x0173: {                              // 371: the dig starts — what lies in the cell
        const uint16_t who = r.u16();
        const int x = r.u16(), y = r.u16();
        const uint8_t form = r.u8();
        int mapD = 0, itemD = 0;
        uint8_t nat = 0;
        if (form) r.i16();                      // (an auto-drill form: [i16] — not modelled)
        else { mapD = 5 * r.u8(); itemD = 5 * r.u8(); nat = r.u8(); }
        const uint16_t seed = r.u16();
        const uint8_t cdm = r.u8();
        if (!r.ok || who != self.eid) break;
        if (drill.phase == DrillSession::Idle || drill.phase == DrillSession::Ending) {
            drill = DrillSession{};             // (a dig we did not see asked: the client's own)
            drill.askedAt = now;
        }
        drill.phase = DrillSession::Info;
        drill.x = x;
        drill.y = y;
        drill.infoAt = now;
        drill.mapDepth = mapD;
        drill.itemDepth = itemD;
        drill.nature = nat;
        drill.seed = seed;
        drill.sensed = (seed & 1) != 0;
        drill.premium = form != 0;
        cdmGauge = cdm;
        if (!form) {
            BuriedCell& c = cell_of(x, y);
            c.state = itemD ? BuriedCell::Item : BuriedCell::Empty;
            c.depth = int16_t(itemD);
            c.at = now;
        }
        break;
    }
    case 0x002F: {                              // 47: someone's dig starts (the same facts, their cell)
        const uint16_t who = r.u16();
        const int x = r.u16(), y = r.u16();
        const uint8_t form = r.u8();
        if (form || who == self.eid) break;
        r.u8();
        const int itemD = 5 * r.u8();
        r.u8();
        if (!r.ok) break;
        BuriedCell& c = cell_of(x, y);
        c.state = itemD ? BuriedCell::Claimed : BuriedCell::Empty;
        c.depth = int16_t(itemD);
        c.at = now;
        othersDig_[who] = OtherDig{ cell_key(x / 16, y / 16), int16_t(itemD) };
        break;
    }
    case 0x0032: {                              // 50: our dig ended (or a Crazy Drill notice)
        const uint8_t reason = r.u8();
        const uint16_t life = r.u16();
        const uint32_t item = r.u32();
        if (r.ok) drill_stop(reason, life, item, now, seq);
        break;
    }
    case 0x0033: {                              // 51: someone's dig ended
        const uint16_t who = r.u16();
        const uint8_t reason = r.u8();
        if (!r.ok || who == self.eid) break;
        auto it = othersDig_.find(who);
        if (it == othersDig_.end()) break;
        auto& zone = buried[self.zone];
        BuriedCell& c = zone[it->second.key];
        if (reason == 3 || reason == 8 || reason == 19) c.state = BuriedCell::Empty;   // dug out
        else if (it->second.depth > 0) { c.state = BuriedCell::Item; c.depth = it->second.depth; }   // still there
        c.at = now;
        othersDig_.erase(it);
        break;
    }
    case 0x000E: {                              // a find pops up out of the digger's hole (owned by the digger)
        GroundItem g;
        g.dropper = r.u16();
        g.oid = r.u16();
        g.x = r.u16(); g.y = r.u16();
        r.u16(); r.u16(); r.u16();
        r.u8(); r.u8(); r.u8();
        ItemData d;
        d.kind = r.u8();
        read_item_data(r, d);
        if (!r.ok) break;
        g.kind = d.kind;
        g.itemId = d.id;
        g.uid = d.uid;
        g.count = d.count;
        g.seenAt = now;
        g.fromDrop = true;
        g.owner = g.dropper;
        groundItems[g.oid] = g;
        if (g.dropper == self.eid) event(seq, fmt("dug up %s x%u", gd_ ? gd_->item_name(d.id).c_str() : "?", d.count));
        break;
    }
    case 0x0432: {                              // 1074: the Crazy Drill's BigBang
        const uint32_t n = r.u32();
        const uint16_t digger = r.u16();
        for (uint32_t i = 0; i < n && i < 64 && r.ok; ++i) {
            GroundItem g;
            g.oid = r.u16();
            r.u16(); r.u16();
            g.x = r.u16(); g.y = r.u16();
            r.u8(); r.u8(); r.u8();
            ItemData d;
            d.kind = r.u8();
            read_item_data(r, d);
            if (!r.ok) break;
            g.kind = d.kind;
            g.itemId = d.id;
            g.uid = d.uid;
            g.count = d.count;
            g.seenAt = now;
            g.fromDrop = true;
            g.dropper = g.owner = digger;
            groundItems[g.oid] = g;
            if (digger == self.eid) {
                drillStats.found.emplace_back(now, g.itemId);
                while (drillStats.found.size() > 200) drillStats.found.pop_front();
                ++drillStats.foundCount[g.itemId];
            }
        }
        r.off = r.n;
        if (digger == self.eid) event(seq, fmt("BigBang: %u items", n));
        break;
    }

    case 0x00A0:
        apply_bag_record(r, seq);
        r.u8();                                 // flag (its extras are not modelled)
        break;
    case 0x0044:
        apply_bag_record(r, seq);
        break;
    case 0x0049:
    case 0x004E: {                              // bought / sold
        uint32_t money = r.u32();
        uint16_t n = r.u16();
        if (!r.ok) break;
        self.money = money;
        for (uint16_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        if (h.opcode == 0x0049) npc.boughtAt = now;
        else npc.soldAt = now;
        break;
    }
    case 0x0097: {                              // an item was used (inferred: one is used up)
        uint64_t uid = r.u64();
        if (!r.ok) break;
        auto it = std::find_if(inventory.begin(), inventory.end(), [&](const InvItem& i) { return i.uid == uid; });
        if (it != inventory.end() && it->count > 0 && --it->count == 0) inventory.erase(it);
        break;
    }
    case 0x0052:
    case 0x0055: {                              // equipped / released
        uint64_t uid = r.u64();
        uint8_t slot = r.u8();
        if (!r.ok || slot >= 23) break;
        const bool on = h.opcode == 0x0052;
        self.equip[slot] = on ? uid : 0;
        if (slot == 1) self.weaponAt = now;
        for (InvItem& i : inventory)
            if (i.uid == uid) i.equipSlot = on ? slot : -1;
        recompute_max();
        break;
    }
    case 0x0206:                                // common slot equipped (bullets in slot 20)
    case 0x0209: {                              // common slot released
        uint64_t uid = r.u64();
        uint8_t slot = r.u8();
        if (!r.ok || slot >= 23) break;
        const bool on = h.opcode == 0x0206;
        if (!on && self.equip[slot] != uid) break;
        self.equip[slot] = on ? uid : 0;
        for (InvItem& i : inventory)
            if (i.uid == uid) i.equipSlot = on ? slot : -1;
        if (on) event(seq, fmt("loaded %s", find_item(uid) && gd_ ? gd_->item_name(find_item(uid)->itemId).c_str()
                                                                    : "bullets"));
        break;
    }
    case 0x0200: {                              // a gun's CRC seed
        uint16_t seed = r.u16();
        if (!r.ok) break;
        self.gunSeed = seed;
        ++self.gunSeeds;
        break;
    }
    case 0x0136: {                              // our attack request failed
        uint32_t why = r.u32();
        if (!r.ok) break;
        self.attackCancelAt = now;
        event(seq, fmt("attack cancelled (%u)", why));
        break;
    }

    case 0x017C:                                // map change
    case 0x0099:                                // warp
    case 0x0017: {                              // go to town
        int zone = r.u16();
        int x = r.u16(), y = r.u16();
        uint32_t hp = h.opcode == 0x0017 ? r.u32() : 0;
        if (!r.ok) break;
        clear_area();
        effects_lose_ids();
        self.zone = zone;
        if (h.opcode == 0x017C) { self.x = self.dx = x; self.y = self.dy = y; }
        if (h.opcode == 0x0017) set_self_hp(hp);
        self.moving = false;
        if (h.opcode != 0x017C) {               // 153 / 0x17: the field change (154 -> 156) follows
            changingZone = true;
            zoneChangeAt = now;
            npc.talkNpc = 0;
            npc.shop = 0;
        }
        event(seq, fmt("zone -> %d", zone));
        break;
    }

    // ── the field change: S2C 154 relocate -> (the client reconnects, C2S 156) -> S2C 437 + 157 ──
    case 0x009A: {                              // [u32 token][u64 chr uid][cstr ip][u32 port][u32][u16 zone][u16 x][u16 y][u8 type]
        r.u32(); r.u64();
        r.cstr();
        r.u32(); r.u32();
        const int zone = r.u16(), x = r.u16(), y = r.u16();
        r.u8();
        if (!r.ok) break;
        if (zone != self.zone) { clear_area(); effects_lose_ids(); }
        self.zone = zone;
        self.x = self.dx = x;
        self.y = self.dy = y;
        self.moving = false;
        changingZone = true;
        zoneChangeAt = now;
        npc.talkNpc = 0;
        npc.shop = 0;
        event(seq, fmt("moving to %s (%d,%d)", gd_ ? gd_->map_name(zone).c_str() : "?", x, y));
        break;
    }
    case 0x01B5: {                              // [u16 self eid][u16 x][u16 y][u32][u8]: in the new map
        const uint16_t eid = r.u16();
        const int x = r.u16(), y = r.u16();
        if (!r.ok) break;
        self.eid = eid;
        self.x = self.dx = x;
        self.y = self.dy = y;
        self.moving = false;
        inWorld = true;
        arrived(now);
        event(seq, fmt("arrived in %s at (%d,%d)", gd_ ? gd_->map_name(self.zone).c_str() : "?", x, y));
        break;
    }
    case 0x009D:                                // [u32]: the client's transition is over
        r.u32();
        if (changingZone) arrived(now);
        break;

    // ── NPC dialogs (npc_talk.cpp) ──
    case 0x0070: {                              // [u16 npc][u16 type][u8 count][cstr msg if count][u16][u16]
        const uint16_t id = r.u16();
        r.u16();
        const uint8_t count = r.u8();
        std::string msg = count ? r.cstr() : std::string();
        if (!r.ok) break;
        npc.talkNpc = id;
        npc.talkAt = now;
        npc.text = std::move(msg);
        npc.first = true;
        npc.msgNo = -1;
        npc.msgQid = 0;
        r.skip(r.left());
        break;
    }
    case 0x0071:                                // []: refused (too far, no such NPC)
        npc.talkRefusedAt = now;
        npc.talkNpc = 0;
        break;
    case 0x0073: {                              // [u16 npc][u16 index][cstr][u16 stype][u16 quest]
        const uint16_t id = r.u16();
        const uint16_t index = r.u16();
        std::string msg = r.cstr();
        uint16_t qid = 0;
        if (r.ok && r.left() >= 4) { r.u16(); qid = r.u16(); }
        r.skip(r.left());
        if (!r.ok) break;
        npc.talkNpc = id;
        npc.talkAt = now;
        npc.text = std::move(msg);
        npc.first = false;
        npc.msgNo = index;
        npc.msgQid = qid;
        break;
    }
    case 0x0074:                                // [] or [u16 npc][u16 index]: a script / message refused
        npc.talkErrorAt = now;
        npc.errMsg = -1;
        if (r.left() >= 4) { r.u16(); npc.errMsg = r.u16(); }
        r.skip(r.left());
        break;

    // ── normal quests (quest.cpp) ──
    case 0x00EA: {                              // 234 the ones on (login) [n]{u32 qid, u16 run}
        const uint16_t n = r.u16();
        std::map<int, int> on;
        for (uint16_t i = 0; i < n && r.ok; ++i) {
            const int q = int(r.u32());
            on[q] = r.u16();
        }
        if (!r.ok) break;
        quests.active = std::move(on);
        quests.listed = true;
        for (const auto& kv : quests.active) quests.hist[kv.first] = QuestHist{ kv.second - 1, kv.second - 1 };
        break;
    }
    case 0x0084: {                              // 132 taken [u32 qid][u16 run][Name][Client][Pos][Comment][u16 qid]
        const int q = int(r.u32());
        const int run = r.u16();
        std::string name = r.cstr();
        r.cstr(); r.cstr(); r.cstr();
        r.u16();
        if (!r.ok) break;
        quests.active[q] = run;
        quests.hist[q] = QuestHist{ run - 1, run - 1 };
        quests.takenAt = now;
        quests.takenQid = q;
        quests.takenRun = run;
        quests.names[q] = name;
        event(seq, fmt("quest %d taken (run %d): %s", q, run, name.c_str()));
        break;
    }
    case 0x0085: {                              // 133 the entry gone [u32 qid]
        const int q = int(r.u32());
        if (!r.ok) break;
        quests.active.erase(q);
        quests.removedAt = now;
        quests.removedQid = q;
        break;
    }
    case 0x0096: {                              // 150 handed in [n]{record}[m]{u32 item, u16}[exp][tm][u16 qid][u16 run][gelder][skill][lv]
        const uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        const uint16_t m = r.u16();
        r.skip(size_t(m) * 6);
        const uint32_t exp = r.u32(), tm = r.u32();
        const int q = r.u16();
        const int run = r.u16();
        const uint32_t gelder = r.u32();
        r.u16(); r.u16();
        if (!r.ok) break;
        quests.active.erase(q);
        quests.hist[q] = QuestHist{ run, run };
        quests.doneAt = now;
        quests.doneQid = q;
        quests.doneRun = run;
        quests.lastExp = exp; quests.lastTm = tm; quests.lastGelder = gelder;
        ++quests.completed;
        event(seq, fmt("quest %d handed in (run %d): %u EXP, %u TM EXP, %u galder", q, run, exp, tm, gelder));
        break;
    }

    // ── scenario quests (quest.cpp squest_*) ──
    case 0x0341: {                              // 833 the ones on (login) [n]{u32 qid, u16 run}
        const uint16_t n = r.u16();
        std::map<int, int> on;
        for (uint16_t i = 0; i < n && r.ok; ++i) {
            const int q = int(r.u32());
            on[q] = r.u16();
        }
        if (!r.ok) break;
        quests.sactive = std::move(on);
        quests.slisted = true;
        for (const auto& kv : quests.sactive) quests.shist[kv.first] = QuestHist{ kv.second - 1, kv.second - 1 };
        break;
    }
    case 0x033D: {                              // 829 taken [u32 qid][u16 run][Name][Client][Pos][Comment]
        const int q = int(r.u32());
        const int run = r.u16();
        std::string name = r.cstr();
        r.cstr(); r.cstr(); r.cstr();
        if (!r.ok) break;
        quests.sactive[q] = run;
        quests.shist[q] = QuestHist{ run - 1, run - 1 };
        quests.stakenAt = now;
        quests.stakenQid = q;
        quests.stakenRun = run;
        event(seq, fmt("scenario quest %d taken (run %d): %s", q, run, name.c_str()));
        break;
    }
    case 0x033F: {                              // 831 handed in [n]{record}[m]{u32 item, u16}[exp][tm][u32 qid][u16 count][gelder]
        const uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        const uint16_t m = r.u16();
        r.skip(size_t(m) * 6);
        const uint32_t exp = r.u32(), tm = r.u32();
        const int q = int(r.u32());
        r.u16();
        const uint32_t gelder = r.u32();
        if (!r.ok) break;
        auto on = quests.sactive.find(q);
        const int run = on != quests.sactive.end() ? on->second : std::max(1, quests.shist[q].lo + 1);
        quests.sactive.erase(q);
        quests.shist[q] = QuestHist{ run, run };
        quests.sdoneAt = now;
        quests.sdoneQid = q;
        quests.lastExp = exp; quests.lastTm = tm; quests.lastGelder = gelder;
        ++quests.completed;
        event(seq, fmt("scenario quest %d handed in: %u EXP, %u TM EXP, %u galder", q, exp, tm, gelder));
        break;
    }
    case 0x033E: {                              // 830 the entry gone [u32 qid]
        const int q = int(r.u32());
        if (!r.ok) break;
        quests.sactive.erase(q);
        break;
    }
    case 0x024A: {                              // an NPC's Give_Item / Take_Item: [n]{bag record}
        const uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        if (r.ok) quests.npcItemsAt = now;
        break;
    }
    case 0x00C3: {                              // 195 an NPC move took its items: [n]{bag record}[m]{u32 item, u16 count}
        const uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        break;
    }

    // ── an exchange shop (exchange_shop.cpp) ──
    case 0x00F9: {                              // 249 opened: [u8 n]{u16 ExchangeID}
        const uint8_t n = r.u8();
        std::vector<uint16_t> ids;
        for (uint8_t i = 0; i < n && r.ok; ++i) ids.push_back(r.u16());
        if (!r.ok) break;
        npc.exIds = std::move(ids);
        npc.exListAt = now;
        npc.talkNpc = 0;                        // (the server ended the talk: S2C 117 before it)
        break;
    }
    case 0x00FF: {                              // 255 exchanged: [u8 n]{bag record}
        const uint8_t n = r.u8();
        for (uint8_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        if (r.ok) npc.exDoneAt = now;
        break;
    }
    case 0x0100:                                // 256 no such exchange / no room
        npc.exFailAt = now;
        break;
    case 0x00BF: {                              // 191 [i32]: an exchange's / a compound's refusal (-1; 0 = no room)
        const int32_t v = int32_t(r.u32());
        if (!r.ok) break;
        if (v == -1) npc.exFailAt = now;
        npc.cmpFailAt = now;
        npc.cmpFailCode = v;
        break;
    }
    // ── a compounder (compound.cpp) ──
    case 0x00BC: {                              // 188 opened: [u8 n][u16 grade]{u16 CompoundID}
        const uint8_t n = r.u8();
        r.u16();
        std::vector<uint16_t> ids;
        for (uint8_t i = 0; i < n && r.ok; ++i) ids.push_back(r.u16());
        if (!r.ok) break;
        npc.cmpIds = std::move(ids);
        npc.cmpListAt = now;
        npc.talkNpc = 0;                        // (S2C 117 before it)
        break;
    }
    case 0x00E4: {                              // 228 the roll: [u8 rate]
        const uint8_t rate = r.u8();
        if (r.ok) { npc.cmpRate = rate; npc.cmpRateAt = now; }
        break;
    }
    case 0x00BE: {                              // 190 made: [u8 n]{u32 res}[u32 waste][u8 made][u8 failed][u8 m]{record}
        const uint8_t n = r.u8();
        for (uint8_t i = 0; i < n && r.ok; ++i) r.u32();
        r.u32();
        const uint8_t made = r.u8(), failed = r.u8(), m = r.u8();
        for (uint8_t i = 0; i < m && r.ok; ++i) apply_bag_record(r, seq);
        if (!r.ok) break;
        npc.cmpMade = made;
        npc.cmpFailed = failed;
        npc.cmpDoneAt = now;
        break;
    }
    case 0x00C1:                                // 193 the compound window closed
        npc.cmpCloseAt = now;
        break;
    case 0x0102:                                // 258 the exchange window closed
        npc.exCloseAt = now;
        break;
    case 0x023A: {                              // 570 a job change: [u16 eid][u16 type][u8 job][u8 hair][u8 tier][u8]
        const uint16_t eid = r.u16();
        r.u16();
        const uint8_t job = r.u8();
        if (r.ok && eid == self.eid && job) self.job = job;
        break;
    }
    case 0x0075:                                // []: the dialog ended
        npc.talkEndAt = now;
        npc.talkNpc = 0;
        break;

    // ── the monster quest (monster_quest.cpp) ──
    case 0x00D2: {                              // 210 taken
        MonQuestState m;
        m.qid = r.u16();
        m.monster = r.u16();
        m.grade = r.u8();
        m.run = r.u8();
        m.kills = r.u16();
        r.u32();                                // the record
        m.name = r.cstr();
        m.client = r.cstr();
        m.pos = r.cstr();
        r.cstr();                               // the comment (the client's format)
        if (!r.ok) break;
        m.has = true;
        m.takenAt = now;
        m.completed = monq.completed;
        m.lastExp = monq.lastExp; m.lastTm = monq.lastTm; m.lastGelder = monq.lastGelder;
        monq = std::move(m);
        if (monq.run >= 1) quests.monHist[monq.qid] = QuestHist{ monq.run - 1, monq.run - 1 };
        event(seq, fmt("monster quest %u taken (run %u): %s", monq.qid, monq.run, monq.name.c_str()));
        break;
    }
    case 0x00D3: {                              // 211 the entry gone
        const uint16_t q = r.u16();
        if (!r.ok) break;
        monq.removedAt = now;
        monq.removedQid = q;
        if (monq.qid == q) { monq.has = false; monq.success = false; monq.timerAt = 0; }
        break;
    }
    case 0x00D7:                                // 215 the timer [remain][total]
        monq.remainMs = r.u32();
        monq.totalMs = r.u32();
        if (r.ok) monq.timerAt = now;
        break;
    case 0x00D8:                                // 216 the time ran out [qid]
        r.u16();
        if (!r.ok) break;
        monq.timeoutAt = now;
        monq.timerAt = 0;
        monq.remainMs = 0;
        break;
    case 0x00D9:                                // 217 [kills][target]
        monq.kills = r.u16();
        monq.target = r.u16();
        break;
    case 0x00DA: {                              // 218 the kills done [qid][u8 a return map]
        const uint16_t q = r.u16();
        const uint8_t portal = r.u8();
        if (!r.ok) break;
        if (!monq.has || monq.qid == q) {
            monq.success = true;
            monq.portal = portal != 0;
            monq.successAt = now;
            event(seq, fmt("monster quest %u: all killed", q));
        }
        break;
    }
    case 0x00DB: {                              // 219 handed in [n]{record}[m]{u32 item, u16}[exp][tm exp][qid][gelder]
        const uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok; ++i) apply_bag_record(r, seq);
        const uint16_t m = r.u16();
        r.skip(size_t(m) * 6);
        const uint32_t exp = r.u32(), tm = r.u32();
        const uint16_t q = r.u16();
        const uint32_t gelder = r.u32();
        if (!r.ok) break;
        monq.doneAt = now;
        ++monq.completed;
        monq.lastExp = exp; monq.lastTm = tm; monq.lastGelder = gelder;
        if (monq.qid == q && monq.run >= 1) quests.monHist[q] = QuestHist{ monq.run, monq.run };
        event(seq, fmt("monster quest %u handed in: %u EXP, %u TM EXP, %u galder", q, exp, tm, gelder));
        break;
    }
    case 0x00EB: {                              // 235 the list (login / map change)
        const uint16_t n = r.u16();
        if (!r.ok) break;
        if (!n) { monq.has = false; monq.timerAt = 0; break; }
        const uint16_t q = r.u16(), mon = r.u16();
        const uint8_t run = r.u8(), grade = r.u8();
        const uint16_t kills = r.u16();
        const uint32_t record = r.u32();
        if (!r.ok) break;
        const bool same = monq.has && monq.qid == q;
        if (!same) { const MonQuestState keep = monq; monq = MonQuestState{}; monq.completed = keep.completed;
                     monq.lastExp = keep.lastExp; monq.lastTm = keep.lastTm; monq.lastGelder = keep.lastGelder; }
        monq.has = true;
        monq.qid = q; monq.monster = mon; monq.run = run; monq.grade = grade; monq.kills = kills;
        monq.success = (same && monq.success) || record > 0;
        if (run >= 1) quests.monHist[q] = QuestHist{ run - 1, run - 1 };
        break;
    }
    case 0x0411: monq.returnOkAt = now; break;  // the portal back: the map change comes in 500 ms
    case 0x0412: monq.returnNoAt = now; break;  //   refused (the quest's timer stopped / no return map)

    // ── shops (shop.cpp) ──
    case 0x00A8: {                              // [u16 shop][cstr script]: its window opened (state 5)
        const uint16_t id = r.u16();
        r.cstr();
        if (!r.ok) break;
        npc.shop = id;
        npc.shopAt = now;
        npc.talkNpc = 0;
        break;
    }
    case 0x0047: {                              // [u16 n] n*{u32 item, u32 price}: what it sells
        const uint16_t n = r.u16();
        std::map<uint32_t, uint32_t> list;
        for (uint16_t i = 0; i < n && r.ok; ++i) {
            const uint32_t id = r.u32(), price = r.u32();
            list[id] = price;
        }
        if (!r.ok) break;
        npc.prices = std::move(list);
        npc.pricesFor = npc.shop;
        npc.pricesAt = now;
        break;
    }
    case 0x004A:                                // []: not bought (money / the list)
    case 0x004B:                                // []: not bought (the weight)
        npc.buyFailAt = now;
        npc.buyFail = h.opcode;
        break;
    case 0x004D:                                // []: not sold
    case 0x0355:                                // [u16 0]: not sold (the money cap)
        npc.sellFailAt = now;
        r.skip(r.left());
        break;
    case 0x0023:                                // []: a pick-up refused, "OverLimitedWt"
        overweightAt = now;
        break;

    // ── the warehouse (warehouse.cpp) ──
    case 0x0089: {                              // [u8 tab][u8 sent] (not sent: [u16 n] n*{[u8 kind][item][u8 0]} [u16 m] rare options)
        const uint8_t tab = r.u8();
        const uint8_t sent = r.u8();
        if (!r.ok) break;
        npc.whOpenAt = now;
        if (sent) break;
        const uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && r.ok; ++i) {
            ItemData d;
            d.kind = r.u8();
            read_item_data(r, d);
            if (r.u8()) r.skip(21);             // an expiry (never written by ProjectPAULA)
        }
        if (!r.ok) break;
        npc.whItems[tab] = n;
        r.skip(r.left());                       // the rare items' options
        break;
    }
    case 0x008C: {                              // [u8 tab][bag record][tab record]: stored
        const uint8_t tab = r.u8();
        apply_bag_record(r, seq);
        const uint8_t t = apply_bag_record(r, seq, false);
        if (!r.ok) break;
        if (t != 1) ++npc.whItems[tab];         // a new item in the tab (1 = it joined a stack)
        npc.whAddAt = now;
        break;
    }
    case 0x008D: {                              // [u8 tab]: not stored
        const uint8_t tab = r.u8();
        if (!r.ok) break;
        npc.whFailAt = now;
        npc.whFailTab = tab;
        break;
    }

    default:
        known = false;
        break;
    }

    if (!known) return;
    if (r.ok) {
        ++applied;
    } else {
        ++malformed;
        lastMalformed = fmt("0x%04X seq %llu (%zu bytes)", h.opcode, (unsigned long long)seq, p.data.size());
    }
}

} // namespace nj
