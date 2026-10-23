# World knowledge, territory and law

Status: **decided by the owner in four rounds, 2026-10-21 and 2026-10-22**, recorded in the
comments on #431. This file gathers those decisions as rules, maps each one onto the code, and
cuts the work into slices.

**Built (as of 2026-10-23):** slices 1-16 are on main -- 1 #443, 2 #453, 3 #448, 4 #459,
5 #449, 6 #456, 7 #450, 8 #454, 9 #447, 10 #452, 11 #460, 12 #465 with its follow-ups in
#467, 13 #445, 14 #457, 15 #462, 16 (chart sales to factions, with #346)
(`PROTO_VERSION` 61, `WORLD_VERSION` 18). **Still planned:** selling charts to players
(#473); war declarations; and the questions under **Open design questions** at the end. The sections
headed "The code before the slices" record the starting point each slice changed; each
slice's *Built* note in the slice plan says what is there now. Related: #342 (the growing world, `world_generation_v2.md`), #347 (corporations),
#432 (the playtester fork: routes, faction shipyards), #433 / #437 (players build outposts),
#438 (what player outposts still lack).

The decisions are not re-opened here. Where a decision leaves a number or a detail free,
the doc proposes one and marks it as tuning; the owner's answers to what the rounds left open are
under **Decided after the rounds**.

## Why this exists

Playtest of 2026-10-21 (#431): systems that NPC factions had settled were unknown to
players, and the first player in was credited "found by <pilot>". Factions were already
there, so their charts exist. The fix touched everything around it -- who knows what, who
names what, who owns a system and what that is worth, how a holding is defended, and what
happens when one player shoots another -- so the owner took it as one design discussion
before anything was built.

## The decisions at a glance

| Round | Topic | Decision |
|---|---|---|
| 1 | Knowledge | Mixed: public charts (centres and whatever factions charted), plus each pilot's own knowledge, shared within a corporation |
| 1 | "Unknown" | Unknown to the centres, not empty. Natives have their own names, from a per-culture generator |
| 1 | A find | A chronicle entry, the right to name empty systems, and charts to sell |
| 1 | Faction first | Straight onto the public charts, "charted by <Faction>". Deep, hidden and hole space stay player finds |
| 2 | Owning a system | Most influence **and** a contestable claim |
| 2 | What owning brings | Market tax, docking rights, police paid from the treasury, naming of empty systems the owner discovered. Not a ban on building |
| 2 | The ladder | A pilot owns structures; a corporation owns systems and runs AI-managed assets within server limits. Corporations are never factions (#347) |
| 2 | Defence | No vulnerability windows. Regeneration by tier, escalating response from real forces, a front line, war through the economy, destroying is not taking |
| 2 | Building in faction space | A standing threshold **and** a licence. Licensed structures are guarded; unlicensed is trespass |
| 3 | PvP | Allowed, and a crime wherever law holds |
| 3 | Response | Scales with security, always from real forces; no invincible police |
| 3 | Consequences | A criminal flag (~15 min), a standing loss, and a pilot security status |
| 3 | Security | Base, plus the owner's paid police, minus nearby pirates. It drives police, pirates, convoy safety and pay |
| 4 | The map over time | Geography is known for good; owner and security are live; structures and markets are as last observed, with the date |
| 4 | Chart trading | Built into the map interface, not an item; its own task |
| 4 | Names | Inhabited: native names revealed at first contact. Empty: the discoverer names it, nobody else. Name rules and moderation |
| 4 | Drift | Charts stay, marked "drifted"; on return, marked outdated |

Also settled alongside (#432, #438):
- **Faction shipyards are built by need,** one per region once it has a market, under the
  normal growth budget. A player may build one as the next step of their own station (#433).
- **Attacks on structures reach offline owners:** in the journal on login now, on Discord later.
- **A corporation's own AI patrols** guard its structures while members are offline,
  configured by its officers, within limits the server sets.

---

## 1. Knowledge

### Rules

**K1. Knowledge has three sources, and a pilot sees their union.**
- **Public charts:** the civilisation centres, and every system a world faction has charted.
  Everyone has them from the first login.
- **Personal knowledge:** systems the pilot visited, scanned (holes, hidden bases: #346
  already works this way) or was given (bought charts, later).
- **Corporation knowledge:** what any member knows personally, shared with every member
  while they are in the corporation.

*Why:* the centres have mapped their own space for centuries, so a newcomer should not be
blind there; a frontier find is worth something only if it is not instantly everyone's; and
a corporation is the unit that explores together.

**K2. A faction charts what it reaches.** An expedition arriving, a survey resolving, an
outpost or Anchor finished, or a claim taken marks the system charted by that faction. It
goes onto the public charts as "charted by <Faction>".
*Why:* the faction was there; its charts exist. This is the bug of #431.

**K3. Unreached space stays a player's discovery.** A system no faction has reached --
deep systems, hidden ones, the far ends of roaming holes -- is found by the first pilot in.
What they find is theirs (and their corporation's) until a faction reaches it or they sell
the charts.
*Why:* the explorer races the factions; this is what keeps "the game begins when a wormhole
opens" honest.

**K4. Unknown means unknown to the centres, not empty.** A native system has its own name
and its people before anyone from outside arrives. Whoever arrives first -- a pilot or a
faction expedition -- makes **first contact**: the native names become known to them, and
they bring the charts back.

**K5. A find brings three things:** a chronicle entry (public, by name of the finder), the
right to name the empty systems in it (section 2), and charts a faction will buy (section 7).

**K6. Geography, once known, is known for good** (round 4). A pilot who has seen a system
never loses its place, links, bodies or name. What changes is said by the live and observed
layers (section 7).

### The code before the slices

| Rule | Today | What changes |
|---|---|---|
| K1 | One world-wide flag, `SystemAggregate::visited`. `KnownUniverse()` is everyone's index: charted systems plus frontier stubs. `KnownUniverseFor(s)` adds only the pilot's roaming holes. `BuildGalaxyState()` skips unvisited systems for everyone. | `KnownUniverseFor(s)` becomes public ∪ the pilot's ∪ their corporation's, with stubs at its edge. `BuildGalaxyState` is filtered per session the same way. Built when a pilot's knowledge changes (a revision counter, as `chartsChanged_` is now), never per tick -- #432 measured the per-tick rebuild as a cost. |
| K2 | `Simulation::TakeControl`, `ResolveSettle`, `ResolveSurvey` and the Anchor plan set `controller`/`claimed` or intel, never `visited`. The tide's own `explored_`/`surveyed_` sets are separate and stay. | Each of those points calls one `ChartBy(system, faction)`: sets `chartedBy`, records first contact if it is the first, and bumps the public charts' revision. |
| K3 | `Simulation_Player.cpp` ~1433: the first ship through a gate sets `visited` and `discoverer` for everybody. | It records first contact for the pilot and adds the system to the pilot's knowledge; it becomes public only through K2 or a sale. `discoverer` keeps its meaning (the first pilot) for naming. |
| K4 | Natives (`Region.natives`, #342 S5) live in up to three systems of a region; their systems carry generator designations (`r<slot>-d-k`). `NativeRoot` makes only the faction's name. | Section 2. |
| K6 | Nothing per pilot. | The account's `known` map (below). |

**`visited` is retired as a meaning.** Its two jobs split: "has anybody from outside been
here" becomes `firstContact` (who, when), and "is it on the public charts" becomes
`chartedBy` (non-empty) or "is in a centre". The flag stays readable in old saves and is
translated on load: a visited system with a discoverer and no faction holder is the
discoverer's personal knowledge; a visited system held by a faction is charted by it.

---

## 2. Names

### Rules

**N1. Inhabited systems are named by their people.** Each native culture has a name
generator; the names of a native region's systems are made with the region, from the seed,
and **revealed at first contact**. Until then they show as designations.
*Why:* the natives lived there before anyone came through the mouth; "R11-2-1" is the
centres' label for a place they have not seen.

**N2. Empty systems keep their designation until the discoverer names them.** Only the
discoverer may name one -- never the later owner, never a faction that arrives after.
*Why:* naming is the explorer's reward (K5); an owner gets the tax and the docks, not the
history. The discoverer names it **from anywhere** (map, the star's menu, or the agent tool with a system), the journal says so on discovery, and a refusal always says why (#513).

**N3. Names have rules:** 3 to 24 characters of letters, digits, spaces, `'` and `-`
(today's `Names::ValidSystemName`), unique in the world regardless of case, through a word
filter, and subject to admin moderation: an admin may rename or clear any player-given name.

### The code before the slices

- `Simulation::NameSystem` already refuses everything but the discoverer of a generated,
  non-centre system, and refuses a second naming. It needs one more refusal: a system with
  natives (named by N1).
- `Names::ValidSystemName` is the shape rule, shared by server and agent; it stays.
- The native **faction** name comes from `NativeRoot` + `NativeName` in `gen/Region.cpp`.
  The system names need a richer generator per culture -- the five themes (colonists, clan,
  sect, automata, traders) each with their own syllables and patterns. It lives in `gen/`
  under the generator's rules (#140): `Gen::Rng` keyed by (seed, slot, system), no
  distributions, no unordered containers. Its output is written into the region document
  (`natives.names`) and sealed with it, so `GENERATOR_VERSION` is bumped and a golden hash
  changes. A region sealed before it keeps designations; regenerating such a region
  (`econserver regenerate`) gives it names.
- **Majors** (centre factions) also get a naming culture: they name the empty systems they
  chart first (Decided after the rounds, 1).
- Moderation: a server console verb `rename <system> <name|->` and the same in the editor's
  world view; the filter is a text file beside `world.json` (`names_blocked.txt`, one word
  or pattern per line, re-read like `allowlist.txt`).

---

## 3. Territory and ownership

### Rules

**T1. Influence comes from structures and presence.** Each party -- a world faction or a
player corporation -- has an influence in each system: the sum of its structures' weights
(by tier: outpost, market, station, Anchor) and its armed presence (a faction's ships, a
corporation's patrols). A lone pilot's structures count for the pilot's corporation; a pilot
in none counts as a corporation of one for this purpose.

**T2. The owner has the most influence and a contestable claim.** A claim is a finished
structure of claim tier (an outpost or above) that can itself be besieged. Strength alone
claims nothing, and a claim nobody can attack would be a flag, which the owner rejected.
On a tie, the holder keeps it.
*Why:* "destroying is not taking" (D5) and "no hard flag" (round 2) both follow from making
the claim a thing in space.

**T3. What owning a system brings:**

| Right | How | Limits |
|---|---|---|
| Market tax | The owner sets the fee on trades at markets in the system; it goes to the owner's treasury | A server cap in `world_rules.json` |
| Docking rights | At the owner's stations and outposts: own, allies, or all | A faction's stations stay open to all but the hated |
| Police | Paid from the treasury; they raise security (L5) | Bought from real ships, never free |
| Naming | Only of empty systems the owner itself discovered (N2) | -- |

**Owning is not a ban on building.** Anyone may build in a system someone owns; what they
build adds to their own influence there, which is how a system changes hands slowly.

**T4. The ladder.** A pilot owns structures. A corporation owns systems. A corporation is
never a world faction (#347 stands); it has its own treasury and runs its own AI-managed
assets (C1) within limits the server sets.

### The code before the slices

| Rule | Today | What changes |
|---|---|---|
| T1 | `SystemAggregate::presence` (PerFaction ships) and `SystemProfile::outposts`, both factions-only. Player structures are owned by an account name (`Structure::GetOwner`); a faction's carry `faction:<id>` (`Outposts::OwnerOf`). | An `Influence` count per system, kept in the profile like `outposts`: per faction and per corporation, from structures by `tier` and presence. Recounted when the static layer changes, as the profile already is. |
| T2 | `StepControl` (`Simulation_Factions.cpp` ~1160): a challenger needs 1.5x the holder's presence plus a finished outpost of its own, held for `ContestPasses()`; an outpost of the holder in place stops it. `TakeControl` sets `controller`, `claimed`. | The same contest, with corporations as parties: the aggregate gains `holderCorp` (a corp id) beside `controller`. Exactly one of them holds. |
| T3 tax | A faction's market fee goes to `FactionMind::treasury` (#343); a player market's fee goes nowhere (#438). | The fee rate is the system owner's, and the fee goes to the owner's treasury (faction or corporation). |
| T3 docking | #437's `DockRefusal`: a private outpost docks owner and corporation; a market docks all. | The owner's setting is read there. |
| T4 | `Corp::Registry` (`sim/Corporation.h`, `corps.json`) has members and roles, no money. | A corporation treasury, and its policies (tax, docking, police, patrols), in `corps.json`. |

---

## 4. Defence without vulnerability windows

The owner rejected windows ("not logical"). Defence works through five mechanics, the same
for factions and players.

### Rules

**D1. Defences regenerate, with a threshold set by tier.** Every structure mends itself at
a rate its tier sets. Damage below that rate is undone; only fire heavier than the threshold
wears it down. A lone frigate cannot kill a station; a fleet can.
*Proposed numbers (tuning):* a tier's mend rate is a share of its hull per second, high
enough that a station needs about ten cruisers' sustained fire to make progress, an outpost
two or three.
*As built:* what a besieging ship takes off a wall is not a tuned number (owner, 2026-10-22:
no hand-tuned number stands in for the world). It is the faction's own warship design's guns
-- the doctrine's `warship` list in `data/ships.json`, the mean when it names several -- as a
player's ship of that hull and fit fires them (`Ships::WarshipDps`: the volley at the player
cooldown). Today's cruiser is 44 hull a second, so an outpost's 60 a second takes two, an
Anchor's 135 four; a faction that fields a heavier gun brings walls down sooner.

**D2. The response escalates, from real nearby strength.** An attack on a structure calls
the owner's ships in the system, then lends from holdings within reach -- the machinery a
siege already uses, run the other way. A faction with nothing nearby sends nothing.

**D3. The front line.** A system may be besieged only from a held or neutral neighbour: the
attacker (or an ally) holds a gate-adjacent system, or one of them is nobody's. A faction's
core -- systems whose every neighbour is its own -- cannot be besieged until a border system
falls first.
*Why:* this defends core territory by geography instead of a "cannot be taken" flag: a big
enough fleet takes the border, then the next system.
*As built (slice 8):* `FrontOpen(system, side)`, side a faction or a corporation. A system
nobody holds, or the side's own, is always open. A neighbour is a link in the route graph:
gates, admin links and an Anchor's or an unstable mouth's gates while they stand. A roaming
hole is not one: it is a pilot's secret and closes, so a hole found into a core would put
that core on the front line with nothing the defender could see coming. A corporation has no
allies yet; a faction's are those of `Stance::Ally`.

**D4. War starts with the economy.** A faction's strength is grown from its stock, so its
convoys and trade are the way in. Cutting supply weakens a holding before any siege.

**D5. Destroying is not taking.** Bringing down a claim ends it; control then needs a new
claim and the most influence (T2).

**D6. Building in a faction's space needs a standing threshold and a licence.** A licence
is a fee to the faction's treasury. A licensed structure is under the faction's law: its
police guard it, and attacking it is a crime. Building without a licence is **trespass**:
the structure is free to shoot (L2), and the faction may besiege it like a rival's outpost.
Corporation structures follow the same rules.

### The code before the slices

| Rule | Today | What changes |
|---|---|---|
| D1 | `StepUpkeep` (`Simulation_Sieges.cpp`): a supplied faction outpost mends `upkeep.repair` (2 %) of its hull per period; an unsupplied one decays. Player structures never mend. Blueprints have `hull`, no tier. | Blueprints gain `tier`; the tier's mend rate goes in `blueprints.json`. Mending runs for every finished structure: in the coarse pass when the system is cold, per tick when it is hot. Upkeep keeps the decay of an unsupplied faction outpost. |
| D2 | `CallPolice` (`Simulation_Agents.cpp`): up to `POLICE_RESPONSE = 2` police ships already in the system, warped in. Siege contingents are lent by holdings within `siege.reach` (3 hops). | Section 5's escalation for crimes; for an attack on a structure, the owner's own ships first, then lent from holdings in reach (the `Plan::Lent` path). |
| D3 | A faction besieges an enemy outpost "next door" to a holding of its own (`Simulation_Factions.cpp` ~775), so NPC sieges already obey a front of sorts. A player's guns obey nothing. | One predicate, `FrontOpen(system, side)`, read by the siege planner and by D1 (it binds sieges and claims, never a player's guns: Decided after the rounds, 3). |
| D4 | Already true: `FactionMind::stock` grows strength, convoys carry it (#407), sieges cost stock. | Nothing; kept as a rule so later work does not add free strength. |
| D5 | Already true: a claim's outpost destroyed shakes the claim (`ShakeClaim`); `StepControl` needs an outpost to claim. | Extended to corporations with T2. |
| D6 | `PlacementProblem` (`Simulation_Build.cpp`): placement rules and builders only. `LawOver`: a player's structure is under the controller's law if the system is claimed and lawful. | A licence record per (faction, owner); `PlacementProblem` checks standing and a licence in faction space; `LawOver` treats an unlicensed structure as lawless; the siege planner may target a trespassing player structure. |

---

## 5. Law, police and PvP

### Rules

**L1. PvP is allowed, and it is a crime wherever law holds.** A lawful system is one held
by a lawful faction (`claimed` and `Factions::IsLawful`).

**L2. What is a crime.** In a lawful system, any attack on someone not themselves flagged:
a player, an independent trader, a lawful NPC, a licensed structure. Firing on pirates,
flagged criminals, declared war enemies and unlicensed structures is free.

**L3. The response scales with security, always from real forces.** There is no invincible
police.
- Every free police ship in the system warps to the crime at once.
- Stations launch extra patrols if the faction has the presence to spare.
- Station and structure guns join in at security 0.5 and above.
- In high security the numbers make the response near-certain; a big enough fleet could in
  principle fight it.

**L4. Consequences of a crime:**
- a **criminal flag** for about 15 minutes, renewed by each crime: anyone may shoot the
  pilot without a crime, and faction docks refuse them;
- a **standing loss** with the system's owner (today's `ChargeAttack`);
- a fall in the pilot's **security status**, which recovers with time and with pirate kills.
  At low status, police in high security engage on sight.

**L5. Security** is the base value, plus the owner's paid police, minus nearby pirates. It
drives the strength of the police response, pirate spawns and convoy safety, and lower
security pays better (missions, bounties, ore). The details are to be refined later.

### The code before the slices

| Rule | Today | What changes |
|---|---|---|
| L1 | *Done in slice 10.* `StepPlayerFire` takes another pilot's ship (`PilotTarget`: in the system, in space, not in cover, alive) and `FirePilot` applies the hit to that session's ship from live positions; a kill is `ServerRespawnPlayer`'s loss on the world tick. An attack order chases a pilot with a follow hold. | War enemies (a later slice). |
| L2 | *Done in slice 10 for pilots.* `IsCrime(st, by, victim, law)` decides ships, structures and pilots: a pilot is under the law of the system (lawful holder, security > 0) unless flagged or an enemy of that law. | Licences (slice 6) and war. |
| L3 | *Done in slice 9.* `CallPolice` answers every crime by the system's security band (`world_rules.json` -> `law`): its free police warp in, a station launches patrols from spare presence then from stock, police within reach are lent as a `Plan::Kind::Response` and go home as Returns; `GunsOn` brings station and faction-structure guns in on a criminal at >= `gunsFrom`. | Paid police from the treasury (slice 11). |
| L4 | *Done in slice 10.* `Player` carries `securityStatus` and `criminalLeft` (seconds, saved with the account, run down by `StepPilotLaw` only while the pilot is in the world); `MarkCrime` flags and charges status beside `ChargeAttack`; lawful docks refuse a flagged pilot; police (`NpcHunts`) chase one, and engage on sight at a low status (`PoliceOnSight`); a flagged pilot's ship lingers `flagLinger`. The snapshot carries both for every pilot. | -- |
| L5 | *Done in slice 11.* `SecurityTarget` = base + `SECURITY_SWING` (0.15) x the police/pirate balance + what the holder pays (`world_rules.json` -> `security`: `policeSwing` x budget / (budget + `policeHalf`)); drift 5 % a pass. A corporation sets a budget per held system (`CorpAct` "police"); `StepPolicePay` takes it from the treasury every macro pass, hot or cold, into the aggregate's `paidPolice`, which flies one police ship per `shipHour` (the role `police`'s faction) and makes the corporation's law the system's (`LawIn`). Of the pirates' strength, `pirateHide` x security lies low; convoys read security (`convoys.policed`); missions, jobs, contract premiums (`payAtZero`) and ore (`oreAtZero`) pay more the lower it is. | Factions pay for their police in presence, as before; a faction treasury buying police is not built. |

**Numbers (tuning, `world_rules.json` → `law`, in place since slice 10):** flag 15 minutes; security status
from -10 to +10, -1 for a crime, -2.5 for a kill, +0.1 per pirate kill, +0.5 per day without
a crime; police engage on sight at status -5 or below in systems of 0.5 and above.

---

## 6. Corporations' own assets

**C1. Corporation patrols.** A corporation may keep AI ships that guard its structures while
members are away. Officers (CEO and directors, `Corp::Registry::Manages`) set where they
stand and what they engage, from the rules of engagement structures already use (#340).
The server sets the limits -- ships per corporation, per system, and upkeep from the
corporation's treasury -- in `world_rules.json` → `patrols`. Patrols are presence (T1) and
count toward influence. They are cold arithmetic in a cold system like any NPC (#295).

**C2. Attacks are told to offline owners.** When a structure is attacked and its owner is
not on, a notice is kept for them (and for their corporation's members) and shown in the
journal on login. A Discord hook is a later slice.

### The code before the slices

- `Simulation_Defence.cpp` keeps `OwnerBook`s in `world.json` (`owners`): standings and the
  crimes a structure committed while its owner was away, settled at login by `StepOwners`.
  Attack notices are not a store of their own (owner, 2026-10-22): they are journal entries
  (`structure_attacked`). An attack under way is kept per structure in `world.json`
  (`attacks`) and told once when it is over; a corporation member's goes into the
  corporation's journal (`corps.json`), which members read at once or at login; anyone
  else's offline entry goes into the pending part of their journal in the account save.
- *Done in slice 12 (C1).* A corporation's patrol is `Corps::Patrol` on its record in
  `corps.json` (ships, design, rules of engagement), set by a `CorpAct` "patrol"; what stands
  there is presence, `SystemAggregate::patrols` by corporation in `world.json`. Where somebody
  is, it flies as `NpcRole::Guard` ships: no faction's, its corporation's (`NpcShip::GetCorp`),
  picking fights by `GuardHunts`/`GuardEngages` from the same rules a structure keeps.
  Follow-ups (owner, 2026-10-23, #186): the verb, the corporation window and `corp_act` set the
  rules' threshold and allow-list too; a ship it makes good is bought from the treasury at
  `DesignPrice` (what a pilot pays for that hull so fitted), and with no money for it the patrol
  waits, said once; a faction weighing an eviction counts the owner's patrol as a garrison of as
  many warships as its design is a match for (`PatrolAsWarships`); and at start a member the
  world never saw online is noted from their account file (`Host::NoteUnseenMembers`, read
  only, another world's or a damaged file skipped), so a corporation's summed standing counts
  them.

---

## 7. The map over time

### Rules

**M1. Three layers, three lifetimes.**

| Layer | What | Lifetime |
|---|---|---|
| Geography | Place, links, star and planets, name | Known for good once known |
| Live | Owner and security, on the public charts | Always current for every system on the pilot's charts |
| Observed | Structures, stations, markets | As last observed, with the date; live only where the pilot, their corporation or their structures are present |

**M2. Chart trading is built into the map, not an item** (slice 16). Docked at a faction's
station, the map's Charts tab lists what that faction would pay for each system on the
pilot's own charts, and sells one or all. Factions buy; selling to players is its own
mechanic (#473).

*Decided by the owner (2026-10-23, #346):* what is sold is the system (that it exists, how to
reach it) and what the pilot observed there; stations and structures raise the price, scanner
finds (wormholes, hidden bases) more. Fresh and rare is worth most; what the buyer knows is
worth almost nothing and cannot be sold to it again; an update sells cheaply, for what is new
only. A sale writes into the faction's intel and pays by how much its picture changes.

*As built:* `Simulation::ChartOfferFor` prices it and `SellCharts` sells it
(`Command::sellChart`, a system or `*`). With `world_rules.json` -> `charts` as the numbers:
- `system` (300) when the faction has no intel on the system, unless it is public by place (a
  centre);
- when the pilot looked more recently than the faction did: `station` (150) for each station
  and `structure` (60) for each structure more or fewer than its intel says, halved every
  `halfLifeHours` (24) since the pilot looked;
- `find` (400) for each of the pilot's resolved signatures standing there that the faction was
  never told of (`FactionMind::finds`, saved; a hole closed is dropped from it);
- all of it over 1 + `rarity` (0.25) x the number of other factions with intel on it.

The sale writes the pilot's look into `Intel` (counts, its date, the outposts they saw, the
live owner and security; the geography on a first sale) and the finds into the mind, takes
the price from the faction's treasury -- a faction that cannot pay buys nothing, and says so
-- and charts the system for the buyer (`ChartBy`, K3). What it bought, it plans on: a fresh
picture is not worth surveying (`SurveyValue`), and outposts seen there are what a siege is
weighed for. The faction docked at is the buyer; where it holds the system or has ships it
sees for itself and buys nothing. Only the pilot's own record sells: a system they went to
themselves (a `system:` key of their own charts), not their corporation's charts. The galaxy
statistics carry, docked, each system's price and what it is (`GalaxySystemStat::chartPrice`,
`chartWhat`) and the buyer with its treasury; econagent's galaxy resource lists them and
`sell_charts` sells.

**M3. A drifting region.** Its charts stay, marked "drifted". When it returns, they are
marked outdated: the geography holds, the rest must be re-checked.

### The code before the slices

- `GalaxyState` (`Protocol.h`) sends live security, pirates, prosperity, controller and
  `claimed` once a second for every visited system. It becomes per session, for the
  systems that pilot knows.
- Nothing records what a pilot observed. The account gains, per known system, the time of
  the last observation and a compact list of what stood there (kind, owner, name). It is
  refreshed whenever the pilot or a corporation member is in the system, or a corporation
  structure stands there.
- A drifted region stays in `universe_` (cut off); an archived one is removed and its charts
  vanish with it (`Simulation_Drift.cpp`). Knowledge of archived systems is kept in the
  account by id; when the archive returns into another slot, the ids are renamed for the slot
  (`r<slot>-…`), so the archive keeps the old-to-new map and accounts translate on read.

---

## Saves and protocol

Each slice bumps what it changes, once. Additions an older reader can ignore need no bump
(CLAUDE.md, #20); a field that changes meaning does.

| File | Field | Slice |
|---|---|---|
| `world.json` (`WORLD_VERSION`) | per system: `chartedBy`, `firstContact {by, kind, at}`; `visited` translated on load | 1, 2 |
| | per system: `holderCorp`; owner policies (tax, docking) by holder | 5 |
| | licences: `{faction, owner, until}` | 6 |
| | `attacks`: attacks under way, per structure | 13 |
| | corporation patrols' presence, per system | 12 |
| account (`ACCOUNT_VERSION`) | `known`: per system {since, observedAt, observed[]}; `contacts` | 1, 14 |
| | `journal` (pending entries), `corpJournal` {corp, seen} | 13 |
| | `securityStatus`, `criminalUntil` (world time) | 10 |
| `corps.json` (`CORPS_VERSION`) | shared knowledge (derived from members, not stored), `treasury`, `policies`, `patrols` | 1, 5, 12 |
| | `journal` per corporation | 13 |
| region documents (`GENERATOR_VERSION`) | `natives.names` per system | 3 |
| `data/world_rules.json` | `law`, `influence`, `licences`, `patrols`, `security` | 5, 6, 10, 11, 12 |
| `data/blueprints.json` | `tier` per blueprint; mend rate per tier | 7 |
| beside `world.json` | `names_blocked.txt` | 4 |

Protocol (`PROTO_VERSION`, bumped by each slice that changes a message):
- `SystemInfo` gains `chartedBy`, `observedAt`, `drifted`/`outdated`; the index and
  `GalaxyState` become per session (1, 14, 15).
- `GalaxySystemStat` gains the holder corporation and its tax (5).
- The snapshot carries each ship's criminal flag, and the pilot's own security status (10).
- New commands: set policy (tax, docking, police budget), buy a licence, set patrols
  (5, 6, 11, 12). Corporation-wide ones are `CorpAct` verbs, as #347 does.
- `Compat::SharedFiles` stays as is unless a new data file both sides must agree on is added.

The agent (`econagent`) gets each verb and each new field in the same slice, and
`docs/agents/reference.md` is regenerated with it.

---

## Decided after the rounds

The owner settled these on 2026-10-22, taking the recommendations put to them.

1. **Factions name what they chart.** An inhabited system keeps its native culture's name.
   An empty system first charted by a major faction takes a name in that faction's style.
   Nobody renames either.
2. **Corporation–faction relations.** A corporation's standing with a faction is the sum of
   its members' standings. It decides licences, docking and whose police guard its
   structures. Pilots keep their own standings (#347). Full diplomacy comes later.
3. **The front line binds sieges and claims, not guns.** It decides whether a structure may
   be besieged or a claim laid. Firing at ships is never limited by it.
4. **A licence is a rent**, paid each week from the owner's or corporation's money. It lapses
   to trespass after a grace day unpaid. The owner chose it as the more MMO-like option.
5. **Law in a corporation's system.** A corporation that pays police in a system it holds is
   a source of law there. The same rules apply, at the security its police buy.
6. **A criminal who logs off.** The ship stays in space for a minute after disconnecting, as
   in EVE. The criminal flag keeps its remaining time, and that time runs down once the
   pilot is back.
7. **War declarations** are a later slice. Until then, players attack each other without a
   crime only where there is no law (security 0).
8. **Chart trading** is slice 16, built into the map interface (M2).

---

## Slice plan

Each slice is its own PR with green CI, and the game works after every one. Dependencies
are marked; slices without one may go in any order.

**Knowledge and names**

1. **Per-pilot and corporation knowledge, public charts.**
   - *Goal:* a pilot sees public charts ∪ their own ∪ their corporation's.
   - *Scope:* account `known`; `KnownUniverseFor` and `BuildGalaxyState` per session, built on
     change; the public set is the centres plus `chartedBy`; old `visited` translated on load.
     First into a system adds it to the pilot's knowledge, not everyone's. The agent's index
     follows.
   - *Tests:* two pilots, one visits: only that one (and a corp mate) has the system; a third
     pilot's index is unchanged; a save of an old world loads with the same public map;
     galaxy stats are not sent for unknown systems.
2. **Faction charting and first contact.** *Depends on 1.*
   - *Goal:* fixes #431's playtest bug.
   - *Scope:* `ChartBy` from survey, settle, Anchor, claim and expedition arrival;
     `firstContact`; chronicle lines; "charted by <Faction>" on the map and in the agent.
   - *Tests:* a faction settles an unvisited system: it is public, charted by it, and the
     first pilot in is not its discoverer; a system no faction reached is still a pilot's find.
   - *Built:* `ChartBy` from surveys, moves and sieges arriving, sites laid, claims
     (`TakeControl`), convoys and faction Anchors; a region's own natives chart nothing for the
     centres, and nobody charts a centre. First contact is not a stored record: it is whoever
     came first, `discoverer` (a pilot) or `chartedBy` with no discoverer (a faction), and the
     news (`contact` in the chronicle). Its names -- the natives' or the major's -- are kept in
     `givenName`, which `NameSystem` already refuses to overwrite. `WORLD_VERSION` 17 charts a
     version 16 world's held systems by their holders; `PROTO_VERSION` 53.
3. **The native name generator.** *Generator part independent; the reveal depends on 2.*
   - *Scope:* per-culture system names in `gen/`, written to `natives.names`; bump
     `GENERATOR_VERSION` and the golden hashes; the names replace designations for whoever
     has made first contact.
   - *Tests:* the same seed gives the same names on three compilers; names are unique in the
     region; before contact a pilot sees designations, after it names.
   - *Built (generator part):* cultures are data, `cultures` in `data/factions.json` (sounds,
     syllable counts, endings, name patterns), one per native theme plus one per major
     (`culture` on the faction); `gen/Names.h` speaks them (`CultureName`, and
     `ChartedName` for slice 2's `ChartBy`), with a short built-in word blocklist. Regions
     write `natives.culture` and `natives.names` (`GENERATOR_VERSION` 10); the index keeps
     the designation until the reveal.
4. **Naming rules.** *Depends on 2, 3.*
   - *Scope:* `NameSystem` refuses inhabited systems and everyone but the discoverer; the
     word filter (`names_blocked.txt`); console and editor `rename`; the agent's tool says the
     rules.
   - *Tests:* the owner of a system cannot name it; a filtered word is refused; an admin
     rename reaches every client's index.
   - *Built:* `Simulation::NameProblem` is the one rule set (shape, `Gen::ChosenNameBlocked`
     with the server's list, unique against every designation, given name and native name, revealed or
     not), used by `NameSystem` and `RenameSystem`; `NameSystem` also refuses a system its
     people named. `names_blocked.txt` is read with the allowlist and bans
     (`Host::AccessList`, re-read on change). The console's `rename <system> <name|->` (so
     `econserver admin <dir> rename ...` too) and R on a saved world's region map in the
     editor, which sends that command through the admin channel (now in the engine,
     `core/AdminChannel.h`), so the server is still the one that names: a chronicle entry
     (`renamed`) and a journal entry for the discoverer, at once or at login (#445's pending
     journal). `-` clears a given name back to its people's or its designation, and the
     discoverer may name it again. The client's name field and econagent's `name_system` say
     which rule a name breaks before it is sent. The filter of a chosen name matches whole
     words (split on spaces, hyphens, apostrophes and digits), so "Parsec", "Torpedo" and
     "Peacock" are names; only a few unambiguous entries, and a `*word` line of
     names_blocked.txt, match anywhere. The generator keeps its own substring rule
     (`Gen::NameBlocked`): a throwaway draw costs nothing, and its output is sealed.

**Territory**

5. **Influence and claims.** *Depends on #437 (players build outposts).*
   - *Scope:* influence per faction and per corporation in the profile; `holderCorp`;
     `StepControl` with corporations as parties; the corporation treasury; the owner's tax
     and docking policies; the market fee goes to the owner (closes #438's fee item).
   - *Tests:* a corp's outpost plus influence takes an unheld system; destroying the outpost
     drops the claim; tax set by the owner is charged and credited; docking policy is honoured.
6. **Licences and standing gates.** *Depends on 5.*
   - *Scope:* buying a licence; `PlacementProblem` checks standing and licence in faction
     space; `LawOver` treats unlicensed as lawless; the siege planner may target a trespasser.
   - *Tests:* below the threshold a build is refused with the reason; licensed, it is guarded
     and attacking it is a crime; unlicensed, it is free to shoot and gets besieged.
   - *Built:* a licence is a party's (a corporation, or a pilot in none) per system, kept on
     the corporation's record (`corps.json`) or the pilot's book of owners (`world.json`),
     `{holder, until, told}` in world time. Bought where it is granted (#509), with
     `Command::buyLicence` docked at a station of the holding faction or at a structure of
     the holding corporation (`Simulation::LicenceDock`), by the pilot or the corporation's
     CEO or a director (from the treasury); the snapshot's `LicenceView` says where and
     whether the pilot is there (`where`, `atHolder`), the station window offers it only
     there, a refused build says who grants it, the price and where to buy, the corporation
     window (a pilot in none: the same view) lists what the party holds with Renew, and the
     galaxy statistics carry the terms of every system (`licenceBy`, `licenceRent`) for the
     map. Rent (`licences` in
     `world_rules.json`: 2000 cr a week, a day's grace, standing 10) is taken in the coarse
     pass; a corporation holder admits by its docking policy. A player structure already
     standing when licences arrived, or whose system changed hands, is asked rent from then,
     so it has the grace day before trespassing. Trespass is weighed by `Think` as a siege of
     the structure (`Plan::evicting`), worth `licences.evict`. A corporation holder's own rent
     price is a later choice; for now the rules' price serves every holder.

**Defence**

7. **Regenerating defences by tier.** *Independent (after #437).*
   - *Scope:* `tier` in blueprints, a mend rate per tier, mending for every finished structure
     hot and cold; upkeep keeps the unsupplied decay.
   - *Tests:* one frigate cannot bring down an outpost; enough cruisers can; a cold system mends
     at the same rate as a hot one over the same time.
8. **The front-line rule.** *Depends on 5, 7.*
   - *Scope:* `FrontOpen(system, side)`; the siege planner reads it; a claim may be laid only
     where the front is open; ships' fire is never limited (Decided after the rounds, 3).
   - *Tests:* a core system with only friendly neighbours is not besieged; once a border
     system falls, it is.

**Law**

9. **Escalating police from real forces.** *Independent.*
   - *Scope:* `CallPolice` sends every free police ship in the system; stations launch patrols
     from presence; station and structure guns at security ≥ 0.5; numbers from security.
   - *Tests:* the response grows with security and with the faction's presence; a system with
     no police sends nobody; the ships come out of the aggregate and go back into it.
10. **PvP with criminal flags and security status.** *Depends on 9.*
    - *Scope:* players as targets in `StepPlayerFire`; `IsCrime`; the flag, docks refusing it,
      the status and its recovery; the flag in the snapshot; the client's overview and target
      panel show it; the agent's tools.
    - *Tests:* shooting a pilot in a lawful system flags the shooter and brings police;
      shooting a flagged pilot is free; low status is engaged on sight in high security;
      lawless space charges nothing.
11. **Security effects.** *Depends on 5, 9.*
    - *Scope:* paid police from the owner's treasury raise the security target; pirate spawns,
      convoy losses and rewards read security.
    - *Tests:* paying police raises security over passes; rewards scale down as security rises;
      a 24 h `--news` run stays stable.
    - *Built:* a corporation's police budget per held system (`CorpAct` "police", in
      `corps.json`, shown in the corporation window and econagent's `corp`), paid each macro
      pass from the treasury and stopped, with a journal entry, when it runs dry; while paid the
      system is lawful under the hired police (`LawIn`: `IsCrime`, `LawOver`, a structure
      firing first) and the director flies them. Pirates lie low with security, and the pay of
      missions, jobs and contract premiums and the yield of ore rise as it falls
      (`world_rules.json` -> `security`). The same arithmetic cold and hot.

**Corporations**

12. **Corporation AI patrols.** *Depends on 5 (treasury), 10 (who is hostile).*
    - *Scope:* patrol settings by officers, server limits, upkeep; patrols as presence; cold
      arithmetic like any NPC.
    - *Tests:* limits are enforced; a patrol guards a structure while its members are offline;
      it does not fire on corporation members.
    - *Built:* `CorpAct` "patrol" (system, ships or none, design, engage), by the CEO or a
      director, where the corporation holds the system or a member has a structure there;
      at most `systemMax` ships in a system and `shipsMax` in all (`world_rules.json` ->
      `patrols`). The design is a name in the officer's library or a stock id, and must carry
      guns; none named, the default doctrine's new `guard` role (`data/ships.json`). Every
      macro pass, hot or cold, `StepPatrols` takes the upkeep from the treasury -- `shipHour`
      a ship of the default guard, scaled by the design's dps x hull -- and a paid patrol
      makes good a ship every `regrowHours`; unpaid, or with nothing left to guard, it stands
      down and the corporation's journal says so once. Its ships are presence: influence at
      a faction ship's weight; in the cold pass they trade shots with pirates at the police's
      meeting rates; at a member's besieged structure they stand with the garrison in the
      siege's attrition and tie up besiegers (both by their designs' `Fighter`s, #186). Where
      somebody is, `FieldPatrols` flies them as guards around the members' structures, counts
      the ones shot down off the number, and lets go what the number no longer covers. A guard
      never fires on a member, answers whoever shot a member, a member's structure or the
      patrol (the provoked books), and fires first as its mode says; a first strike is the
      CEO's crime, as a structure's is its owner's. Shooting a guard is a crime under the
      system's law, as shooting a member's structure is. The corporation window lists each
      patrol with a Stand down button, `/corp patrol` sets one; econagent's `corp`,
      `corp_act` and full `observe` carry them. PROTO_VERSION 59. Follow-ups in #467
      (PROTO_VERSION 60): the faction siege planner weighs a patrol when it decides to evict
      (`PatrolAsWarships`), a ship made good is bought from the treasury at `DesignPrice`, the
      verb sets the threshold and allow-list, and unseen members are counted at start.
13. **Offline attack notices.** *Independent; small, may go early.*
    - *Scope:* journal entries, merged per structure per attack: the owner's pending journal
      in the account save, or the corporation's journal its members read; capped.
    - *Tests:* an attack while offline is in the journal on login, once; a long siege is one
      merged notice, not hundreds.

**The map over time**

14. **Map staleness.** *Depends on 1.*
    - *Scope:* the observed layer per known system with its date; live where the pilot, their
      corporation or their structures are present; the map shows "seen <date>".
    - *Tests:* a station built after a pilot left is not on their map; it appears when they
      or a corp mate return; owner and security stay live.
15. **Drift marks.** *Depends on 1, 14.*
    - *Scope:* charts of a drifted or archived region kept, marked drifted; on return, renamed
      ids translated and marked outdated until re-observed.
    - *Tests:* archive and return: the pilot still knows the geography under the new ids, and
      its observed layer is outdated.
    - *Built:* a drifted region's systems say `drifted` in the pilot's index; an archived
      region's charts are renamed into `a<archive>:<id>` (no system is called that, and its slot
      may take a region whose ids are the same) and shown, drifted and unlinked to the galaxy,
      from the archive's own geography while the slot is empty. On return they are renamed to
      the new ids and every sighting of them is `outdated` until looked at again. The world
      keeps the renamings (`archive.renames` in world.json); sessions and corporations are
      renamed at once, and an account saves how many it has seen (`chartRenames`) and is read
      through the rest. The map draws such a system faint, and the map and econagent's galaxy
      resource say "drifted away" and "before it drifted: outdated".
16. **Chart trading in the map UI.** *Depends on 1, 14.*
    - *Scope:* factions buy (players: #473) what a pilot has of a system, by how much it
      changes their intel; the map's Charts tab, a command, the agent's tool.
    - *Tests:* an unknown system sells; the same again pays nothing and is refused; an update
      pays only for what is new; scanner finds raise the price; another faction still pays;
      the faction's intel sees what it bought.
    - *Built:* see M2. `PROTO_VERSION` 61; `world.json` gains `finds` per faction mind and
      `structures` per intel, which an older reader ignores, so `WORLD_VERSION` stays.

Related work outside this plan: faction shipyards by need and the player shipyard upgrade
(#432 part D, with #433); the remaining #438 items (station perks; storage, refunds over
every tier and client docking at a corp mate's outpost are done).

---

## Structure ownership (#468) -- built

Decided by the owner on 2026-10-23 and built: a structure a corporation member builds belongs
to the **corporation** (owner `corp:<id>`, beside a faction's `faction:<id>`) and stays with it
when the member leaves; one built before joining stays the pilot's until they hand it over.
`Simulation::StructureParty` is the party it pays its licence as -- the corporation, or the
pilot's own book -- no longer the owner's current corporation.

- **Transfer** (`CorpAct` "transfer": a structure's id, its system, and a pilot or a ticker):
  a pilot hands over their own, a corporation's CEO or a director the corporation's; one verb
  for pilot or corporation to pilot or corporation. A pilot receiving one must be online.
- **The licence moves with it**, paid-to and owed alike, unless the new party has one there
  already; the old party keeps its own only while something else of its stands there. The
  holder is asked again (`Admits`): a transfer to a party it would refuse is refused.
- **What stays with the structure**: its storage (#438), its rules of engagement (#340), and
  an attack under way, whose end is told to the new owner (slice 13).
- **A disbanded corporation** leaves its structures *abandoned*: no licence or rent, no
  patrol, no law over them (`LawOver`), no fire from them, no fee from their markets. Anyone
  may shoot one down, or **take** it (`CorpAct` "take"): from a ship in space within its
  blueprint's reach, by the holder's leave and licence as building there is, for the taker's
  corporation if they are in one.
- **An older world** (`Save::WORLD_VERSION` 17 or less): a structure whose owner was a
  corporation member is the corporation's on load, which is what it was in effect.

The details the decisions left free are in [DECISIONS.md](../DECISIONS.md) (2026-10-23, "Whose
a structure is").

## Open design questions

Not built and not yet designed in full. Each waits for a talk with the owner.

- **NPC fits from the living world (#464).** Since #463 NPCs fight by the player combat model
  with one doctrine design per role. Their fits should come from the world instead: a gang's
  wealth and losses, its faction's doctrine, the region, its security and its market. No balance
  patch in the meantime.
- **The corporation enemy list (#466).** Today a corporation's hostility to a faction is its
  members' summed standing. An EVE-style contact list set by the CEO and directors, read by
  patrols and structures in "enemies" mode, comes with the war declarations that this plan
  deferred (Decided after the rounds, 7).
- **Selling charts to players (#473).** A player-to-player exchange of charts; slice 16 sells
  to factions only.

---

## How others do it

| Game | How it works | What we take |
|---|---|---|
| EVE Online | Security status, criminal and suspect flags, CONCORD, sovereignty by structures, "reinforced" timers | Flags, status and docks refusing criminals; sovereignty by structures. **Not** the invincible CONCORD and **not** the timers: our police are real ships, and defence is regeneration and a front line |
| EVE (wormhole space) | No local, no public map; what you know you scanned | Personal knowledge for unreached space, and scanning (#346) |
| Elite Dangerous | First discoverer tagged on the system; Universal Cartographics buys exploration data | The chronicle entry and selling charts, here to factions and built into the map |
| Stellaris | First contact as an event; natives with their own names | First contact reveals the natives' names |
| Foxhole | Territory taken only from adjacent territory, along a front | The front line: sieges only from a held or neutral neighbour |
