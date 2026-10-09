# Architecture

EconSpace is built around one idea: **the server is the single source of truth, and the client only renders snapshots and sends commands.** There is exactly one mode — a client connected to an authoritative server — so there is one game, not two. Everything below describes what exists today; [planned directions](#planned-directions-not-built-yet) are marked as such at the end.

## Modules

The code is seven CMake targets:

- **`engine`** — a static library with the shared core: the world and entity model, archetypes, factions, resources, the presentation seam and everything that draws through it, and reusable UI widgets. **It depends on none of the others.**
- **`netproto`** — the wire protocol, the event journal, the login and the TCP transport, compiled once and linked by everything that speaks the wire.
- **`econspace`** — the client: rendering, input, UI, camera. It does **not** link `Simulation`; it shares only the protocol and `sim/PlayerStep`, the movement step it must run identically to the server in order to predict its own ship.
- **`econserver`** — the authoritative server, running the `Simulation` headless, with its smoke tests built in.
- **`econagent`** — the MCP server for AI agents, which is also an ordinary game client (`src/agent/`). See [docs/agents/](docs/agents/README.md).
- **`worldeditor`** — the world editor and the gallery.
- **`tests`** — the doctest suite, which links the simulation itself rather than testing it only through a socket.

```
src/
  engine/   core/ entities/ economy/ render/ ui/      (shared static library)
  game/     core/ (client)  sim/ (Simulation, protocol, PlayerStep, server)
            entities/ player/ economy/ missions/ net/ (transport)
  agent/    econagent, the MCP server
  editor/   the world editor and the gallery
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

  `Simulation` is one class spread over several translation units, named after what each decides (#17). `Simulation.cpp` holds only its spine — lifetime, the world RNG, the event journal — and the rules live in `Simulation_World` (the galaxy, hydration, routes, persistence), `Simulation_Agents` (NPC behaviour, combat, the spawn director, the macro step), `Simulation_Player` (the server-side player verbs, the account, missions), `Simulation_Orders` (the standing-order executor) and `Simulation_Snapshot` (authoritative state translated into wire messages). `Simulation.h` remains the single declaration of the class's surface. `Editor` is split the same way: `Editor_Palette`, `Editor_Panel`, `Editor_Galaxy`, `Editor_Universe` and `Editor_Gallery` (every archetype at once, for judging a look — #118).
- The **client** (`Game`, `src/game/core/`) maps input to a `Command`, renders from a `Snapshot`, and never mutates authoritative state directly. It does not read a live world at all — it builds render proxies from a `SystemLayout` plus per-tick snapshots. The only game state it owns is its *prediction* of its own ship.
- **Play is always over the seam**: client to authoritative server over TCP. Because the seam is a transport interface rather than a socket call, tests can drive the exact same server loop in one process without a network — that is the only other use of the seam, not a second game mode.

## Protocol & transport

- **Protocol** (`src/game/sim/Protocol.*`): `Command` (client→server intents), `Snapshot` (server→client view of the player's system, including the player, entities, fire events, market, mission views, and account mirror), `SystemLayout` (static system geometry), and `GalaxyState` (periodic galaxy-wide stats for the map). Other players in the same system arrive in the snapshot as entities of kind `PlayerShip` and are drawn through the same interpolation path as NPCs (#4); the recipient's own ship is not among them — it is in `PlayerView`, predicted locally. A snapshot carries only what changes. Objects that cannot move — stars, stations, gates, belts, nebulae, wrecks — are not in it at all, and neither are the static fields of the ones that are; both come from the `SystemLayout` sent on entry, and `Proto::CompleteFromLayout` puts the whole system back together on arrival so nothing downstream knows the difference (#16, #97). Positions are rounded to a hundredth of a unit, and snapshots go out at 20 Hz rather than once per server loop — the client renders 100 ms in the past and interpolates, which is what makes sparse updates work. A client opens with `Hello`, which names the account it plays under — until that arrives the server has a socket and no player behind it (#3). Messages are JSON (nlohmann/json), tagged with a type field (`"t"`) and stamped with `PROTO_VERSION`. Every decoder refuses a message from another version, because decoding is permissive per field (`value(key, default)`) and without the check a peer built against an older protocol would silently read defaults instead of failing — bump `PROTO_VERSION` whenever a message changes meaning.
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
- **Account** — money, skills, reputation, and wanted levels live in `ClientSession::account`, one per connected player; effects are applied server-side and the client account is a read-only mirror of the snapshot. Persisted per name to `account_<name>.json`, together with where the player was — system, position, heading, cargo and active missions (#49) — so a reconnect resumes rather than restarts. The file carries a schema version and a newer one is refused rather than read leniently (#20), and it records which ships the account owns and which one it is flying (#5) — the server decides, because the server is what charges for a ship.
- **Missions** — the job board, acceptance, progress, and turn-in live in `Simulation::missions_`; missions address stations by stable id so they survive jumps.
- **World** — persisted to `world.json`.

## The living galaxy

Beyond the player's system, the server simulates the whole galaxy at a lower level of detail: per-system aggregates, controllers, a gate-line economy, territory captures/reclaims, and an event feed. This is summarized into a `GalaxyState` message (sent roughly once a second) that drives the client's galaxy map.

## Data-driven world

The world is data, not code: `data/universe.json` indexes systems and links; `data/systems/*.json` describe each system's objects. Factions and their relations live in `data/factions.json`. The format is documented in [documents/world_format.md](documents/world_format.md), and the world editor writes exactly this format.

## Agents and standing orders

**Standing orders — a strategic layer above the tactical tick.** A human streams a `Command` sixty times a second; nothing else should have to. The server also understands durable, high-level orders — "mine this belt until the hold is full", "fly to that station", "travel to Verge" — and carries them out over seconds or minutes. An order decides what each tick's command should be and drives it through the same step a human client drives, so an ordered ship behaves exactly like a flown one. Progress and outcomes arrive as typed events in a per-session journal.

**The agent seam — `econagent` (#42).** An AI agent is an ordinary player, not a special case in the server. `econagent` is a **separate process** that is two things at once: an MCP server on stdio for the model, and a normal TCP game client to `econserver`, speaking the same `Command`/`Snapshot` protocol as every other client. It is written in C++ and links the protocol code, so the wire format has a single implementation rather than a second, hand-maintained one. The server knows nothing about MCP. On top: a compact text projection of the world (what a model actually reads), a blocking wait on the event journal so an agent sleeps until something happens rather than polling, and MCP resources and prompts. **How to build on it: [docs/agents/](docs/agents/README.md).**

## Presentation

**One seam, several backends (#35).** An entity no longer draws itself: it returns a `Render::Item` from `Describe()` and a backend turns items into pixels, characters or lines of text. `Render::FromArchetype` is the one place a `Visual` becomes an `Item`, so a tool that draws an archetype without an entity — the gallery — cannot show a picture the world would not. The backends today are shapes, glyphs and text (the last with no raylib calls, for agents and tests). A scene carries a `Render::Lighting` beside its items (#119) — a *list* of lights derived from the objects in the system, because two-star systems and player-built beacons are both wanted; a backend that has no use for it ignores it. Over the finished frame sits `Render::Treatment` (#120): the world is drawn into a texture and put through an ordered chain of full-screen passes described in `data/look.json`. Its data half (`TreatmentConfig`) is deliberately a separate file from its GPU half, because a CI runner has no graphics card and the data half is the part a test can hold. `Render::Material` and `MaterialLibrary` are split the same way for the same reason (#121): an archetype names a material, a material maps shader uniforms to sources on the item and in the scene, and the backend resolves them per object — so an object stays as ignorant of shaders as it already is of backends. `Render::Silhouette` completes the set (#122): an object's *shape* is a composition of parts described in its archetype rather than a figure compiled into the backend, so a new kind of object needs no new drawing code at all. **Glyphs are no longer the primary look**; see the 2026-09-03 entry in [DECISIONS.md](DECISIONS.md) and milestone M6. What survives from the original reasoning is the part that mattered: a player-built object needs no new drawing code, because its look comes from its archetype.

## Planned directions (not built yet)

None of the following exists in the codebase. It is recorded here so new work lands in the right shape. Sequencing lives in [ROADMAP.md](ROADMAP.md).

**A system a million units across (M9).** Scale has been a hostage of travel time: one speed meant everything had to be within a minute of flying. Systems become forty times larger, and travel is chosen from the overview's list rather than pointed at. Positions stay `float`, which is why a million and not ten: at 1e6 the gap between representable values is about 0.06 of a unit.

**A generated region (M7).** The server generates the region from a seed and sends each client the `SystemLayout` it already sends, so nothing about the wire changes. The seed joins the save; changing the generator's rules becomes a migration.

**World mutation and `LayoutDelta` (#44).** `SystemLayout` is sent **once**, when the client enters a system; everything that changes afterwards travels as per-tick `Snapshot` entries for entities the client already knows about. That is exactly why the world cannot change shape today: there is no message that says "a structure now exists here" or "this one is gone". The planned fix is authoritative world mutation on the server plus a `LayoutDelta` message (added/removed/changed layout entries) alongside the existing snapshot stream, with construction, ownership, permissions, limits, and upkeep built on top, feeding the macro-dynamics that already run.
