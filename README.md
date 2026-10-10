# EconSpace

**A 2D space MMO in the spirit of EVE, built in C++ on top of [raylib](https://www.raylib.com/) — where an AI agent is an ordinary player, and the world is generated rather than drawn.**

You fly a ship in a persistent galaxy: mine, trade, run missions, fight, build reputation with factions — while every system keeps simulating around you. The world lives on an authoritative server; the client renders what the server sends and sends back what you do. There is no single-player mode: playing means running a server, or connecting to one.

[![Build](https://github.com/HEL3AN/econspace-mmo/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/HEL3AN/econspace-mmo/actions/workflows/build.yml)
[![CodeQL](https://github.com/HEL3AN/econspace-mmo/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/HEL3AN/econspace-mmo/actions/workflows/codeql.yml)
![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platforms: Windows | Linux | macOS](https://img.shields.io/badge/platforms-Windows%20(MinGW)%20%7C%20Linux%20(GCC)%20%7C%20macOS%20(Clang)-lightgrey.svg)
![Status: Prototype](https://img.shields.io/badge/status-prototype-orange.svg)

> **Status — the honest version.** EconSpace is a working **prototype**, not a finished game. The client–server core, the netcode, multiplayer, accounts and the agent interface are real and tested. The look is new and moving fast. Content is thin and there is no audio. The direction is set — see [Where it is going](#where-it-is-going) — and contributions are welcome, from people and from agents.

---

## Start here

The repository has several documents at the top. Each owns one thing:

| Read | When you want |
|---|---|
| **this README** | what EconSpace is, what works today, how to build and play |
| [ARCHITECTURE.md](ARCHITECTURE.md) | how the code fits together |
| [CONTRIBUTING.md](CONTRIBUTING.md) | how to work here — build on each platform, tests, style, pull requests |
| [docs/agents/](docs/agents/) | **how to build your own bot** on the game's MCP server |
| [docs/howto/](docs/howto/) | **how to add a new kind of object** — an archetype, its shape, a material, a screen pass — worked by example |
| [documents/world_format.md](documents/world_format.md) | the contract for everything in `data/` — archetypes, shapes, materials, the screen treatment |
| [ROADMAP.md](ROADMAP.md) | where it is going, milestone by milestone |
| [DECISIONS.md](DECISIONS.md) | *why* things are the way they are — append-only, newest at the bottom |
| [CLAUDE.md](CLAUDE.md), [HANDOFF.md](HANDOFF.md) | operating notes for AI agents working *in* this repository |

---

## What works today

**Playing**
- Newtonian flight with a stabilizer, autopilot, server-authoritative **warp**, and standing behaviours: **orbit** and **keep at range** a chosen object.
- Mining, a station market with price impact and recovery, ship refits.
- Combat shared by players and NPCs, missions (bounty, mining, delivery), factions, reputation tiers, wanted levels and bounties, and role-based NPC AI — traders, miners, police, pirates, warships.
- A multi-system galaxy joined by jump gates, with a full-screen star map.

**Playing together**
- Many players on one server. Each connection has its own ship, account and missions; players see one another in a system; progress survives a reconnect.
- Accounts are named and have a secret. The first login sets it, and the secret never crosses the wire again: the server sends a challenge and the client answers it.
- One account, one session — logging in elsewhere displaces the earlier connection, saving it first.

**The world simulates itself**
- An authoritative simulation on a fixed `1/60` tick, decoupled from rendering, running headless in `econserver`.
- A living galaxy: system controllers, a gate-line economy, territory captures, an event feed — all of it persisted.

**The look** — generated, not drawn
- Every object is a **composition described in data**: a trade hub is a core, three arms with pads, a docking ring, lamps and a mast — and none of that is in C++.
- Lit by the system's **own stars** (two stars light a system twice), shaded by **materials** per part, and moving: rings turn, lamps blink out of step, engines burn, moons pass behind their planets, planets turn on their axes.
- A tunable **screen treatment** over the top — bloom, pixels, scanlines, grain, fringe, vignette — which you can reorder, retune or switch off in game (**F10**).

**Agents are players**
- `econagent` is an **MCP server written in C++** that is also an ordinary game client. A language model observes the world as text, gives standing orders, sleeps on an event journal and acts on what happened — by exactly the rules a human plays by. See [docs/agents/](docs/agents/).

**Tools**
- `worldeditor` — a visual editor for systems and galaxy links, and a **gallery** that shows every kind of object at once, lit and treated exactly as the game draws them, with their look editable live.
- `econserver` — the same simulation, headless, with smoke tests built in.

---

## Build

**Requirements**
- A C++17 compiler: **MinGW-w64 g++** on Windows (from [MSYS2](https://www.msys2.org/)), **GCC** on Linux, **Apple Clang** (Xcode command-line tools) on macOS.
- CMake 3.16 or later.
- On Linux, raylib's build dependencies: `sudo apt install libasound2-dev libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev`.
- Internet on the first build: raylib 5.5, nlohmann/json 3.11.3, doctest and PicoSHA2 are fetched automatically.

```sh
cmake -S . -B build -G "MinGW Makefiles"     # on Linux: cmake -S . -B build
cmake --build build                          # every target; the first build is slow
ctest --test-dir build --output-on-failure   # the unit suite
```

On Windows, close a running executable before rebuilding — Windows will not let it be overwritten. On Linux the binaries have no `.exe` suffix.

**macOS** builds and passes the tests in CI on every change, but nobody on the team plays on a Mac, so what the game *looks* like there has not been seen. Use the default generator (`cmake -S . -B build`, no `-G`). If you run it, a screenshot in an issue is the most useful thing you can send — Retina displays in particular are expected to misplace the screen treatment until #179.

## Play

Two halves, both required: a server, and a client connected to it.

```sh
# terminal 1 — an authoritative server
./build/bin/server/econserver.exe host 50800

# terminal 2 — a client, under an account name and its secret (the first login sets the secret)
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2
```

Run more clients under other names and they share the galaxy. To play over a network, start the server with `--public` (`econserver host 50800 --public`) and connect to its address instead of `127.0.0.1`. Without it the server accepts only this machine. That keeps Windows from raising a firewall prompt for every build, and nothing travels encrypted except the login.

**An agent instead of a human:**

```sh
./build/bin/agent/econagent.exe connect 127.0.0.1 50800 agent its-secret
```

That process speaks MCP on stdio. To hand it to Claude Code: `claude mcp add econspace -- <path>/econagent.exe connect 127.0.0.1 50800 agent its-secret`. The full guide is [docs/agents/](docs/agents/).

**Everything else**

```sh
./build/bin/editor/worldeditor.exe                 # the world editor
./build/bin/editor/worldeditor.exe gallery shapes  # every kind of object at once (F2 backend, F10 treatment)
./build/bin/editor/worldeditor.exe gallery shapes card station.trade_hub zoom 3  # one archetype, large

./build/bin/server/econserver.exe hosttest         # smoke tests: the server loop,
./build/bin/server/econserver.exe accttest         #   account persistence,
./build/bin/server/econserver.exe worldtest        #   galaxy persistence and the clock,
./build/bin/server/econserver.exe ordertest        #   standing orders, routes and the event journal
```

---

## Controls

| | Flying | | | Windows and view |
|---|---|-|---|---|
| `W` / `S` | thrust / brake | | `O` | overview — every object, by distance |
| `A` / `D` | turn | | `T` | target |
| `X` | stabilizer on / off | | `R` | radar |
| `E` | dock, when beside a station | | `J` | missions |
| `M` | mining on / off | | `G` | galaxy map |
| `F` | weapon on / off | | wheel | zoom, from a hull to a whole system |
| left click | select | | middle-drag | look away from the ship |
| right click | actions on an object — approach, **orbit**, **keep at range**, warp, dock, jump, mine | | `C` | camera back to the ship |
| | | | `F2` | shapes ⇄ glyphs |
| | | | `F10` | screen treatment settings |
| `F1` | debug: add money | | `F11` | fullscreen |

The right-click menu works on a row of the overview too. That is the intended way to fly: choose a thing from the list and choose what to do about it, rather than hunting for it on screen.

---

## Where it is going

Planned, not built. Details and order are in [ROADMAP.md](ROADMAP.md); the reasoning is in [DECISIONS.md](DECISIONS.md).

- **A system forty times bigger** (M9). Systems become a million units across, a station towers over a ship, and travel is chosen from a list — EVE's overview — rather than pointed at.
- **A generated region** (M7). The game begins when a wormhole opens into an unexplored region. The world beyond is generated from a seed; nothing has a name until somebody goes there and names it; what the region becomes is the record of what players did to it.
- **A builder** (M8). Players design their own *types* of structure from the same parts the world is built from — and a design's parts decide what it can do.
- **Open to others** (M10). Documentation and examples so that a developer or an agent can land here and build on it quickly.

---

## Repository layout

```
data/              the world as data: archetypes, systems, galaxy, factions, materials, shaders
src/
  engine/          shared core (static library): world, entities, factions, render, UI
  game/            the client, the authoritative Simulation and the server
    sim/           Simulation, the wire protocol, the shared player step
    net/           the TCP transport
  agent/           econagent, the MCP server
  editor/          the world editor and the gallery
tests/             the doctest suite
docs/agents/       building your own bot
docs/howto/        adding a new kind of object, by example
documents/         the data format, and design notes (older ones are marked historical)
```

CMake targets: **`engine`** (static library), **`netproto`** (the wire protocol and transport, compiled once), the client **`econspace`**, the server **`econserver`**, the MCP server **`econagent`**, **`worldeditor`**, and **`tests`**. `engine` depends on none of the others.

---

## Contributing

Contributions are welcome — code, world content, documentation, bug reports, and bots. Start with [CONTRIBUTING.md](CONTRIBUTING.md). Issues labelled [`good first issue`](../../issues?q=is%3Aopen+label%3A%22good+first+issue%22) and [`community`](../../issues?q=is%3Aopen+label%3Acommunity) are good places to begin.

Found a security problem? Please report it privately — see [SECURITY.md](SECURITY.md).

Licensed under the [MIT license](LICENSE).
