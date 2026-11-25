# Economy scales: do the flows of the world agree with each other?

Status: **analysis, no code change** (refs #584; the rules just built are #566, #580). It
answers one question: a belt of 358 Iron gives a native holding about 36 Iron an hour, a
cruiser takes 242, and the stations' recipes draw on the same Iron -- is that a one-off, or
do the world's scales disagree by design? They disagree by design, and by a lot. The
recommended set of rules is at the end (section 3). **Built since:** the trader pile-up (#594), R1 and R2 (#595), the yard plan that was dropped (#596, the cause of the yards-per-system finding) and R3 (ore from need, `Gen::OreModel`, GENERATOR_VERSION 14). R4 and R5 wait for a measure after these.

## Method

`econserver fleetbench` (Release, no players, so every system is cold and stepped by the coarse
pass) on seeds 1-3, 7 days, plus 14 days to see that nothing changes later. Each run is one
generated civilisation centre and one attached frontier region (131 systems on seed 1). The
bench prints shelves per good, but not flows, so I ran it with a throwaway tally on the
market step, the belt step, the fleet purchase and the haulers (every unit that enters, is
converted, is used up, is clipped at the stock cap, is bought into a ship or is hauled, per
market, summed by region class). The tally is not part of this change. The region statistics
are the generator's own, over seeds 1-12 (36 centre regions, 36 frontier regions). Numbers
are units an hour unless said otherwise; "wanted" is what the recipes would draw at their
rate if their inputs were there. `macrobench` measures CPU, not flows, so it was not used.

Two corrections to the issue first, because the arithmetic in it is the premise:

- A belt does not regrow a tenth of *what is missing*. `AsteroidField::Regrow` adds
  `beltRegrow * oreMax * hours`: **a tenth of its full size an hour, constant**. A belt of 358
  grows back 36 an hour, not 12.
- The miners do not cap the yield. A system's crew is `min(1, miners / 2)` (two miners is a
  full crew, whatever the number of belts), and every belt is worked at `beltMined = 0.15`
  of what is left an hour. At the steady state `0.15 * ore = 0.1 * oreMax`, so the belt sits
  at two thirds full (239 of 358 on the issue's system, as measured) and **yields exactly
  `beltRegrow * oreMax` an hour**: 20-45 Iron for a belt of 200-450. More miners or a higher
  `beltMined` change nothing; only the belt's size (or `beltRegrow`) moves the yield.

## 1. Flows per region

### What a belt and a station are worth, from the data

| thing | per hour |
|---|---|
| Iron belt, generated size 200-450 (+40 per depth) | 20-45 Iron (0.1 of its ore) |
| Mining outpost, Steel recipe (rate 20, 3 Iron each) | wants **60 Iron** (and 30 Ice for Fuel) |
| Shipyard, ten module recipes (rate 1 each) | wants **44 Iron**, 13 Crystal, 3 Ice, 3 Steel, 1 Circuits, 1 Ammunition (+12 Steel, 6 Crystal for paste) |
| Trade hub (Circuits, Medicine) | 12 Crystal, 6 Steel, 5 Ice, 10 Food |
| Military (Ammunition) | 8 Steel, 8 Fuel |
| A cruiser (every faction's warship, one design) | 242 Iron, 19 Crystal, 4 Ice, one railgun (3 Steel, 1 Ammunition) |
| A holding's keep | 3-6 ships (`capacity`); a faction buys `growth` of the shortfall a minute |
| A holding's income (`holdingIncome`) | 1500 credits at full prosperity: 0.43 cruisers an hour at base prices |

So one mining outpost asks for more Iron than one and a half Iron belts give, and one
shipyard for as much as one belt and a half. Population draws no Iron at all: the only sinks
for Iron are recipes and hulls.

### Centre region (what a player starts in)

Generator mean over 36 regions: 10.9 systems, 23 belts, of which **Iron 3.5 belts and 1,418
ore** (range 271-2,716), Ice 9.9 belts and 4,137, Crystal 10.0 belts and 4,317; stations 5.9
trade hubs, 1.9 shipyards, 3.1 military, 6.4 mining posts. The sustainable yield is a tenth
of the ore an hour: Iron 142, Ice 414, Crystal 432. Recipes at their rates would want **468
Iron**, 228 Ice and 107 Crystal.

Measured (seeds 1 / 2 / 3, 7 days):

| good | mined | recipes wanted | recipes got | ships took | lost at stock cap | shelf of target, start to end |
|---|---|---|---|---|---|---|
| Iron | 202 / 26 / 25 | 448 / 268 / 164 | 106 / 32 / 32 | 131 / 22 / 19 | 0 | 68% to 21%, 57% to 15%, 69% to 19% |
| Ice | 286 / 242 / 284 | 226 / 116 / 98 | 166 / 97 / 95 | 7 / 2 / 4 | 29 / 73 / 85 | 3-5x target at the end |
| Crystal | 637 / 399 / 482 | 134 / 86 / 103 | 26 / 9 / 11 | 7 / 2 / 4 | **512 / 302 / 393** | 8-10x target (the cap) |
| Steel | made 32 / 9 / 9 | 102 / 94 / 81 | 34 / 12 / 11 | 0 | 0 | 17%, 2%, 15% |
| Food | planets 664 / 360 / 288 | | | used 196 / 132 / 144 | 228 / 51 / 19 | 4-8x target |

Ships bought in the centre: 1.2 / 0.4 / 0.7 an hour. The fleets end (7 and 14 days alike)
at about 75% of keep (48 of 65, 32 of 40, 32 of 45): not a collapse, a steady state held down
by Iron. The Iron shelf is pinned near its floor (15-20% of target, the price at the
ceiling of 3x) because mining and consumption have met: `mined = recipes + ships`.

### Native host (the regions a native faction lives in)

A host system is generated with the native station set: 6-11 stations in one system, 5-10
of them with a shipyard. The yards run the same ten module recipes each, so a host system
**wants 5 x 44 = 220 Iron an hour (up to 440 with ten yards) from belts that give 4-117**. Seeds 1 / 2 / 3, whole native group (12 / 14 / 14 systems):

| good | mined | recipes wanted | recipes got | ships took | lost at stock cap | hauled |
|---|---|---|---|---|---|---|
| Iron | 337 / 481 / 451 | 789 / 856 / 1,277 | 162 / 181 / 286 | 50 / 60 / 94 | 41 / 123 / 20 | **0 / 0 / <1** |
| Crystal | 457 / 548 / 443 | 379 / 395 / 539 | 58 / 52 / 95 | 1 / 1 / 1 | 249 / 310 / 119 | 0 |
| Steel | made 7 / 17 / 27 | 301 / 312 / 408 | 6 / 10 / 8 | 0 | 0 | 0 |

The group's Iron looks long (shelf 1,600 to 19,017 on seed 1) and the yard systems are
dry. Both are true, in different systems: of the native systems with a market, those with
no yard mine 4-119 Iron an hour, want none, and sit at the cap of 4,000 (ten times target);
the yard systems on belts of the same size want up to 410 and sit at 0-1,600. **Next to nothing hauls in a
native region** (0 Iron an hour on seeds 1-2, under 1 on seed 3): a hauler goes only to a
neighbour that has a market, and a native host has none (checked on seed 1: every host
system has 0 market neighbours).
A native region is a set of islands, and `StepHauls` only knows the next door.

The issue's system (seed 1, `r1-31-3`): belts Iron 358 / Ice 1,622 / Crystal 1,581; six
markets, five of them yards; the belt gives 36.5 Iron, the recipes ask 216.6 and take
38.9 (all that arrives); nobody hauls; the shelf is 0, the ask 3.1x. The shelf was never
"stuck below the hull": **the recipes spend it before the fleet's order comes round**,
because `Market::Step` runs every two seconds and a fleet buys once a minute from whatever
is left.

### Frontier region (what a player explores)

No market in the bench's frontier region in 7 or 14 days, so there is no flow to measure
(a frontier region has 1.1 trade hubs, 0.6 mining posts, 0.2 military and no yard on
average): 18 systems, 38.6 belts with Iron 3.6 belts and 1,486 ore, Ice 5,481, **Crystal
15,824**. The yield is Iron 149, Ice 548, **Crystal 1,582 an hour with a demand of about
12**. The ore is there for a player to find; what the generator makes is a world in which
the further out, the more Crystal (the roll puts Crystal below `15 + 12 * depth` and Iron below 60, so Iron's share of belts is
45% minus 12 points a level: 33% at depth 1, 9% at depth 3, none from depth 4) and the one thing
every hull needs a lot of is the one thing that stays at 1,400-1,500 a region.

### Hauling

Haulers work (a system sends a trader every two minutes, 30 Iron of volume a trip, so a
road has room for 900 Iron an hour), but only between adjacent markets and only where
prices differ by the margin. In the centre it moves 49 Iron / 26 Steel / 184 Ice an hour in
seed 1 and a few units in seeds 2-3: a centre with one Iron belt has nothing to haul.
Incidentally, **traders pile up**: after three days the capital had 556 traders, others 269 and
227, where a system keeps 2-6 (`traders = 2 + 4 * security`). Each haul takes a trader out
and the top-up makes the system whole from nothing, while the hauler arrives and stays;
nothing takes the surplus away from the sink end of a gradient. It does not change the
goods (a haul is limited by prices, not traders) but it would be 556 ships in a hot system
and is a bug to file on its own.

## 2. Where it does not balance

1. **Iron is structurally short, everywhere, by 2-5 times.** Supply is about 140 an hour
   in a centre region and about the same in a frontier one; the recipes' appetite is 450
   (centre) and 220 per yard host. Even the end demand (population's Steel, the yards' and
   the military's, ammunition, paste, circuits, and a fleet's replacement of about 1% of
   its hulls an hour) exploded back to ore comes to about 350 Iron an hour in a typical
   centre by my back-of-envelope, 2.5 times the belts. The ore roll makes it worse the further out a region lies.
2. **Crystal and Ice are long by 4-100 times**, and the surplus is destroyed: 300-500
   Crystal an hour is clipped at the stock cap in a centre, 120-310 in natives. The world
   pays for mining that nobody uses. Food is the same (4-8x target, up to 228 an hour clipped).
3. **Recipes run before orders.** Steel, module and paste recipes take the Iron that
   arrives every two seconds; the fleet's purchase, on its own minute, finds a shelf at the
   floor. The profit gate (#566) cannot see this: Steel is dear too, so turning Iron into
   Steel still "pays" at three times the base price. Fleet purchases are the only
   consumers with a deadline, and the only ones that lose.
4. **Appetite is per station, not per supply.** A recipe has a rate in goods.json and no
   notion of what its system can feed: five yards in one host system are five times the
   appetite on one belt. Eight of the ten module goods are player-only (hull parts); with
   no player they are stock-keeping up to ten times target, and they are bought by nobody.
5. **Native regions are islands.** Hauling is one hop and needs a market next door; hosts
   have none, so a host's Iron neither leaves nor arrives, and the yard-heavy hosts are
   the dry ones. That is why 1-2 natives per seed stay at zero ships while the group looks
   rich.
6. **Money is a faucet that does not look at goods.** 1,500 credits an hour per holding is
   150 Iron an hour at the base price, against 20-45 an hour that each of the holding's belts
   gives. Treasuries stand at 35k-480k while the shelf is at the ceiling. The willingness to
   pay (#566) opens the purse and finds nothing to buy.
7. **Scale of the fleet against the flow.** A standing fleet of a centre (65 ships) is
   15,700 Iron of hull: five days of the region's whole Iron production. A native holding's
   keep of three is 730 Iron, twenty hours of a good host belt. Rebuilding after a loss is
   not a minutes-scale thing in any of them, and that is fine -- but the keep is not derived
   from what the region can supply.

What is **not** off: the hull. A cruiser's 242 Iron is in proportion to its mass (frame
24 mass for 90 Iron, sections 3-4 Iron per mass, modules 2-2.5), as the owner decided on
#566. The scales that disagree are the belts', the recipes' and the haulers'.

## 3. Rules that would make the scales agree

Each is a rule derived from data the world already has, not a tuned constant (#463).

**R1. Make to target (recipes idle when their output is full).** A recipe does not run
while the shelf of every good it makes is at or above its target. Today a producer works up
to ten times target, until the price falls below its inputs'. *Changes:* `Market::Step`;
Food, Fuel, Medicine and the module goods stop stacking at 2-9 times target, the yards stop
turning Iron into hull parts that nobody orders, the Ice and Crystal inputs they take
stay on the shelf. *Premise:* nothing from nothing; production follows demand (the
profit gate, #566, generalised from price to order). *Bench:* the Iron the recipes want in
a host falls from 217 an hour to what the shelves actually consume (a few tens); module
goods at 100-130% of target, not 126-330%; `lost at cap` for Food/Fuel/Medicine falls to
about nothing.

**R2. A holding's order is reserved before recipes (orders before recipes).** A recipe may
use the part of an input's stock that is above what its holder's open ship order needs (its
shortfall against keep times the design's parts), for as long as that order stands. The
market then holds back the Iron for the cruiser instead of turning every unit into Steel as
it arrives. *Changes:* `Market::Step` takes a reserve per good from the holder's order
(the same number `BuyShips` computes as `need`); no new store, the order is the existing
shortfall. *Premise:* numbers from the world, one combat model for NPCs and players (a
fleet's replacement is demand like any other and has a deadline). *Bench:* the 584 system
reaches a shelf of 242 Iron within about seven hours of the belt (not never); natives at
zero ships on seeds 1-3 over 14 days: none.

**R3. Ore follows need (generator).** A region's belts are sized and typed from the
region's own end demand, exploded through the recipes to ore: population uses, the stations'
`uses`, a fleet's replacement (keep x its loss rate x its design's parts) -- computed from
`goods.json`, `ships.json` and the temperaments by the generator, with the margin the
belt's one-tenth yield implies. Where the need for an ore is small (Crystal, about 12-110
an hour) one belt; where it is large (Iron, about 350) more and bigger belts, in the
system with the stations that use it. A native host's own belts carry its yards. The depth
roll for the *type* of belt becomes a roll among what the need leaves open (so the
frontier is still richer in Crystal for players, but a region's Iron is no longer 10%).
*Changes:* the belt block of `src/engine/gen/Region.cpp`, `GENERATOR_VERSION` (sealed regions
keep their content, so only regions opened later change). *Premise:* the world is generated
and numbers come from the world (M7, #463): the same derivation the siege's damage uses.
*Bench:* per region `mined / wanted >= 1` for Iron in every class; Crystal lost at cap under
5% of mined; no market dry of Iron for more than a day; fleets above 90% of keep in 14 days.

**R4. Keep follows the region's flow.** A holding keeps what its region can maintain:
`min(capacity, region's Iron flow / (replacement rate x parts))`, so a poor region keeps
fewer ships and does not look like a collapse. *Changes:* the keep in the faction period step in `Simulation_Factions.cpp`.
*Premise:* numbers from the world (#463), living world agency (a faction acts on value
against risk). *Bench:* ships/keep stays near 1 where Iron is lean, in place of 0-50%;
"COLLAPSE" lines disappear for supply reasons, remain for siege.

**R5. Income from output.** A holding's income is a share of `SystemOutput` (what its
belts, planets and markets give in value per hour, already computed for the licence rent)
in place of the constant 1,500. *Changes:* `holdingIncome` in the faction pass. *Premise:*
nothing from nothing: money comes from value made. *Bench:* treasuries stay within a few
hulls; the ask at the shelf does not sit on the ceiling while credits idle.

Considered and not recommended:

- **Miners that scale with demand.** The crew saturates at two and the belt's regrowth is
  the cap, so more miners yield nothing more. What would help is to let miners idle at a
  full shelf (the ore stays in the belt instead of being clipped), but that saves ore,
  not Iron. It is a result of R1 and needs no rule of its own.
- **Cheaper hulls, or faster `beltRegrow`.** The first was rejected on #566 (tuning); a
  uniform regrow multiplies the Crystal surplus along with the Iron.
- **Multi-hop haulers.** Breaks hot/cold sameness (#566), and R3 makes the islands
  self-supplied, which is the cause removed rather than a transport added.
- **Recipe rates from a station's size.** The stations have no size in the data that the
  recipes read; R1 and R2 bound the appetite by what is ordered and what is there, which
  does the same job without a number.

### Recommended set

**R1 + R2 first, then R3; R4 and R5 after they are measured.** R1 and R2 are small (the
market step, reading numbers the purchase already has) and remove the competition between
Steel and hulls without moving any belt, which is enough to answer #584 for an existing
world. R3 is the cause: the belts are three to five times too small in Iron and a hundred
times too big in Crystal, and no rule on the consumers can feed a hull from a shortfall of
ore; it is a generator bump, so it only reaches regions opened after it. R4 and R5 are
about what is left once the flow is right. A bench for the set: `fleetbench` seeds 1-3 for
14 days plus the flow tally, with these lines to read: Iron `mined / wanted` per class,
`lost at cap` per good, haul in/out in native regions, and ships against keep.

## 4. How the others keep production chains in balance

**EVE Online** does not balance chains in the simulation at all. Players are the producers,
the miners and the buyers; the game sets the *sources and sinks* and leaves the ratios to
the market. Ore is not a flow to be matched but a regional resource with a fixed shape:
belts and anomalies by security band and region, with respawn on a daily rhythm, so the
supply of each mineral is a property of the map and the number of players. A hull is a
fixed bill of minerals, ship loss is the main demand ("destruction drives the economy"),
and CCP watches the totals (its monthly economic report) and tunes ore yields, blueprints
and sinks by hand when a mineral drifts. In short: the scale is held by one control (player
counts and a designer who rebalances), and by prices. This game's analogue of the designer
is R3, done by the generator once, from the data, rather than by hand later.

**X4: Foundations** is the closer model, a fully simulated economy with no players needed.
A station is a set of production modules with a fixed cycle (our recipe with a rate); each
ware has a minimum and maximum price and the price floats with the stock of the station
(our `PriceOnShelf`); stations post buy and sell offers, and AI traders and miners are
assigned to the offers by need. Two things keep the chains in balance there that this game
lacks. First, a station *stops producing when its output storage is full* and *stops
buying inputs when its input storage is full* (R1). Second, the AI factions *build more
production where a ware is short*, which makes the number of modules a function of unmet
demand rather than a constant of the map (our `RegionWantsYard` does this for yards only;
R3 does it for ore). Resource fields in X4 regenerate at a fixed rate per sector and run
short for the miners, as here: it is the factories' growth, not the field, that adapts.
(Both paragraphs are from my knowledge of the games, not from a source I checked here.)

## Findings that are not rules

- Native host systems get 5-10 shipyard stations each; a yard is not capped per system
  (the growth rule asks for one per region, but the generator places the whole native
  set). Worth a look with R1, because each is 44 Iron an hour.
- Traders accumulate at the sink end of a gradient (556 in one system after three days):
  a plain bug in the cold arithmetic, to file separately.
- The residual of the tally (about 2.4 Iron an hour per native system) is the rescale of a
  market's shelf when a station is added, not trade.
