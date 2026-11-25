# Mining and resource fields: many kinds, born from the world's own events

Status: **slices 1 and 2 built** (a field with a life; the event engine with Impact and
Collision, section 9); the rest is design (refs #617; the
owner's vision is in the issue, and the owner accepted every recommendation in section 10). It
proposes how a resource field comes to exist, what kinds of resource there are and how each is
gathered, how all of it stays inside the living economy (#584, `economy_scales.md`), how it
looks in the generated art (M6), and a slice plan. The forks that change the game's feel are
at the end, in "Decisions for the owner", each with a recommendation.

## 1. Where mining stands today

What the code does now, so the design extends it instead of standing beside it.

- **A belt is a static.** `AsteroidField` (an `Entity` of kind `Field`) holds one ore, `oreMax`
  and `oreRemaining`, and the archetype `field.asteroid` / `field.rich` / `field.motherlode`
  gives its look and `mineable { extractRate, range }`. A belt is placed by the generator
  (`gen/Region.cpp`: a ring round a gas giant, a gap-remnant, a cluster) with an `ore` size of
  200-450 (+40 per depth, more for a cluster and a rich find), and R3 (`Gen::OreModel`,
  `SizeOreToNeed`) retypes and grows belts until the region's belts give what its stations and
  fleets need, times the margin.
- **It never ends.** `Regrow` adds `beltRegrow * oreMax` an hour (a tenth of full), forever;
  the system's miners take `beltMined` (0.15) of what is left, so a belt settles at two thirds
  and yields exactly a tenth of its size an hour *whatever the miners do*. The belt is a
  constant spring, which is the dullness the issue names: nothing about where to mine ever
  changes.
- **One gathering mechanic.** A ship with `provides.mining` (the tractor beam, the hammerhead
  bow, a hull's own) turns the beam on near a `Mineable` and `StepPlayerMining` pulls one unit
  per `miningRate * extractRate * skill * security` per second into the hold. Mined goods
  (`GoodDef::mined`: Iron, Ice, Crystal) may fill the ore hold (`provides.ore`, the hopper mid
  section, #572); nothing else about the three differs but their volume and price.
- **Hidden things have a signature** (#346): an archetype `signature { strength }` hides the
  object per pilot until a scan resolves it; the find is the account's. `field.rich` already
  uses it (0.35).
- **Statics change only through `Simulation`** (`AddStatic`, `RemoveStatic`,
  `MarkStaticChanged`, a `LayoutDelta` before the snapshot, `changes` in `world.json`); a belt's
  ore reaches clients as `BeltOre` in every snapshot, not in the layout.
- **Timed things have a pattern already.** Roaming holes (`Simulation_Holes.cpp`) are decided
  per epoch from `Key(seed, epoch, slot)`, *then kept*; wrecks and structures sit on a due-queue
  (`StructureSchedule`); orbits are `f(world time)` (#210); the news is `Announce`/`Record`.
- **Gas exists only as a planet's output.** `planet.gas` gives Fuel in the market step; there
  is nothing to fly to and harvest. Nebulae are decoration with a `hazard` (`hidesShips`).
- **Stars already flare.** `nature.flare`, `nature.prominence` and friends are star modules in
  the grammar; they are cosmetic and have no consequence yet.

## 2. What the comparable games do, and what matters here

| Game | What it does | What we take, and what we leave |
|---|---|---|
| **EVE Online** | Belts are permanent and respawn at downtime. On top: **ore anomalies** (visible on probing-free overview, a few days' timer or until mined out, richer ore than belts, may attract rats), **cosmic signatures** (hidden until scanned: gas sites, relic/data sites, wormhole-space ore), **ice** belts that respawn some hours after being mined out, and **moon mining** (a structure drills a moon, a chunk is delivered at a time and decays after about two days). Ore tiers and security class decide richness; gas is its own site type with its own harvester module and hold usage. | The split between *visible and scanned* sources, finite sites with a timer, one harvester per family of matter, sites that draw pirates, and moon chunks that expire. Leave the respawn-at-downtime feel: it is the static answer this issue complains about. |
| **Elite Dangerous** | Rings are generated per body; **hotspots** are persistent spots that raise the odds of a mineral; **core** asteroids are cracked with a seismic charge and pay in bursts; surface **geological sites** need a scanner and a vehicle; sources are found with a prospector limpet or a pulse analyser, so *reading the field* is the skill. Everything is regenerated, nothing is finite. | Reading a field (what is rich, where) as the gameplay; a field with a *structure* (a core, a hotspot). Leave persistence: ED's hotspots never move, so mining there is solved once. |
| **Star Citizen** | Rocks have a composition and mass; a scan ping shows signatures with colour-coded value; the laser has a window of power and instability, so a rock is a small skill game. Gas and quantum-mineable clouds in newer systems. Fields are fixed per body; yield depends on the rock, not the world. | Value shown as a signature before you commit; the per-rock composition; a harvest that *depends on density*. Leave the per-rock minigame: this is a 2D MMO with agents as players. |
| **X4: Foundations** | Resources sit in sector fields with a density, and **regenerate** at a per-field rate; probes reveal them; NPC miners deplete them too, so a region's supply is *a shared rate* and prices follow it; separate gas collectors and ship classes (miners for solids, collectors for gas) with their own holds. | The closest to our economy: a field with an explicit regeneration number, NPC and player depletion in one stock, and separate hardware for gas. Its weakness is the one we have: fields never change location. |
| **No Man's Sky** | Resources are in the world as hand-placed variety (plants, minerals, gas, asteroids with rare cores), found by scanning; *events* (a storm, a comet strike, a derelict) are mostly spectacle with a small reward. | An event the player is *there to see* with a reward attached. Leave "event is only a cutscene": ours must move supply. |
| **Space Engineers** | Asteroids are finite voxels in generated fields, ore found with a detector, drilled by hand-built ships; nothing respawns, so players move on and *the map is consumed*. | A field that is finite and consumed makes "where next" a real question. The cost: a world with no inflow dies, which is why ours must have a ledger (section 4). |

What they share and we are missing: **fields that end, sources that are *read* before they
are worked, and different matter taking different tools**. What none of them has is a *cause*:
in every one of these a field is placed by a table. Ours should be placed by something that
happened, which the pilot can see, read in the news, and use to guess where the next one will
be.

## 3. The event model

### 3.1 Vocabulary

A **phenomenon** is a world process that leaves a resource field behind. It is data
(`data/events.json`, a registry like `factions.json`; a kind with an unknown field is a load
error, #191), not code. Each has a *cause* the pilot can name, a *place* it needs, a *yield*,
a *life*, a *look* and a *line* for the news.

| Kind | Cause (the cause the pilot reads) | Needs in the system | Leaves | Moves | Life |
|---|---|---|---|---|---|
| **Impact** | A stream of rock meets a planet (the owner's example) | a planet | a debris arc on the planet's orbit: rock (Iron, Crystal) | with the planet, spreading along the orbit | 2-4 days |
| **Comet pass** | A comet falls past the star and sheds | a star | a trail of ice and volatiles, a long streak | on its path, then out of the system | 6-12 hours |
| **Moon breakup** | A moon inside the Roche limit of a gas giant | a gas giant with a moon | a ring of rock and ice, large and rich | in the giant's orbit | 5-10 days |
| **Collision** | Two asteroids of one belt meet | a belt | fresh fragments, richer than the belt (a thin share of the belt's ore, moved) | slow drift off the belt | 12-24 hours |
| **Flare** | A star's flare ionises a nebula | a star and a nebula | an ionised gas cloud over the nebula: gas, densest at a core | the cloud disperses | 4-8 hours |
| **Graveyard** | A battle or siege that destroyed ships | a siege's or fight's site | scrap: what the hulls were made of, salvaged | none | 1-3 days |
| **Vent** | An ice moon's geyser, periodic, *not* an event but a rhythm | an ice-bearing body | puffs of ice on a fixed cycle `f(time, seed)` | none | permanent, cycling |

Impact, Comet, Collision and Flare are the first four; Breakup and Graveyard come later (they
need the heavier rules in section 6). Vent is the steady, predictable alternative in the
family: it gives a player a place they can plan a day around, so that not everything churns.

The list is a vocabulary, not a closed set: a new process is a new entry in `events.json`
plus, at most, an archetype and a module in the art. The code names no kind.

### 3.2 Scheduling: decided per epoch, then kept (as roaming holes are)

The roaming hole is the precedent, and the lesson from it is that *"that is decided once and
kept"*: rules in `world_rules.json` and `events.json` will be tuned after worlds exist, and a
field must not change its mind. So:

1. **Epoch.** `world_rules.json` `events.epochHours` (suggest 1 hour). When an epoch begins the
   server asks, for each live region, what happens in it, from
   `Gen::Rng(Key(seed, EVENT_KEY, epoch, slot))` -- keyed by *what* (#140): the epoch and the
   slot, never the order regions were opened. No unordered containers, no transcendental
   functions in the choice.
2. **Ledger decides how much** (section 4): the region's **ore bank** says how much ore the
   epoch may place. If the bank is empty nothing happens, which is as it should be.
3. **Pick a kind and a place.** Kinds are weighted by what the region has (an Impact needs a
   planet; a Flare needs a nebula and a star; a Collision a belt), by `depth`, and by the
   region's class (centres have less of the wild kinds). The system is weighted towards ones
   that **have a market or touch one by a gate**, so the economy's own miners and haulers can
   reach the ore (section 4.3), with a share left for the frontier.
4. **Fix the whole phenomenon once**: kind, system, `t0` (inside the epoch, so events do not
   all fall on the hour), the impact point or path, the yield's size and composition, the
   life, a `seed` for the art. This record is **written down** (`world.json`, a `fields` log
   beside the opening log, bounded by what is alive) and a due-queue entry is put on a
   `StructureSchedule`-like queue for `t0` and for each later phase boundary and the end. The
   queue is why a 1,500-system world does no per-tick scan (#295).
5. **At `t0` the server makes the field.** `AddStatic` of an `AsteroidField` (the existing
   entity grown, section 5.1), `LayoutDelta` before the next snapshot if the system is hot,
   nothing if cold; the news line; the journal line for anyone who has a stake. Persisted
   under the usual `changes` key `+N` (#286), together with `taken` (what has been mined).
   At the end the due-queue calls `RemoveStatic`.

**New regions are alive from the first minute** (#342 premise): when a region opens,
`Opening` pre-rolls the last `preRollEpochs` epochs for it with the same keys, so a pilot who
enters it finds fields at every stage of life, not a clean slate. That is part of the sealed
region's opening (the pre-rolled phenomena go into the fields log), so older regions keep what
they have and only later ones change; a change to the *rules* is a `GENERATOR_VERSION` bump
for the base belts, and a `events.json` hash note in `world.json` for the rest, as for admin
packs.

**Not f(time) all the way, and why.** The owner's constraint (#136) is that anything that moves
is `f(time, seed)`. A field is a server static, so what *exists* and its ore is server state
and arrives by `LayoutDelta`/`BeltOre`. What *moves* is f(time): a debris arc rides its
planet's orbit (a satellite, `Orbits::Place`); a comet follows `p0 + v * (now - t0)` plus a
bend, wrapped in the same way as turns are; a cloud's drift is a tiny vector times its age.
Nothing is integrated per tick, and nothing about motion crosses the wire except `t0`, the
start point, the velocity and the seed, once. The *age curve of the ore* (below) is f(age)
too, so the server and the client agree without messages.

### 3.3 How a player sees it happen

An event has up to three beats, each cheap and each only if somebody is there:

1. **Omen** (minutes before `t0`, for kinds that have one): the thing itself is the static,
   already placed and not yet mineable (`phase = arriving`). A comet drawn falling in on its
   path, a moon's tidal shreds, a star's `nature.flare` knot brightening on the limb that
   will ionise the cloud. A pilot already in the system sees it coming and can park where it
   will happen. Because the omen is f(time) from the layout, a pilot who enters at minute 3
   of 5 sees minute 3.
2. **The moment** (`t0`): a flash (an item `blink`, a `nature.flare`-style soft disc, a second
   long), the field's phase becomes *bloom*, the news line goes out. Only a hot system draws
   it; a cold one does the same by numbers.
3. **The aftermath**: the field's look follows its age (section 7).

**The line** is a `Record`/`Announce` kind `field` with the system, cut per pilot like the
map is (#431): you read the news of systems *you know* and the map shows a mark there; an
unknown system's event is not leaked on a public feed. It names the cause, never the number:
"A stream of rock has struck the third planet of Ithar; debris is spreading along its orbit."
A pilot present at the moment gets a journal entry of their own ("witnessed"), and econagent's
`observe` carries the same text and the field's kind, richness class and age, so an agent
sees what a pilot sees.

**Signature or visible, by size** (decision D2). A large phenomenon is *public*: its field is a
visible archetype that everyone in the system sees and the overview lists. A small one is a
*signature*: its field uses an archetype with `signature { strength }` (the same component the
hidden base and `field.rich` use), is left out of a pilot's layout until a scan resolves it
(#346), and the find is the account's. The size class is decided at the phenomenon's creation
from its share of the bank, so a pilot who wants the small, quiet, worthwhile ones has a
reason to carry a probe.

### 3.4 Ageing: rich, thinning, drifting, gone

A field has a **cap** that follows a curve of its age, and the ore it holds is what is left of
the cap after what was taken:

```
cap(age)   = oreMax * curve(age / life)            // rises, plateaus, then falls to nothing
ore(now)   = max(0, cap(age) - taken)              // `taken` is only ever added to
```

| Phase | Share of life | Cap | What it is | Look |
|---|---|---|---|---|
| **Arriving** | omen, before `t0` | 0 | the thing is coming; nothing to mine | the moving body |
| **Bloom** | 0-10% | 0.4 -> 1.0 | debris cooling and spreading; the field is still compact | bright, dense, compact |
| **Rich** | 10-35% | 1.0 | the point to be there | full colour |
| **Thinning** | 35-80% | 1.0 -> 0.25 | dispersal: ore is lost to space whether mined or not | wider, paler (`intensity` already does this for a belt) |
| **Scattered** | 80-100% | 0.25 -> 0 | too thin to chase; NPCs leave; a rough place to find scraps | sparse, drifting parts |
| **Gone** | end | -- | `RemoveStatic` | -- |

Because `taken` is added to and `cap` falls, an unmined field **loses ore to dispersal**: ore
leaves the world without having been sold, which is a *sink* and so never a mint (section 4).
Mining speeds up with density too: the yield per second is
`miningRate * extractRate * (0.3 + 0.7 * ore / cap)`, so a thinning field is slower and a
pilot feels it before it is gone, and *where to mine is a question each time*.

The curve and the life are per kind in `events.json`; a belt is the special case with no
`life` and a `regrow` instead (the existing behaviour, unchanged: the steady base).

**Drift.** A field has a velocity (a few units a second at most, a debris arc follows its
planet) so that over its life it covers its own width a few times. The mining range is
`size + extractRange` today; a field's size grows during Bloom and Thinning with the same
curve, so "stay at the range" stays true without chasing.

## 4. Coupling to the living economy: nothing from nothing

This is the part that can break the game quietly, so it is a rule, not a hope.

### 4.1 A ledger per region, the ore bank

`economy_scales.md` fixed how much ore a region *needs*: `Gen::OreModel` turns its stations'
`uses`, its holdings' fleet turnover and the margin into units an hour per ore
(`regrowPermille`, `marginPermille`, R3, GENERATOR_VERSION 14). The steady belts deliver
that today. The design **keeps that flow and splits it**:

```
need[ore]      = what R3 already computes  (units / hour, with the margin)
base[ore]      = (1 - eventShare) * need[ore]   // steady belts, sized by R3 as before, but to this share
bank[ore]     += eventShare * need[ore] * hours // accrued continuously, capped at bankHours of flow
event size    <= bank[ore]                       // an event withdraws what it places
```

- A region's event fields can **never** hold more ore than the region's bank has accrued: the
  world spends only what the demand-derived flow earned. `eventShare` and `bankHours` are
  `world_rules.json` (`events`), the same place as the margin (#463: numbers from the world).
- **The total is conserved** in expectation: belts carry `1 - eventShare` of the need, the
  bank carries `eventShare`. Whether it is delivered as a steady belt or as bursts, the
  supply *per day* is the same, so the market sees bursts and prices swing, not starves.
- **Bank is saved** (`world.json`, a number per ore per region) and **capped** (`bankHours`,
  suggest 48): unspent bank does not pile up into one giant event; a quiet week does not
  produce a week of ore at once. What is capped away is lost, which is again a sink.
- **Frontier** regions have a different source: their *unclaimed* share (R3: richer the
  further out, because the player is the demand there). `eventShare` rises with depth
  (suggest 0.4 in a centre, 0.7 in a frontier), and a frontier's bank accrues from a
  `frontierFlow` per region that is a rule and a number in the file, never a hidden multiplier.
  A field in a frontier with no markets is only a player's.
- **Salvage kinds draw on real losses.** A Graveyard is not paid from the bank at all: its
  scrap is *the hulls that died*, in goods, from `Ships::Stats::cost` of the destroyed designs
  at a recovery share (`salvageShare`, suggest 0.4); a siege that destroys forty hulls leaves
  forty hulls' worth. That is the closest the design gets to "the world's own events" in the
  sense of #463, and it costs no new balance.

### 4.2 Collisions and impacts move ore, they do not make it

A **Collision** is paid from the *belt it happens at*: it moves a share of that belt's `ore`
(e.g. 15%) into the new fragments (richer, `extractRate` higher, a different, rarer ore if the
belt is a rich one), so the belt thins and the fragments are rich. Net zero. An **Impact**'s
rock is paid from the bank (the stream came from somewhere unseen, which is the bank's meaning).

### 4.3 NPC miners and the cold coarse pass

- **Today's rule is wrong for finite fields.** `StepMarket` takes `beltMined * crew` of the
  *remaining* ore of every belt each pass, which is right only because a belt's steady state
  caps it. On a fresh 3,000-ore field, 15% an hour is 450 an hour for two miners, far more
  than two ships can carry. The rule for a finite field is **absolute**: a crew takes at most
  `crew * minerRate * hours`, where `minerRate` is the `miner` design's `miningRate` from
  `Ships::Stats` (the same design, the same number, as for the fleets: one combat and one
  mining model for NPCs and players, #463), and never below the field's `ore` left. A field
  with `life > 0` uses this rule; a belt keeps the old one.
- **They work what pays and what is near.** NPC crews go only to fields **in their own
  system** (a miner needs a belt in its system today, `Simulation_Agents.cpp`), and **only
  while the market's shelf for that ore is under its cap** -- the "miners idle at a full
  shelf" that `economy_scales.md` said follows from R1. A field in a system with no market,
  or an ore the system does not need, is **left for players**. That keeps the frontier's
  fields a player's, and means a big field is not wasted on a full shelf.
- **Hot and cold are the same.** A phenomenon is a static *and* a due-queue entry, and the
  market pass already walks every system's entities hot or cold; an event in a cold system is
  numbers: the static is added, the news is `Record`ed, and no snapshot is built. A system
  that turns hot after the event finds the field as it is by then (aged, partly taken), the way
  it finds its ships (`Warm`). The coarse pass needs no new function, only the absolute-rate
  branch above.
- **Pirates are drawn to what is rich** (existing `security.oreAtZero` pays more where law is
  absent): a rich new field in a lawless or low system spawns, through the current spawn
  director, more `Pirate` presence for its life, because the world's value there went up. No
  new NPC role.
- **Haulers**: unchanged. They carry between adjacent markets when prices differ by the
  margin, and an event's burst *creates* exactly the gradient that makes them move: an Iron
  glut at one end is a reason to haul. That is the economy responding, for free.

### 4.4 What to measure (a bench, not a belief)

`econserver fleetbench` seeds 1-3, 7 days, with the flow tally from `economy_scales.md`:
**mined / wanted per ore per region class** stays at or above 1 in every class (bursts
averaged), **lost at cap** under 5% of mined, **dispersed** (ore that left unmined) reported as
its own line and under a quarter of event ore, no market dry of Iron for more than a day, and a
**fields alive** count per region so the world is never empty of them. These are the pass
criteria of slice 2, as R1-R3 had theirs.

## 5. Resource kinds and how each is gathered

### 5.1 One entity, grown

`AsteroidField` stays the one entity for every resource field (it is a "belt" only by name).
It grows: `bornAt`, `life`, a `curve` id, a `velocity`, `taken`, and a `phase`, with the ore it
holds computed as in 3.4; a belt is `life == 0`. It does not become a new type: a new type
would need a layout kind, a snapshot kind, a save format and a client path, and nothing about
a gas cloud needs them. The *kind of matter* is not in the class, it is in the archetype's
`mineable` component, which grows from `{ extractRate, range }` to also name `harvest`, the
gathering method (`"beam"` today, `"siphon"` for gas, `"cutter"` for ice if we split it).

### 5.2 The kinds

| Matter | Source | Harvest | Hold | Yield | Notes |
|---|---|---|---|---|---|
| **Rock ore** (Iron, Crystal) | belts, impact and breakup debris, collision fragments | **beam** (`provides.mining`: tractor, hammerhead; as today) | the ore hold (`provides.ore`) or the general hold | `rate * (0.3 + 0.7 * density)`, rock after rock | unchanged mechanic; fresh fragments get a higher `extractRate` |
| **Ice** | ice belts, comet trails, vents | beam, but ice is harder to cut: `extractRate` lower, `volume` the same | ore hold | like rock; a comet's ice also **sublimates**: its cap falls faster than rock's | Ice is already `mined`; this is a rate and a curve, not hardware |
| **Gas** (new goods, e.g. *Hydrogen*, *Ionised Gas*) | ionised nebulae, comet volatiles, a gas giant's upper atmosphere | **siphon** (new `provides.siphon`) | the ore hold (gas is a mined good), larger `volume` per unit | `siphonRate * density(position)`: **densest at a cloud's core**, a gradient to fly | the first harvest that rewards *where in the field you sit* |
| **Scrap** | graveyards, wrecks | the existing salvage (`loot`), plus a bulk **beam** mode for small scrap | ore hold | goods of the destroyed hulls at `salvageShare` | no new goods: scrap is Iron, Steel, Circuits at their volumes |
| **Rare / exotic** (later) | a core in a rich impact, an ionised cloud's rare isotope | a charge, a siphon of a second type | -- | burst: one pull pays many | optional; ED's core mining as the model; not in the first slices |

**Gas goods and the economy.** Gas must be needed by something or it is a number: the first
gas, *Ionised Gas*, feeds the existing recipes by replacing part of a recipe's input (Fuel from
Ice and a share of gas, Circuits' Crystal), in `goods.json`, and `Gen::OreModel` learns it by
the same `Goods::OreFor` explosion that already turns `uses` into ore: gas is one more `mined`
good, so R3's need covers it **without a new rule** and gas fields are paid from the same bank.
A planet's `Fuel` output stays: gas giants *are* the steady source of Fuel; clouds are the
bursts, as belts and debris are for rock.

**Hull and hold.** The hold side is nearly free: an ore hold exists, and gas is a mined good.
The hardware is the real change and is **one new module family**: a *siphon*, `provides.siphon`,
a bow or a mid (a hopper-like "scoop" mid with an intake), cost in Iron and Crystal like the
other harvesters, and a mining skill that applies to both (the skill is *Mining* today; gathering
gas is the same trade). A hull fitted with a beam and a siphon carries both and is a
generalist; a specialist keeps its slots for one. Which hull gets which is `ships.json` data.
A hull's `mining` stat stays the beam's rate; `siphon` is the new stat on `Ships::Stats` and
the `preview_fit` view, read by the Fit tab and econagent alike (#279: never a copy).

**Yield curve in one line.** *Rate = harvester rate x field richness (`extractRate`) x density
(age and, for gas, distance from the core) x skill x security.* Every factor is a number in
data, and the only new one is density.

## 6. Scanner and discovery

- A field is **visible** (public) or has a **signature** (hidden until a scan resolves it,
  #346), by size (3.3). A hidden field's scan needs the same probe and takes the same seconds
  (`world_rules.json` `scan`), so the scanner (P) already works; it gains a line: the field's
  *kind of matter* is revealed at a partial resolve (the way EVE shows a signature type before
  it shows a site), its *richness* when resolved.
- A resolved signature is the pilot's (account `scanned`). A field's end deletes the key
  (the usual `RemoveStatic`), and the `scanned` set is pruned by `foundSignatures_`'s existing
  rule. The first pilot to resolve one is news, as for hidden bases.
- **Sensor screen (V) and overview**: both list a field with its matter and its age phase
  (arriving, bloom, rich, thinning); the overview's "actions" apply (warp to, orbit, mine).
  The overview is the instrument (M9), so the information that tells a pilot *where to go* is
  there; the world view stays free of faction colour (#117) and the field's colour is its
  matter, not an allegiance.

## 7. The look: each source and event reads differently

The generated look (M6) means a field is a **composition in data**, bound to a material, lit by
the system's own stars (#119). No new primitive and no hand-drawn art is needed.

- **Seven primitives only** (`disc`, `bar`, `capsule`, `chevron`, ...; the grammar is narrow on
  purpose). Everything below is made of them with `repeat`, `mirror`, `row`, `tint`, `alpha`
  and `soft`, the way `field.asteroid` (rocks on offsets), `nebula.cloud` (soft discs with a
  `row` and `blink`) and `nature.corona-streamer` (a tapering ray) already are.
- **Per kind**, a new archetype each, each choosing its material and modules:

| Source | Archetype (new) | Parts | Material and tint |
|---|---|---|---|
| Rock belt | `field.asteroid` (exists) | rocks on offsets, ore veins | `hull`, grey |
| Impact debris | `field.debris` | an **arc**: a `row` of rocks with `turn` and `taper` along the planet's orbit, a few `nature.ore-vein`, a hot soft glow at the point of impact fading with age | `hull`, scorched dark with a warm `trim` at fresh |
| Comet trail | `field.trail` | a bright icy head (hard `disc`, `ice-chunk`s) and a long **tail**: soft discs in a tapering `row`, like `corona-streamer` helmet and ray | new material `ice`: high albedo, a cool specular rim; tail `alpha` low |
| Collision fragments | `field.fragments` | a tight scatter of bright, sharp `nature.rock` `shard`s with fresh ore veins; more `minPixels` so they read from afar | `hull`, bright, high contrast |
| Ice belt, vent | `field.ice`, `field.vent` | ice chunks in the belt composition; a vent adds a periodic soft plume (`blink`, f(time)) | `ice` |
| Gas cloud | `field.gas` | stacked soft `disc`s, **drawn like `nebula.cloud` but smaller and denser, with a brighter core** and a drifting `row` | new material `gas`, emissive, **tint by gas** (ionised blue-white, volatile pale green) |
| Moon breakup ring | `field.ring` | a full arc of rocks and ice, wide, with a few large chunks | `hull` and `ice` |
| Graveyard | `field.scrap` | `wreck.*` modules (`breach`, `scorch`, `dead-lights`) as wrecks use | `plating`, the faction's `livery` of what died |

- **Ageing is read from the picture without a new renderer feature.** `Describe()` already sets
  `intensity = ore / oreMax` and a worn belt draws paler. For the *spread* of a thinning field
  the cheap, honest way is **three archetype variants by phase** (`.fresh`, `.mature`,
  `.scattered`), swapped at the two phase boundaries by `MarkStaticChanged`, instead of
  teaching the backend an age: the sparse variant simply has fewer parts and wider offsets.
  If a smoother read is wanted later, `Render::Item` gains an `age` field a material may bind
  (`#121`: "a field of the `Render::Item`" is already a legal source) -- an additive change.
- **The moment** is a soft `disc` flash with a short `blink`, in the same pass as `nature.flare`;
  the screen treatment (switchable) is unaffected.
- **Every one needs a gallery card** (`worldeditor gallery shapes card field.debris ...`), the
  screenshot proof the compiler cannot give (`economy`-style "see the frame"), a sockets view,
  `truescale`, and the editor's pins can place one by hand (admin content is the exception on
  top, M7).

## 8. Persistence, wire and generator, in the owner's terms

- **World file:** `fields` log (the phenomena alive: kind, system, `t0`, path, seed, size,
  `taken`), `bank` per region and ore, `eventsHash`. `WORLD_VERSION`: adding fields an older
  reader can ignore needs no bump; an older server reading a newer world refuses it by the
  existing rule.
- **Wire:** the layout carries the archetype and the `t0`, the velocity and the seed once;
  `BeltOre` already carries ore and max, extended with the cap so the client can show the
  phase. A field's `harvest` kind and the `siphon` stat are new meaning on messages, so
  `PROTO_VERSION` is bumped when slice 1 changes it.
- **Generator:** the base belts are sized to `(1 - eventShare) * need` (a `GENERATOR_VERSION`
  bump; sealed regions keep their belts). The pre-roll uses its own `Gen::Rng` key, so one more
  stream never moves another; golden hashes in `gen_tests.cpp` cover it.
- **Agents:** `observe` lists fields with matter, phase and richness; verbs are the existing
  `mine` and the scan; `siphon` is the same toggle (`toggleMining`) on a hull with the module.

## 9. Slice plan, smallest playable first

Each slice is playable alone and ends with a bench or a screenshot, not a belief.

1. **A field with a life** (built; `Simulation_Fields.cpp`, rules in `world_rules.json` `fields`,
   the console's `field <system|pilot> <ore> <amount> <hours> [size]`). `AsteroidField` gains `bornAt`/`life`/`curve`/`taken`; cap and
   density-scaled yield; the absolute-rate NPC branch (4.3); the snapshot carries the cap; the
   server's console gets `field <system> <ore> <size> <hours>` to place one by hand. Playable:
   an admin drops a field, pilots and NPC crews work it, it thins and ends. *Proves:* the yield
   curve, the market pass with a finite field, `LayoutDelta` at birth and end.
2. **The event engine with two kinds** (built; `Simulation_Events.cpp`, `EventKinds.cpp`,
   `data/events.json`, `world_rules.json` `events`). `data/events.json`, `world_rules.json` `events`, the
   epoch roll, the **bank** (4.1), the due-queue, the `fields` log in the world file, the news
   line and the journal line, the pre-roll at opening, `Impact` (debris arc, a satellite of its
   planet) and `Collision` (moves a belt's ore). *Proves:* the ledger with the flow tally of
   `fleetbench` (4.4), reproducibility (the same seed and clock make the same field, and a
   saved world reloads it), hot/cold sameness.
3. **See it happen.** The omen and the moment (the static in `arriving` phase, a flash), phase
   archetypes (`.fresh`, `.mature`, `.scattered`), the public / signature split by size and the
   scanner showing the matter, the overview and sensor lines, econagent `observe`. *Proves:*
   gallery cards for the debris and fragments, a screenshot of the moment, a pilot who enters
   during the omen sees it in progress.
4. **Ice and comets.** `Comet pass` with a moving trail (position `f(time)`), the `ice`
   material, sublimation's faster curve, `Vent` as the steady source. *Proves:* a field that
   moves through a system and leaves, with no per-tick integration.
5. **Gas.** New gas goods and recipes, the `siphon` module and stat, the `field.gas` archetype
   with the `gas` material, the density gradient, the `Flare` kind that ties a star's visible
   flare to a nebula's cloud; `OreModel` learns the gas goods; Fit tab and `preview_fit` show
   the siphon. *Proves:* R3's need covers gas without a new rule; a pilot flies *to the core*.
6. **Breakups, graveyards, rare cores.** `Moon breakup` rings, `Graveyard` from real losses
   (the siege and fight code reports hull losses; `salvageShare`), the optional rare/exotic
   burst. *Proves:* "numbers from the world" end to end.
7. **Later, and only after the owner says so:** player-built **extractors** that work a field
   at a rate (the #44 line, as EVE's moon drills and an outpost's upkeep), a *claim* on a field
   under the territory rules (#431), pins and packs that place a phenomenon by hand
   (`events` in a pack), and events a player *causes* (redirecting a comet). None is designed
   here.

## 10. Decisions for the owner

Only the forks that change the game's feel. Everything else above is an engineering choice
that follows the premises and can be decided as it is built.

**D1. How much of the world's ore comes from events, and how much from steady belts?**
This sets how much of the economy's feel is "plan" versus "luck".

| Option | Feel | Risk |
|---|---|---|
| A. Events are a bonus; belts remain nearly the whole supply (`eventShare` 0.1-0.2) | Safe, stable prices; events are a treat | the owner's complaint stays: the world is still mostly static |
| B. A mix: belts the larger half, events the rest (0.4 in a centre, 0.7 in a frontier) | Prices have waves; a centre is reliable, the frontier is a hunt | needs the bench in 4.4 to prove no starvation |
| C. Events are most of it everywhere (0.8+) | Constant churn; "where to mine" every time | A centre can starve between events; the economy is a lottery |

*Recommendation:* **B**. A centre is where a new pilot learns; it should not run dry because a
dice roll went badly. A frontier is a hunt by design (#342: the game begins when a wormhole
opens into the unexplored). The share is one number in `world_rules.json` and can move later.

**D2. How are fields found?**

| Option | Feel |
|---|---|
| A. Everything is announced in the news and visible (a gold rush: first to arrive wins) | Rewards speed and reading the news; no scanner needed; crowds and fights at fresh fields |
| B. Everything is a hidden signature (EVE's cosmic signatures) | Rewards scanning and exploring; quiet; solitary; the news has nothing to say |
| C. By size: large events are public, small ones are signatures | Both: the big event is shared news and a crowd, the small one is the prospector's reward |

*Recommendation:* **C**. The owner wants events a player "can see happen and witness" (A's
half) and the existing scanner (#346) already has a place to put B's half. News is cut per pilot
by what they have charted, so an unexplored system's event is not leaked.

**D3. Gas: separate hardware, or the same beam?**

| Option | Feel |
|---|---|
| A. A new harvester, the **siphon**, a separate module; the ore hold is shared | A pilot chooses what to fit; a hull is a specialist or a generalist; gas is its own trade at the shipyard |
| B. The beam gathers gas too, with a lower rate | Zero new hardware; one trade; gas is only a look |
| C. A new harvester *and* a new gas hold (tank) | The most specialisation; a ship carries one or the other, so a gas run is its own job; more parts to design and balance |

*Recommendation:* **A**. The issue asks for "its own modules" per resource, and it makes the
Fit tab matter. A separate tank (C) is the next step *if* the owner wants gas to be a different
profession; it is additive (`provides.tank` beside `provides.ore`), so choosing A now does not
close C.

**D4. How fast do fields come and go?**

| Option | Life | Feel |
|---|---|---|
| A. Hours (comets 2-4 h, debris 12-24 h) | A session | "Right now" play; strong urgency; an offline pilot always misses it |
| B. Mixed by kind, hours to days (6 h to 10 days, table in 3.1) | Kinds differ | Some things are an evening, some a week's reason to move a base; the wild kinds are fast, the large ones are slow |
| C. Days (2-7 days all) | Slow world | Planning, hauling campaigns; little urgency |

*Recommendation:* **B**. A world with many players across timezones should have both the thing
you must be there for and the thing you can plan around; the table in 3.1 sets the numbers per
kind and they are data. A Vent is the always-there rhythm.

**D5. Does the world compete with the players for a field?**

| Option | Feel |
|---|---|
| A. NPC crews work fields like belts, at a limited rate and only in systems with a market whose shelf is not full (4.3) | The living world is visible: a field near a market is worked out in a day and its ore lands on the shelf; the frontier's are a player's |
| B. NPCs never touch event fields; only players do | Fields are the player's; but ore from events never reaches the economy except through players, so R3's need is not met by them, and the market is not fed |
| C. NPCs take a fixed share of every field | A predictable tax on every field, close to the old belts, and the same wasted ore at a full shelf |

*Recommendation:* **A**. B breaks the ledger (the bank is the supply the markets are
calibrated against); C is the old behaviour in a new coat. A keeps the world alive and leaves
the frontier to the players, and the rate is the miner design's own, not a tuned number.

**Not asked, with a default.** Pirates follow rich fields in low-security systems through the
existing spawn director (4.3); fields are never `claimable` until the territory rules say so
(slice 7); events never destroy a player's structure (an Impact avoids bodies' paths and
structures by the placement rules, the same clearance as `Build`).

## Sources

- EVE University wiki, [Ore anomalies](https://wiki.eveuniversity.org/User:Lexara_ellecon/Ore_and_Ice_Anomalies), and Mining: anomalies despawn when mined out or after about three days; ice respawns hours after being mined out; gas and hidden sites are cosmic signatures; moon mining delivers timed chunks.
- Elite Dangerous community material on [hotspots](https://elite-dangerous.fandom.com/wiki/Hotspot): persistent hotspots raise the odds of a mineral, mostly for core asteroids; prospecting is how a player reads a ring.
- X4: Foundations community and forum material on resource fields: a per-field regeneration rate, probes that reveal density, gas collectors as separate ships.
- Star Citizen, No Man's Sky and Space Engineers rows are from the author's general knowledge of those games, not from fetched pages, and should be read as summaries rather than quotations.
