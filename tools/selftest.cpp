// tas_selftest — console check of the whole link without the UI:
//   1. loads + decodes against the packet db
//   2. launches a client (PreLaunch skipped), injects the DLL, links the pipes
//   3. prints hook state / DLL log / captured packets
//   4. on the first C2S, sends DE AD BE EF as opcode 0x1234 on that connection
// `tas_selftest 0` = db check only. Run it with a fake login server on LoginIP:LoginPort to see the captured and
// injected frames arrive. Kills the client at the end.
#include "app/config.h"
#include "app/game_data.h"
#include "app/item_sources.h"
#include "app/atlas.h"
#include "app/quest_graph.h"
#include "app/world.h"
#include "app/actions.h"
#include "app/packet_db.h"
#include "app/session.h"
#include "app/text_util.h"
#include "hook/client_addr.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <climits>
#include <set>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace nj;

static void decode_sample(PacketDb& db, uint8_t dir, uint16_t op, std::vector<uint8_t> data)
{
    const PacketDef* d = db.find(dir, op);
    printf("  %s 0x%04X %s:", dir == DIR_C2S ? "C2S" : "S2C", op, d ? d->name.c_str() : "(none)");
    if (!d || d->fields.empty()) { printf(" no fields\n"); return; }
    std::vector<DecodedRow> rows;
    db.decode(*d, data.data(), data.size(), rows);
    printf("\n");
    for (const DecodedRow& r : rows)
        printf("    %*s%-12s %-8s %s%s\n", r.depth * 2, "", r.label.c_str(), r.type.c_str(), r.value.c_str(),
               r.error ? "  <ERROR>" : "");
}


// ── world model check: a login + a fight + loot, built like ProjectPAULA writes them ──
struct Pk {
    std::vector<uint8_t> b;
    Pk& u8(uint8_t v) { b.push_back(v); return *this; }
    Pk& u16(uint16_t v) { u8(v & 0xFF); return u8(v >> 8); }
    Pk& u32(uint32_t v) { u16(v & 0xFFFF); return u16(v >> 16); }
    Pk& u64(uint64_t v) { u32((uint32_t)v); return u32((uint32_t)(v >> 32)); }
    Pk& str(const char* s) { while (*s) u8((uint8_t)*s++); return u8(0); }
    Pk& zeros(size_t n) { b.insert(b.end(), n, 0); return *this; }
};

static void world_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    printf("game data: %zu items, %zu mobs, %zu skills %s\n", gd.items(), gd.mobs(), gd.skills(), err.c_str());
    WorldModel w(&gd);
    uint64_t seq = 0;
    // also written as an exported capture, for TricksterAutomationStudio.exe --open
    const std::string capDir = exe_dir() + "captures\\";
    CreateDirectoryA(capDir.c_str(), nullptr);
    std::ofstream cap(capDir + "selftest_world.txt", std::ios::binary);
    cap << "# Trickster Automation Studio capture - selftest world sequence\r\n";
    auto feed = [&](uint8_t dir, uint16_t op, const Pk& p) {
        CapPacket c;
        c.hdr.seq = ++seq;
        c.hdr.dir = dir;
        c.hdr.opcode = op;
        c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
        c.data = p.b;
        w.feed(c);
        cap << fmt("%llu\t00:00:00.000\t%s\t0x%04X\t%u\t-\t\t", (unsigned long long)seq, dir == DIR_C2S ? "C>S" : "S>C",
                   op, c.hdr.size)
            << hex_preview(p.b.data(), p.b.size(), p.b.size()) << "\r\n";
    };
    const uint16_t ME = 500, MOB = 600;

    { Pk p; p.u64(0x1122334455667788ull).zeros(9); const char nm[20] = "Leao"; for (char ch : nm) p.u8((uint8_t)ch);
      p.u16(9).u8(3).u8(1).zeros(69 - 41); feed(DIR_S2C, 0x02A7, p); }
    feed(DIR_S2C, 0x00E6, Pk().u32(0).u16(ME).u16(1000).u16(800));
    feed(DIR_S2C, 0x00E7, Pk().u16(2)
                              .u8(1).u32(101).u64(1).u16(5)
                              .u8(2).u32(200).u64(2).u16(1).zeros(4).u8(1).u32(10).u32(5).u32(0));
    { Pk p; p.zeros(278);
      auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
      auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
      p.b[0] = 2;                                   // equip slot 0 = uid 2
      put16(184 + 28, 5); put16(184 + 30, 27);      // TM 5, level 27
      put16(219, 148); put32(217 + 14, 66208); put32(217 + 18, 1170); put32(217 + 22, 1410); put32(217 + 30, 12345);
      feed(DIR_S2C, 0x00E9, p); }
    feed(DIR_S2C, 0x0008, Pk().u16(MOB).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(1100).u16(800).u16(1100).u16(800)
                              .str("Sea Scorpion").zeros(14));
    feed(DIR_S2C, 0x0008, Pk().u16(601).u16(1).u16(9).u8(18).u8(0).u16(40).u32(0).u16(900).u16(700).u16(950).u16(700)
                              .str("Mari").zeros(55));
    feed(DIR_C2S, 0x0018, Pk().u16(1000).u16(800).u16(1080).u16(800).u8(0));
    feed(DIR_S2C, 0x001F, Pk().u16(ME).u16(1).u16(MOB).u16(1080).u16(800).u16(1100).u16(800).u16(100).u8(1));
    feed(DIR_S2C, 0x002A, Pk().u16(MOB).u32(100).zeros(24));
    feed(DIR_S2C, 0x011E, Pk().u16(MOB).u32(735).u16(0).u8(0));
    feed(DIR_S2C, 0x016D, Pk().u16(MOB).u16(ME));
    feed(DIR_S2C, 0x001F, Pk().u16(MOB).u16(1).u16(ME).u16(1100).u16(800).u16(1080).u16(800).u16(70).u8(1));
    feed(DIR_S2C, 0x002A, Pk().u16(ME).u32(70).zeros(24));
    feed(DIR_S2C, 0x002B, Pk().u16(MOB).u8(0));
    feed(DIR_S2C, 0x0406, Pk().u32(835));
    feed(DIR_S2C, 0x000A, Pk().u16(700).u16(1110).u16(805).u16(0).u8(0).u8(0).u8(0).u8(1).u32(101).u64(9).u16(3));
    feed(DIR_S2C, 0x000A, Pk().u16(701).u16(1120).u16(810).u16(0).u8(0).u8(0).u8(0).u8(1).u32(102).u64(10).u16(1));
    feed(DIR_S2C, 0x000F, Pk().u16(MOB).u8(0));
    feed(DIR_S2C, 0x0022, Pk().u16(ME).u16(700).u8(1));
    feed(DIR_S2C, 0x00A0, Pk().u8(1).u8(1).u32(101).u64(1).u16(8).u8(0));
    feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(102).u64(10).u16(1).u8(0));
    feed(DIR_S2C, 0x010F, Pk().u16(4).u16(1001).u8(5).u16(0).u16(1002).u8(1).u16(0)
                              .u16(2009).u8(3).u16(0).u16(1106).u8(1).u16(0));
    feed(DIR_S2C, 0x0024, Pk().u32(1170));
    feed(DIR_S2C, 0x0029, Pk().u32(66300));
    feed(DIR_S2C, 0x0018, Pk().u16(601).u16(950).u16(700).u16(990).u16(720).u8(0));
    feed(DIR_S2C, 0x0019, Pk().u16(601).u16(990).u16(720).u8(1));
    feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(2000).u64(11).u16(3).u8(0));          // Pink Potion A x3
    feed(DIR_S2C, 0x0008, Pk().u16(602).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(1200).u16(800).u16(1200)
                              .u16(800).str("Sea Scorpion").zeros(14));
    feed(DIR_C2S, 0x0018, Pk().u16(1080).u16(800).u16(1180).u16(800).u8(2));         // a run, at FILETIME 0
    feed(DIR_S2C, 0x00E9, Pk().zeros(10));                             // malformed on purpose

    const SelfState& me = w.self;
    printf("\nself: %s #%u uid %llx species %u | Lv %d TM %d | HP %lld/%lld MP %lld/%lld | EXP %lld (+%lld) | "
           "galder %lld | zone %d pos (%d,%d) moving=%d | kills %d\n",
           me.name.c_str(), me.eid, (unsigned long long)me.charUid, me.species, me.level, me.tmLevel, (long long)me.hp,
           (long long)me.hpMax, (long long)me.mp, (long long)me.mpMax, (long long)me.exp, (long long)me.expGained,
           (long long)me.money, me.zone, me.x, me.y, me.moving, me.kills);
    for (auto& kv : w.entities)
        printf("entity #%u %s '%s' Lv %u pos (%d,%d) moving=%d hp %lld/%lld dead=%d\n", kv.first,
               entity_kind_name(kv.second.kind), kv.second.name.c_str(), kv.second.level, kv.second.x, kv.second.y,
               kv.second.moving, (long long)kv.second.hp, (long long)kv.second.maxHp, kv.second.dead);
    for (auto& kv : w.groundItems)
        printf("ground #%u %s x%u at (%d,%d)\n", kv.first, gd.item_name(kv.second.itemId).c_str(), kv.second.count,
               kv.second.x, kv.second.y);
    for (const InvItem& i : w.inventory)
        printf("bag uid %llu %s x%u kind %u worn %d\n", (unsigned long long)i.uid, gd.item_name(i.itemId).c_str(), i.count,
               i.kind, i.equipSlot);
    for (const SkillEntry& sk : w.skills) printf("skill %u %s Lv %u\n", sk.id, gd.skill_name(sk.id).c_str(), sk.level);
    for (const std::string& e : w.events) printf("event %s\n", e.c_str());
    printf("applied %llu, malformed %llu (%s)\n", (unsigned long long)w.applied, (unsigned long long)w.malformed,
           w.lastMalformed.c_str());

    // the C2S action builders on this world (actions.cpp); the last C2S 0x18 started at FILETIME 0,
    // so 0.25 s later at the default 160 units/s we are 40 units along: (1120, 800)
    {
        const uint64_t now = 2500000;
        int px, py;
        w.self_pos_at(now, px, py);
        printf("\nself_pos_at(0.25 s) = (%d,%d) %s\n", px, py, px == 1120 && py == 800 ? "PASS" : "FAIL");
        int pass = 0, total = 0;
        auto check = [&](const Action& a, uint16_t op, const char* hex) {
            ++total;
            const std::string got = a.ok() ? hex_preview(a.body.data(), a.body.size(), a.body.size()) : "";
            const bool ok = hex ? (a.ok() && a.op == op && got == hex) : (!a.ok() && a.op == op);
            pass += ok;
            printf("  %s 0x%04X %-36s %s\n", ok ? "PASS" : "FAIL", a.op, a.ok() ? got.c_str() : "(refused)",
                   a.ok() ? a.label.c_str() : a.error.c_str());
        };
        SkillTarget autoT;
        check(act::attack(w, 602, true), 0x001D, "5A 02 02");
        if (const GroundItem* g = nearest_item(w, now)) check(act::pick_up(w, g->oid, true), 0x0021, "BD 02 02");
        check(act::sit(13), 0x0040, "0D");
        check(act::use_item(w, 11), 0x0057, "0B 00 00 00 00 00 00 00");
        check(act::use_item(w, 99), 0x0057, nullptr);                         // not in the bag
        // the Portable Port AD (Options 0x8000): C2S 0x0108 [u64 uid][i16 4][u32 ZoneIndex 19 = Gate of Vamp Castle]
        feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(19501).u64(12).u16(1).u8(0));
        check(act::use_item_custom(w, 12, { 19, 0, 0, 0 }, "to Gate of Vamp Castle"), 0x0108,
              "0C 00 00 00 00 00 00 00 04 00 13 00 00 00");
        check(act::use_item_custom(w, 11, { 19, 0, 0, 0 }, "a potion"), 0x0108, nullptr);   // no Options 0x8000
        check(act::use_skill(w, 1001, autoT, now), 0x00E0, "02 E9 03 05 01 00 02 00 5A 02");   // ESAUI 1 -> mob
        check(act::use_skill(w, 1002, autoT, now), 0x00EE, "EA 03 01");        // ESAUI 0 buff
        check(act::use_skill(w, 2009, autoT, now), 0x00E0, "02 D9 07 03 06 00 04 00 B0 04 20 03");  // ESAUI 6 point
        check(act::use_skill(w, 1106, autoT, now), 0x00E0, nullptr);          // passive: refused
        check(act::use_skill(w, 9999, autoT, now), 0x00E0, nullptr);          // not learned: refused
        check(act::move(w, 1200, 900, true, now), 0x0018, "60 04 20 03 B0 04 84 03 02");
        check(act::move(w, 1200, 900, false, now), 0x0018, "60 04 20 03 B0 04 84 03 01");
        check(act::portal_walk(1200, 900, true), 0x01D4, "B0 04 84 03 02");
        printf("actions: %d/%d PASS\n", pass, total);

        // S2C 0x01D8 [self][20] at FILETIME 0 = walk speed 80: 0.25 s -> 20 units along: (1100, 800)
        WorldModel w2 = w;
        CapPacket sp;
        sp.hdr.dir = DIR_S2C;
        sp.hdr.opcode = 0x01D8;
        sp.data = Pk().u16(ME).u8(20).b;
        sp.hdr.size = sp.hdr.dataLen = (uint32_t)sp.data.size();
        w2.feed(sp);
        w2.self_pos_at(now, px, py);
        printf("after 0x01D8 speed %d: self_pos_at(0.25 s) = (%d,%d) %s\n", w2.self.speed, px, py,
               w2.self.speed == 80 && px == 1100 && py == 800 ? "PASS" : "FAIL");
    }

    // the bot (bot/) on a copy of this world: heal, attack, kill, our drop while a monster hits us,
    // the next target, roaming on the real map, another player's drop after its 30 s; then a gun
    {
        WorldModel wb = w;
        const uint64_t T0 = 100000000ull;            // 10 s
        const uint64_t MS = 10000ull;
        WorldModel* cur = &wb;
        auto at = [&](uint64_t t, uint16_t op, const Pk& p) {
            CapPacket c;
            c.hdr.dir = DIR_S2C;
            c.hdr.opcode = op;
            c.hdr.fileTime = t;
            c.data = p.b;
            c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
            cur->feed(c);
        };
        int pass = 0, total = 0;
        Bot bot;
        Action last;
        // hex = expected body; op 0 + nullptr = nothing sent; hex "*" = any body of that opcode
        auto step = [&](Bot& b, uint64_t t, uint16_t op, const char* hex, const char* what) {
            Action a;
            const bool sent = b.tick(*cur, t, a);
            const std::string got = sent ? hex_preview(a.body.data(), a.body.size(), a.body.size()) : "";
            const bool ok = hex ? (sent && a.op == op && (std::string(hex) == "*" || got == hex)) : !sent;
            ++total;
            pass += ok;
            if (sent) last = a;
            printf("  %s t+%5llums %-30s %s 0x%04X %s   [%s]\n", ok ? "PASS" : "FAIL",
                   (unsigned long long)((t - T0) / MS), what, sent ? "sent" : "none", sent ? a.op : 0, got.c_str(),
                   b.doing.c_str());
        };
        printf("\nbot:\n");
        at(T0, 0x0024, Pk().u32(400));                                  // HP 400 / 1170 = 34 %
        bot.attack.enabled = false;
        bot.roam.enabled = false;
        bot.start(wb, T0);
        step(bot, T0, 0x0057, "0B 00 00 00 00 00 00 00", "HP 34% -> potion");
        step(bot, T0 + 200 * MS, 0, nullptr, "waits for the answer");
        at(T0 + 300 * MS, 0x0024, Pk().u32(1100));                      // the potion's answer
        at(T0 + 300 * MS, 0x000F, Pk().u16(701).u8(0));                 // (the old item #701 goes)
        bot.attack.enabled = true;
        step(bot, T0 + 400 * MS, 0x001D, "5A 02 02", "attack #602");
        step(bot, T0 + 800 * MS, 0, nullptr, "fighting, no resend");
        at(T0 + 900 * MS, 0x016D, Pk().u16(602).u16(ME));
        at(T0 + 1000 * MS, 0x002B, Pk().u16(602).u8(0));
        step(bot, T0 + 1100 * MS, 0, nullptr, "killed, waits for the drops");
        at(T0 + 1200 * MS, 0x000C, Pk().u16(602).u16(800).u16(1190).u16(805).u16(0).u8(0).u8(0).u8(0)
                                       .u8(1).u32(2000).u64(0).u16(1));
        at(T0 + 1300 * MS, 0x0008, Pk().u16(604).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(1230).u16(800)
                                       .u16(1230).u16(800).str("Sea Scorpion").zeros(14));
        at(T0 + 1500 * MS, 0x001F, Pk().u16(604).u16(1).u16(ME).u16(1230).u16(800).u16(1180).u16(800).u16(5).u8(1));
        step(bot, T0 + 1900 * MS, 0x0021, "20 03 02", "our drop #800, #604 hits us");
        at(T0 + 2000 * MS, 0x0022, Pk().u16(ME).u16(800).u8(1));
        step(bot, T0 + 2300 * MS, 0x001D, "5C 02 02", "then the attacker #604");
        at(T0 + 2400 * MS, 0x002B, Pk().u16(604).u8(0));
        at(T0 + 2400 * MS, 0x000F, Pk().u16(604).u8(0));
        at(T0 + 2500 * MS, 0x0008, Pk().u16(603).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(1250).u16(800)
                                       .u16(1250).u16(800).str("Sea Scorpion").zeros(14));
        at(T0 + 2500 * MS, 0x016D, Pk().u16(603).u16(601));             // the player #601 fights it
        at(T0 + 2600 * MS, 0x000C, Pk().u16(603).u16(801).u16(1200).u16(810).u16(0).u8(0).u8(0).u8(0)
                                       .u8(1).u32(2000).u64(0).u16(1));
        step(bot, T0 + 3200 * MS, 0, nullptr, "#603 + #801 are #601's");
        bot.roam.enabled = true;
        bot.roam.mode = RoamAction::WholeMap;
        at(T0 + 3300 * MS, 0x0019, Pk().u16(ME).u16(949).u16(2228).u8(1));   // to Coral Town's open spawn point
        {
            Action a;
            const bool sent = bot.tick(wb, T0 + 3500 * MS, a);
            int sx = 0, sy = 0, dx = 0, dy = 0;
            if (sent && a.body.size() == 9) {
                sx = a.body[0] | a.body[1] << 8; sy = a.body[2] | a.body[3] << 8;
                dx = a.body[4] | a.body[5] << 8; dy = a.body[6] | a.body[7] << 8;
            }
            const MapGrid* map = gd.map(wb.self.zone);
            const bool ok = sent && a.op == 0x0018 && (!map || (!map->blocked(dx, dy) && map->line_clear(sx, sy, dx, dy)));
            ++total;
            pass += ok;
            printf("  %s t+ 3500ms %-30s %s 0x%04X (%d,%d) -> (%d,%d) %s   [%s]\n", ok ? "PASS" : "FAIL",
                   "nothing to do -> roam", sent ? "sent" : "none", sent ? a.op : 0, sx, sy, dx, dy,
                   map ? fmt("map %s %dx%d", gd.map_name(wb.self.zone).c_str(), map->w, map->h).c_str() : "no map",
                   bot.doing.c_str());
        }
        step(bot, T0 + 3900 * MS, 0, nullptr, "walking the leg");
        at(T0 + 32600 * MS, 0x0019, Pk().u16(ME).u16(1180).u16(800).u8(1));  // back next to #801
        step(bot, T0 + 32700 * MS, 0x0021, "21 03 02", "30 s later #801 is free");
        printf("bot: %d/%d PASS\n", pass, total);
        for (const std::string& l : bot.log) printf("  log %s\n", l.c_str());

        // a gun: Gun type 1 (3500: GaugeSpeed 85, PowRange 180, PowStartPoint 150) + Ammo (5500) in the bag
        printf("\nbot with a gun:\n");
        pass = total = 0;
        WorldModel wg = wb;
        cur = &wg;
        const uint64_t G = T0 + 40000 * MS;
        at(G, 0x00A0, Pk().u8(4).u8(1).u32(3500).u64(50).u16(1).u8(0));
        at(G, 0x0052, Pk().u64(50).u8(1));                             // in the weapon slot (the shot timer starts)
        at(G, 0x00A0, Pk().u8(4).u8(1).u32(5500).u64(51).u16(10).u8(0));
        at(G, 0x0008, Pk().u16(605).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(1260).u16(800)
                             .u16(1260).u16(800).str("Sea Scorpion").zeros(14));
        Bot gb;
        gb.loot.enabled = false;
        gb.roam.enabled = false;
        gb.start(wg, G);
        step(gb, G + 100 * MS, 0x0205, "33 00 00 00 00 00 00 00 14", "bullets in the bag -> load");
        at(G + 200 * MS, 0x0206, Pk().u64(51).u8(20));
        step(gb, G + 500 * MS, 0x01FF, "", "no seed yet -> ask");
        at(G + 600 * MS, 0x0200, Pk().u16(0x1234));
        step(gb, G + 900 * MS, 0, nullptr, "987+250 ms after the equip");
        step(gb, G + 1300 * MS, 0x01DD, "5D 02 F4 E4 01 00 02", "shot: crc (0x1234 ^ key) & ~1");
        step(gb, G + 1700 * MS, 0, nullptr, "waits for the new seed");
        at(G + 1800 * MS, 0x0200, Pk().u16(0x0BCE));
        at(G + 1900 * MS, 0x01E5, Pk().u16(ME).u16(2).u16(605).u16(1180).u16(800).u16(1260).u16(800).u16(30).u8(0));
        printf("  %s bullets after the shot: %d (10 - 1)\n", loaded_bullets(wg) == 9 ? "PASS" : "FAIL", loaded_bullets(wg));
        ++total; pass += loaded_bullets(wg) == 9;
        step(gb, G + 2500 * MS, 0, nullptr, "reload (987+250 ms after the shot)");
        step(gb, G + 3200 * MS, 0x01DD, "5D 02 0E FD 01 00 02", "next shot, new seed");
        step(gb, G + 4000 * MS, 0, nullptr, "no seed comes back...");
        step(gb, G + 5800 * MS, 0, nullptr, "...2.5 s: gun off, no more shots");
        printf("  %s gun off = %d\n", gb.attack.gunOff ? "PASS" : "FAIL", gb.attack.gunOff);
        ++total; pass += gb.attack.gunOff;
        printf("gun: %d/%d PASS\n", pass, total);
        for (const std::string& l : gb.log) printf("  log %s\n", l.c_str());

        // skills only: a rotation [Mana Storm 2009 (point, Range 200, CoolTime 5), Shock Wave 1001 (Chase)],
        // basic attack off. Out of reach -> walk within it; cast; the gate waits for S2C 240; the next skill
        // while the first cools; a 239 [12] makes it rest; the first again after its cool time.
        printf("\nbot skills only (rotation):\n");
        pass = total = 0;
        WorldModel wk = w;
        cur = &wk;
        const uint64_t K = T0 + 60000 * MS;
        at(K, 0x0019, Pk().u16(ME).u16(1600).u16(800).u8(1));
        Bot kb;
        kb.loot.enabled = false;
        kb.roam.enabled = false;
        kb.attack.basic = false;
        kb.attack.rotation = { RotationSkill{ 2009, true }, RotationSkill{ 1001, true } };
        kb.start(wk, K);
        step(kb, K + 100 * MS, 0x0018, "40 06 20 03 46 05 20 03 02", "Mana Storm out of reach -> walk");
        at(K + 200 * MS, 0x0019, Pk().u16(ME).u16(1350).u16(800).u8(1));
        step(kb, K + 500 * MS, 0x00E0, "02 D9 07 03 06 00 04 00 B0 04 20 03", "in reach -> Mana Storm");
        step(kb, K + 800 * MS, 0, nullptr, "waits for its answer");
        at(K + 1500 * MS, 0x00F0, Pk().u16(2009).u8(3));
        step(kb, K + 1600 * MS, 0x00E0, "02 E9 03 05 01 00 02 00 5A 02", "fired; cooling -> Shock Wave");
        at(K + 1800 * MS, 0x00EF, Pk().u16(12));
        step(kb, K + 2000 * MS, 0, nullptr, "refused (cool time): both wait");
        step(kb, K + 3000 * MS, 0x00E0, "02 E9 03 05 01 00 02 00 5A 02", "rested 1 s -> Shock Wave again");
        at(K + 3500 * MS, 0x00F0, Pk().u16(1001).u8(5));
        step(kb, K + 6000 * MS, 0, nullptr, "both cooling");
        step(kb, K + 6300 * MS, 0x00E0, "02 D9 07 03 06 00 04 00 B0 04 20 03", "Mana Storm again (priority)");
        printf("rotation: %d/%d PASS\n", pass, total);
        for (const std::string& l : kb.log) printf("  log %s\n", l.c_str());

        // buffs: Pumping Heart 1002 kept up (ESAUI 0 -> 0xEE). S2C 240 + 224 on us = up for its 100 s;
        // 850 = 10 s left; recast 3 s before the end; 280 ends it; a zone change loses the ids, a 280
        // with an unknown id then ends the one whose time is nearest.
        printf("\nbot buffs:\n");
        pass = total = 0;
        WorldModel wf = w;
        cur = &wf;
        wf.self.zone = 2;                                      // (a field: Pumping Heart is refused in a town)
        const uint64_t B = T0 + 80000 * MS;
        Bot fb;
        fb.attack.enabled = false;
        fb.loot.enabled = false;
        fb.roam.enabled = false;
        fb.buffs.enabled = true;
        fb.buffs.keep = { 1002 };
        fb.start(wf, B);
        step(fb, B + 100 * MS, 0x00EE, "EA 03 01", "not up -> Pumping Heart");
        at(B + 1100 * MS, 0x00F0, Pk().u16(1002).u8(1));
        at(B + 1100 * MS, 0x00E0, Pk().u16(ME).u16(ME).u16(900).u16(1125).u8(2).u8(3).u16(1002).u16(0).u8(1));
        step(fb, B + 1200 * MS, 0, nullptr, "up");
        {
            const Effect* e = wf.effect_of(1002, B + 1200 * MS);
            const bool ok = e && e->esId == 900 && e->endAt == B + 1100 * MS + 100000 * MS;
            printf("  %s effect 1002 id %u ends after %.1f s\n", ok ? "PASS" : "FAIL", e ? e->esId : 0,
                   e ? double(e->endAt - e->startAt) / 1e7 : 0.0);
            ++total; pass += ok;
        }
        at(B + 91100 * MS, 0x0352, Pk().u16(1125));
        step(fb, B + 95000 * MS, 0, nullptr, "10 s left, not yet");
        step(fb, B + 98200 * MS, 0x00EE, "EA 03 01", "3 s before the end -> again");
        at(B + 99000 * MS, 0x0118, Pk().u16(ME).u16(900));
        at(B + 99100 * MS, 0x00F0, Pk().u16(1002).u8(1));
        at(B + 99100 * MS, 0x00E0, Pk().u16(ME).u16(ME).u16(901).u16(1125).u8(2).u8(3).u16(1002).u16(0).u8(1));
        step(fb, B + 99300 * MS, 0, nullptr, "up again");
        at(B + 100000 * MS, 0x017C, Pk().u16(6).u16(1000).u16(800));   // (another field)
        at(B + 150000 * MS, 0x0118, Pk().u16(ME).u16(77));
        {
            const bool ok = wf.effects.empty();
            printf("  %s zone change + 280 with a new id -> %zu effects\n", ok ? "PASS" : "FAIL", wf.effects.size());
            ++total; pass += ok;
        }
        step(fb, B + 150100 * MS, 0x00EE, "EA 03 01", "gone -> again");
        printf("buffs: %d/%d PASS\n", pass, total);
        for (const std::string& l : fb.log) printf("  log %s\n", l.c_str());

        // heal skills (act_heal.cpp): Catastrophe Healing 2207 (ESAction 1168, no selection -> 0xEE) with the HP
        // potions off: cast at <= 70 %, its answer awaited, nothing more while it cools, again after; the HP potion
        // (when on) first below its own %; Chakra Balance 1506 (1482, over time) not while a monster swings at us.
        printf("\nbot heal skills:\n");
        pass = total = 0;
        WorldModel wh = w;
        cur = &wh;
        const uint64_t H = T0 + 200000 * MS;
        at(H, 0x010F, Pk().u16(3).u16(1001).u8(5).u16(0).u16(2207).u8(1).u16(0).u16(1506).u8(1).u16(0));
        at(H, 0x0024, Pk().u32(600));                                   // 600 / 1170 = 51 %
        const SkillLevelInfo* hl = gd.skill_level(2207, 1);
        const uint64_t cool = uint64_t((hl ? std::max(0.5f, hl->coolTime - 0.5f) : 1.0f) * 1000);
        printf("  (Catastrophe Healing lv 1: %s, cool time %.1f s, MP %d)\n", HealAction::heal_kind(&gd, 2207),
               hl ? hl->coolTime : 0.0f, hl ? hl->mpCon : 0);
        Bot hb;
        hb.attack.enabled = hb.loot.enabled = hb.roam.enabled = false;
        hb.heal.hpPotions = false;
        hb.heal.skills = { 2207 };
        hb.heal.skillPct = 70;
        hb.start(wh, H);
        ++total; pass += HealAction::heal_skill(&gd, 2207) && HealAction::heal_skill(&gd, 1506) && !HealAction::heal_skill(&gd, 1002);
        printf("  %s heal skills: 2207 %d, 1506 %d, Pumping Heart 1002 %d\n", pass == total ? "PASS" : "FAIL",
               HealAction::heal_skill(&gd, 2207), HealAction::heal_skill(&gd, 1506), HealAction::heal_skill(&gd, 1002));
        step(hb, H + 100 * MS, 0x00EE, "9F 08 01", "HP 51% -> Catastrophe Healing");
        step(hb, H + 500 * MS, 0, nullptr, "waits for its answer");
        at(H + 1000 * MS, 0x00F0, Pk().u16(2207).u8(1));
        at(H + 1000 * MS, 0x0024, Pk().u32(900));                       // 77 %
        step(hb, H + 1100 * MS, 0, nullptr, "healed to 77%: nothing");
        at(H + 1200 * MS, 0x0024, Pk().u32(500));                       // 43 %, no HP potion
        step(hb, H + 1300 * MS, 0, nullptr, "43% while it cools: no potion (off)");
        step(hb, H + 1000 * MS + cool * MS + 200 * MS, 0x00EE, "9F 08 01", "cooled -> again");
        at(H + 1000 * MS + cool * MS + 900 * MS, 0x00F0, Pk().u16(2207).u8(1));
        hb.heal.hpPotions = true;
        hb.heal.hpPct = 50;
        at(H + 1000 * MS + cool * MS + 950 * MS, 0x0024, Pk().u32(400));   // 34 %: the potion first
        step(hb, H + 1000 * MS + cool * MS + 1300 * MS, 0x0057, "0B 00 00 00 00 00 00 00", "34%, potion on -> potion");
        // Chakra Balance: not while swung at, then cast
        Bot cb;
        cb.attack.enabled = cb.loot.enabled = cb.roam.enabled = false;
        cb.heal.hpPotions = false;
        cb.heal.skills = { 1506 };
        const uint64_t C = H + 60000 * MS;
        at(C, 0x0024, Pk().u32(600));
        at(C, 0x0008, Pk().u16(606).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(1200).u16(800).u16(1200).u16(800)
                             .str("Sea Scorpion").zeros(14));
        at(C, 0x001F, Pk().u16(606).u16(1).u16(ME).u16(1200).u16(800).u16(1180).u16(800).u16(5).u8(1));
        cb.start(wh, C);
        step(cb, C + 100 * MS, 0, nullptr, "Chakra Balance: swung at -> no");
        at(C + 200 * MS, 0x000F, Pk().u16(606).u8(0));
        step(cb, C + 4500 * MS, 0x00EE, "E2 05 01", "4 s without a swing -> Chakra Balance");
        printf("heal skills: %d/%d PASS\n", pass, total);
        for (const std::string& l : hb.log) printf("  log %s\n", l.c_str());

        // black / whitelists + the Third Eye: an item outside the whitelist stays; whitelisted it is taken.
        // A blacklisted species is not hunted; a shadow monster (16001) only with the Third Eye (ESAction
        // 1390 on us), dropped when it ends; a blacklisted monster that hits us is fought back.
        printf("\nbot lists + Third Eye:\n");
        pass = total = 0;
        WorldModel wq = w;
        cur = &wq;
        const uint64_t F = T0 + 300000 * MS;
        Bot qb;
        qb.roam.enabled = false;
        qb.attack.enabled = false;
        qb.loot.items.mode = IdFilter::OnlyWhite;
        qb.start(wq, F);
        step(qb, F + 100 * MS, 0, nullptr, "item 102 not in the whitelist");
        qb.loot.items.white.insert(102);
        step(qb, F + 500 * MS, 0x0021, "BD 02 02", "whitelisted -> pick up #701");
        Bot ab;
        ab.loot.enabled = false;
        ab.roam.enabled = false;
        ab.attack.mobs.black = { 2000 };
        ab.start(wq, F + 1000 * MS);
        step(ab, F + 1100 * MS, 0, nullptr, "species 2000 blacklisted");
        at(F + 1200 * MS, 0x0008, Pk().u16(603).u16(3).u16(16001).u8(0).u8(0).u16(60).u32(0).u16(1250).u16(800)
                                      .u16(1250).u16(800).str("Shadow").zeros(14));
        step(ab, F + 1500 * MS, 0, nullptr, "shadow #603, no Third Eye");
        at(F + 1600 * MS, 0x00E0, Pk().u16(ME).u16(ME).u16(910).u16(1390).u8(2).u8(7).u16(8001).u16(0).u8(1));
        step(ab, F + 1900 * MS, 0x001D, "5B 02 02", "Third Eye up -> shadow #603");
        at(F + 2000 * MS, 0x0118, Pk().u16(ME).u16(910));
        step(ab, F + 2300 * MS, 0, nullptr, "Third Eye over -> #603 dropped");
        at(F + 2400 * MS, 0x001F, Pk().u16(602).u16(1).u16(ME).u16(1200).u16(800).u16(1180).u16(800).u16(5).u8(1));
        step(ab, F + 2700 * MS, 0x001D, "5A 02 02", "blacklisted #602 hits us -> fight back");
        printf("lists: %d/%d PASS\n", pass, total);
        for (const std::string& l : ab.log) printf("  log %s\n", l.c_str());
    }

    // the max HP / MP is worked out, not "the highest HP seen": Leao (ProjectPAULA db, char 15) logs in with
    // 857 of 1890 HP — 236 MaxHP points -> gulim13 row 60 = 1890, 59 MaxMP points -> row 15 = 500; a gun with
    // options 10001 (AC +2) / 10012 (gun AP) and no MaxHP bonus. With the heal at 50 % the potion goes at once.
    {
        printf("\nmax HP:\n");
        WorldModel wl(&gd);
        const uint64_t T = 100000000ull, MS = 10000ull;
        auto at = [&](uint64_t t, uint16_t op, const Pk& p) {
            CapPacket c;
            c.hdr.dir = DIR_S2C;
            c.hdr.opcode = op;
            c.hdr.fileTime = t;
            c.data = p.b;
            c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
            wl.feed(c);
        };
        int pass = 0, total = 0;
        auto check = [&](bool ok, const std::string& what) { ++total; pass += ok; printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str()); };
        at(T, 0x00E6, Pk().u32(0).u16(ME).u16(1000).u16(800));
        at(T, 0x00E7, Pk().u16(2)
                          .u8(2).u32(3506).u64(22).u16(1).zeros(4).u8(2).u32(10).u32(10001 | 2u << 16).u32(0)
                                                                 .u32(10).u32(10012 | 128u << 16).u32(0)
                          .u8(1).u32(2000).u64(11).u16(10));                     // Pink Potion A x10
        { Pk p; p.zeros(278);
          auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
          auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
          p.b[8] = 22;                                              // slot 1 = the gun (uid 22)
          put16(184 + 2 * 3, 59); put16(184 + 2 * 9, 236);           // MaxMP / MaxHP points
          put16(184 + 30, 60);                                      // level 60
          put16(219, 19); put32(217 + 18, 857); put32(217 + 22, 500);
          at(T, 0x00E9, p); }
        check(wl.self.hpMax == 1890 && wl.self.mpMax == 500,
              fmt("login 857 HP: max HP %lld (calc %lld), max MP %lld", (long long)wl.self.hpMax, (long long)wl.self.hpMaxCalc,
                  (long long)wl.self.mpMax));
        Bot hb;
        hb.attack.enabled = hb.roam.enabled = hb.loot.enabled = false;
        hb.town.enabled = false;
        hb.start(wl, T);
        Action a;
        const bool sent = hb.tick(wl, T + 400 * MS, a);
        check(sent && a.op == 0x0057, fmt("HP 857 / 1890 = 45%% < 50%%: a potion at once (%s)", sent ? a.label.c_str() : hb.doing.c_str()));
        at(T + 600 * MS, 0x0024, Pk().u32(1500));
        const bool again = hb.tick(wl, T + 1000 * MS, a);
        check(!again, "HP 1500 / 1890 = 79%: no more");
        at(T + 1100 * MS, 0x006D, Pk().u16(4).u16(59).u16(0).u16(0).u16(59).u16(0).u16(0).u16(0).u16(0).u16(0).u16(276).u16(0).u16(0));
        check(wl.self.hpMaxCalc == wl.data()->stat_value(276 / 4 + 1, ST_MAXHP),
              fmt("S2C 0x6D points (276 MaxHP) -> max HP %lld", (long long)wl.self.hpMax));
        printf("max HP: %d/%d PASS\n", pass, total);
    }

    // the same sequence back through the capture reader must give the same model
    cap.close();
    Session s(1, &gd);
    if (!s.load_capture(capDir + "selftest_world.txt", err)) { printf("reload: %s\n", err.c_str()); return; }
    const WorldModel& r = *s.world;
    printf("reloaded capture: %llu packets, applied %llu, HP %lld EXP %lld kills %d, %zu entities, %zu ground, %zu bag "
           "-> %s\n", (unsigned long long)s.total_packets(), (unsigned long long)r.applied, (long long)r.self.hp,
           (long long)r.self.exp, r.self.kills, r.entities.size(), r.groundItems.size(), r.inventory.size(),
           r.applied == w.applied && r.self.exp == w.self.exp && r.inventory.size() == w.inventory.size() ? "MATCH"
                                                                                                         : "MISMATCH");
}

// Dump the model + a per-opcode report, to see exactly what a live session delivered.
static void print_world(Session& s)
{
    const WorldModel& w = *s.world;
    const SelfState& me = w.self;
    printf("\n== model ==\n");
    printf("self: '%s' #%u uid %llx species %u hair %u ftype %u | Lv %d TM %d (pts %d) | HP %lld/%lld MP %lld/%lld | "
           "EXP %lld TM %lld | galder %lld | zone %d home %d pos (%d,%d) moving=%d dead=%d | kills %d deaths %d\n",
           me.name.c_str(), me.eid, (unsigned long long)me.charUid, me.species, me.hair, me.fType, me.level, me.tmLevel,
           me.tmPoints, (long long)me.hp, (long long)me.hpMax, (long long)me.mp, (long long)me.mpMax, (long long)me.exp,
           (long long)me.tmExp, (long long)me.money, me.zone, me.homeZone, me.x, me.y, me.moving, me.dead, me.kills,
           me.deaths);
    printf("entities %zu, ground %zu, bag %zu, skills %zu | applied %llu malformed %llu (last %s)\n",
           w.entities.size(), w.groundItems.size(), w.inventory.size(), w.skills.size(), (unsigned long long)w.applied,
           (unsigned long long)w.malformed, w.lastMalformed.empty() ? "-" : w.lastMalformed.c_str());

    // the login-burst opcodes we rely on, and what actually arrived
    struct Key { uint16_t op; const char* what; };
    const Key keys[] = { { 0x02A7, "char blob (expect 69B)" }, { 0x00E6, "batch header (10B)" },
                         { 0x00E7, "item list" }, { 0x00E9, "stats block (expect 278B)" },
                         { 0x010F, "skill list" }, { 0x0007, "appear-login" }, { 0x0008, "appear" },
                         { 0x03A5, "self eid" }, { 0x0018, "move" } };
    printf("-- login-burst opcodes seen (S2C) --\n");
    for (const Key& k : keys) {
        auto it = s.opStats.find(uint32_t(DIR_S2C) << 16 | k.op);
        if (it == s.opStats.end())
            printf("   0x%04X %-26s : NOT SEEN\n", k.op, k.what);
        else
            printf("   0x%04X %-26s : %llu packets, avg %llu B\n", k.op, k.what, (unsigned long long)it->second.count,
                   (unsigned long long)(it->second.bytes / (it->second.count ? it->second.count : 1)));
    }
    printf("-- all opcodes seen --\n");
    for (const auto& kv : s.opStats) {
        uint8_t dir = kv.first >> 16;
        uint16_t op = (uint16_t)kv.first;
        printf("   %s 0x%04X  x%llu  %llu B total\n", dir == DIR_C2S ? "C>S" : "S>C", op,
               (unsigned long long)kv.second.count, (unsigned long long)kv.second.bytes);
    }
}

// ── town: the atlas on the real data, then the Town action on a synthetic world: a supply trip (the
//    portal to the town, the shop: open, prices, buy, close, the portal back), then a death (Go to
//    Town, the rest in town, the way back). The server's side is played from ProjectPAULA's rules. ──
static void town_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    const uint64_t L0 = GetTickCount64();
    const Atlas& at = gd.atlas();
    printf("%s  (%llu ms)\n", at.load_report().c_str(), (unsigned long long)(GetTickCount64() - L0));
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };

    // routes over the real maps
    printf("\natlas:\n");
    RouteOptions ro;
    ro.level = 50;
    ro.money = 100000;
    auto show = [&](int a, int b, bool tp) {
        RouteOptions o = ro;
        o.teleports = tp;
        const uint64_t t0 = GetTickCount64();
        const Route r = at.route(gd, a, 500, 400, b, o);
        std::string via;
        for (const RouteHop& h : r.hops)
            via += fmt(" > %s%s", h.kind == RouteHop::Teleport ? "[tp] " : "", at.map_label(h.toZone).c_str());
        printf("    %s -> %s%s: %s %zu hops %.0f s %dg%s (%llu ms)\n", at.map_label(a).c_str(), at.map_label(b).c_str(),
               tp ? "" : " (no teleports)", r.ok ? "ok" : r.why.c_str(), r.hops.size(), r.secs, r.gelder, via.c_str(),
               (unsigned long long)(GetTickCount64() - t0));
        return r;
    };
    check(show(9, 3, true).ok, "Beach Field 1 -> Gate of Desert Beach");
    check(show(3, 9, true).ok, "Gate of Desert Beach -> Beach Field 1");
    const Route longWay = show(9, 148, true);
    check(longWay.ok, "Beach Field 1 -> Coral Town");
    show(9, 148, false);
    check(show(1, 7, true).ok, "Megalopolis -> Tapasco Volcano gate");
    // a walk inside map 3 into a portal's rectangle, every leg a straight open line, the last one inside it
    if (const MapGrid* g = gd.map(3)) {
        const AtlasMap* m3 = at.map(3);
        const AtlasPoint* a = nullptr;
        for (const AtlasPoint& p : m3->points) if (p.type == 2) { a = &p; break; }
        if (a && !m3->portals.empty()) {
            const AtlasPortal& p0 = m3->portals[0];
            std::vector<std::pair<int, int>> legs;
            std::string why;
            const uint64_t t0 = GetTickCount64();
            const bool ok = Atlas::path_rect(*g, a->x, a->y, p0, 600, legs, &why);
            bool clear = ok;
            int cx = a->x, cy = a->y;
            for (const auto& l : legs) { clear = clear && g->line_clear(cx, cy, l.first, l.second); cx = l.first; cy = l.second; }
            const bool in = cx >= p0.x1 - 16 && cx < p0.x2 + 16 && cy >= p0.y1 - 16 && cy < p0.y2 + 16;
            printf("    path in map 3 (%d,%d) -> portal [%d,%d..%d,%d]: %s, %zu legs, end (%d,%d) (%llu ms)\n", a->x, a->y,
                   p0.x1, p0.y1, p0.x2, p0.y2, ok ? "ok" : why.c_str(), legs.size(), cx, cy,
                   (unsigned long long)(GetTickCount64() - t0));
            check(ok && clear && in, "map 3 path into the portal, legs open, the last one inside");
        }
    }
    // the planner tries every way out: the route is the quickest of them (Atlas::alternatives)
    {
        const int pairs[][2] = { { 9, 3 }, { 3, 9 }, { 9, 148 }, { 148, 9 }, { 1, 7 }, { 33, 148 }, { 1, 148 }, { 7, 1 } };
        int same = 0;
        uint64_t worst = 0;
        for (const auto& pr : pairs) {
            const AtlasMap* m = at.map(pr[0]);
            int x = 500, y = 400;
            for (const AtlasPoint& q : m->points) if (q.type == 2) { x = q.x; y = q.y; break; }
            const uint64_t t0 = GetTickCount64();
            const Route best = at.route(gd, pr[0], x, y, pr[1], ro);
            const std::vector<Route> alts = at.alternatives(gd, pr[0], x, y, pr[1], ro, 50);
            worst = std::max<uint64_t>(worst, GetTickCount64() - t0);
            const bool ok = best.ok && !alts.empty() && std::fabs(best.secs - alts.front().secs) < 0.01;
            same += ok;
            std::string list;
            for (size_t i = 0; i < alts.size() && i < 4; ++i)
                list += fmt("%s%.0f s via %s", i ? ", " : "", alts[i].secs, at.map_label(alts[i].hops.front().toZone).c_str());
            printf("    %s -> %s: %.0f s, ways out: %s\n", at.map_label(pr[0]).c_str(), at.map_label(pr[1]).c_str(), best.secs,
                   list.c_str());
        }
        check(same == int(std::size(pairs)), fmt("the route = the quickest way out, %d/%zu pairs (slowest %llu ms)", same,
                                                std::size(pairs), (unsigned long long)worst));
    }
    // how often the straight line picks another nearest exit than the walk on the grid (the old estimate)
    {
        int maps = 0, differ = 0;
        std::string example;
        for (const auto& kv : at.maps()) {
            const AtlasMap& m = kv.second;
            const MapGrid* g = gd.map(m.id);
            if (!g || m.portals.size() < 2) continue;
            const AtlasPoint* a = nullptr;
            for (const AtlasPoint& p : m.points) if (p.type == 2 || p.type == 1) { a = &p; break; }
            if (!a) continue;
            ++maps;
            int byLine = -1, byWalk = -1;
            double bestLine = 1e18, bestWalk = 1e18;
            for (size_t i = 0; i < m.portals.size(); ++i) {
                const AtlasPortal& p = m.portals[i];
                std::vector<std::pair<int, int>> legs;
                if (!Atlas::path_rect(*g, a->x, a->y, p, 100000, legs)) continue;
                double walk = 0;
                int cx = a->x, cy = a->y;
                for (const auto& l : legs) { walk += std::hypot(double(l.first - cx), double(l.second - cy)); cx = l.first; cy = l.second; }
                const double line = std::hypot(double(p.cx() - a->x), double(p.cy() - a->y));
                if (line < bestLine) { bestLine = line; byLine = int(i); }
                if (walk < bestWalk) { bestWalk = walk; byWalk = int(i); }
            }
            if (byLine >= 0 && byWalk >= 0 && byLine != byWalk) {
                ++differ;
                if (example.empty())
                    example = fmt("%s from (%d,%d): the straight line says the portal to %s, the walk says the one to %s",
                                  at.map_label(m.id).c_str(), a->x, a->y, at.map_label(m.portals[size_t(byLine)].toZone).c_str(),
                                  at.map_label(m.portals[size_t(byWalk)].toZone).c_str());
            }
            if (maps >= 150) break;
        }
        printf("    nearest portal by the straight line != by the walk: %d of %d maps%s%s\n", differ, maps,
               example.empty() ? "" : "\n      e.g. ", example.c_str());
    }

    // ── the bot: hunting in map 9, a potion running out, the shop of map 3 ──
    const int HUNT = 9, TOWN = 3;
    const AtlasMap* mh = at.map(HUNT);
    const AtlasNpc* shop = nullptr;
    uint32_t potion = 0;
    for (int i : at.shops()) {
        const AtlasNpc* n = at.npc(i);
        if (n->zone != TOWN) continue;
        for (uint32_t id : n->items) {
            const ItemInfo* ii = gd.item(id);
            if (ii && ii->type == 10 && ii->hp > 0) { shop = n; potion = id; break; }
        }
        if (shop) break;
    }
    if (!mh || !shop) { printf("no shop with a potion in map %d: %d/%d\n", TOWN, pass, total); return; }
    int sx = 500, sy = 400;
    for (const AtlasPoint& p : mh->points) if (p.type == 1 && p.param == uint32_t(TOWN)) { sx = p.x; sy = p.y; break; }
    if (const MapGrid* g = gd.map(HUNT)) if (g->blocked(sx, sy)) g->nearest_open(sx, sy, 8, sx, sy);
    printf("\ntown trip: hunt map %s from (%d,%d), shop %s (%s) at (%d,%d), potion %s\n", at.map_label(HUNT).c_str(), sx, sy,
           shop->name.c_str(), shop->key().c_str(), shop->x, shop->y, gd.item_name(potion).c_str());

    WorldModel w(&gd);
    const uint16_t ME = 500, SHOP = 900;
    const uint64_t MS = 10000ull;
    uint64_t now = 100000000ull;
    auto feed = [&](uint8_t dir, uint16_t op, const Pk& p) {
        CapPacket c;
        c.hdr.dir = dir;
        c.hdr.opcode = op;
        c.hdr.fileTime = now;
        c.data = p.b;
        c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
        w.feed(c);
    };
    auto enter_zone = [&](int zone, int x, int y) {         // 153 / 154 / 437 / 157, as a portal does it
        feed(DIR_S2C, 0x0099, Pk().u16(uint16_t(zone)).u16(0).u16(0));
        feed(DIR_S2C, 0x009A, Pk().u32(0).u64(0x1122).str("127.0.0.1").u32(8002).u32(0).u16(uint16_t(zone))
                                  .u16(uint16_t(x)).u16(uint16_t(y)).u8(2));
        now += 1500 * MS;
        feed(DIR_S2C, 0x01B5, Pk().u16(ME).u16(uint16_t(x)).u16(uint16_t(y)).u32(0).u8(0));
        feed(DIR_S2C, 0x009D, Pk().u32(0));
        if (zone == TOWN)                                     // the shop NPC (kind 5) appears
            feed(DIR_S2C, 0x0008, Pk().u16(SHOP).u16(5).u16(shop->species).u8(0).u8(0).u16(1).u32(0)
                                      .u16(uint16_t(shop->x)).u16(uint16_t(shop->y)).u16(uint16_t(shop->x))
                                      .u16(uint16_t(shop->y)).str("Item Girl").zeros(12));
    };
    { Pk p; p.u64(0x1122).zeros(9); const char nm[20] = "Leao"; for (char ch : nm) p.u8((uint8_t)ch);
      p.u16(9).u8(3).u8(1).zeros(69 - 41); feed(DIR_S2C, 0x02A7, p); }
    feed(DIR_S2C, 0x00E6, Pk().u32(0).u16(ME).u16(uint16_t(sx)).u16(uint16_t(sy)));
    feed(DIR_S2C, 0x00E7, Pk().u16(1).u8(1).u32(potion).u64(77).u16(2));
    { Pk p; p.zeros(278);
      auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
      auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
      put16(184 + 30, 40);                                   // level 40
      put16(219, uint16_t(HUNT)); put32(217 + 14, 50000); put32(217 + 18, 1000); put32(217 + 22, 500);
      feed(DIR_S2C, 0x00E9, p); }

    Bot bot;
    bot.attack.enabled = bot.loot.enabled = bot.roam.enabled = false;
    bot.start(w, now);
    bot.town.supply = true;
    bot.town.shopKey = shop->key();
    bot.town.buy.push_back(BuyRow{ potion, 5, 20, true });
    std::map<uint16_t, int> sent;
    // tick every 100 ms; our own C2S go back into the model as the hook would capture them
    auto run_until = [&](uint16_t want, double maxSecs, Action* got) {
        const uint64_t end = now + uint64_t(maxSecs * 1000) * MS;
        while (now < end) {
            now += 100 * MS;
            Action a;
            if (!bot.tick(w, now, a)) continue;
            ++sent[a.op];
            feed(DIR_C2S, a.op, Pk{ a.body });
            if (a.op != 0x0018) printf("    t=%6.1f s  0x%04X %-40s [%s]\n", double(now - 100000000ull) / 1e7, a.op, a.label.c_str(),
                                       bot.doing.c_str());
            if (a.op == want) { if (got) *got = a; return true; }
        }
        printf("    (no 0x%04X in %.0f s: %s)\n", want, maxSecs, bot.doing.c_str());
        return false;
    };
    auto take_portal = [&]() {                               // the server: the armed walk ends in the rectangle
        const RouteHop& h = bot.town.nav.route().hops.front();
        const AtlasPortal& p = at.map(h.fromZone)->portals[size_t(h.portal)];
        int ax = 500, ay = 400;
        bool found = false;
        for (const AtlasPoint& q : at.map(h.toZone)->points)
            if (q.type == 1 && q.param == uint32_t(h.fromZone) && q.id == (p.id & 0xFF)) { ax = q.x; ay = q.y; found = true; break; }
        if (!found) for (const AtlasPoint& q : at.map(h.toZone)->points) if (q.type == 1) { ax = q.x; ay = q.y; break; }
        now += 800 * MS;
        enter_zone(h.toZone, ax, ay);
        return h.toZone;
    };

    Action a;
    check(run_until(0x01D4, 120, &a), "supply trip: walks to the portal of map 9 and enters it (0x1D4)");
    check(take_portal() == TOWN && w.self.zone == TOWN, "in the town map");
    check(run_until(0x0046, 120, &a) && a.body.size() == 2 && (a.body[0] | a.body[1] << 8) == SHOP, "walks to the shop, opens it (0x46)");
    feed(DIR_S2C, 0x00A8, Pk().u16(SHOP).str("Hello"));
    check(run_until(0x00A4, 5, &a), "asks the prices (0xA4)");
    feed(DIR_S2C, 0x0047, Pk().u16(1).u32(potion).u32(30));
    const bool bought = run_until(0x0048, 5, &a);
    const std::string body = bought ? hex_preview(a.body.data(), a.body.size(), a.body.size()) : "";
    Pk want; want.u16(SHOP).u16(1).u32(potion).u16(18);
    check(bought && a.body == want.b, "buys 18 (2 -> 20): " + body);
    feed(DIR_S2C, 0x0049, Pk().u32(50000 - 18 * 30).u16(1).u8(1).u8(1).u32(potion).u64(77).u16(20));
    check(w.count_item(potion) == 20, "the bag has 20 now");
    check(run_until(0x004F, 5, &a), "closes the shop (0x4F)");
    check(run_until(0x01D4, 120, &a), "back: walks to the portal of the town (0x1D4)");
    check(take_portal() == HUNT, "back in the hunting map");
    for (int i = 0; i < 400 && bot.town.trip(); ++i) { now += 100 * MS; Action b; if (bot.tick(w, now, b)) feed(DIR_C2S, b.op, Pk{ b.body }); }
    check(!bot.town.trip() && bot.anchorZone == HUNT, "the trip is over, hunting map again: " + bot.doing);

    // ── a death: Go to Town, rest, back ──
    printf("\ndeath:\n");
    feed(DIR_S2C, 0x002B, Pk().u16(ME).u8(0));
    const uint64_t died = now;
    check(run_until(0x0016, 10, &a) && now - died >= 5000 * MS, "Go to Town (0x16) after the 5 s delay");
    feed(DIR_S2C, 0x0017, Pk().u16(TOWN).u16(0).u16(0).u32(300));
    {
        int tx = 500, ty = 400;
        for (const AtlasPoint& q : at.map(TOWN)->points) if (q.type == 2) { tx = q.x; ty = q.y; break; }
        enter_zone(TOWN, tx, ty);
    }
    check(!w.self.dead && w.self.zone == TOWN, "alive in the town");
    check(run_until(0x0040, 5, &a) && a.body.size() == 1 && a.body[0] == 13, "rests (0x40 13)");
    feed(DIR_S2C, 0x0040, Pk().u16(ME).u8(13));
    now += 3000 * MS;
    feed(DIR_S2C, 0x0024, Pk().u32(950));
    feed(DIR_S2C, 0x0025, Pk().u32(500));
    check(run_until(0x0040, 5, &a) && a.body.size() == 1 && a.body[0] == 0, "HP 95%: stands up (0x40 0)");
    feed(DIR_S2C, 0x0040, Pk().u16(ME).u8(0));
    check(run_until(0x01D4, 120, &a), "nothing to buy (20 >= 20): back through the portal (0x1D4)");
    check(take_portal() == HUNT, "back where it died");
    for (int i = 0; i < 400 && bot.town.trip(); ++i) { now += 100 * MS; Action b; if (bot.tick(w, now, b)) feed(DIR_C2S, b.op, Pk{ b.body }); }
    check(!bot.town.trip() && bot.running, "the trip is over, the bot runs: " + bot.doing);
    // a live log: a max HP buff before the death (1500 seen), gone after it: the rest stops when HP stops rising at 1000
    printf("\ndeath with a buffed max HP seen before it:\n");
    feed(DIR_S2C, 0x0024, Pk().u32(1500));
    feed(DIR_S2C, 0x002B, Pk().u16(ME).u8(0));
    check(run_until(0x0016, 10, &a), "Go to Town (0x16)");
    feed(DIR_S2C, 0x0017, Pk().u16(TOWN).u16(0).u16(0).u32(300));
    {
        int tx = 500, ty = 400;
        for (const AtlasPoint& q : at.map(TOWN)->points) if (q.type == 2) { tx = q.x; ty = q.y; break; }
        enter_zone(TOWN, tx, ty);
    }
    check(run_until(0x0040, 5, &a) && a.body.size() == 1 && a.body[0] == 13, "rests (0x40 13)");
    feed(DIR_S2C, 0x0040, Pk().u16(ME).u8(13));
    now += 3000 * MS;
    feed(DIR_S2C, 0x0024, Pk().u32(1000));                  // full: 1000 / 1500 seen = 66 %
    feed(DIR_S2C, 0x0025, Pk().u32(500));
    printf("    HP %lld / %lld known\n", (long long)w.self.hp, (long long)w.self.hpMax);
    check(run_until(0x0040, 25, &a) && a.body.size() == 1 && a.body[0] == 0, "HP still 15 s at 66 % of the max seen: stands up (0x40 0)");
    feed(DIR_S2C, 0x0040, Pk().u16(ME).u8(0));
    check(run_until(0x01D4, 120, &a), "and goes back (0x1D4)");
    check(take_portal() == HUNT, "back where it died");
    for (int i = 0; i < 400 && bot.town.trip(); ++i) { now += 100 * MS; Action b; if (bot.tick(w, now, b)) feed(DIR_C2S, b.op, Pk{ b.body }); }
    check(!bot.town.trip() && bot.running, "the trip is over, the bot runs again: " + bot.doing);
    printf("\nsent:");
    for (const auto& kv : sent) printf(" 0x%04X x%d", kv.first, kv.second);
    printf("\nbot log:\n");
    for (const std::string& l : bot.log) printf("    %s\n", l.c_str());

    // ── the bag is full: the warehouse (store B), then the shop (sell A), in a map that has both ──
    const AtlasNpc* keeper = nullptr;
    const AtlasNpc* shop2 = nullptr;
    for (int wi : at.warehouses()) {
        const AtlasNpc* k = at.npc(wi);
        for (int si : at.shops())
            if (at.npc(si)->zone == k->zone) { keeper = k; shop2 = at.npc(si); break; }
        if (keeper) break;
    }
    if (keeper) {
        const int Z = keeper->zone;
        printf("\nstore + sell in %s: keeper %s (%d,%d) args %zu, shop %s (%d,%d)\n", at.map_label(Z).c_str(),
               keeper->name.c_str(), keeper->x, keeper->y, keeper->warehouseArgs.size(), shop2->name.c_str(), shop2->x, shop2->y);
        const uint16_t KEEPER = 910, SHOP2 = 911;
        const uint32_t A = 101, B = 102;                     // A sold, B stored
        WorldModel w2(&gd);
        WorldModel* keep = &w;
        (void)keep;
        // the same harness on a second world
        auto feed2 = [&](uint8_t dir, uint16_t op, const Pk& p) {
            CapPacket c;
            c.hdr.dir = dir;
            c.hdr.opcode = op;
            c.hdr.fileTime = now;
            c.data = p.b;
            c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
            w2.feed(c);
        };
        int x0 = 500, y0 = 400;
        for (const AtlasPoint& q : at.map(Z)->points) if (q.type == 2) { x0 = q.x; y0 = q.y; break; }
        { Pk p; p.u64(0x2233).zeros(9); const char nm[20] = "Leao"; for (char ch : nm) p.u8((uint8_t)ch);
          p.u16(9).u8(3).u8(1).zeros(69 - 41); feed2(DIR_S2C, 0x02A7, p); }
        feed2(DIR_S2C, 0x00E6, Pk().u32(0).u16(ME).u16(uint16_t(x0)).u16(uint16_t(y0)));
        feed2(DIR_S2C, 0x00E7, Pk().u16(2).u8(1).u32(A).u64(201).u16(5).u8(1).u32(B).u64(202).u16(3));
        { Pk p; p.zeros(278);
          auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
          auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
          put16(184 + 30, 40); put16(219, uint16_t(Z)); put32(217 + 14, 50000); put32(217 + 18, 1000); put32(217 + 22, 500);
          feed2(DIR_S2C, 0x00E9, p); }
        feed2(DIR_S2C, 0x0008, Pk().u16(KEEPER).u16(2).u16(keeper->species).u8(0).u8(0).u16(1).u32(0)
                                   .u16(uint16_t(keeper->x)).u16(uint16_t(keeper->y)).u16(uint16_t(keeper->x))
                                   .u16(uint16_t(keeper->y)).str("Keeper").zeros(6));
        feed2(DIR_S2C, 0x0008, Pk().u16(SHOP2).u16(5).u16(shop2->species).u8(0).u8(0).u16(1).u32(0)
                                   .u16(uint16_t(shop2->x)).u16(uint16_t(shop2->y)).u16(uint16_t(shop2->x))
                                   .u16(uint16_t(shop2->y)).str("Shop").zeros(12));
        Bot b2;
        b2.attack.enabled = b2.loot.enabled = b2.roam.enabled = false;
        b2.start(w2, now);
        TownAction& t2 = b2.town;
        t2.supply = true;
        t2.shopKey = shop2->key();
        t2.sell = true;                                     // every item but the kept ones: A is kept from the warehouse,
        t2.store = true;                                    //  so B is stored, A sold
        t2.whKey = keeper->key();
        t2.storeList.black.insert(A);
        t2.bagFull = 2;
        auto run2 = [&](uint16_t want, double maxSecs, Action* got) {
            const uint64_t end = now + uint64_t(maxSecs * 1000) * MS;
            while (now < end) {
                now += 100 * MS;
                Action a2;
                if (!b2.tick(w2, now, a2)) continue;
                feed2(DIR_C2S, a2.op, Pk{ a2.body });
                if (a2.op != 0x0018)
                    printf("    0x%04X %-36s %s\n", a2.op, a2.label.c_str(), hex_preview(a2.body.data(), a2.body.size(), 40).c_str());
                if (a2.op == want) { if (got) *got = a2; return true; }
            }
            printf("    (no 0x%04X in %.0f s: %s)\n", want, maxSecs, b2.doing.c_str());
            return false;
        };
        check(run2(0x006F, 120, &a) && a.body == Pk().u16(KEEPER).b, "the bag is full: walks to the keeper, talks (0x6F)");
        feed2(DIR_S2C, 0x0070, Pk().u16(KEEPER).u16(keeper->species).u8(1).str("Hi\\Deposit\\Warehouse_Start,1,2,3").u16(0).u16(0));
        check(run2(0x007F, 5, &a) && a.body == Pk().u16(KEEPER).u8(16).u8(3).u16(1).u16(2).u16(3).b,
              "Warehouse_Start,1,2,3 (0x7F func 16)");
        feed2(DIR_S2C, 0x0075, Pk());
        feed2(DIR_S2C, 0x0029, Pk().u32(49950));
        feed2(DIR_S2C, 0x0089, Pk().u8(2).u8(0).u16(0).u16(0));
        check(run2(0x008B, 5, &a) && a.body == Pk().u8(2).u64(202).u32(B).u16(3).b, "stores B x3 in tab 2 (0x8B)");
        feed2(DIR_S2C, 0x008C, Pk().u8(2).u8(7).u8(1).u32(B).u64(202).u16(3).u8(8).u8(1).u32(B).u64(0).u16(3));
        check(!w2.find_item(202), "B left the bag (record type 7)");
        check(run2(0x008A, 5, &a), "closes the warehouse (0x8A)");
        check(run2(0x0046, 120, &a) && a.body == Pk().u16(SHOP2).b, "walks to the shop, opens it (0x46)");
        feed2(DIR_S2C, 0x00A8, Pk().u16(SHOP2).str("Hello"));
        check(run2(0x039F, 5, &a) && a.body == Pk().u16(0x004C).u8(0).u16(SHOP2).u16(1).u64(201).u32(A).u16(5).b,
              "sells A x5 (0x39F inner 76)");
        feed2(DIR_S2C, 0x004E, Pk().u32(50100).u16(1).u8(3).u8(1).u32(A).u64(201).u16(5));
        check(w2.inventory.empty() && w2.self.money == 50100, "the bag is empty, the galder in");
        check(run2(0x004F, 5, &a), "nothing to buy: closes the shop (0x4F)");
        for (int i = 0; i < 600 && t2.trip(); ++i) { now += 100 * MS; Action b; if (b2.tick(w2, now, b)) feed2(DIR_C2S, b.op, Pk{ b.body }); }
        check(!t2.trip(), "back at the start point: " + b2.doing);
        printf("bot log:\n");
        for (const std::string& l : b2.log) printf("    %s\n", l.c_str());
        // an old profile (only the whitelisted items sold): every item but the kept ones now, Auto-Sell off until checked
        Bot b3;
        b3.load_text("town.sell=1\ntown.sellList.listMode=1\ntown.sellList.white=101\ntown.store=1\ntown.storeList.black=55\n");
        check(!b3.town.sell && b3.town.sellList.mode == IdFilter::AllButBlack && b3.town.sellList.white.empty() &&
              !b3.town.sellMigrated.empty(), "an old whitelist profile: the list is \"all but the kept\", Auto-Sell turned off, a notice");
        check(b3.town.store && b3.town.storeList.black.count(55) && b3.town.storeMigrated.empty(),
              "its \"all but\" storage list kept as it was (55 kept), Auto-Storage still on");
    } else {
        printf("\n(no map with a warehouse keeper and a shop)\n");
    }

    // ── the Travel tab: Beach Town -> Coral Town through an NPC teleport, the bot stops there ──
    {
        const int FROM = 33, TO = 148;
        RouteOptions o = ro;
        const Route r = at.route(gd, FROM, 500, 400, TO, o);
        const RouteHop* tp = nullptr;
        for (const RouteHop& h : r.hops) if (h.kind == RouteHop::Teleport && h.fromZone == FROM) { tp = &h; break; }
        if (tp) {
            const AtlasNpc* n = at.npc(tp->npc);
            printf("\ntravel %s -> %s: %s at (%d,%d) slot %d, %d galder\n", at.map_label(FROM).c_str(), at.map_label(TO).c_str(),
                   n->name.c_str(), n->x, n->y, tp->slot, tp->cost);
            WorldModel w3(&gd);
            auto feed3 = [&](uint8_t dir, uint16_t op, const Pk& p) {
                CapPacket c;
                c.hdr.dir = dir;
                c.hdr.opcode = op;
                c.hdr.fileTime = now;
                c.data = p.b;
                c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
                w3.feed(c);
            };
            int x0 = 500, y0 = 400;
            for (const AtlasPoint& q : at.map(FROM)->points) if (q.type == 2) { x0 = q.x; y0 = q.y; break; }
            const uint16_t NPC = 920;
            { Pk p; p.u64(0x3344).zeros(9); const char nm[20] = "Leao"; for (char ch : nm) p.u8((uint8_t)ch);
              p.u16(9).u8(3).u8(1).zeros(69 - 41); feed3(DIR_S2C, 0x02A7, p); }
            feed3(DIR_S2C, 0x00E6, Pk().u32(0).u16(ME).u16(uint16_t(x0)).u16(uint16_t(y0)));
            feed3(DIR_S2C, 0x00E7, Pk().u16(0));
            { Pk p; p.zeros(278);
              auto put16 = [&](size_t o2, uint16_t v) { p.b[o2] = v & 0xFF; p.b[o2 + 1] = v >> 8; };
              auto put32 = [&](size_t o2, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o2 + i] = (v >> (8 * i)) & 0xFF; };
              put16(184 + 30, 40); put16(219, uint16_t(FROM)); put32(217 + 14, 50000); put32(217 + 18, 1000); put32(217 + 22, 500);
              feed3(DIR_S2C, 0x00E9, p); }
            feed3(DIR_S2C, 0x0008, Pk().u16(NPC).u16(2).u16(n->species).u8(0).u8(0).u16(1).u32(0).u16(uint16_t(n->x))
                                       .u16(uint16_t(n->y)).u16(uint16_t(n->x)).u16(uint16_t(n->y)).str("Npc").zeros(6));
            Bot b3;
            b3.start(w3, now);
            b3.travelOnly = true;
            b3.town.travel_to(TO, now);
            auto run3 = [&](uint16_t want, double maxSecs, Action* got) {
                const uint64_t end = now + uint64_t(maxSecs * 1000) * MS;
                while (now < end) {
                    now += 100 * MS;
                    Action a3;
                    if (!b3.tick(w3, now, a3)) continue;
                    feed3(DIR_C2S, a3.op, Pk{ a3.body });
                    if (a3.op != 0x0018)
                        printf("    0x%04X %-36s %s\n", a3.op, a3.label.c_str(), hex_preview(a3.body.data(), a3.body.size(), 40).c_str());
                    if (a3.op == want) { if (got) *got = a3; return true; }
                }
                printf("    (no 0x%04X in %.0f s: %s)\n", want, maxSecs, b3.doing.c_str());
                return false;
            };
            check(run3(0x006F, 120, &a) && a.body == Pk().u16(NPC).b, "walks to the teleporter, talks (0x6F)");
            feed3(DIR_S2C, 0x0070, Pk().u16(NPC).u16(n->species).u8(1).str("Where to?").u16(0).u16(0));
            check(run3(0x007F, 5, &a) && a.body == Pk().u16(NPC).u8(19).u8(1).u16(uint16_t(tp->slot)).b,
                  fmt("Teleport_Start [%d] (0x7F func 19)", tp->slot));
            feed3(DIR_S2C, 0x0075, Pk());
            feed3(DIR_S2C, 0x0029, Pk().u32(uint32_t(50000 - tp->cost)));
            feed3(DIR_S2C, 0x0099, Pk().u16(uint16_t(TO)).u16(0).u16(0));
            int ax = 500, ay = 400;
            for (const AtlasPoint& q : at.map(TO)->points) if (q.type == 7) { ax = q.x; ay = q.y; break; }
            feed3(DIR_S2C, 0x009A, Pk().u32(0).u64(0x3344).str("127.0.0.1").u32(8002).u32(0).u16(uint16_t(TO))
                                       .u16(uint16_t(ax)).u16(uint16_t(ay)).u8(3));
            now += 1500 * MS;
            feed3(DIR_S2C, 0x01B5, Pk().u16(ME).u16(uint16_t(ax)).u16(uint16_t(ay)).u32(0).u8(0));
            feed3(DIR_S2C, 0x009D, Pk().u32(0));
            for (int i = 0; i < 50 && b3.running; ++i) { now += 100 * MS; Action b; if (b3.tick(w3, now, b)) feed3(DIR_C2S, b.op, Pk{ b.body }); }
            check(w3.self.zone == TO && !b3.running && !b3.town.trip(), "in Coral Town, the bot stopped: " + b3.doing);
        } else {
            printf("\n(no teleport on the way %d -> %d)\n", FROM, TO);
        }
    }
    printf("\ntown: %d/%d PASS\n", pass, total);
}

// ── drill: the Drill action on map 2 (Gate of Caballa Relics: 50 %, items 20..40 deep) with real drills ──
static void drill_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const int ZONE = 2;
    const MapDigInfo* md = gd.map_dig(ZONE);
    const MapGrid* g = gd.map(ZONE);
    const DrillInfo* blue = gd.drill(8200);      // Blue Drill: reaches 80, any ground
    const DrillInfo* cleaner = gd.drill(8001);   // Cleaner Drill: reaches 30, sand
    if (!md || !g || !blue || !cleaner) { printf("missing data (map 2 / drills 8200, 8001): %s\n", err.c_str()); return; }

    printf("\nhelpers:\n");
    check(g->nature.size() == g->cell.size(), fmt("map 2's ground natures read (%zu cells)", g->nature.size()));
    check(drill_dig_ms(*blue, 30) == 700 + 3600, fmt("Blue Drill, 30 deep: the gauge takes %d ms (700 bar + 30 scoops)", drill_dig_ms(*blue, 30)));
    check(drill_shortest_ms(*blue, 30, 40) <= drill_dig_ms(*blue, 30) * 7 / 10 + 1,
          fmt("the server's shortest time %d ms <= 0.7 x that", drill_shortest_ms(*blue, 30, 40)));
    {
        const uint16_t seed = 0x2469;           // (the seed | the sense bit)
        const uint16_t crc = drill_crc(seed, *blue, 30, 1);
        const uint16_t calc = uint16_t(crc ^ uint16_t(blue->barSpeed << 1) ^ uint16_t(blue->barEffArea) ^
                                       uint16_t(blue->scoopMass << 5) ^ uint16_t(30 << 8) ^ uint16_t(1 << 12)) & 0xFFFE;
        check(calc == (seed & 0xFFFE), fmt("C2S 0x31 CRC %04X passes the server's check (-> %04X)", crc, calc));
    }

    // the start: an 11 x 11 block of the grid (first cells 1 + 10k) all open and diggable, nearest the middle; we stand
    // one cell in from its top-left corner, so its snake starts there and runs rightward
    const int S = 11, STEP = 10;
    int bx0 = -1, by0 = -1;
    for (int r = 0; r < 40 && bx0 < 0; ++r)
        for (int ky = -r; ky <= r && bx0 < 0; ++ky)
            for (int kx = -r; kx <= r && bx0 < 0; ++kx) {
                if (std::max(std::abs(kx), std::abs(ky)) != r) continue;
                const int x0 = 1 + (g->w / 2 / STEP + kx) * STEP, y0 = 1 + (g->h / 2 / STEP + ky) * STEP;
                bool ok = x0 > 0 && y0 > 0;
                for (int y = 0; y < S && ok; ++y)
                    for (int x = 0; x < S && ok; ++x) ok = g->diggable(x0 + x, y0 + y) && g->raw(x0 + x, y0 + y) == 0;
                if (ok) { bx0 = x0; by0 = y0; }
            }
    if (bx0 < 0) { printf("no open 11 x 11 block in map 2\n"); return; }
    const int X0 = (bx0 + 1) * 16 + 8, Y0 = (by0 + 1) * 16 + 8;
    auto cc = [](int cell) { return cell * 16 + 8; };
    printf("map %s: %d x %d cells, items in %.0f %% %d..%d deep; start (%d,%d), its block's corner cell (%d,%d)\n",
           gd.map_name(ZONE).c_str(), g->w, g->h, double(md->itemPct), md->minDepth, md->maxDepth, X0, Y0, bx0, by0);

    WorldModel w(&gd);
    const uint16_t ME = 500, MOB = 610;
    const uint64_t MS = 10000ull;
    uint64_t now = 100000000ull;
    uint64_t seq = 0;
    const std::string capDir = exe_dir() + "captures\\";     // also an exported capture, for TricksterAutomationStudio.exe --open
    CreateDirectoryA(capDir.c_str(), nullptr);
    std::ofstream cap(capDir + "selftest_drill.txt", std::ios::binary);
    cap << "# Trickster Automation Studio capture - selftest drill sequence\r\n";
    auto feed = [&](uint8_t dir, uint16_t op, const Pk& p) {
        CapPacket c;
        c.hdr.seq = ++seq;
        c.hdr.dir = dir;
        c.hdr.opcode = op;
        c.hdr.fileTime = now;
        c.data = p.b;
        c.hdr.size = c.hdr.dataLen = (uint32_t)p.b.size();
        w.feed(c);
        cap << fmt("%llu\t00:00:00.000\t%s\t0x%04X\t%u\t-\t\t", (unsigned long long)seq, dir == DIR_C2S ? "C>S" : "S>C", op,
                   c.hdr.size)
            << hex_preview(p.b.data(), p.b.size(), p.b.size()) << "\r\n";
    };
    { Pk p; p.u64(0x1122).zeros(9); const char nm[20] = "Leao"; for (char ch : nm) p.u8((uint8_t)ch);
      p.u16(9).u8(3).u8(1).zeros(69 - 41); feed(DIR_S2C, 0x02A7, p); }
    feed(DIR_S2C, 0x00E6, Pk().u32(0).u16(ME).u16(uint16_t(X0)).u16(uint16_t(Y0)));
    feed(DIR_S2C, 0x00E7, Pk().u16(2).u8(2).u32(8200).u64(0xD1).u16(3).u32(0).u8(0)
                              .u8(2).u32(8001).u64(0xD2).u16(300).u32(0).u8(0));
    { Pk p; p.zeros(278);
      auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
      auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
      auto put64 = [&](size_t o, uint64_t v) { for (int i = 0; i < 8; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
      put64(7 * 8, 0xD1);                                    // slot 7: the Blue Drill
      put16(184 + 12, 40);                                   // MaxWT points
      put16(184 + 14, 40);                                   // DA points
      put16(184 + 30, 40);                                   // level 40
      put16(219, uint16_t(ZONE)); put32(217 + 14, 50000); put32(217 + 18, 1000); put32(217 + 22, 500);
      feed(DIR_S2C, 0x00E9, p); }
    feed(DIR_S2C, 0x010F, Pk().u16(2).u16(3006).u8(1).u16(0).u16(3013).u8(11).u16(0));   // Sixth Sense, Basic Detection
    check(equipped_drill_info(w) == blue && w.self.zone == ZONE, "logged in with the Blue Drill on, in map 2");

    Bot bot;
    for (BotAction* a : bot.actions()) a->enabled = false;
    bot.drill.enabled = true;
    bot.start(w, now);
    std::vector<uint16_t> ops;                 // every C2S the bot sent
    // tick every 100 ms until a C2S `want` goes out (0 = none expected: run the whole time); our own C2S go back
    // into the model as the hook would capture them
    auto run = [&](uint16_t want, double secs, Action* got) {
        const uint64_t end = now + uint64_t(secs * 1000) * MS;
        while (now < end) {
            now += 100 * MS;
            Action a;
            if (!bot.tick(w, now, a)) continue;
            ops.push_back(a.op);
            feed(DIR_C2S, a.op, Pk{ a.body });
            printf("    t=%6.1f s  0x%04X %-44s [%s]\n", double(now - 100000000ull) / 1e7, a.op, a.label.c_str(), bot.doing.c_str());
            if (want && a.op == want) { if (got) *got = a; return true; }
        }
        if (want) printf("    (no 0x%04X in %.1f s: %s)\n", want, secs, bot.doing.c_str());
        return false;
    };
    auto u16at = [](const Action& a, size_t o) { return o + 1 < a.body.size() ? int(a.body[o] | a.body[o + 1] << 8) : -1; };
    int px = X0, py = Y0;                       // where the server has us
    // the server's side of a dig asked at (x, y): 0x2D, the walk (0x18 to us) when it is another cell, then 371
    auto answer = [&](int x, int y, int item, bool sensed, uint16_t seed) {
        int ax = 0, ay = 0;
        w.self_pos_at(now, ax, ay);
        px = ax; py = ay;
        feed(DIR_S2C, 0x002D, Pk());
        if (x / 16 != px / 16 || y / 16 != py / 16) {
            feed(DIR_S2C, 0x0018, Pk().u16(ME).u16(uint16_t(px)).u16(uint16_t(py)).u16(uint16_t(x)).u16(uint16_t(y)).u8(2));
            now += uint64_t(std::hypot(double(x - px), double(y - py)) / 160.0 * 1000.0 + 50) * MS;
            px = x; py = y;
        } else {
            x = px; y = py;
        }
        feed(DIR_S2C, 0x0173, Pk().u16(ME).u16(uint16_t(x)).u16(uint16_t(y)).u8(0).u8(uint8_t(md->maxDepth / 5))
                                  .u8(uint8_t(item / 5)).u8(g->nature_at(x / 16, y / 16)).u16(uint16_t((seed & 0xFFFE) | (sensed ? 1 : 0))).u8(0));
    };
    auto stop50 = [&](uint8_t reason, uint16_t life, uint32_t item) { feed(DIR_S2C, 0x0032, Pk().u8(reason).u16(life).u32(item)); };
    auto at_cell = [&](const Action& a, int cx, int cy) { return u16at(a, 0) / 16 == cx && u16at(a, 2) / 16 == cy; };

    // 1. the first dig: the corner of its block (in lines from there); an item 30 deep -> the find after the gauge's
    //    time, a buff due meanwhile waits
    printf("\n1. the find:\n");
    Action a;
    check(run(0x002C, 2, &a) && at_cell(a, bx0, by0) && u16at(a, 0) == cc(bx0),
          fmt("starts at its block's top-left corner (%d,%d), the cell's centre", bx0, by0));
    check(bot.drill.hasBlock && bot.drill.blockCellX == bx0 && bot.drill.blockCellY == by0 && bot.drill.blockSide == S,
          fmt("the block: (%d,%d) %d x %d, fail limit %d", bot.drill.blockCellX, bot.drill.blockCellY, bot.drill.blockSide,
              bot.drill.blockSide, bot.drill.failLimit));
    answer(cc(bx0), cc(by0), 30, false, 0x2468);
    const uint64_t infoAt = now;
    bot.buffs.enabled = true;
    bot.buffs.keep = { 3006 };                  // Sixth Sense due now: it must wait for the end of the dig
    size_t mark = ops.size();
    const bool dug = run(0x0031, 8, &a);
    const uint64_t took = (now - infoAt) / MS;
    check(dug && ops.size() == mark + 1, fmt("nothing else while digging: %zu C2S (the buff waits)", ops.size() - mark));
    const int cnt = u16at(a, 0), ms = u16at(a, 2), depth = u16at(a, 4), crc = u16at(a, 6);
    const uint16_t calc = uint16_t(crc ^ uint16_t(blue->barSpeed << 1) ^ uint16_t(blue->barEffArea) ^ uint16_t(blue->scoopMass << 5) ^
                                   uint16_t(depth << 8) ^ uint16_t(cnt << 12)) & 0xFFFE;
    check(cnt == 1 && depth == 30 && calc == 0x2468, fmt("0x31: cnt %d, depth %d, CRC ok (%04X)", cnt, depth, calc));
    check(took >= uint64_t(drill_dig_ms(*blue, 30)) && ms >= drill_shortest_ms(*blue, 30, md->maxDepth),
          fmt("reported after %llu ms (gauge %d, server minimum %d), ms field %d", (unsigned long long)took, drill_dig_ms(*blue, 30),
              drill_shortest_ms(*blue, 30, md->maxDepth), ms));
    stop50(3, 2, 101);
    feed(DIR_S2C, 0x000E, Pk().u16(ME).u16(700).u16(uint16_t(px + 16)).u16(uint16_t(py)).u16(uint16_t(px)).u16(uint16_t(py)).u16(0)
                              .u8(0).u8(0).u8(0).u8(1).u32(101).u64(0).u16(1));
    const BuriedCell* c0 = w.buried_at(bx0, by0);
    check(w.drillStats.finds == 1 && w.drillStats.foundCount[101] == 1 && w.find_item(0xD1)->count == 2 && c0 &&
          c0->state == BuriedCell::Empty && w.groundItems.count(700) && w.groundItems.at(700).owner == ME,
          "the model: 1 find (counted per item), life 2, the cell empty, the find on the ground, ours");

    // 2. between digs: the buff first, then the pick-up
    printf("\n2. between digs:\n");
    check(run(0x00EE, 2, &a) && u16at(a, 0) == 3006, "the buff now (0xEE Sixth Sense)");
    feed(DIR_S2C, 0x00F0, Pk().u16(3006).u8(1));
    feed(DIR_S2C, 0x00E0, Pk().u16(ME).u16(ME).u16(950).u16(1092).u8(2).u8(3).u16(3006).u16(0).u8(1));
    check(run(0x0021, 2, &a) && u16at(a, 0) == 700, "picks up its find (0x21)");
    feed(DIR_S2C, 0x0022, Pk().u16(ME).u16(700).u8(1));
    feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(101).u64(0x500).u16(1).u8(0));

    // 3. a cell with nothing: a free cancel; the next cell is the next one of the row
    printf("\n3. nothing there:\n");
    check(run(0x002C, 2, &a) && at_cell(a, bx0 + 1, by0), "the next dig: the next cell of the row");
    answer(u16at(a, 0), u16at(a, 2), 0, false, 0x1110);
    const uint64_t t3 = now;
    check(run(0x0030, 2, &a) && u16at(a, 0) == 0 && (now - t3) / MS <= 400, "cancelled at once, cnt 0 (no life)");
    stop50(1, 2, 0);

    // 4. a sensed empty cell: the client cancels it by itself
    printf("\n4. sensed and empty:\n");
    check(run(0x002C, 2, &a) && at_cell(a, bx0 + 2, by0), "the next cell of the row");
    answer(u16at(a, 0), u16at(a, 2), 0, true, 0x1112);
    mark = ops.size();
    run(0, 1.0, nullptr);
    check(ops.size() == mark, "waits for the client's own cancel");
    feed(DIR_C2S, 0x0030, Pk().u16(0).u16(30));
    stop50(1, 2, 0);
    check(!w.drill_open(now), "the dig is over");

    // 5. the last life: the swap to the Cleaner Drill
    printf("\n5. worn out:\n");
    check(run(0x002C, 2, &a) && at_cell(a, bx0 + 3, by0), "the next cell of the row");
    answer(u16at(a, 0), u16at(a, 2), 20, false, 0x3330);
    check(run(0x0031, 6, &a) && u16at(a, 4) == 20, "digs out the item 20 deep");
    stop50(3, 0, 102);
    check(run(0x0051, 2, &a) && a.body.size() == 9 && a.body[8] == 7 && a.body[0] == 0xD2, "the drill is worn out -> puts on the Cleaner Drill (0x51 [D2][7])");
    feed(DIR_S2C, 0x0055, Pk().u64(0xD1).u8(7));
    feed(DIR_S2C, 0x0052, Pk().u64(0xD2).u8(7));
    check(equipped_drill_info(w) == cleaner, "the Cleaner Drill is on");

    // 6. an item deeper than the Cleaner reaches (30): left, for free
    printf("\n6. too deep:\n");
    check(run(0x002C, 2, &a) && at_cell(a, bx0 + 4, by0), "the next cell of the row");
    const int dx6 = u16at(a, 0), dy6 = u16at(a, 2);
    answer(dx6, dy6, 40, false, 0x4440);
    check(run(0x0030, 2, &a) && u16at(a, 0) == 0, "an item 40 deep: cancelled, cnt 0");
    stop50(1, 300, 0);
    const BuriedCell* c6 = w.buried_at(dx6 / 16, dy6 / 16);
    check(c6 && c6->state == BuriedCell::Item && c6->depth == 40, "known: an item 40 deep there");

    // 7. Basic Detection: the grid becomes its square (Lv 11: 15 x 15); a new block, scanned ONCE from its centre;
    //    then its points are dug, nothing else of it
    printf("\n7. Basic Detection:\n");
    bot.drill.set_detect(3013, true);
    check(run(0x00EE, 6, &a) && u16at(a, 0) == 3013, "walks to the block's centre and casts Basic Detection (0xEE)");
    const int sbx = bot.drill.blockCellX, sby = bot.drill.blockCellY, ss = bot.drill.blockSide;
    int qx = 0, qy = 0;
    w.self_pos_at(now, qx, qy);
    check(ss == 15 && std::abs(qx - (16 * sbx + 8 * ss)) <= 8 && std::abs(qy - (16 * sby + 8 * ss)) <= 8,
          fmt("a %d x %d block at (%d,%d); cast from (%d,%d), its centre (%d,%d)", ss, ss, sbx, sby, qx, qy, 16 * sbx + 8 * ss, 16 * sby + 8 * ss));
    std::vector<std::pair<int, int>> pts;       // two open diggable cells of the block nothing is known of
    for (int y = sby + 2; y < sby + ss && pts.size() < 2; y += 5)
        for (int x = sbx + 3; x < sbx + ss && pts.size() < 2; ++x)
            if (g->diggable(x, y) && g->raw(x, y) == 0 && !w.buried_at(x, y)) { pts.emplace_back(x, y); break; }
    if (pts.size() < 2) { printf("no detector test cells\n"); return; }
    feed(DIR_S2C, 0x00F0, Pk().u16(3013).u8(11));
    feed(DIR_S2C, 0x00E0, Pk().u16(ME).u16(ME).u16(0).u16(1170).u8(2).u8(255).u16(3013).u16(0)
                              .u8(1).u16(2).u16(uint16_t(pts[0].first * 16)).u16(uint16_t(pts[0].second * 16))
                              .u16(uint16_t(pts[1].first * 16)).u16(uint16_t(pts[1].second * 16)).u16(3));
    {
        int none = 0, open = 0;
        for (int y = sby; y < sby + ss; ++y)
            for (int x = sbx; x < sbx + ss; ++x) {
                if (!(g->raw(x, y) == 0 && g->nature_at(x, y) != 6)) continue;
                const BuriedCell* b = w.buried_at(x, y);
                ++open;
                none += b && (b->noneTo >= 110 || b->state != BuriedCell::Unknown);
            }
        const BuriedCell* p0 = w.buried_at(pts[0].first, pts[0].second);
        check(p0 && p0->state == BuriedCell::Detected && w.detectFound == 2, "the points: detected");
        check(none == open, fmt("the rest of the block: nothing down to 110 (%d / %d open cells known)", none, open));
    }
    std::set<std::pair<int, int>> dugPts;
    for (int k = 0; k < 2; ++k) {
        const bool asked = run(0x002C, 4, &a);
        const std::pair<int, int> cell{ u16at(a, 0) / 16, u16at(a, 2) / 16 };
        check(asked && std::find(pts.begin(), pts.end(), cell) != pts.end() && !dugPts.count(cell),
              fmt("digs a detected cell (%d,%d)", cell.first, cell.second));
        dugPts.insert(cell);
        answer(u16at(a, 0), u16at(a, 2), k ? 20 : 30, false, uint16_t(0x5550 + 2 * k));
        check(run(0x0031, 6, &a), "and digs it out");
        stop50(3, uint16_t(299 - k), 103);
    }

    // 8. a monster swings at us: no fight in drill mode, unless "fight back"
    printf("\n8. fight back:\n");
    bot.drill.set_detect(3013, false);          // (blocks of 11 again)
    w.self_pos_at(now, px, py);
    feed(DIR_S2C, 0x0008, Pk().u16(MOB).u16(3).u16(2000).u8(0).u8(0).u16(22).u32(0).u16(uint16_t(px + 40)).u16(uint16_t(py)).u16(uint16_t(px + 40))
                              .u16(uint16_t(py)).str("Sea Scorpion").zeros(14));
    feed(DIR_S2C, 0x001F, Pk().u16(MOB).u16(1).u16(ME).u16(uint16_t(px + 40)).u16(uint16_t(py)).u16(uint16_t(px)).u16(uint16_t(py)).u16(5).u8(1));
    mark = ops.size();
    run(0, 1.5, nullptr);
    check(std::find(ops.begin() + long(mark), ops.end(), uint16_t(0x001D)) == ops.end() && ops.size() > mark,
          "fight back off: it goes on (no 0x1D)");
    w.drill = DrillSession{};                   // (the server never answered what it asked)
    bot.drill.fightBack = true;
    feed(DIR_S2C, 0x001F, Pk().u16(MOB).u16(1).u16(ME).u16(uint16_t(px + 40)).u16(uint16_t(py)).u16(uint16_t(px)).u16(uint16_t(py)).u16(5).u8(1));
    check(run(0x001D, 1.5, &a) && u16at(a, 0) == MOB, "fight back on: attacks the monster (0x1D)");
    feed(DIR_S2C, 0x002B, Pk().u16(MOB).u8(0));
    feed(DIR_S2C, 0x000F, Pk().u16(MOB).u8(0));
    now += 5000 * MS;                           // (the swings are old now)
    w.drill = DrillSession{};

    // 9. a block where nothing is found: left after its fail limit, the next block away from it (another area)
    printf("\n9. a dry block:\n");
    bot.drill.area = DrillAction::WholeMap;
    int dryX = INT_MIN, dryY = INT_MIN, limit = 0, fails = 0;
    bool moved = false;
    for (int k = 0; k < 40 && !moved; ++k) {
        if (!run(0x002C, 4, &a)) break;
        const int cx = u16at(a, 0) / 16, cy = u16at(a, 2) / 16;
        if (dryX == INT_MIN) { dryX = bot.drill.blockCellX; dryY = bot.drill.blockCellY; limit = bot.drill.failLimit; }
        if (bot.drill.blockCellX != dryX || bot.drill.blockCellY != dryY) {
            moved = true;
            const int away = std::max(std::abs(bot.drill.blockCellX - dryX), std::abs(bot.drill.blockCellY - dryY));
            check(fails == limit && away > STEP, fmt("after %d probes with nothing (limit %d) the next block (%d,%d) is %d cells away from (%d,%d)",
                                                    fails, limit, bot.drill.blockCellX, bot.drill.blockCellY, away, dryX, dryY));
            check(bot.drill.poor().count(DrillAction::block_key(ZONE, dryX, dryY)) == 1, "the dry block is left for a while");
            break;
        }
        answer(cc(cx), cc(cy), 0, false, uint16_t(0x6000 + 2 * k));
        if (!run(0x0030, 2, &a)) break;
        stop50(1, 298, 0);
        ++fails;
    }
    check(moved, "it moved to another area");
    w.drill = DrillSession{};

    // 10. the Crazy Drill Mode: the Cleaner (30) does not reach the map's depth (40) -> no digs while it runs
    printf("\n10. Crazy Drill Mode:\n");
    now += 2000 * MS;
    stop50(11, 20, 0);
    mark = ops.size();
    run(0, 1.5, nullptr);
    check(ops.size() == mark && bot.drill.status.find("Crazy Drill") != std::string::npos, "waits: " + bot.drill.status);
    stop50(16, 0, 0);
    check(run(0x002C, 3, &a), "over: digs again");

    // 11. selling by weight: the Supply trip is due when the bag weighs that share of MaxWT
    printf("\n11. sell by weight:\n");
    {
        TownAction& t = bot.town;
        const Atlas& at = gd.atlas();
        for (int i : at.shops()) { t.shopKey = at.npc(i)->key(); break; }
        t.enabled = t.supply = t.sell = true;
        t.sellList.white = { 101 };
        const int maxWt = w.stat_base(ST_MAXWT);
        const int64_t wt = w.bag_weight();
        t.weightPct = 100;
        const std::string none = t.need(w);
        t.weightPct = 1;
        const std::string due = t.need(w);
        check(maxWt > 0 && wt > 0 && none.find("weighs") == std::string::npos && due.find("weighs") != std::string::npos,
              fmt("weight %lld / %d: at 100 %% \"%s\", at 1 %% \"%s\"", (long long)wt, maxWt, none.c_str(), due.c_str()));
    }

    printf("\ndrill: %d/%d\n", pass, total);
}

// ── layouts: the hook's client builds checked against the client files on disk (mapped as images, nothing runs) ──
// ── stuck: the drawn avatar against the server (bot/stuck.cpp), walls in the target / item choice and the walk
//    round them (act_attack.cpp / act_loot.cpp) ──
struct StuckRig {
    enum { ME = 500 };
    std::unique_ptr<WorldModel> w;
    Bot bot;
    uint64_t now = 100000000ull, seq = 0;
    std::vector<Action> sent;
    bool drawn = false;             // the hook's avatar feed is on
    bool follows = true;            // the avatar is drawn where the server has us (else it stays at dX, dY)
    int dX = 0, dY = 0;

    explicit StuckRig(const GameData& gd) : w(new WorldModel(&gd)) {}
    void feed(uint8_t dir, uint16_t op, const std::vector<uint8_t>& b)
    {
        CapPacket c;
        c.hdr.seq = ++seq;
        c.hdr.dir = dir;
        c.hdr.opcode = op;
        c.hdr.fileTime = now;
        c.data = b;
        c.hdr.size = c.hdr.dataLen = (uint32_t)b.size();
        w->feed(c);
    }
    void login(int zone, int x, int y, int level = 40)
    {
        Pk n; n.u64(0x1122).zeros(9);
        const char nm[20] = "Leao";
        for (char ch : nm) n.u8((uint8_t)ch);
        n.u16(9).u8(3).u8(1).zeros(69 - 41);
        feed(DIR_S2C, 0x02A7, n.b);
        feed(DIR_S2C, 0x00E6, Pk().u32(0).u16(ME).u16(uint16_t(x)).u16(uint16_t(y)).b);
        Pk p; p.zeros(278);
        auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
        auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
        put16(184 + 30, uint16_t(level));
        put16(219, uint16_t(zone)); put32(217 + 14, 50000); put32(217 + 18, 1000); put32(217 + 22, 500);
        feed(DIR_S2C, 0x00E9, p.b);
        dX = x; dY = y;
    }
    void monster(uint16_t eid, int x, int y, const char* name, uint16_t species = 2000)
    {
        feed(DIR_S2C, 0x0008, Pk().u16(eid).u16(3).u16(species).u8(0).u8(0).u16(22).u32(0).u16(uint16_t(x)).u16(uint16_t(y))
                                  .u16(uint16_t(x)).u16(uint16_t(y)).str(name).zeros(14).b);
    }
    void npc(uint16_t eid, uint16_t species, int x, int y)
    {
        feed(DIR_S2C, 0x0008, Pk().u16(eid).u16(2).u16(species).u8(0).u8(0).u16(1).u32(0).u16(uint16_t(x)).u16(uint16_t(y))
                                  .u16(uint16_t(x)).u16(uint16_t(y)).str("Npc").zeros(6).b);
    }
    // 153 / 154, then 437 / 157 1.5 s later (a portal, a relocate)
    void enter_zone(int zone, int x, int y)
    {
        feed(DIR_S2C, 0x0099, Pk().u16(uint16_t(zone)).u16(0).u16(0).b);
        feed(DIR_S2C, 0x009A, Pk().u32(0).u64(0x1122).str("127.0.0.1").u32(8002).u32(0).u16(uint16_t(zone))
                                  .u16(uint16_t(x)).u16(uint16_t(y)).u8(2).b);
        now += 1500 * 10000ull;
        feed(DIR_S2C, 0x01B5, Pk().u16(ME).u16(uint16_t(x)).u16(uint16_t(y)).u32(0).u8(0).b);
        feed(DIR_S2C, 0x009D, Pk().u32(0).b);
        dX = x; dY = y;
    }
    int timerLeft = -1;             // a monster quest's timer: S2C 215 each second while >= 0 (ms)
    uint64_t timerAt = 0;
    void avatar()
    {
        if (!drawn) return;
        if (follows) w->self_pos_at(now, dX, dY);
        EvAvatar a{};
        a.fileTime = now;
        a.actor = 0x01234560;
        a.x = a.dx = int16_t(dX);
        a.y = a.dy = int16_t(dY);
        w->feed_avatar(a);
    }
    // ticks of 100 ms for `secs` (stops at the first C2S `want`; 0 = all of it); our C2S go back into the model
    bool run(uint16_t want, double secs, Action* got = nullptr)
    {
        const uint64_t end = now + uint64_t(secs * 1000) * 10000ull;
        while (now < end) {
            now += 100 * 10000ull;
            avatar();
            if (timerLeft >= 0 && now - timerAt >= 1000 * 10000ull) {
                timerAt = now;
                timerLeft = std::max(0, timerLeft - 1000);
                feed(DIR_S2C, 0x00D7, Pk().u32(uint32_t(timerLeft)).u32(1800000).b);
            }
            Action a;
            if (!bot.tick(*w, now, a)) continue;
            sent.push_back(a);
            feed(DIR_C2S, a.op, a.body);
            printf("    t=%6.1f s  0x%04X %-46s [%s]\n", double(now - 100000000ull) / 1e7, a.op, a.label.c_str(), bot.doing.c_str());
            if (want && a.op == want) { if (got) *got = a; return true; }
        }
        return false;
    }
    bool logged(const char* text) const
    {
        for (const std::string& l : bot.log) if (l.find(text) != std::string::npos) return true;
        return false;
    }
};

static void stuck_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const int ZONE = 2;
    const MapGrid* g = gd.map(ZONE);
    if (!g) { printf("missing data (map 2): %s\n", err.c_str()); return; }
    auto u16at = [](const Action& a, size_t o) { return o + 1 < a.body.size() ? int(a.body[o] | a.body[o + 1] << 8) : -1; };
    // Reach::straight: the line to the spot, its last 24 units left out
    auto straight = [&](int px, int py, int x, int y) {
        const double d = std::hypot(double(x - px), double(y - py));
        if (d <= 24.0) return true;
        return g->line_clear(px, py, px + int((x - px) * (d - 24.0) / d), py + int((y - py) * (d - 24.0) / d));
    };

    // the places, from a start P: R = round a wall (no straight line, a walk 1.4..2.5 x the distance), U = nearer
    // than C but another area / a long way round, C = an open straight line
    struct Spot { int x = -1, y = -1; double d = 0, walk = 0; };
    int PX = -1, PY = -1;
    Spot R, U, C;
    std::vector<int32_t> f;
    for (int cy = 4; cy < g->h - 4 && PX < 0; cy += 3)
        for (int cx = 4; cx < g->w - 4 && PX < 0; cx += 3) {
            if (!g->open_cell(cx, cy)) continue;
            const int px = cx * 16 + 8, py = cy * 16 + 8;
            Atlas::walk_field(*g, px, py, f);
            Spot r, u, o;
            for (int dy = -16; dy <= 16; ++dy)
                for (int dx = -16; dx <= 16; ++dx) {
                    if (!g->open_cell(cx + dx, cy + dy)) continue;
                    const int mx = (cx + dx) * 16 + 8, my = (cy + dy) * 16 + 8;
                    const double d = std::hypot(double(mx - px), double(my - py));
                    if (d < 100 || d > 250) continue;
                    const double wk = Atlas::field_walk(*g, f, mx, my);
                    const bool line = straight(px, py, mx, my);
                    if (r.x < 0 && !line && wk > d * 1.4 && wk < d * 2.5) r = { mx, my, d, wk };
                    if (u.x < 0 && d < 170 && (wk < 0 || wk > d * 3 + 200)) u = { mx, my, d, wk };
                    if (o.x < 0 && line && d > 190 && wk > 0 && wk < d * 1.15) o = { mx, my, d, wk };
                }
            if (r.x >= 0 && u.x >= 0 && o.x >= 0) { PX = px; PY = py; R = r; U = u; C = o; }
        }
    if (PX < 0) { printf("no start in map 2 with a wall to walk round, a cut-off spot and an open one\n"); return; }
    printf("map %s: start P (%d,%d)\n  R round a wall (%d,%d): %.0f away, a %.0f walk\n  U cut off (%d,%d): %.0f away, %s\n"
           "  C open (%d,%d): %.0f away\n", gd.map_name(ZONE).c_str(), PX, PY, R.x, R.y, R.d, R.walk, U.x, U.y, U.d,
           U.walk < 0 ? "no way there" : fmt("a %.0f walk", U.walk).c_str(), C.x, C.y, C.d);

    auto fresh = [&](StuckRig& s, bool attack, bool loot) {
        s.login(ZONE, PX, PY);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.attack.enabled = attack;
        s.bot.loot.enabled = loot;
        s.run(0, 3.5);                                     // past the map's settle time
        s.bot.start(*s.w, s.now);
    };

    printf("\nthe target is chosen by the walk (U is nearer in a straight line):\n");
    {
        StuckRig s(gd);
        fresh(s, true, false);
        s.monster(601, U.x, U.y, "Cut Off");
        s.monster(602, C.x, C.y, "In The Open");
        Action a;
        const bool hit = s.run(0x001D, 3, &a);
        check(hit && u16at(a, 0) == 602, fmt("the open one (602) is attacked, not the cut-off one (0x1D target %d)", hit ? u16at(a, 0) : -1));
    }

    printf("\na wall on the line: the grid path first, the attack once the line is open:\n");
    {
        StuckRig s(gd);
        s.drawn = true;                                    // drawn where the server has us: no stuck
        fresh(s, true, false);
        s.monster(603, R.x, R.y, "Behind The Wall");
        Action a;
        const bool hit = s.run(0x001D, 25, &a);
        size_t legs = 0;
        bool open = true;
        for (const Action& x : s.sent) {
            if (x.op != 0x0018) continue;
            ++legs;
            open = open && g->line_clear(u16at(x, 0), u16at(x, 2), u16at(x, 4), u16at(x, 6));
        }
        int px = 0, py = 0;
        s.w->self_pos_at(s.now, px, py);
        check(!s.sent.empty() && s.sent.front().op == 0x0018, "the first request is a walk (0x18), not the attack");
        check(legs >= 1 && open, fmt("%zu legs, each an open straight line", legs));
        check(hit && u16at(a, 0) == 603 && straight(px, py, R.x, R.y),
              fmt("then 0x1D at 603 from (%d,%d), the line open", px, py));
        check(s.bot.guard.live && s.bot.guard.count == 0, "drawn where the server has us all along: never stuck");
    }

    printf("\nthe client cannot follow the server's chase: stuck -> stand where we are drawn, the target left:\n");
    {
        StuckRig s(gd);
        s.drawn = true;
        fresh(s, true, false);
        s.monster(604, C.x, C.y, "Out Of Reach");
        Action a;
        const bool hit = s.run(0x001D, 3, &a);
        check(hit && u16at(a, 0) == 604, "0x1D at 604");
        // the server walks us straight at it (S2C 0x18 for us); the client finds no way and the avatar stays at P
        const double d = std::hypot(double(C.x - PX), double(C.y - PY));
        const int qx = C.x + int((PX - C.x) * 30 / d), qy = C.y + int((PY - C.y) * 30 / d);
        s.follows = false;
        s.dX = PX; s.dY = PY;
        s.feed(DIR_S2C, 0x0018, Pk().u16(StuckRig::ME).u16(uint16_t(PX)).u16(uint16_t(PY)).u16(uint16_t(qx)).u16(uint16_t(qy)).u8(2).b);
        const uint64_t chaseAt = s.now;
        Action r;
        bool resync = false;
        for (int k = 0; k < 30 && !ft_older(s.now, chaseAt, 6000) && s.run(0x0018, 6, &r); ++k)
            if (u16at(r, 0) == u16at(r, 4) && u16at(r, 2) == u16at(r, 6)) { resync = true; break; }
        check(resync && u16at(r, 0) == PX && u16at(r, 2) == PY,
              fmt("C2S 0x18 [drawn][drawn] = stand at (%d,%d), %.1f s after the chase", u16at(r, 0), u16at(r, 2),
                  double(s.now - chaseAt) / 1e7));
        int px = 0, py = 0;
        s.w->self_pos_at(s.now, px, py);
        check(px == PX && py == PY, fmt("the model has us back at (%d,%d)", px, py));
        check(s.bot.guard.count == 1 && s.logged("stuck: drawn at"), "one stuck event, logged");
        s.run(0, 1);
        check(s.bot.attack.target != 604 && s.logged("could not walk there"), "604 is left (2 min)");
    }

    printf("\nstuck again and again (a drawn position that cannot be right): 4 resyncs, then a 5 min pause:\n");
    {
        StuckRig s(gd);
        s.drawn = true;
        fresh(s, false, false);
        s.follows = false;
        s.dX = PX; s.dY = PY;
        int resyncs = 0;
        for (int k = 0; k < 6; ++k) {                       // the server keeps walking us off, the avatar stays
            s.feed(DIR_S2C, 0x0018, Pk().u16(StuckRig::ME).u16(uint16_t(PX)).u16(uint16_t(PY)).u16(uint16_t(C.x))
                                        .u16(uint16_t(C.y)).u8(2).b);
            Action r;
            if (s.run(0x0018, 5, &r) && u16at(r, 0) == u16at(r, 4) && u16at(r, 2) == u16at(r, 6)) ++resyncs;
        }
        check(resyncs == 4 && s.bot.guard.pausedUntil > s.now && s.logged("the stuck guard pauses"),
              fmt("%d resyncs, then paused (%.0f s left)", resyncs,
                  s.bot.guard.pausedUntil > s.now ? double(s.bot.guard.pausedUntil - s.now) / 1e7 : 0.0));
    }

    printf("\nthe same without the avatar feed (an older hook DLL): nothing is seen, no resync:\n");
    {
        StuckRig s(gd);
        fresh(s, true, false);
        s.monster(605, C.x, C.y, "Out Of Reach");
        s.run(0x001D, 3);
        s.feed(DIR_S2C, 0x0018, Pk().u16(StuckRig::ME).u16(uint16_t(PX)).u16(uint16_t(PY)).u16(uint16_t(C.x)).u16(uint16_t(C.y)).u8(2).b);
        const size_t before = s.sent.size();
        s.run(0, 4);
        bool resync = false;
        for (size_t i = before; i < s.sent.size(); ++i)
            resync |= s.sent[i].op == 0x0018 && u16at(s.sent[i], 0) == u16at(s.sent[i], 4) && u16at(s.sent[i], 2) == u16at(s.sent[i], 6);
        check(!s.bot.guard.live && !resync && s.bot.guard.count == 0, "no feed: not live, no stand-here");
    }

    printf("\nan item behind a wall: the grid path first, the pick-up once the line is open:\n");
    {
        StuckRig s(gd);
        fresh(s, false, true);
        s.feed(DIR_S2C, 0x000A, Pk().u16(700).u16(uint16_t(R.x)).u16(uint16_t(R.y)).u16(0).u8(0).u8(0).u8(0)
                                    .u8(2).u32(8001).u64(0xA1).u16(1).u32(0).u8(0).b);
        Action a;
        const bool got = s.run(0x0021, 25, &a);
        int px = 0, py = 0;
        s.w->self_pos_at(s.now, px, py);
        check(!s.sent.empty() && s.sent.front().op == 0x0018, "the first request is a walk (0x18)");
        check(got && u16at(a, 0) == 700 && straight(px, py, R.x, R.y), fmt("then 0x21 for item 700 from (%d,%d), the line open", px, py));
    }

    printf("\n%d / %d checks passed\n", pass, total);
}

// ── the roam's sweep (bot/act_roam.cpp): rounds over the areas, no goal twice in a round, each round another
//    route turning the other way, walking without pauses; another bot starts another way ──
static void roam_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const int ZONE = 2;
    const MapGrid* g = gd.map(ZONE);
    const AtlasMap* m = gd.atlas().map(ZONE);
    if (!g || !m) { printf("missing data (map 2): %s\n", err.c_str()); return; }
    int sx = 600, sy = 600;
    for (const AtlasPoint& p : m->points) if ((p.type == 2 || p.type == 1) && !g->blocked(p.x, p.y)) { sx = p.x; sy = p.y; break; }
    // the areas we can walk to from there (the roam's 256-unit areas, the open cell nearest each middle)
    std::vector<int32_t> f;
    Atlas::walk_field(*g, sx, sy, f);
    int reachable = 0;
    for (int ay = 0; ay * 16 < g->h; ++ay)
        for (int ax = 0; ax * 16 < g->w; ++ax) {
            int best = INT_MAX, bx = -1, by = -1;
            for (int cy = ay * 16; cy < std::min(g->h, ay * 16 + 16); ++cy)
                for (int cx = ax * 16; cx < std::min(g->w, ax * 16 + 16); ++cx) {
                    if (!g->open_cell(cx, cy)) continue;
                    const int d = (cx - ax * 16 - 8) * (cx - ax * 16 - 8) + (cy - ay * 16 - 8) * (cy - ay * 16 - 8);
                    if (d < best) { best = d; bx = cx; by = cy; }
                }
            if (bx >= 0 && Atlas::field_walk(*g, f, bx * 16 + 8, by * 16 + 8) >= 0) ++reachable;
        }
    printf("map %s: %d x %d cells, start (%d,%d), %d areas within reach\n", gd.map_name(ZONE).c_str(), g->w, g->h, sx, sy, reachable);

    // one run of the sweep (a new bot: its own draws): the goals of each round in order, its sense, the most
    // areas seen in it
    struct SweepRun {
        std::vector<std::vector<std::pair<int, int>>> goals;    // [round]
        std::vector<int> sense, done;                           // [round]
        int round1Secs = -1;                                    // when round 2 began
        double walked = 0, secsMoving = 0;
        int speed = 160;
        std::vector<Action> sent;
    };
    auto sweep = [&](int secs) {
        SweepRun out;
        StuckRig s(gd);
        s.login(ZONE, sx, sy);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.attack.enabled = true;
        s.bot.attack.radius = 200;                          // (a small radius: more to walk)
        s.bot.roam.enabled = true;
        s.bot.roam.mode = RoamAction::WholeMap;
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        int lastX = sx, lastY = sy, firstLegAt = -1;
        const uint64_t t0 = s.now;
        for (int k = 0; k < secs * 10; ++k) {
            s.run(0, 0.1);
            int px = 0, py = 0;
            s.w->self_pos_at(s.now, px, py);
            out.walked += std::hypot(double(px - lastX), double(py - lastY));
            lastX = px;
            lastY = py;
            if (firstLegAt < 0 && !s.sent.empty()) firstLegAt = k;
            const RoamAction& r = s.bot.roam;
            const size_t n = size_t(r.sweepRound.n);
            if (!n) continue;
            if (out.goals.size() <= n) { out.goals.resize(n + 1); out.sense.resize(n + 1, 0); out.done.resize(n + 1, 0); }
            if (n >= 2 && out.round1Secs < 0) out.round1Secs = k / 10;
            out.sense[n] = r.sweepRound.sense;
            out.done[n] = std::max(out.done[n], r.sweepDone);
            auto& gl = out.goals[n];
            if (r.hasGoal && r.goalWhy.rfind("sweeping", 0) == 0 && (gl.empty() || gl.back() != std::make_pair(r.goalX, r.goalY)))
                gl.push_back({ r.goalX, r.goalY });
        }
        out.secsMoving = double(s.now - t0) / 1e7 - firstLegAt / 10.0;
        out.speed = s.w->self.speed > 0 ? s.w->self.speed : 160;
        out.sent = s.sent;
        return out;
    };
    const int secs = 240;
    const SweepRun a = sweep(secs);
    const size_t rounds = a.goals.empty() ? 0 : a.goals.size() - 1;   // started (the last one may be cut short)
    for (size_t n = 1; n < a.goals.size(); ++n) {
        std::string first;
        for (size_t i = 0; i < std::min<size_t>(5, a.goals[n].size()); ++i) first += fmt(" (%d,%d)", a.goals[n][i].first, a.goals[n][i].second);
        printf("  round %zu %s: %zu goals, %d areas seen, first:%s\n", n, a.sense[n] > 0 ? "clockwise" : "counter-clockwise",
               a.goals[n].size(), a.done[n], first.c_str());
    }
    check(a.round1Secs >= 0 && rounds >= 1 && a.done[1] >= reachable * 9 / 10,
          fmt("round 1 over after %d s with %d areas seen (%d within reach)", a.round1Secs, rounds >= 1 ? a.done[1] : 0, reachable));
    int repeats = 0;
    for (size_t n = 1; n < a.goals.size(); ++n)
        for (size_t i = 0; i < a.goals[n].size(); ++i)
            for (size_t j = 0; j < i; ++j) repeats += a.goals[n][i] == a.goals[n][j];
    check(repeats == 0, fmt("%zu rounds in %d s, no area a goal twice in a round (%d repeats)", rounds, secs, repeats));
    bool differ = rounds >= 3, alternate = rounds >= 2;
    for (size_t n = 2; n < rounds; ++n) differ = differ && a.goals[n] != a.goals[n - 1];      // (the whole rounds)
    for (size_t n = 2; n <= rounds; ++n) alternate = alternate && a.sense[n] == -a.sense[n - 1];
    check(differ, "each whole round took another route than the one before");
    check(alternate, "each round turned the other way round the map");
    check(a.walked >= 0.75 * a.speed * a.secsMoving,
          fmt("it keeps walking: %.0f units = %.0f %% of the time at %d units/s", a.walked, 100.0 * a.walked / (a.speed * a.secsMoving),
              a.speed));
    bool open = true;
    for (const Action& x : a.sent) {
        if (x.op != 0x0018 || x.body.size() < 8) continue;
        const int ax = x.body[0] | x.body[1] << 8, ay = x.body[2] | x.body[3] << 8, bx = x.body[4] | x.body[5] << 8, by = x.body[6] | x.body[7] << 8;
        if (!g->line_clear(ax, ay, bx, by))
            printf("    leg (%d,%d) -> (%d,%d) crosses a wall (start %s)\n", ax, ay, bx, by, g->blocked(ax, ay) ? "in a wall" : "open");
        open = open && g->line_clear(ax, ay, bx, by);
    }
    check(open, "every leg an open straight line (the grid path)");
    printf("\nanother bot from the same start (its own draws):\n");
    const SweepRun b = sweep(60);
    const size_t common = b.goals.size() > 1 && rounds >= 1 ? std::min(a.goals[1].size(), b.goals[1].size()) : 0;
    check(common >= 5 && !std::equal(a.goals[1].begin(), a.goals[1].begin() + long(common), b.goals[1].begin()),
          fmt("its first round starts another way (%zu goals compared)", common));

    printf("\n%d / %d checks passed\n", pass, total);
}

// ── the monster quest (bot/act_quest.cpp): the data, the switches, then a whole round on the real atlas ──
static void quest_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    auto u16at = [](const Action& a, size_t o) { return o + 1 < a.body.size() ? int(a.body[o] | a.body[o + 1] << 8) : -1; };
    auto args_of = [](const Action& a) {                  // a C2S 0x7F's arguments
        std::vector<uint16_t> v;
        if (a.body.size() < 4) return v;
        for (size_t i = 0; i < a.body[3] && 4 + 2 * i + 1 < a.body.size(); ++i)
            v.push_back(uint16_t(a.body[4 + 2 * i] | a.body[5 + 2 * i] << 8));
        return v;
    };
    auto list = [](const std::vector<uint16_t>& v) { std::string s; for (uint16_t x : v) s += (s.empty() ? "" : " ") + std::to_string(x); return s; };
    const Atlas& at = gd.atlas();
    const int LV = 40;                                      // (StuckRig::login's level)

    printf("\nthe dialog's arguments, as the client sends them (sub_C033A0):\n");
    {
        std::vector<NpcScriptCall> calls;
        Atlas::parse_monq_calls("Well done!\\No\\5\\Yes\\MonQuest_Complete,m2,m3,6,10,8\\*\\*block", 2, calls);
        Atlas::parse_monq_calls("Go?\\Exit\\Exit\\Sure\\(lv)MonQuest_Accept_2,q1,s2,7'", 1, calls);
        check(calls.size() == 2 && calls[0].func == act::npcfn::MonQuest_Complete &&
              calls[0].args == std::vector<uint16_t>{ 258, 259, 6, 10, 8 },
              fmt("MonQuest_Complete,m2,m3,6,10,8 -> func %u [%s]", calls.empty() ? 0 : calls[0].func,
                  calls.empty() ? "" : list(calls[0].args).c_str()));
        check(calls.size() == 2 && calls[1].func == 243 && calls[1].args == std::vector<uint16_t>{ 129, 514, 7 },
              fmt("(lv)MonQuest_Accept_2,q1,s2,7 -> func %u [%s]", calls.size() > 1 ? calls[1].func : 0,
                  calls.size() > 1 ? list(calls[1].args).c_str() : ""));
    }

    printf("\ndata:\n");
    // the quests the bot can run: a giver in the atlas with the take and the hand-in in its dialog
    int doable = 0, QID = 0;
    const AtlasNpc* giver = nullptr;
    for (const MonQuestInfo& q : gd.monster_quests()) {
        for (int i : at.quest_givers(q.id)) {
            const AtlasNpc* n = at.npc(i);
            const int sl = n->quest_slot(q.id);
            if (!n->call(act::npcfn::monq_accept(sl)) || !n->call(act::npcfn::monq_complete(sl))) continue;
            ++doable;
            const int g = q.grade_for(LV);
            if (!QID && g >= 0 && q.level_ok(LV) && q.returnMap == n->zone && !gd.spawns_of(uint32_t(q.grades[size_t(g)].monster)).empty()) {
                QID = q.id;
                giver = n;
            }
            break;
        }
    }
    check(doable > 0, fmt("%d of the %zu monster quests have a giver in the atlas with both actions", doable, gd.monster_quests().size()));
    if (!giver) { printf("no quest for level %d with its giver in its return map\n", LV); return; }
    const MonQuestInfo* qi = gd.monster_quest(QID);
    const MonQuestGrade& gr = qi->grades[size_t(qi->grade_for(LV))];
    const int sl = giver->quest_slot(QID);
    const NpcScriptCall* take = giver->call(act::npcfn::monq_accept(sl));
    const NpcScriptCall* give = giver->call(act::npcfn::monq_complete(sl));
    check(true, fmt("quest %d \"%s\" by %s in %s (slot %d): at level %d kill %d x %s in %d min; take [%s], hand in [%s]", QID,
                    qi->name.c_str(), giver->name.c_str(), at.map_label(giver->zone).c_str(), sl, LV, gr.cnt,
                    gd.mob_name(uint32_t(gr.monster)).c_str(), gr.timeMin, list(take->args).c_str(), list(give->args).c_str()));
    const uint16_t MOB = uint16_t(gr.monster);
    const std::vector<MobSpawn> sp = gd.spawns_of(MOB);
    check(!sp.empty(), fmt("%s lives in %zu maps, the most in %s (%d)", gd.mob_name(MOB).c_str(), sp.size(),
                           at.map_label(sp.front().zone).c_str(), sp.front().count));

    printf("\nthe switches (the Drill and the combat exclude each other, the quest brings the Attack):\n");
    {
        Bot b;
        b.drill.enabled = true;
        b.sync_modes();
        check(b.drill.enabled && !b.attack.enabled && !b.quest.enabled, "Drill on -> Attack and Quest off");
        b.attack.enabled = true;
        b.sync_modes();
        check(b.attack.enabled && !b.drill.enabled, "Attack on -> Drill off");
        b.drill.enabled = true;
        b.sync_modes();
        b.quest.enabled = true;
        b.sync_modes();
        check(b.quest.enabled && b.attack.enabled && !b.drill.enabled, "Quest on -> Attack on, Drill off");
        b.attack.enabled = false;
        b.sync_modes();
        check(!b.quest.enabled && !b.attack.enabled, "Attack off -> Quest off");
        b.load_text("drill.enabled=1\nattack.enabled=1\nquest.enabled=1\n");
        check(b.drill.enabled && !b.attack.enabled && !b.quest.enabled, "a profile with all three on: the Drill keeps its mode");
        b.load_text("quest.enabled=1\nattack.enabled=0\n");
        check(b.quest.enabled && b.attack.enabled, "a profile with the quest on and the Attack off: the Attack comes on");
    }

    // an open spot `d` away from (x, y) with an open line to it, else (x, y)
    auto spot = [&](int zone, int x, int y, int d, int& ox, int& oy) {
        ox = x; oy = y;
        const MapGrid* g = gd.map(zone);
        if (!g) { ox = x + d; return; }
        for (int k = 0; k < 16; ++k) {
            const double a = k * 3.14159265358979 / 8;
            const int tx = x + int(std::cos(a) * d), ty = y + int(std::sin(a) * d);
            if (!g->blocked(tx, ty) && g->line_clear(x, y, tx, ty)) { ox = tx; oy = ty; return; }
        }
    };
    const int HUNT = sp.front().zone != giver->zone ? sp.front().zone : (sp.size() > 1 ? sp[1].zone : sp.front().zone);
    int hx = 600, hy = 600;
    if (const AtlasMap* hm = at.map(HUNT))
        for (const AtlasPoint& p : hm->points)
            if (p.type == 2 || p.type == 1) {
                if (const MapGrid* g = gd.map(HUNT)) if (g->blocked(p.x, p.y)) continue;
                hx = p.x; hy = p.y;
                break;
            }
    printf("\na round: %s gives it, the hunt in %s (arrival (%d,%d)):\n", giver->name.c_str(), at.map_label(HUNT).c_str(), hx, hy);
    StuckRig s(gd);
    const uint16_t NPC = 900;
    int sx = giver->x, sy = giver->y;
    spot(giver->zone, giver->x, giver->y, 64, sx, sy);
    s.login(giver->zone, sx, sy);
    s.npc(NPC, giver->species, giver->x, giver->y);
    for (BotAction* a : s.bot.actions()) a->enabled = false;
    s.bot.quest.enabled = true;
    s.bot.quest.questId = QID;
    s.bot.quest.huntZone = HUNT;
    s.bot.sync_modes();
    check(s.bot.attack.enabled && !s.bot.drill.enabled, "quest on: the Attack is on, the Drill off");
    s.run(0, 3.5);
    s.bot.start(*s.w, s.now);
    Action a;
    bool ok = s.run(0x006F, 3, &a);
    check(ok && u16at(a, 0) == NPC, "talks to the NPC (C2S 0x6F)");
    s.feed(DIR_S2C, 0x0070, Pk().u16(NPC).u16(giver->species).u8(1).str("Welcome\\Exit\\1").u16(0).u16(0).b);
    ok = s.run(0x007F, 2, &a);
    check(ok && a.body.size() >= 4 && a.body[2] == act::npcfn::monq_accept(sl) && args_of(a) == take->args,
          fmt("C2S 0x7F MonQuest_Accept (func %u) [%s]", act::npcfn::monq_accept(sl), ok ? list(args_of(a)).c_str() : "-"));
    // the server: 210, the timer, the kills, the NPC's message
    s.feed(DIR_S2C, 0x00D2, Pk().u16(uint16_t(QID)).u16(MOB).u8(uint8_t(qi->grade_for(LV))).u8(1).u16(0).u32(0).str(qi->name.c_str()).str("Hunter Yuri")
                                .str("Megalopolis").str("%s").b);
    s.timerLeft = 1800000;
    s.timerAt = s.now;
    s.feed(DIR_S2C, 0x00D7, Pk().u32(1800000).u32(1800000).b);
    s.feed(DIR_S2C, 0x00D9, Pk().u16(0).u16(uint16_t(gr.cnt)).b);
    s.feed(DIR_S2C, 0x0073, Pk().u16(NPC).u16(take->args.empty() ? 1 : take->args[0]).str("Go!").u16(0).u16(uint16_t(QID)).b);
    check(s.w->monq.has && s.w->monq.qid == QID && s.w->monq.target == gr.cnt && !s.w->monq.success,
          fmt("the model: quest %d taken, 0 / %d", QID, gr.cnt));
    ok = s.run(0x007F, 2, &a);
    check(ok && a.body.size() >= 4 && a.body[2] == act::npcfn::Exit && a.body[3] == 0, "then Exit (func 1: S2C 117 closes the window)");
    s.feed(DIR_S2C, 0x0075, {});
    // its monster right here, out of the hunt map: not fought
    int px = sx, py = sy;
    spot(giver->zone, sx, sy, 80, px, py);
    s.monster(950, px, py, "Quest Mob", MOB);
    size_t mark = s.sent.size();
    s.run(0, 3);
    bool fought = false;
    for (size_t i = mark; i < s.sent.size(); ++i) fought |= s.sent[i].op == 0x001D;
    check(!fought, fmt("out of the hunt map: its monster next to us is left alone (%zu requests, plan: %s)", s.sent.size() - mark,
                       s.bot.quest.plan.c_str()));
    // in the hunt map: its monster, not the other one nearer
    s.enter_zone(HUNT, hx, hy);
    int ax = hx, ay = hy, bx = hx, by = hy;
    spot(HUNT, hx, hy, 160, ax, ay);
    spot(HUNT, hx, hy, 60, bx, by);
    s.monster(951, ax, ay, "Quest Mob", MOB);
    s.monster(952, bx, by, "Other", uint16_t(MOB == 2010 ? 2011 : 2010));
    ok = s.run(0x001D, 6, &a);
    check(ok && u16at(a, 0) == 951, fmt("in %s: 0x1D at its monster (951), not the nearer other one (got %d)",
                                        gd.map_name(HUNT).c_str(), ok ? u16at(a, 0) : -1));
    // the kills done: the portal back
    s.feed(DIR_S2C, 0x00D9, Pk().u16(uint16_t(gr.cnt)).u16(uint16_t(gr.cnt)).b);
    s.feed(DIR_S2C, 0x00DA, Pk().u16(uint16_t(QID)).u8(1).b);
    bool early = false;
    for (const Action& x : s.sent) early |= x.op == 0x007F && x.body.size() > 2 && x.body[2] == act::npcfn::monq_complete(sl);
    check(!early, "no MonQuest_Complete before the 218 (the server would drop us)");
    ok = s.run(0x0410, 3, &a);
    check(ok && a.body.size() == 4 && a.body[0] == 2, "all killed (218): C2S 0x410 [u32 2], the portal back");
    s.feed(DIR_S2C, 0x0411, {});
    s.now += 500 * 10000ull;
    s.enter_zone(giver->zone, sx, sy);                    // (the relocate: here next to the NPC)
    s.npc(NPC + 1, giver->species, giver->x, giver->y);
    ok = s.run(0x006F, 4, &a);
    check(ok && u16at(a, 0) == NPC + 1, "back in its map: talks to the NPC");
    s.feed(DIR_S2C, 0x0070, Pk().u16(NPC + 1).u16(giver->species).u8(1).str("Welcome\\Exit\\1").u16(0).u16(0).b);
    ok = s.run(0x007F, 2, &a);
    check(ok && a.body.size() >= 4 && a.body[2] == act::npcfn::monq_complete(sl) && args_of(a) == give->args,
          fmt("C2S 0x7F MonQuest_Complete (func %u) [%s]", act::npcfn::monq_complete(sl), ok ? list(args_of(a)).c_str() : "-"));
    s.feed(DIR_S2C, 0x00DB, Pk().u16(0).u16(0).u32(1600).u32(0).u16(uint16_t(QID)).u32(500).b);
    s.feed(DIR_S2C, 0x0073, Pk().u16(NPC + 1).u16(give->args.empty() ? 1 : give->args[0]).str("Thanks").u16(0).u16(uint16_t(QID)).b);
    s.feed(DIR_S2C, 0x00D3, Pk().u16(uint16_t(QID)).b);
    s.timerLeft = -1;
    ok = s.run(0x007F, 2, &a);
    check(ok && a.body[2] == act::npcfn::Exit, "then Exit");
    s.feed(DIR_S2C, 0x0075, {});
    check(s.bot.quest.runs == 1 && s.bot.quest.expGained == 1600 && s.bot.quest.gelderGained == 500,
          fmt("handed in: run %d, %lld EXP, %lld galder", s.bot.quest.runs, (long long)s.bot.quest.expGained,
              (long long)s.bot.quest.gelderGained));
    ok = s.run(0x006F, 3, &a);
    s.feed(DIR_S2C, 0x0070, Pk().u16(NPC + 1).u16(giver->species).u8(1).str("Welcome\\Exit\\1").u16(0).u16(0).b);
    ok = ok && s.run(0x007F, 2, &a);
    check(ok && a.body[2] == act::npcfn::monq_accept(sl), "and takes it again");

    // in the hunt map with the quest running: a rig, its quest on (210 + the timer + 217), the Roam off
    auto hunt_rig = [&](StuckRig& h) {
        h.login(HUNT, hx, hy);
        for (BotAction* x : h.bot.actions()) x->enabled = false;
        h.bot.quest.enabled = true;
        h.bot.quest.questId = QID;
        h.bot.quest.huntZone = HUNT;
        h.bot.sync_modes();
        h.feed(DIR_S2C, 0x00D2, Pk().u16(uint16_t(QID)).u16(MOB).u8(uint8_t(qi->grade_for(LV))).u8(1).u16(0).u32(0).str("q")
                                    .str("npc").str("pos").str("%s").b);
        h.timerLeft = 1800000;
        h.timerAt = h.now;
        h.feed(DIR_S2C, 0x00D7, Pk().u32(1800000).u32(1800000).b);
        h.feed(DIR_S2C, 0x00D9, Pk().u16(0).u16(uint16_t(gr.cnt)).b);
        h.run(0, 3.5);
        h.bot.start(*h.w, h.now);
    };
    printf("\nin the hunt map, its monster far away (beyond the Attack's radius): taken at once, no search:\n");
    {
        StuckRig h(gd);
        hunt_rig(h);
        int fx = -1, fy = -1;
        if (const MapGrid* g = gd.map(HUNT)) {
            std::vector<int32_t> f;
            Atlas::walk_field(*g, hx, hy, f);
            double best = 1e18;
            for (int cy = 0; cy < g->h; ++cy)
                for (int cx = 0; cx < g->w; ++cx) {
                    if (!g->open_cell(cx, cy)) continue;
                    const double wd = Atlas::field_walk(*g, f, cx * 16 + 8, cy * 16 + 8);
                    if (wd >= 900 && wd < best) { best = wd; fx = cx * 16 + 8; fy = cy * 16 + 8; }
                }
        }
        if (fx < 0) { fx = hx + 900; fy = hy; }
        h.monster(960, fx, fy, "Far Mob", MOB);
        const size_t mark = h.sent.size();
        h.run(0, 1.5);
        bool toward = false;
        for (size_t i = mark; i < h.sent.size(); ++i) toward |= h.sent[i].op == 0x001D || h.sent[i].op == 0x0018;
        check(h.bot.attack.target == 960 && toward,
              fmt("%.0f units off (radius %d): the Attack's target at once, %zu request(s) toward it", std::hypot(double(fx - hx), double(fy - hy)),
                  h.bot.attack.radius, h.sent.size() - mark));
    }
    printf("\nnone of it to take: the patrol of its spawn spots, rounds in a changing order, a pause at each:\n");
    {
        StuckRig h(gd);
        hunt_rig(h);
        std::vector<std::pair<int, int>> pts;
        for (const MobSpawn& sp : gd.spawns_of(MOB))
            if (sp.zone == HUNT) pts = at.regen_points(HUNT, sp.regions);
        const RoamAction& r = h.bot.roam;
        const MapGrid* hg = gd.map(HUNT);
        std::vector<std::vector<int>> order;                // [round] -> the spots paused at, in order
        std::vector<std::set<int>> visits;                    // [round] -> the spots we came within 64 units of
        std::vector<int> sense;
        std::vector<double> pauses;                         // each pause (s)
        bool was = false;
        uint64_t pauseAt = 0;
        const size_t mark = h.sent.size();
        const int secs = 600;
        for (int k = 0; k < secs * 10; ++k) {
            h.run(0, 0.1);
            const size_t n = size_t(r.patrolRound.n);
            if (!n) continue;
            if (order.size() <= n) { order.resize(n + 1); visits.resize(n + 1); sense.resize(n + 1, 0); }
            sense[n] = r.patrolRound.sense;
            int px = 0, py = 0;
            h.w->self_pos_at(h.now, px, py);
            for (size_t i = 0; i < r.spots.size(); ++i)
                if (std::hypot(double(r.spots[i].x - px), double(r.spots[i].y - py)) <= 64.0) visits[n].insert(int(i));
            const bool paused = r.spotPaused();
            if (paused && !was) { order[n].push_back(r.spotGoal()); pauseAt = h.now; }
            if (!paused && was) pauses.push_back(double(h.now - pauseAt) / 1e7);
            was = paused;
        }
        const size_t rounds = order.empty() ? 0 : order.size() - 1;     // started (the last one may be cut short)
        for (size_t i = 0; i < r.spots.size(); ++i)
            printf("  spot %zu at (%d,%d): %d points\n", i + 1, r.spots[i].x, r.spots[i].y, r.spots[i].points);
        for (size_t n = 1; n < order.size(); ++n) {
            std::string o;
            for (int i : order[n]) o += fmt(" %d", i + 1);
            printf("  round %zu %s: paused at%s; %zu spots visited\n", n, sense[n] > 0 ? "clockwise" : "counter-clockwise", o.c_str(),
                   visits[n].size());
        }
        check(r.spots.size() >= 3 && r.spotPoints > 0,
              fmt("%zu regeneration points of %s in %s, %d within reach: %zu spots", pts.size(), gd.mob_name(MOB).c_str(),
                  gd.map_name(HUNT).c_str(), r.spotPoints, r.spots.size()));
        bool all = rounds >= 3;
        for (size_t n = 1; n < rounds; ++n) all = all && visits[n].size() == r.spots.size();     // (the whole rounds)
        check(all, fmt("%zu rounds in %d s, every spot visited in each whole round", rounds, secs));
        bool differ = rounds >= 3, alternate = rounds >= 2;
        for (size_t n = 2; n < rounds; ++n) differ = differ && order[n] != order[n - 1];
        for (size_t n = 2; n <= rounds; ++n) alternate = alternate && sense[n] == -sense[n - 1];
        check(differ, "each whole round in another order than the one before");
        check(alternate, "each round turned the other way round");
        double lo = 1e9, hi = 0;
        for (double p : pauses) { lo = std::min(lo, p); hi = std::max(hi, p); }
        check(pauses.size() >= r.spots.size() && lo >= 0.6 * r.spotPause - 0.15 && hi <= 1.4 * r.spotPause + 0.35,
              fmt("%zu pauses at the spots, %.1f..%.1f s (about %d s)", pauses.size(), pauses.empty() ? 0.0 : lo, hi, r.spotPause));
        bool open = true, onlyLegs = h.sent.size() > mark;
        for (size_t i = mark; i < h.sent.size(); ++i) {
            const Action& x = h.sent[i];
            if (x.op != 0x0018) { onlyLegs = false; continue; }
            if (x.body.size() < 8 || !hg) continue;
            const int ax = x.body[0] | x.body[1] << 8, ay = x.body[2] | x.body[3] << 8, bx = x.body[4] | x.body[5] << 8, by = x.body[6] | x.body[7] << 8;
            open = open && hg->line_clear(ax, ay, bx, by);
        }
        check(open && onlyLegs, fmt("%zu requests, all walking legs (C2S 0x18), each an open straight line", h.sent.size() - mark));
    }

    printf("\nthe login list (235): a quest whose kills were done before the relog:\n");
    {
        StuckRig r(gd);
        r.login(giver->zone, sx, sy);
        r.feed(DIR_S2C, 0x00EB, Pk().u16(1).u16(uint16_t(QID)).u16(MOB).u8(3).u8(0).u16(uint16_t(gr.cnt)).u32(654321).b);
        check(r.w->monq.has && r.w->monq.success && r.w->monq.kills == gr.cnt, "235 with a record: done, to be handed in");
        r.feed(DIR_S2C, 0x00EB, Pk().u16(0).b);
        check(!r.w->monq.has, "235 empty: none");
    }

    printf("\n%d / %d checks passed\n", pass, total);
}

// ── the item hunt (item_sources.cpp + bot/act_hunt.cpp): the drop and dig tables, the expected drops per kill
//    against the server's own rolls, an item's sources, then a hunt ──
static void hunt_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const ItemSources& src = gd.item_sources();
    printf("data: %zu drop lists, %zu sub-tables, %zu maps in MapItem_All\n", src.lists(), src.tables(), src.dig_maps());
    check(src.lists() > 1000 && src.tables() > 100, "the drop tables loaded");
    // Sea Scorpion 2000: list 1 (Scorpion Jerky 10000 at 0.4, the rates sum 0.9), ItemDropCnt 1
    const double jerky = src.per_kill(2000, 10000);
    // (its Cnt: 1 in the server yml, 3 in the client's paula_libconfig.xml)
    const double perStack = jerky / 0.4;
    check(jerky >= 0.3995 && std::fabs(perStack - std::round(perStack)) < 0.002,
          fmt("Sea Scorpion: Scorpion Jerky %.4f a kill (its band 0..3999 of one roll, x %.0f a drop)", jerky, std::round(perStack)));

    // the expected drops (expect_kill) against many kills rolled as drop.cpp rolls them
    printf("\nper kill, worked out vs 200000 kills rolled as the server does:\n");
    std::vector<uint32_t> picks;
    for (const auto& kv : gd.mob_table()) {
        const MobInfo& m = kv.second;
        if (m.itemDropCnt >= 2 && src.drops(kv.first) && picks.size() < 4) picks.push_back(kv.first);
    }
    for (const auto& kv : gd.mob_table())
        if (kv.second.itemDropCnt == 1 && src.drops(kv.first) && picks.size() < 6) picks.push_back(kv.first);
    std::sort(picks.begin(), picks.end());
    std::mt19937 rng(12345);
    for (uint32_t mob : picks) {
        const int N = 200000;
        std::map<uint32_t, double> sum;
        std::vector<std::pair<uint32_t, int>> loot;
        for (int i = 0; i < N; ++i) {
            src.simulate_kill(mob, rng, loot);
            for (const auto& l : loot) sum[l.first] += l.second;
        }
        const auto* d = src.drops(mob);
        int bad = 0, n = 0;
        double worst = 0;
        for (const auto& p : *d) {
            const double mc = sum[p.first] / N;
            const double tol = 6.0 * std::sqrt(std::max(p.second, 1e-4) * 4.0 / N) + 0.002 * p.second;
            worst = std::max(worst, std::fabs(mc - p.second) / tol);
            bad += std::fabs(mc - p.second) > tol;
            ++n;
            sum.erase(p.first);
        }
        for (const auto& kv : sum) bad += kv.second > 0;    // (rolled but not worked out)
        const MobInfo* m = gd.mob(mob);
        check(bad == 0, fmt("%s (#%u, %d rolls, list %d): %d items, worst %.2f of the tolerance%s", m->name.c_str(), mob,
                            m->itemDropCnt, m->itemDropIdx, n, worst, d->empty() ? "" :
                            fmt(", top %s %.4f", gd.item_name(d->front().first).c_str(), d->front().second).c_str()));
    }

    // the buried items: per map the shares of a find sum to at most 1 (a type with no items finds nothing)
    printf("\nburied items:\n");
    int maps = 0, over = 0;
    for (int z = 1; z < 2000; ++z) {
        const auto* b = src.buried(z);
        if (!b) continue;
        ++maps;
        double s = 0;
        for (const auto& p : *b) s += p.second;
        over += s > 1.0 + 1e-9;
    }
    check(maps > 100 && over == 0, fmt("%d maps with buried items, the shares of a find sum to <= 1 in each (%d over)", maps, over));
    if (const auto* b = src.buried(2)) {
        std::string top;
        for (size_t i = 0; i < std::min<size_t>(3, b->size()); ++i)
            top += fmt(" %s %.3f;", gd.item_name((*b)[i].first).c_str(), (*b)[i].second);
        printf("  %s: %zu items, the most a find:%s\n", gd.map_name(2).c_str(), b->size(), top.c_str());
    }

    // an open point of a map (its portals' / spawn points first), else (600, 600)
    auto open_point = [&](int zone, int& x, int& y) {
        x = 600; y = 600;
        const AtlasMap* am = gd.atlas().map(zone);
        const MapGrid* g = gd.map(zone);
        if (am)
            for (const AtlasPoint& p : am->points)
                if ((p.type == 2 || p.type == 1) && (!g || !g->blocked(p.x, p.y))) { x = p.x; y = p.y; return; }
    };

    // ── the sources for a level-40 character: a common item a monster of its level drops, best by killing ──
    printf("\nthe sources (level 40, no drill):\n");
    StuckRig probe(gd);
    probe.login(2, 600, 600);
    uint32_t ITEM = 0;
    HuntSource KS;
    std::vector<std::pair<uint32_t, MobInfo>> mobsById(gd.mob_table().begin(), gd.mob_table().end());
    std::sort(mobsById.begin(), mobsById.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& kv : mobsById) {
        if (ITEM) break;
        if (kv.second.level < 20 || kv.second.level > 40 || gd.spawns_of(kv.first).empty()) continue;
        const auto* d = src.drops(kv.first);
        if (!d) continue;
        for (const auto& p : *d) {
            const ItemInfo* ii = gd.item(p.first);
            if (!ii || ii->cls != 1 || p.second < 0.05 || TownAction::protected_item(*ii)) continue;   // (not a potion: never sold)
            const std::vector<HuntSource> ss = ItemHuntAction::sources(*probe.w, p.first);
            if (ss.empty() || ss.front().kind != HuntSource::Kill || !ss.front().usable || !gd.map(ss.front().zone)) continue;
            ITEM = p.first;
            KS = ss.front();
            break;
        }
    }
    check(ITEM != 0, ITEM ? fmt("%s: best = %s, %.3f a kill, ~%.1f / h", gd.item_name(ITEM).c_str(), KS.label.c_str(), KS.perUnit,
                                KS.perHour) : std::string("no common item found"));
    if (!ITEM) { printf("\n%d / %d checks passed\n", pass, total); return; }
    // a buried item the Blue Drill (8200, reaches 80) gets: unusable without a drill, usable with it
    uint32_t DIG = 0;
    int DZ = 0;
    for (int z = 1; z < 2000 && !DIG; ++z) {
        const auto* b = src.buried(z);
        const MapDigInfo* md = gd.map_dig(z);
        if (!b || !md || md->maxDepth > 80 || !gd.map(z)) continue;
        for (const auto& p : *b)
            if (p.second >= 0.02) { DIG = p.first; DZ = z; break; }
    }
    {
        const std::vector<HuntSource> no = ItemHuntAction::sources(*probe.w, DIG);
        const HuntSource* d0 = nullptr;
        for (const HuntSource& x : no) if (x.kind == HuntSource::Dig && x.zone == DZ) d0 = &x;
        probe.feed(DIR_S2C, 0x00E7, Pk().u16(1).u8(2).u32(8200).u64(0xD1).u16(3).u32(0).u8(0).b);   // a Blue Drill in the bag
        const std::vector<HuntSource> yes = ItemHuntAction::sources(*probe.w, DIG);
        const HuntSource* d1 = nullptr;
        for (const HuntSource& x : yes) if (x.kind == HuntSource::Dig && x.zone == DZ) d1 = &x;
        check(DIG && d0 && !d0->usable && d1 && d1->usable,
              fmt("%s buried in %s: without a drill \"%s\", with the Blue Drill usable (%.1f %% of a find, ~%.1f / h)",
                  gd.item_name(DIG).c_str(), gd.map_name(DZ).c_str(), d0 ? d0->why.c_str() : "-", d1 ? d1->perUnit * 100 : 0.0,
                  d1 ? d1->perHour : 0.0));
    }

    printf("\nthe switches:\n");
    {
        Bot b;
        b.hunt.enabled = true;
        b.sync_modes();
        check(b.hunt.enabled && b.attack.enabled && !b.drill.enabled && !b.quest.enabled, "Item Hunt on -> Attack on, Drill / Quest off");
        b.quest.enabled = true;
        b.sync_modes();
        check(b.quest.enabled && !b.hunt.enabled, "Quest on -> Item Hunt off");
        b.quest.enabled = false;
        b.hunt.enabled = true;
        b.sync_modes();
        b.drill.enabled = true;
        b.sync_modes();
        check(!b.hunt.enabled && !b.attack.enabled, "Drill on -> Item Hunt and Attack off");
        b.hunt.list = { HuntItem{ 100, 5, -1, 0 }, HuntItem{ 200, 3, 1, 7 } };
        b.hunt.switchMin = 15;
        b.hunt.stopWhenDone = false;
        Bot b2;
        b2.load_text(b.save_text());
        check(b2.hunt.list.size() == 2 && b2.hunt.list[0].want == 5 && b2.hunt.list[1].srcKind == 1 && b2.hunt.list[1].srcZone == 7 &&
                  b2.hunt.switchMin == 15 && !b2.hunt.stopWhenDone, "the list and the options through a profile");
        TownAction t;
        t.sellList.mode = IdFilter::AllButBlack;
        t.storeList.mode = IdFilter::AllButBlack;
        InvItem it;
        it.itemId = ITEM;
        it.kind = 1;
        it.count = 5;
        const bool before = t.sellable(*probe.w, it);
        t.keep.insert(ITEM);
        check(before && !t.sellable(*probe.w, it) && !t.storable(*probe.w, it), "a hunted item is never sold or stored");
    }

    // ── a kill hunt: in the source's map, the hunted species (not the nearer other one), its drop picked up with the
    //    Loot OFF, then done -> the bot stops ──
    printf("\na kill hunt in %s:\n", gd.map_name(KS.zone).c_str());
    {
        StuckRig k(gd);
        int sx, sy;
        open_point(KS.zone, sx, sy);
        k.login(KS.zone, sx, sy);
        for (BotAction* a : k.bot.actions()) a->enabled = false;
        k.bot.hunt.enabled = true;
        k.bot.hunt.list = { HuntItem{ ITEM, 1, -1, 0 } };
        k.bot.sync_modes();
        k.run(0, 3.5);
        k.bot.start(*k.w, k.now);
        const MapGrid* g = gd.map(KS.zone);
        auto spot = [&](int d, int& ox, int& oy) {
            ox = sx + d; oy = sy;
            for (int t = 0; t < 16 && g; ++t) {
                const double a = t * 3.14159265358979 / 8;
                const int tx = sx + int(std::cos(a) * d), ty = sy + int(std::sin(a) * d);
                if (!g->blocked(tx, ty) && g->line_clear(sx, sy, tx, ty)) { ox = tx; oy = ty; return; }
            }
        };
        int ox, oy, px, py;
        spot(60, ox, oy);
        spot(160, px, py);
        uint16_t other = 2000;
        while (std::find(KS.mobs.begin(), KS.mobs.end(), other) != KS.mobs.end()) ++other;
        k.monster(950, ox, oy, "Other", other);
        k.monster(951, px, py, "Prey", KS.mobs.front());
        Action a;
        const bool hit = k.run(0x001D, 3, &a);
        check(hit && (a.body[0] | a.body[1] << 8) == 951, fmt("0x1D at #%d (%s), not the nearer %s", hit ? (a.body[0] | a.body[1] << 8) : 0,
                                                                 gd.mob_name(KS.mobs.front()).c_str(), gd.mob_name(other).c_str()));
        k.feed(DIR_S2C, 0x016D, Pk().u16(951).u16(StuckRig::ME).b);
        k.feed(DIR_S2C, 0x002B, Pk().u16(951).u8(0).b);
        k.feed(DIR_S2C, 0x000C, Pk().u16(951).u16(960).u16(uint16_t(px)).u16(uint16_t(py)).u16(0).u8(0).u8(0).u8(0).u8(1)
                                    .u32(ITEM).u64(0).u16(1).b);
        const bool picked = k.run(0x0021, 4, &a);
        check(picked && (a.body[0] | a.body[1] << 8) == 960, "its drop picked up with the Loot off (0x21 #960)");
        k.feed(DIR_S2C, 0x0022, Pk().u16(StuckRig::ME).u16(960).u8(1).b);
        k.feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(ITEM).u64(0x777).u16(1).u8(0).b);
        k.run(0, 1.5);
        check(!k.bot.running && k.logged("item hunt done") && k.bot.stopFinished, fmt("1 / 1 in the bag: the bot stopped (\"%s\")", k.bot.doing.c_str()));
    }

    // ── smart: a second item B a second species of the same map drops (its best place there) -> one map for both,
    //    killing both species; the first target the nearer B species although B is second in the list; B done -> its
    //    species is left ──
    printf("\nsmart: the whole list in %s:\n", gd.map_name(KS.zone).c_str());
    {
        uint32_t B = 0;
        uint16_t S2 = 0;
        for (const auto& kv : mobsById) {
            if (B) break;
            const uint16_t m = uint16_t(kv.first);
            if (std::find(KS.mobs.begin(), KS.mobs.end(), m) != KS.mobs.end() || kv.second.level > 45) continue;
            bool there = false;
            for (const MobSpawn& sp : gd.spawns_of(m)) there = there || sp.zone == KS.zone;
            const auto* d = src.drops(m);
            if (!there || !d) continue;
            for (const auto& p : *d) {
                const ItemInfo* ii = gd.item(p.first);
                if (!ii || ii->cls != 1 || p.second < 0.03 || p.first == ITEM) continue;
                bool byA = false;
                for (uint16_t a : KS.mobs) byA = byA || src.per_kill(a, p.first) > 0;
                const std::vector<HuntSource> ss = ItemHuntAction::sources(*probe.w, p.first);
                if (byA || ss.empty() || ss.front().kind != HuntSource::Kill || ss.front().zone != KS.zone) continue;
                B = p.first;
                S2 = m;
                break;
            }
        }
        check(B != 0, B ? fmt("B = %s, dropped there by %s (%.3f a kill)", gd.item_name(B).c_str(), gd.mob_name(S2).c_str(),
                              src.per_kill(S2, B)) : std::string("no second item of that map found"));
        if (B) {
            ItemHuntAction plan;
            plan.list = { HuntItem{ ITEM, 20, -1, 0 }, HuntItem{ B, 3, -1, 0 } };
            const auto t0 = std::chrono::steady_clock::now();
            const HuntPlan pl = plan.make_plan(*probe.w);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const HuntPlan::Cand* top = pl.cands.empty() ? nullptr : &pl.cands.front();
            auto has = [](const std::vector<uint16_t>& v, uint16_t x) { return std::find(v.begin(), v.end(), x) != v.end(); };
            if (top) printf("  plan (%.1f ms, %zu places): %s, %zu items in %.2f h, the list %.2f h; %zu route step(s)\n", ms,
                            pl.cands.size(), top->src.label.c_str(), top->covered.size(), top->farmHours, top->totalHours, pl.route.size());
            check(top && top->src.zone == KS.zone && top->rate.count(ITEM) && top->rate.count(B) && has(top->src.mobs, S2) &&
                      has(top->src.mobs, KS.mobs.front()),
                  "the plan: that map for both items, killing both species");
            check(top && pl.route.size() == 1 && pl.route[0].done.size() == 2, "the route: one stop, both done there");

            StuckRig k(gd);
            int sx, sy;
            open_point(KS.zone, sx, sy);
            k.login(KS.zone, sx, sy);
            for (BotAction* a : k.bot.actions()) a->enabled = false;
            k.bot.hunt.enabled = true;
            k.bot.hunt.smart = true;
            k.bot.hunt.list = { HuntItem{ ITEM, 20, -1, 0 }, HuntItem{ B, 1, -1, 0 } };
            k.bot.sync_modes();
            k.run(0, 3.5);
            k.bot.start(*k.w, k.now);
            const MapGrid* g = gd.map(KS.zone);
            auto spot = [&](int d, int& ox, int& oy) {
                ox = sx + d; oy = sy;
                for (int t = 0; t < 16 && g; ++t) {
                    const double a = t * 3.14159265358979 / 8;
                    const int tx = sx + int(std::cos(a) * d), ty = sy + int(std::sin(a) * d);
                    if (!g->blocked(tx, ty) && g->line_clear(sx, sy, tx, ty)) { ox = tx; oy = ty; return; }
                }
            };
            int ox, oy, bx, by, ax, ay;
            spot(50, ox, oy);
            spot(110, bx, by);
            spot(220, ax, ay);
            uint16_t other = 2000;
            while (has(KS.mobs, other) || other == S2) ++other;
            k.monster(970, ox, oy, "Other", other);
            k.monster(971, bx, by, "B's", S2);
            k.monster(972, ax, ay, "A's", KS.mobs.front());
            Action a;
            const bool hit = k.run(0x001D, 3, &a);
            const int t1 = hit ? (a.body[0] | a.body[1] << 8) : 0;
            check(t1 == 971, fmt("first 0x1D at #%d: %s (B's species, B second in the list), not the nearer %s", t1,
                                 gd.mob_name(S2).c_str(), gd.mob_name(other).c_str()));
            // B in the bag: its species is left, the next target is A's
            k.feed(DIR_S2C, 0x002B, Pk().u16(971).u8(0).b);
            k.feed(DIR_S2C, 0x000F, Pk().u16(971).u8(0).b);
            k.feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(B).u64(0x778).u16(1).u8(0).b);
            k.monster(973, bx, by, "B's again", S2);
            const bool hit2 = k.run(0x001D, 4, &a);
            const int t2 = hit2 ? (a.body[0] | a.body[1] << 8) : 0;
            check(t2 == 972, fmt("B done: next 0x1D at #%d (%s), the new %s nearby left alone", t2,
                                 gd.mob_name(KS.mobs.front()).c_str(), gd.mob_name(S2).c_str()));
        }
    }

    // ── smart on a long list: 8 items of monsters of our level from different maps (+ a buried one, with the drill) ──
    printf("\nsmart: a list of 9 items:\n");
    {
        ItemHuntAction plan;
        std::set<uint32_t> picked;
        for (const auto& kv : mobsById) {
            if (plan.list.size() >= 8) break;
            if (kv.second.level < 25 || kv.second.level > 45 || gd.spawns_of(kv.first).empty()) continue;
            const auto* d = src.drops(kv.first);
            if (!d) continue;
            for (const auto& p : *d) {
                const ItemInfo* ii = gd.item(p.first);
                if (!ii || ii->cls != 1 || p.second < 0.02 || picked.count(p.first)) continue;
                picked.insert(p.first);
                plan.list.push_back(HuntItem{ p.first, 5 + int(plan.list.size()) * 3, -1, 0 });
                break;
            }
        }
        plan.list.push_back(HuntItem{ DIG, 4, -1, 0 });     // (probe has the Blue Drill)
        const auto t0 = std::chrono::steady_clock::now();
        const HuntPlan pl = plan.make_plan(*probe.w);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double oneByOne = 0.0, routeH = 0.0;
        size_t solvable = 0, routeDone = 0;
        for (const auto& b : pl.best)
            if (b.second > 0.0) { ++solvable; oneByOne += 0.1 + pl.need.at(b.first) / b.second; }
        bool stepsOk = true;
        std::set<uint32_t> doneSet;
        for (const HuntPlan::Step& st : pl.route) {
            routeH += st.hours;
            stepsOk = stepsOk && !st.done.empty();
            for (uint32_t i : st.done) doneSet.insert(i);
        }
        routeDone = doneSet.size();
        bool sorted = true;
        for (size_t i = 1; i < pl.cands.size(); ++i) sorted = sorted && pl.cands[i - 1].totalHours <= pl.cands[i].totalHours + 1e-9;
        for (size_t k = 0; k < pl.route.size(); ++k) {
            std::string dn;
            for (uint32_t i : pl.route[k].done) dn += (dn.empty() ? "" : ", ") + gd.item_name(i);
            printf("  %zu. %s  -> %s  (%.2f h)\n", k + 1, pl.cands[size_t(pl.route[k].cand)].src.label.c_str(), dn.c_str(),
                   pl.route[k].hours);
        }
        check(ms < 500.0, fmt("planned in %.1f ms (%zu places for %zu items)", ms, pl.cands.size(), pl.need.size()));
        check(sorted && stepsOk && routeDone == solvable, fmt("the route does every item with a source (%zu of %zu), %zu stop(s)",
                                                              routeDone, solvable, pl.route.size()));
        check(!pl.cands.empty() && pl.cands.front().totalHours <= oneByOne + 1e-6 && routeH <= oneByOne + 1e-6,
              fmt("never slower than one by one: the plan %.2f h (route %.2f h) vs %.2f h", pl.cands.empty() ? 0.0 : pl.cands.front().totalHours,
                  routeH, oneByOne));
    }

    // ── a dig hunt: in the map, the Drill works (a drill put on / a probe), nothing fought ──
    printf("\na dig hunt in %s:\n", gd.map_name(DZ).c_str());
    {
        StuckRig d(gd);
        int sx, sy;
        open_point(DZ, sx, sy);
        d.login(DZ, sx, sy);
        d.feed(DIR_S2C, 0x00E7, Pk().u16(1).u8(2).u32(8200).u64(0xD1).u16(3).u32(0).u8(0).b);
        for (BotAction* a : d.bot.actions()) a->enabled = false;
        d.bot.hunt.enabled = true;
        d.bot.hunt.list = { HuntItem{ DIG, 1, HuntSource::Dig, DZ } };
        d.bot.sync_modes();
        d.run(0, 3.5);
        d.bot.start(*d.w, d.now);
        const size_t mark = d.sent.size();
        Action a;
        const bool equip = d.run(0x0051, 4, &a);
        d.feed(DIR_S2C, 0x0052, Pk().u64(0xD1).u8(7).b);       // (the server's answer: on, slot 7)
        const bool probe2 = d.run(0x002C, 6, &a);
        bool fight = false;
        for (size_t i = mark; i < d.sent.size(); ++i) fight |= d.sent[i].op == 0x001D;
        check(equip && probe2 && !fight && d.bot.hunt.digging(),
              fmt("the Drill works there: the Blue Drill put on (0x51), then a probe 0x2C (%zu requests, nothing fought)",
                  d.sent.size() - mark));
    }

    // the live log of 2026-10-24 14:14: Brass, Glass Piece, Magnetite, Magic Powder — "no usable source", nothing said why.
    // They drop only from Lv 184-196 species of the Vamp Dungeons 2-5 (no way in below level 135). Now the stop says it
    // per item, and the "levels over yours" limit is a setting.
    {
        printf("\nno usable source: the stop says why (Brass, Glass Piece, Magnetite, Magic Powder at level 40):\n");
        StuckRig r(gd);
        r.login(1, 1100, 1300, 40);
        for (BotAction* a : r.bot.actions()) a->enabled = false;
        r.bot.hunt.enabled = true;
        r.bot.hunt.smart = true;
        for (uint32_t it : { 11621u, 11622u, 11623u, 11624u }) { HuntItem hi; hi.item = it; hi.want = 3; r.bot.hunt.list.push_back(hi); }
        r.bot.sync_modes();
        r.run(0, 3.5);
        r.bot.start(*r.w, r.now);
        r.run(0, 3);
        printf("    %s\n", r.bot.stopWhy.c_str());
        check(!r.bot.running && r.bot.stopWhy.find("Brass Knight") != std::string::npos &&
                  r.bot.stopWhy.find("over yours") != std::string::npos && r.bot.stopWhy.find("no way there now") != std::string::npos,
              "the stop names each item's monster, its level over yours and that no way gets there now");
        ItemHuntAction h2;
        std::ostringstream o;
        h2.maxOver = 160;
        h2.save(o);
        ItemHuntAction h3;
        std::istringstream in(o.str());
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('='), dot = line.find('.');
            if (eq != std::string::npos && dot != std::string::npos) h3.load(line.substr(dot + 1, eq - dot - 1), line.substr(eq + 1));
        }
        check(h3.maxOver == 160, "the levels-over limit goes through the profile (hunt.maxOver)");
    }

    printf("\n%d / %d checks passed\n", pass, total);
}

static void layouts_selftest(const Config& cfg)
{
    const size_t cut = cfg.tricksterPath.find_last_of("\\/");
    const std::string dir = cut == std::string::npos ? std::string() : cfg.tricksterPath.substr(0, cut + 1);
    int pass = 0, total = 0;
    for (const char* name : { "Trickster.bin", "TricksterS2.exe" }) {
        const std::string path = dir + name;
        HMODULE h = LoadLibraryExA(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        if (!h) { printf("%s: cannot map it (%lu)\n", path.c_str(), GetLastError()); continue; }
        const uint8_t* base = (const uint8_t*)((uintptr_t)h & ~(uintptr_t)3);
        const auto* dos = (const IMAGE_DOS_HEADER*)base;
        const auto* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        const uint32_t size = nt->OptionalHeader.SizeOfImage, imageBase = nt->OptionalHeader.ImageBase;
        printf("%s: image base %08X, size %08X\n", name, imageBase, size);
        int matched = 0;
        bool avatarOk = false;
        for (const client::Layout& L : client::kLayouts) {
            auto at = [&](uint32_t rva, const uint8_t* stub, uint32_t n) { return rva + n <= size && memcmp(base + rva, stub, n) == 0; };
            const bool c2s = at(L.rvaC2SFrame, L.c2sStub, L.c2sStubLen);
            const bool s2c = at(L.rvaS2CEvent, L.s2cStub, L.s2cStubLen);
            // the connection's vtable: its first slots are code of this image
            bool vt = L.rvaConnVtable + 12 <= size;
            for (int i = 0; vt && i < 3; ++i) {
                const uint32_t f = *(const uint32_t*)(base + L.rvaConnVtable + 4 * i);
                vt = f > imageBase && f < imageBase + size;
            }
            const bool send = L.rvaNetSendRaw + 4 <= size;
            // the avatar chain (EV_AVATAR): the world global's address in the code with its +0x568 read just after
            int chain = 0;
            if (L.rvaWorld && L.rvaWorld + 4 <= size) {
                const uint32_t va = imageBase + L.rvaWorld;
                for (uint32_t i = 0x1000; i + 20 < size; ++i) {
                    if (memcmp(base + i, &va, 4) != 0) continue;
                    for (uint32_t k = i + 4; k < i + 16; ++k)
                        if (memcmp(base + k, &L.worldAvatar, 4) == 0) { ++chain; break; }
                }
            }
            printf("  %-40s C2S framer %-5s  S2C site %-5s  connection vtable %-4s  avatar [%08X]+%X read %d x\n", L.name,
                   c2s ? "match" : "no", s2c ? "match" : "no", vt ? "code" : "no", imageBase + L.rvaWorld, L.worldAvatar, chain);
            if (c2s && s2c && vt && send) { ++matched; avatarOk = chain >= 50; }
        }
        ++total;
        pass += matched == 1 && avatarOk;
        printf("  -> %s%s\n", matched == 1 ? "exactly one build matches: the hook picks it" : matched ? "several builds match!" : "no build matches",
               matched == 1 ? (avatarOk ? "; its avatar chain is in the code" : "; its avatar chain is NOT in the code") : "");
        FreeLibrary(h);
    }
    printf("\nlayouts: %d/%d\n", pass, total);
}

// ── the Key Quests (quest_graph.cpp + bot/act_keyquest.cpp): the dialog graph, the towns, the prerequisites ──
// A state for the planner: nothing done, nothing on, an empty bag (or what the test sets).
struct TestView : QStateView {
    const GameData& gd;
    int lv = 100, ft = 0;
    std::map<uint64_t, int> h;
    std::set<uint64_t> on;
    explicit TestView(const GameData& g) : gd(g) {}
    bool fresh = true;              // a new character: every quest known as never done (else unknown)
    int  hist_lo(QKey q) const override { auto it = h.find(q.v()); return it == h.end() ? 0 : it->second; }
    int  hist_hi(QKey q) const override { auto it = h.find(q.v()); return it != h.end() ? it->second : fresh ? 0 : -1; }
    bool active(QKey q) const override { return on.count(q.v()) != 0; }
    bool items(QKey) const override { return false; }
    bool has_item(uint32_t) const override { return false; }
    int  level() const override { return lv; }
    int  ftype() const override { return ft; }
    bool monq_busy(QKey) const override { return false; }
    bool gatherable(uint32_t item) const override
    {
        const ItemSources& is = gd.item_sources();
        return !is.mobs(item).empty() || !is.digs(item).empty();
    }
};

static std::string conds_text(const QuestGraph& g, const QGate& gt)
{
    std::string s;
    for (const QCond& c : gt.conds) s += (s.empty() ? "" : "; ") + g.cond_text(c);
    return s.empty() ? "-" : s;
}

static void keyquest_graph_dump(const GameData& gd, int& pass, int& total)
{
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const Atlas& at = gd.atlas();
    const QuestGraph& g = gd.quest_graph();
    const std::string capDir = exe_dir() + "captures\\";
    CreateDirectoryA(capDir.c_str(), nullptr);
    std::ofstream out(capDir + "keyquest_graph.txt", std::ios::binary);
    printf("\nthe function table (client 0xFACE78):\n");
    check(Atlas::func_id("Quest_Check") == 2 && Atlas::func_id("Quest_Complete_3") == 102 && Atlas::func_id("MonQuest_Accept_2") == 243 &&
          Atlas::func_id("CheckQuestState_1") == 104 && Atlas::func_id("Quest_Once_Accept_4") == 205 && Atlas::func_id("Nope") == 0,
          "names -> ids (Quest_Check 2, Quest_Complete_3 102, MonQuest_Accept_2 243, CheckQuestState_1 104, Quest_Once_Accept_4 205)");
    {
        const std::vector<DialogOpt> o = Atlas::parse_options("    Message: 'Hi\\No way!\\Exit\\Maybe?\\Quest_Check,1,3,4,7\\Go\\12\\@\\@img,i10411\\Ok\\(lv)Quest_Accept_2,q0,q1,8'");
        check(o.size() == 4 && o[0].func == 1 && o[1].func == 2 && o[1].args == std::vector<uint16_t>{ 1, 3, 4, 7 } && o[2].link == 12 &&
              o[3].func == 97 && o[3].args == std::vector<uint16_t>{ 128, 129, 8 },
              fmt("a message's options: Exit, Quest_Check [1 3 4 7], link 12, (lv)Quest_Accept_2 [128 129 8] (%zu parsed)", o.size()));
    }

    printf("\nthe Key Quest towns:\n");
    const std::vector<KeyTown>& towns = g.towns();
    int withKey = 0, quests = 0;
    for (const KeyTown& t : towns) { withKey += t.key.id != 0; quests += int(t.quests.size()); }
    check(withKey == 11, fmt("%zu towns, %d with a key quest (want 11: the 10 sticker towns + Megalopolis), %d sticker quests", towns.size(),
                             withKey, quests));
    // Megalopolis: no sticker — Fix Toys wants 4 Instant Adhesive, the Poppuri Dungeon's quests give one at their 5th run;
    // the chain goes on to Poppuri Redemption (it takes Fix Toys' Mended Toy Box)
    {
        const KeyTown* mg = nullptr;
        for (const KeyTown& t : towns) if (t.name.find("Megalopolis") != std::string::npos) mg = &t;
        std::string chain;
        if (mg) for (const QKey& k : mg->chain) chain += fmt(" %d", k.id);
        check(mg && mg->key.id == 424 && mg->need == 4 && mg->quests.size() >= 8 && mg->chain.size() >= 2 && mg->chain[1].id == 425,
              fmt("Megalopolis (Poppuri Dungeon): key Fix Toys x4 Instant Adhesive, %zu quests give it, chain%s", mg ? mg->quests.size() : 0,
                  chain.c_str()));
    }
    TestView v(gd);
    int reachable = 0, blocked = 0, withPre = 0, withMonq = 0;
    std::map<std::string, int> whyCount;
    for (const KeyTown& t : towns) {
        const QuestInfo* kq = t.key.id ? gd.quest(t.key.id) : nullptr;
        out << "\n## " << t.name << " (sticker " << t.sticker << "): key " << (kq ? fmt("#%d %s, %d stickers, Lv %d", kq->id, kq->name.c_str(), t.need, kq->minLevel) : std::string("none"))
            << "\n   chain:";
        for (const QKey& c : t.chain) out << " " << c.id;
        out << "\n";
        v.lv = kq ? std::max(kq->minLevel, 1) : 300;
        for (const StickerQuest& sq : t.quests) {
            const QuestInfo* qi = gd.quest(sq.q.id);
            v.lv = std::max(v.lv, qi ? qi->minLevel : 1);
        }
        for (const StickerQuest& sq : t.quests) {
            const QuestInfo* qi = gd.quest(sq.q.id);
            std::vector<QJob> jobs;
            const bool ok = g.expand(sq.q, sq.runs, v, jobs);
            ok ? ++reachable : ++blocked;
            withPre += jobs.size() > 1;
            bool mq = false;
            for (const QJob& j : jobs) mq |= j.q.mon;
            withMonq += mq;
            out << fmt("  #%-5d %-40s runs %d (first %d) stickers %.2f -> %s\n", sq.q.id, qi ? qi->name.c_str() : "?", sq.runs, sq.firstRun,
                       sq.stickers, ok ? "ok" : "BLOCKED");
            for (const QJob& j : jobs) {
                const QuestPlaces* qp = g.places(j.q);
                std::string where = "-", gate = "-";
                if (qp && j.accept >= 0) {
                    const QPlace& p = qp->accepts[size_t(j.accept)];
                    const AtlasNpc* n = at.npc(p.npc);
                    where = fmt("%s @ %s (slot %d, %s [%zu args])", n->name.c_str(), gd.map_name(n->zone).c_str(), p.slot, Atlas::func_name(p.func), p.args.size());
                    if (j.gate >= 0) gate = conds_text(g, p.gates[size_t(j.gate)]);
                }
                out << fmt("      %s %-44s x%-2d %s%s | take: %s | gate: %s%s\n", j.q.mon ? "M" : "Q", g.quest_name(j.q).c_str(), j.target,
                           j.keepActive ? "(keep on) " : "", j.blocked ? ("BLOCKED: " + j.why).c_str() : "", where.c_str(), gate.c_str(),
                           j.complete >= 0 && qp && size_t(j.complete) < qp->completes.size() && qp->completes[size_t(j.complete)].npc != (j.accept >= 0 ? qp->accepts[size_t(j.accept)].npc : -1)
                               ? fmt(" | hand in at %s", at.npc(qp->completes[size_t(j.complete)].npc)->name.c_str()).c_str() : "");
                if (j.blocked) ++whyCount[j.why.substr(0, 60)];
            }
        }
        // the key chain itself
        for (const QKey& c : t.chain) {
            std::vector<QJob> jobs;
            const bool ok = g.expand(c, 1, v, jobs);
            out << fmt("  key step #%d %s -> %s (%zu jobs)\n", c.id, g.quest_name(c).c_str(), ok ? "ok" : ("BLOCKED: " + jobs.back().why).c_str(), jobs.size());
            const QuestPlaces* qp = g.places(c);
            if (ok || !qp) continue;
            out << "      why: " << g.explain(c, v) << "\n";
            for (const QPlace& p : qp->accepts) {                   // why: each way and what its conditions name
                out << fmt("      take %s @ %s [%s]\n", Atlas::func_name(p.func), at.npc(p.npc)->name.c_str(), at.npc(p.npc)->msgTable.c_str());
                for (const QGate& gt : p.gates) {
                    std::string pth;
                    for (int m : gt.path) pth += fmt(" %d", m);
                    out << "        gate:" << pth << " | " << conds_text(g, gt) << "\n";
                    for (const QCond& cd : gt.conds)
                        if (cd.kind == QCond::HistGE || cd.kind == QCond::Active) {
                            const QuestPlaces* cp = g.places(cd.q);
                            out << fmt("          %s: %zu takes, %zu hand-ins", g.quest_name(cd.q).c_str(), cp ? cp->accepts.size() : 0,
                                       cp ? cp->completes.size() : 0);
                            if (cp) for (const QPlace& x : cp->accepts) out << fmt(" [%s @ %s, %zu gates]", Atlas::func_name(x.func), at.npc(x.npc)->name.c_str(), x.gates.size());
                            out << "\n";
                        }
                }
            }
        }
    }
    printf("  sticker quests: %d with a way, %d blocked; %d with quests before them, %d with a monster quest before them\n",
           reachable, blocked, withPre, withMonq);
    for (const auto& w : whyCount) printf("    blocked: %3d x %s\n", w.second, w.first.c_str());
    check(reachable * 10 >= (reachable + blocked) * 8, fmt("at least 80 %% of the sticker quests have a way (%d / %d)", reachable, reachable + blocked));
    printf("  (dump: captures\\keyquest_graph.txt)\n");
    // the real case: a character whose history the bot does not know yet (the quests before are taken as maybe done)
    {
        v.fresh = false;
        int sOk = 0, sAll = 0, kOk = 0, kAll = 0;
        size_t assumed = 0;
        std::string kBad;
        for (const KeyTown& t : towns) {
            const QuestInfo* kq = t.key.id ? gd.quest(t.key.id) : nullptr;
            v.lv = kq ? std::max(kq->minLevel, 1) : 300;
            for (const StickerQuest& sq : t.quests) {
                std::vector<QJob> jobs;
                ++sAll;
                if (g.expand(sq.q, sq.runs, v, jobs)) ++sOk;
                for (const QJob& j : jobs) assumed += j.assumed.size();
            }
            for (const QKey& c : t.chain) {
                std::vector<QJob> jobs;
                ++kAll;
                if (g.expand(c, 1, v, jobs)) ++kOk;
                else kBad += fmt(" #%d (%s)", c.id, g.explain(c, v).c_str());
            }
        }
        printf("  history unknown: %d / %d sticker quests with a way (%zu conditions taken as true), key steps %d / %d%s\n", sOk, sAll,
               assumed, kOk, kAll, kBad.empty() ? "" : (": blocked" + kBad).c_str());
        check(sOk >= reachable && kOk + 2 >= kAll, "with the history unknown no more quests are blocked, the key steps have a way (but an item to bring)");
        v.fresh = true;
    }

    // Genius Cochma's Black Swamp survey: 7107 (Survey 4, the sticker) <- 7106 x5 <- 7105 x5 <- the monster quest 266 x5
    {
        std::vector<QJob> jobs;
        v.lv = 150;
        g.expand(QKey{ false, 7107 }, 5, v, jobs);
        std::string order;
        for (const QJob& j : jobs) order += fmt("%s%s%d x%d", order.empty() ? "" : " -> ", j.q.mon ? "M" : "", j.q.id, j.target);
        auto at_ = [&](QKey k) { for (size_t i = 0; i < jobs.size(); ++i) if (jobs[i].q == k) return int(i); return -1; };
        const int a = at_(QKey{ true, 266 }), b = at_(QKey{ false, 7105 }), c = at_(QKey{ false, 7106 }), d = at_(QKey{ false, 7107 });
        check(a >= 0 && b > a && c > b && d > c && !jobs.back().blocked,
              "Swamp survey: monster quest 266 x5 -> 7105 x5 -> 7106 x5 -> 7107 x5 (" + order + ")");
        // with 7106 already done five times: only 7107
        v.h[QKey{ false, 7106 }.v()] = 5;
        std::vector<QJob> j2;
        g.expand(QKey{ false, 7107 }, 5, v, j2);
        check(j2.size() == 1 && j2[0].q.id == 7107, fmt("with 7106 done 5 times: just 7107 (%zu jobs)", j2.size()));
        v.h.clear();
    }
}

// A fake ProjectPAULA for one NPC's dialog (npc_talk.cpp / quest.cpp): the PreCheck's first message (S2C 112), C2S 0x72
// -> 115, the checks' branches -> 115, Quest_Accept -> 132 + 115, Quest_Complete -> 150 (the bag records) + 115 + 133,
// Exit -> 117. The bag is the model's (the rig feeds both).
struct FakeNpcServer {
    const GameData& gd;
    StuckRig& s;
    const AtlasNpc* npc = nullptr;
    uint16_t eid = 0;
    std::map<uint64_t, int> hist;       // QKey::v() -> done
    std::map<int, int> on;              // normal quests on: qid -> run
    uint64_t nextUid = 0x9000;
    int talks = 0, nexts = 0, checks = 0, accepts = 0, completes = 0, exits = 0;
    int monq = 0, monqRun = 0;          // the monster quest on (0 = none) and its run
    bool monqKilled = false;
    bool dropped = false;               // a MonQuest_Complete before its kills (the server would close the session)
    std::vector<std::string> trace;

    int  h(QKey q) const { auto it = hist.find(q.v()); return it == hist.end() ? 0 : it->second; }
    std::string text_of(int index) const
    {
        if (npc && npc->dialog) { auto m = npc->dialog->msgs.find(index); if (m != npc->dialog->msgs.end()) return m->second.text; }
        return fmt("(message %d)", index);
    }
    void msg(int index, int qid)
    {
        s.feed(DIR_S2C, 0x0073, Pk().u16(eid).u16(uint16_t(index)).str(utf8_to_cp949(text_of(index)).c_str()).u16(0).u16(uint16_t(qid)).b);
    }
    bool holds(const QCond& c) const
    {
        switch (c.kind) {
        case QCond::HistGE: return h(c.q) >= c.n;
        case QCond::HistLT: return h(c.q) < c.n;
        case QCond::Active: return !c.q.mon && on.count(c.q.id);
        case QCond::NotActive: return c.q.mon || !on.count(c.q.id);
        default: return true;
        }
    }
    // how many runs of the quest's items one bag stack each holds (quest.cpp quest_items_ok)
    bool items_ok(const QuestInfo& q) const
    {
        for (const QuestNeed& n : q.src) if (s.w->count_item(n.item) < n.cnt) return false;
        return true;
    }
    int exchanges = 0, exCloses = 0, uses = 0;
    uint64_t usedUid = 0;
    int shopOpens = 0, buys = 0, shopCloses = 0, cmpOpens = 0, cmpRolls = 0, cmpMakes = 0, cmpCloses = 0;
    uint32_t boughtItem = 0;
    int boughtCnt = 0, cmpId = 0, cmpSpins = 0;
    std::vector<std::pair<uint64_t, uint16_t>> cmpGiven;    // the roll's items (uid, count)
    void respond(const Action& a)
    {
        auto u16at = [&](size_t o) { return o + 1 < a.body.size() ? int(a.body[o] | a.body[o + 1] << 8) : -1; };
        auto u64at = [&](size_t o) { uint64_t v = 0; for (int k = 7; k >= 0; --k) v = v << 8 | (o + size_t(k) < a.body.size() ? a.body[o + size_t(k)] : 0); return v; };
        // shop.cpp: 0x46 -> 0xA8, 0x48 [shop][n]{id, cnt} -> 0x49 [money][n]{record}, 0x4F
        if (a.op == 0x0046) { ++shopOpens; trace.push_back("shop opened"); s.feed(DIR_S2C, 0x00A8, Pk().u16(uint16_t(u16at(0))).str("Hello").b); return; }
        if (a.op == 0x0048) {
            ++buys;
            boughtItem = uint32_t(a.body[4] | a.body[5] << 8 | a.body[6] << 16 | uint32_t(a.body[7]) << 24);
            boughtCnt = u16at(8);
            trace.push_back(fmt("bought %d x %u", boughtCnt, boughtItem));
            s.feed(DIR_S2C, 0x0049, Pk().u32(uint32_t(std::max<int64_t>(0, s.w->self.money - 100))).u16(1).u8(4).u8(1).u32(boughtItem)
                                       .u64(nextUid++).u16(uint16_t(boughtCnt)).b);
            return;
        }
        if (a.op == 0x004F) { ++shopCloses; trace.push_back("shop closed"); return; }
        // compound.cpp: Compound_Start (func 20) -> 117 + 188; 227 -> 228; 186 -> 190; 192 -> 193
        if (a.op == 0x007F && a.body.size() > 2 && a.body[2] == 20) {
            ++cmpOpens;
            trace.push_back("compounder");
            s.feed(DIR_S2C, 0x0075, {});
            Pk p;
            p.u8(uint8_t(npc->compounds.size())).u16(0);
            for (int id : npc->compounds) p.u16(uint16_t(id));
            s.feed(DIR_S2C, 0x00BC, p.b);
            return;
        }
        if (a.op == 0x00E3) {
            ++cmpRolls;
            cmpId = u16at(0);
            cmpSpins = a.body.size() > 3 ? a.body[3] : 0;
            const uint8_t n = a.body.size() > 4 ? a.body[4] : 0;
            cmpGiven.clear();
            for (uint8_t i = 0; i < n && size_t(5 + 14 * (i + 1)) <= a.body.size(); ++i)
                cmpGiven.push_back({ u64at(5 + 14 * size_t(i)), uint16_t(u16at(5 + 14 * size_t(i) + 12)) });
            trace.push_back(fmt("roll compound %d x%d (%u items)", cmpId, cmpSpins, n));
            s.feed(DIR_S2C, 0x00E4, Pk().u8(100).b);
            return;
        }
        if (a.op == 0x00BA) {
            ++cmpMakes;
            const CompoundDef* cd = gd.atlas().compound(cmpId);
            Pk r;
            uint8_t cnt = 0;
            for (const auto& gv : cmpGiven) {
                const InvItem* it = s.w->find_item(gv.first);
                if (!it) continue;
                if (it->count > gv.second) r.u8(1).u8(1).u32(it->itemId).u64(gv.first).u16(uint16_t(it->count - gv.second));
                else r.u8(3).u8(1).u32(it->itemId).u64(gv.first).u16(0);
                ++cnt;
            }
            if (cd) for (uint32_t res : cd->res) { r.u8(4).u8(1).u32(res).u64(nextUid++).u16(uint16_t(cmpSpins)); ++cnt; }
            Pk p;
            p.u8(uint8_t(cd ? cd->res.size() : 0));
            if (cd) for (uint32_t res : cd->res) p.u32(res);
            p.u32(cd ? cd->waste : 0).u8(uint8_t(cmpSpins)).u8(0).u8(cnt);
            p.b.insert(p.b.end(), r.b.begin(), r.b.end());
            trace.push_back(fmt("compounded %d", cmpSpins));
            s.feed(DIR_S2C, 0x00BE, p.b);
            return;
        }
        if (a.op == 0x00C0) { ++cmpCloses; trace.push_back("compounder closed"); s.feed(DIR_S2C, 0x00C1, {}); return; }
        if (a.op == 0x00FE) {                               // exchange_shop.cpp handle_exchange: the items given -> the ResIDs
            ++exchanges;
            const int id = u16at(0);
            const ExchangeDef* x = gd.atlas().exchange(id);
            const uint8_t n = a.body.size() > 2 ? a.body[2] : 0;
            Pk r;
            uint8_t cnt = 0;
            for (uint8_t i = 0; i < n && size_t(3 + 14 * (i + 1)) <= a.body.size(); ++i) {
                uint64_t uid = 0;
                for (int k = 7; k >= 0; --k) uid = uid << 8 | a.body[3 + 14 * i + size_t(k)];
                const InvItem* it = s.w->find_item(uid);
                if (!it) continue;
                const uint16_t give = uint16_t(a.body[3 + 14 * i + 12] | a.body[3 + 14 * i + 13] << 8);
                if (it->count > give) r.u8(1).u8(1).u32(it->itemId).u64(uid).u16(uint16_t(it->count - give));
                else r.u8(3).u8(1).u32(it->itemId).u64(uid).u16(0);
                ++cnt;
            }
            if (x) for (uint32_t res : x->res) { r.u8(4).u8(1).u32(res).u64(nextUid++).u16(1); ++cnt; }
            trace.push_back(fmt("exchange %d", id));
            Pk p;
            p.u8(cnt);
            p.b.insert(p.b.end(), r.b.begin(), r.b.end());
            s.feed(DIR_S2C, 0x00FF, p.b);
            return;
        }
        if (a.op == 0x0101) { ++exCloses; trace.push_back("exchange closed"); s.feed(DIR_S2C, 0x0102, {}); return; }
        if (a.op == 0x0057) {                               // an item used (a port item: the map change is not simulated)
            ++uses;
            for (int k = 7; k >= 0; --k) usedUid = usedUid << 8 | (size_t(k) < a.body.size() ? a.body[size_t(k)] : 0);
            trace.push_back("item used");
            return;
        }
        if (a.op == 0x007F && a.body.size() > 2 && a.body[2] == 22) {   // ExchangeShop_Start: S2C 117 + 249 (state 18)
            trace.push_back("exchange shop");
            s.feed(DIR_S2C, 0x0075, {});
            Pk p;
            p.u8(uint8_t(npc->exchanges.size()));
            for (int id : npc->exchanges) p.u16(uint16_t(id));
            s.feed(DIR_S2C, 0x00F9, p.b);
            return;
        }
        if (a.op == 0x006F) {
            ++talks;
            const QuestGraph& g = gd.quest_graph();
            int m0 = 0;
            for (const auto& st : g.starts(*npc)) {
                bool ok = true;
                for (const QCond& c : st.second) ok = ok && holds(c);
                if (ok) { m0 = st.first; break; }
            }
            trace.push_back(fmt("112 msg %d", m0));
            s.feed(DIR_S2C, 0x0070, Pk().u16(eid).u16(npc->species).u8(20).str(utf8_to_cp949(text_of(m0)).c_str()).u16(0).u16(0).b);
        } else if (a.op == 0x0072) {
            ++nexts;
            const int idx = a.body.size() > 2 ? a.body[2] : -1;
            trace.push_back(fmt("0x72 -> %d", idx));
            msg(idx, npc->quests[0]);
        } else if (a.op == 0x007F) {
            const uint8_t func = a.body.size() > 2 ? a.body[2] : 0;
            std::vector<int> args;
            for (size_t i = 0; a.body.size() > 3 && i < a.body[3]; ++i) args.push_back(u16at(4 + 2 * i));
            int slot = 0;
            const QuestGraph::Fn fn = QuestGraph::decode(func, slot);
            const int qid = slot <= 4 ? npc->quests[slot] : 0;
            const QuestInfo* qi = gd.quest(qid);
            auto arg = [&](int r) { return r >= 1 && size_t(r) <= args.size() ? args[size_t(r - 1)] : -1; };
            switch (fn) {
            case QuestGraph::Fn::Exit:
                ++exits;
                trace.push_back("exit");
                s.feed(DIR_S2C, 0x0075, {});
                break;
            case QuestGraph::Fn::Check: {
                ++checks;
                int r;
                if (on.count(qid)) r = on[qid] > qi->maxCount ? 4 : items_ok(*qi) ? 2 : 3;
                else r = h(QKey{ false, qid }) >= qi->maxCount ? 4 : 1;
                trace.push_back(fmt("Quest_Check slot %d -> %d", slot, arg(r)));
                msg(arg(r), qid);
                break;
            }
            case QuestGraph::Fn::CheckState: {
                ++checks;
                const bool yes = npc->checkNum[slot] > 0 && h(QKey{ false, npc->checkQuest[slot] }) >= npc->checkNum[slot];
                trace.push_back(fmt("CheckQuestState_%d -> %d", slot, arg(yes ? 1 : 2)));
                msg(arg(yes ? 1 : 2), qid);
                break;
            }
            case QuestGraph::Fn::SCheck: {                  // quest.cpp squest_check (no scenario quest on here)
                ++checks;
                const QuestInfo* si = gd.scenario_quest(qid);
                const int r = si && h(QKey{ false, qid, true }) < si->maxCount ? 1 : 4;
                trace.push_back(fmt("S_Quest_Check slot %d -> %d", slot, arg(r)));
                msg(arg(r), qid);
                break;
            }
            case QuestGraph::Fn::SCheckState: {             // S_CheckQuestState(_1/_2): the scenario quest's history
                ++checks;
                const bool yes = npc->checkNum[slot] > 0 && h(QKey{ false, npc->checkQuest[slot], true }) >= npc->checkNum[slot];
                trace.push_back(fmt("S_CheckQuestState_%d -> %d", slot, arg(yes ? 1 : 2)));
                msg(arg(yes ? 1 : 2), qid);
                break;
            }
            case QuestGraph::Fn::SActiveId:
                ++checks;
                trace.push_back(fmt("S_ActiveQuestID_%d -> %d", slot, arg(2)));
                msg(arg(2), qid);
                break;
            case QuestGraph::Fn::Accept: {
                ++accepts;
                if (!qi || on.count(qid) || h(QKey{ false, qid }) >= qi->maxCount) { s.feed(DIR_S2C, 0x0074, {}); trace.push_back("accept refused"); break; }
                const int run = h(QKey{ false, qid }) + 1;
                on[qid] = run;
                trace.push_back(fmt("accept %d run %d", qid, run));
                s.feed(DIR_S2C, 0x0084, Pk().u32(uint32_t(qid)).u16(uint16_t(run)).str(qi->name.c_str()).str("npc").str("pos").str("c").u16(uint16_t(qid)).b);
                msg(arg(1), qid);
                break;
            }
            case QuestGraph::Fn::Complete: {
                ++completes;
                if (!qi || !on.count(qid) || !items_ok(*qi)) { s.feed(DIR_S2C, 0x0074, {}); trace.push_back("complete refused"); break; }
                const int run = on[qid];
                Pk p;
                std::vector<std::pair<uint32_t, int>> recs;          // the items taken: their stack's new count
                Pk r;
                uint16_t n = 0;
                for (const QuestNeed& q : qi->src)
                    for (const InvItem& it : s.w->inventory)
                        if (it.itemId == q.item) {
                            r.u8(1).u8(1).u32(it.itemId).u64(it.uid).u16(uint16_t(it.count - q.cnt));
                            ++n;
                            break;
                        }
                if (const QuestResultRow* row = qi->result_for(run))
                    for (const QuestNeed& q : row->items) { r.u8(4).u8(1).u32(q.item).u64(nextUid++).u16(uint16_t(q.cnt)); ++n; }
                p.u16(n);
                p.b.insert(p.b.end(), r.b.begin(), r.b.end());
                p.u16(0).u32(1000).u32(0).u16(uint16_t(qid)).u16(uint16_t(run)).u32(0).u16(0).u16(0);
                on.erase(qid);
                hist[QKey{ false, qid }.v()] = run;
                trace.push_back(fmt("complete %d run %d", qid, run));
                s.feed(DIR_S2C, 0x0096, p.b);
                msg(arg(run < qi->maxCount ? 1 : 3), qid);
                s.feed(DIR_S2C, 0x0085, Pk().u32(uint32_t(qid)).b);
                break;
            }
            case QuestGraph::Fn::MCheck: {                  // monster_quest.cpp monq_check
                ++checks;
                const MonQuestInfo* mi = gd.monster_quest(qid);
                int r;
                if (monq && monq != qid) r = 6;
                else if (monq) r = monqKilled ? 2 : 3;
                else r = mi && h(QKey{ true, qid }) >= mi->maxCount ? 4 : 1;
                trace.push_back(fmt("MonQuest_Check slot %d -> %d", slot, arg(r)));
                msg(arg(r), qid);
                break;
            }
            case QuestGraph::Fn::MAccept: {
                ++accepts;
                const MonQuestInfo* mi = gd.monster_quest(qid);
                if (!mi || monq) { s.feed(DIR_S2C, 0x0074, {}); trace.push_back("monster accept refused"); break; }
                const int grade = std::max(0, mi->grade_for(s.w->self.level));
                monq = qid;
                monqRun = h(QKey{ true, qid }) + 1;
                monqKilled = false;
                trace.push_back(fmt("monster accept %d run %d", qid, monqRun));
                s.feed(DIR_S2C, 0x00D2, Pk().u16(uint16_t(qid)).u16(uint16_t(mi->grades[size_t(grade)].monster)).u8(uint8_t(grade))
                                            .u8(uint8_t(monqRun)).u16(0).u32(0).str("mq").str("npc").str("pos").str("%s").b);
                s.feed(DIR_S2C, 0x00D7, Pk().u32(1200000).u32(1200000).b);
                s.feed(DIR_S2C, 0x00D9, Pk().u16(0).u16(uint16_t(mi->grades[size_t(grade)].cnt)).b);
                msg(arg(1), qid);
                break;
            }
            case QuestGraph::Fn::MComplete: {
                ++completes;
                if (monq != qid || !monqKilled) { dropped = true; trace.push_back("MONSTER COMPLETE BEFORE THE KILLS"); break; }
                trace.push_back(fmt("monster complete %d run %d", qid, monqRun));
                s.feed(DIR_S2C, 0x00DB, Pk().u16(0).u16(0).u32(5000).u32(0).u16(uint16_t(qid)).u32(100).b);
                msg(arg(1), qid);
                s.feed(DIR_S2C, 0x00D3, Pk().u16(uint16_t(qid)).b);
                hist[QKey{ true, qid }.v()] = monqRun;
                monq = 0;
                break;
            }
            default:
                trace.push_back(fmt("func %u (not simulated)", func));
                s.feed(DIR_S2C, 0x0074, {});
                break;
            }
        }
    }
};

// Rose Garden (user: "they need a key - get the keys, it is no bug"): King Edward / Queen Cecilia stand in the Secret
// Garden of Rosetta (only Prince Sebastian's move with the Garden key in the bag). The key is Alfredo's monster quest's
// reward (level 120): it joins the plan. The Butler of Vamp Castle (Vamp Dungeon 1): the server data's only way in is
// the Light Filled Egg's teleport at Trickster's Shrine (level 135) — "no way now" below that, no key opens it.
static void rose_garden_test(const GameData& gd, int& pass, int& total)
{
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const QuestGraph& g = gd.quest_graph();
    const KeyTown* rose = nullptr;
    for (const KeyTown& t : g.towns())
        for (const StickerQuest& sq : t.quests) if (sq.q.id == 666) rose = &t;
    if (!rose) { printf("no town with quest 666\n"); return; }
    for (int qid : { 666 }) {
        StuckRig s(gd);
        s.login(89, 1240, 3368, 130);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.keyq.enabled = true;
        s.bot.keyq.sticker = rose->sticker;
        s.bot.keyq.picked = { qid };
        s.bot.keyq.doKey = false;
        const std::vector<QJob> plan = s.bot.keyq.make_plan(*s.w, s.bot.book);
        std::string txt;
        bool monq = false, blocked = false;
        for (const QJob& j : plan) {
            txt += j.giveItem ? fmt(" give:%u", j.giveItem) : fmt(" %c%d%s", j.q.tag(), j.q.id, j.blocked ? "(blocked)" : "");
            monq = monq || (j.q.mon && j.q.id == 21);
            blocked = blocked || (j.q.id == qid && j.blocked);
        }
        printf("  quest %d at level 130 from Rose Field 1: plan%s\n", qid, txt.c_str());
        check(monq && !blocked, fmt("%d (Queen Cecilia, Secret Garden): the Garden Key Monster Quest (Alfredo, m21) joins the plan, "
                                    "no block", qid));
    }
    {
        WorldModel w(&gd);
        RouteOptions o;
        o.bagKnown = true;
        o.money = 100000;
        o.level = 130;
        const bool at130 = gd.atlas().zone_reachable(89, 107, o);
        o.bag[11179] = 1;
        const bool key130 = gd.atlas().zone_reachable(89, 107, o);
        o.bag.clear();
        o.level = 140;
        const bool at140 = gd.atlas().zone_reachable(89, 107, o);
        check(!at130 && !key130 && at140, "Vamp Dungeon 1 (Butler of Vamp Castle): no way at level 130 (not with the Garden key either), "
                                          "a way at 140 (the teleport at Trickster's Shrine, level 135)");
    }
    // and at level 40 the monster quest (level 120) cannot be taken: no provider, the job waits for the key
    {
        StuckRig s(gd);
        s.login(89, 1240, 3368, 40);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.keyq.enabled = true;
        s.bot.keyq.sticker = rose->sticker;
        s.bot.keyq.picked = { 666 };
        bool monq = false;
        for (const QJob& j : s.bot.keyq.make_plan(*s.w, s.bot.book)) monq = monq || (j.q.mon && j.q.id == 21);
        check(!monq, "at level 40: not the monster quest (it wants level 120)");
    }
}

// The book after an errand (a live log: Bean's Request handed in, then "learned s1239 done fewer than 1 times" and the bot
// hunted its Cool Ice again): the server's exact history wins over a check answered before the hand-in.
static void book_learn_test(const GameData& gd, int& pass, int& total)
{
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    WorldModel w(&gd);
    w.quests.shist[1239] = QuestHist{ 1, 1 };            // S2C 831: handed in, run 1
    QuestBook b;
    b.merge_model(w);
    QCond lt;
    lt.kind = QCond::HistLT;
    lt.q = QKey{ false, 1239, true };
    lt.n = 1;
    std::vector<std::string> notes;
    b.learn({ lt }, &notes, &w);
    const auto it = b.h.find(lt.q.v());
    check(it != b.h.end() && it->second.lo == 1 && it->second.hi == 1 && notes.empty(),
          "a check answered before the hand-in (\"fewer than 1\") does not undo the server's \"done once\"");
    QCond lt2 = lt;
    lt2.q = QKey{ false, 1240, true };
    b.learn({ lt2 }, &notes, &w);
    const auto i2 = b.h.find(lt2.q.v());
    check(i2 != b.h.end() && i2->second.hi == 0, "a quest the server said nothing of is still learned from the dialog");
}

static void keyquest_run_test(const GameData& gd, int& pass, int& total)
{
    rose_garden_test(gd, pass, total);
    book_learn_test(gd, pass, total);
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const Atlas& at = gd.atlas();
    const QuestGraph& g = gd.quest_graph();
    const KeyTown* desert = nullptr;
    for (const KeyTown& t : g.towns()) if (t.name == "Desert") desert = &t;
    const QuestInfo* qi = gd.quest(255);
    const QuestPlaces* qp = g.places(QKey{ false, 255 });
    if (!desert || !qi || !qp || qp->accepts.empty() || qp->completes.empty()) { printf("no Desert town / quest 255 in the data\n"); return; }
    const AtlasNpc* npc = at.npc(qp->accepts.front().npc);
    printf("\na run: Desert, quest 255 \"%s\" by %s in %s (5 runs, the sticker at the 5th), its items in the bag:\n", qi->name.c_str(),
           npc->name.c_str(), at.map_label(npc->zone).c_str());

    StuckRig s(gd);
    const uint16_t NPC = 910;
    int sx = npc->x + 40, sy = npc->y;
    if (const MapGrid* mg = gd.map(npc->zone)) if (mg->blocked(sx, sy)) { sx = npc->x; sy = npc->y + 40; }
    s.login(npc->zone, sx, sy);
    s.npc(NPC, npc->species, npc->x, npc->y);
    const uint32_t ITEM = qi->src.front().item;
    s.feed(DIR_S2C, 0x00E7, Pk().u16(1).u8(1).u32(ITEM).u64(0x501).u16(uint16_t(qi->src.front().cnt * 5)).b);
    FakeNpcServer srv{ gd, s, npc, NPC };
    for (BotAction* a : s.bot.actions()) a->enabled = false;
    s.bot.keyq.enabled = true;
    s.bot.keyq.sticker = desert->sticker;
    s.bot.keyq.picked = { 255 };
    s.bot.keyq.doKey = false;
    s.bot.sync_modes();
    check(s.bot.attack.enabled && !s.bot.drill.enabled && !s.bot.quest.enabled && !s.bot.hunt.enabled,
          "Key Quest on: the Attack on, the Drill / Monster Quest / Item Hunt off");
    s.run(0, 3.5);
    s.bot.start(*s.w, s.now);
    size_t seen = 0;
    std::vector<Action> sent;
    for (int k = 0; k < 900 && s.bot.running; ++k) {
        s.run(0, 0.1);
        for (; seen < s.sent.size(); ++seen) { sent.push_back(s.sent[seen]); srv.respond(s.sent[seen]); }
    }
    // the first take: 0x6F, then the way 0 > 1 > 8 > 9 (Quest_Check's 1st argument, two messages), then Quest_Accept
    std::string first;
    for (size_t i = 0; i < sent.size() && i < 8; ++i) {
        const Action& a = sent[i];
        first += fmt(" %04X", a.op);
        if (a.op == 0x0072 && a.body.size() > 2) first += fmt("[%d]", a.body[2]);
        if (a.op == 0x007F && a.body.size() > 2) first += fmt("[%s]", Atlas::func_name(a.body[2]));
    }
    printf("  the first requests:%s\n", first.c_str());
    const bool wayOk = sent.size() >= 6 && sent[0].op == 0x006F && sent[1].op == 0x007F && sent[1].body[2] == 2 && sent[2].op == 0x0072 &&
                       sent[3].op == 0x0072 && sent[4].op == 0x007F && sent[4].body[2] == 3;
    check(wayOk, "the take walks the dialog: talk, Quest_Check, message, message, Quest_Accept (as the client clicks it)");
    check(srv.accepts == 5 && srv.completes == 5 && srv.hist[QKey{ false, 255 }.v()] == 5,
          fmt("5 takes and 5 hand-ins (%d / %d), the server's history 5", srv.accepts, srv.completes));
    check(s.w->count_item(desert->sticker) >= 1, fmt("the sticker came with the 5th hand-in (%d in the bag)", s.w->count_item(desert->sticker)));
    check(srv.exits >= 10, fmt("each errand ends with Exit (%d)", srv.exits));
    const auto b = s.bot.book.h.find(QKey{ false, 255 }.v());
    check(b != s.bot.book.h.end() && b->second.lo == 5 && b->second.hi == 5, "the book: quest 255 done 5 times");
    check(!s.bot.running && s.bot.stopFinished, "then the bot stops: everything picked is done (" + s.bot.stopWhy + ")");
    bool refused = false;
    for (const std::string& t : srv.trace) refused = refused || t.find("refused") != std::string::npos;
    check(!refused, "the server refused nothing");
    std::string tr;
    for (size_t i = 0; i < srv.trace.size() && i < 14; ++i) tr += (tr.empty() ? "" : ", ") + srv.trace[i];
    printf("  server: %s ...\n", tr.c_str());

    // a quest whose dialog puts another first, the history unknown: Genius Cochma (Swamp) — 7107 (Survey 4) is taken as
    // possible, the walk finds out from the PreCheck / CheckQuestState that 7106 was never done, and the plan changes
    const QuestPlaces* cp = g.places(QKey{ false, 7107 });
    const KeyTown* swamp = nullptr;
    for (const KeyTown& t : g.towns()) if (t.name == "Swamp") swamp = &t;
    if (!cp || cp->accepts.empty() || !swamp) { printf("no Swamp / 7107 in the data\n"); return; }
    const AtlasNpc* coch = at.npc(cp->accepts.front().npc);
    printf("\nlearning from the dialog: Swamp, quest 7107 by %s in %s, nothing known of its chain:\n", coch->name.c_str(), at.map_label(coch->zone).c_str());
    StuckRig s2(gd);
    int cx = coch->x + 40, cy = coch->y;
    if (const MapGrid* mg = gd.map(coch->zone)) if (mg->blocked(cx, cy)) { cx = coch->x; cy = coch->y + 40; }
    s2.login(coch->zone, cx, cy, 145);                    // (Swamp's quests: level 140)
    s2.npc(NPC, coch->species, coch->x, coch->y);
    FakeNpcServer srv2{ gd, s2, coch, NPC };
    for (BotAction* a : s2.bot.actions()) a->enabled = false;
    s2.bot.keyq.enabled = true;
    s2.bot.keyq.sticker = swamp->sticker;
    s2.bot.keyq.picked = { 7107 };
    s2.bot.keyq.doKey = false;
    s2.bot.sync_modes();
    s2.run(0, 3.5);
    s2.bot.start(*s2.w, s2.now);
    const std::vector<QJob>& j0 = s2.bot.keyq.jobs();
    size_t seen2 = 0;
    std::vector<Action> sent2;
    bool asked7106 = false;
    for (int k = 0; k < 200 && s2.bot.running; ++k) {
        s2.run(0, 0.1);
        for (; seen2 < s2.sent.size(); ++seen2) { sent2.push_back(s2.sent[seen2]); srv2.respond(s2.sent[seen2]); }
        for (const QJob& j : s2.bot.keyq.jobs()) asked7106 = asked7106 || j.q.id == 7106;
        if (asked7106 && s2.bot.keyq.jobs().size() >= 2) break;
    }
    (void)j0;
    const auto b2 = s2.bot.book.h.find(QKey{ false, 7106 }.v());
    std::string jl;
    for (const QJob& j : s2.bot.keyq.jobs()) jl += fmt(" %s%d x%d", j.q.mon ? "M" : "", j.q.id, j.target);
    check(b2 != s2.bot.book.h.end() && b2->second.hi >= 0 && b2->second.hi < 5,
          fmt("the walk learned that 7106 is not done 5 times (book %d..%d)", b2 == s2.bot.book.h.end() ? -9 : b2->second.lo,
              b2 == s2.bot.book.h.end() ? -9 : b2->second.hi));
    check(asked7106, "the plan now puts 7106 first:" + jl);
    std::string tr2;
    for (size_t i = 0; i < srv2.trace.size() && i < 10; ++i) tr2 += (tr2.empty() ? "" : ", ") + srv2.trace[i];
    printf("  server: %s\n", tr2.c_str());
    for (const std::string& l : s2.bot.log) if (l.find("key quest") != std::string::npos) printf("    log: %s\n", l.c_str());

    // the chain's monster quest: 7105 (Survey 2) comes after Genius Cochma's monster quest 266 done 5 times — the bot
    // takes 266 through the dialog, hunts (here the server says the kills are done), hands it in, takes it again
    printf("\na chain's monster quest: Swamp, quest 7105 after monster quest 266 (%s), nothing of it done:\n", coch->name.c_str());
    StuckRig s3(gd);
    s3.login(coch->zone, cx, cy, 145);
    s3.npc(NPC, coch->species, coch->x, coch->y);
    FakeNpcServer srv3{ gd, s3, coch, NPC };
    for (BotAction* a : s3.bot.actions()) a->enabled = false;
    s3.bot.keyq.enabled = true;
    s3.bot.keyq.sticker = swamp->sticker;
    s3.bot.keyq.picked = { 7107 };
    s3.bot.keyq.doKey = false;
    for (QKey q : { QKey{ true, 266 }, QKey{ false, 7105 }, QKey{ false, 7106 }, QKey{ false, 7107 } }) s3.bot.book.h[q.v()] = QuestHist{ 0, 0 };
    s3.bot.sync_modes();
    s3.run(0, 3.5);
    s3.bot.start(*s3.w, s3.now);
    std::string firstPlan;
    for (const QJob& j : s3.bot.keyq.jobs()) firstPlan += fmt(" %s%d x%d", j.q.mon ? "M" : "", j.q.id, j.target);
    if (firstPlan.empty())
        for (const QJob& j : s3.bot.keyq.make_plan(*s3.w, s3.bot.book)) firstPlan += fmt(" %s%d x%d", j.q.mon ? "M" : "", j.q.id, j.target);
    size_t seen3 = 0;
    uint64_t takenAt = 0;
    bool hunted = false, secondTake = false;
    for (int k = 0; k < 600 && s3.bot.running && !secondTake; ++k) {
        s3.run(0, 0.1);
        for (; seen3 < s3.sent.size(); ++seen3) srv3.respond(s3.sent[seen3]);
        if (srv3.monq && !srv3.monqKilled) {
            if (!takenAt) takenAt = s3.now;
            const std::string& st = s3.bot.keyq.status;
            if (s3.bot.keyq.monq_hunting(*s3.w) && (st.rfind("monster quest:", 0) == 0 || st.rfind("to ", 0) == 0 || st.find("walking") != std::string::npos))
            {
                if (!hunted) printf("    hunting: %s\n", st.c_str());
                hunted = true;
            }
            if (s3.now - takenAt > 30000000ull) {                  // 3 s later: all killed
                const MonQuestInfo* mi = gd.monster_quest(266);
                const int cnt = mi ? mi->grades[size_t(std::max(0, mi->grade_for(s3.w->self.level)))].cnt : 10;
                s3.feed(DIR_S2C, 0x00D9, Pk().u16(uint16_t(cnt)).u16(uint16_t(cnt)).b);
                s3.feed(DIR_S2C, 0x00DA, Pk().u16(266).u8(1).b);
                srv3.monqKilled = true;
            }
        }
        secondTake = srv3.hist[QKey{ true, 266 }.v()] >= 1 && srv3.monq == 266;
    }
    check(firstPlan.find("M266 x5") != std::string::npos && firstPlan.find("M266") < firstPlan.find("7107"),
          "the plan starts with the monster quest:" + firstPlan);
    check(hunted, "after the take (210) the bot hunts its monster: " + s3.bot.keyq.status);
    check(!srv3.dropped, "no MonQuest_Complete before the kills (the server would drop us)");
    check(srv3.hist[QKey{ true, 266 }.v()] >= 1, fmt("the monster quest handed in (%d)", srv3.hist[QKey{ true, 266 }.v()]));
    check(secondTake, "and taken again for its 2nd run");
    const auto b3 = s3.bot.book.h.find(QKey{ true, 266 }.v());
    check(b3 != s3.bot.book.h.end() && b3->second.lo >= 1, "the book: monster quest 266 done once");
    std::string tr3;
    for (size_t i = 0; i < srv3.trace.size() && i < 16; ++i) tr3 += (tr3.empty() ? "" : ", ") + srv3.trace[i];
    printf("  server: %s\n", tr3.c_str());

    // the providers: 18 (Help Al-Hauri) wants a Filled Water Bucket, which only 19's results give; 19 wants a Water
    // Bucket, which only 254's give — picking 18 alone brings 19 and 254 into the plan
    {
        StuckRig s4(gd);
        s4.login(npc->zone, sx, sy);
        KeyQuestAction k4;
        k4.sticker = desert->sticker;
        k4.picked = { 18 };
        k4.doKey = false;
        std::string pl;
        int has19 = 0, has254 = 0;
        for (const QJob& j : k4.make_plan(*s4.w, s4.bot.book)) {
            pl += fmt(" %d x%d", j.q.id, j.target);
            has19 += j.q.id == 19;
            has254 += j.q.id == 254;
        }
        check(has19 && has254, "picking 18 alone brings in the quests whose rewards it needs:" + pl);
        // the key quest (5 stickers) with one sticker quest picked: it says how many more to pick
        KeyQuestAction k5;
        k5.sticker = desert->sticker;
        k5.picked = { 255 };
        k5.doKey = true;
        k5.refresh(*s4.w, s4.now, false, s4.bot.book);
        std::string note5;
        for (size_t i = 0; i < k5.jobs().size() && i < k5.job_info().size(); ++i)
            if (k5.jobs()[i].q == desert->key) note5 = k5.job_info()[i].note;
        check(note5.find("4 more") != std::string::npos, "the key quest with one sticker quest picked: \"" + note5 + "\"");
        // "only complete sticker quests" (doKey off): the plan has the picked quests, not the key quest's chain
        KeyQuestAction k6;
        k6.sticker = desert->sticker;
        k6.picked = { 255, 256 };
        bool keyIn = false, keyOut = true;
        k6.doKey = true;
        for (const QJob& j : k6.make_plan(*s4.w, s4.bot.book)) keyIn = keyIn || j.q == desert->key;
        k6.doKey = false;
        std::string pl6;
        for (const QJob& j : k6.make_plan(*s4.w, s4.bot.book)) {
            pl6 += fmt(" %d", j.q.id);
            for (const QKey& c : desert->chain) keyOut = keyOut && !(j.q == c);
        }
        check(keyIn && keyOut, "only sticker quests: the key quest and its chain left out of the plan:" + pl6);
    }
}

static void keyquest_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    keyquest_graph_dump(gd, pass, total);
    keyquest_run_test(gd, pass, total);
    printf("\n%d / %d checks passed\n", pass, total);
}

// ── the common gear grades (game_data.cpp equip_tier): not looted by default, sold by default ──
static void equip_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    printf("\nthe grades by name (weapons / helmets / shields):\n");
    const struct { uint32_t id; int tier; } cases[] = {
        { 600000, ET_Old }, { 600001, ET_None }, { 600002, ET_Strong }, { 600005, ET_Art }, { 600006, ET_Ult }, { 600007, ET_None },
        { 620002, ET_Strong }, { 640000, ET_Old }, { 610002, ET_Strong }, { 630022, ET_Ult }, { 40093, ET_None }, { 7284, ET_None },
        { 10156, ET_None },
    };
    for (const auto& k : cases) {
        const int t = gd.equip_tier(k.id);
        check(t == k.tier, fmt("%u %s -> %s", k.id, gd.item_name(k.id).c_str(), t == ET_None ? "none" : GameData::equip_tier_name(t)));
    }
    int counts[ET_Count] = {};
    for (uint32_t id = 1; id < 700000; ++id) { const int t = gd.equip_tier(id); if (t != ET_None) ++counts[t]; }
    printf("  in the item table: %d Old, %d Strong / Sharp, %d Art, %d Ult.\n", counts[0], counts[1], counts[2], counts[3]);

    printf("\nloot (an Old sword and a plain item on the ground):\n");
    auto loot_run = [&](bool skip, bool old) {
        StuckRig s(gd);
        s.login(2, 1000, 800);
        s.feed(DIR_S2C, 0x000A, Pk().u16(700).u16(1040).u16(800).u16(0).u8(0).u8(0).u8(0).u8(1).u32(600000).u64(9).u16(1).b);
        s.feed(DIR_S2C, 0x000A, Pk().u16(701).u16(1080).u16(820).u16(0).u8(0).u8(0).u8(0).u8(1).u32(101).u64(10).u16(1).b);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.loot.enabled = true;
        s.bot.loot.skipEquip = skip;
        for (int t = 0; t < ET_Count; ++t) s.bot.loot.lootTier[t] = t == ET_Old ? old : true;
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        std::set<int> picked;
        size_t seen = 0;
        for (int k = 0; k < 60; ++k) {
            s.run(0, 0.1);
            for (; seen < s.sent.size(); ++seen)
                if (s.sent[seen].op == 0x0021 && s.sent[seen].body.size() >= 2) {
                    const int oid = s.sent[seen].body[0] | s.sent[seen].body[1] << 8;
                    picked.insert(oid);
                    s.feed(DIR_S2C, 0x0022, Pk().u16(StuckRig::ME).u16(uint16_t(oid)).u8(1).b);   // (taken)
                }
        }
        return picked;
    };
    {
        const std::set<int> p = loot_run(true, true);
        check(p.count(701) && !p.count(700), "by default: the plain item picked up, the Old sword left on the ground");
    }
    {
        const std::set<int> p = loot_run(false, true);
        check(p.count(701) && p.count(700), "\"don't loot equipment\" off, Old checked: both picked up");
    }
    {
        const std::set<int> p = loot_run(false, false);
        check(p.count(701) && !p.count(700), "off, but Old unchecked: the Old sword left");
    }
    {
        LootAction l;
        l.items.white.insert(600000);
        check(l.equip_allowed(&gd, 600000) && !l.equip_allowed(&gd, 600005), "an item whitelisted by its id is looted anyway (only it)");
    }

    printf("\nsell (Supply):\n");
    {
        StuckRig s(gd);
        s.login(2, 1000, 800);
        TownAction& t = s.bot.town;
        InvItem old; old.uid = 1; old.itemId = 600000; old.count = 1; old.kind = 2;
        InvItem art = old; art.uid = 2; art.itemId = 600005;
        InvItem worn = old; worn.uid = 3; worn.equipSlot = 1;
        InvItem chaos = old; chaos.uid = 4; chaos.itemId = 600007;
        check(t.sellable(*s.w, old) && t.sellable(*s.w, art), "by default the Old and Art swords are sold (the list is a whitelist, empty)");
        check(!t.sellable(*s.w, worn), "never the one you wear");
        check(!t.sellable(*s.w, chaos), "a Chaos sword is not one of the grades: the list decides (not sold)");
        t.sellTier[ET_Art] = false;
        check(t.sellable(*s.w, old) && !t.sellable(*s.w, art), "Art unchecked: only the Old one");
        t.sellList.black.insert(600000);
        check(!t.sellable(*s.w, old), "blacklisted by its id: kept");
        t.keep.insert(600005);
        t.sellTier[ET_Art] = true;
        check(!t.sellable(*s.w, art), "an Item Hunt / Key Quest item (town.keep): kept");
    }

    printf("\nthe profile:\n");
    {
        Bot b;
        b.loot.skipEquip = false;
        b.loot.lootTier[ET_Strong] = false;
        b.town.sellTier[ET_Ult] = false;
        const std::string txt = b.save_text();
        Bot c;
        c.load_text(txt);
        check(!c.loot.skipEquip && c.loot.lootTier[ET_Old] && !c.loot.lootTier[ET_Strong] && c.town.sellTier[ET_Art] && !c.town.sellTier[ET_Ult],
              "the switches come back from the profile");
        Bot d;
        d.load_text("loot.enabled=1\n");
        check(d.loot.skipEquip && d.town.sellTier[ET_Old] && d.town.sellTier[ET_Ult], "an old profile: not looted, sold (the defaults)");
    }
    printf("\n%d / %d checks passed\n", pass, total);
}

// ── buffs in a town (skill.cpp: SkillOption 0x200 refused in map Type 1 / 5 / 6), the idle watchdog ──
static void misc_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const SkillInfo* si = gd.skill(1002);
    check(si && (si->skillOption & 0x200), fmt("1002 %s has SkillOption 0x200", si ? si->name.c_str() : "?"));
    check(gd.no_skill_zone(33) && gd.no_skill_zone(1) && !gd.no_skill_zone(2), "Beach Town - Paradise / Megalopolis Square: no such skills; map 2: allowed");

    printf("\na buff kept up, in a town then out of it:\n");
    {
        StuckRig s(gd);
        int tx = 600, ty = 600;
        if (const AtlasMap* m = gd.atlas().map(33))
            for (const AtlasPoint& p : m->points) if (p.type == 2 || p.type == 1) { tx = p.x; ty = p.y; break; }
        s.login(33, tx, ty);
        s.feed(DIR_S2C, 0x010F, Pk().u16(1).u16(1002).u8(1).u16(0).b);          // 271: the learned skills
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.buffs.enabled = true;
        s.bot.buffs.keep = { 1002 };
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        s.run(0, 4);
        bool cast = false;
        for (const Action& a : s.sent) cast = cast || a.op == 0x00EE || a.op == 0x00E0;
        check(s.w->skill_level(1002) == 1 && !cast, "in Beach Town - Paradise: not cast (" + s.bot.buffs.status + ")");
        int fx = 600, fy = 600;
        if (const AtlasMap* m = gd.atlas().map(2))
            for (const AtlasPoint& p : m->points) if (p.type == 2 || p.type == 1) { fx = p.x; fy = p.y; break; }
        s.enter_zone(2, fx, fy);
        Action a;
        const bool ok = s.run(0x00EE, 6, &a);
        check(ok, "in the field (map 2): cast (C2S 0x00EE)");
    }

    printf("\nthe idle watchdog:\n");
    {
        StuckRig s(gd);
        s.login(2, 1000, 800);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.loot.enabled = true;                                           // (nothing to pick up: it sends nothing)
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        s.run(0, 25);
        check(!s.logged("idle 30 s"), "25 s without a request: nothing yet");
        s.run(0, 8);
        check(s.logged("idle 30 s"), "after 30 s: the snapshot in the log");
        std::string line;
        for (const std::string& l : s.bot.log) if (l.find("idle 30 s") != std::string::npos) line = l;
        printf("    %s\n", line.c_str());
        const uint64_t n = s.bot.notes;
        s.run(0, 30);
        check(s.bot.notes == n, "not again within a minute");
    }

    // area skills (user: "support area skills"): Mana Ring (2008, aimed at a monster) and Mana Storm (2009, a point of the
    // ground) go where they take the most monsters, only when that many reach the skill's "min mobs"
    // Scorching Earth (user: "counted as a buff"): Type 3, but AffectionRel 32 (enemies) = a fire field on the ground, the
    // Attack's; Mist of Mana (Type 3, AffectionRel 1) stays a buff
    {
        const SkillInfo* se = gd.skill(2129);
        uint16_t mist = 0;
        for (uint16_t id = 1000; id < 9000 && !mist; ++id) if (gd.skill(id) && gd.skill_name(id) == "Mist of Mana") mist = id;
        const SkillInfo* mm = mist ? gd.skill(mist) : nullptr;
        check(se && se->type == 3 && !se->buff() && esaui_point(se->esaui),
              fmt("Scorching Earth (2129): Type %d, AffectionRel %d -> not a buff, a point skill of the attack", se ? se->type : -1,
                  se ? se->affectionRel : -1));
        check(mm && mm->buff(), fmt("Mist of Mana (%u): a buff", mist));
        StuckRig s(gd);
        s.login(2, 1000, 800, 120);
        s.feed(DIR_S2C, 0x010F, Pk().u16(1).u16(2129).u8(5).u16(0).b);
        s.monster(701, 1100, 800, "A");
        s.bot.buffs.keep = { 2129 };                       // (an old profile that kept it as a buff)
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.buffs.enabled = true;
        s.bot.sync_modes();
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        s.run(0, 3);
        bool self = false;
        for (const Action& a : s.sent)
            if (a.op == 0x00E0 || a.op == 0x00EE) self = true;
        check(!self && AttackAction::area_radius(*s.w, 2129) > 0.0f, "kept in the Buffs: never cast on ourselves; an area for the Attack (radius " +
              fmt("%.0f)", AttackAction::area_radius(*s.w, 2129)));
    }
    printf("\narea skills: three monsters together, one apart:\n");
    {
        StuckRig s(gd);
        s.login(2, 1000, 800, 100);
        s.feed(DIR_S2C, 0x010F, Pk().u16(2).u16(2008).u8(5).u16(0).u16(2009).u8(5).u16(0).b);
        const float ring = AttackAction::area_radius(*s.w, 2008), storm = AttackAction::area_radius(*s.w, 2009);
        s.monster(701, 1150, 800, "A");
        s.monster(702, 1150 + int(ring * 0.4f), 820, "B");
        s.monster(703, 1150, 800 + int(ring * 0.5f), "C");
        s.monster(704, 700, 1300, "D");
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        BotCtx ctx{ *s.w, s.now, 1000, 800, false, s.bot, Action{} };
        const AreaSpot r1 = s.bot.attack.area_spot(ctx, 2008);
        const AreaSpot r2 = s.bot.attack.area_spot(ctx, 2009);
        printf("    radius: Mana Ring %.0f, Mana Storm %.0f\n", ring, storm);
        check(ring > 0 && r1.count == 3 && r1.eid >= 701 && r1.eid <= 703, fmt("Mana Ring: aimed at one of the three (#%u), 3 in its area", r1.eid));
        check(storm > 0 && r2.count == 3, fmt("Mana Storm: a point taking the three (%d,%d), 3 in its area", r2.x, r2.y));
        // min mobs 4: not cast; 3: cast at the spot
        s.bot.attack.enabled = true;
        s.bot.attack.basic = false;
        s.bot.attack.rotation = { RotationSkill{ 2009, true, 4 } };
        s.bot.sync_modes();
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        s.run(0, 4);
        bool cast = false;
        for (const Action& a : s.sent) cast = cast || a.op == 0x00E0;
        check(!cast, "min mobs 4 with 3 there: Mana Storm not cast (" + s.bot.attack.status + ")");
        s.bot.attack.rotation = { RotationSkill{ 2009, true, 3 } };
        Action a;
        const bool ok = s.run(0x00E0, 4, &a);
        int px = -1, py = -1;
        if (ok && a.body.size() >= 12) { px = a.body[8] | a.body[9] << 8; py = a.body[10] | a.body[11] << 8; }
        int inArea = 0;
        for (const auto& kv : s.w->entities)
            if (kv.second.kind == EK_MONSTER && kv.first >= 701 && kv.first <= 703)
                inArea += std::hypot(double(kv.second.x - px), double(kv.second.y - py)) <= storm;
        check(ok && inArea == 3, fmt("min mobs 3: Mana Storm cast at (%d,%d), a spot that takes the three (%d in its area)", px, py, inArea));
    }

    // the moves between maps (a live log: 7-18 maps round, a Hologram Port spent, back into the map it was in)
    printf("\nthe routes:\n");
    {
        const Atlas& at = gd.atlas();
        RouteOptions o;
        o.level = 120;
        o.money = 100000;
        o.goalX = 1808;
        o.goalY = 470;
        const Route same = at.route(gd, 25, 2780, 299, 25, o);
        check(same.ok && same.hops.empty(), "a point of the map we are in: a walk, never a way round through other maps");
        // ways left out for a while: no port item spent (Swamp Town with every way out left out, a Hologram Port in the bag)
        RouteOptions b;
        b.level = 120;
        b.money = 100000;
        b.bagKnown = true;
        b.bag[11797] = 1;
        if (const AtlasMap* m = at.map(69)) {
            for (size_t i = 0; i < m->portals.size(); ++i) {
                RouteHop h;
                h.kind = RouteHop::Portal;
                h.fromZone = 69;
                h.portal = int(i);
                b.banned.insert(Atlas::hop_key(h));
            }
            for (int t : m->teleporters)
                for (const TeleportSlot& sl : at.npc(t)->teleports) {
                    RouteHop h;
                    h.kind = RouteHop::Teleport;
                    h.fromZone = 69;
                    h.npc = t;
                    h.slot = sl.slot;
                    b.banned.insert(Atlas::hop_key(h));
                }
        }
        const Route r = at.route(gd, 69, 2024, 2024, 118, b);
        bool item = false;
        for (const RouteHop& h : r.hops) item = item || h.kind == RouteHop::Item;
        check(!item, "every way out left out for a while: no Hologram Port used (" + std::string(r.ok ? "a route" : r.why) + ")");
    }
    // a portal that does not take us: once more, then left out; three left out in 2 min = no detours, a wait
    printf("\na portal that never takes us (Relics Field 1 -> Relics Field 2):\n");
    {
        StuckRig s(gd);
        s.login(8, 2900, 3700, 120);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.town.enabled = false;
        s.bot.travelOnly = false;
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        Navigator nav;
        Navigator::Goal g;
        g.zone = 25;
        nav.go(g);
        for (int k = 0; k < 900; ++k) {
            s.now += 100 * 10000ull;
            BotCtx c{ *s.w, s.now, 0, 0, false, s.bot, Action{} };
            s.w->self_pos_at(s.now, c.px, c.py);
            if (nav.step(c) == Step::Act && c.out.ok()) { s.sent.push_back(c.out); s.feed(DIR_C2S, c.out.op, c.out.body); }
        }
        int fails = 0, maxMaps = 0;
        for (const std::string& l : s.bot.log) {
            fails += l.find("did not take us") != std::string::npos;
            const size_t p = l.find("route to Relics Field 2");
            if (p != std::string::npos) maxMaps = std::max(maxMaps, std::atoi(l.c_str() + l.find(": ", p) + 2));
        }
        for (const std::string& l : s.bot.log) if (l.find("did not take") != std::string::npos || l.find("3 ways") != std::string::npos ||
                                                  l.find("way round") != std::string::npos) printf("    %s\n", l.c_str());
        check(s.logged("did not take us: once more"), "the first failure: the same portal once more");
        check(s.logged("3 ways failed") || s.logged("the way round would take") || maxMaps <= 4,
              fmt("no runaway detour (the longest route planned: %d maps)", maxMaps));
        check(fails >= 2, fmt("%d failures seen", fails));
    }
    printf("\n%d / %d checks passed\n", pass, total);
}

// ── the Story (quest_graph.cpp chapters + bot/act_story.cpp): the chapters over the real dialogs ──
static void story_graph_dump(const GameData& gd, int& pass, int& total)
{
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const Atlas& at = gd.atlas();
    const QuestGraph& g = gd.quest_graph();
    const std::string capDir = exe_dir() + "captures\\";
    CreateDirectoryA(capDir.c_str(), nullptr);
    std::ofstream out(capDir + "story_graph.txt", std::ios::binary);
    const std::vector<QuestGraph::Chapter>& chs = g.chapters();
    size_t covered = 0;
    for (const auto& c : chs) covered += c.quests.size();
    check(chs.size() == 23, fmt("%zu chapters (episodes 0..6), %zu of the %zu scenario quests in them", chs.size(), covered,
                                gd.scenario_quests().size()));
    TestView v(gd);
    int ok = 0, blocked = 0, monq = 0, gives = 0;
    std::map<std::string, int> whyCount;
    QuestBook book;
    for (size_t i = 0; i < chs.size(); ++i) {
        const QuestGraph::Chapter& c = chs[i];
        v.lv = std::max(1, c.minLevel);
        v.fresh = true;
        std::vector<QJob> jobs;
        const bool good = g.expand(QKey{ false, c.last, true }, 1, v, jobs);
        bool allGood = good;
        for (const QJob& j : jobs) { allGood = allGood && !j.blocked; monq += j.q.mon; }
        allGood ? ++ok : ++blocked;
        out << fmt("\n## Episode %d chapter %d \"%s\" (%d..%d, Lv %d): %zu quests, plan %zu jobs -> %s\n", c.episode, c.chapter, c.name.c_str(),
                   c.first, c.last, c.minLevel, c.quests.size(), jobs.size(), allGood ? "ok" : "BLOCKED");
        for (const QJob& j : jobs) {
            const QuestPlaces* qp = g.places(j.q);
            std::string take = "-", hand = "-";
            if (qp && j.accept >= 0) {
                const QPlace& p = qp->accepts[size_t(j.accept)];
                take = fmt("%s @ %s", at.npc(p.npc)->name.c_str(), gd.map_name(at.npc(p.npc)->zone).c_str());
            }
            if (qp && j.complete >= 0 && size_t(j.complete) < qp->completes.size()) {
                const QPlace& p = qp->completes[size_t(j.complete)];
                hand = at.npc(p.npc)->name;
            }
            std::string items;
            if (const QuestInfo* qi = g.info(j.q))
                for (const QuestNeed& n : qi->src) {
                    const bool src = v.gatherable(n.item);
                    items += fmt("%s%s x%d%s", items.empty() ? "" : ", ", gd.item_name(n.item).c_str(), n.cnt, src ? "" : "*");
                }
            out << fmt("   %c%-5d %-42s %s | take %s | hand in %s | %s\n", j.q.tag(), j.q.id, g.quest_name(j.q).c_str(),
                       j.blocked ? ("BLOCKED: " + j.why).c_str() : "", take.c_str(), hand.c_str(), items.c_str());
            if (j.blocked) {
                ++whyCount[j.why.substr(0, 70)];
                out << "      why: " << g.explain(j.q, v) << "\n";
            }
        }
        // the runner's providers (another quest's reward, an NPC's gift)
        StuckRig s(gd);
        s.login(2, 1000, 800);
        StoryAction st;
        st.fromChapter = int(i);
        st.toChapter = int(i);
        const std::vector<QJob> plan = st.make_plan(*s.w, book);
        for (const QJob& j : plan) gives += j.giveItem != 0;
        std::string extra;
        for (const QJob& j : plan) {
            bool in = false;
            for (const QJob& x : jobs) in = in || x.q == j.q;
            if (!in) extra += fmt(" %s%s", j.giveItem ? "give:" : "", j.giveItem ? gd.item_name(j.giveItem).c_str() : fmt("%c%d", j.q.tag(), j.q.id).c_str());
        }
        if (!extra.empty()) out << "   + providers:" << extra << "\n";
    }
    printf("  chapters: %d with a way, %d blocked (a fresh character); %d monster quest steps; %d NPC gifts\n", ok, blocked, monq, gives);
    for (const auto& w : whyCount) printf("    blocked: %2d x %s\n", w.second, w.first.c_str());
    printf("  (dump: captures\\story_graph.txt)\n");
    check(ok * 10 >= int(chs.size()) * 7, fmt("at least 70 %% of the chapters have a way (%d / %zu)", ok, chs.size()));
}

// The maps only an item opens (atlas.cpp route: NPC moves, port items; quest_graph.cpp QCond::Enter).
static void story_entry_checks(const GameData& gd, int& pass, int& total)
{
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const Atlas& at = gd.atlas();
    const QuestGraph& g = gd.quest_graph();
    printf("  %s\n", at.load_report().c_str());
    auto route = [&](int from, int to, const std::map<uint32_t, int>& bag) {
        RouteOptions o;
        o.level = 200;
        o.money = 100000;
        o.bagKnown = true;
        o.bag = bag;
        const AtlasMap* m = at.map(from);
        int x = 500, y = 400;
        if (m) for (const AtlasPoint& p : m->points) if (p.type == 7 || p.type == 2) { x = p.x; y = p.y; break; }
        return at.route(gd, from, x, y, to, o);
    };
    auto text = [&](const Route& r) {
        if (!r.ok) return "no way: " + r.why;
        std::string s;
        for (const RouteHop& h : r.hops)
            s += fmt(" > %s%s", h.kind == RouteHop::Item ? "[item] " : h.move ? "[move] " : h.kind == RouteHop::Teleport ? "[tp] " : "",
                     at.map_label(h.toZone).c_str());
        return s;
    };
    // Mirage Island Field 1 (271) -> Ocean's End (274): Love Hunter Robin's move, the Contract of Secrecy in the bag (kept)
    const Route noContract = route(271, 274, {});
    const Route contract = route(271, 274, { { 11730, 1 } });
    printf("    271 -> 274 without the contract:%s\n    with it:%s\n", text(noContract).c_str(), text(contract).c_str());
    check(!noContract.ok, "Ocean's End: no way without the Contract of Secrecy");
    check(contract.ok && !contract.hops.empty() && contract.hops.back().move, "with it: Love Hunter Robin's NPC move");
    // Coral Town (148) -> Hologram Chamber (289): the Hologram Port (used up: only when nothing else gets there)
    const Route port = route(148, 289, { { 11797, 1 } });
    printf("    148 -> 289 with the Hologram Port:%s\n", text(port).c_str());
    check(port.ok && port.hops.size() == 1 && port.hops.front().kind == RouteHop::Item && port.hops.front().item == 11797,
          "Hologram Chamber: the Hologram Port where we stand");
    const Route back = route(289, 148, {});
    printf("    289 -> 148:%s\n", text(back).c_str());
    check(back.ok && !back.hops.empty() && back.hops.front().move, "out of it: Don Cavalier's NPC move");
    // a port item is never used where a walk gets there (Witch's broom / return wings stay in the bag)
    const Route town = route(54, 148, { { 11909, 1 } });
    bool usedWing = false;
    for (const RouteHop& h : town.hops) usedWing = usedWing || h.kind == RouteHop::Item;
    check(town.ok && !usedWing, "an open map: no port item used" + text(town));
    // an NPC on a walled-in pocket of the grid (Indiana John in Relics Field 2: 8 cells) — the route goes to NEAR it
    {
        RouteOptions o;
        o.level = 120;
        o.money = 100000;
        o.goalX = 1808;
        o.goalY = 470;
        const Route r = at.route(gd, 8, 1672, 1592, 25, o);
        check(r.ok && r.hops.size() == 1, "Relics Field 1 -> Indiana John (Relics Field 2, a pocket of 8 cells): a way" + text(r));
    }
    // Random_Move (Peach of mischievous: one of the 1F classrooms at random)
    {
        RouteOptions o;
        o.level = 100;
        const Route r = at.route(gd, 111, 3600, 700, 113, o);
        const AtlasNpc* n = !r.hops.empty() ? at.npc(r.hops.front().npc) : nullptr;
        const TeleportSlot* sl = n && r.hops.front().slot >= 0 && size_t(r.hops.front().slot) < n->teleports.size()
                               ? &n->teleports[size_t(r.hops.front().slot)] : nullptr;
        check(r.ok && sl && sl->random, "Phantom School 1F -> classroom 1-2: Peach of mischievous' Random_Move" + text(r));
    }
    // the Portable Port AD (19501, teleport_adv_db): a shortcut used freely, never for a walk it does not beat
    {
        const Route ad = route(148, 168, { { 19501, 1 } });
        const Route noAd = route(148, 168, {});
        printf("    148 -> 168 with the Portable Port AD:%s (~%.0f s)\n    without:%s (~%.0f s)\n", text(ad).c_str(), ad.secs,
               text(noAd).c_str(), noAd.secs);
        check(ad.ok && ad.hops.size() == 1 && ad.hops.front().kind == RouteHop::Item && ad.hops.front().advIndex == 19 &&
              ad.hops.front().item == 19501, "Coral Town -> Gate of Vamp Castle: the Port AD, index 19");
        check(noAd.ok && noAd.hops.size() > 3, "without it: the long way round");
        const Route via = route(148, 107, { { 19501, 1 } });
        const Route viaNo = route(148, 107, {});
        check(via.ok && !via.hops.empty() && via.hops.front().advIndex >= 0 && via.secs + 30 < viaNo.secs,
              fmt("Coral Town -> Vamp Dungeon 1 (no gate door in the data): the Port AD first, ~%.0f s against ~%.0f s",
                  via.secs, viaNo.secs) + text(via));
        const Route oneMap = route(8, 25, { { 19501, 1 } });
        bool usedAd = false;
        for (const RouteHop& h : oneMap.hops) usedAd = usedAd || h.kind == RouteHop::Item;
        check(oneMap.ok && !usedAd, "Relics Field 1 -> 2 (one portal): no Port AD" + text(oneMap));
        RouteOptions o;
        o.level = 200;
        o.money = 100000;
        o.bagKnown = true;
        o.bag[19501] = 1;
        o.portAd = false;
        const Route off = at.route(gd, 148, ad.hops.empty() ? 300 : ad.hops.front().x, ad.hops.empty() ? 300 : ad.hops.front().y, 168, o);
        check(off.ok && !off.hops.empty() && off.hops.front().kind != RouteHop::Item, "the Travel switch off: no Port AD" + text(off));
        // an Option 128 map: not from there (S2C 799 [4])
        o.portAd = true;
        int noPortZone = 0;
        for (const auto& kv : at.maps())
            if (kv.second.noPort() && kv.first != 168 && at.zone_reachable(kv.first, 168, o)) { noPortZone = kv.first; break; }
        if (noPortZone) {
            const AtlasMap* m = at.map(noPortZone);
            int x = 500, y = 400;
            for (const AtlasPoint& p : m->points) if (p.type == 7 || p.type == 2 || p.type == 1) { x = p.x; y = p.y; break; }
            const Route r = at.route(gd, noPortZone, x, y, 168, o);
            check(r.ok && !r.hops.empty() && r.hops.front().kind != RouteHop::Item,
                  fmt("from %s (Option 128): no Port AD where we stand", at.map_label(noPortZone).c_str()) + text(r));
        }
    }
    // the graph: the Hologram Chamber's NPC wants the Hologram Port (or being in there)
    int root = 0;
    check(g.entry_item(289, &root) == 11797 && root == 289, fmt("Hologram Chamber opened by %s", gd.item_name(g.entry_item(289)).c_str()));
    check(g.entry_item(274) == 11730, fmt("Ocean's End opened by %s", gd.item_name(g.entry_item(274)).c_str()));
    check(!g.exchanges(64111).empty(), fmt("Approved Memory Kit 2 from %zu exchange(s) (JOKER)", g.exchanges(64111).size()));
}

// The live log of 2026-10-23 (Rose Fields, key quest): the bot stood 60 s at a time at the route planner for NPCs in maps
// it had no way to (Butler of Vamp Castle in Vamp Dungeon 1, a teleport's level), walked again and again to an NPC the
// server did not put there (Rosemary), and went minutes without progress. Now: such a job waits ("no way now", no walk),
// an absent NPC is left 10 min at the first miss (the log names the NPCs that are there), the runner idle lets the Attack
// fight, and the stall watch logs what it was doing and steps in: errand dropped, a step aside, a restart, then a stop.
static void stall_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const QuestGraph& g = gd.quest_graph();
    const KeyTown* town = nullptr;
    for (const KeyTown& t : g.towns())
        for (const StickerQuest& sq : t.quests) if (sq.q.id == 672) town = &t;
    if (!town) { printf("no town with quest 672 (Butler's request)\n"); return; }
    printf("\nkey quest %s, quest 672 \"Butler's request\" (Butler of Vamp Castle, Vamp Dungeon 1) from Rose Field 1 at level 130:\n",
           town->name.c_str());
    StuckRig s(gd);
    s.login(89, 1240, 3368, 130);                         // (672 wants level 130; the teleport to it 135)
    for (BotAction* a : s.bot.actions()) a->enabled = false;
    s.bot.keyq.enabled = true;
    s.bot.keyq.sticker = town->sticker;
    s.bot.keyq.picked = { 672 };
    s.bot.keyq.doKey = false;
    s.bot.sync_modes();
    s.run(0, 3.5);
    s.bot.start(*s.w, s.now);
    s.run(0, 15);
    std::string plan;
    int noWay = -1;
    for (size_t i = 0; i < s.bot.keyq.jobs().size(); ++i) {
        const QJob& j = s.bot.keyq.jobs()[i];
        const int st = i < s.bot.keyq.job_info().size() ? s.bot.keyq.job_info()[i].state : -1;
        plan += fmt(" %c%d=%s", j.q.tag(), j.q.id, st >= 0 ? QuestRunner::state_name(st) : "?");
        if (j.q.id == 672) noWay = st;
    }
    printf("  plan:%s\n", plan.c_str());
    for (const std::string& l : s.bot.log) printf("    log: %s\n", l.c_str());
    check(noWay == QuestRunner::JNoWay, "672: \"no way now\" (Vamp Dungeon 1 is not reached at level 130) - no walk toward it");
    check(!s.logged("no route: no way from Rose Field 1 - Stairway to Sorrow (89) to Vamp Dungeon 1"),
          "the navigator was never sent toward Vamp Dungeon 1 (no 60 s at the route planner)");
    bool absentNote = false;
    for (const std::string& l : s.bot.log) absentNote = absentNote || (l.find("is not in") != std::string::npos && l.find("NPCs here") != std::string::npos);
    const bool rosemary = plan.find("q671") != std::string::npos;
    check(!rosemary || absentNote, rosemary ? "Rosemary (671 first) not there: the log says so and names the NPCs that are"
                                            : "(671 not in the plan)");
    check(!s.bot.running && s.bot.stopWhy.find("no way to Butler of Vamp Castle") != std::string::npos,
          "its only job has no way: the bot stops at once and says why (not 60 s standing, again and again)");
    {
        StuckRig s40(gd);                                   // at level 40: under its MinLevel, never asked for
        s40.login(89, 1240, 3368, 40);
        for (BotAction* a : s40.bot.actions()) a->enabled = false;
        s40.bot.keyq.enabled = true;
        s40.bot.keyq.sticker = town->sticker;
        s40.bot.keyq.picked = { 672 };
        s40.bot.keyq.doKey = false;
        s40.bot.sync_modes();
        s40.run(0, 3.5);
        s40.bot.start(*s40.w, s40.now);
        s40.run(0, 15);
        printf("  at level 40: %s\n", s40.bot.stopWhy.c_str());
        check(!s40.bot.running && s40.bot.stopWhy.find("waits for level 130 (now 40)") != std::string::npos,
              "at level 40: the bot stops and says the quest waits for level 130");
    }
    // the stall watch: quest 255's NPC stands there but never answers (no server) — the talks fail, the job waits, nothing
    // moves on: the notes, the remedies, then the stop
    const QuestPlaces* qp = g.places(QKey{ false, 255 });
    const KeyTown* desert = nullptr;
    for (const KeyTown& t : g.towns()) if (t.name == "Desert") desert = &t;
    if (!qp || qp->accepts.empty() || !desert) { printf("no Desert / quest 255\n"); return; }
    const AtlasNpc* npc = gd.atlas().npc(qp->accepts.front().npc);
    printf("\nnothing moves on: Desert quest 255 at %s, who never answers, no monster around:\n", npc->name.c_str());
    StuckRig s2(gd);
    int sx = npc->x + 40, sy = npc->y;
    if (const MapGrid* mg = gd.map(npc->zone)) if (mg->blocked(sx, sy)) { sx = npc->x; sy = npc->y + 40; }
    s2.login(npc->zone, sx, sy);
    s2.npc(910, npc->species, npc->x, npc->y);
    for (BotAction* a : s2.bot.actions()) a->enabled = false;
    s2.bot.keyq.enabled = true;
    s2.bot.keyq.sticker = desert->sticker;
    s2.bot.keyq.picked = { 255 };
    s2.bot.keyq.doKey = false;
    s2.bot.sync_modes();
    s2.run(0, 3.5);
    s2.bot.start(*s2.w, s2.now);
    s2.run(0, 360);
    bool stepAside = false;
    for (const Action& a : s2.sent) stepAside = stepAside || (a.op == 0x0018 && a.label.find("move") != std::string::npos);
    for (const std::string& l : s2.bot.log)
        if (l.find("progress") != std::string::npos || l.find("remedy") != std::string::npos || l.find("stopped") != std::string::npos)
            printf("    log: %s\n", l.c_str());
    check(s2.logged("no progress 60 s"), "after 60 s without progress: what it was doing and every action's status in the log");
    check(s2.logged("remedy 1") && s2.logged("remedy 2: a step aside"), "remedy 1, then a step aside (a C2S 0x18)");
    check(s2.logged("remedy 3"), "then every action started over");
    check(!s2.bot.running && s2.bot.stopWhy.find("no progress for 5 min") != std::string::npos,
          "then the stop with the reason (" + s2.bot.stopWhy + ")");
    // the live log of 23:22..23:47: the NPC not at its point, one of the same look 600+ units away — the bot walked to
    // that one, the walk to the point pulled it back, back and forth for minutes. Now: never chased; the NPC is "not at
    // its point" (what stands there in the log), the job left 10 min.
    {
        printf("\nthe NPC is not at its point, one of the same look 700 units away (Desert quest 255, %s):\n", npc->name.c_str());
        StuckRig p(gd);
        int px0 = npc->x + 40, py0 = npc->y;
        if (const MapGrid* mg0 = gd.map(npc->zone)) if (mg0->blocked(px0, py0)) { px0 = npc->x; py0 = npc->y + 40; }
        p.login(npc->zone, px0, py0);
        const int fx = npc->x + 700, fy = npc->y;
        p.npc(911, npc->species, fx, fy);
        for (BotAction* a : p.bot.actions()) a->enabled = false;
        p.bot.keyq.enabled = true;
        p.bot.keyq.sticker = desert->sticker;
        p.bot.keyq.picked = { 255 };
        p.bot.keyq.doKey = false;
        p.bot.sync_modes();
        p.run(0, 3.5);
        p.bot.start(*p.w, p.now);
        p.run(0, 40);
        bool chased = false, talked = false;
        for (const Action& a : p.sent) {
            talked = talked || a.op == 0x006F;
            if (a.op == 0x0018 && a.body.size() >= 8) {
                const int dx = a.body[4] | a.body[5] << 8;
                chased = chased || dx > npc->x + 400;
            }
        }
        for (const std::string& l : p.bot.log) if (l.find("point") != std::string::npos) printf("    log: %s\n", l.c_str());
        check(!chased, "no walk toward the one of the same look far away (no back and forth)");
        check(!talked, "no talk to it either");
        check(p.logged("is not at its point") && p.logged("the same look elsewhere"), "the log: not at its point, what stands there, the look-alike");
        int st = -1;
        for (size_t i = 0; i < p.bot.keyq.jobs().size() && i < p.bot.keyq.job_info().size(); ++i)
            if (p.bot.keyq.jobs()[i].q.id == 255) st = p.bot.keyq.job_info()[i].state;
        check(st == QuestRunner::JHeld || !p.bot.running, fmt("the job left alone (%s)", st >= 0 ? QuestRunner::state_name(st) : p.bot.stopWhy.c_str()));
    }

    // a hunt with kills: never a stall
    printf("\nprogress keeps it quiet: a kill every 40 s for 4 min\n");
    StuckRig h(gd);
    h.login(89, 1240, 3368);
    for (BotAction* a : h.bot.actions()) a->enabled = false;
    h.bot.keyq.enabled = true;
    h.bot.keyq.sticker = town->sticker;
    h.bot.keyq.picked = { 672 };
    h.bot.sync_modes();
    h.run(0, 3.5);
    h.bot.start(*h.w, h.now);
    for (int k = 0; k < 6; ++k) {
        h.run(0, 40);
        h.feed(DIR_S2C, 0x0406, Pk().u32(uint32_t(1000 * (k + 1))).u32(0).b);
    }
    check(!h.logged("no progress"), "EXP every 40 s: no stall note");

    // the drill (live log 22:53 -> 23:00: 6.5 min without a line): a server that refuses every dig (S2C 0x2E) / one that
    // never answers — no cell is asked more than twice, the log says why, the block is left
    const int ZONE = 2;
    const MapGrid* mg = gd.map(ZONE);
    int bx0 = -1, by0 = -1;
    for (int r = 0; mg && r < 40 && bx0 < 0; ++r)
        for (int ky = -r; ky <= r && bx0 < 0; ++ky)
            for (int kx = -r; kx <= r && bx0 < 0; ++kx) {
                if (std::max(std::abs(kx), std::abs(ky)) != r) continue;
                const int x0 = 1 + (mg->w / 2 / 10 + kx) * 10, y0 = 1 + (mg->h / 2 / 10 + ky) * 10;
                bool ok = x0 > 0 && y0 > 0;
                for (int y = 0; y < 11 && ok; ++y)
                    for (int x = 0; x < 11 && ok; ++x) ok = mg->diggable(x0 + x, y0 + y) && mg->raw(x0 + x, y0 + y) == 0;
                if (ok) { bx0 = x0; by0 = y0; }
            }
    if (bx0 < 0) { printf("no open block in map 2\n"); printf("\n%d / %d checks passed\n", pass, total); return; }
    for (int mode = 0; mode < 2; ++mode) {
        printf("\nthe drill, a server that %s:\n", mode == 0 ? "refuses every dig (S2C 0x2E)" : "never answers a dig request");
        StuckRig d(gd);
        const int X0 = (bx0 + 1) * 16 + 8, Y0 = (by0 + 1) * 16 + 8;
        d.login(ZONE, X0, Y0);
        d.feed(DIR_S2C, 0x00E7, Pk().u16(1).u8(2).u32(8200).u64(0xD1).u16(300).u32(0).u8(0).b);
        Pk p; p.zeros(278);
        auto put16 = [&](size_t o, uint16_t v) { p.b[o] = v & 0xFF; p.b[o + 1] = v >> 8; };
        auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) p.b[o + i] = (v >> (8 * i)) & 0xFF; };
        for (int i = 0; i < 8; ++i) p.b[size_t(7 * 8 + i)] = uint8_t((0xD1ull >> (8 * i)) & 0xFF);   // slot 7: the Blue Drill
        put16(184 + 12, 40); put16(184 + 14, 40); put16(184 + 30, 40);
        put16(219, uint16_t(ZONE)); put32(217 + 14, 50000); put32(217 + 18, 1000); put32(217 + 22, 500);
        d.feed(DIR_S2C, 0x00E9, p.b);
        for (BotAction* a : d.bot.actions()) a->enabled = false;
        d.bot.drill.enabled = true;
        d.bot.sync_modes();
        d.bot.start(*d.w, d.now);
        std::map<uint32_t, int> asks;
        size_t seen = 0;
        for (int k = 0; k < 900; ++k) {
            d.run(0, 0.1);
            for (; seen < d.sent.size(); ++seen) {
                const Action& a = d.sent[seen];
                if (a.op != 0x002C || a.body.size() < 4) continue;
                const int x = a.body[0] | a.body[1] << 8, y = a.body[2] | a.body[3] << 8;
                ++asks[WorldModel::cell_key(x / 16, y / 16)];
                if (mode == 0) d.feed(DIR_S2C, 0x002E, {});
            }
        }
        int most = 0;
        for (const auto& kv : asks) most = std::max(most, kv.second);
        for (const std::string& l : d.bot.log)
            if (l.find("refused") != std::string::npos || l.find("no answer") != std::string::npos || l.find("block") != std::string::npos ||
                l.find("again:") != std::string::npos || l.find("pauses") != std::string::npos)
                printf("    log: %s\n", l.c_str());
        check(!asks.empty() && most <= 2, fmt("%zu cells asked, none more than twice (most: %d)", asks.size(), most));
        check(d.logged(mode == 0 ? "the server refused the dig (S2C 0x2E)" : "no answer to the dig request"), "the log says why each one was left");
        if (mode == 0) check(d.logged("3 digs refused in a minute"), "3 refusals in a minute: the drill pauses 30 s (a condition / no drill?)");
        check(d.logged("cells whose dig never started"), "the block where the digs never start is left (another area)");
    }
    // a town whose quests span two levels (a live log: Swamp's level-150 quests asked for at 140, refused, again and
    // again): every one of our level first, the others wait for theirs (the server's take answers 3 under MinLevel)
    {
        const KeyTown* two = nullptr;
        int lo = 0, hi = 0;
        for (const KeyTown& t : g.towns()) {
            std::set<int> lvs;
            for (const StickerQuest& sq : t.quests) if (const QuestInfo* qi = gd.quest(sq.q.id)) lvs.insert(qi->minLevel);
            if (t.name.find("Swamp") != std::string::npos && lvs.size() >= 2) { two = &t; lo = *lvs.begin(); hi = *lvs.rbegin(); }
        }
        if (!two) { printf("no Swamp town with two levels\n"); printf("\n%d / %d checks passed\n", pass, total); return; }
        const int at = (lo + hi) / 2;
        printf("\nkey quest %s: quests of level %d and %d, the character at %d:\n", two->name.c_str(), lo, hi, at);
        const QuestPlaces* kp = g.places(two->quests.front().q);
        const AtlasNpc* kn = kp && !kp->accepts.empty() ? gd.atlas().npc(kp->accepts.front().npc) : nullptr;
        StuckRig s(gd);
        s.login(kn ? kn->zone : 69, kn ? kn->x : 1000, kn ? kn->y : 1000, at);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.keyq.enabled = true;
        s.bot.keyq.sticker = two->sticker;
        for (const StickerQuest& sq : two->quests) s.bot.keyq.picked.insert(sq.q.id);
        s.bot.keyq.doKey = false;
        KeyQuestAction& k = s.bot.keyq;
        k.refresh(*s.w, s.now, false, s.bot.book);
        int high = 0, highWait = 0, lowReady = 0, low = 0, firstHigh = -1, lastLow = -1;
        std::string plan;
        std::set<uint32_t> highItems, lowItems;
        for (size_t i = 0; i < k.jobs().size() && i < k.job_info().size(); ++i) {
            const QJob& j = k.jobs()[i];
            const QuestInfo* qi = j.giveItem || j.q.mon ? nullptr : g.info(j.q);
            if (!qi) continue;
            const int st = k.job_info()[i].state;
            plan += fmt(" %c%d(%d)=%s", j.q.tag(), j.q.id, qi->minLevel, QuestRunner::state_name(st));
            bool own = false;                               // (a quest a sticker quest waits for joins after them)
            for (const StickerQuest& sq : two->quests) own = own || sq.q == j.q;
            if (qi->minLevel > at) {
                ++high;
                highWait += st == QuestRunner::JLevel;
                if (firstHigh < 0 && own) firstHigh = int(i);
                for (const QuestNeed& n : qi->src) highItems.insert(n.item);
            } else {
                ++low;
                lowReady += st == QuestRunner::JReady || st == QuestRunner::JLater || st == QuestRunner::JNoWay;
                if (own) lastLow = int(i);
                for (const QuestNeed& n : qi->src) lowItems.insert(n.item);
            }
        }
        printf("  plan:%s\n", plan.c_str());
        check(high > 0 && highWait == high, fmt("the %d quest(s) of level %d: \"waits for its level\"", high, hi));
        check(low > 0 && lowReady == low, fmt("the %d of level %d: to be taken", low, lo));
        check(lastLow < firstHigh, "the lower level first in the plan (the town's quests)");
        bool onlyLow = true;
        for (const HuntItem& h : k.gather.list) onlyLow = onlyLow && (lowItems.count(h.item) || !highItems.count(h.item));
        check(!k.gather.list.empty() && onlyLow, fmt("the items asked for: the level-%d quests' only (%zu)", lo, k.gather.list.size()));
        // the level-150 ones never begin: no take of them in what the bot sends
        s.bot.sync_modes();
        s.bot.start(*s.w, s.now);
        s.run(0, 20);
        bool highAsked = false;
        for (const std::string& l : s.bot.log)
            for (const StickerQuest& sq : two->quests)
                if (const QuestInfo* qi = gd.quest(sq.q.id))
                    if (qi->minLevel > at && l.find("take " + g.quest_name(sq.q)) != std::string::npos) highAsked = true;
        check(!highAsked, fmt("no level-%d quest taken at %d", hi, at));
        // everything of our level done: the stop says what waits
        for (const StickerQuest& sq : two->quests)
            if (const QuestInfo* qi = gd.quest(sq.q.id)) if (qi->minLevel <= at) s.bot.book.set(sq.q, sq.runs, sq.runs);
        KeyQuestAction& k2 = s.bot.keyq;
        k2.refresh(*s.w, s.now, false, s.bot.book);
        bool highGather = false;
        for (const HuntItem& h : k2.gather.list) highGather = highGather || highItems.count(h.item);
        check(highGather || highItems.empty(), "then the level-" + std::to_string(hi) + " quests' items are gathered (ahead of the level)");
        // a live log: those items drop only from monsters Lv 164..219 -> "no usable source" and the stop; now the weakest
        // of them are hunted
        printf("  the gather: %s (%zu items)\n", k2.gather.current() >= 0 ? k2.gather.source().label.c_str() : "no source",
               k2.gather.list.size());
        check(k2.gather.current() >= 0 && k2.gather.source().zone > 0, "a source for them though every monster that drops them is "
                                                                       "more than 10 levels over 145");
        bool strongNoted = !k2.gather.smart_plan().over.empty();
        check(strongNoted, fmt("the plan knows %zu item(s) come only from stronger monsters", k2.gather.smart_plan().over.size()));
        // a live log: the stall watch's remedy 1 gave the gather's source up and the runner stopped the bot 7 ms later ("no
        // usable source"), before the gather planned again; now the next plan takes over
        {
            if (!s.bot.running) s.bot.start(*s.w, s.now);
            s.run(0, 2);
            const bool before = s.bot.running && s.bot.keyq.gather.source().zone > 0;
            BotCtx ctx{ *s.w, s.now, s.w->self.x, s.w->self.y, false, s.bot, Action{} };
            s.bot.keyq.stalled(ctx, 300000);
            s.bot.keyq.gather.abandon_source(ctx, "no progress there");   // (as the remedy does while it gathers)
            s.run(0, 0.1);                                  // the very next tick (the live stop came 7 ms later)
            const bool nextTick = s.bot.running;
            s.run(0, 3);
            check(nextTick, "the very next tick: still running");
            printf("  after the remedy: %s / %s\n", s.bot.running ? "running" : ("stopped: " + s.bot.stopWhy.substr(0, 80)).c_str(),
                   s.bot.keyq.gather.source().zone ? s.bot.keyq.gather.source().label.c_str() : "no source");
            check(before && s.bot.running && s.bot.keyq.gather.source().zone > 0,
                  "the remedy gives the source up: the bot goes on with the next one (no \"no usable source\" stop)");
        }
        // and with the Haro alone (Lotus Wormhole Lv 219, 74 over): still a source
        ItemHuntAction h;
        h.driven = true;
        h.stopWhenDone = false;
        h.list.push_back(HuntItem{ 10460, 3 });
        h.refresh(*s.w, s.now, false);
        check(h.current() >= 0 && h.source().zone > 0, "Haro alone (Lotus Wormhole Lv 219): " +
              (h.current() >= 0 ? h.source().label : "no source - " + h.why_none(*s.w)));
    }
    printf("\n%d / %d checks passed\n", pass, total);
}

// The other ways to get an item (user: "some items can be bought or exchanged / compounded"): a sticker quest's item a shop
// sells is bought (C2S 0x46 / 0x48 / 0x4F) within the galder, not hunted; one a compounder makes from the bag's
// ingredients is compounded (Compound_Start, C2S 227 type 10, 186, 192).
static void acquire_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const QuestGraph& g = gd.quest_graph();
    const Atlas& at = gd.atlas();
    // ── a shop ──
    const KeyTown* town = nullptr;
    const QuestInfo* quest = nullptr;
    uint32_t item = 0;
    int shopNpc = -1;
    for (const KeyTown& t : g.towns()) {
        for (const StickerQuest& sq : t.quests) {
            const QuestInfo* qi = gd.quest(sq.q.id);
            if (!qi || qi->minLevel > 150) continue;
            for (const QuestNeed& n : qi->src) {
                const std::vector<int>& sh = at.shops_selling(n.item);
                if (sh.empty() || !at.npc(sh.front())->hasPoint || at.shop_price(gd, *at.npc(sh.front()), n.item) <= 0) continue;
                town = &t; quest = qi; item = n.item; shopNpc = sh.front();
                break;
            }
            if (quest) break;
        }
        if (quest) break;
    }
    if (!quest) { printf("no sticker quest asks for an item a shop sells\n"); return; }
    const AtlasNpc* shop = at.npc(shopNpc);
    const int price = at.shop_price(gd, *shop, item);
    printf("\nbuy: %s's quest %d \"%s\" asks for %s, which %s (%s) sells at %d:\n", town->name.c_str(), quest->id, quest->name.c_str(),
           gd.item_name(item).c_str(), shop->name.c_str(), at.map_label(shop->zone).c_str(), price);
    int need = 0;
    for (const QuestNeed& n : quest->src) if (n.item == item) need = n.cnt;
    auto rig_up = [&](StuckRig& s, int64_t money) {
        s.login(shop->zone, shop->x + 40, shop->y, std::max(1, quest->minLevel) + 5);
        s.feed(DIR_S2C, 0x0029, Pk().u32(uint32_t(money)).b);
        s.feed(DIR_S2C, 0x0084, Pk().u32(uint32_t(quest->id)).u16(1).str("q").str("c").str("p").str("x").u16(uint16_t(quest->id)).b);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.keyq.enabled = true;
        s.bot.keyq.sticker = town->sticker;
        s.bot.keyq.picked = { quest->id };
        s.bot.keyq.doKey = false;
    };
    {
        StuckRig s(gd);
        rig_up(s, 2000000);
        bool planned = false;
        for (const QJob& j : s.bot.keyq.make_plan(*s.w, s.bot.book)) planned = planned || (j.shop == shopNpc && j.giveItem == item && j.target >= 1);
        check(planned, fmt("with 2,000,000 galder: a job to buy %s at %s", gd.item_name(item).c_str(), shop->name.c_str()));
        StuckRig p(gd);
        rig_up(p, 0);
        bool planned0 = false;
        for (const QJob& j : p.bot.keyq.make_plan(*p.w, p.bot.book)) planned0 = planned0 || j.shop >= 0;
        check(!planned0, "with no galder: nothing to buy (hunted / other ways instead)");
        StuckRig q(gd);
        rig_up(q, 2000000);
        q.bot.keyq.buyItems = false;
        bool plannedOff = false;
        for (const QJob& j : q.bot.keyq.make_plan(*q.w, q.bot.book)) plannedOff = plannedOff || j.shop >= 0;
        check(!plannedOff, "the switch off: nothing bought");
    }
    {
        StuckRig s(gd);
        rig_up(s, 2000000);
        const uint16_t SHOP = 930;
        s.feed(DIR_S2C, 0x0008, Pk().u16(SHOP).u16(5).u16(shop->species).u8(0).u8(0).u16(1).u32(0).u16(uint16_t(shop->x))
                                   .u16(uint16_t(shop->y)).u16(uint16_t(shop->x)).u16(uint16_t(shop->y)).str("Shop").zeros(12).b);
        FakeNpcServer srv{ gd, s, shop, SHOP };
        s.bot.sync_modes();
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        size_t seen = 0;
        for (int k = 0; k < 900 && s.bot.running && !srv.shopCloses; ++k) {
            s.run(0, 0.1);
            for (; seen < s.sent.size(); ++seen) srv.respond(s.sent[seen]);
        }
        for (int k = 0; k < 10; ++k) { s.run(0, 0.1); for (; seen < s.sent.size(); ++seen) srv.respond(s.sent[seen]); }
        std::string tr;
        for (const std::string& t : srv.trace) tr += (tr.empty() ? "" : ", ") + t;
        printf("  server: %s\n", tr.c_str());
        for (const std::string& l : s.bot.log) if (l.find("bought") != std::string::npos || l.find("buy") != std::string::npos) printf("    log: %s\n", l.c_str());
        bool boughtIt = false;
        for (const std::string& t : srv.trace) boughtIt = boughtIt || (t.find("bought") == 0 && t.find(fmt("x %u", item)) != std::string::npos);
        check(srv.shopOpens >= 1 && srv.buys >= 1 && boughtIt, fmt("C2S 0x46 then 0x48 for %s", gd.item_name(item).c_str()));
        check(srv.boughtCnt >= need && s.w->count_item(item) >= need, fmt("as many as the quest asks (%d): %d in the bag", need, s.w->count_item(item)));
        check(srv.shopCloses >= 1, "C2S 0x4F: the shop closed after it");
        check(s.logged("bought "), "the log says what was bought");
    }
    // ── Snow Hill (a live report: "lots of droppable items shown as to be bought") ──
    for (const KeyTown& t : g.towns()) {
        if (t.name.find("Snow") == std::string::npos) continue;
        StuckRig s(gd);
        int lv = 1;
        for (const StickerQuest& sq : t.quests) if (const QuestInfo* qi = gd.quest(sq.q.id)) lv = std::max(lv, qi->minLevel);
        const AtlasMap* home = nullptr;
        for (const auto& kv : at.maps()) if (kv.second.name.find("Laplanoel") != std::string::npos) home = &kv.second;
        int hx = 1000, hy = 1000;
        if (home) for (const AtlasPoint& p : home->points) if (p.type == 7 || p.type == 2) { hx = p.x; hy = p.y; break; }
        s.login(home ? home->id : 589, hx, hy, lv + 3);
        s.feed(DIR_S2C, 0x0029, Pk().u32(2000000).b);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.keyq.enabled = true;
        s.bot.keyq.sticker = t.sticker;
        for (const StickerQuest& sq : t.quests) s.bot.keyq.picked.insert(sq.q.id);
        s.bot.keyq.doKey = false;
        int buys = 0, buyDrop = 0;
        printf("\nkey quest %s at level %d (%zu sticker quests):\n", t.name.c_str(), lv + 3, t.quests.size());
        for (const QJob& j : s.bot.keyq.make_plan(*s.w, s.bot.book)) {
            if (j.shop < 0) continue;
            ++buys;
            const AtlasNpc* sn = at.npc(j.shop);
            const bool drops = !gd.item_sources().mobs(j.giveItem).empty() || !gd.item_sources().digs(j.giveItem).empty();
            buyDrop += drops;
            if (buys <= 12)
                printf("    buy %-26s x%-3d at %-18s %-40s %5d galder%s\n", gd.item_name(j.giveItem).c_str(), j.target, sn ? sn->name.c_str() : "?",
                       sn ? at.map_label(sn->zone).c_str() : "?", sn ? at.shop_price(gd, *sn, j.giveItem) : -1, drops ? "  (a monster drops it)" : "");
        }
        printf("  %d buy jobs, %d of items a monster drops / a map buries\n", buys, buyDrop);
        check(buyDrop == 0, "Snow Hill: no droppable item planned as \"to buy\" (a shop's OutOfStock rows are what it buys from you)");
    }
    // ── a compound with the bag's ingredients ──
    const QuestInfo* cq = nullptr;
    const KeyTown* ct = nullptr;
    uint32_t citem = 0;
    const QCompound* qc = nullptr;
    for (const KeyTown& t : g.towns()) {
        for (const StickerQuest& sq : t.quests) {
            const QuestInfo* qi = gd.quest(sq.q.id);
            if (!qi || qi->minLevel > 200) continue;
            for (const QuestNeed& n : qi->src)
                for (const QCompound& c : g.compounds(n.item))
                    if (!qc && at.npc(c.place.npc)->hasPoint && !c.place.gates.empty()) { qc = &c; cq = qi; ct = &t; citem = n.item; }
            if (qc) break;
        }
        if (qc) break;
    }
    if (!qc) {                                              // (no sticker quest: any quest of a key town's chain won't do)
        printf("no sticker quest asks for an item a compounder makes\n");
        printf("\n%d / %d checks passed\n", pass, total);
        return;
    }
    const AtlasNpc* cn = at.npc(qc->place.npc);
    const CompoundDef* cd = at.compound(qc->id);
    printf("\ncompound: %s's quest %d \"%s\" asks for %s, which %s (%s) makes (compound %d, %d%%, fee %d):\n", ct->name.c_str(), cq->id,
           cq->name.c_str(), gd.item_name(citem).c_str(), cn->name.c_str(), at.map_label(cn->zone).c_str(), cd->id, cd->probability, cd->fee);
    {
        StuckRig s(gd);
        s.login(cn->zone, cn->x + 40, cn->y, std::max(1, cq->minLevel) + 5);
        s.feed(DIR_S2C, 0x0029, Pk().u32(2000000).b);
        s.feed(DIR_S2C, 0x0084, Pk().u32(uint32_t(cq->id)).u16(1).str("q").str("c").str("p").str("x").u16(uint16_t(cq->id)).b);
        int needC = 0;
        for (const QuestNeed& n : cq->src) if (n.item == citem) needC = n.cnt;
        const int spins = QuestRunner::compound_spins(*cd, needC);
        uint64_t uid = 0x7700;
        for (const auto& rq : cd->req)
            s.feed(DIR_S2C, 0x00A0, Pk().u8(4).u8(1).u32(rq.first).u64(uid++).u16(uint16_t(rq.second * spins + 1)).u8(0).b);
        for (BotAction* a : s.bot.actions()) a->enabled = false;
        s.bot.keyq.enabled = true;
        s.bot.keyq.sticker = ct->sticker;
        s.bot.keyq.picked = { cq->id };
        s.bot.keyq.doKey = false;
        s.bot.keyq.buyItems = false;
        bool planned = false;
        for (const QJob& j : s.bot.keyq.make_plan(*s.w, s.bot.book)) planned = planned || (j.compound == cd->id && j.giveItem == citem);
        check(planned, fmt("its ingredients in the bag: a job to compound %s at %s", gd.item_name(citem).c_str(), cn->name.c_str()));
        const uint16_t NPC = 931;
        s.npc(NPC, cn->species, cn->x, cn->y);
        FakeNpcServer srv{ gd, s, cn, NPC };
        s.bot.sync_modes();
        s.run(0, 3.5);
        s.bot.start(*s.w, s.now);
        size_t seen = 0;
        for (int k = 0; k < 900 && s.bot.running && !srv.cmpCloses; ++k) {
            s.run(0, 0.1);
            for (; seen < s.sent.size(); ++seen) srv.respond(s.sent[seen]);
        }
        for (int k = 0; k < 10; ++k) { s.run(0, 0.1); for (; seen < s.sent.size(); ++seen) srv.respond(s.sent[seen]); }
        std::string tr;
        for (const std::string& t : srv.trace) tr += (tr.empty() ? "" : ", ") + t;
        printf("  server: %s\n", tr.c_str());
        for (const std::string& l : s.bot.log) if (l.find("compound") != std::string::npos) printf("    log: %s\n", l.c_str());
        check(srv.cmpOpens >= 1, "Compound_Start (func 20) through the compounder's dialog");
        check(srv.cmpRolls >= 1 && srv.cmpId == cd->id && srv.cmpGiven.size() == cd->req.size(),
              fmt("C2S 227: compound %d x%d with its %zu ingredients (type 10: exactly them)", srv.cmpId, srv.cmpSpins, cd->req.size()));
        check(srv.cmpMakes >= 1 && s.w->count_item(citem) >= needC, fmt("C2S 186: %d %s in the bag (%d asked)", s.w->count_item(citem),
                                                                        gd.item_name(citem).c_str(), needC));
        check(srv.cmpCloses >= 1, "C2S 192: the compounder closed after it");
    }
    printf("\n%d / %d checks passed\n", pass, total);
}

// A run: Episode 5 chapter 1 with the quests before Rosaline's Memory done and a Memory Kit 2 in the bag — the Story
// trades it at JOKER's exchange shop for the Approved Memory Kit 2, then uses that to get into Rosaline's Memory.
static void story_run_test(const GameData& gd, int& pass, int& total)
{
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    const Atlas& at = gd.atlas();
    const QuestGraph& g = gd.quest_graph();
    int ch = -1;
    for (size_t i = 0; i < g.chapters().size(); ++i)
        if (g.chapters()[i].episode == 5 && g.chapters()[i].chapter == 1) ch = int(i);
    const std::vector<QExchange>& ex = g.exchanges(64111);
    if (ch < 0 || ex.empty()) { printf("no Episode 5 chapter 1 / Approved Memory Kit 2 exchange in the data\n"); return; }
    const AtlasNpc* joker = at.npc(ex.front().place.npc);
    printf("\na run: Episode 5 chapter 1, the Memory Kit 2 in the bag, %s's exchange shop in %s:\n", joker->name.c_str(),
           at.map_label(joker->zone).c_str());
    StuckRig s(gd);
    const uint16_t NPC = 920;
    int sx = joker->x + 40, sy = joker->y;
    if (const MapGrid* mg = gd.map(joker->zone)) if (mg->blocked(sx, sy)) { sx = joker->x; sy = joker->y + 40; }
    s.login(joker->zone, sx, sy, std::max(40, g.chapters()[size_t(ch)].minLevel));   // (the chapter's level)
    s.npc(NPC, joker->species, joker->x, joker->y);
    s.feed(DIR_S2C, 0x00E7, Pk().u16(1).u8(1).u32(64093).u64(0x601).u16(1).b);
    FakeNpcServer srv{ gd, s, joker, NPC };
    for (BotAction* a : s.bot.actions()) a->enabled = false;
    // everything before the chapter's Rosaline's Memory quests done; those two known not done
    for (size_t i = 0; i < size_t(ch); ++i)
        for (int q : g.chapters()[i].quests) s.bot.book.h[QKey{ false, q, true }.v()] = QuestHist{ 1, 1 };
    for (int q : g.chapters()[size_t(ch)].quests) {
        const QKey k{ false, q, true };
        const QPlace* p = g.places(k) && !g.places(k)->accepts.empty() ? &g.places(k)->accepts.front() : nullptr;
        const bool memory = p && at.npc(p->npc)->zone == 506;
        s.bot.book.h[k.v()] = memory ? QuestHist{ 0, 0 } : QuestHist{ 1, 1 };
        srv.hist[k.v()] = memory ? 0 : 1;
    }
    s.bot.story.enabled = true;
    s.bot.story.toChapter = ch;
    s.bot.sync_modes();
    s.run(0, 3.5);
    s.bot.start(*s.w, s.now);
    std::string plan;
    for (const QJob& j : s.bot.story.make_plan(*s.w, s.bot.book))
        plan += j.exchange ? " exchange:" + gd.item_name(j.giveItem) : j.giveItem ? " give:" + gd.item_name(j.giveItem) : fmt(" %c%d", j.q.tag(), j.q.id);
    printf("  plan:%s\n", plan.c_str());
    size_t seen = 0;
    for (int k = 0; k < 600 && s.bot.running && !srv.uses; ++k) {
        s.run(0, 0.1);
        for (; seen < s.sent.size(); ++seen) srv.respond(s.sent[seen]);
    }
    std::string tr;
    for (const std::string& t : srv.trace) tr += (tr.empty() ? "" : ", ") + t;
    printf("  server: %s\n", tr.c_str());
    for (const std::string& l : s.bot.log)
        if (l.find("story") != std::string::npos || l.find("route") != std::string::npos || l.find("using") != std::string::npos)
            printf("    log: %s\n", l.c_str());
    check(srv.exchanges == 1, "C2S 254: the Memory Kit 2 given at the exchange shop");
    check(srv.exCloses >= 1, "C2S 257: the exchange window closed after it");
    check(s.w->count_item(64111) >= 1 || srv.uses, fmt("the Approved Memory Kit 2 came (%d)", s.w->count_item(64111)));
    const InvItem* kit = nullptr;
    for (const InvItem& it : s.w->inventory) if (it.itemId == 64111) kit = &it;
    check(srv.uses == 1 && kit && srv.usedUid == kit->uid, "then C2S 0x57 with it: the port to Rosaline's Memory");
    check(!srv.dropped, "the server refused nothing it would close the session for");
}

static void story_selftest(const Config& cfg)
{
    GameData gd;
    std::string err;
    gd.load(cfg.gameDbDir, err);
    int pass = 0, total = 0;
    story_graph_dump(gd, pass, total);
    story_entry_checks(gd, pass, total);
    story_run_test(gd, pass, total);
    // a live report: "the story quests are linear and the bot does not follow that". (1) Check_Job: Clever Owl's two
    // scenario quests are two classes' variants (msg 0 "Check_Job,21,11"); the bot kept asking for the other class's one.
    // (2) the plan of a chapter starts at its first quest, not at its last (one walk per quest, backwards).
    auto check = [&](bool ok, const std::string& what) {
        ++total;
        pass += ok;
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    };
    {
        const QuestGraph& g = gd.quest_graph();
        const QKey a{ false, 1123, true }, b{ false, 1124, true };     // The Owl from that Time / The Owl's Worry
        const QuestPlaces* pa = g.places(a);
        const AtlasNpc* owl = pa && !pa->accepts.empty() ? gd.atlas().npc(pa->accepts.front().npc) : nullptr;
        if (owl) {
            int inJob = 0, outJob = 0;                              // a job Check_Job answers 1 for, one it answers 2 for
            for (int j = 1; j <= 33; ++j) {
                const bool has = j >= 17 ? ((owl->jobFlag2 >> (j - 17)) & 1) : ((owl->jobFlag1 >> (j - 1)) & 1);
                if (has && !inJob) inJob = j;
                if (!has && !outJob) outJob = j;
            }
            printf("\nCheck_Job at %s: JobTypeFlag1 0x%X, JobTypeFlag2 0x%X (job %d -> 1, job %d -> 2)\n", owl->name.c_str(), owl->jobFlag1,
                   owl->jobFlag2, inJob, outJob);
            for (int pick = 0; pick < 2; ++pick) {
                const int job = pick == 0 ? inJob : outJob;
                if (!job) continue;
                StuckRig s(gd);
                s.login(owl->zone, owl->x + 40, owl->y, 60);
                s.feed(DIR_S2C, 0x023A, Pk().u16(StuckRig::ME).u16(1).u8(uint8_t(job)).u8(0).u8(1).u8(0).b);
                const QuestRunner::View v(s.bot.book, *s.w);
                std::vector<QJob> ja, jb;
                g.expand(a, 1, v, ja);
                g.expand(b, 1, v, jb);
                const bool aNo = !ja.empty() && ja.back().notForUs, bNo = !jb.empty() && jb.back().notForUs;
                check(s.w->self.job == job && aNo != bNo, fmt("job %d: \"%s\" %s, \"%s\" %s", job, g.quest_name(a).c_str(),
                                                             aNo ? "not for us" : "for us", g.quest_name(b).c_str(), bNo ? "not for us" : "for us"));
            }
        }
    }
    {
        const std::vector<QuestGraph::Chapter>& chs = gd.quest_graph().chapters();
        int ci = -1;                                                // a chapter of several quests
        for (size_t i = 0; i < chs.size() && ci < 0; ++i) if (chs[i].quests.size() >= 4 && chs[i].first != chs[i].last) ci = int(i);
        if (ci >= 0) {
            const QuestGraph::Chapter& ch = chs[size_t(ci)];
            StuckRig s(gd);
            s.login(1, 1100, 1300, std::max(1, ch.minLevel) + 5);
            s.bot.story.fromChapter = ci;
            s.bot.story.toChapter = ci;
            const std::vector<QJob> plan = s.bot.story.make_plan(*s.w, s.bot.book);
            int scen = 0;
            bool first = false;
            for (const QJob& j : plan) { scen += j.q.scen; first = first || (j.q.scen && j.q.id == ch.first); }
            printf("\nEp %d-%d \"%s\": %zu quests in the chapter, the plan (nothing known) has %d scenario quests%s\n", ch.episode, ch.chapter,
                   ch.name.c_str(), ch.quests.size(), scen, first ? ", its first one among them" : "");
            check(first && scen >= int(ch.quests.size()) / 2, "nothing known of the chapter: the plan starts at its first quest (linear)");
            // a chapter before the start one counts as done
            if (ci > 0) {
                bool earlier = false;
                for (const QJob& j : plan) earlier = earlier || (j.q.scen && gd.quest_graph().chapter_of(j.q.id) >= 0 &&
                                                                 gd.quest_graph().chapter_of(j.q.id) < ci);
                check(!earlier, "the chapters before the start one count as done (none of their quests in the plan)");
            }
        }
    }
    {                                                               // the weight: a batch of runs at a time
        StuckRig s(gd);
        s.login(1, 1100, 1300, 100);
        const QuestInfo* heavy = nullptr;
        for (const QuestInfo& q : gd.quests()) {
            int per = 0;
            for (const QuestNeed& n : q.src) if (const ItemInfo* ii = gd.item(n.item)) per += std::max(1, ii->weight) * std::max(1, n.cnt);
            if (q.maxCount >= 20 && per >= 40) { heavy = &q; break; }
        }
        const int maxWt = s.w->stat_base(ST_MAXWT);
        if (heavy) {
            const int b = QuestRunner::batch_runs(*s.w, *heavy, 25);
            printf("\nweight: \"%s\" x25 runs, max weight %d -> a batch of %d run(s)\n", heavy->name.c_str(), maxWt, b);
            check(b >= 1 && b < 25, "a quest of heavy items: its items gathered a batch at a time, not 25 runs at once");
        }
    }
    printf("\n%d / %d checks passed\n", pass, total);
}

int main(int argc, char** argv)
{
    GameData::keep_libconfig(true);                     // (the tests load the data many times: conf\paula_libconfig.xml once)
    // watch [account] [seconds]: launch the client via the launcher, capture, send NOTHING, dump the model.
    const bool watchMode = argc > 1 && strcmp(argv[1], "watch") == 0;
    const bool worldOnly = argc > 1 && strcmp(argv[1], "world") == 0;
    // attach <pid> [seconds]: link to a running client, print what happens, send NOTHING, leave it running
    const bool attachMode = argc > 2 && strcmp(argv[1], "attach") == 0;
    const uint32_t attachPid = attachMode ? (uint32_t)atoi(argv[2]) : 0;
    const int seconds = worldOnly ? 0 : attachMode ? (argc > 3 ? atoi(argv[3]) : 20) : argc > 1 ? atoi(argv[1]) : 40;
    PacketDb db;
    std::string err;
    for (const char* f : { "packets_db.yml", "packets_neo.yml" })
        if (!db.load(exe_dir() + "data\\" + f, err)) printf("db: %s\n", err.c_str());
    printf("packet db: %zu definitions\n", db.size());
    decode_sample(db, DIR_C2S, 0x18, { 0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x01 });
    decode_sample(db, DIR_S2C, 271, { 0x02, 0x00, 0xE9, 0x03, 0x05, 0x00, 0x00, 0xEA, 0x03, 0x01, 0x07, 0x00 });
    decode_sample(db, DIR_S2C, 184, { 0x34, 0x12, 0xBE, 0xC8, 0xB3, 0xE7, 0x00 });   // CP949 "안녕"
    decode_sample(db, DIR_S2C, 15, { 0x34, 0x12 });                                  // short -> error row
    decode_sample(db, DIR_S2C, 7002, { 0x01, 0x00, 'L', 'e', 'a', 'o', 0x00, 0x94, 0x00, 0xFF });
    // field encoder: build C2S 0x18 MOVE from values and check the bytes
    if (const PacketDef* mv = db.find(DIR_C2S, 0x18)) {
        std::vector<uint8_t> body;
        std::string eerr;
        int bad = -1;
        bool ok = PacketDb::encode(*mv, { "16", "32", "0x30", "64", "1" }, body, eerr, bad);
        const std::vector<uint8_t> want = { 0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x01 };
        printf("encode C2S 0x18 MOVE: %s -> %s (%s)\n", ok ? "ok" : eerr.c_str(),
               hex_preview(body.data(), body.size(), 32).c_str(), ok && body == want ? "MATCH" : "MISMATCH");
    }
    if (worldOnly) {
        Config wc;
        load_config(wc, err);
        world_selftest(wc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "town") == 0) {
        Config tc;
        load_config(tc, err);
        town_selftest(tc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "layouts") == 0) {
        Config lc;
        load_config(lc, err);
        layouts_selftest(lc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "drill") == 0) {
        Config dc;
        load_config(dc, err);
        drill_selftest(dc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "stuck") == 0) {
        Config sc;
        load_config(sc, err);
        stuck_selftest(sc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "quest") == 0) {
        Config qc;
        load_config(qc, err);
        quest_selftest(qc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "roam") == 0) {
        Config rc;
        load_config(rc, err);
        roam_selftest(rc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "equip") == 0) {
        Config ec;
        load_config(ec, err);
        equip_selftest(ec);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "misc") == 0) {
        Config mc;
        load_config(mc, err);
        misc_selftest(mc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "stall") == 0) {
        Config st;
        load_config(st, err);
        stall_selftest(st);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "story") == 0) {
        Config sc2;
        load_config(sc2, err);
        story_selftest(sc2);
        return 0;
    }
    // route <from> <to> [level] [item ...]: the atlas route (and why none), what opens the map, its NPCs
    if (argc > 3 && strcmp(argv[1], "route") == 0) {
        Config rc;
        load_config(rc, err);
        GameData gd;
        gd.load(rc.gameDbDir, err);
        const Atlas& at = gd.atlas();
        const int from = atoi(argv[2]), to = atoi(argv[3]);
        RouteOptions o;
        o.level = argc > 4 ? atoi(argv[4]) : 100;
        o.money = 1000000;
        o.bagKnown = true;
        const AtlasMap* m = at.map(from);
        int x = 500, y = 400;
        if (m) for (const AtlasPoint& p : m->points) if (p.type == 7 || p.type == 2) { x = p.x; y = p.y; break; }
        for (int i = 5; i < argc; ++i) {
            if (argv[i][0] == '@') sscanf(argv[i] + 1, "%d,%d", &x, &y);      // @x,y: the start point
            else if (argv[i][0] == '#') sscanf(argv[i] + 1, "%d,%d", &o.goalX, &o.goalY);   // #x,y: a goal point
            else o.bag[uint32_t(atoi(argv[i]))] = 1;
        }
        printf("start (%d,%d)\n", x, y);
        const Route r = at.route(gd, from, x, y, to, o);
        printf("%s -> %s (level %d, %zu item(s) in the bag): %s\n", at.map_label(from).c_str(), at.map_label(to).c_str(), o.level,
               o.bag.size(), r.ok ? fmt("%zu hops, ~%.0f s", r.hops.size(), r.secs).c_str() : r.why.c_str());
        for (const RouteHop& h : r.hops) {
            const AtlasNpc* n = at.npc(h.npc);
            printf("  > %-45s %s\n", at.map_label(h.toZone).c_str(),
                   h.kind == RouteHop::Item ? ("port item " + gd.item_name(h.item)).c_str()
                   : h.kind == RouteHop::Teleport ? fmt("%s %s%s", h.move ? "NPC move" : "teleport", n ? n->name.c_str() : "?",
                                                         h.cost ? fmt(" %dg", h.cost).c_str() : "").c_str()
                   : "portal");
        }
        int root = 0;
        const uint32_t it = gd.quest_graph().entry_item(to, &root);
        if (it) printf("  %s is opened only by %s (%u) first in %s\n", at.map_label(to).c_str(), gd.item_name(it).c_str(), it, at.map_label(root).c_str());
        for (int ni : at.map(to) ? at.map(to)->npcs : std::vector<int>{}) {
            const AtlasNpc* n = at.npc(ni);
            printf("  NPC %-28s species %5u at (%d,%d)%s\n", n->name.c_str(), n->species, n->x, n->y, n->hasPoint ? "" : " (no point: the server's 600,600)");
        }
        return 0;
    }
    // qinfo <name part>: the scenario / normal quests of that name, their takes' NPCs, gates (conditions) and messages
    if (argc > 2 && strcmp(argv[1], "qinfo") == 0) {
        Config qc;
        load_config(qc, err);
        GameData gd;
        gd.load(qc.gameDbDir, err);
        const QuestGraph& g = gd.quest_graph();
        auto lower_s = [](std::string t) { for (char& ch : t) ch = char(std::tolower((unsigned char)ch)); return t; };
        const std::string needle = lower_s(argv[2]);
        for (int scen = 1; scen >= 0; --scen)
            for (const QuestInfo& q : scen ? gd.scenario_quests() : gd.quests()) {
                if (lower_s(q.name).find(needle) == std::string::npos) continue;
                const QKey k{ false, q.id, scen == 1 };
                printf("%c%d \"%s\" Lv %d-%d, max %d, chapter %d\n", k.tag(), q.id, q.name.c_str(), q.minLevel, q.maxLevel, q.maxCount,
                       scen ? g.chapter_of(q.id) : -1);
                const QuestPlaces* qp = g.places(k);
                if (!qp) { printf("  no places\n"); continue; }
                for (const QPlace& p : qp->accepts) {
                    const AtlasNpc* n = gd.atlas().npc(p.npc);
                    printf("  take at %s (func %u, slot %d), %zu gate(s)\n", n ? n->name.c_str() : "?", p.func, p.slot, p.gates.size());
                    for (const QGate& gt : p.gates) {
                        std::string cs, path;
                        for (const QCond& c : gt.conds) cs += (cs.empty() ? "" : "; ") + g.cond_text(c);
                        for (int m : gt.path) path += fmt("%d ", m);
                        printf("    path %s: %s\n", path.c_str(), cs.empty() ? "(nothing)" : cs.c_str());
                    }
                    if (n && n->dialog)
                        for (const auto& dm : n->dialog->msgs)
                            printf("    msg %d: %.150s\n", dm.first, dm.second.text.c_str());
                    break;
                }
            }
        return 0;
    }
    // datacheck: the game data from conf\paula_libconfig.xml against the ProjectPAULA yml files (counts + samples)
    if (argc > 1 && strcmp(argv[1], "datacheck") == 0) {
        Config dc;
        load_config(dc, err);
        GameData x, y;
        std::string ex, ey;
        x.load(dc.gameDbDir, ex);
        y.set_libconfig(exe_dir() + "conf\\no_such_libconfig.xml");
        y.load(dc.gameDbDir, ey);
        printf("xml: %s%s\nyml: %s%s\n", x.source().c_str(), ex.empty() ? "" : ("  errors: " + ex).c_str(), y.source().c_str(),
               ey.empty() ? "" : ("  errors: " + ey).c_str());
        auto row = [](const char* what, size_t a, size_t b) { printf("  %-28s xml %7zu   yml %7zu%s\n", what, a, b, a == b ? "" : "   (differs)"); };
        row("items", x.items(), y.items());
        row("monsters", x.mobs(), y.mobs());
        row("skills", x.skills(), y.skills());
        row("maps", x.maps(), y.maps());
        row("quests", x.quests().size(), y.quests().size());
        row("scenario quests", x.scenario_quests().size(), y.scenario_quests().size());
        row("monster quests", x.monster_quests().size(), y.monster_quests().size());
        row("drop lists", x.item_sources().lists(), y.item_sources().lists());
        row("drop sub-tables", x.item_sources().tables(), y.item_sources().tables());
        row("dig maps", x.item_sources().dig_maps(), y.item_sources().dig_maps());
        row("atlas NPCs", x.atlas().npcs().size(), y.atlas().npcs().size());
        row("atlas shops", x.atlas().shops().size(), y.atlas().shops().size());
        row("atlas warehouses", x.atlas().warehouses().size(), y.atlas().warehouses().size());
        row("key towns", x.quest_graph().towns().size(), y.quest_graph().towns().size());
        row("story chapters", x.quest_graph().chapters().size(), y.quest_graph().chapters().size());
        printf("  xml atlas: %s\n  yml atlas: %s\n", x.atlas().load_report().c_str(), y.atlas().load_report().c_str());
        // samples: the same id in both
        int same = 0, diff = 0;
        for (const auto& kv : y.mob_table()) {
            const MobInfo* a = x.mob(kv.first);
            if (!a) continue;
            (a->level == kv.second.level && a->maxHp == kv.second.maxHp && a->itemDropIdx == kv.second.itemDropIdx) ? ++same : ++diff;
        }
        printf("  monsters in both: %d the same level / HP / drop list, %d not\n", same, diff);
        same = diff = 0;
        for (uint32_t id = 1; id < 1000000 && same + diff < 30000; ++id) {
            const ItemInfo* a = x.item(id);
            const ItemInfo* b = y.item(id);
            if (!a || !b) continue;
            (a->type == b->type && a->effect == b->effect && a->options == b->options && a->weight == b->weight) ? ++same : ++diff;
        }
        printf("  items in both: %d the same type / effect / options / weight, %d not\n", same, diff);
        // the quests' items a shop sells / a compounder makes (the other ways to get them)
        {
            const Atlas& at = x.atlas();
            const QuestGraph& g = x.quest_graph();
            std::set<uint32_t> items, sold, made, onlyMade;
            for (const QuestInfo& q : x.quests()) for (const QuestNeed& n : q.src) items.insert(n.item);
            for (const QuestInfo& q : x.scenario_quests()) for (const QuestNeed& n : q.src) items.insert(n.item);
            std::string soldEx, madeEx;
            for (uint32_t it : items) {
                const bool s = !at.shops_selling(it).empty(), m = !g.compounds(it).empty();
                const bool gat = !x.item_sources().mobs(it).empty() || !x.item_sources().digs(it).empty();
                if (s) { sold.insert(it); if (soldEx.size() < 160) soldEx += x.item_name(it) + fmt(" (%u), ", it); }
                if (m) { made.insert(it); if (madeEx.size() < 200) madeEx += x.item_name(it) + fmt(" (%u%s), ", it, gat ? "" : " only"); }
                if (m && !gat && !s) onlyMade.insert(it);
            }
            printf("  quest items: %zu, sold by a shop %zu, made by a compounder %zu (%zu only that way)\n    sold: %s\n    made: %s\n",
                   items.size(), sold.size(), made.size(), onlyMade.size(), soldEx.c_str(), madeEx.c_str());
        }
        for (uint32_t id : { 2006u, 2129u, 1001u }) {
            const SkillInfo* a = x.skill(id);
            const SkillInfo* b = y.skill(id);
            printf("  skill %u: xml %s type %d esaui %d rel %d levels %zu radius %.0f | yml type %d esaui %d rel %d levels %zu radius %.0f\n", id,
                   a ? a->name.c_str() : "-", a ? a->type : -1, a ? a->esaui : -1, a ? a->affectionRel : -1, a ? a->levels.size() : 0,
                   a && !a->levels.empty() ? a->levels.back().radius : 0.0f, b ? b->type : -1, b ? b->esaui : -1, b ? b->affectionRel : -1,
                   b ? b->levels.size() : 0, b && !b->levels.empty() ? b->levels.back().radius : 0.0f);
        }
        return 0;
    }
    // sources <level> <zone> <item id or name>...: every monster / dig source of each item as the Item Hunt sees it
    if (argc > 4 && strcmp(argv[1], "sources") == 0) {
        Config sc;
        load_config(sc, err);
        GameData gd;
        gd.load(sc.gameDbDir, err);
        const int level = atoi(argv[2]), zone = atoi(argv[3]);
        const ItemSources& is = gd.item_sources();
        RouteOptions o;
        o.level = level;
        o.money = 1000000;
        o.bagKnown = true;
        for (int i = 4; i < argc; ++i) {
            uint32_t item = uint32_t(atoi(argv[i]));
            if (!item) for (uint32_t id : gd.search_items(argv[i], 20)) if (gd.item_name(id) == argv[i]) { item = id; break; }
            if (!item) { const auto f = gd.search_items(argv[i], 1); if (!f.empty()) item = f.front(); }
            printf("%s (%u):\n", gd.item_name(item).c_str(), item);
            for (const ItemMobSource& m : is.mobs(item)) {
                const MobInfo* mi = gd.mob(m.mob);
                for (const MobSpawn& sp : gd.spawns_of(m.mob))
                    printf("  kill %-24s Lv %3d (%+d)  %.4f/kill  x%-3d in %-45s %s\n", gd.mob_name(uint32_t(m.mob)).c_str(), mi ? mi->level : 0,
                           (mi ? mi->level : 0) - level, m.perKill, sp.count, gd.atlas().map_label(sp.zone).c_str(),
                           gd.atlas().zone_reachable(zone, sp.zone, o) ? "" : "(no way)");
                if (gd.spawns_of(m.mob).empty())
                    printf("  kill %-24s Lv %3d  %.4f/kill  (spawns nowhere)\n", gd.mob_name(uint32_t(m.mob)).c_str(), mi ? mi->level : 0, m.perKill);
            }
            for (const ItemDigSource& d : is.digs(item))
                printf("  dig in %-45s %.4f/find\n", gd.atlas().map_label(d.zone).c_str(), d.perFind);
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "keyquest") == 0) {
        Config kc;
        load_config(kc, err);
        keyquest_selftest(kc);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "acquire") == 0) {
        Config ac;
        load_config(ac, err);
        acquire_selftest(ac);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "hunt") == 0) {
        Config hc;
        load_config(hc, err);
        hunt_selftest(hc);
        return 0;
    }

    // ── watch: launch via the launcher, capture only, dump the model + opcode report ──
    if (watchMode) {
        Config wcfg;
        if (!load_config(wcfg, err)) { printf("config: %s\n", err.c_str()); return 1; }
        GameData gd;
        std::string gerr;
        gd.load(wcfg.gameDbDir, gerr);
        printf("game data: %zu items, %zu mobs, %zu skills\n", gd.items(), gd.mobs(), gd.skills());
        const std::string acc = argc > 2 ? argv[2] : "1";
        const int secs = argc > 3 ? atoi(argv[3]) : 90;
        printf("launching account %s -> %s:%d. LOG IN and ENTER THE MAP. Watching %d s (sending nothing).\n",
               acc.c_str(), wcfg.loginIp.c_str(), wcfg.loginPort, secs);
        Session ws(1, &gd);
        ws.launch(wcfg, Account{ acc, acc });
        std::string ls;
        size_t lp = 0;
        uint64_t lv = 0;
        for (int t = 0; t < secs * 10; ++t) {
            Sleep(100);
            ws.poll();
            if (ws.status != ls) { printf("[state] %s (packets %llu)\n", ws.status.c_str(),
                                          (unsigned long long)ws.total_packets()); ls = ws.status; }
            if (app_log_version() != lv) {
                lv = app_log_version();
                app_log_read([&](const std::deque<std::string>& l) { for (; lp < l.size(); ++lp) printf("[log] %s\n", l[lp].c_str()); });
            }
        }
        print_world(ws);
        // dump every captured packet (raw hex) so the S2C opcodes/bytes can be inspected
        const std::string capDir = exe_dir() + "captures\\";
        CreateDirectoryA(capDir.c_str(), nullptr);
        std::ofstream out(capDir + "watch_capture.txt", std::ios::binary);
        out << "# seq\tdir\topcode\tparam\tsize\thex\n";
        for (uint64_t i = 0; i < ws.total_packets(); ++i) {
            const CapPacket* p = ws.packet_at(i);
            if (!p) continue;
            out << fmt("%llu\t%s\t0x%04X\t0x%04X\t%u\t", (unsigned long long)p->hdr.seq,
                       p->hdr.dir == DIR_C2S ? "C>S" : "S>C", p->hdr.opcode, p->hdr.param, p->hdr.size)
                << hex_preview(p->data.data(), p->data.size(), p->data.size()) << "\n";
        }
        printf("wrote %llu packets to captures\\watch_capture.txt\n", (unsigned long long)ws.total_packets());
        ws.close();
        return 0;
    }

    if (seconds <= 0) return 0;   // db check only, no client

    Config cfg;
    if (!load_config(cfg, err)) { printf("config: %s\n", err.c_str()); return 1; }
    cfg.preLaunch.clear();
    printf("client %s -> %s:%d\n", cfg.tricksterPath.c_str(), cfg.loginIp.c_str(), cfg.loginPort);

    Session s(1, nullptr);
    if (attachMode) s.attach(cfg, attachPid);
    else s.launch(cfg, Account{ "1", "selftest" });

    uint64_t printed = 0, logSeen = 0;
    size_t logPrinted = 0;
    std::string lastStatus;
    bool sent = false;
    for (int tick = 0; tick < seconds * 10; ++tick) {
        Sleep(100);
        s.poll();
        if (s.status != lastStatus) { printf("[state] %s\n", s.status.c_str()); lastStatus = s.status; }
        if (app_log_version() != logSeen) {
            logSeen = app_log_version();
            app_log_read([&](const std::deque<std::string>& l) {
                for (; logPrinted < l.size(); ++logPrinted) printf("[log] %s\n", l[logPrinted].c_str());
            });
        }
        for (; printed < s.total_packets(); ++printed) {
            const CapPacket* p = s.packet_at(printed);
            if (attachMode && printed >= 40) continue;   // a live client: just count
            const PacketDef* d = db.find(p->hdr.dir, p->hdr.opcode);
            printf("[pkt] #%llu %s%s 0x%04X %-24s ctx 0x%08X len %u | %s\n", (unsigned long long)p->hdr.seq,
                   p->hdr.dir == DIR_C2S ? "C>S" : "S>C", p->hdr.flags & PF_INJECTED ? "*" : " ", p->hdr.opcode,
                   d ? d->name.c_str() : "", p->hdr.ctx, p->hdr.size,
                   hex_preview(p->data.data(), p->data.size(), 24).c_str());
            if (!attachMode && !sent && p->hdr.dir == DIR_C2S && !(p->hdr.flags & PF_INJECTED)) {
                if (s.send(p->hdr.ctx, 0x1234, 0, { 0xDE, 0xAD, 0xBE, 0xEF }, err)) printf("[send] queued 0x1234\n");
                else printf("[send] %s\n", err.c_str());
                sent = true;
            }
        }
        for (const SendRecord& r : s.sends)
            if (r.done) { printf("[send] #%u -> %s %s\n", r.reqId, r.ok ? "SENT" : "FAILED", r.error.c_str()); }
        s.sends.erase(std::remove_if(s.sends.begin(), s.sends.end(), [](const SendRecord& r) { return r.done; }),
                      s.sends.end());
    }
    if (attachMode) {
        // Attach is read-only: never terminate a client we did not launch ourselves.
        printf("hook state %u, packets %llu, model applied %llu - unlinking (the client keeps running)\n",
               s.hookState, (unsigned long long)s.total_packets(), (unsigned long long)s.world->applied);
        s.close();
        return 0;
    }
    printf("hook state %u, packets %llu - killing the test client we launched\n", s.hookState,
           (unsigned long long)s.total_packets());
    s.kill_client();
    s.close();
    return 0;
}
