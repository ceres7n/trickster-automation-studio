# Trickster Automation Studio

Automated test clients for **Trickster Online** server emulators.

Trickster Automation Studio launches the real game client, hooks its network layer and drives it
from a desktop panel. Everything it knows comes from the packets, and everything it does goes out
as the packets the client itself would send. It reads no game memory for state. That makes it a
practical tool for exercising a server end to end: log in, walk the maps, fight, loot, drill,
run quests and town errands for hours, and watch every packet while it happens.

It is **server-independent**. You point it at *your* server's libconfig XML and data folder, and it
reads every game table from there: items, monsters, skills, maps, NPCs, dialogs, quests, shops,
drops, compounds and teleports.

> **Use it on servers you run or are allowed to test.** No game data, client binaries or game art
> are included in this repository; you bring your own client and your own server's data.

## What it does

- **Sessions**: launch the client for an account (server IP redirect, splash bypass, hook
  injection) or attach to a client you launched. Several clients can run side by side.
- **Packet tools**: live capture of every C2S / S2C packet (plaintext, before encryption), a
  decoder driven by `data/packets_db.yml`, filters, export / replay of captures, and a Send page.
- **World model**: you, the entities around you, the bag, skills, effects, quests and the map, all
  rebuilt from the packets. The Debug pages show it, including a radar with the map's walls.
- **Bot**, as a list of actions in priority order:
  - Heal & Buffs: potions, heal skills, buffs kept up.
  - Loot and Attack: the skill rotation and area skills (black / whitelists).
  - Drill: the dig gauge and depths, worked out like the client does.
  - Monster Quest: a repeatable monster quest, round after round.
  - Key Quest: a town's sticker quests and its key quest, followed through the NPC dialogs.
  - Story Quest: the scenario quests chapter by chapter, in order, with each class's own variants.
  - Item Hunt: a list of items, planned against the drop and dig tables.
  - Roam & Travel: an A* route over every map's portals, NPC teleports and port items.
  - Auto-Supply / Auto-Sell / Auto-Storage: town trips to refill, sell and store.
  - Items for the quests also come from shops, exchange shops and compounders.
- **Self-test** (`tas_selftest`): scripted checks of the world model and of every bot action
  against fake server answers.

## Requirements

- Windows, a **32-bit** build (the client is 32-bit): Visual Studio 2022 or later with the C++
  desktop workload, and CMake 3.20 or later.
- A supported client build. The hook's code addresses are build-specific: see
  `src/hook/client_addr.h`, which has the layouts for the KR 2014 "Paula" client (`Trickster.bin`)
  and a Season 2 `TricksterS2.exe`. Another build needs its own layout there.
- From **your server**:
  - its **libconfig XML**: the `<INI><TABLE name=...><FIELDINFO .../><ROW>...</ROW></TABLE></INI>`
    export of the game tables (e.g. `paula_libconfig.xml`);
  - its **data folder** (the client's `data\` works): `map\*.md3` for the maps and walls,
    `gulim13.fnt2` for the stat table.

## Build

```
cmake -S . -B build -A Win32
cmake --build build --config Release --target TricksterAutomationStudio tas_hook tas_selftest
```

The output goes to `bin\`:

- `TricksterAutomationStudio.exe`
- `tas_hook.dll`
- `tas_selftest.exe`
- `data\`
- `tas.ini.default`

## Set up

Open **Settings** in the app (the gear), or edit `bin\tas.ini`. The app writes it from
`tas.ini.default` the first time you save.

| Setting | What to put there |
|---|---|
| Data > Server XML (libconfig) | Your server's libconfig XML. Empty = the first `conf\*.xml` next to the exe. |
| Data > Server Data Folder | Your server's / client's `data` folder (`map\*.md3`, `gulim13.fnt2`). Empty = `conf\` next to the exe. |
| Client > Trickster Path | The client to launch (`Trickster.bin` or `.exe`). |
| Client > Original IP | The server IP baked into your client build. Every copy in memory is redirected to the Login IP. |
| Server > Login IP / Port | Your login server. |
| Accounts | `login=label` pairs: one "Open" each on the Welcome page. |

The simplest layout is a `conf\` folder next to the exe:

```
bin\conf\your_server_libconfig.xml
bin\conf\map\*.md3
bin\conf\gulim13.fnt2
```

The Welcome page shows what was found: the XML, the maps, the client and the server.

## Run

- `TricksterAutomationStudio.exe` opens on the Welcome page: open an account, attach to a running
  client or open a capture.
- Command line:
  - `--launch <login>` (repeatable)
  - `--attach <pid>`
  - `--open <capture.txt>`
  - `--tab <page>`, where a page is `overview`, `heal`, `loot`, `attack`, `drill`, `quest`,
    `keyquest`, `story`, `hunt`, `travel`, `supply`, `sell`, `storage`, `settings` or a Debug page
  - `--shot <file.png>` renders a few frames to a PNG and quits
- The bot pages each have a switch. **Start** runs the bot, and the activity panel of the quest
  pages says what it is doing and why it waits. Every run is logged to `bin\logs\`.

## Self-test

```
bin\tas_selftest.exe world      (also: quest hunt town drill stuck roam keyquest equip misc story stall acquire)
bin\tas_selftest.exe datacheck  (the libconfig XML against a yml db folder)
bin\tas_selftest.exe route <from zone> <to zone> [level] [items...]
bin\tas_selftest.exe sources <level> <zone> <item>...
bin\tas_selftest.exe qinfo <quest name>
```

Most modes read the game data, so they need the XML and the data folder, as the app does. Many
checks name quests, maps and monsters of the KR 2014 data, so another server's data can fail
those checks without anything being wrong.

## Item icons

The panel shows item and skill icons when `data\icons\` holds the icon sheets (`icons.tsv` and
`icons_<n>.png`). They are made from the client's own art, so they are **not** included. Without
them the panel works the same, just without pictures.

## Layout

```
src/app/        the panel: world model, game data (libconfig.cpp), atlas / routes, quest graph, UI pages
src/app/bot/    the bot actions (heal, town, buffs, quests, hunt, loot, attack, drill, roam), navigator, dialog walk
src/hook/       tas_hook.dll: the C2S / S2C capture and the sender, inside the client
src/shared/     the pipe protocol between the panel and the hook
data/           packets_db.yml (the packet definitions), packets_neo.yml
tools/          selftest.cpp, make_icon.py
docs/           DEVELOPMENT_NOTES.md: how each part was reversed, built and verified
3rdparty/imgui  Dear ImGui (MIT, see its LICENSE.txt)
```

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut and contributors (MIT).
- Trickster Online is a trademark of its respective owners. This project is not affiliated with
  them.
