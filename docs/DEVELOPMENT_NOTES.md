# Trickster Automation Studio - development notes

> Development notes - the detailed, historical record of how each part was built and verified (written while the tool was developed against the ProjectPAULA emulator and the KR 2014 client). Names of files, settings and builds may be older than the code; the README is the reference for setting the tool up. The icon builder (tools/build_icons.py) mentioned below is not distributed.


Packet-based rewrite of janus-master: an ImGui panel driving the Trickster client
(`Client_live\Trickster.bin`, KTO 2014) through its own network layer. It reads no
memory offsets: state comes from the packets, actions go out as C2S packets.

## Build (32-bit only)

```
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release
```

Output in `bin\`: `TricksterAutomationStudio.exe`, `tas_hook.dll`, `data\`, `tas.ini.default`.
Copy `tas.ini.default` to `tas.ini` to change local settings.

## Run

- `TricksterAutomationStudio.exe` opens on the Welcome page (VS Code's home): open a registered account, attach to
  a running client (listed like recent projects) or replay a capture. "+" in the activity bar
  brings it back for another session.
- `TricksterAutomationStudio.exe --launch 1 --launch 2` launches accounts 1 and 2 directly, like
  ABRIR_CLIENT.bat. `--attach <pid>`, `--open <capture.txt>` and `--tab <page>` also work
  (`home|settings|overview|heal|rest|buffs|loot|attack|drill|quest|hunt|roam|town|supply|travel` or a Debug page:
  `session|world|character|packets|opcodes|send|log`). `--shot <file.png>` renders a few frames, saves the window's
  back buffer and quits (UI checks without a screen grab); `--size <w>x<h>` sets the window's size (a long page in
  one picture, capped by the screen); `--scroll <px>` scrolls the page that far (the bottom of a long page);
  `--toast` puts two sample notices in the window (no Windows notification).
- **Open capture** (Clients tab, or `--open`) replays a file saved with Packets > Export
  into an offline session. The same world model rebuilds from it, so a capture is enough to
  debug the model without the client.
- Launching does the janus_launcher steps: the PreLaunch command, the splash-bypass command
  line, the IP redirect (`OriginalIP` → `LoginIP`) and the hook DLL. The DLL hooks before
  the login connection, so the whole session is captured.

## How it works

```
Trickster.bin ─┬─ c2s_frame_packet 0x811FF0 ─┐  hooks (all C2S / all S2C, plaintext)
               └─ post_game_event  0x82A820 ─┤
                                             ▼
 tas_hook.dll   PacketStore (32 MB history) ── \\.\pipe\neojanus-<pid>-evt ──► TricksterAutomationStudio.exe
                     sender (game window thread)  ◄─ \\.\pipe\neojanus-<pid>-cmd ──  (Session, UI)
                       └─ Net_SendPacket_Raw 0x82A210 (the client frames + encrypts)
```

- **Client builds** (`src/hook/client_addr.h`): the hook knows two, and picks the one whose two
  capture sites both hold their expected prologues (`tas_selftest layouts` checks them against
  the files on disk):

  | | Paula `Trickster.bin` | Season 2 `TricksterS2.exe` |
  |---|---|---|
  | C2S framer `__stdcall(op, param, src, size, buf, cap, flags)` | `0x811FF0` | `0x6251F0` |
  | S2C site | `post_game_event 0x82A820` `(this, category, op, src, size)` | `NetworkManager::OnPacket 0x58AC50` `(this, category, param, op, cursor{src, size})` |
  | `Net_SendPacket_Raw(conn, op, param, src, size, buf, 0x8000, 0x0F)` | `0x82A210` | `0x58AF80` |
  | ecx at the framer (the packet's ctx) | connection + 8 | connection + 0x28 |
  | connection vtable / SOCKET / state | `0x9EE0AC` / +4 / +0x8130 | `0xAB6140` / +0x24 / +0x10040 |
  | local avatar: `[world]` + 0x568 -> +0x5C flags (& 7 moving), +0x6C x / y drawn, +0x70 move end | `[0x10F2C60]` | `[0xCE1D00]` |

  The Season 2 deframer (`0x58B270`) queues each decrypted packet; the game thread's pump (`0x589A30`)
  hands them to `OnPacket`, so its S2C are captured when the game reads them. The S2C `param` field
  holds the connection's category (0 / 1 / 2) for both builds.
- **Drawn avatar** (`src/hook/avatar.cpp`, `EV_AVATAR`): every 100 ms the events thread reads where the
  client draws our own character (the table's last row, every read SEH-guarded) and sends it when it
  changed, else once a second. The world model keeps it apart from the server's position
  (`self.drawn`); the bot's stuck guard compares the two. An older DLL simply sends none.
- **Hooks** (`src/hook/hooks.cpp`): a 5-byte jmp at each choke point to a naked detour.
  The jmp is written only once the client code matches the expected prologue.
- **History** (`packet_store.cpp`): every packet is kept (bounded) and replayed when the
  app connects or reconnects, so a late attach still gets the login burst.
- **Sending** (`sender.cpp`): the game window is subclassed. Sends run on its thread
  between the client's frames. Injected packets show as `C>S*`.
- **Packet db** (`src/app/packet_db.cpp`): `data/packets_db.yml` (511 RE definitions)
  plus the `data/packets_neo.yml` overlay of verified layouts. Field syntax is in the
  overlay header.
- **World model** (`src/app/world.cpp`): every packet of a session, in capture order (the
  history replay included), applied to the game state. It tracks the character (stats,
  HP/MP/EXP/TM, money, zone, position), the entities around it, the ground items, the
  inventory with equipment, and the skills. The header of `world.cpp` lists each S2C layout;
  they come from ProjectPAULA's writer of that packet.
- **Actions** (`src/app/actions.cpp`): the C2S requests built from the world model, with the layout
  of ProjectPAULA's handler for each one. Requests the server would answer by dropping the session
  (an unknown or passive skill) are refused before they are sent. The Send tab uses them, and the
  automation (phase 4) will too.
- **Game data** (`src/app/game_data.cpp`): item, monster and skill names plus monster max HP,
  read from `ProjectPAULA\db` (`[Data] GameDbDir` in the ini).

## Layout

VS Code's layout and "Dark Modern" colours (`ui_theme.cpp`):

- **Activity bar** (left): one bot icon per session (its name under it, a status dot; right-click
  = unlink / kill / close), then Debug, Settings and "+" (a new session: the Welcome page).
  Clicking the active one hides the side bar.
- **Side bar**: the session's Character (HP / MP bars, level, map, galder) and its Effects (every
  buff on it with the time left, and the skills still cooling).
- **Editor tabs**: the pages of the view. A dot after a bot tab = that action is on.
- **Panel** (bottom, resizable; minimize = only its header, x = hidden): ACTIVITY (the bot's log),
  EVENTS (the world model's), OUTPUT (launcher + DLL log).
- **Status bar**: the client, character, map and position, HP / MP; Start / Stop the bot and
  show / hide the panel. Purple = no client, blue = a client, orange = the bot is running.

> **Game data** (2026-10-25): every table is read from the client's libconfig XML, `bin\conf\paula_libconfig.xml` (a copy of `janus-emulator\doc\paula_libconfig.xml`; `[Data] LibConfig` points elsewhere), the maps from `bin\conf\map` (the client's `data\map\*.md3`) and `bin\conf\stat_table.fnt2`. `src/app/libconfig.cpp` indexes the 10460 tables and makes each ProjectPAULA db file's records from them (same field names; item_db = the four ItemParam tables, mob_db = MonsterParamEx2 renamed, npc_db / shop_db / mon_spawn_db = the tables MapInfoEx names + the CharacterInfo name, ...; Enum lists ORed, zero / empty values left out as the yml files did). Without the XML the yml files of `[Data] GameDbDir` are read. `tas_selftest datacheck` compares the two.

| View > page | What it does |
|-------------|--------------|
| Bot > Overview | State, Start / Stop, the run's numbers (kills, EXP, galder, kills/h, time), the target, the actions in priority order with their switches and status |
| Bot > Heal & Buffs / Loot / Attack | One page per action (Heal and Buffs share one; there is no Rest action: the Heal's potions and heal skills do it; Type 3 skills on enemies - AffectionRel 32: Scorching Earth, Deadly Fen, Armor Breaker - are the Attack's, not buffs): its switch, settings and what it sees (potions in the bag, buff skills + effects on you, the items on the ground nearby, the skill rotation). Loot and Attack have a black / whitelist (items / monster species: search by name or id, or add what is nearby). Loot > Equipment: "Don't loot equipment" (on by default) leaves the common gear grades on the ground — weapons / helmets / shields named Old, Strong (Sharp), Art, Ult. (`GameData::equip_tier`), the Drill's finds too; off, the checked grades are picked up. Supply > Sell sells those grades by default (each one a checkbox). Profile per character |
| Bot > Monster Quest | The activity panel (as the Key Quest's: off / not running / standing still / taking it / to the hunt map / hunting x/y / handing in), then the monster quest: the quests the atlas's NPCs give (search, only for my level), its grades, its NPC, the hunt map (where its monster lives, how many), the portal back / repeat / stop after N, what runs now and the run's numbers |
| Bot > Key Quest | A town's Key Quest: the towns (each one's key quest, its NPC, the stickers in the bag / asked for, done or not); the town's sticker quests with a checkbox each (pick the quickest N / all / none): its NPC, done x/5 as the server told it (`n+` = not known yet), what it asks for each run (orange = nothing drops / buries it), the run that gives the sticker, a time estimate, and the quests the dialogs put before it (a chain, a monster quest); then the key quest and its follow-ups, unless "Only complete sticker quests" is on (the town's quests only, the stickers kept in the bag); the plan (each quest: done, state, NPC, why it waits). Rules in `bot/act_keyquest.cpp`, the dialog graph in `quest_graph.cpp`, the dialog walk in `bot/dialog_walk.cpp`  Megalopolis (Poppuri Dungeon) has no sticker: its key Fix Toys wants 4 Instant Adhesive, which the Poppuri Dungeon's quests give at their 5th run, and its chain goes on to Poppuri Redemption. An item no quest of the plan gives joins the plan through its providers: a quest's or a monster quest's reward (the Garden key of the Secret Garden of Rosetta: Alfredo's monster quest, level 120), several when none gives enough alone (8 Heart Gems from 8 quests), an NPC's gift, an exchange shop; a map no way reaches from here brings in the item that opens it.  On top: the activity panel (the state - not running / standing still / at an NPC / getting items / waiting -, the step, the jobs done, the last request's age, the latest log lines when it stands). |
| Bot > Key Quest / Story Quest: other ways to get an item | "Buy the items a shop sells" (at most N galder a purchase, never below the Town's galder kept): an item the plan needs that a shop reached from here sells is bought - C2S 0x46, 0x48 [shop][1]{item, count} (half as many on 0x4A / 0x4B, three times), 0x4F - before it is hunted; an exchange / a compound whose items the bag already holds; then the quests, NPCs and exchange shops; "Compound the items a compounder makes": what nothing else gives is made at a compounder (Compound_Start func 20 -> S2C 188, C2S 227 [id][10][spins]{uid, item, ReqNum x spins} -> 228, C2S 186 -> 190, C2S 192), spins = the items wanted / its Probability, its ingredients gathered like the rest. `tas_selftest acquire` runs both against a fake server |
| Bot > Story Quest | The scenario quests chapter by chapter (libconfig ScenarioQuestEpisode: 23 chapters, episodes 0..6): each chapter's level, its quests done x/y, its state, whether it has a way (or what stops it) and its special steps (a map only an item opens, an exchange); a click picks the chapter to reach, the Start combo the first one (Auto = the first not done); the chosen chapter's quests (NPC, done, what it asks for, what it gives); the plan (each step: done, state, NPC, why it waits). Scenario quests are taken as soon as their NPC offers them (6 at once). Rules in `bot/act_story.cpp` + `bot/quest_runner.cpp` | Linear: a scenario quest whose history is unknown counts as not done (the chapters before the start one as done), so the plan starts at the chapter's first quest; Check_Job / CheckGender are judged by the character's job (our appear / S2C 570) and model, another class's variant is "not for this character". The items are gathered a batch of runs at a time (40 % of the max weight) and handed in when the batch is in the bag (or the bag weighs 70 %); the Loot takes only quest items from 80 % of the weight; a 92 % bag is a town trip when selling / storing is on.
| Bot > Item Hunt | A list of items and how many of each (search by name or id, then + Add); each one's source (Auto, or a chosen "kill X in map" / "dig in map"), have / want, state; Smart: the plan (the map now, the items it gives and their rates, the species killed / left alone, the route ahead with times, the next best maps; a preview before Start); where the selected item comes from (every monster and map, per kill / per find, the estimate an hour); the run's numbers  The smart plan hunts species at most "Monsters: up to N levels over yours" (default 10, profile hunt.maxOver) and leaves out a map no way reaches now; when nothing is left the stop says why per item (its monster, the level over yours, no way there). |
| Bot > Roam & Travel | The Roam (walk when there is nothing to do, the monster quest's spawn patrol), the Town (death: Go to Town, the rest in town - up to HP / MP %, or until they stop rising for 15 s: a max HP seen with a buff is not waited for -, then back) and the Travel (the map graph, NPC teleports, the Portable Port AD, a trip to a map) on one page |
| Bot > Auto-Supply / Auto-Sell / Auto-Storage | Three switches, a town trip runs for any of them. Auto-Supply: the shop, the refill rows, when a trip runs (bag full / too heavy / weight %). Auto-Sell and Auto-Storage: every item except the ones checked "keep" (a table of the bag's items with a checkbox each, plus a search to keep one that is not in the bag); never sold / stored: what you wear, the refill items, the quests' / Item Hunt's items, the potions and port items the bot uses, cash items (Ncash), rare items unless "rare items too" (sell). An old profile's whitelist is replaced and that switch turned off with a notice, so nothing is sold by surprise |
| Welcome | Open an account, attach to a running client, open a capture; the sessions; the setup |
| Settings | Every `tas.ini` setting and the accounts; Save writes `tas.ini` (made from the default the first time) |
| Notices | When a bot ends (the item hunt's list done, the quest's runs, a trip arrived; green) or stops on a problem (died, no way, the session closed; orange): a Windows notification (the tray icon's balloon = a Windows 10 toast; a click brings the window back), the taskbar button flashing until the window is in front, and a notice in the window's corner (until clicked, or 15 s once the window is in front). Not for a stop you made. Settings > Bot > Notify (`[Bot] Notify=1`). `src/app/notify.cpp`, `NeoApp::notice_stops` |
| Debug > Session | The links: state, hook state, packets, base, unlink / kill / close |
| Debug > World | Radar with the map's walls (wheel = zoom, right-click = walk there; with the bot's target, roam point and start point), entities by distance (HP, owner, target), ground items, events |
| Debug > Character | Stats, HP/MP, EXP, galder, stat points, equipment, inventory, skills |
| Debug > Packets | Live list with filters (`0x18 move !0x76C`), decoded fields, hex, export to `captures\` (tab-separated) |
| Debug > Opcodes | Count / bytes / last seen per opcode; double-click filters the list |
| Debug > Send | Quick actions built from the world (attack, pick up, sit, skill, use item, move), field editor per opcode or raw hex, connection picker, results |
| Debug > Log | Launcher + DLL log |

## App icon

`res\tas.ico` (a gear and "TAS" on the accent blue) is TricksterAutomationStudio.exe's icon through
`res\tas.rc` (icon id 1: Explorer, the taskbar, the window class in `main.cpp`). It is drawn by
`python tools\make_icon.py` (Pillow) at 1024 and reduced to 16..256.

## Game icons

`data\icons` may hold the game's own item, skill and buff icons. They are made from the client's
art, so neither the icons nor the importer are distributed. The app only reads the result:

- Which file + image: the libconfig's ItemParam2 / Ex / Ex2 / CM2 `InvFileName` + `InvBundleNum`
  for items, SkillParam2 `FileName` + `BundleNum` for skills, and `StateIconName` +
  `StateBundleNum` for buff state icons.
- Output the app expects: `icons_<n>.png`, 2048-wide sheets of 32x32 cells, plus `icons.tsv`
  (`kind id sheet cell w h`). The app loads them with WIC into DX11 textures (`gfx.cpp`).

## Self-test

`tas_selftest` is not built by default:
`cmake --build build --config Release --target tas_selftest`.

- `bin\tas_selftest.exe 0` checks the db and decoder.
- `bin\tas_selftest.exe world` feeds a synthetic login, fight and loot through the world
  model and prints the result. It then builds every action on that world and compares the bytes
  with the expected ones (PASS/FAIL), including the move interpolation. Then it runs a bot
  scenario on it: potion, attack, kill, loot its own drop, and leave another player's drop until
  its 30 s are up. Then a gun, a skills-only rotation (walk within reach, cast, wait for the
  answer, the next skill while the first cools, a refusal) and a buff kept up through S2C 224 /
  850 / 280 and a zone change. It also writes `captures\selftest_world.txt` and checks that
  reloading that file gives the same model.
- `bin\tas_selftest.exe town` builds the atlas from the real data and plans routes over it,
  then plays three scenarios against ProjectPAULA's rules: a supply trip (through the portal to
  the town, then the shop: open, prices, buy, close, then back), a death (Go to Town, rest, back)
  and a full bag (warehouse, then sell). Then a Travel-tab trip through a teleport NPC.
- `bin\tas_selftest.exe drill` checks the drill helpers against the server's formulas (the gauge
  time, the shortest dig time, the C2S 0x31 CRC), then drills map 2 with the real Blue / Cleaner
  Drills: the first dig at its block's corner and the next ones along the row, a find reported
  after the gauge's time with a valid CRC and nothing else sent meanwhile (a buff due then waits),
  the find picked up, a free cancel, a sensed empty cell left to the client's own cancel, a
  worn-out drill swapped, an item too deep left, Basic Detection (walks to the block's centre,
  one scan, its points dug, the rest of the block skipped), fight back off / on, a dry block left
  for another area two blocks away, the Crazy Drill Mode with a drill that cannot reach the map's
  depth, and the Supply trip by weight. It writes `captures\selftest_drill.txt` (open it with
  `--open ... --tab drill`).
- `bin\tas_selftest.exe layouts` maps `Trickster.bin` and `TricksterS2.exe` (next to
  `[Client] TricksterPath`) as images, nothing runs, and checks that each matches exactly one of the
  hook's client builds: both capture sites' prologues, the connection vtable, and the avatar chain
  (the world global with its `+0x568` read in the code: 1211 times in Paula, 847 in Season 2).
- `bin\tas_selftest.exe stuck` finds spots on map 2 around a start: one round a wall, one cut
  off but nearer in a straight line, one in the open. It checks that the open one is attacked rather
  than the cut-off one; that the walled one is reached by open legs before the `0x1D` (never stuck
  while drawn where the server has us); that a server chase the drawn character does not follow
  ends in `0x18 [drawn][drawn]` with that target left; that four resyncs in a row pause the guard;
  that nothing happens without the feed; and that an item behind a wall is walked round before the
  `0x21`.
- `bin\tas_selftest.exe stall` (the live log of 2026-10-23): a key quest whose NPC stands in a map not reached at the
  character's level (Butler of Vamp Castle) is "no way now" and never walked toward - its only job, so the bot stops at
  once with the reason (at level 40 it waits for its level 130, and the stop says so); Swamp at level 145: the 140
  quests ready, the 150 ones waiting, never taken, their items only after; an NPC that never answers and no monster around: after 60 s the note (what it was doing, every
  action's status), remedy 1, a step aside at 2 min, every action started over at 3.5 min, the stop at 5 min; EXP every
  40 s: no note. Then the drill (live log: 6.5 min without a line) against a server that refuses every dig (S2C 0x2E)
  and one that never answers: no cell asked more than twice, the log says why each was left, 3 refusals in a minute
  pause the drill 30 s, a block whose digs never start is left. And the NPC away from its point with a look-alike 700
  units off: never chased, no talk, the log names what stands there, the job left alone.
- `bin\tas_selftest.exe route <from> <to> [level] [item ...]`: the atlas route between two maps with that level and
  those items in the bag (each hop: portal, teleport, NPC move, port item), or why there is none, the item that opens
  the map, and its NPCs with their points.
- `bin\tas_selftest.exe quest` checks the dialog argument conversion, counts the quests the bot can
  run, the Drill / Attack / Quest switches, then a whole round with a real quest of the atlas: talk,
  `MonQuest_Accept` with its dialog's arguments, Exit, its monster left alone out of the hunt map, the
  `0x1D` at it (not at a nearer other one) in the hunt map, no hand-in before the 218, the portal back,
  `MonQuest_Complete`, Exit, the next take; its monster far beyond the Attack's radius taken at once;
  with none, 10 minutes of the patrol of its spawn spots (every spot in each round, each round in
  another order and the other way round, the pauses, only open walking legs); and the login list 235.
- `bin\tas_selftest.exe roam` sweeps map 2 for 4 minutes with a small Attack radius: round 1 sees
  every area within reach, no area a goal twice in a round, each round another route turning the other
  way, walking all the time, every leg an open line; then another bot from the same start goes another way.
- `bin\tas_selftest.exe keyquest` (Key Quest): the client's script function table; a message's options; the
  11 towns (10 key quests) and their 169 sticker quests over the real dialogs (how many have a way, with a fresh
  character and with the history unknown; the dump `captures\keyquest_graph.txt`: each one's plan, takes, gates); the
  Swamp survey chain (monster quest 266 x5 -> 7105 x5 -> 7106 x5 -> 7107 x5). Then against a fake ProjectPAULA
  (the PreCheck's first message, C2S 0x72 / 0x7F answered as `npc_talk.cpp` / `quest.cpp` / `monster_quest.cpp` do):
  quest 255 taken and handed in five times by walking its dialog (talk, Quest_Check, two messages, Quest_Accept, Exit;
  talk, Quest_Check, Quest_Complete, Exit), the sticker at the 5th, the book, the stop; the walk to 7107 learning from
  the PreCheck that 7106 was never done and the plan changing; monster quest 266 taken through the dialog, hunted,
  handed in only after its kills, taken again; quest 18 alone bringing 19 and 254 (their rewards) into the plan.
- `bin\tas_selftest.exe story` (Story): the 23 chapters over the real dialogs, each one's plan for a fresh character
  (the dump `captures\story_graph.txt`: quests, takes, hand-ins, providers); the maps only an item opens (no way to
  Ocean's End without the Contract of Secrecy, Love Hunter Robin's move with it; the Hologram Port where you stand; out of
  the Hologram Chamber through Don Cavalier's move; no return wing used where a walk gets there); then a run against the
  fake ProjectPAULA: Episode 5 chapter 1 with a Memory Kit 2 in the bag -> JOKER's dialog (S_Quest_Check, a message,
  S_Quest_Check_2, ExchangeShop_Start), C2S 254, C2S 257, the Approved Memory Kit 2, then C2S 0x57 with it into
  Rosaline's Memory.
- `bin\tas_selftest.exe equip`: the gear grades by name (Old / Strong-Sharp / Art / Ult.; not Chaos, a ring, the
  Ultimate Amadas series), the Loot leaving an Old sword by default and taking it when chosen, the Supply selling them
  by default (not worn, not blacklisted, not kept), the profile keys.
- `bin\tas_selftest.exe hunt` (Item Hunt): the drop and dig tables; the expected drops per kill worked out
  (`item_sources.cpp`) against 200000 kills rolled exactly as `drop.cpp` rolls them, for six monsters; the buried
  items' shares per map; an item's sources for a level-40 character (a dig source unusable without a drill, usable
  with the Blue Drill); the switches; the list through a profile; a hunted item never sold or stored; a kill hunt
  (only the hunted species, not the nearer other one; its drop picked up with the Loot off; done -> the bot stops);
  a dig hunt (the drill put on, then a probe, nothing fought); smart: two items of two species of one map -> that
  map for both, killing both species, the first target the second item's species, an item done takes its species
  out; a 9-item list planned in well under a second, every item routed, never slower than one by one. The `world`
  test has the heal skills too.
- `bin\tas_selftest.exe 30` launches a client, links, prints the captured packets
  and sends `DE AD BE EF` as opcode 0x1234, then kills the client. Run it with a TCP
  listener on 127.0.0.1:9980 to see the frames arrive.

## Roadmap

1. ~~Hook DLL, launcher, IPC, ImGui shell, packet inspector~~ (done)
2. ~~World model from S2C~~ (done, checked live against ProjectPAULA). Known approximations:
   max HP/MP is the highest value seen, because the client derives the real max from the stats.
   0x97 (item used) decrements the count, inferred from the server.
3. ~~Actions as C2S~~ (done; attack, pick-up, sit, skill (0xEE and 0xE0) and use item were
   checked live; movement only with the selftest so far). Quick actions in the Send tab fill the editor, or send at once with
   "Send at once":
   - Attack `0x1D [target][type]`, Pick up `0x21 [oid][type]`, Sit / Stand / Rest `0x40`.
   - Skill: a skill without a selection UI (ESAUI 0) goes out as `0xEE [skill][lv]`. One with a
     selection UI goes out as the client's own reply to S2C 241, sent directly:
     `0xE0 [2][skill][lv][ESAUI][size][target | x,y]`. The server needs no `0xEE` before it, so the
     client's targeting cursor never opens. Target: auto (a buff on yourself, anything else on the
     nearest monster), self, nearest monster, an entity id, or the move point.
   - Use item `0x57 [uid]`: the usable bag items, potions first.
   - Move `0x18 [from][to][type]`. "From" is where the model has you right now, worked out from the
     move segment and the S2C `0x01D8` speed. The server echoes S2C `0x18` to you and the client
     walks on it, the same way it does during the server's own chase. Portal walk is
     `0x1D4 [to][type]`. Right-click on the World radar moves there.
4. ~~Bot~~ (done; checked with the selftest scenarios, the live check is pending). It runs per
   client in `src/app/bot/`. Each action is its own module with its own settings and state:
   `act_heal`, `act_town`, `act_rest`, `act_buffs`, `act_loot`, `act_attack`, `act_drill` and `act_roam`. Every tick the
   actions observe the world. Then they are asked in priority order, and the first one with
   something to do sends ONE request (300 ms apart). The bot keeps running while the window is
   minimized. The server rules each action follows (ProjectPAULA):
   - **Heal**: a potion below the max only. It uses the smallest one that fills the gap and waits
     for the answer. **Heal skills** (the Heal tab lists the learned ones; checked = cast, in their
     order) at or below their own HP %, by the skill's ESAction: 1085 Cure (`skill.cpp` esa_heal, ESAUI 1 ->
     `0xE0` with our id: MaxHP x Ratio), 1168 Catastrophe Healing (`skill_dragon.cpp` esa_first_aid, `0xEE`:
     (MA + LK) x 0.5 x Ratio), 1323 Heart's Grace (esa_wide_first_aid: the party within Radius, us included,
     alone too: MaxHP x Ratio + MA x Ratio x 40), 1482 Chakra Balance (`skill_power.cpp` esa_self_cure: heals
     over time and ends at the first swing at us -> only out of a fight, not while it runs). One skill request
     at a time (none while another waits for its answer, none while a dig is open), then nothing until the HP
     moves. The HP potions can be turned off (a Dragon heals itself; the MP potions keep going); on, the HP
     potion goes first below its own %. Heal skills are not listed in the Attack's rotation.
   - **Rest**: `0x40 [13]` out of a fight. Sitting blocks move, attack and pick-up, so every other
     action stands up first.
   - **Loot**: a drop belongs to its monster's loot owner (S2C 0x16D) for 30 s, and the server
     refuses anyone else's pick-up without a reply. A pick-up ends the attack. So the bot takes
     its own kills' drops between two targets, even with monsters around, and free items only out
     of a fight. An item that stays on the ground is left alone for 60 s. Galder has its own
     switch; every other item goes through the black / whitelist. A shadow monster's drop (the
     item's event flag) needs the Third Eye.
   - **Buffs**: the checked buff skills are kept up. Every effect on you is tracked from the
     packets: S2C 224 `[caster][target][esId][ESAction][2][ESClass][skill][EState]` starts it,
     280 `[target][esId]` ends it, 850 `[ESAction]` says 10 s are left. Its length is the skill's
     ESAction row `Seconds`. A zone change gives the carried effects new ids with no 224, so the
     next unknown 280 ends the one whose time is nearest. A buff is cast again when it is gone or
     ends within N s, once its cool time and MP allow.
   - **Skills (all actions)**: one request at a time, like the client's lock bit (0x20 of
     player+26512): the bot waits for S2C 240 (fired; the cool time starts: `CoolTime - 0.5 s` at
     your level, from `esa_db.yml`), 239 (refused; 12 = cool time) or 243 (cancelled), or a
     timeout. A refused skill rests 1-5 s.
   - **Attack**: the monster hitting us first, else the nearest one in the radius that the black /
     whitelist allows (whatever hits us is fought back unless that is switched off). Shadow monsters
     (16000..16999) only with the Third Eye up (ESAction 1390 on us, Mind's Eye 8001): without it the
     server ignores the attack, so a shadow target is dropped when it ends. Optionally other
     players' monsters are skipped; a target is left after 12 s with no progress.
     - **Rotation**: a list of skills in priority order; the first ready one (learned, off its cool
       time, MP / HP for its cost) is cast at the target. The tab lists every learned attack skill,
       as Buffs does: check the ones to use, the arrows set the order. Black / whitelists sit at the
       bottom of every tab. A Chase skill (ESAIC 2) goes from
       anywhere, because the server walks into range itself. Any other one needs us within its
       ESAII `Range` (150 around us for a self-centred one with none), so the bot walks there first.
     - **Basic attack** (can be switched off for skill-only classes):
       - **Melee**: `0x1D`, sent again right after a skill.
       - **Gun**: only with a gun (SubType 54) in the weapon slot and bullets. It loads them with
       `0x205` and asks for a seed with `0x1FF`, then sends ONE `0x1DD` per shot. Each shot is
       keyed with the last S2C 0x0200 seed and comes at least the FastShot time
       (`(int)((int)(20000/(GaugeSpeed/6))*0.7)` ms) + 250 ms after the last shot or weapon change.
       The first shot that brings no new seed turns the gun off for the run, because three bad
       shots cut the player.
   - **Drill and combat exclude each other** (`Bot::sync_modes`): turning the Drill on turns the Attack
     (and the Monster Quest) off, turning the Attack on turns the Drill off. In drill mode the only
     fighting is the Drill tab's "fight back". The Monster Quest needs the Attack: on, it turns the
     Attack on and the Drill off; the Attack turned off turns it off. A profile with all of them on
     keeps the drill mode.
   - **Drill** (`act_drill.cpp`, the Drill tab; ProjectPAULA `drill.cpp` + the KR client's gauge). On = drill mode:
     nothing is hunted. The Attack action only fights a monster that swings at you, and only with
     "fight back" checked (with the Attack tab's weapon and skills, whatever its lists say). Roam is
     off; the drill walks by itself.
     - **A dig**: `0x2C [x][y][type]` at a cell. In your own cell the dig starts at once; in another
       one the server walks you there in a straight line, so the bot only asks cells within 400
       units on an open line and walks the BFS path first otherwise. S2C 371
       `[me][x][y][0][mapDepth/5][itemDepth/5][nature][seed|sensed][CDM %]` gives **the depth of the
       cell's item (0 = none) whatever the sense roll says**. The roll only decides what the client
       shows; a sensed empty cell makes the client cancel by itself, so the bot waits for that.
     - **The find**: an item the drill reaches is reported with `0x31 [cnt 1][ms][depth][crc]`, the
       CRC being `seed ^ BarEffArea ^ BarSpeed<<1 ^ ScoopMass<<5 ^ depth<<8 ^ cnt<<12`. It goes out
       once the client's gauge would have got there for a player who never lets it overheat:
       `600 * (200 - BarEffArea) / BarSpeed` ms for the bar, then 120 ms per ScoopMass, plus
       150-450 ms. The server cuts the session for a find faster than 0.7 x that, a bad CRC or a
       depth past DigDepth / the map's depth, so `act::drill_end` refuses those. One start = 1 life
       (twice on a ground other than the drill's GroundNature).
     - **Anything else** (no item, or one too deep for the drill) is cancelled at once with
       `0x30 [0][ms]`, which costs no life. "Dig the cells with nothing to find" digs it to the
       bottom instead, for the dig's EXP (1 life).
     - **Where**: the model keeps what is known of every cell of the map (`WorldModel::buried`):
       our own 371s, other players' digs (S2C 47 / 51), the ends (a dug-out cell is empty; the
       server lays the new item at a random cell) and the detectors. The map is worked **block by
       block, in lines**, as players do: a block is the detector's square (`skill.cpp
       detect_buried`: (2 x Radius) / 16 cells, starting at an odd cell, e.g. 15 x 15 for Basic
       Detection Lv 11), else "Lines" cells a side (11 by default). Its cells go row by row in a
       snake from the corner nearest to you; with "the drill's ground first" its own-ground cells
       come first, then the rest; "only the drill's ground" skips the rest. The next block is the
       one of the nearest cell to dig by walking (a BFS over the map's walls; a known item is worth
       10 cells). The area is a radius around the start point, or the whole map.
     - **Detection skills** (checked ones): **one scan when a block starts**, never in the middle
       of one, and only when the map's items lie within the skill's reach and a quarter of the
       block is still unknown. Basic Detection (3013, reach = its row's Depth) is cast from the
       block's centre, where its square is exactly the block; the Item Detector (3102, reach =
       DA x DepthRatio / 10) at the block's centre from within its Range. The bot gets in position
       first and waits for the cool time a few seconds (17 s more when it is looking for a new
       area), else it works that block in lines without it. The S2C 224 tail
       `[applied][n]{x, y}[secs]` marks each point as an item; the other open cells of the square
       hold none down to the reach (50-point cap kept). Then only the points are dug.
     - **Another area**: a block where N probes in a row find nothing (N = 4 / the chance that a
       cell holds an item your drill reaches: ItemPercentage x the share of the map's depths it
       reaches, 6..30) is left for 10 min. A block left without a find (failing, or a scan that
       showed nothing) makes the next block keep away from it: at least one block between them.
       Around the start point, when nothing is left there, "move on when it runs dry" moves the
       start point to the nearest block that still has cells to dig.
     - **Between digs only**: a skill, a sit / stand or an attack ends an open dig (`skill.cpp`,
       `game_session.cpp`), so while one is open only potions go out. Buffs (the Drill tab lists the
       DA buffs — Sixth Sense, Concentration, Aura of Anger — and Crazy Drilling, sharing the Buffs
       action's checkboxes), rest, loot and town trips wait for its end. After the end the next dig
       waits 0.8 s: the client stays in its drill state 700 ms and a new S2C 0x2D before that would
       not restart its gauge.
     - **Finds** pop up beside you (S2C 0x0E, yours for 30 s) and are picked up whatever the Loot
       lists say (a switch). The Drill tab lists every item dug up with its count and a "sell"
       checkbox (the Supply sell list), and "sell the checked items when the bag weighs N % or
       more" turns on the Supply trip by weight (`TownAction::weightPct`, MaxWT as the server counts
       it; the shop is the Supply tab's). A worn-out drill (life 0), or one too short for the map's items, is
       swapped for the best of the bag (`0x51 [uid][7]`): the one reaching the map's depth, then the
       one whose ground covers most of the map, then the grade.
     - **Crazy Drill Mode** (S2C 0x32 [11][secs] .. [16..18]): a dig is instant (no 371, 1 life,
       200 ms apart). Known item cells come first, any depth. A drill that does not reach the map's
       depth gets "LowDrillGrade" on every dig, so the bot waits for the mode to end.
     - A model check: the inventory count of a drill is its life (rare items count as one in
       `count_item`, as the server's GetCount does).
   - **Monster Quest** (`act_quest.cpp`, the Quest tab; ProjectPAULA `monster_quest.cpp` + `npc_talk.cpp`). A
     repeatable quest of an NPC, round and round: take it, kill its grade's monster in the chosen map
     within its time, come back, hand it in, take it again.
     - **The quests**: `monster_quest_db.yml` (5 grades: the one whose level range holds yours says the
       monster, the count, the minutes, the EXP). The tab lists the ones an NPC of the atlas gives: a
       QuestId slot of its `npc_db` row and the take in its dialog (358 of 600). The hunt map: where
       the monster is regenerated (`mon_spawn_db` through each map's MonsterTable), the most first;
       auto = the most.
     - **Take / hand in**: walk to the NPC, `0x6F`, then `0x7F [npc][func][n][args]` with the action of
       its own dialog (`npc_msg_db`): `MonQuest_Accept` (8; `_1.._4` = 242..245 for its QuestId1..4)
       and `MonQuest_Complete` (9; 246..249), `Once_Accept` 185 / `NH_Complete` 149. The arguments are
       message numbers, converted as the client does (`m<n>` = 256 + n, `q<n>` = 128 + n, `s<n>` = 512 +
       n; ids and syntax checked in the client: table `0xFACE78`, `sub_C033A0`). Then `Exit` (func 1:
       S2C 117 closes the client's window). Taken = S2C 210 + the timer 215 + the kills 217; handed in
       = 219 + 211. A refusal (level, MaxCount, once a day, full bag) is a message without them: asked
       again after 60 s, the bot stops after three.
     - **The kills**: only the quest's monster, only in the hunt map, and only while the kills are to
       do; a kill counts for the monster's owner (the first to hurt it). Nothing is fought on the way;
       Loot and Roam work in the hunt map. 218 = all killed. The Attack takes the quest's monster
       anywhere on the map (the nearest by the walk, no radius: the map is one sector); with none to
       take (all dead, another player's, out of reach) the bot patrols its spawn spots (the Roam is
       asked there even when off). Its regeneration points (`regen.cpp`: a Mon_ row's Region R > 0 ->
       the type-6 points of id R, 0 -> the type-3 points; each monster comes at a random one) are
       grouped: the point that sees the most others within 400 units (an open line) is a spot and takes
       them, then the same among the rest (at most 12; 280 / 200 units when that gives fewer than 3).
       The spots are gone over in rounds like the sweep's areas (below): the next one drawn among the
       nearest not visited this round, each round the other way round, a pause of about `spotPause`
       seconds (Roam tab, 0.6..1.4 x) at each; a spot passed on the way counts.
     - **Back**: the success portal `0x410 [u32 2]` (only while the quest's timer runs) -> S2C 0x411 and
       a relocate to its ReturnMapID; refused (0x412) or out of time -> walking. **The hand-in goes only
       after the 218** (or a login 235 entry with its record): before that the server drops the player.
     - After a death the Town trip ends in town and the quest leads the way (the quest is lost if its
       kills were not done).
   - **Item Hunt** (`act_hunt.cpp`, the Item Hunt tab; data `src/app/item_sources.cpp`). A list of items and
     how many of each; the bot goes where each comes from and works there until the bag holds them all
     (counted with `count_item`; then it stops, by default, with a notice). **Smart** (the default) plans the whole list;
     off, the items go one by one in the list's order.
     - **Where an item comes from**, from the server's own tables and rolls. Monster drops (`drop.cpp` +
       `world/drop_db.hpp`, retail loaders kept: float bands, the sub-tables' never-cleared 20-slot buffer):
       ItemDropCnt rolls of `10000 * rand() / 0x8000` against the item bands, then the sub-table bands; a
       sub-table fills the loot up to ItemDropCnt with `rand() % total` (which favours the low values — kept
       exactly); a rare item drops one. The expected count of every item per kill is worked out exactly over
       the loot size (checked against 200000 rolled kills). Buried items (`drill.cpp` map_item_pop +
       `world/drill_db.hpp`): a find's item is rolled only at the find, its type by the map's S / E / N1..N3
       rates x (1 + Rate_Add) and its own Item0..29, then the item within the type: its share of a find.
     - **The sources** of an item: Kill = a map where species that drop it live (`mon_spawn_db`), per kill
       their mix weighted by how many of each the map keeps; Dig = a map that buries it, usable with a drill
       in the bag that reaches its depths (10 x [Min/10, Max/10]). The estimate an hour only ranks them
       (about 300 kills an hour with 10 of them in the map, less for fewer or stronger ones; a probe ~3 s and a
       find ~8 s more); Auto takes the best usable one, or the list's row names one.
     - **Kill**: the Attack takes only those species, only in that map (whatever its lists say, anywhere on
       the map: one sector), the Roam patrols their spawn spots (the quest's patrol), nothing is fought on the
       way. **Dig**: the Drill works there with its own settings (drill mode). The list's items are always
       picked up (even with the Loot off) and never sold or stored (`town.keep`). In a source's map with none
       of its item for `switchMin` minutes (30) the next source is tried; no way to the map for 60 s: the same.
       Nothing changes while a dig is open. After a death the Town trip ends in town and the hunt leads.
     - **Smart plan** (`make_plan`): the time to finish the whole list. Each missing item's rate at each place (a
       map killing: its own best mix of the map's species up to 10 levels over ours; a map digging) and its best
       anywhere. Per place: every missing item it gives, less the ones quicker at their best place (the way, ~6 min
       a map, included) than the time they add here; the farm there, each step killing every species that drops
       one of its items still missing (an item done takes its species out); the whole list = the way + the farm +
       the rest one by one. The quickest place is hunted (its whole species mix is the Attack's target), the bot
       moves when another is 15 % quicker; the plan is made again at every count change. The pace model: a kill
       = its fight (12 / 20 / 40 / 120 s up to 0 / 5 / 10 / more levels over ours) + the walk to the next target
       (2 s with 10 or more targets in the map, up to 14 s with one); the bot takes the nearest, so each species
       of a mix is killed as often as the map keeps it. The tab shows the route ahead and the next best maps.
     - The Quest and the Item Hunt exclude each other; both bring the Attack, the Drill turns them off.
   - **Roam**: when there is nothing to fight or loot. Modes: stay at the start point, sweep around
     it, or sweep the whole map, on the map's `.md3` layer 1 from `[Data] MapDir` (default
     `ProjectPAULA\data\map`).
     - **The server shows every monster of the map**: ProjectPAULA (as the Paula GameServer.ini) has
       no SectorWidth, so a map is one sector and its monsters' appears / moves reach us wherever we
       are. A monster the Attack wants but out of its radius is walked to at once: no searching for
       what the bot already knows.
     - **The sweep** (`act_roam.cpp`), for the rest: the map in areas of 256 x 256 units, each with its
       open centre. An area is seen when its centre comes within the Attack's radius of you. The sweep
       goes in rounds, every area once a round. The next goal is drawn among the nearest areas not seen
       this round (by the walk; a goal against the round's sense round the middle of the map counts up
       to 25 % longer), and each round turns the other way (clockwise, then counter-clockwise), so the
       route changes from round to round instead of repeating (the next one at once when the goal is
       seen on the way; after a fight the goal is drawn again from where the bot stands). It walks the
       grid path (Navigator legs, each an open line, no pauses); an area it cannot reach is left out for
       10 minutes. Without the map file: random legs, blind.
   - **Walls and getting stuck** (`bot/stuck.cpp`). ProjectPAULA walks every chase (`0x1D`, Chase
     skills), pick-up walk (`0x21`) and move in a straight line, through walls (`player_order`; the
     retail anti-stuck warp is not ported). The client path-finds each move the server orders for us
     (an A* over the same `.md3` layer: 255 nodes, 128 points) and leaves the character standing when
     it finds no way. The server then fights and walks on from a place the client never reached.
     - **Prevention**: targets and items are chosen by the walk on the map's grid, not the straight
       distance. One in another area, or round a long wall (a walk over 3 x its distance + 200), is
       left alone; an attacker within 96 is fought wherever it stands. With a wall on the straight
       line, the bot walks the grid path itself (`0x18` legs, each an open line) and attacks or picks
       up once the line is open (30 s at most, else 2 min off). If the server has us inside a wall,
       the grid cannot judge and the straight line is used.
     - **Detection**: the hook's drawn position against the server's. 48+ units apart while the
       drawn character stands still for 1.2 s (or 6 s while it walks a detour) = stuck. The bot then
       sends the client's own stop-key request, `0x18 [drawn][drawn]`: the server trusts the source,
       so it is back where you see the character. The target, item, drill cell or roam leg involved is
       left for a while (a target / item 2 min). Four resyncs within a minute mean the drawn position
       cannot be right: the guard pauses 5 min. A drawn point outside the map's bounds is ignored. The
       Bot overview shows both positions, the count and the pause.
   - **Town** (`act_town.cpp`, second in the order, right after Heal): the trips away from the hunt.
     - **Death**: after N s, `0x16` Go to Town. The server revives you in the return town with 30% HP
       (S2C 0x17 + the map change). In town the bot rests (`0x40 [13]`) up to the set HP / MP; the
       Heal leaves the potions alone during that rest. Then the supply errands (optional), then back
       to the map where it died and its start point there, or to a chosen map, or it stops in town.
       Off = the bot stops on death, as before.
     - **Supply** (the Supply tab): a trip starts when a refill item drops to its "trip at" count,
       when the bag reaches N items (300 max), or when a pick-up is refused for the weight (S2C
       0x23), or (optionally) when the bag weighs N % of the max weight (the MaxWT stat). It never
       starts during a fight. The errands, in order:
       - **Warehouse**: walk to the keeper, talk (`0x6F`), then `Warehouse_Start` (`0x7F` func 16
         with the arguments of its message, e.g. `1,2,3`). That costs 50 galder and needs level 5.
         Then one `0x8B [tab][uid][item][count]` per item, waiting for each answer (140 / 141). It
         tries the next tab (2, 21..29 per the slots) when one refuses, then `0x8A`.
       - **Shop**: walk to it, then `0x46`. Sell first with `0x39F` inner 76
         `[u16 76][""][shop][n]{uid, item, count}` (up to 20 lines). Then ask the prices (`0xA4` ->
         S2C 0x47) and buy up to each "buy up to" count with `0x48` (up to 20 lines, within the
         galder minus the galder kept). On a refusal (0x4B weight / 0x4A) it tries half as many.
         Then `0x4F`: the shop state swallows every move until then.
       - **Back** where the trip started (its start point there).
       What is never sold or stored: what you wear and the refill items. Never sold: Options 0x200.
       Never stored: Options 0x10000 and the loaded bullets' kind (the server refuses both). A trip
       that does not fix its cause means no new trip for 10 min.
     - **On the way**: the lower actions only defend. Attack takes only what hits you; loot, rest
       and roam wait.
   - **Travel** (`navigator.cpp` + `src/app/atlas.cpp`, the Travel tab): the atlas holds every map's
     portals (the `.md3` type-1 regions) and points, plus the NPCs, shops, warehouses and teleport
     NPCs of `npc_db` / `shop_db` / `teleport_db` / `npc_msg_db`, the NPC moves of `npc_move_db` and
     the port items of `random_port_db`. That is 695 maps, 980 portals, 94 shops, 12 warehouses, 505
     teleporters (271 NPC moves) and 49 port items, built in about 0.1 s.
     - **Route**: the quickest by time. The cost is the walks, ~4 s per map change, and a
       teleport's talk and price. The search is an A* over the arrival points: where a hop leaves
       you is a node — the start, a portal's matching type-1 point, or a random one of a set (the
       mean over its points). Every walk is measured on the map's grid: a distance field from each
       point, cached, to every portal (its rectangle) and teleport NPC. The walk to the goal point
       of the last map counts too. A teleport counts only when your level reaches its MinLevel and
       you have the galder. Every way out of the map is tried (`Atlas::alternatives`). The log names
       the runner-up ("next best ~N s by ..."), and the Travel tab lists every way out with its time.
     - **In a map**: an A* path on the 16-unit grid (8-way, no corner cutting), pulled into straight
       open legs of `0x18`.
     - **Portal**: the path goes to the nearest open cell of the rectangle (grown by 16) and its last
       leg is the `0x1D4`. The server takes you when your position and that point are inside it
       (grown by 24).
     - **Teleport NPC**: talk (`0x6F`), then `Teleport_Start` (`0x7F` func 19 `[slot]`).
     - **NPC move** (`npc_move_db`): talk, then `Move_Complete` (func 15) with its dialog's arguments; it
       wants its items in the bag (Love Hunter Robin -> Ocean's End: the Contract of Secrecy), its level and
       FType; the move comes 2 s later on the zone's type-9 point. Free, so it is not the teleports switch's.
       It is the way out of the Hologram Chambers (Don Cavalier).
     - **Port item** (`random_port_db`, a row with one zone): `0x57` where you stand (not from an Option
       128 map). It is used up, so a route takes one only when no portal / teleport / move gets there
       (the Hologram Port, the Holographic Photo, the Approved Memory Kits); a return wing stays in the bag.
     - **Portable Port AD** (19501 / 19502 / 19505: Type 15, Effect 20, Options 0x8000): `0x0108 [uid][i16 4]
       [u32 ZoneIndex]` where you stand, to any of the 31 towns and gates of `teleport_adv_db` (not from an Option
       128 map). One item each use, so it counts as ~15 s more than a map change: a route starts with it whenever
       that is still quicker (Coral Town -> Gate of Vamp Castle: 1 use, ~20 s, against 13 maps, ~216 s). The Travel
       tab's "Portable Port AD" switch (profile `town.navPortAd`) turns it off; a use that does not port you = no
       Port AD for 5 min.
     - **Map change**: the client reconnects (154 -> C2S 156 -> 437 / 157), so the bot sends
       nothing until it is over. The session follows the new link through its C2S 156.
     A hop that does not take you is tried once more (and not blamed at all when the hook draws the avatar far from it: the client never walked there), then left out for 2 min; three left out within 2 min = the moves are failing: every one back in and 15 s of waiting, never a detour; a route round left-out hops longer than 1.5 x the direct one + 30 s is not taken. The map you are in is always a walk; a port item is used only when no way exists at all. "Go" on
     the Travel tab starts the bot if it is stopped, travels, and stops on arrival. The Debug >
     World radar draws the map's portals and the walk's legs.

   - **Watchdogs**: nothing sent for 30 s while it could act -> an "idle 30 s" snapshot in the log. No progress for
     60 s in a goal mode (key quest, story, monster quest, item hunt, a town trip) - progress = new ground (150+ units
     from every spot of the stall: a walk back and forth is none), a map change, a kill, EXP, galder, an item, a quest
     step - -> a "no progress" snapshot and a remedy that grows: the errand under way dropped (its job left 5 min) and
     a dialog left open closed; a step aside (2 min); every action started over (3.5 min); the stop with the reason
     (5 min); the same errand dropped again waits 20, then 60 min (kept over the restart). A quest job whose NPC no
     route reaches from here now is "no way now" (not walked toward); a quest under its MinLevel "waits for its level" (the
     server's take answers 3: never asked for; the quests after it wait too) and one over its MaxLevel is blocked; the
     town's quests go by MinLevel (Swamp: every level-140 one, then the 150 ones), the items of those waiting for their
     level are gathered only once the others' are in the bag, and when only those are left the bot stops: "N quests wait
     for level L (now M)". An NPC is looked for only within 400 of its
     point (the server spawns it there; one of the same look farther away is another NPC - chasing it was a back
     and forth), else it is left 10 min at the first miss and the log says what stands at its point; every job waiting
     = the Attack fights what is around meanwhile. The drill mode is watched too (a dig is progress), and the
     drill never asks the same spot again and again: a cell whose dig does not start (refused, no answer, a walk
     that never gets there) is left 60 s, 10 min the second time; 4 such cells leave the block. A log line said
     again is counted ("the line above again: N times") instead of vanishing. The log is appended to
     `logs\bot_<char uid>.log`.

   Settings are saved per character in `profiles\bot_<char uid>.ini`. The bot sends on the game
   link, never on the community link.

   Max HP / MP is not on the wire, so the model works it out like the server does
   (`recompute_stats`). Each one is `gulim13[points / 4 + 1]` from `db\stat_table.fnt2`, using the
   points of the 33-byte stat block or S2C 0x6D. Each worn item then adds its options `10000 + stat`
   when it has any, else its ItemParam `MaxHPPlus` / `MaxMPPlus`. The model takes the larger of that
   and the highest HP seen, because buffs are not counted. It was checked against the server's own
   1890 / 500 for a level-60 character. Before this, logging in hurt made the heal fire far too late.

   Known limit: an effect cast on you by someone else is timed with your own level of that skill
   (or level 1).
