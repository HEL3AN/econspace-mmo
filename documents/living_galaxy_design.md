# Living Galaxy / Server Backend — Transition Design

> A transition from "the player = the center of the logic" to "the player = one of the
> agents of an authoritative world simulation". We are preparing the server
> infrastructure: the galaxy lives on its own, independent of the player's presence and
> location. This is the foundation for a future MMO. Implementation is track **L**
> (Living Galaxy), stages L0–L4 (+ F6). It is closely tied to factions: **F5
> (macrodynamics) is implemented as stage L3**.

## 1. Principle

Right now the player is the center: the world exists only where the player is, and only
while they are there. The goal is an **authoritative galaxy simulation** in which the
player is merely one of the agents. All systems live simultaneously (economy, factions,
combat), even without the player. The player connects as a **client**: they see their
system and issue commands, but they do not "hold" the world in memory by their presence.

## 2. MMO Clarifications (they define the framework)

Decisions made during the planning stage:

1. **Agents are first-class citizens.** NPCs (and the player) are not "a spawn in a
   system" but **persistent galaxy agents** with a stable `id` and a "current system"
   field. Key ships/convoys **actually travel between systems** (through gates, changing
   their system), rather than being recreated. (The "Deep" background simulation was
   chosen.)
2. **There can be many hot systems.** In an MMO, full simulation runs everywhere there
   are players — that is N systems at once, not one. Therefore the architecture does NOT
   assume "a single active system": the simulation holds a set of systems, each with a
   level of detail (fidelity). Right now (a single player) hot is usually one, but the
   interface and the tick are designed for N.
3. **Live actions matter more than statistics.** In an MMO, real agent actions are
   preferred, and pure statistics are a **degradation** for systems where there is truly
   no one (neither players nor "important" agents). Even in cold mode, key agents live
   individually (in a reduced form), while the extras run as an aggregate.
4. **Hybrid time model.** Systems near a presence tick continuously; distant empty ones
   **catch up** on elapsed time when a presence appears (catch-up by timestamp). An
   authoritative fixed tick, designed for several clients.
5. **Headless seam — at the end (L4).** First a living world inside the game, then
   extract the simulation into a render-less mode.

## 3. Current State (per the code, starting point)

- `Game` owns everything: `entities_` (**only the active system**), `playerShip_`,
  `player_`, `market_` (one, for the current system), `missions_`, camera, UI,
  `universe_` (only the system index).
- A jump (`LoadSystemById`) **destroys** `entities_` and rebuilds them from scratch;
  `PopulateNpcs` is a **one-time** spawn by role/owner/security (F4).
- Simulation = the render loop: a single frequency, everything in `Game::UpdateWorld`
  for one system. NPCs are not persistent and are not saved.
- The save = **only the player** (money, skills, reputation, ships, cargo, system,
  missions).

The key thing we are decoupling: the simulation and the client are fused in `Game`;
only one system exists at a time; NPCs are ephemeral.

## 4. Target Architecture

**(a) An authoritative `Simulation`.** A new layer (in `engine` or a new `sim` module)
owns the galaxy's state: `systemId → SystemState`. It ticks at a **fixed step** (e.g.
1/30 s) with an accumulator, separate from FPS. `Game` is a client: it renders the
present system from the sim's state, turns input into **commands** of the player agent,
and reads state for the UI.

**(b) Agents are first-class.** `Agent` (NPC and player) with a stable `id`, faction,
role, current system, position/heading/hull, and AI state. Agents belong to the galaxy,
not to a system; "entering/leaving a system" is a change of the system field, not a
recreation.

**(c) LOD — system levels of detail:**
- **Hot** (there is a presence — player/client): full agent simulation — positions, AI,
  combat, beams. There can be several such systems.
- **Warm** (neighbors of hot systems): continuous but lightweight tick (agents live,
  without expensive render effects).
- **Cold** (no one): degradation. Key agents — individually and rarely; the extras — as
  aggregates (population by role/faction, market, security pressure, ongoing events).
  When a presence appears — **catch-up** on the elapsed time, then materialization
  (hydrate) into full agents.

**(d) Determinism.** A single **seeded PRNG** in the sim (instead of raylib's
`GetRandomValue`). Needed for reproducibility, saves, and server authority.

**(e) The client↔sim boundary (maturing toward L4).** Commands inward (thrust, fire,
dock, jump), state snapshots outward (what to render/show in the UI). This is the future
network protocol; for now — calls within a single process, but with an API designed for
networking.

## 5. Combining with Factions

- **F5 (macrodynamics: territories, wars, security, economy)** — this is the content of
  the living simulation, implemented as **stage L3**. Without a living world there is
  nowhere to hang it.
- **F6 (editor for factions/owner/relations)** — orthogonal: it edits static data
  (`factions.json`, systems' `owner`), which the simulation consumes. It can be slotted
  in at any point.
- The factions/AI design — `documents/factions_ai_design.md`. F1–F4 are done.

## 6. Staged Plan (track L). Each stage is a working build + commit.

| Stage | Gist | Main changes |
|------|------|-------------------|
| **L0** | **Seam: an authoritative `Simulation`** (without changing gameplay) | Move the world state out of `Game` into a `Simulation` that owns `SystemState`. A fixed tick `Tick(fixedDt)` with an accumulator, separate from rendering. `Game` renders/inputs through the sim. **The API does not assume a single system** — a set of systems with a fidelity level (currently de facto one hot). Gameplay is identical. |
| **L1** | **Agent-citizens + galaxy persistence** | NPCs and the player → `Agent` (a stable `id`, a current system). Systems are no longer destroyed — state lives on. Hydrate/dehydrate = a change of agent fidelity, not their creation/deletion. The save expands to **the whole galaxy**. Check: kill pirates, fly away, come back — it remembers. |
| **L2** | **Cold degradation + hybrid time** | Empty systems tick cheaply: key agents — individually, the extras — as an aggregate. Neighbors of hot — a warm tick. Distant ones — catch-up by timestamp on entry. A seeded PRNG. Check: fly away for a long time, come back — the world changed on its own. |
| **L3 (=F5)** | **Macrodynamics: inter-system agents, economy, wars, territories** | Convoys/agents actually fly between systems (through gates, changing system). The economy links systems by moving goods (stock/prices at both ends). Wars (relation War) — skirmishes in the borderlands (cold events + materialization when the player is present). `owner` changes under pressure. All of it visible on the star map, including where the player has not been. |
| **L4** | **Headless / server seam (MMO-prep)** | The simulation runs **without raylib**: a separate target (`econsim`) or a flag — ticks the galaxy and dumps state. A clean commands↔snapshots interface — the future network boundary (without the network itself). Support for **multiple hot systems** is shaken out here. |
| **F6** | **Editor: factions/owner/relations** | A UI for editing `factions.json` and systems' `owner`. Independently, whenever convenient. |

## 7. Cross-Cutting Questions (resolved along the way)

- **Stable ids**: agents, systems, stations — stable identifiers (for persistence,
  inter-system travel, the future network).
- **Player commands through a queue** (rather than direct world mutation) — the future
  network input. Introduced gradually, starting with L0/L1.
- **State snapshots** for the client/UI — the future protocol.
- **Multiple hot systems**: the tick processes all present ones; "presence" =
  client/player (one now, many later). Do not hardcode a single one.
- **Cold time model**: catch-up by a saved timestamp; cap the amount of catch-up so that
  entering a long-abandoned system does not lag.
- **Save**: a versioned galaxy format (`world_save.json`) + player.
- **PRNG**: a single seeded generator; the seed is in the save for reproducibility.

## 8. Risks / What We Are NOT Doing Now

- **We are not writing the network.** Only preparing the boundary (commands/snapshots/ids).
  The network is a separate large track after L4.
- **We do not materialize all systems at once** — that is the whole point of LOD; cold
  must be cheap.
- **L0 is the riskiest refactor** (it touches almost all of `Game`); we do it in small
  steps with build checks, without changing behavior.
- **Depth (individual inter-system agents)** is the most expensive; we introduce it
  carefully across L1–L3, starting with a small number of "key" agents.
- Balancing cold rules and density is empirical (as with F4's spawn).

## 9. Start

We begin with **L0** — the seam on which everything else rests; gameplay does not
change, and it is easy to verify that nothing broke.
