# World knowledge, territory and law

Status: **decided by the owner in four rounds, 2026-10-21 and 2026-10-22**, recorded in the
comments on #431. This file gathers those decisions as rules, maps each one onto today's
code, and cuts the work into slices. It is waiting for the owner's approval; nothing in it is
built yet. Related: #342 (the growing world, `world_generation_v2.md`), #347 (corporations),
#432 (the playtester fork: routes, faction shipyards), #433 / #437 (players build outposts),
#438 (what player outposts still lack).

The decisions are not re-opened here. Where a decision leaves a number or a detail free,
the doc proposes one and marks it as tuning; what is still genuinely undecided is listed
under **Open questions**.

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

### Today's code

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
history.

**N3. Names have rules:** 3 to 24 characters of letters, digits, spaces, `'` and `-`
(today's `Names::ValidSystemName`), unique in the world regardless of case, through a word
filter, and subject to admin moderation: an admin may rename or clear any player-given name.

### Today's code

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
- **Majors** (centre factions) also need a naming culture if they name what they chart: see
  Open questions.
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

### Today's code

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

**D2. The response escalates, from real nearby strength.** An attack on a structure calls
the owner's ships in the system, then lends from holdings within reach -- the machinery a
siege already uses, run the other way. A faction with nothing nearby sends nothing.

**D3. The front line.** A system may be besieged only from a held or neutral neighbour: the
attacker (or an ally) holds a gate-adjacent system, or one of them is nobody's. A faction's
core -- systems whose every neighbour is its own -- cannot be besieged until a border system
falls first.
*Why:* this defends core territory by geography instead of a "cannot be taken" flag: a big
enough fleet takes the border, then the next system.

**D4. War starts with the economy.** A faction's strength is grown from its stock, so its
convoys and trade are the way in. Cutting supply weakens a holding before any siege.

**D5. Destroying is not taking.** Bringing down a claim ends it; control then needs a new
claim and the most influence (T2).

**D6. Building in a faction's space needs a standing threshold and a licence.** A licence
is a fee to the faction's treasury. A licensed structure is under the faction's law: its
police guard it, and attacking it is a crime. Building without a licence is **trespass**:
the structure is free to shoot (L2), and the faction may besiege it like a rival's outpost.
Corporation structures follow the same rules.

### Today's code

| Rule | Today | What changes |
|---|---|---|
| D1 | `StepUpkeep` (`Simulation_Sieges.cpp`): a supplied faction outpost mends `upkeep.repair` (2 %) of its hull per period; an unsupplied one decays. Player structures never mend. Blueprints have `hull`, no tier. | Blueprints gain `tier`; the tier's mend rate goes in `blueprints.json`. Mending runs for every finished structure: in the coarse pass when the system is cold, per tick when it is hot. Upkeep keeps the decay of an unsupplied faction outpost. |
| D2 | `CallPolice` (`Simulation_Agents.cpp`): up to `POLICE_RESPONSE = 2` police ships already in the system, warped in. Siege contingents are lent by holdings within `siege.reach` (3 hops). | Section 5's escalation for crimes; for an attack on a structure, the owner's own ships first, then lent from holdings in reach (the `Plan::Lent` path). |
| D3 | A faction besieges an enemy outpost "next door" to a holding of its own (`Simulation_Factions.cpp` ~775), so NPC sieges already obey a front of sorts. A player's guns obey nothing. | One predicate, `FrontOpen(system, side)`, read by the siege planner and by D1 (see Open questions for how it binds a player's guns). |
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

### Today's code

| Rule | Today | What changes |
|---|---|---|
| L1 | `StepPlayerFire` (`Simulation_Player.cpp` ~461) accepts only an NPC or a structure as a target: there is no PvP. | It accepts a player ship (one not docked, in range, not hidden), and the hit is applied to that session's ship by the server. Prediction is untouched: a hit is never computed from an interpolated position (CLAUDE.md, #157). |
| L2 | `ChargeAttack` charges standing and bounty for hitting a lawful faction's NPC; `LawOver` decides it for structures. | One `IsCrime(attacker, victim, system)` used for ships, players and structures, reading flags, war and licences. |
| L3 | `CallPolice`: police already in the system and hostile to the shooter, at most two, warped beside them; `POLICE_CALL_SECONDS` between calls. Stations fire on the hostile (`StepStationDefence`). | All free police in the system; a station launches patrols from the faction's presence there (made real by the spawn director, counted out of the aggregate); guns at ≥ 0.5. |
| L4 | Bounty per faction (`Account::bounty_`), standing, `AccountHostileToFaction` (pirates, wanted, hostile tier). No flag, no status. | A `criminalUntil` and a `securityStatus` on the account; the flag in the snapshot so every client can show it; docks read it. |
| L5 | After #434, `SecurityTarget` = base + `SECURITY_SWING` (0.15) x the police/pirate balance; drift 5 % a pass. | A paid-police term, from the owner's treasury, added to the target; pirate spawns, convoy losses and rewards read security. |

**Proposed numbers (tuning, `world_rules.json` → `law`):** flag 15 minutes; security status
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
not on, a notice is kept for them (and for their corporation's officers) and shown in the
journal on login. A Discord hook is a later slice.

### Today's code

- `Simulation_Defence.cpp` keeps `OwnerBook`s in `world.json` (`owners`): standings and the
  crimes a structure committed while its owner was away, settled at login by `StepOwners`.
  Offline notices go into the same book (`notices`, capped) and are posted to the journal by
  the same step.
- NPC ships are `NpcShip` with a faction and a role; a corporation patrol needs an owner
  that is a corporation, not a faction. It is a new role whose standings and allow list come
  from the corporation's settings, not the faction registry.

---

## 7. The map over time

### Rules

**M1. Three layers, three lifetimes.**

| Layer | What | Lifetime |
|---|---|---|
| Geography | Place, links, star and planets, name | Known for good once known |
| Live | Owner and security, on the public charts | Always current for every system on the pilot's charts |
| Observed | Structures, stations, markets | As last observed, with the date; live only where the pilot, their corporation or their structures are present |

**M2. Chart trading is built into the map, not an item.** It is its own task and needs
careful design (slice 16).

**M3. A drifting region.** Its charts stay, marked "drifted". When it returns, they are
marked outdated: the geography holds, the rest must be re-checked.

### Today's code

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
| | `owners.<account>.notices` (offline attack notices) | 13 |
| | corporation patrols' presence, per system | 12 |
| account (`ACCOUNT_VERSION`) | `known`: per system {since, observedAt, observed[]}; `contacts` | 1, 14 |
| | `securityStatus`, `criminalUntil` (world time) | 10 |
| `corps.json` (`CORPS_VERSION`) | shared knowledge (derived from members, not stored), `treasury`, `policies`, `patrols` | 1, 5, 12 |
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

## Open questions

These are not decided; each names a recommendation for the owner to accept or change.

1. **Do factions name what they chart?** N2 gives naming to the discoverer. When the
   discoverer is a major faction, the system could take a name from that faction's culture,
   or keep its designation for ever. *Recommendation:* majors get a naming culture and name
   what they chart first; nobody renames it.
2. **Corporation–faction relations** (round 2, left open). Does a corporation have a
   standing of its own with factions, or is it its members' (for licences, docking, police)?
   *Recommendation:* the licence is held by the corporation and the standing threshold is
   checked against the officer who buys it; standings stay each pilot's (#347).
3. **How the front line binds a player's guns.** For NPC sieges D3 decides whether a siege
   may be laid. For players there is no siege plan, only fire. *Recommendation:* behind the
   line, a structure's mend rate is doubled; the line decides how hard, never whether.
4. **Is a licence a one-off or a rent?** *Recommendation:* a rent, paid each period from the
   owner's (or corporation's) money, lapsing to trespass after a grace day.
5. **Law in a corporation's system.** L1 makes law a lawful faction's. Is a corporation that
   pays police a source of law? *Recommendation:* yes, in systems it holds, at the security
   its police buy, with its standings toward the attacker deciding "hostile".
6. **A criminal who logs off.** Does the flag keep running, and does the ship stay in space?
   *Recommendation:* the flag counts world time, so it runs out offline; a ship that
   disconnects flagged lingers in space for the flag's remaining time rather than the usual
   linger (`ClientSession::lingerLeft`, `StepLinger`), so logging off is no escape.
7. **War declarations.** L2 frees fire on "declared war enemies", and nothing declares war
   today. *Recommendation:* corporations declare war on each other for a fee, a later slice;
   factions' stances are their war.
8. **Who may sell charts, and what a sale reveals.** Waits for slice 16's design.

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
3. **The native name generator.** *Generator part independent; the reveal depends on 2.*
   - *Scope:* per-culture system names in `gen/`, written to `natives.names`; bump
     `GENERATOR_VERSION` and the golden hashes; the names replace designations for whoever
     has made first contact.
   - *Tests:* the same seed gives the same names on three compilers; names are unique in the
     region; before contact a pilot sees designations, after it names.
4. **Naming rules.** *Depends on 2, 3.*
   - *Scope:* `NameSystem` refuses inhabited systems and everyone but the discoverer; the
     word filter (`names_blocked.txt`); console and editor `rename`; the agent's tool says the
     rules.
   - *Tests:* the owner of a system cannot name it; a filtered word is refused; an admin
     rename reaches every client's index.

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

**Defence**

7. **Regenerating defences by tier.** *Independent (after #437).*
   - *Scope:* `tier` in blueprints, a mend rate per tier, mending for every finished structure
     hot and cold; upkeep keeps the unsupplied decay.
   - *Tests:* one frigate cannot bring down an outpost; enough cruisers can; a cold system mends
     at the same rate as a hot one over the same time.
8. **The front-line rule.** *Depends on 5, 7.*
   - *Scope:* `FrontOpen(system, side)`; the siege planner reads it; a player's attacks per the
     answer to Open question 3.
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

**Corporations**

12. **Corporation AI patrols.** *Depends on 5 (treasury), 10 (who is hostile).*
    - *Scope:* patrol settings by officers, server limits, upkeep; patrols as presence; cold
      arithmetic like any NPC.
    - *Tests:* limits are enforced; a patrol guards a structure while its members are offline;
      it does not fire on corporation members.
13. **Offline attack notices.** *Independent; small, may go early.*
    - *Scope:* `OwnerBook.notices`, posted to the journal at login for the owner and the
      corporation's officers; capped and merged per structure.
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
16. **Chart trading in the map UI (design only).** *Depends on 1, 14.*
    - *Scope:* a design document and an owner discussion; no code. What is sold (systems,
      layers, a region), to whom (factions, players), at what price and with what proof.

Related work outside this plan: faction shipyards by need and the player shipyard upgrade
(#432 part D, with #433); the remaining #438 items (storage, station perks, refunds, client
docking at a corp mate's outpost).

---

## How others do it

| Game | How it works | What we take |
|---|---|---|
| EVE Online | Security status, criminal and suspect flags, CONCORD, sovereignty by structures, "reinforced" timers | Flags, status and docks refusing criminals; sovereignty by structures. **Not** the invincible CONCORD and **not** the timers: our police are real ships, and defence is regeneration and a front line |
| EVE (wormhole space) | No local, no public map; what you know you scanned | Personal knowledge for unreached space, and scanning (#346) |
| Elite Dangerous | First discoverer tagged on the system; Universal Cartographics buys exploration data | The chronicle entry and selling charts, here to factions and built into the map |
| Stellaris | First contact as an event; natives with their own names | First contact reveals the natives' names |
| Foxhole | Territory taken only from adjacent territory, along a front | The front line: sieges only from a held or neutral neighbour |
