# Roadmap

This is an honest snapshot of where EconSpace is and where it's headed. The project has deliberately invested in a correct **client–server architecture first**; content and presentation are the deliberate next frontier.

## Done

**Core sandbox (single-player)**
- Windowed UI, menu panel, adaptive window / fullscreen, settings.
- Missions (clear / mine / deliver): job board, journal, rewards.
- A large multi-system world (~25k units): warp, parallax background, planet types, station roles, nebulae/derelicts/jump gates, pirate spawns on the frontier.
- Multi-system galaxy: `universe.json` + `systems/*.json`, working jump gates, full-screen star map.
- A visual **world editor** (systems, objects, properties, galaxy links).
- A texture store with a shape fallback (the sprites themselves are not drawn yet — see `documents/texture_assets.md`).
- Save/load, with autosave on docking.

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
- A `Command` / `Snapshot` / `SystemLayout` / `GalaxyState` protocol over a swappable `ITransport` (`LocalTransport` loopback + `TcpTransport` winsock).
- The player is a server-side agent; movement uses client-side prediction + reconciliation (own ship) and interpolation (others).
- `econserver host [port]` + `econspace connect <host> [port]`: real authoritative play over TCP.
- Server-authoritative selection, docking, trading, combat (beams/damage/death/respawn), a live galaxy snapshot, **player accounts** (money/skills/reputation/wanted, persisted to `account.json`), **missions on the server**, and reputation-gated docking.

## Next

- **Multi-client** — the server accepts multiple connections; each player is a per-connection session with its own ship and account; interest management so players in the same system see each other and can interact.
- **Scale** — sharding / larger galaxies once multi-client is solid.
- **Server-side ship ownership** — persist owned ships / current ship type per account.

## The bigger picture (help wanted)

The architecture is strong; the game around it is thin. The highest-leverage work is **not** more networking:

- **Art** — replace placeholder shapes with real sprites (prompts in `documents/texture_assets.md`).
- **Audio** — there is none yet.
- **Content & depth** — more systems, a real progression loop, economic depth.
- **A vertical slice** — enough polish and content to hand a stranger and get honest feedback.

If any of that excites you, contributions are very welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).
