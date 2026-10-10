# itch.io page — DRAFT

> **DRAFT for the owner.** Nothing here has been published. Every claim below is meant to
> match what the repository does today (checked against README.md, ROADMAP.md and the code
> on 2026-10-10); anything marked *(owner)* is a decision this draft does not make.

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
begins where the map ends — a wormhole at the edge of the home system opens into a region
generated from a seed, where nothing has a name until somebody gets there first and names
it. Everything you see is generated as well: stations, planets and wrecks are compositions
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
- **A region nobody has mapped.** Beyond the wormhole, systems are generated from the
  world's seed — stars, planets, belts, wrecks and rare finds — and stay uncharted until
  somebody visits. The first visitor names them, once, for everyone.
- **Mining, trading, missions and combat.** A station market with price impact and
  recovery, ship refits, bounty, mining and delivery missions, reputation with factions,
  wanted levels and bounties.
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
