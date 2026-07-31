# EconSpace — Concept Document

> Status: working draft (v0.4). This is a living document — updated as the project evolves.
> v0.4 — genre change: from a management strategy game to a space simulator (EVE-like).

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

The game is inspired by EVE Online, but it is a **single-player, offline game** built
on its own logic on top of raylib.

The "character" in the game is the pilot's persona: their skills, reputation with
factions, and wallet. Physically the player is always inside a ship; ships can be
swapped at stations.

## 2. Genre and Audience

- **Genre:** space simulator-sandbox (single-player, real-time).
- **Audience:** fans of space sims and economic sandboxes (kindred spirits:
  EVE Online, Elite, Endless Sky).
- **Platform:** PC (Windows first and foremost).

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

Control is **hybrid**: the ship can be flown manually (thrust, turning, inertia) or
handed over to the autopilot ("fly to", "approach", "dock"). Manual input cancels the
autopilot.

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
- **Combat.** Weapons, shields and hull, hostile NPCs. Present, but not the center of
  gameplay.

## 5. World

At the start — **a single star system**: a star, planets (backdrop/landmarks),
stations, asteroid belts. The world is populated gradually. Several systems connected
by jump gates are a possible future expansion.

## 6. Graphics

2D, pixel-art styling, with a primarily black-and-white palette. Color is used
sparingly: to highlight objects, for effects (glow, engine exhaust), and to
distinguish types (factions, resource kinds, link types).

## 7. Technology

- C++17, MinGW compiler (g++ from MSYS2), CMake build.
- raylib 5.5 — window, rendering, input, sound.
- nlohmann/json 3.11.3 — world data and saves.

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

> Status: stages R1–R9 are implemented — the EVE-like MVP is closed. Next comes polish
> and content.

Later: ownership of stations and mining assets, hiring NPCs, a developed mission
system, modular ship fitting, several star systems.

## 9. Project History

The project began as a management strategy game (player = a company, managed by
clicking on planets from a top-down view). The first MVP — stages M0–M7 — was brought
to a playable state: economy, market, an AI rival company, saves. That version is
preserved in the git commit history.

In version v0.4 the concept was rethought: the game became a space simulator with ship
control. The technical foundation was reused; the gameplay mechanics are being designed
from scratch along the R1–R9 roadmap.
