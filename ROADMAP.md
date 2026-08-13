# Roadmap

This is an honest snapshot of where EconSpace is and where it's headed. The project deliberately invested in a correct **client–server architecture first**. That part is real. The target is an **MMO** — one authoritative server, many players, no single-player mode — with a glyph-based presentation, AI agents as ordinary players, and a world players can build in.

The "Done" section below is what exists. Everything under the tracks is **planned**.

## Done

**Core sandbox**
- Windowed UI, menu panel, adaptive window / fullscreen, settings.
- Missions (clear / mine / deliver): job board, journal, rewards.
- A large multi-system world (~25k units): warp, parallax background, planet types, station roles, nebulae/derelicts/jump gates, pirate spawns on the frontier.
- Multi-system galaxy: `universe.json` + `systems/*.json`, working jump gates, full-screen star map.
- A visual **world editor** (systems, objects, properties, galaxy links).
- A texture store that draws sprites from `data/textures/` and falls back to vector shapes when a file is missing (no art has been contributed yet — see `documents/texture_assets.md`).
- Server-side persistence: the player account, and the galaxy itself, survive a restart.

**Factions & AI**
- Data-driven factions and relations (`data/factions.json`).
- Reputation and effects: tiers, station prices/access, mission rewards, NPC hostility by reputation/wanted level, crimes → wanted/bounty.
- NPC roles (trader/miner/police/pirate/warship) and a behavior state machine; generalized combat; role-based spawning by system security and owner.

**Living galaxy / server backend**
- An authoritative `Simulation` / `SystemState` with a fixed tick decoupled from rendering.
- Persistent systems and stable agent ids; whole-galaxy save.
- Macro-dynamics: system controllers, a gate-line economy, territory captures/reclaims, an event feed shown on the map.
- A headless simulation seam that grew into the `econserver` dedicated server.

**Client–server / networking (track M)**
- The full simulation (agents, spawn director, macro, persistence) lives in `Simulation`; `econserver` runs the exact same logic headless.
- A `Command` / `Snapshot` / `SystemLayout` / `GalaxyState` protocol over a swappable `ITransport` (`TcpTransport` winsock for play, `LocalTransport` loopback for tests).
- The player is a server-side agent; movement uses client-side prediction + reconciliation (own ship) and interpolation (others).
- `econserver host [port]` + `econspace connect <host> [port]`: real authoritative play over TCP.
- Server-authoritative selection, docking, trading, combat (beams/damage/death/respawn), a live galaxy snapshot, **player accounts** (money/skills/reputation/wanted, persisted to `account.json`), **missions on the server**, and reputation-gated docking.

## Tracks

Three epics carry the work from here. They are largely independent and can advance in parallel; the milestones below say how they interleave. Nothing in this section is implemented yet.

### Track A — Agent API (#42)

Make an AI agent a first-class player: the game speaks to a model the same way it speaks to a human, over the same protocol.

- **Standing orders on the server** — durable, high-level orders (mine here, haul there, defend that gate) that the server executes over seconds, while the tactical loop keeps running at 60 Hz. Nobody should have to stream thrust bits to play.
- **A compact text projection of world state** — what a model actually reads: the current system, nearby objects, the ship, the account, order progress.
- **`econagent`, the MCP server** — a separate process that is an MCP server on stdio for the model and an ordinary TCP game client to `econserver`. Written in C++ and linked against the existing protocol code so the wire format stays a single source of truth.
- **Event journal + blocking wait** — an agent sleeps until something happens instead of polling.
- **Multi-jump route planning** — orders that cross systems, not just one.
- **MCP resources and prompts** — the world as readable resources, plus prompts for common play patterns.
- **Fleet command** — one operator directing several agent-piloted ships. The human stops being a pilot and becomes a commander.
- **A scripted agent for tests** — a deterministic non-LLM driver so the agent seam is testable in CI.

### Track D — Data-driven world and ASCII presentation (#43)

- **Data-driven archetypes with components** replacing the C++ class hierarchy for world objects: what a thing *is* becomes data, so new content stops requiring new classes.
- **A presentation layer with pluggable backends** — drawing goes through one seam instead of entities calling raylib themselves.
- **Glyphs as the primary look** (#36) — ASCII/text is the game's actual visual language. It closes the art gap honestly, it lets players build structures without an artist, and it is close to the projection an agent reads. Sprites remain an optional backend.
- **The editor on the same layer** — the world editor draws through the same presentation seam as the game, so authored content and played content cannot diverge.

### Track C — Player-mutable world (#44)

- **Authoritative world mutation + `LayoutDelta`** — today `SystemLayout` is sent once on entering a system, which is the concrete blocker: there is no message that says a structure now exists. This is the piece everything else in the track waits on.
- **Blueprints and construction** — deployables and structures players actually build.
- **Structures feeding the macro simulation** — what players build changes prosperity, security, and territory control, which the server already simulates.
- **Ownership, permissions, limits, upkeep** — so a buildable world does not become an unbounded one.

## Milestones

Planned sequence, roughly in order:

1. **Ground truth** — the docs tell the truth, the leftover single-player path is removed (#23), general hygiene. Cheap, and everything after it is easier.
2. **Agent MVP** — standing orders, the text projection, and `econagent` far enough that an agent plays end to end without a human at the controls.
3. **Glyph world** — the presentation layer lands and glyphs become the default look.
4. **Multiplayer core** — multi-client (#3): per-connection sessions with their own ship and account, plus interest management so players in a system see each other. This is **foundational, not optional** — it is what makes the rest an MMO rather than a simulator with one seat.
5. **Constructible galaxy** — world mutation, `LayoutDelta`, construction, and structures with real macro effects.
6. **Fleets & depth** — fleet command over agents, server-side ship ownership, economic and progression depth, scale work (sharding, larger galaxies).

## Help wanted

Contributions are still very welcome, and the useful work has shifted. The most valuable help right now:

- **Multi-client server work** (#3) — sessions, per-connection state, interest management. The highest-leverage code in the project.
- **The presentation layer and glyph backend** (#36, #43) — this unblocks both the visual identity and player-built content.
- **`econagent` and the agent seam** (#42) — C++ MCP server work, and play-testing what an agent can and cannot actually do with the orders it is given.
- **World mutation plumbing** (#44) — starting with `LayoutDelta` and the server-side mutation path.
- **Content & depth** — more systems (via the editor), a real progression loop, economic depth.
- **Audio** — there is still none.

Sprite art is no longer on the critical path: glyphs are the primary look, and sprites become an optional backend. Art is welcome, but it is not what the project is blocked on.

If any of that interests you, see [CONTRIBUTING.md](CONTRIBUTING.md) and the [issue tracker](../../issues).
