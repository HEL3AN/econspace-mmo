# World generation v2: a world that grows

Status: **chosen by the owner on 2026-10-11.** The choice is option B, "the Tide and the
Anchors", with a **faction registry from the start** (no temporary "houses"). Tracking
issue: #342. The owner-facing proposal (in Russian) is at
https://claude.ai/artifact/TVLGJWjF2kk4i3m88dG6Sx; this file is its English record and
the reference for the slices below.

## What the owner asked for

- **No required base systems.** Each server generates a whole world of its own.
- **The world grows**, over time and with players. Growth needs a clear and interesting
  logic grounded in lore, and players must not wait long for new space.
- **An admin cap.** Above it the server generates no more.
- **New regions are alive.** Factions build outposts, markets and stations there, not
  only claim them. Beyond the first wormhole it must not be dark and silent.
- **Several generated civilisation centres.** A player chooses one or gets one at
  random, and the admin can make one of their own systems a start.
- **Wormholes play three roles:**
  - links that weave admin-made content in;
  - dynamic EVE-like holes;
  - gates to new regions.
- **Admin authoring:**
  - systems, or whole regions with their own story and factions, from scratch before
    the first launch, woven in at random places;
  - edits to anything after generation.

## What exists today and what carries over

- **Today's world:** three hand-written systems (`core`, `reach`, `verge` in
  `data/universe.json`) plus one generated region of 18 systems behind one wormhole. The
  region is made from the seed on first launch, laid out in rings of 2–4 systems with
  security falling by 0.1 per ring, and has no stations.
- **Regeneration and saved changes:** `GenerateRegion` already writes the JSON the loaders
  read. The region is regenerated, not saved. Changes to the static layer persist in
  `world.json` → `changes` under stable keys (#286), and pins apply after generation
  (#147, #237).
- **The living world already runs** (#295, variant A). Factions survey on intel,
  stockpile, build outposts, pay upkeep, and lay pooled sieges. Surplus NPCs migrate.
  Cold systems are arithmetic only: `macrobench 1000` costs 0.13 ms per tick with nobody
  on.
- **Limits to lift:**
  - factions are `enum FactionId` with `FACTION_COUNT = 4`, with `presence[4]` in
    aggregates and saves;
  - `AttachRegion` runs once and is tied to `universe_.startId`;
  - generator keys are tied to a system's ordinal within one region;
  - `PlanRoute` is an O(n²) Dijkstra.

Almost the whole machine for a living, growing world exists: the deterministic generator,
cold systems, and factions that build. Three things are missing:
1. a world of many regions, each with its own generation key;
2. the rules for when the next region opens;
3. stations and markets that factions put into a new region, not only outposts.

## Lore: why space grows

The chosen frame is **the Tide and the Anchors**.

- **The Tide.** Space around settled worlds is "folded" and slowly unfolds. Where it is
  under strain, unstable mouths break through into the unknown. Traffic through the
  frontier speeds this up.
- **Anchors.** A mouth lives about a day and then collapses. Only an **Anchor**, a
  gravitational stabiliser built at the mouth, holds a region open as permanent gates.
- **Who builds them and why.** Factions build Anchors for the mines and markets beyond.
  Players build them to be first to name the systems.
- **Background lore.** The dormant gates of a vanished civilisation (the Forerunners)
  explain why Anchors are possible at all: people learned to repeat the old technology,
  one mouth at a time. Their ruins sit in the centres, and the finds of #146 fit here.

This keeps the original premise: the game begins when a wormhole opens into the unknown.
In v2 that moment repeats on every server, many times.

The alternatives considered:
- **Option A, the Rift:** mouths open by themselves and are permanent at once. It is
  option B with Anchors switched off, available as a setting.
- **Option C, Dormant gates:** the whole world is generated up front and gates are woken
  with keys. It was rejected because the world would not really grow, only be revealed.

## World structure

| Level | What it is | Default size | Links |
|---|---|---|---|
| Galaxy | A skeleton of region slots on the map, up to the admin's cap, generated whole from the seed at world creation | up to `maxRegions` | slot adjacency: who may connect to whom |
| Civilisation centre | A core region: high security, every station type, markets, an owner faction, a start | 3 centres × 8–14 systems | gates inside; 1–2 links out at creation |
| Frontier region | Opened by the tide. It has a theme (ore, nebulae, ship graveyard, pirate) and a native minor faction | 12–30 systems | gates inside; Anchor gates to its parent; roaming holes |
| Pocket (later, optional) | One or two systems behind a roaming hole, existing while it is open | 1–2 | that hole only |
| Admin region | Hand-made systems woven into a free skeleton slot | any | admin links; may be a start |

**Centres.**
- **How many:** 3 by default; the admin chooses 1–6.
- **Placement:** placed far apart in the skeleton, about 4–6 regions, so the frontiers
  grow toward each other. The first link between two civilisations is a mid-life event
  in the chronicle.
- **Ownership:** each centre belongs to one major faction. Pirates have no centre; they
  are born on the frontier.
- **Generation:** a centre is a new region type of the same generator, not hand-written
  content. It has a trade hub, a shipyard, a military station and mining outposts.

**Gradients.**

| Parameter | Computed from | Effect |
|---|---|---|
| Security | region hops to the nearest centre, depth within the region | 1.0 at a centre's heart, 0.5 at its edge, 0.0–0.3 two or three regions out |
| Richness | distance from a centre, plus the region theme | more belts, ore, finds and ruins; frontier ore cheaper, goods for living dearer |
| Danger | the region theme and its native faction | pirate regions, drone regions, peaceful colonies |
| Services | what factions and players built | the frontier starts with outposts and trading posts; a full station appears within hours |

## Growth: the tide

Every region on the edge (one with free adjacent skeleton slots) has a `tide` value. It
accumulates in the coarse pass, the way faction stock already does, and is saved in
`world.json`. When it crosses the threshold, an **unstable mouth** opens from the region's
deepest system into the next skeleton slot.

```
// per period (one minute), for an edge region
tide += base                                    // time: the world grows even when empty
      + perPlayer  * min(players in the region this period, 10)
      + perExplore * share of the region's systems visited by players or surveyed by factions
      + perSurvey  * faction surveys in this region this period
threshold = baseThreshold * (1 + openRegions / maxRegions)   // slows toward the cap
```

| Trigger | Effect | Why |
|---|---|---|
| Time | `base`: about one new region per 2–3 days on an empty server | the world does not freeze without players (#295's rule) |
| Players in the region | `perPlayer`, capped at 10 | it grows where people play, but a crowd cannot explode the map |
| Exploring the frontier | share of the region visited or surveyed; pressure climbs fast above 60% | a player who reaches the edge does not wait long |
| An Anchor | does not open a mouth; it makes an open one permanent | growth becomes a decision worth fighting over |
| Server population | more active players allow more concurrent mouths (1 + players/20) | a server of 5 and one of 500 grow at their own pace |
| Admin command | `econserver`: open a mouth now, pause growth | events, tests, manual control |

Pacing targets (defaults, all in `data/world_rules.json`):

| Event | Target |
|---|---|
| A player has explored 60% of a frontier region | a mouth within 3 h |
| An empty server | one region per 48–72 h |
| An unstable mouth's life | 24 h |
| Building an Anchor | 45–60 min |
| A faction's first station in a new region | about 2–4 h |
| Minimum between mouths from one region | 6 h |

For scale: a region is about 20 systems, so a 2,000-system cap is about 100 regions. At
one new region a day a server reaches the cap in about three months, and an active one
reaches it sooner.

**The cap.**
- `maxSystems` and/or `maxRegions` are server settings. The skeleton is generated exactly
  up to the cap.
- Each slot has its own key, so raising the cap only extends the outer rings and never
  moves existing slots.
- At the cap no new regions open. The tide keeps working, but becomes roaming holes
  between existing regions: new short routes.
- Whether abandoned regions may collapse and free their slot is an open question.

**Determinism.**
- **The skeleton** is `f(world seed, slot)`, generated whole at world creation. It is
  cheap: points on the map and adjacency.
- **A region's contents** are `f(world seed, slot, GENERATOR_VERSION)`. They do not
  depend on when or from where the region opened, so the order the game opens regions in
  never changes what is in them.
- **What is saved:** a small **opening log**, each entry
  `{slot, openedAt, parent, mouth, anchor: who/when, state}`. Loading is seed + log, then
  packs, then pins, then `changes`.
- **Ids and keys:** system ids become global, e.g. `r14-2-3` (slot 14, ring 2, system 3),
  shown to players as `R14 · W-2.3`. `Gen::Rng` keys take the slot as their first part.
  The rules in `Rng.h` are unchanged: no sin/cos/exp, and keys by what is generated.
- **The golden test** splits into a skeleton hash and per-slot region hashes, so changing
  one region type breaks only its own hash.

## Three kinds of wormhole

|  | Mouth → Anchor gates | Roaming hole (EVE-like) | Admin link |
|---|---|---|---|
| Role | growth: leads to a new skeleton slot | dynamics: shortcuts, risk, exploration | weaves admin content in |
| Source | the tide crosses its threshold | a schedule by epochs (4 h windows): `Key(seed, epoch, slot)` picks ends from the sorted list of open systems | declared in an admin pack |
| Life | 24 h unstable; with an Anchor, permanent while the Anchor stands | 4–24 h by key; sooner if its mass is spent | permanent, or a lifetime the admin sets |
| Limits | small ships only without an Anchor | a mass budget and a maximum ship class; every jump spends mass | as the admin sets |
| Finding it | visible to everyone in the system and on the map; a chronicle event | needs a scan: the signature shows only after probing (a new module and agent tool) | visible like ordinary gates |
| Saved | an opening-log entry; the Anchor is a structure (#39, `changes`) | almost nothing: existence follows from the epoch; only spent mass and early collapse are kept | nothing: pack plus seed |
| What the player sees | a pulsing mouth with a collapse timer, then permanent gates named for whoever anchored them | a signature on scan, then a hole marked fresh or ending and its mass as much or little | gates named after the admin region |

**Routes.** A roaming hole changes the route graph. Warps and routes are decided by whoever
orders them (#160), so the server broadcasts a hole opening or closing as a
`GalaxyState`/`LayoutDelta` change (#38) before the first snapshot that shows it. That
changes a message's meaning, so `PROTO_VERSION` is bumped.

**A drifting region.** When a mouth collapses with no Anchor, the region stays in the log
with everything built there. Roaming holes still reach it, and the next mouth from the
same parent leads there again until it is anchored. Player structures are never lost;
their upkeep (#41) just costs more, because supply is cut off.

## Living new regions

Three layers. Each one works in cold systems as arithmetic, as #295 already does.

1. **Natives, from generation.**
   - Each frontier region gets a **minor faction** that fits its theme: breakaway
     colonists, a pirate clan, a sect at ruins, drone automata.
   - It has 1–3 stations with markets (a settlement, a pirate den, a trading post), its
     own supply and demand, and NPC traffic from the first minute.
   - The first player through a mouth finds inhabited points but not a station network.
     Prices are the frontier's: ore cheap, food and medicine dear.
2. **A wave of expeditions, in the first hours.**
   - **The opening:** a mouth gives the factions of neighbouring regions intel on the
     entry system and a temporary `curiosity` bonus. The existing #295 loop then runs:
     survey, then settle, then outpost.
   - **New: an upgrade chain by need**, as in X4: `outpost` → `outpost_market` (market,
     repair) → `station.trade_hub` / `station.mining_outpost`. Each step is a plan paid
     from stock, with build time. It is visible in a hot system and can be disrupted.
     Blueprints say `"builders": ["faction", "player"]`, so players build the same things
     (M8, #39).
   - **The Anchor** is a new faction plan beside Settle and Siege. Its value is the
     region's promise from surveying the entry system, its risk is who else is at the
     mouth, and it can be besieged (#295 slice 5).
   - **The builder convoy** players can meet (variant B's opening in #295) belongs here:
     an Anchor is built only from a convoy, and the convoy is a visible event.
3. **The frontier economy.**
   - A station's price is the base price × a remoteness factor along its supply route.
     That creates demand for hauling from a centre to the frontier, and delivery jobs
     without a script.
   - NPC traffic in cold systems already comes from the aggregate, and the surplus NPC
     migration of #369 carries traffic into new regions by itself.

**Factions: a registry from the start** (owner's decision, 2026-10-11). `enum FactionId` →
an index into a registry built from `factions.json`, generated factions and pack factions.
`presence[4]` → a small vector. Saves and the protocol use faction ids, not indices. It is
a broad but mechanical change: about two slices. It is needed for native factions,
admin-region factions and, later, player corporations (#347). Tracking issue: #355.

|  | Centre | Frontier |
|---|---|---|
| Security | high, police | low; each minor faction has its own |
| Stations | every type, shipyards | native posts, outposts, growing stations |
| Prices | narrow spread, everything stocked | wide spread, shortages; ore and finds cheaper |
| Names | named at generation | designations until someone names them (#145) |
| What happens | trade, stability | expeditions, sieges, Anchors, the race to name |

## Admin authoring

**A pack** is a directory, `data/packs/<id>/`. Inside are the formats the loaders already
read, plus one weaving description. The editor writes exactly what the runtime reads.

```json
// data/packs/helios/pack.json
{
  "schema": 1,
  "id": "helios",
  "kind": "region",
  "systems": [ { "id": "core", "file": "core.json", "map": [0, 0] } ],
  "links":   [ ["core", "verge"], ["core", "reach"] ],
  "factions": [],
  "start": ["core"],
  "weave": {
    "place": "random",
    "links": { "count": [1, 2], "kind": "static" }
  }
}
```

The field notes:
- `kind` is `"region"` or `"system"`.
- `factions` is optional.
- `start` lists the systems that become start points.
- `weave.place` is one of `"random"`, `"near-centre"`, `{"depth": [2, 4]}` or
  `{"slot": 7}`.
- `weave.links.kind` is one of `"static"`, `"dynamic"` or `"anchor-required"`.

**Weaving before the first launch.**
1. At world creation the server reads `data/packs/*/pack.json` in id order.
2. Each region pack takes a skeleton slot, chosen by `Key(seed, hash(pack id))` among
   slots matching `weave.place`. A system pack replaces one system of an
   already-generated region, which is exactly the `replace` mode of #147's pins.
3. Links are admin links from random systems in adjacent slots, also keyed by pack id.
4. The included packs and their content hashes are written to `world.json`. If a pack
   changes later, the server names it.

**Edits after generation.**
- **Opening a region:** the editor opens any region of the world
  (`worldeditor world <world.json> region 14`). The region/system mode and pins exist
  (#237); a pin only needs a slot key, `{"slot": 14, "system": "r14-2-3", ...}`.
- **Saving:** an edit is saved as a pin, the difference. Pin rules are unchanged: gates
  are topology, satellites belong to their planet, and a pin that cannot apply says why.
- **The living world:** its changes (structures, claims) stay in `changes` (#286). The
  order is generation → packs → pins → `changes`.
- **Unopened slots:** pins for slots not yet opened are allowed, so an admin can prepare
  a surprise that appears when the tide gets there.

**Starts.**
- The start list is the hearts of the centres plus everything a pack marks `"start"`.
- On an account's first login the client receives the list and chooses, or takes a random
  one. This is a new message after `Auth`, so `PROTO_VERSION` is bumped. An agent chooses
  with an MCP tool.
- The admin may disable the choice (random for everyone) or close a start, e.g. "only for
  new accounts until date X".

**Safety.**
- `econserver validate [path]` checks packs and pins without running a world. An unknown
  field, form or source is a load error (#191).
- **Before the first launch,** a bad pack stops world creation with a list of problems.
  The world does not exist yet, so nothing is lost.
- **After it,** a bad pin is not applied and is reported in the log and the admin's view,
  as today. A pack whose topology changed after world creation is refused as a whole:
  player structures already stand on its gates.
- **Pack limits:**
  - at most X systems;
  - coordinates within `SYSTEM_RADIUS`;
  - every system has at least one link;
  - a start must have a station with a dock.

## Scale and performance

Cold systems make size cheap in CPU. Everything else grows linearly. The 10,000 column
is scaled from the 1,000-system `macrobench`, not measured.

| What | 1,000 systems (measured) | 10,000 (estimate) | What to do |
|---|---|---|---|
| Tick, nobody on | 0.13 ms | ≈ 0.5–1 ms | nothing |
| Coarse pass (every 2 s) | 0.63 ms | ≈ 6 ms | spread over passes, a region at a time |
| Generation + materialisation at start | ≈ 0.2 s | ≈ 2 s | acceptable; a new region is about 2 ms for 20 systems |
| System documents (JSON text) | ≈ 2 MB | ≈ 22 MB | may be dropped and regenerated on warm |
| Static layer of cold systems | ≈ 7 MB | ≈ 70 MB | drop for cold systems, rebuild on warm (planned in #295) |
| `world.json` | ≈ 400 KB, 20 ms | ≈ 4 MB, 200 ms | write on a background thread from a snapshot (#350) |
| `PlanRoute` (O(n²)) | tolerable | bad | two-level routes: regions, then systems |
| The client's map | the whole list | too much | only what the player knows (#144), galaxy → region levels |

Proposed limits: a default of 2,000 systems and a tested maximum of 10,000, after the
scale slice. Nothing above that is promised. Positions within a system stay `float` within
a radius of 1e6; the galaxy map has its own scale.

## Migration from today's world

| What | What happens to it |
|---|---|
| The 3 hand-written systems | become the bundled pack `data/packs/helios/` (Helios Core, Sigma Reach, Tau Verge) with `"start"` on Helios Core; whether it is on by default is open; `data/universe.json` stops being required |
| The W-* region | its generator becomes the "frontier region" type; old ids like `w2-1` become `r<slot>-2-1` |
| `GENERATOR_VERSION` | bumped in slice 1; an old world is refused, as designed (#140); the golden hash splits into skeleton and per-region hashes |
| `world.json` | refused with a clear message; only a dev server lives today, so a new world is cheaper than a converter; `WORLD_VERSION` bumped for the opening log, tide and Anchors |
| Accounts | kept (ship, credits, standing); if an account's system no longer exists, the player gets the start choice on login |
| `data/pins.json` | empty, nothing to carry; the format gains `slot` |
| CLAUDE.md, DECISIONS.md, ROADMAP | the M7 premise is rewritten (done with this document) |

**Sealing (open question).** An MMO world lives for months, and `GENERATOR_VERSION` refuses
it whenever the rules change. One answer is to seal: when a region opens, its generated
documents go to their own file, `regions/<slot>.json`, so new rules apply only to new
regions. That breaks "the region is never saved" for opened regions, so it is for the owner
to decide.

## Slice plan

Each slice is its own PR with green CI, and the game works after every one.

1. **S1 — the skeleton and the slot key.**
   - `Gen::GenerateGalaxySkeleton(seed, cap)`.
   - `GenerateRegion` takes a slot; ids become `r<slot>-d-k`.
   - `AttachRegion` works for any slot, not only next to the start.
   - Bump `GENERATOR_VERSION`, use two golden hashes, and turn `regiondump` into
     `galaxydump`.
   - Gameplay is unchanged: one slot is open.
2. **S2 — civilisation centres and starts.**
   - A "centre" region type: stations, markets, an owner, high security.
   - 3 centres by default.
   - Helios moves into `data/packs/helios/`.
   - Start choice on login (`PROTO_VERSION`), with account migration.
   - Survey (F4) shows the centres.
   - After this slice no base systems are required.
3. **S3 — the opening log and the tide.**
   - Pressure per region, the threshold and the cap, in `data/world_rules.json`.
   - Opening a region while the game runs: cold aggregates, and a `GalaxyState` update to
     all clients.
   - Chronicle lines and admin commands.
   - Bump `WORLD_VERSION`.
   - A 24 h `econserver` run shows the pacing.
4. **S4 — mouths and Anchors.**
   - An unstable mouth with a timer and a ship-class limit.
   - An `anchor` blueprint for factions and players.
   - An Anchor faction plan in the value-against-risk step.
   - Collapse and drift.
   - Sieging an Anchor.
5. **S5 — native factions.**
   - A minor faction per frontier region, in the faction registry.
   - 1–3 generated stations with markets.
   - Frontier prices by remoteness.
6. **S6 — expeditions and growing stations.**
   - Intel on a new region and a curiosity bonus.
   - The `outpost` → `outpost_market` → station chain by need.
   - The builder convoy players can meet in a hot system.
7. **S7 — admin packs.**
   - The `pack.json` format and `econserver validate`.
   - Weaving at world creation, admin links and admin starts.
   - Pack hashes in `world.json`.
8. **S8 — the world in the editor.**
   - A galaxy map in the editor, with F5 levels.
   - Opening any region of the world.
   - Pins with `slot`, including pins for unopened slots.
   - Creating a region from scratch and exporting it as a pack.
9. **S9 — roaming holes.**
   - An epoch schedule, mass and ship class.
   - Probes and signature scanning, with agent tools.
   - Route updates on the wire.
   - Pockets later.
10. **S10 — scale to 10,000.**
    - Drop the static layer of cold systems.
    - Two-level `PlanRoute`.
    - A map of what the player knows (#144).
    - Background saves, and `macrobench 10000`.

**Alongside, and needed before S5:** the faction registry (#355), about two slices.

## Open questions

1. **At the cap:** does the world stop growing (growth goes into roaming holes only), or
   can abandoned regions collapse and free their slot?
2. **Sealing:** should opened regions' generated documents be kept in the save, so that a
   generator update does not refuse a live world?
3. **Helios:** should the three hand-written systems be a pack on by default, a pack the
   admin chooses, or removed?

(Answered on 2026-10-11: option B; a faction registry straight away.)

## How others do it

| Game | How it works | What we take |
|---|---|---|
| Elite Dangerous (Stellar Forge) | the whole galaxy from rules and a seed; only discoveries and changes saved; hand-made "bubbles" on top | our seed + log + changes. The lesson: emptiness scares more than repetition, so events must be dense |
| No Man's Sky | the universe exists whole and growth is discovery; settlements and frigates were added later because it was empty | native inhabitants from day one, not after complaints |
| EVE: wormhole space, Thera, Drifters | holes with a lifetime, mass and ship class; signature scanning; Thera as a hub; Drifter lore explains who opens holes | roaming holes almost one to one; a hub on the frontier later |
| Starbase | a hand-built world extended by developer patches | the counter-example: hand-made growth cannot keep up with players, and a capped generator solves it |
| X4 | factions build stations by shortage; the economy stalls without tuning | the outpost → market → station chain by need, capped per region |
| Stellaris | the galaxy exists up front, but L-gates and ancient gates open later and change the mid-game | the Forerunner lore as background, and the meeting of civilisations as a mid-game event |
