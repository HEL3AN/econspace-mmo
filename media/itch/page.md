# itch.io page — DRAFT

**Published:** https://hel3an.itch.io/econspace. This file is the draft it was made from; the live page is the source of truth.

> **The draft the live page was made from**, kept for reference. Its claims were checked
> against the code on 2026-10-10 and the features list was brought up to date on 2026-10-23;
> anything marked *(owner)* is a decision this draft does not make.

## Assets in this folder

| File | Use on itch.io |
|---|---|
| `cover-630x500.png` | Cover image (630×500) |
| `banner-960x200.png` | Page banner / header image (960 wide) |
| `screenshot-01-trade-hub.png` … `screenshot-08-shipyard.png` | Screenshots, 1280×720, in the suggested order |

The pictures are taken by the game and the editor themselves and skip the screen treatment
(bloom, scanlines, grain), so they are plainer than the game looks with it on.

## Title

**EconSpace**

Short tagline: *A 2D space MMO in the spirit of EVE, where an AI agent is an ordinary player
and the world is generated rather than drawn.*

## Pitch

EconSpace is a 2D space MMO in the spirit of EVE Online. You fly a ship in a persistent
galaxy that keeps simulating whether anyone is watching: factions trade, patrol, raid and
take territory on their own, and a market moves with what is bought and sold. The game
begins where the map ends — every server generates its own world from a seed, and as it
grows, wormholes open into regions where nothing has a name until somebody gets there first
and names it. Everything you see is generated as well: stations, planets and wrecks are compositions
described in data, lit by the stars of the system they are in. And you may not be the only
kind of player: the game ships its own MCP server, so a language model can log in and play
by exactly the rules you do.

## Features

- **An authoritative server and a persistent galaxy.** Many players share one server; each
  has an account, a ship and missions, and progress survives a reconnect.
- **A living galaxy.** Traders, miners, police, pirates and warships act on their own;
  factions move where the prize is worth the risk, and systems change hands when the balance
  holds. An event feed reports it.
- **Fly by choosing, not by pointing.** Systems are a million units across. The overview
  lists everything by distance; right-click a row to approach, orbit, keep at range, warp,
  dock or jump. Warp is server-authoritative and bends around stars and planets.
- **A world that grows.** Each server generates a whole world from its seed: civilisation
  centres to start in, and a frontier that opens region by region. New regions have native
  factions, stations and markets from the first minute; a wormhole mouth stays only if
  someone builds an Anchor on it. Every pilot has their own map, and the first to reach a
  system names it.
- **Mining, trading, missions and combat.** Markets whose prices follow their stock, ships
  you design and refit, jobs posted by faction plans, ship loss and insurance, one combat
  model for players and NPCs, and PvP that is a crime under the law.
- **Corporations and territory.** Members and roles, chat, outposts that keep goods, systems
  held by influence and a claim, police and patrols paid from the treasury.
- **AI agents are players.** `econagent` is an MCP server written in C++ that is also an
  ordinary game client: an agent observes the world as text, gives standing orders, waits
  on an event journal and acts — with no rules of its own.
- **A look generated from data.** Every object is a composition of a few primitives in
  JSON, shaded per part by materials and lit by the system's own stars. Rings turn, lamps
  blink, moons pass behind their planets. A tunable screen treatment (bloom, pixels,
  scanlines, grain, fringe, vignette) can be reordered or switched off in game.
- **A module library.** Seven packs, 101 modules and 314 variants — hatches, turrets, docks,
  domes, craters, cities — placed by name and varied by seed.
- **Tools included.** A world editor with a gallery of every archetype and a survey of
  fifty generated systems at a time.
- **Open source.** C++17 on raylib, MIT licensed; built and tested in CI on Windows (MinGW),
  Linux (GCC) and macOS (Clang).

## Honest status

EconSpace is a working **prototype**, not a finished game. The client–server core,
netcode, multiplayer, accounts and the agent interface are real and tested. Content is thin,
there is no audio, and the look is changing quickly. Playing means running a server (it is
one command) or connecting to someone else's.

## Suggested metadata *(owner)*

- Classification: Game · Kind: Downloadable *(owner: decide whether to upload builds or
  link to the source only)*
- Release status: Prototype / In development
- Genre: Simulation; tags such as *space*, *mmo*, *sandbox*, *procedural-generation*,
  *economy*, *open-source*, *ai*
- Platforms: Windows, Linux *(macOS builds in CI, but has not been played on — see README)*
- Links: the GitHub repository; docs/agents/ for building a bot
- Price: *(owner)*
