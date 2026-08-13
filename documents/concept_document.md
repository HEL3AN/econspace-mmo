# EconSpace — Concept Document

> Status: working draft (v0.5). This is a living document — updated as the project evolves.
> v0.4 — genre change: from a management strategy game to a space simulator (EVE-like).
> v0.5 — genre change: from a single-player offline game to an MMO (an authoritative
> server, a shared persistent world, no single-player mode); glyph/ASCII becomes the
> primary look; AI agents are first-class players.

---

## TABLE OF CONTENTS

1. Introduction
2. Genre and Audience
3. Gameplay Loop
4. Game Systems
5. World
6. Graphics
7. Technology
8. Roadmap (stages R1–R9)
9. Project History

---

## 1. Introduction

EconSpace is a 2D space simulator-sandbox. The player controls a **ship** within a
star system: mining resources, trading, running missions, and fighting when needed.
Progression happens along several tracks at once — ships and modules, character
skills, money, and reputation.

The game is inspired by EVE Online and is an **MMO**: the world lives on an
authoritative server, and the client renders the snapshots it receives and sends
commands back. There is **no single-player mode** — playing means running a server or
connecting to one. The game runs on its own logic on top of raylib.

Players are not assumed to be human. **AI agents are first-class players**: the intent
is to ship an MCP server with the game so that an agent can log in, fly a ship, trade
and fight through the same commands a human client sends. Nothing in the world model
distinguishes the two kinds of pilot.

The "character" in the game is the pilot's persona: their skills, reputation with
factions, and wallet. Physically the player is always inside a ship; ships can be
swapped at stations. The persona lives on the server as an account and survives
disconnects.

## 2. Genre and Audience

- **Genre:** space simulator-sandbox MMO (real-time, client–server).
- **Audience:** fans of space sims and economic sandboxes (kindred spirits:
  EVE Online, Elite, Endless Sky). Plus a second audience the genre does not usually
  have: people who want to point an AI agent at a persistent world and watch it play.
- **Platform:** PC (Windows first and foremost) for the client; the server is
  headless and runs without a window.

## 3. Gameplay Loop

There is no single "correct" activity — this is a sandbox. The basic early-game loop
(~10 minutes):

```
Undock from station  →  Fly to asteroid belt  →  Mine ore into cargo hold
        ↑                                                      ↓
  Buy a module/ship  ←  Sell ore on the market  ←  Dock at a station
```

Instead of mining, the player can: take a cargo-hauling contract, buy low and sell
high between stations, fend off a pirate, or go hunting themselves.

The loop runs inside a **shared world**. The belt being mined, the market being traded
on, and the pirate being shot at are the same ones every other pilot in that system
sees; prices move because someone moved them, and an asteroid another player stripped
is gone for everyone. The world keeps running while a player is logged off.

Control is **hybrid**: the ship can be flown manually (thrust, turning, inertia) or
handed over to the autopilot ("fly to", "approach", "dock"). Manual input cancels the
autopilot. Both paths end as commands sent to the server, which is the only authority
on what actually happened — the client predicts its own ship and corrects itself when
the server disagrees.

## 4. Game Systems

- **Flight.** A ship with physics: thrust, turning, inertia, top speed.
- **Navigation.** System objects (stations, asteroid belts, planets); autopilot
  commands.
- **Stations.** Hubs. Docking opens the station screen: market, repair, hangar,
  missions. Space and station are distinct game modes.
- **Mining.** Asteroid belts; a mining module fills the cargo hold; asteroids deplete.
- **Economy.** A market at each station with prices; buying and selling; the player's
  wallet. Later — ownership of stations and mining assets, hiring NPCs.
- **Ships and modules.** Several ship types; modules (engine, cargo hold, mining
  laser, weapons) are installed in the hangar.
- **Skills and reputation.** Character skills (piloting, mining, trade); reputation
  with factions unlocks opportunities.
- **NPCs.** AI ships (traders, miners, patrols, pirates) that live their own lives.
- **Combat.** Weapons, shields and hull, hostile NPCs and hostile players. Present,
  but not the center of gameplay.
- **Multiplayer.** An authoritative server owns the world; clients hold no truth of
  their own. Several pilots share a system, see each other, and compete for the same
  belts, contracts, and markets.
- **Agent access.** An MCP server shipped with the game exposes the same command set
  to AI agents, so an agent is a client like any other rather than a scripted NPC.
  Planned, not yet built.

## 5. World

The building block is a **star system**: a star, planets (backdrop/landmarks),
stations, asteroid belts. Systems are connected by jump gates into a galaxy, and the
world is populated gradually.

The world is **persistent and server-side**. It is not a level loaded per session: the
server simulates every system, including the ones nobody is currently flying in, and
holds the authoritative state between sessions. A player logging in rejoins a world
that moved on without them.

## 6. Graphics

2D, with a primarily black-and-white palette. Color is used sparingly: to highlight
objects, for effects (glow, engine exhaust), and to distinguish types (factions,
resource kinds, link types).

The **primary look is glyph/ASCII rendering**: the world is drawn as characters on a
grid rather than as pixel-art sprites. This is a deliberate design decision, not a
debug view or a stand-in until art arrives — it fits the black-and-white palette, it
reads well at the densities an MMO produces, and it keeps the game legible without
depending on an art pipeline the project does not have.

Sprites remain an **optional alternative rendering backend**. The texture path exists
in the engine and falls back to vector shapes when a PNG is missing; the specification
for producing those sprites is in `documents/texture_assets.md` and stays valid for
anyone who wants that backend. Art is no longer on the project's critical path.

## 7. Technology

- C++17, MinGW compiler (g++ from MSYS2), CMake build.
- raylib 5.5 — window, rendering, input, sound (client only).
- nlohmann/json 3.11.3 — world data and saves.
- TCP (winsock) for the client–server transport.

The code is split into three modules: **engine** (a shared static library — world,
entities, factions, UI, render), **game** (the client, the authoritative simulation,
and the headless server), and **editor** (the world editor). `game` and `editor` link
`engine`; `engine` never depends on them.

The simulation is authoritative and runs on a fixed tick, decoupled from rendering.
The same simulation code runs inside the headless server; the client is a renderer and
an input source over a command/snapshot protocol. Rendering is being restructured so
the glyph backend is the default and the sprite backend is one of the options behind
the same drawing calls.

The MCP server that lets AI agents play is intended to sit on the same protocol as
the human client, not beside it. It is not implemented yet.

The technical foundation (game loop, entity classes, rendering, camera, JSON loading,
save/load) was carried over from the previous version of the game.

## 8. Roadmap (stages R1–R9)

| Stage | Result |
|------|-----------|
| **R1. Ship flight** | Player pilots the ship (thrust/turn/inertia), the camera follows; the star and planets are backdrop |
| **R2. Navigation and autopilot** | System objects (stations, asteroid belts); "fly to / approach" commands |
| **R3. Stations and docking** | Docking → station screen; "flight" and "at station" modes |
| **R4. Mining and cargo hold** | Asteroid belts, mining module, cargo hold, asteroid depletion |
| **R5. Market and trade** | Station market: buying/selling, player's wallet |
| **R6. Ships and modules** | Hangar, several ship types, module installation |
| **R7. Skills and reputation** | Character skills, reputation with factions |
| **R8. NPC ships and factions** | AI ships fly around the system; factions |
| **R9. Combat** | Weapons, shields/hull, hostile NPCs (pirates) |

> Status: stages R1–R9 are implemented — the EVE-like MVP is closed. The work after
> them moved to the client–server track: the authoritative simulation, the headless
> server, and networked play.

Later: multiple clients on one server and interest management, server-side ownership
of ships and stations, the glyph renderer as the default presentation, the MCP server
for agent players, hiring NPCs, a developed mission system, and modular ship fitting.

## 9. Project History

The project began as a management strategy game (player = a company, managed by
clicking on planets from a top-down view). The first MVP — stages M0–M7 — was brought
to a playable state: economy, market, an AI rival company, saves. That version is
preserved in the git commit history.

In version v0.4 the concept was rethought: the game became a space simulator with ship
control. The technical foundation was reused; the gameplay mechanics are being designed
from scratch along the R1–R9 roadmap.

In version v0.5 the concept changed again: the game became an MMO. The single-player,
offline framing was dropped — the world belongs to an authoritative server, and the
client is a view onto it. The single-player code path that remains in the repository is
residue from the v0.4 stage and is scheduled for deletion (issue #23); it is not part
of the target design. Two further decisions were settled at the same time: glyph/ASCII
rendering becomes the game's primary look rather than a debug view (issue #36), with
sprites demoted to an optional backend; and AI agents are treated as first-class
players, to be served by an MCP server shipped with the game.
