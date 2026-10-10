# Roadmap

An honest snapshot of where EconSpace is and where it is going. The project invested in a correct **client–server architecture first**, and that part is real. The target is an **MMO** — one authoritative server, many players, no single-player mode — with generated art, AI agents as ordinary players, and a world players design and build in.

**The premise, settled on 2026-09-04:** the game begins at the moment a wormhole opens into an unexplored region. The world beyond it is **generated from a seed**, nothing has a name until somebody goes there and names it, and what the region becomes is the record of what players did to it. Hand-written content is a layer on top of the generated one, not the other way round. The reasoning is in [DECISIONS.md](DECISIONS.md).

[Done](#done) is what exists. [Milestones](#milestones) is the plan, in order. Live progress is on the [milestones page](https://github.com/HEL3AN/econspace-mmo/milestones).

---

## Done

**Playing**
- Flight with a stabilizer, autopilot, server-authoritative warp, and the standing behaviours a fight is flown with: orbit and keep at range (#157).
- A multi-system galaxy (`universe.json` + `systems/*.json`) joined by jump gates, with a full-screen star map. Systems are currently about twenty-five thousand units across; M9 makes them forty times larger.
- Mining, a station market with price impact and recovery, ship refits, missions (bounty, mining, delivery) with a job board and a journal.
- A camera that belongs to the player: zoom from a hull to a system, look away and come back, a scale bar (#158).

**Factions and AI**
- Data-driven factions and relations (`data/factions.json`); reputation tiers, station prices and access, wanted levels and bounties.
- NPC roles — trader, miner, police, pirate, warship — with a behaviour state machine, shared combat, and spawning by system security and owner.

**The server and the living galaxy**
- An authoritative `Simulation` on a fixed `1/60` tick, running headless in `econserver`.
- Persistent systems and agents with stable ids; the whole galaxy and every account survive a restart, and refuse a save from a newer build (#20).
- Macro-dynamics: system controllers, a gate-line economy, territory captures, an event feed on the map.

**Multiplayer**
- Many players on one server, each with their own session, ship, account and missions (#3); players see each other (#4); progress survives a reconnect (#49, #5).
- Accounts are named and prove themselves with a challenge rather than sending a secret (#106); one account plays in one place at a time (#105).
- A snapshot carries only what changed, twenty times a second (#16, #97); the transport refuses what a peer should not be able to make it do (#14).
- Built and played in CI on Windows and on Linux (#12), including two clients at once (#107).

**Agents**
- Standing orders the server carries out over seconds or minutes; a text projection of the world; multi-jump routes; an event journal to sleep on.
- `econagent`, an MCP server written in C++ that is also an ordinary game client: nine tools, two resources, four prompts — and a guide for building your own bot, with an example CI runs (#171).

**The look** — generated, not drawn (M6)
- Every object is a composition of parts described in `data/archetypes.json` (#122), lit by its system's own stars (#119), shaded per part by a material (#121, #135).
- It moves: rings turn, lamps blink out of step, engines burn (#136); moons pass behind their planets (#165); planets turn on their axes with surfaces projected onto a sphere (#166); stars, belts and nebulae live too (#161).
- A screen treatment — bloom, pixels, scanlines, grain, fringe, vignette — tunable, reorderable and switchable in game (#120).
- A gallery that shows every kind of object at once, lit and treated as the game draws them, with the look editable live and saved back into the data (#118).

**Earlier milestones, for the record**
- **Ground truth.** The single-player path removed; the client a renderer plus an input source (#23); the server outlives client sessions and persists the galaxy (#13, #48); the wire layer one library (#25). It found a use-after-free on system change (#46) and a simulation clock that had never advanced (#57) on the way.
- **Agent MVP.** An agent observes, orders, sleeps until done and acts on the result, entirely through MCP (#26–#33); the protocol gained a version and a handshake (#15).
- **Data-driven world.** A kind tag rather than RTTI (#19); object types and their components in data (#34); entities describe themselves and a backend draws them (#35); the editor draws through the same seam (#37). Glyphs were the default look here (#36) — a decision later reversed (see M6).
- **Multiplayer core (M3).** Everything under *Multiplayer* above. A review of it then found and closed what stood between a demo and a service: one account playable twice at once (#105), a name that was a claim rather than a credential (#106), and no test that had ever connected two clients (#107).

---

## Milestones

In the order they are meant to be done.

### M6 — The look *(eight of ten done)*

Glyphs have their **sensor screen** (`V`): the surroundings projected onto a fixed grid, the one job they are genuinely good at, and the place where colour means allegiance as the pilot looking sees it (#123, #117). Left of #123: the world view stops offering glyphs at all, now that they have somewhere better to be. Queued behind it: shape derived from what an object does (#137), damage that takes parts off (#138), and variation that changes a silhouette rather than nudging it (#139).

### M9 — The scale of a system *(in progress)*

A station was 5.6 times a ship and a planet was nine — numbers nobody chose, which fell out of the game having one speed, so everything had to be within a minute of flying. A system becomes forty times bigger (a million units), a station towers over a ship, and travel stops being a matter of pointing at what you can see.

Done: orbit and keep at range (#157, first half), the player's camera (#158), life for planets and regions (#161), orbiting parts (#165), surfaces on a sphere (#166). The overview's sorting and filtering (#157), **the scale change itself** (#159) and warp for the new distances (#160) are done too. Left: correct materials on high-DPI displays (#179).

**Before M7**, because the generator will bake in whatever scale exists when it is written.

### M10 — Open to others *(in progress)*

A developer or an agent can land in the repository, understand how it works, and build their own bot or content without reading the source first. Documents that tell one story (#170), a guide to building bots (#171), a tool reference generated from the server so it cannot drift (#172), how-tos for adding content (#173), and repository hygiene (#174).

### M7 — A region nobody has mapped

The world stops being written and starts being generated. In order: the seed and its place in the save (#140), a screen that shows fifty systems at once, because a generator is judged on its hundredth output (#141), the rules that fill one system (#142), a region that grows outward from the wormhole and worsens with distance (#143), systems unknown until somebody goes there (#144), names that exist only once somebody gives them (#145), what the generator leaves behind (#146), and the pins that keep hand-authored content alive (#147).

### M4 — Constructible galaxy

World mutation and `LayoutDelta` (#38–#41), and the rules about players fighting (#94). The server can now add, change and remove a static object while players watch, and a `LayoutDelta` tells them before any snapshot that reflects it (#38, first slice); what remains of #38 is making those changes survive a restart.

### M8 — The builder

A player designs a *type* and builds it. The parts editor becomes a tool in the client (#148); what a design can do follows from what is on it (#149) — the rule of #137 read backwards, and what makes the builder systemic rather than decorative; cost and build time follow from the same parts (#150); the design is data the server validates, stores and shares (#151); building it (#152); and a structure changing the region it stands in (#153), where the macro simulation stops being flavour and becomes the score. Waits on M4.

### M5 — Fleets and depth

Fleet command over agent-piloted ships, agents that can take a mission, buy and fight (#109), and economic and progression depth.

---

## Help wanted

The most useful help right now:

- **Bots.** Build one on `econagent` and tell us what it could not do — [docs/agents/](docs/agents/README.md). What an agent finds impossible is the agent interface's to-do list (#109).
- **The agent interface** (#109) — orbit, buying, missions and combat for bots; C++ MCP work.
- **World mutation** (M4) — starting with `LayoutDelta` and the server-side mutation path.
- **Content** — new kinds of object, composed in data and judged in the gallery.
- **Audio** — there is none.

Art is welcome but is no longer what the project is blocked on: an object's look is generated from its archetype, and a hand-made sprite wins wherever one exists.

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [issue tracker](https://github.com/HEL3AN/econspace-mmo/issues).
