# Roadmap

An honest snapshot of where EconSpace is and where it is going. The project invested in a correct **client–server architecture first**, and that part is real. The target is an **MMO** — one authoritative server, many players, no single-player mode — with generated art, AI agents as ordinary players, and a world players design and build in.

**The premise:** the game begins at the moment a wormhole opens into an unexplored region. Each server **generates a whole world from a seed** that grows by regions as the "tide" rises (#342), nothing has a name until somebody goes there and names it, and what a region becomes is the record of what players and factions did to it. Hand-written content is a layer on top (admin packs and pins), not the other way round. The reasoning is in [DECISIONS.md](DECISIONS.md).

[Done](#done) is what exists. [Next](#next) is what comes now, in order. [Milestones](#milestones) is the longer plan. Live progress is on the [milestones page](https://github.com/HEL3AN/econspace-mmo/milestones). The first public release, **0.1 Anchor**, was tagged on 2026-10-17.

---

## Done

**Playing**
- Flight with a stabilizer, autopilot, server-authoritative warp that bends around stars and planets (#160), and the standing behaviours a fight is flown with: orbit, keep at range and follow (#157, #377).
- Systems a million units across (#159), flown by choosing from the overview rather than pointing (#157); a camera that belongs to the player (#158).
- One route planner with EVE's preferences (shortest, safer, less secure), waypoints and favourites, on a galaxy map with a star menu (#432).
- Mining from belts that run out and grow back; a goods chain in data (`data/goods.json`) with stock-driven prices, buying, and a fee on every trade (#343, #348); NPC haulers that carry goods between markets.
- Missions, jobs posted by faction plans (supply, escort, siege, #345) and delivery contracts priced from real shortages (#407).
- Ships are designs (#279): stats derived from parts, refits at a shipyard paid in credits, a named design library; NPCs fly designs chosen by their faction's doctrine.
- Ship loss and insurance: a lost ship is lost, and you wake at home (#344).
- **One combat model** for NPCs and players: an NPC fires its design's real fit, at the player cooldown (#463).

**The world, generated and growing (#342, slices S1-S9)**
- A galaxy skeleton of region slots per seed, several civilisation centres as starts, and a new pilot choosing one.
- The tide opens new regions while the server runs; an opened region is sealed into the save. A mouth stays only if an Anchor is built on it; otherwise the region drifts, and at the cap it may go into an archive and come back decayed.
- New regions are alive from the first minute: native factions with stations, markets and names in their own culture; expeditions from neighbours grow outposts into stations.
- Roaming wormholes and a scanner that resolves hidden things (#346): the holes, pirate hidden bases, and rich belts deep in the frontier.
- Charts sold to factions from the map (#346, #431 slice 16): a faction pays by what its own picture lacks -- a system it never saw, what was built since its last look, a pilot's scanner finds -- fresh and rare worth most, and decides on what it bought.
- Admin packs, validated and woven into a new world, and pins (#147); `econserver validate`.
- Factions are a data registry, not a compiled enum (#355).

**Factions, territory and law (#295, #431 slices 1-15)**
- Systems are physical only where a player is (hot and cold systems); everything else runs as arithmetic in a coarse pass, so the world does not stop where nobody is (#295).
- Factions settle, build outposts with upkeep, besiege, and send convoys that live or die by their own strength.
- Every pilot has their own map: public charts, their own and their corporation's; factions chart what they reach, and first contact reveals native names. The map remembers what you saw, and when.
- A corporation holds a system by influence and a claim, from next door only; building in held space takes a licence; defences mend by tier.
- PvP is a crime under the law: criminal flags, security status, police from real forces; security is bought with police.
- Attacks on your structures reach you offline, in your journal.

**Players together**
- Many players on one server, each with their own session, ship, account and missions (#3); accounts proved by challenge and answer (#106); one account, one session (#105).
- Local chat, private lines and a corporation channel; corporations with members, roles, a treasury and their own AI patrols (#347, #431 slice 12).
- Players build: structures from blueprints (#39), armed batteries with rules of engagement (#340), outposts that grow along a chain, keep goods, dock corporation mates and pay their owner's market fee (#433, #438), and shipyards built by need (#432). What a corporation member builds is the corporation's; a structure changes hands with its licence, and a disbanded corporation's are abandoned for anyone to take (#468).

**The server**
- An authoritative `Simulation` on a fixed `1/60` tick, headless in `econserver`; every world and account survives a restart and refuses a save from a newer build (#20).
- Snapshot deltas against the acknowledged state and an area of interest (#349); limits on frames, backlogs, connections and logins (#341, #14); client prediction under test (#358).
- A friends' server (#400): `--world`, `server.json`, an allowlist and bans, a console and an admin channel, backups, stamped logs; relocatable binaries and a portable package (#399, #403); a version and codename in every binary, and a release workflow.
- Built and tested in CI on Windows, Linux (also under ASan and UBSan) and macOS, with two clients at once (#12, #107).

**Agents**
- `econagent`, an MCP server written in C++ that is also an ordinary game client: 49 tools, two resources and five prompts, with a reference generated from the server and checked by CI, a guide, and an example bot CI runs (#171, #172).

**The look** — generated, not drawn (M6, done)
- Every object is a composition of parts in `data/archetypes.json` (#122), built from a module library (#240), lit by its system's own stars (#119), shaded per part by a material (#121, #135), and moving as `f(time, seed)` (#136, #165, #166).
- A screen treatment, tunable and switchable in game (#120); glyphs as a sensor screen, where colour means allegiance (#123, #117).
- An interface framework: a desk of windows with one mouse owner per frame, themes, a UI scale, layout by Clay (#297); accessibility settings (#362).
- A world editor: the gallery, a survey of generated systems, region and galaxy maps, saved worlds, packs and pins.

---

## Next

In order. Releases are the owner's call.

1. **#464 — NPC fits from the living world.** Gang, faction, wealth and location decide an NPC's fit. Slices 1-4 are built (variants, fleets, region style, buying with money limiting the count); next: refit and drift, pirates and captures, what players and agents see, damage types.
2. **#466 — a corporation's enemy list**, with the war declarations #431 deferred.
3. **#425-#427 — playtest polish** from 0.1: docking, reconnect, the job board, planet surfaces, UI overlaps and raw numbers.
4. **A playtest on a fresh world** (so much has changed since 0.1 that an old world would not show it), then **0.2** if the owner decides to tag it.

Also open and not scheduled: #342 S10 (scale to 10,000 systems), #407 (work near the starts), the review issues #350-#360 (code structure, saves, transport security), #179 (high-DPI), #333 (industry, a proposal).

---

## Milestones

The milestones predate the 0.1 release. Most new work since is filed as issues without one.

- **M3 Multiplayer core, M6 The look, M10 Open to others** — done.
- **M7 A region nobody has mapped** — done in substance, then superseded by world generation v2 (#342).
- **M9 The scale of a system** — done except #179 (materials and the screen treatment on high-DPI displays).
- **M4 Constructible galaxy** (#39-#41, #94) — largely built: blueprints and structures, outposts with upkeep and storage, rules of engagement, PvP under the law. Left: ownership transfer and rent, and what remains of permissions and limits.
- **M8 The builder** (#148-#153) — a player designs a *type* from the same parts the world is built from, and a design's parts decide what it can do. Not started; ship fitting (#279) is its first relative.
- **M5 Fleets and depth** (#6-#9, #32) — fleet command over agent-piloted ships, onboarding, deeper NPC movement.
- Queued without a milestone: shape derived from what an object does (#137), damage that takes parts off (#138), variation that changes a silhouette (#139), audio (#2).

---

## Help wanted

- **Bots.** Build one on `econagent` and tell us what it could not do — [docs/agents/](docs/agents/README.md).
- **Playtesting.** Join a server, or host one for friends ([docs/hosting.md](docs/hosting.md)), and file what breaks.
- **Content** — new kinds of object, composed in data and judged in the gallery; admin packs.
- **Audio** — there is none.

Art is welcome but is not what the project is blocked on: an object's look is generated from its archetype, and a hand-made sprite wins wherever one exists.

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [issue tracker](https://github.com/HEL3AN/econspace-mmo/issues).
