# EconSpace

**A 2D EVE-like space-sim sandbox with an authoritative client–server core, built in C++ on top of [raylib](https://www.raylib.com/).**

You pilot a ship in a persistent, multi-system galaxy: mine, trade, run missions, fight, and build reputation with factions — while the galaxy simulates itself around you on an authoritative server.

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%20(MinGW)-lightgrey.svg)
![Status: Prototype](https://img.shields.io/badge/status-prototype-orange.svg)

> **Project status — honest version.** EconSpace is an engineering-driven **prototype**, not a finished game. The client–server architecture and netcode are solid and real; the *content* is not: sprites are placeholder shapes (no art yet), there is no audio, and the world is small. See [ROADMAP.md](ROADMAP.md) for where it is and where it's going. Contributions — especially art, content, and gameplay depth — are very welcome.

---

## What's in it

EconSpace runs its own game logic on top of raylib (windowing/render/input only). The same simulation runs locally (single-player) and headless (dedicated server), so single-player is literally "the server in the same process."

**Gameplay**
- Newtonian-ish flight, warp travel, autopilot, and a parallax starfield.
- Mining, a station market (buy/sell with price impact and recovery), and ship refits.
- Weapons and combat shared by the player and NPCs (anyone can fight anyone).
- Missions: bounty, mining, and delivery — with a station job board and a journal.
- Factions, reputation tiers, wanted levels/bounties, and role-based NPC AI (trader, miner, police, pirate, warship).
- A multi-system galaxy connected by jump gates, with a full-screen star map.

**Simulation & world**
- An authoritative `Simulation` core with a fixed `1/60` tick, decoupled from rendering.
- A "living galaxy": every system is simulated (system controllers, gate-line economy, territory captures, an event feed) — visible on the galaxy map.
- Persistent systems and agents with stable ids.

**Tooling**
- A visual **world editor** (`worldeditor`) for editing systems and galaxy links, saved to JSON.
- A **headless server** (`econserver`) that runs the exact same simulation without a window.

**Networking**
- A `Command` / `Snapshot` / `SystemLayout` protocol over a swappable transport (`ITransport`): in-process loopback for single-player, TCP (winsock) for real networked play.
- Client-side prediction + server reconciliation with input replay for the player's own ship, and entity interpolation for everyone else (the classic [Gambetta](https://www.gabrielgambetta.com/client-server-game-architecture.html) model).
- Server-authoritative combat, docking, trading, missions, and player accounts.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how it all fits together.

---

## Build & run

**Requirements**
- A C++17 compiler — MinGW-w64 g++ (from [MSYS2](https://www.msys2.org/)).
- CMake 3.16+.
- Internet on the first build: [raylib](https://github.com/raysan5/raylib) 5.5 and [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 are fetched and built automatically via CMake `FetchContent`.

```sh
cmake -S . -B build -G "MinGW Makefiles"   # configure (first build downloads & builds deps — slow)
cmake --build build                        # build game + editor + server + tests

./build/bin/game/econspace.exe             # run the game (client)
./build/bin/editor/worldeditor.exe         # run the world editor
./build/bin/server/econserver.exe          # headless galaxy simulation

ctest --test-dir build --output-on-failure # run the unit tests (doctest)
```

> On Windows, close the running game/editor window before rebuilding — Windows won't let you overwrite a running `.exe`. The `data/` folder is copied next to each executable on every build.

### Play over the network

```sh
# terminal 1 — start an authoritative server on a TCP port
./build/bin/server/econserver.exe host 50800

# terminal 2 — connect a client to it
./build/bin/game/econspace.exe connect 127.0.0.1 50800
```

Single-player needs no server — it runs the same `Simulation` in-process over a loopback transport.

---

## Controls

| Key | Action | | Key | Action |
|-----|--------|-|-----|--------|
| `W` / `S` | Thrust / brake | | `E` | Dock |
| `A` / `D` | Turn | | `X` | Stabilizer |
| Mouse wheel | Zoom | | `M` | Mine |
| Left click | Select object | | `F` | Fire weapon |
| Right click | Context menu / autopilot to point | | | |
| `T` | Target window | | `F5` / `F9` | Save / load |
| `O` | Overview window | | `F1` | Debug: +money |
| `R` | Radar window | | `F2` | Pause |
| `G` | Galaxy star map | | `F11` | Fullscreen |

---

## Repository layout

```
data/                 the game world as JSON (systems, galaxy index) — not hard-coded
src/
  engine/             shared core (static lib): world, entities, factions, UI, render
  game/               the game client + the authoritative Simulation + headless server
  editor/             the visual world editor
tests/                doctest unit tests (protocol / TCP round-trips)
documents/            design docs (concept, world format, factions/AI, living galaxy, assets)
```

The code is split into three modules — **engine** (shared static library), **game**, and **editor**. `game` and `editor` link `engine`; `engine` never depends on them.

---

## Contributing

EconSpace is open source under the MIT license and contributions are welcome — code, world content (via the editor), art to replace the placeholder shapes, docs, and bug reports. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and [CONVENTIONS.md](CONVENTIONS.md) first, and see the [issue tracker](../../issues) for good places to start.

## License

[MIT](LICENSE) © 2026 HEL3AN and EconSpace contributors.

Third-party dependencies keep their own licenses: raylib (zlib/libpng) and nlohmann/json (MIT).
