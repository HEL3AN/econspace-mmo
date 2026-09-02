# EconSpace

**A 2D EVE-like space MMO with an authoritative client–server core, built in C++ on top of [raylib](https://www.raylib.com/).**

You pilot a ship in a persistent, multi-system galaxy: mine, trade, run missions, fight, and build reputation with factions — while the galaxy simulates itself around you. The world lives on an authoritative server; the client renders snapshots and sends commands. There is no single-player mode — playing means running (or connecting to) a server.

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform: Windows](https://img.shields.io/badge/platform-Windows%20(MinGW)-lightgrey.svg)
![Status: Prototype](https://img.shields.io/badge/status-prototype-orange.svg)

> **Project status — honest version.** EconSpace is an engineering-driven **prototype**, not a finished game. The client–server architecture and netcode are solid and real; the *content* is not: there is no audio, and the world is small. The look is **glyphs** — that is the game's visual language, not a stand-in for missing art. The server currently accepts **one** client at a time — multi-client is the next foundational piece, not an extra. See [ROADMAP.md](ROADMAP.md) for where it is and where it's going. Contributions are very welcome.

---

## What's in it

EconSpace runs its own game logic on top of raylib (windowing/render/input only). The authoritative `Simulation` runs headless in `econserver`; the client is a renderer and an input source. There is one mode — connected play — and everything below already works today.

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

**Presentation**
- **Glyphs are the look.** Every object is a character: the glyph says what class of thing it is, the colour says whose it is, the size is its actual size. Areas — nebulae, asteroid belts — are drawn as regions rather than one huge character, and ships turn to face where they are going.
- Object types live in `data/archetypes.json`, so a new kind of object needs no new art and no new drawing code.
- Rendering goes through one seam with pluggable backends: glyphs, shapes/sprites (F2), and a headless text projection that needs no window — which is how an AI agent and the test suite see the same world.

**Agents**
- `econagent`, an **MCP server written in C++**, so the wire protocol has a single source of truth. An LLM agent observes the world as text, gives a standing order, waits on an event journal, and acts on the result.

**Tooling**
- A visual **world editor** (`worldeditor`) for editing systems and galaxy links, saved to JSON. It draws through the same presentation seam as the game, and its creation palette is generated from the archetype registry.
- A **headless server** (`econserver`) that runs the exact same simulation without a window.

**Networking**
- A `Command` / `Snapshot` / `SystemLayout` protocol over a swappable transport (`ITransport`): TCP (winsock) for play, plus an in-process `LocalTransport` used as a **test** seam by the `econserver hosttest` smoke test and the doctest suite.
- Client-side prediction + server reconciliation with input replay for the player's own ship, and entity interpolation for everyone else (the classic [Gambetta](https://www.gabrielgambetta.com/client-server-game-architecture.html) model).
- Server-authoritative combat, docking, trading, missions, and player accounts.

See [ARCHITECTURE.md](ARCHITECTURE.md) for how it all fits together.

---

## Where it's going

Everything in this section is **planned, not implemented**. It is here so contributors know what the project is aiming at, and so nobody builds against the old assumptions. Details and sequencing live in [ROADMAP.md](ROADMAP.md).

- **A player-mutable world** (#44). Players build deployables and structures that feed the macro simulation that already exists — prosperity, security, territory control. The archetype registry and the glyph layer were built for this: a structure a player invents needs no artist and no recompile.
- **Fleets** (#32). One commander, several agent-piloted ships.

The world is still read-only content authored in the editor. Several people can share a galaxy: each connection has its own ship and account, they see one another in a system, and progress survives a reconnect. An account is named and has a secret; the first login sets it, and the secret itself never crosses the wire after that (#106). There is no transport encryption, so everything else does. Nobody can shoot anybody — whether they should be able to, where, and at what cost is #94.

---

## Build & run

**Requirements**
- A C++17 compiler — MinGW-w64 g++ (from [MSYS2](https://www.msys2.org/)).
- CMake 3.16+.
- Internet on the first build: [raylib](https://github.com/raysan5/raylib) 5.5 and [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 are fetched and built automatically via CMake `FetchContent`.

```sh
cmake -S . -B build -G "MinGW Makefiles"   # configure (first build downloads & builds deps — slow)
cmake --build build                        # build game + editor + server + tests

ctest --test-dir build --output-on-failure # run the unit tests (doctest)
```

> On Windows, close the running game/editor window before rebuilding — Windows won't let you overwrite a running `.exe`. The `data/` folder is copied next to each executable on every build.

**Play** — start a server, then connect a client to it. Both halves are required; the client is not a game on its own.

```sh
# terminal 1 — start an authoritative server on a TCP port
./build/bin/server/econserver.exe host 50800

# terminal 2 — connect a client to it
./build/bin/game/econspace.exe connect 127.0.0.1 50800
```

Several clients can connect at once; each names the account it plays under (`connect <host> <port> <name>`, default `pilot`) and the server keeps their progress apart. They do not see each other in space yet — that is #4. Run it on `127.0.0.1` for solo play, or on a reachable host to play over a network.

**Other executables**

```sh
./build/bin/editor/worldeditor.exe         # visual world editor (systems, objects, galaxy links)
./build/bin/server/econserver.exe          # batch headless simulation (no client, prints galaxy stats)
./build/bin/server/econserver.exe hosttest # server-loop smoke test over the in-process transport
```

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

EconSpace is open source under the MIT license and contributions are welcome — code, world content (via the editor), docs, and bug reports. [ROADMAP.md](ROADMAP.md) lists what help is most useful right now. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and [CONVENTIONS.md](CONVENTIONS.md) first, and see the [issue tracker](../../issues) for good places to start.

## License

[MIT](LICENSE) © 2026 HEL3AN and EconSpace contributors.

Third-party dependencies keep their own licenses: raylib (zlib/libpng) and nlohmann/json (MIT).
