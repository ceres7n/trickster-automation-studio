// C2S action builders. Each layout is the ProjectPAULA handler's read sequence (= the client's
// sender); the refusals mirror the server checks that would otherwise drop the session or lock the
// client, so a bad request is caught here instead of on the wire.
#include "app/actions.h"
#include "app/game_data.h"
#include "app/text_util.h"
#include "app/world.h"
#include <algorithm>
#include <cmath>

namespace nj {
namespace {

struct Body {
    std::vector<uint8_t> b;
    Body& u8(uint8_t v)   { b.push_back(v); return *this; }
    Body& u16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); return *this; }
    Body& u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Body& u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
};

Action make(uint16_t op, Body body, std::string label)
{
    Action a;
    a.op = op;
    a.body = std::move(body.b);
    a.label = std::move(label);
    return a;
}

Action fail(uint16_t op, std::string why)
{
    Action a;
    a.op = op;
    a.error = std::move(why);
    return a;
}

uint16_t clamp_coord(int v) { return uint16_t(std::clamp(v, 0, 0xFFFF)); }

double dist2(int ax, int ay, int bx, int by) { return double(ax - bx) * (ax - bx) + double(ay - by) * (ay - by); }

} // namespace

const Entity* nearest_monster(const WorldModel& w, uint64_t now)
{
    int px, py;
    w.self_pos_at(now, px, py);
    const Entity* best = nullptr;
    double bestD = 1e18;
    for (const auto& kv : w.entities) {
        const Entity& e = kv.second;
        if (e.kind != EK_MONSTER || e.dead) continue;
        const double d = dist2(e.x, e.y, px, py);
        if (d < bestD) { bestD = d; best = &e; }
    }
    return best;
}

const GroundItem* nearest_item(const WorldModel& w, uint64_t now)
{
    int px, py;
    w.self_pos_at(now, px, py);
    const GroundItem* best = nullptr;
    double bestD = 1e18;
    for (const auto& kv : w.groundItems) {
        const double d = dist2(kv.second.x, kv.second.y, px, py);
        if (d < bestD) { bestD = d; best = &kv.second; }
    }
    return best;
}

bool usable_item(const WorldModel& w, const InvItem& it)
{
    if (it.equipSlot >= 0 || it.count == 0) return false;
    const ItemInfo* info = w.data() ? w.data()->item(it.itemId) : nullptr;
    return info && (info->options & 2);
}

bool usable_skill(const WorldModel& w, const SkillEntry& sk, std::string* why)
{
    auto no = [&](const char* r) { if (why) *why = r; return false; };
    const SkillInfo* info = w.data() ? w.data()->skill(sk.id) : nullptr;
    // case 238 / 224: a skill the server does not know, or a passive one, closes the session (sub_8AA290)
    if (!info) return no("not in skill_db (the server would drop the session)");
    if (info->type == 1) return no("passive (the server would drop the session)");
    if (sk.level == 0 || sk.level > 11) return no("level out of range");
    if (info->esaui == 0 && (info->skillOption & 0x10)) return no("SkillOption 0x10: never used through 0x00EE");
    if (info->esaui != 0 && !esaui_object(info->esaui) && !esaui_point(info->esaui))
        return no("unknown selection kind");
    return true;
}

const GunInfo* equipped_gun(const WorldModel& w, uint32_t* itemId)
{
    const InvItem* it = w.self.equip[1] ? w.find_item(w.self.equip[1]) : nullptr;
    const GameData* gd = w.data();
    if (!it || !gd) return nullptr;
    const ItemInfo* ii = gd->item(it->itemId);
    if (!ii || ii->subType != 54) return nullptr;
    if (itemId) *itemId = it->itemId;
    return gd->gun(it->itemId);
}

int loaded_bullets(const WorldModel& w)
{
    const InvItem* it = w.self.equip[20] ? w.find_item(w.self.equip[20]) : nullptr;
    const ItemInfo* ii = it && w.data() ? w.data()->item(it->itemId) : nullptr;
    return ii && ii->type == 29 ? it->count : 0;
}

uint64_t spare_bullets(const WorldModel& w)
{
    if (!w.data()) return 0;
    for (const InvItem& it : w.inventory) {
        if (it.uid == w.self.equip[20] || it.count == 0) continue;
        const ItemInfo* ii = w.data()->item(it.itemId);
        if (ii && ii->type == 29) return it.uid;
    }
    return 0;
}

// FastShot (gun.cpp): (int)((int)(20000 / (GaugeSpeed * 1/6)) * 0.7) ms, the retail float constants.
int gun_shot_interval_ms(const GunInfo& g)
{
    if (g.gaugeSpeed <= 0) return 3000;
    const int predict = int(20000.0 / (double(g.gaugeSpeed) * 0.1666666716337204));
    return int(double(predict) * 0.699999988079071);
}

const InvItem* equipped_drill(const WorldModel& w)
{
    const InvItem* it = w.self.equip[7] ? w.find_item(w.self.equip[7]) : nullptr;
    const ItemInfo* ii = it && w.data() ? w.data()->item(it->itemId) : nullptr;
    return ii && ii->type == 6 ? it : nullptr;
}

const DrillInfo* equipped_drill_info(const WorldModel& w)
{
    const InvItem* it = equipped_drill(w);
    return it && w.data() ? w.data()->drill(it->itemId) : nullptr;
}

// The gauge (Driller sub_562A90): held, the bar rises dt / 6 * BarSpeed / 100 per ms; from 200 - BarEffArea on
// the drill digs, ScoopMass every 120 ms of that state (held or let go), and the avatar's check (sub_6B3110)
// stops it at the item's depth / the map's / DigDepth. Past 200 it overheats (the depth back to 0, one more start).
int drill_dig_ms(const DrillInfo& d, int depth)
{
    const int bar = d.barSpeed > 0 ? int(std::ceil(600.0 * std::max(0, 200 - d.barEffArea) / d.barSpeed)) : 2000;
    const int sm = std::max(1, d.scoopMass);
    const int scoops = depth > 0 ? (depth + sm - 1) / sm : 0;
    return bar + 120 * scoops;
}

// drill.cpp shortest_dig_ms (sub_5E8F20), the retail float arithmetic.
int drill_shortest_ms(const DrillInfo& d, int depth, int mapDepth)
{
    if (d.barSpeed <= 0) return 0;
    const int a3 = depth > 0 ? depth : mapDepth;
    const float v8 = float(double(100 * (200 - d.barEffArea)) / (double(d.barSpeed) * 0.1666666716337204));
    const float v9 = float(d.scoopMass ? 120 * a3 / d.scoopMass : 0);
    const float v6 = v9 + v8;
    if (v6 < 0.0f) return 0;
    return int(int64_t(float(double(v6) * 0.699999988079071)));
}

uint16_t drill_crc(uint16_t seed, const DrillInfo& d, int depth, int cnt)
{
    return uint16_t(uint16_t(d.barEffArea) ^ uint16_t(d.barSpeed << 1) ^ uint16_t(d.scoopMass << 5) ^
                    uint16_t(depth << 8) ^ uint16_t(cnt << 12) ^ seed);
}

namespace act {

// C2S 0x1DD (gun.cpp handle_gun_attack): the server takes (crc ^ (key & 0xFFFE)) & 0xFFFE as the
// seed, key = PowStartPoint<<5 ^ PowRange<<8 ^ GaugeSpeed<<12; bit 0 = a power shot (never here).
// A wrong CRC or a shot before the FastShot time gets NO answer and counts: three in a row cut the
// player. An accepted one brings a new seed (S2C 0x0200) even when it then fails (0x0136).
Action gun_shot(const WorldModel& w, uint16_t target, bool run)
{
    uint32_t gunId = 0;
    const GunInfo* g = equipped_gun(w, &gunId);
    if (!g) return fail(0x01DD, gunId ? "the gun has no Item_GunParam2 row" : "no gun in the weapon slot");
    if (!target) return fail(0x01DD, "no target");
    if (!w.self.gunSeeds) return fail(0x01DD, "no seed yet (S2C 0x0200)");
    if (loaded_bullets(w) < 1) return fail(0x01DD, "no bullets loaded");
    const uint32_t key = (uint32_t(uint16_t(g->powStart)) << 5) ^ (uint32_t(uint16_t(g->powRange)) << 8) ^
                         (uint32_t(uint16_t(g->gaugeSpeed)) << 12);
    const uint16_t crc = uint16_t((w.self.gunSeed ^ key) & 0xFFFE);
    return make(0x01DD, Body().u16(target).u16(crc).u16(1).u8(run ? 2 : 1),
                fmt("shoot %s", w.entity_label(target).c_str()));
}

// C2S 0x205 (handle_equip_common): [u64 uid][u8 slot] -> S2C 0x206 [uid][slot] / 0x207 / 0x20A.
Action load_bullets(const WorldModel& w, uint64_t uid)
{
    const InvItem* it = w.find_item(uid);
    if (!it) return fail(0x0205, "not in the bag");
    const std::string name = w.data() ? w.data()->item_name(it->itemId) : fmt("item %u", it->itemId);
    return make(0x0205, Body().u64(uid).u8(20), fmt("load %s x%u", name.c_str(), it->count));
}

Action gun_seed_request()
{
    return make(0x01FF, Body(), "ask for a gun seed");
}

// C2S 0x2C (handle_drill_start): swallowed while sitting / resting, in a shop, a compound or a dig already open.
Action drill_start(int x, int y, bool run)
{
    return make(0x002C, Body().u16(clamp_coord(x)).u16(clamp_coord(y)).u8(run ? 2 : 1), fmt("dig at (%d,%d)", x, y));
}

// C2S 0x31 (handle_drill_end). Everything the server would cut the session for is refused here: no seed, cnt 0,
// a depth past the drill's DigDepth or the map's depth, a find reported before its shortest dig time.
Action drill_end(const WorldModel& w, int cnt, int depth, uint64_t now)
{
    const DrillInfo* d = equipped_drill_info(w);
    if (!d) return fail(0x0031, "no drill in slot 7 (or no Item_DrillParam row)");
    if (w.drill.phase != DrillSession::Info) return fail(0x0031, "no dig open (no S2C 371)");
    if (w.drill.premium) return fail(0x0031, "an auto-drill dig (371 form byte set): not modelled");
    if (cnt < 1) return fail(0x0031, "cnt 0 (the server cuts the session)");
    if (depth < 0 || depth > d->digDepth || depth > w.drill.mapDepth)
        return fail(0x0031, fmt("depth %d past the drill's %d / the map's %d (the server cuts the session)", depth,
                                d->digDepth, w.drill.mapDepth));
    const uint64_t ms = now > w.drill.infoAt ? (now - w.drill.infoAt) / 10000 : 0;
    if (w.drill.itemDepth && depth == w.drill.itemDepth &&
        ms < uint64_t(drill_shortest_ms(*d, depth, w.drill.mapDepth)) + 100)
        return fail(0x0031, fmt("too soon: %llu ms, the server wants %d (it would cut the session)", (unsigned long long)ms,
                                drill_shortest_ms(*d, depth, w.drill.mapDepth)));
    const uint16_t crc = drill_crc(w.drill.seed, *d, depth, cnt);
    return make(0x0031, Body().u16(uint16_t(cnt)).u16(uint16_t(ms)).u16(uint16_t(depth)).u16(crc),
                fmt("dug %d deep%s", depth, w.drill.itemDepth && depth == w.drill.itemDepth ? " (the item)" : ""));
}

// C2S 0x30 (handle_drill_cancel): the life it costs = cnt (twice on a ground of another nature).
Action drill_cancel(const WorldModel& w, int cnt, uint64_t now)
{
    const uint64_t from = w.drill.infoAt ? w.drill.infoAt : w.drill.askedAt;
    const uint64_t ms = from && now > from ? (now - from) / 10000 : 0;
    return make(0x0030, Body().u16(uint16_t(std::max(0, cnt))).u16(uint16_t(ms)), "cancel the dig");
}

// C2S 0x51 (equip.cpp handle_equip_item): the item in that slot comes off with it; S2C 0x55 + 0x52, or 0x53.
Action equip(const WorldModel& w, uint64_t uid, uint8_t slot)
{
    const InvItem* it = w.find_item(uid);
    if (!it) return fail(0x0051, "not in the bag");
    const std::string name = w.data() ? w.data()->item_name(it->itemId) : fmt("item %u", it->itemId);
    return make(0x0051, Body().u64(uid).u8(slot), fmt("equip %s", name.c_str()));
}

// C2S 0x1D (game_session.cpp handle_attack): [u16 target][u8 move type] (0 counts as run). S2C 0x1E
// acks it; the server chases (LazyBattleFollowing) and swings; the client walks on the S2C 0x18 it
// gets for itself.
Action attack(const WorldModel& w, uint16_t eid, bool run)
{
    if (!eid) return fail(0x001D, "no target");
    return make(0x001D, Body().u16(eid).u8(run ? 2 : 1), fmt("attack %s", w.entity_label(eid).c_str()));
}

// C2S 0x21 (inventory.cpp handle_pickup): [u16 oid][u8 move type] — walks there first when farther
// than 4 cells.
Action pick_up(const WorldModel& w, uint16_t oid, bool run)
{
    auto it = w.groundItems.find(oid);
    if (it == w.groundItems.end()) return fail(0x0021, "item not on the ground");
    const std::string name = w.data() ? w.data()->item_name(it->second.itemId) : fmt("item %u", it->second.itemId);
    return make(0x0021, Body().u16(oid).u8(run ? 2 : 1), fmt("pick up %s #%u", name.c_str(), oid));
}

// C2S 0x40 (Player case 64, handle_action): [u8] 0 stand / 2 sit (state 8) / 13 rest (state 15).
Action sit(uint8_t state)
{
    const char* what = state == 2 ? "sit" : state == 13 ? "rest" : "stand";
    return make(0x0040, Body().u8(state), what);
}

// C2S 0x57 (case 87, item_use.cpp handle_use_item): [u64 uid]. No such item -> S2C 0x5A; a potion
// answers with the block {0x97, 0x5B, 0x24/0x25}. The client's own use-lock is not set by an
// injected request (it is the client's sender that sets it), and every answer clears it anyway.
Action use_item(const WorldModel& w, uint64_t uid)
{
    const InvItem* it = w.find_item(uid);
    if (!it) return fail(0x0057, "not in the bag");
    if (it->equipSlot >= 0) return fail(0x0057, "worn (unequip it first)");
    const ItemInfo* info = w.data() ? w.data()->item(it->itemId) : nullptr;
    if (!info || !(info->options & 2)) return fail(0x0057, "not usable (ItemParam Options & 2)");
    if (info->minLevel > 0 && w.self.level > 0 && w.self.level < info->minLevel)
        return fail(0x0057, fmt("needs level %d", info->minLevel));
    return make(0x0057, Body().u64(uid), fmt("use %s x%u", info->name.c_str(), it->count));
}

Action use_item_custom(const WorldModel& w, uint64_t uid, const std::vector<uint8_t>& data, const std::string& what)
{
    const InvItem* it = w.find_item(uid);
    if (!it) return fail(0x0108, "not in the bag");
    if (it->equipSlot >= 0) return fail(0x0108, "worn");
    const ItemInfo* info = w.data() ? w.data()->item(it->itemId) : nullptr;
    if (!info || !(info->options & 2) || !(info->options & 0x8000)) return fail(0x0108, "not an item used with data (Options 0x8000)");
    if (data.size() > 256) return fail(0x0108, "more than 256 bytes");
    Body b;
    b.u64(uid).u16(uint16_t(data.size()));
    for (uint8_t v : data) b.u8(v);
    return make(0x0108, b, fmt("use %s: %s", info->name.c_str(), what.c_str()));
}

// A skill (skill.cpp). Without a selection UI (ESAUI 0): C2S 0xEE [u16 skill][u8 lv], the level the
// server has (lv != learned -> ignored). With one: the client's answer to S2C 241 directly —
// C2S 0xE0 [u8 2][u16 skill][u8 lv][u16 uiKind][u16 size][payload], uiKind = ESAUI (the client's
// ESAUISelect class passes its kind, e.g. sub_C0B180 -> c2s_224(.., 1, ..)); kinds 1/7/8 carry
// [u16 target], 3/6/10 [u16 x][u16 y]. Case 224 needs no 0xEE before it (it only clears the
// selection flag), so the client's selection cursor never opens.
Action use_skill(const WorldModel& w, uint16_t skill, const SkillTarget& t, uint64_t now)
{
    const SkillEntry* sk = nullptr;
    for (const SkillEntry& e : w.skills)
        if (e.id == skill) { sk = &e; break; }
    if (!sk) return fail(0x00E0, "not learned (the server would drop the session)");
    std::string why;
    if (!usable_skill(w, *sk, &why)) return fail(0x00E0, why);
    const SkillInfo& info = *w.data()->skill(skill);
    const std::string name = info.name.empty() ? fmt("skill %u", skill) : info.name;

    if (info.esaui == 0)
        return make(0x00EE, Body().u16(skill).u8(sk->level), fmt("%s Lv %u", name.c_str(), sk->level));

    SkillTarget::Mode mode = t.mode;
    if (mode == SkillTarget::Auto) mode = info.buff() ? SkillTarget::Self : SkillTarget::NearestMonster;
    uint16_t eid = 0;
    int x = 0, y = 0;
    switch (mode) {
    case SkillTarget::Self:
        eid = w.self.eid;
        w.self_pos_at(now, x, y);
        break;
    case SkillTarget::NearestMonster:
        if (const Entity* m = nearest_monster(w, now)) { eid = m->eid; x = m->x; y = m->y; }
        break;
    case SkillTarget::Entity:
        eid = t.eid;
        if (auto it = w.entities.find(eid); it != w.entities.end()) { x = it->second.x; y = it->second.y; }
        else if (eid == w.self.eid) w.self_pos_at(now, x, y);
        else eid = 0;
        break;
    case SkillTarget::Point:
        x = t.x; y = t.y;
        break;
    default:
        break;
    }

    Body b;
    b.u8(2).u16(skill).u8(sk->level).u16(uint16_t(info.esaui));
    if (esaui_object(info.esaui)) {
        if (!eid) return fail(0x00E0, mode == SkillTarget::Point ? "this skill takes a target, not a point"
                                                                 : "no target");
        b.u16(2).u16(eid);
        return make(0x00E0, b, fmt("%s Lv %u -> %s", name.c_str(), sk->level,
                                   eid == w.self.eid ? "self" : w.entity_label(eid).c_str()));
    }
    if (mode != SkillTarget::Point && !eid) return fail(0x00E0, "no target point");
    b.u16(4).u16(clamp_coord(x)).u16(clamp_coord(y));
    return make(0x00E0, b, fmt("%s Lv %u at (%d,%d)", name.c_str(), sk->level, x, y));
}

// C2S 0x18 (case 24, game_session.cpp handle_move): [u16 srcX][u16 srcY][u16 dstX][u16 dstY][u8 type],
// type 1 walk / 2 run. The server trusts the source, moves us and echoes S2C 0x18 to ourselves —
// the same packet its chase sends, which the client walks on. Source = where the model has us now.
Action move(const WorldModel& w, int x, int y, bool run, uint64_t now)
{
    if (!w.inWorld || !w.self.eid) return fail(0x0018, "not in the world");
    int sx, sy;
    w.self_pos_at(now, sx, sy);
    if (sx == x && sy == y) return fail(0x0018, "already there");
    Body b;
    b.u16(clamp_coord(sx)).u16(clamp_coord(sy)).u16(clamp_coord(x)).u16(clamp_coord(y)).u8(run ? 2 : 1);
    return make(0x0018, b, fmt("%s (%d,%d) -> (%d,%d)", run ? "run" : "walk", sx, sy, x, y));
}

Action stop_at(int x, int y, bool run)
{
    Body b;
    b.u16(clamp_coord(x)).u16(clamp_coord(y)).u16(clamp_coord(x)).u16(clamp_coord(y)).u8(run ? 2 : 1);
    return make(0x0018, b, fmt("stand at (%d,%d)", x, y));
}

// C2S 0x1D4 (case 468, handle_portal_move): [u16 dstX][u16 dstY][u8 type] — the same walk from where
// the SERVER has us, with the portal armed (the client sends it for a click inside a portal).
Action portal_walk(int x, int y, bool run)
{
    Body b;
    b.u16(clamp_coord(x)).u16(clamp_coord(y)).u8(run ? 2 : 1);
    return make(0x01D4, b, fmt("portal walk to (%d,%d)", x, y));
}

Action go_to_town()
{
    return make(0x0016, Body(), "go to town");
}

Action npc_talk(const WorldModel& w, uint16_t npc)
{
    if (!npc) return fail(0x006F, "no NPC");
    return make(0x006F, Body().u16(npc), fmt("talk to %s", w.entity_label(npc).c_str()));
}

Action npc_script(uint16_t npc, uint8_t func, const std::vector<uint16_t>& args, const std::string& label)
{
    if (!npc) return fail(0x007F, "no NPC");
    if (args.size() > 255) return fail(0x007F, "too many arguments");
    Body b;
    b.u16(npc).u8(func).u8(uint8_t(args.size()));
    for (uint16_t a : args) b.u16(a);
    return make(0x007F, b, label);
}

Action npc_next(uint16_t npc, int index)
{
    if (!npc) return fail(0x0072, "no NPC");
    if (index < 0 || index > 255) return fail(0x0072, fmt("message %d is not a plain one", index));
    return make(0x0072, Body().u16(npc).u8(uint8_t(index)), fmt("the NPC's message %d", index));
}

Action monq_return()
{
    return make(0x0410, Body().u32(2), "monster quest: the portal back");
}

Action npc_end()
{
    return make(0x0075, Body(), "close the NPC dialog");
}

Action shop_open(const WorldModel& w, uint16_t shop)
{
    if (!shop) return fail(0x0046, "no shop");
    return make(0x0046, Body().u16(shop), fmt("open the shop %s", w.entity_label(shop).c_str()));
}

Action shop_prices(uint16_t shop)
{
    return make(0x00A4, Body().u16(shop), "ask the shop's prices");
}

// The server cuts the session when n * 6 != the payload - 4, and refuses n outside 1..20, an item id 0
// or a count 0 (shop.cpp handle_shop_buy).
Action shop_buy(const WorldModel& w, uint16_t shop, const std::vector<ShopLine>& lines)
{
    if (!shop) return fail(0x0048, "no shop");
    if (lines.empty() || lines.size() > 20) return fail(0x0048, "1..20 lines");
    Body b;
    b.u16(shop).u16(uint16_t(lines.size()));
    int items = 0;
    for (const ShopLine& l : lines) {
        if (!l.item || !l.count) return fail(0x0048, "an empty line");
        b.u32(l.item).u16(l.count);
        items += l.count;
    }
    const std::string first = w.data() ? w.data()->item_name(lines[0].item) : fmt("item %u", lines[0].item);
    return make(0x0048, b, lines.size() == 1 ? fmt("buy %s x%u", first.c_str(), lines[0].count)
                                             : fmt("buy %zu kinds (%d items)", lines.size(), items));
}

Action shop_sell(const WorldModel& w, uint16_t shop, const std::vector<SellLine>& lines)
{
    if (!shop) return fail(0x039F, "no shop");
    if (lines.empty() || lines.size() > 20) return fail(0x039F, "1..20 lines");
    Body b;
    b.u16(0x004C).u8(0);                        // inner 76, an empty second password
    b.u16(shop).u16(uint16_t(lines.size()));
    for (const SellLine& l : lines) {
        const InvItem* it = w.find_item(l.uid);
        if (!it || it->itemId != l.item) return fail(0x039F, "an item not in the bag");
        if (it->equipSlot >= 0) return fail(0x039F, "a worn item");
        if (!l.count || l.count > it->count) return fail(0x039F, "a bad count");
        b.u64(l.uid).u32(l.item).u16(l.count);
    }
    const std::string first = w.data() ? w.data()->item_name(lines[0].item) : fmt("item %u", lines[0].item);
    return make(0x039F, b, lines.size() == 1 ? fmt("sell %s x%u", first.c_str(), lines[0].count)
                                             : fmt("sell %zu items", lines.size()));
}

Action shop_close()
{
    return make(0x004F, Body(), "close the shop");
}

Action wh_add(const WorldModel& w, uint8_t tab, uint64_t uid, uint16_t count)
{
    const InvItem* it = w.find_item(uid);
    if (!it) return fail(0x008B, "not in the bag");
    if (it->equipSlot >= 0) return fail(0x008B, "worn");
    if (!count || count > it->count) return fail(0x008B, "a bad count");
    const std::string name = w.data() ? w.data()->item_name(it->itemId) : fmt("item %u", it->itemId);
    return make(0x008B, Body().u8(tab).u64(uid).u32(it->itemId).u16(count), fmt("store %s x%u", name.c_str(), count));
}

Action wh_close()
{
    return make(0x008A, Body(), "close the warehouse");
}

Action exchange(const WorldModel& w, int id, const std::vector<SellLine>& give)
{
    if (give.empty() || give.size() > 5) return fail(0x00FE, "1..5 items");
    Body b;
    b.u16(uint16_t(id)).u8(uint8_t(give.size()));
    std::string what;
    for (const SellLine& l : give) {
        if (!w.find_item(l.uid)) return fail(0x00FE, "not in the bag");
        b.u64(l.uid).u32(l.item).u16(l.count);
        what += (what.empty() ? "" : ", ") + fmt("%s x%u", w.data() ? w.data()->item_name(l.item).c_str() : "?", l.count);
    }
    b.u8(1);
    return make(0x00FE, b, fmt("exchange %d: %s", id, what.c_str()));
}

Action exchange_close()
{
    return make(0x0101, Body(), "close the exchange shop");
}

Action compound_roll(const WorldModel& w, int id, int spins, const std::vector<SellLine>& give)
{
    if (give.empty() || give.size() > 5) return fail(0x00E3, "1..5 items");
    if (spins < 1 || spins > 255) return fail(0x00E3, "1..255 spins");
    Body b;
    b.u16(uint16_t(id)).u8(10).u8(uint8_t(spins)).u8(uint8_t(give.size()));
    std::string what;
    for (const SellLine& l : give) {
        if (!w.find_item(l.uid)) return fail(0x00E3, "not in the bag");
        b.u64(l.uid).u32(l.item).u16(l.count);
        what += (what.empty() ? "" : ", ") + fmt("%s x%u", w.data() ? w.data()->item_name(l.item).c_str() : "?", l.count);
    }
    return make(0x00E3, b, fmt("compound %d x%d: %s", id, spins, what.c_str()));
}

Action compound_make(const WorldModel& w, const std::vector<SellLine>& give)
{
    if (give.empty() || give.size() > 5) return fail(0x00BA, "1..5 items");
    Body b;
    b.u8(uint8_t(give.size()));
    for (const SellLine& l : give) {
        if (!w.find_item(l.uid)) return fail(0x00BA, "not in the bag");
        b.u64(l.uid).u32(l.item).u16(l.count);
    }
    return make(0x00BA, b, "compound");
}

Action compound_close()
{
    return make(0x00C0, Body(), "close the compounder");
}

} // namespace act
} // namespace nj
