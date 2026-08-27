# Architecture

EconSpace is built around one idea: **the server is the single source of truth, and the client only renders snapshots and sends commands.** There is exactly one mode — a client connected to an authoritative server — so there is one game, not two. Everything below describes what exists today; [planned directions](#planned-directions-not-built-yet) are marked as such at the end.

## Modules

The code is three CMake targets:

- **`engine`** — a static library with the shared core: world/entity model, factions, resources, textures (with shape fallback), and reusable UI widgets. It never depends on `game` or `editor`.
- **`game`** — two executables built from one source tree: the *client* `econspace` (rendering, input, UI, camera) and the authoritative server `econserver` (the `Simulation` core). The client does **not** link `Simulation`: it shares only the wire protocol and `sim/PlayerStep`, the movement step it must run identically to the server in order to predict its own ship.
- **`editor`** — the executable `worldeditor`, a visual editor for systems and galaxy links. Links `engine`.

```
src/
  engine/   core/ entities/ economy/ render/ ui/      (shared static lib)
  game/     core/ (client)  sim/ (Simulation + protocol + PlayerStep + server)
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

  `Simulation` is one class spread over several translation units, named after what each decides (#17). `Simulation.cpp` holds only its spine — lifetime, the world RNG, the event journal — and the rules live in `Simulation_World` (the galaxy, hydration, routes, persistence), `Simulation_Agents` (NPC behaviour, combat, the spawn director, the macro step), `Simulation_Player` (the server-side player verbs, the account, missions), `Simulation_Orders` (the standing-order executor) and `Simulation_Snapshot` (authoritative state translated into wire messages). `Simulation.h` remains the single declaration of the class's surface. `Editor` is split the same way: `Editor_Palette`, `Editor_Panel`, `Editor_Galaxy` and `Editor_Universe`.
- The **client** (`Game`, `src/game/core/`) maps input to a `Command`, renders from a `Snapshot`, and never mutates authoritative state directly. It does not read a live world at all — it builds render proxies from a `SystemLayout` plus per-tick snapshots. The only game state it owns is its *prediction* of its own ship.
- **Play is always over the seam**: client to authoritative server over TCP. Because the seam is a transport interface rather than a socket call, tests can drive the exact same server loop in one process without a network — that is the only other use of the seam, not a second game mode.

## Protocol & transport

- **Protocol** (`src/game/sim/Protocol.*`): `Command` (client→server intents), `Snapshot` (server→client view of the player's system, including the player, entities, fire events, market, mission views, and account mirror), `SystemLayout` (static system geometry), and `GalaxyState` (periodic galaxy-wide stats for the map). A client opens with `Hello`, which names the account it plays under — until that arrives the server has a socket and no player behind it (#3). Messages are JSON (nlohmann/json), tagged with a type field (`"t"`) and stamped with `PROTO_VERSION`. Every decoder refuses a message from another version, because decoding is permissive per field (`value(key, default)`) and without the check a peer built against an older protocol would silently read defaults instead of failing — bump `PROTO_VERSION` whenever a message changes meaning.
- **Transport** (`src/game/net/`): the `ITransport` interface (`Send` / `Poll`) hides the wire. `LocalTransport` is an in-process loopback used **for testing** — the `econserver hosttest` server-loop smoke test and the doctest suite; `TcpTransport` is winsock TCP with length-prefixed framing. Swapping TCP for UDP/ENet later means a new `ITransport`, not rewritten game logic.

### One movement step, two callers

`Sim::StepPlayerShip` (`src/game/sim/PlayerStep.h`) is the player ship's physical step, and it exists as a single function precisely because **both** sides run it: the server applies it authoritatively, and the client applies the identical step to predict its own ship and to replay unacknowledged inputs. Prediction only works while the two compute the same result from the same input, so there must not be two implementations kept in step by hand. `PLAYER_WEAPON_RANGE` lives beside it for the same reason — the server decides whether a shot connects, the client draws the targeting circle.

If you change how the player's ship moves, you are changing both sides at once. That is the point.

## Netcode

Movement follows the standard [Gambetta](https://www.gabrielgambetta.com/client-server-game-architecture.html) model:

- **Your own ship** — client-side prediction + server reconciliation with **input replay**. Each input is numbered (`Command.seq`), the server acks the last processed input (`PlayerView.lastInput`), and the client replays unacknowledged inputs on top of the authoritative state. Critically, the server steps the player **one input = one tick** so the step counts match and the ship doesn't jitter.
- **Everyone else** — entity interpolation: the client buffers timestamped snapshots and renders non-self entities "in the past" (`render time = now − 100 ms`), interpolating position and taking the shortest arc for heading.
- Warp/autopilot state is server-authoritative and carried in the snapshot, so the client restores it *before* replay (otherwise two independent timers fight each other).

## Authority (what lives on the server)

Everything that affects game state is applied inside `Simulation` step methods and mirrored to the client through the snapshot:

- **Player physics / combat / mining** — `StepPlayerShip` / `StepPlayerFire` / `StepPlayerMining`.
- **Docking & trading** — `StepPlayerDock` / `StepPlayerUndock` / `StepPlayerSell` / `RefitPlayer` (server-authoritative, including reputation-gated docking).
- **Account** — money, skills, reputation, and wanted levels live in `ClientSession::account`, one per connected player; effects are applied server-side and the client account is a read-only mirror of the snapshot. Persisted per name to `account_<name>.json`, together with where the player was — system, position, heading, cargo and active missions (#49) — so a reconnect resumes rather than restarts. Which ships an account owns is still missing (#5), and the file carries no schema version yet (#20).
- **Missions** — the job board, acceptance, progress, and turn-in live in `Simulation::missions_`; missions address stations by stable id so they survive jumps.
- **World** — persisted to `world.json`.

## The living galaxy

Beyond the player's system, the server simulates the whole galaxy at a lower level of detail: per-system aggregates, controllers, a gate-line economy, territory captures/reclaims, and an event feed. This is summarized into a `GalaxyState` message (sent roughly once a second) that drives the client's galaxy map.

## Data-driven world

The world is data, not code: `data/universe.json` indexes systems and links; `data/systems/*.json` describe each system's objects. Factions and their relations live in `data/factions.json`. The format is documented in [documents/world_format.md](documents/world_format.md), and the world editor writes exactly this format.

## Planned directions (not built yet)

None of the following exists in the codebase. It is recorded here so new work lands in the right shape and so nobody has to reverse-engineer the intent from issue threads. Sequencing lives in [ROADMAP.md](ROADMAP.md).

**Standing orders — a strategic layer above the tactical tick.** The server today only understands per-tick `Command` input, which suits a human at 60 Hz and suits nothing else. The plan is a second layer on the server: durable, high-level orders ("mine this belt until the hold is full", "haul to that station", "defend this gate") that the server itself executes over seconds or minutes, reporting progress. The 60 Hz tactical loop stays exactly as it is; the order layer sits on top and issues into it. This is what makes an agent-driven or fleet-driven player viable — nobody, human or model, should have to stream thrust bits to play.

**The agent seam — `econagent` (#42).** An AI agent becomes an ordinary player, not a special case in the server. `econagent` is planned as a **separate process** that is two things at once: an MCP server on stdio for the model, and a normal TCP game client to `econserver` — the same `Command`/`Snapshot` protocol every other client speaks. It is written in C++ and links the existing protocol code so the wire format keeps a single source of truth instead of drifting into a second, hand-maintained implementation. The server gains no knowledge of MCP. Companion pieces: a compact text projection of world state (what the model actually reads), an event journal with a blocking wait so an agent can sleep until something happens rather than poll, and MCP resources/prompts on top.

**Presentation layer with pluggable backends (#36).** Rendering is currently entity types drawing themselves through raylib calls. The plan is to route drawing through a presentation layer that maps an entity's archetype to a visual, with the backend swappable: **glyph** (the primary look — ASCII/text, which is also close to what the agent projection needs), shapes (today's placeholders), and sprites as an optional backend for whoever wants to supply art. This also removes the artist as a hard dependency for player-built content, and gives the editor and the game one shared way to draw the same thing.

**World mutation and `LayoutDelta` (#44).** `SystemLayout` is sent **once**, when the client enters a system; everything that changes afterwards travels as per-tick `Snapshot` entries for entities the client already knows about. That is exactly why the world cannot change shape today: there is no message that says "a structure now exists here" or "this one is gone". The planned fix is authoritative world mutation on the server plus a `LayoutDelta` message (added/removed/changed layout entries) alongside the existing snapshot stream, with construction, ownership, permissions, limits, and upkeep built on top, feeding the macro-dynamics that already run.
