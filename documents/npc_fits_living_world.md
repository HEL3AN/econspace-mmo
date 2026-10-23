# NPC fits from the living world

Status: **decided, being built** (#464): slices 1 (variants), 2 (fleets) and 3 (region
style) are built. The owner's direction of 2026-10-23 is the premise and
is not re-opened: NPC fits differ **strongly** between regions; a region's style, politics and
economy decide what its NPCs fly; a region whose markets and industry are full of lasers flies
mostly lasers and rarely railguns. The owner answered every open question the same day
(section 9). No balance patches in the meantime -- starter systems are high-sec and have no
pirates. Related: #463 (one combat model), #458 (siege damage from warship designs), #431
slice 12 (corporation patrols), #343 / #407 / #409 / #418 / #420 (markets, haulers, convoys,
shortages), #472 (holds and storage; modules as items), #279 (designs).

Where this doc leaves a number free, it proposes one and marks it as tuning. Every tuning
number lives in data (`data/ships.json`, `data/world_rules.json`), never beside the rule.

## Why this exists

Since #463 an NPC fires its real fit, the same arithmetic a player's ship uses. That made the
fit matter, and showed how little the world decides it: `data/ships.json` names one design per
role (`default` doctrine: raider for pirates, cutter for patrols, cruiser for warships), and the
Syndicate's patrol list is the only exception. A pirate in a clan's frontier and a pirate next
to a centre are the same Raider with the same railgun. The owner wants the opposite: an NPC is
fitted with what its people have, buy and take, so travelling somewhere new means meeting
different ships, and reading a region's economy tells you what you will fight.

## The code today

- **A module is a fit line, not an item.** `Ships::ModulePart` (ships.json `modules`) has a
  mass, what it `provides` (damage, hull, thrust, cargo, mining) and a `cost` in goods. A
  `Design` is a frame, a bow / mids / stern of `Section`s and `fit` lines (`module`, `in`,
  `on`, `count`). `Ships::Derive` turns that into `Stats`. Nothing stores, carries or trades a
  module; a design's materials are paid for once, at refit, at the station market's prices
  (`DesignPrice`).
- **Two weapon families exist**: `weapons.laser` (damage 3, mass 2, Iron 4 + Crystal 3) and
  `weapons.railgun` (damage 6, mass 5, Iron 10 + Crystal 5). Their archetypes in
  `data/modules/weapons.json` are tagged `energy` and `kinetic`, but only the look reads the
  tags. **There are no damage types and no resistances**: a volley is one number.
- **Two defensive layers exist**: every ship's shields (60, regenerating 8 a second, the same
  for every ship) and its hull (frame, sections, `hull.armour` modules and `rules.baseHull`).
  Armour is hull today, not a layer of its own.
- **Doctrines**: `Catalogue::DesignsFor(faction, role)` -- the faction's list, else
  `default`'s. `Catalogue::Pick(faction, role, key)` picks from the list by the NPC's key at
  spawn, and a ship keeps its design for life.
- **Fighting cold and hot is one model** (#463, #186): `Ships::RoleFighter` averages dps and
  combat hull over the doctrine's list; `Simulation::FighterOf(f, role)` caches it per
  (faction, role). The cold pass (`Simulation_Agents.cpp`, pirates vs police / traders /
  miners / patrols), sieges (`Simulation_Sieges.cpp`, #458) and corporation patrols
  (`PatrolAsWarships`) all read it.
- **Strength is a float.** `SystemAggregate` holds `traders`, `miners`, `police`, `pirates`
  and `presence[faction]`, in ships, saved. A holding's presence regrows towards its
  temperament's `capacity` every faction step **for free**; pirates breed from unrest.
- **Factions have money and little to spend it on.** `FactionMind::treasury` collects the fee
  on every trade at the faction's markets and pays the supply contracts its markets post
  (#343, #407). `FactionMind::stock` (materials and credits as one number) pays for outposts,
  sieges, anchors and upgrades.
- **Markets**: one per system (`Market`), a stock of every good in `goods.json` with a price
  that follows the stock. Stations run recipes (`Steel`, `Circuits`, `Ammunition`,
  `RepairPaste`, ...) and use goods; haulers (#343) carry a good wherever the bid beats the ask
  by the haul margin. Every unit bought moves the price, so a demand is visible and haulers
  answer it.
- **Regions**: a frontier region's natives get a `theme` (colonists, clan, sect, automata,
  traders) from what the region is (graveyards, belts, ruins, lawlessness), and speak the
  culture of the same name. Majors have a culture in `factions.json` (`guild`, `syndicate`).
- **Corporation patrols** (#431 slice 12) already fly a design the corporation chose and pay
  for replacements at the local market's prices (`PatrolShipPrice`). That is the shape this
  doc gives every NPC.
- **Designs reach clients** either from ships.json (stock, both sides) or once per connection
  as a filed design (`DesignStore`, the `design` message).

## How others do it

| Game | What they do | What we take | What we skip |
|---|---|---|---|
| EVE Online | Each pirate faction has a fixed damage profile and resist profile (Guristas kinetic, Serpentis thermal/kinetic, Blood Raiders EM/thermal) and lives in fixed regions. Players fit resists for the region they rat in. Every turret deals some damage to every layer; resist holes are 0-60%, never 100%. | The **player loop**: knowing where you go tells you what to tank, and the region is the key. Moderate holes, never immunity. | The profile being a constant. Ours comes from the economy and moves. |
| Starsector | A faction's fleets are drawn from the blueprints it knows, weighted by a doctrine (size, aggression, quality). Ship quality comes from production: an Orbital Works and a nanoforge mean fewer defects; lose them and the fleets get worse. Energy weapons are strong against shields and weak against armour; kinetic the reverse; both always do damage. | **Production decides quality and mix**, and losing industry visibly degrades a faction's fleets. The shield/armour split of damage types. | A player-set doctrine screen for NPC factions (corporations already choose their patrol designs). |
| X4: Foundations | Ships are built at wharfs from real wares; missing hull or engine parts stall production, and a yard builds a cheaper ship when it lacks parts. | **Purchases are real demand**, and a shortage falls back to the cheaper fit instead of nothing. | Per-ware build queues and stalled factions; our presence must not freeze (the living-world rule). |
| Mount & Blade, Kenshi | Troops and gear by culture; a region's settlements decide what its recruits carry. | **Culture as the base** that the economy then bends. | Fixed troop trees. |

Sources: [EVE University, NPC damage types](https://wiki.eveuniversity.org/Npc_damage),
[Starsector: Blueprints, Doctrine, and Production](https://fractalsoftworks.com/2018/02/12/blueprints-doctrine-and-production),
[Starsector wiki: Orbital Works](https://starsector.wiki.gg/wiki/Orbital_Works),
[Starsector wiki: Pirates](https://starsector.wiki.gg/wiki/Pirates),
[Starsector wiki: Damage](https://starsector.wiki.gg/wiki/Damage),
X4 player reports on wharf shortages ([Steam discussion](https://steamcommunity.com/app/392160/discussions/0/3832042807505516096)).

## The mechanic in one paragraph

Every armed stock design comes in **variants**: one per weapon family and one per hull line,
derived from the stock design by data rules on both sides of the wire, so no new design has to
be written or sent. Each region has a **style**: a weighting over weapon families and hull
lines, **proportional** to what its markets hold and its industry makes, with its people's
culture as a modest prior. Every system keeps, per faction and role, a **fleet**: how many
ships of which design are in its presence. A faction grows its presence by **buying** ships at
the holding's market, from its treasury, in the style's proportions as far as the market has
the materials and the treasury the money -- money limits how many ships, not only how good.
The materials leave the market, prices rise, haulers bring more. Losses come off the fleet by
design; pirates add what they capture. Every fight -- hot, cold, siege, patrol -- reads the
real fleet. So a laser region flies lasers because lasers are what it makes, and if its
circuit industry is burnt out it drifts toward whatever it can still buy.

## 1. Region style

### What it is

A style is two small weight vectors:

- **weapon families**: one weight per family. Today `laser` and `railgun`; a family is a set
  of weapon modules, declared in ships.json `families` with the goods it is made of.
- **hull lines**: one weight per line. A line is a set of section substitutions, declared in
  ships.json `lines`: `light` (bow.needle, mid.slim, stern.twin), `heavy` (bow.wedge /
  bow.blunt, mid.belt, stern.row), `brawler` (bow.fang, mid.spine, stern.cluster) -- three to
  start (tuning). A line changes mass, hull and the silhouette, so a region also **looks**
  different -- which is what the generated look (M6, M8) is for.

Each weight is a share, the shares sum to 1.

### Where it comes from (decided, owner 2026-10-23, Q3)

**Proportional, like the real world, not a fixed exponent.** A region whose markets and
industry hold 100 units' worth of laser goods against 25 of railgun goods flies about 80/20;
one that holds as much of each flies 50/50. The difference between regions comes from their
economies differing.

`share_i = (1 - c) x supply_i / sum_j supply_j  +  c x culture_i`, per vector, where

- **supply** -- what the region has and makes, now. Each family names its goods in ships.json
  (laser `Crystal`, `Circuits`; railgun `Steel`, `Ammunition`); each hull line names its goods
  the same way (heavy `Steel`, `Iron`; light `Crystal`, `Fuel`; brawler `Iron`, `Ammunition`
  -- tuning). Over the region's systems with a market, `supply_i` is the stock of those goods
  plus what the region makes of them in `styles.hours` hours (raw sources and recipe outputs,
  `MarketRates`; 24 hours, tuning). It is a function of the markets, which are saved, so the
  style is **derived, never stored**.
- **culture** -- the region's people, a modest prior from data: `world_rules.json`
  `styles.cultures` maps a culture id to weights (clan railgun 3 / laser 1 and brawler; sect
  laser 3 / railgun 1 and heavy; automata laser 2 / railgun 1 and light; colonists, traders,
  guild and syndicate as data says -- tuning), normalised to shares. The region's culture is
  its natives' (their theme is their culture), else that of the faction that holds most of
  its systems, else none (an even prior). It is a pure function of the region's documents and
  the registry: not saved, no generator change.
- **c** -- the culture prior's weight, `styles.culture`, 0.25 (tuning). It is a weight, not a
  sharpness: c = 0 is the economy alone, and no exponent sharpens either term.
- A region with no market at all has no supply: its style is the culture prior alone.

### How it drifts

The style is recomputed every macro period from the markets, so it moves as fast as they do.
The **fleets** move slower: only new and refitted ships take the current style (section 2), so
a region that switched to railguns still flies a tail of lasers for a while. That inertia is
the point -- it is history the player can see, and a region stripped of its industry by a
siege or a blockade shows it in its ships some days later. Proposed half-life of a fleet's mix
under a changed style: about two world days (tuning, from the refit rate below).

Style is per **region** (a slot), because that is the unit a player learns and the unit a
culture lives in (decided, Q4). What a holding can actually buy is its **own** market's
(section 3), so there is local variation inside a region, never a different culture.

## 2. Where a fit is chosen, and how often

- **At purchase, in the faction step.** When a holding's presence grows, the growth is ships
  bought (section 3). They are bought in the style's proportions, among the variants of the
  faction's doctrine designs for that role, as far as the market and the treasury allow. The
  fleet gains those designs.
- **At refit, slowly.** Each period a holding refits a share of its fleet (proposed 5%,
  tuning) to the current style if the market has the parts and the treasury the money -- the
  price difference between the old and new fit. This is the drift.
- **At spawn, never.** The spawn director no longer chooses a design; it draws one **from the
  fleet**, weighted by count, keyed as today (the pick key), so the same sky comes back when a
  system warms. A hot ship that dies takes one off its own design's count.
- Pirates, police, traders and miners (the role populations, not faction presence) follow the
  same rule with the system's controller or the pirates as the buyer (section 5).

A **fleet** is the one new store: per system, per (faction, role), a map design id -> ships,
in `SystemAggregate::fleets` beside `presence` and `patrols`, saved with them. Why nothing
existing carries it: presence is deliberately one number per faction, and a doctrine is what a
faction *would* fly, not what it *has*; "captured gear and losses change it" needs a record of
what is there.

**The float stays the truth for "how many", the fleet says "which".** Every macro pass a fleet
is **reconciled** to the number it describes -- scaled, design by design, so its total equals
it -- and a fleet that is empty while its number is not is seeded from the doctrine (its
designs in equal shares, as `Pick` spreads them today). That one rule makes every existing
change of a count correct without touching it: a cold loss, a migration, a lent siege, a
regrowth in today's stock mix are all proportional, so they leave the shares alone. Only what
knows *which* ship -- a hot death, a purchase, a capture -- edits a design's count, after
reconciling. The numbers fleets describe:

| Fleet | Its number |
|---|---|
| (pirates, `pirate`) | `pirates` |
| (the police faction, `patrol`) | `police` |
| (the trade faction, `trader`) | `traders` |
| (unaffiliated, `miner`) | `miners` |
| (each faction with presence here, `warship`) | `presence[faction]`: what it would lend a siege |

A fleet whose number is zero is dropped; it is seeded again when ships come back.

Variants are derived **in the catalogue**: `Catalogue::Variant(design, family, line)` replaces
the design's weapon modules with the family's (count kept, the module's own drawing size) and
its sections with the line's, each position falling back to the next candidate and then to
the design's own section where a substitution would not validate, and validates that the
result derives. Ids are stable strings (`cutter~railgun`, `cutter~laser~heavy`); the variant
with the design's own family and hull *is* the stock design and keeps its id. Both sides load
ships.json, so a client knows and draws every variant without a `design` message, exactly as
it knows a stock design. A variant that does not derive is a **load error**, like an unknown
form.

## 3. Cost and economy

- **Module costs say what a family is made of** (decided, Q7). Today every weapon costs Iron
  and Crystal, so no market can favour one. Laser `Crystal 2, Circuits 1`; railgun `Steel 3,
  Ammunition 1` (tuning). This changes `DesignPrice` for players too, which is right: one
  economy, one price. It is a data change, made in slice 4.
- **Who pays**: the faction's `treasury` (credits), at the holding market's **ask** for every
  good the design's frame, sections and fit lines cost, and the goods leave that market's
  stock (`Market::Buy`). `stock` (expansion money) is untouched.
- **Money limits the number of ships** (decided, owner 2026-10-23, Q2): a holding's growth is
  what it buys. `p += growth x (keep - p)` becomes "want that many ships, buy as many as the
  treasury and the market allow". The owner's reasoning: stepping away from the living world
  now and coming back later is harder than building it once. The failure to design against is
  **a poor faction quietly emptying half the map**, and the answer is the world's own
  feedback, never a balance patch or a free floor:
  - **Income refills the treasury.** The trade fee and the contracts already flow; a holding
    also pays its holder a tax on what its market makes and moves, `styles.holdingIncome`
    credits per prosperity-hour (tuning) -- the counterpart of what a corporation's holding
    pays it (#431 T4). Money comes from holding space, so a faction that holds space can always
    rebuild some of it.
  - **The stock design is the cheapest fallback.** A purchase falls back from the style's
    variant to the doctrine's stock design before it gives up, so a poor holding thins and
    flies plain ships rather than none.
  - **A weakened faction turns defensive.** A faction whose holdings stand below
    `styles.defensiveBelow` of what they keep (0.6, tuning) plans no new outposts and no
    sieges -- its `stock` waits -- and its purchases go to its weakest holdings first.
  - **Losses raise local prices, which pull haulers in.** Every purchase is a real `Buy`; a
    holding that keeps losing ships keeps buying, its goods' prices rise, and haulers (#343)
    and the faction's own supply contracts answer.
  - **Tested over the long run**: a macro test and a bench run many simulated days on several
    seeds and assert that no faction's fleet collapses to zero absent a real defeat (losing
    its holdings), and the PR reports the curves.
- **Shortage**: buy family by family in the style's order of weight. If the preferred
  family's goods are out of stock or too dear (price over `styles.dearAt` x base, 2, tuning),
  buy the next family; then a lighter line; then the doctrine's stock design at whatever is
  available. If nothing can be bought, that period's growth waits for the next.
- **Losses** come off the fleet by design. Cold, losses are taken proportionally across the
  fleet's designs; hot, by the ship that died.
- **Captured gear**: pirates who win a cold exchange add a share of what they destroyed to
  their own fleet, in the victims' family (proposed: one captured ship-equivalent per four
  kills, tuning) -- they refit with what they took. That is how a pirate gang in a laser region
  ends up with lasers even without a market. Hot, a wreck keeps its loot as today; once
  modules are items (#472) a wreck drops its weapons and anyone, NPC or pilot, can take them.
- **Modules as goods later (#472)**: if modules become items, `supply` reads the stock of the
  modules themselves instead of their key goods, and the purchase takes modules off the
  market. The formula has one input to swap; nothing else changes.

## 4. A fleet as concrete designs, and how fights read it

- `Ships::Fighter FleetFighter(fleet)`: the count-weighted mean of `DesignFighter` over the
  fleet. The per-design numbers are cached by design id, invalidated by
  `Archetypes::Generation()` as now.
- `Simulation::FighterOf(f, role)` stays the doctrine's average; `FighterIn(const
  SystemState&, f, role)` is the fleet's fighter there, falling back to `FighterOf` for an
  empty fleet. Every caller already has the system in hand: the cold pass,
  `PatrolAsWarships`, siege attrition.
- **Sieges** (#458): when a siege sets out, each contingent's fighter (dps, hull) is taken
  from its holding's warship fleet and the plan keeps the force-weighted mean in two saved
  floats. A siege from a laser holding takes a wall down at the laser fleet's rate. The ships
  it lent are taken off that holding's presence (and so, proportionally, its fleet) and given
  back with a Return.
- Hot fights need nothing new: every NPC already fights with its own design (#463).

## 5. Not only pirates

| Who | Buyer | Base style | Notes |
|---|---|---|---|
| A major's warships and police in its holdings | the faction, at the holding's market | the region's (its culture prior where it holds the region) | a major's police in a native region slowly takes the region's fit |
| Natives | the natives | their culture | their culture *is* the region's prior |
| Pirates | the pirates, at markets that will deal with them (a lawless held system, a clan's) | the region's | plus captured gear; no market at all means captures and the stock raider |
| Corporation patrols | the corporation | none | unchanged: the corporation chooses (#431 slice 12) |
| Traders and haulers | the controller (a lawful one), else Independent | hull lines only | the cargo line is the region's line; an armed escort variant where security is low |
| Miners | Independent | hull lines only | barge variants by line |
| Convoy escorts (#407) | the faction | as its warships | the escort's fighter is the holding's warship fleet's |

NPCs do not fly player designs for now (decided, Q6); revisit with #472 when modules and hulls
become goods players make.

## 6. What the player sees and can exploit

- **The ship says it.** The overview, target panel and sensor view show an NPC's design name,
  which includes the variant ("Cutter (railguns, heavy)"). The look shows it too: lens colours
  on a laser mount, long rails on a railgun, the line's silhouette. A pilot learns a region by
  sight.
- **The region says it -- only through what the pilot knows** (decided, Q5). The system/region
  info a pilot already has for **known** systems (#431 knowledge) gains one line: "Local fits:
  lasers 80%, railguns 20%; heavy hulls". It is what the pilot's charts know -- as old as the
  knowledge, and carried by charts they bought (#431 slice 16) -- not the live truth. A region's
  fit becomes part of what a chart is worth.
- **The market says it.** A laser region's Crystal and Circuits sit high on the price list
  because its factions keep buying them. A trader who brings Circuits into a laser region sells
  well (the bid is high) and **feeds** its fleets; a pilot who wants that region weaker sells
  elsewhere, raids its haulers, or besieges its circuit industry and watches the style turn.
- **The fit answers it.** With damage types and resistances (section 10) it becomes the EVE
  loop: fit against the region's family. Before them the families differ in damage per slot and
  in mass (lasers light and fast, railguns heavy and hard-hitting).
- **Agents** read all of it through `econagent` (section 7), so an agent can plan a trade or a
  fit from the same signals.

## 7. Save, protocol, agents, tests

- **World save**: `SystemAggregate::fleets` (per faction and role, design id -> ships) and a
  siege plan's two fighter floats. Both are additions an older reader ignores, so no
  `Save::WORLD_VERSION` bump is required; a save without fleets is seeded from the doctrine by
  the first reconcile -- the world it describes is exactly today's. A fleet naming a design the
  catalogue no longer has (a variant rule changed) folds into the role's stock design,
  counted, never dropped.
- **Generator**: unchanged. Style is derived from documents and markets.
- **Protocol**: NPC design ids already travel (`design` string on an NPC). Variant ids are
  derived on both sides, so no `design` message is needed. The region-style line is a new
  field on the system info a pilot gets for known systems: `PROTO_VERSION` bump in that slice.
- **Agents**: `observe` already names an NPC's design; it gains the system's style for known
  systems (same rule as the client), and the market view is unchanged. Regenerate the agent
  reference.
- **Tests** (doctest, `tests/`):
  - every variant of every armed stock design derives; an underivable variant rule fails the
    load; a variant's id and stats are the same on every load;
  - style: proportional -- 100 against 25 gives 80/20 with c = 0, an even supply gives the
    culture prior's lean scaled by c; shares sum to 1; deterministic for the same markets;
  - buying: stock leaves the market, the treasury pays the ask, the fleet gains the designs;
    shortage falls back family -> line -> stock design -> nothing; an empty treasury buys
    nothing;
  - the long run: many world days on several seeds, no faction's ships reach zero without
    losing its holdings;
  - `FleetFighter` is the weighted mean; an empty fleet falls back to the doctrine;
  - cold exchange: losses come off proportionally, captures add the victim's family;
  - siege fighter taken at launch and given back with the Return;
  - save round-trip of fleets, and a load without them.

## 8. Slice plan

One PR per slice, each from `main`, each leaving the game working.

1. **Variants in the catalogue.** Weapon families and hull lines in ships.json,
   `Catalogue::Variant`, load-time validation, every variant drawn on both sides. No behaviour
   change.
2. **Fleets.** `SystemAggregate::fleets`, reconciled and seeded from today's doctrine, saved;
   the spawn director draws from the fleet; hot losses come off it. `FighterIn` reads it in the
   cold pass, patrols and sieges (siege fighter stored on the plan). Same numbers as today,
   since every fleet is the doctrine's mix. `MacroBench` before and after.
3. **Region style.** Culture prior from `world_rules.json` `styles`, supply from markets,
   proportional shares. Computed and logged (server log, `worldtest`); not yet used.
4. **Buying.** Growth becomes purchases at the holding's market from the treasury, in the
   style's proportions, with the shortage fallback; money limits the count; holding income, the
   defensive turn and the long-run test; module costs move to family goods (data). This is
   where regions start to differ.
5. **Refit and drift.** The periodic refit share; benchmark the drift half-life.
6. **Pirates and captures.** Pirate buying at markets that deal with them; captures in the
   cold pass.
7. **What the player and agents see.** Variant names in the instruments, the region-style
   line for known systems, `observe`; `PROTO_VERSION` bump. Optionally, a faction short of a
   family's goods posts a supply job (#345) -- the jobs system already exists.
8. **Damage types and resistances** (section 10), its own issue under the one combat model,
   for players and NPCs alike. It reads only `Fighter` and the families, so it may move ahead
   of 5-7 if the owner wants the fit loop sooner.

## 9. Decided (owner, 2026-10-23)

1. **Damage types and resistances: yes**, for players and NPCs alike, as its own later slice
   (section 10). Hard rule: **no weapon is useless against any layer.** Every weapon always
   damages shield, armour and hull; a family is only better against one layer and worse
   against another, with a moderate spread.
2. **Money limits the number of ships**, built now, not later -- against the failure mode of
   a poor faction emptying the map, answered by the world's own feedback (section 3) and a
   long-run test.
3. **No artificial sharpening.** The style is proportional to the region's real supply, with
   culture as a modest prior (section 1). A knob for the prior's weight stays; there is no
   sharpness exponent.
4. **Style per region**, buying on the holding's market.
5. **Players see a region's style only through their own knowledge** and the charts they
   bought.
6. **NPCs do not fly player designs** for now; revisit with #472.
7. **Module prices come from the family's goods** -- lasers from Crystal and Circuits,
   railguns from Steel and Ammunition -- one economy for players and NPCs.

## 10. Damage types and resistances (slice 8)

### The rule

Every weapon damages every layer. A family is **better** against one layer and **worse**
against another; a mismatched gun is weaker, never pointless. Proposed spread: the best
pairing 1.25, the worst 0.75, and **no effective multiplier below `minEffect` = 0.6** after
every modifier, enforced at load (an entry that could fall below it is a load error, not a
clamp nobody sees).

### Layers

A ship has three layers, hit in order: **shield** (every ship's today, regenerating), **armour**
(new: what `hull.armour` modules provide, moved out of `hull`, so a ship's total is unchanged),
**hull** (frame, sections, `rules.baseHull`). Damage that breaks a layer carries on into the
next at the next layer's multiplier. Armour is repaired with the hull.

### Profiles, as data

- **Damage profile per family**, ships.json `families.<id>.damage`: shares over damage types,
  summing to 1. Laser `{ energy: 1 }`, railgun `{ kinetic: 1 }`; a later family may mix
  (`{ energy: 0.5, kinetic: 0.5 }`). The base gun every ship has (`rules.baseGun`) is
  **untyped**: 1.0 against every layer, which further keeps a mismatched ship useful.
- **Resistance profile per layer**, ships.json `layers.<layer>`: the multiplier each damage
  type does to it.

  | | energy | kinetic |
  |---|---|---|
  | shield | 1.25 | 0.75 |
  | armour | 0.75 | 1.25 |
  | hull | 1.0 | 1.0 |

- **Per hull line**, `lines.<id>.resist`: a modest shift on top (heavy armour resists kinetic
  a little, light shields resist energy a little, brawler none --
  tuning). As multipliers: heavy `{ armour: { kinetic: 0.9 } }`, light
  `{ shield: { energy: 0.9 } }`. Later, modules may add their own (a shield hardener, reactive plating) through the
  same table.
- **Effective multiplier** of a volley on a layer: `sum_type share_type x layer[type] x
  line[layer][type]`, checked `>= minEffect` for every family x layer x line at load.

### Where it is read

- **Hot**: `Ship::TakeDamage` and `NpcShip::TakeDamage` take the volley with its profile
  (fired from `Stats`), shields first. One function in `engine` computes the split, read by
  the server and by the client's damage numbers -- never two copies.
- **Cold**: `Ships::Fighter` gains per-type dps and per-layer pools; the cold pass's bite of
  A on B becomes `1 / sum_layer (pool_l / effective_dps_l)` instead of `dps / hull` -- the
  same arithmetic as the hot fight, so a laser region's police kill shield-heavy pirates
  faster, cold or hot.
- **Sieges**: a structure's layers get a profile too (walls are armour-heavy: railguns
  besiege better), from its blueprint.
- **Player**: the target panel shows shield / armour / hull bars; the Fit tab and
  `preview_fit` show the fit's damage profile and the hull's resistances; `observe` gives an
  NPC's design (from which its profile is known) and a target's three layers. `PROTO_VERSION`
  bump (the snapshot carries armour). No save change: a ship's layers are repaired to full.
