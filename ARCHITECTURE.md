# Architecture

EconSpace is built around one idea: **the server is the single source of truth, and the client only renders snapshots and sends commands.** The same simulation powers both single-player (server in-process) and networked play (remote server), so there is one game, not two.

## Modules

The code is three CMake targets:

- **`engine`** — a static library with the shared core: world/entity model, factions, resources, textures (with shape fallback), and reusable UI widgets. It never depends on `game` or `editor`.
- **`game`** — the executable `econspace`. Contains the game *client* (rendering, input, UI, camera) **and** the authoritative `Simulation` core **and** the headless server `econserver`.
- **`editor`** — the executable `worldeditor`, a visual editor for systems and galaxy links. Links `engine`.

```
src/
  engine/   core/ entities/ economy/ render/ ui/      (shared static lib)
  game/     core/ (client)  sim/ (Simulation + protocol + server)
            entities/ player/ economy/ missions/ net/ (transport)
  editor/   the world editor
```

## The client–server seam

```
                COMMANDS  (player input / actions)
  [ Client ] ───────────────────────────────▶ [ Server: Simulation ]
   render,                                       world, agents, combat,
   input,    ◀───────────────────────────────   spawn director, macro,
   UI              SNAPSHOTS  (view of a system)  player as an agent, account
```

- The **server** (`Simulation`, `src/game/sim/`) owns the world, all agents, combat, the spawn director, macro-dynamics, the player ship, the player account, and missions. It advances on a fixed `SIM_DT = 1/60` tick, independent of the render frame rate.
- The **client** (`Game`, `src/game/core/`) maps input to a `Command`, renders from a `Snapshot`, and never mutates authoritative state directly. In networked mode it doesn't even read a live world — it builds render proxies from a `SystemLayout` plus per-tick snapshots.
- **Single-player** is the same seam over an in-process loopback transport; **networked** play is the same seam over TCP. Only the transport differs.

## Protocol & transport

- **Protocol** (`src/game/sim/Protocol.*`): `Command` (client→server intents), `Snapshot` (server→client view of the player's system, including the player, entities, fire events, market, mission views, and account mirror), `SystemLayout` (static system geometry), and `GalaxyState` (periodic galaxy-wide stats for the map). Messages are versioned JSON (nlohmann/json).
- **Transport** (`src/game/net/`): the `ITransport` interface (`Send` / `Poll`) hides the wire. `LocalTransport` is an in-process loopback (single-player/tests); `TcpTransport` is winsock TCP with length-prefixed framing. Swapping TCP for UDP/ENet later means a new `ITransport`, not rewritten game logic.

## Netcode

Movement follows the standard [Gambetta](https://www.gabrielgambetta.com/client-server-game-architecture.html) model:

- **Your own ship** — client-side prediction + server reconciliation with **input replay**. Each input is numbered (`Command.seq`), the server acks the last processed input (`PlayerView.lastInput`), and the client replays unacknowledged inputs on top of the authoritative state. Critically, the server steps the player **one input = one tick** so the step counts match and the ship doesn't jitter.
- **Everyone else** — entity interpolation: the client buffers timestamped snapshots and renders non-self entities "in the past" (`render time = now − 100 ms`), interpolating position and taking the shortest arc for heading.
- Warp/autopilot state is server-authoritative and carried in the snapshot, so the client restores it *before* replay (otherwise two independent timers fight each other).

## Authority (what lives on the server)

Everything that affects game state is applied inside `Simulation` step methods and mirrored to the client through the snapshot:

- **Player physics / combat / mining** — `StepPlayerShip` / `StepPlayerFire` / `StepPlayerMining`.
- **Docking & trading** — `StepPlayerDock` / `StepPlayerUndock` / `StepPlayerSell` / `RefitPlayer` (server-authoritative, including reputation-gated docking).
- **Account** — money, skills, reputation, and wanted levels live in `Simulation::account_`; effects are applied server-side and the client account is a read-only mirror of the snapshot. Persisted to `account.json`.
- **Missions** — the job board, acceptance, progress, and turn-in live in `Simulation::missions_`; missions address stations by stable id so they survive jumps.
- **World** — persisted to `world.json`.

## The living galaxy

Beyond the player's system, the server simulates the whole galaxy at a lower level of detail: per-system aggregates, controllers, a gate-line economy, territory captures/reclaims, and an event feed. This is summarized into a `GalaxyState` message (sent roughly once a second) that drives the client's galaxy map.

## Data-driven world

The world is data, not code: `data/universe.json` indexes systems and links; `data/systems/*.json` describe each system's objects. Factions and their relations live in `data/factions.json`. The format is documented in [documents/world_format.md](documents/world_format.md), and the world editor writes exactly this format.
