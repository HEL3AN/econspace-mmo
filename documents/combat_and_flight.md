# Combat and flight

Status: **accepted by the owner** (#619, every recommendation). Slice 1, lock before fire, is built;
the rest of the slice plan (section 5) is not. This is the design that
corporation wars (#574) rest on, and the forks that change the game's feel are collected in
"Decisions for the owner" at the end; everything before that plans on top of the premises in
CLAUDE.md and does not re-open them: an MMO with an authoritative server, one combat model for
NPCs and players (#463, #186), agents as first-class players (#42), the overview as the
instrument (M9), allegiance only on instruments (#117), a system a million units across
(#159), and numbers that come from the world rather than from a balance pass.

Where this doc names a number it is a *proposal for tuning* and lives in data
(`data/ships.json`, `data/world_rules.json`), never beside the rule.

## Contents

1. Current state and its problems
2. How other games do it
3. The proposed whole
4. Netcode
5. Slice plan
6. Decisions for the owner

---

## 1. Current state and its problems

### What exists

**Firing.** One key. `F` flips `weaponOn` on the session (`Command::toggleWeapon`, applied by
the server). While it is on, every tick `Simulation::StepPlayerFire` fires at
`Command::targetId`, which the client sets to the *selected* object every tick
(`cmd_.targetId = selected_->GetId()`). A shot is instantaneous and always hits: if the target
is within `Sim::PLAYER_WEAPON_RANGE` (a flat **280 units**, a structure's size added) and the
cooldown `Ships::PLAYER_FIRE_INTERVAL` (a flat **0.5 s**) is spent, `TakeDamage(volley, mix)`
is applied. A volley is the design's `Stats::volley` = sum of fitted guns' `damage` + the hull
rules' `baseGun` (16), and the same for an NPC (`StepNpcCombat`: nearest hostile within the
same 280, `ResetFireTimer`). There is no aim, no miss, no travel time, no lock, no cost to
fire. `Ships::Dps` is `volley / 0.5`; sieges, cold-pass attrition and patrols are all
arithmetic over that one number (`Ships::Fighter`, `Bite`).

**Damage.** Layers are real (#464 slice 8, #557): shield, armour, hull, each with a table of
multipliers per damage type (`energy`, `kinetic`), shifted by a hull line's `resist`. Lasers
deal energy, railguns kinetic. A ship's shield is the same for every design (60, regen 8 per
second, `Ship::maxShields_`); armour comes from `hull.armour` modules. There is no way to
repair in a fight and nothing a pilot can do to a shield but wait.

**Modules.** `data/ships.json` has ten: cockpit, engine, thruster, rcs, cargo, armour,
laser, railgun, tractor, probe. Weapons are *summed*: two lasers are "6 damage a volley", not
two things a pilot can switch, group or point. Nothing in a fit is active except the mining
toggle (`M`) and the stabiliser (`X`). A railgun costs `Ammunition` to *build*, never to fire.

**Targeting.** Selection is the only notion. Left click selects what is under the cursor
(or a sensor cell, or an overview row); selecting opens the SELECTED ITEM window; the selection
is also what the weapon fires at. Right click opens the one action list
(`Actions::For`: Approach, Orbit, Keep, Follow, Warp, Attack, ...). There is no lock, no lock
count, no lock time, nothing the target can learn, and no range limit to selecting: you can
fire at anything you can click the moment it enters 280 units.

**Flight.** Direct: `W` thrust, `S` brake, `A`/`D` turn, `X` stabiliser (omni-thrust that
cancels inertia, or Newtonian). Commanded: autopilot to a point, warp (#160), and the standing
holds of #157 -- orbit, keep at range, follow -- plus `Attack`, an order that closes to weapon
range and arms the weapon. All of it runs through `Sim::StepPlayerShip`, the one step both
sides share. Ships do around a few hundred units a second; the weapon reaches 280.

**Orientation.** Brackets on every ship (hostile ones notched and brighter, #362); a status
window with shield/armour/hull gauges and a `WPN` chip; the overview with a Hostile tab,
sorted hostile-first; a radar; the SELECTED ITEM window (name, distance, faction in the
instrument colour, a few fields, the action buttons -- no target shield, armour or speed);
the sensor screen `V` over a fixed grid; the scanner `P`. One thin ring of 280 units is drawn
round the ship while the weapon is on.

**econagent.** `observe` lists hostiles; `attack` is the order above; `hold_station`,
`move_to`, `set_defence` and `patrol` cover flying and structures' and patrols' rules of
engagement. There is no tool for a single module, a target or a group, because there is no
such thing.

### The problems

1. **Fire is a side effect of selection.** The owner's words: you press F and fire at
   whatever is selected. Clicking a station to read its name arms it as a target; a stray
   click changes who you are shooting. Nothing separates *looking at* from *committing to*.
2. **There is no fight to fly.** Range is one number, a hit is certain, the cooldown is one
   number. Once inside 280 nothing you do with your hands matters except staying there, and
   outside it nothing does either. Skill has nowhere to live -- no range to choose, no angle
   to manage, no resource to spend.
3. **One scale.** 280 units against a million-unit system: a fleet cannot engage across
   anything but point-blank, a long gun and a short gun are the same gun, and the overview
   cannot be the instrument of a fight because a fight is never further than a screen.
4. **Weapons are not things.** A fit's guns are one number. A player cannot assign, cycle,
   hold or point them; a player-built design (#279) gains damage from a gun but nothing else
   about it. The grammar that lets a player build a *type* has no combat verbs to express.
5. **Defence is passive.** Shield is the same everywhere, nothing repairs, nothing can be
   hardened or saved. The only defensive verb is leaving, and leaving has no rule.
6. **The target is opaque.** The panel does not show the enemy's shield, armour, speed or
   how well you are hitting it; nothing tells you that you are being targeted.
7. **NPCs and players agree, but only because there is nothing to disagree about.** The
   promise of #463 (one model) is cheap while the model is "within 280, always hits". Every
   richer rule (lock time, tracking, energy) must be written once and read by both, or the
   promise breaks at the first one.
8. **An agent cannot fight tactically.** `attack` is the whole verb. An agent that wants to
   focus one ship, keep a long gun at range, or turn a repairer on cannot say so.

### What the game shows today

Run against a local server and a fresh pilot (start: a high-security station; the starter
systems have no pirates, so no fight could be arranged without a second pilot): with a station
selected, the SELECTED ITEM window offers name, distance, faction, a range slider and the
Orbit/Hold/Follow/Approach/Dock buttons; the status window offers Shields and Hull gauges, and a
row of toggles `STAB`, `MINE`, `WPN`, `AUTO`. There is nowhere in either that a lock, a
weapon, a target's shield or an energy bar could be read, and the one weapon control is a
single `WPN` chip -- the sentence in section 3.6 about growing it into the module bar is
literal.

---

## 2. How other games do it

Each entry is what matters for *this* game, not a review.

### EVE Online -- the model we inherit most from

- **Locks.** A target is *locked* before anything fires at it. Lock range and count are limits
  (sensors, skills); lock time falls with the ship's scan resolution and rises as the target's
  signature radius shrinks, so a battleship is locked at once and a frigate takes seconds. A
  locked ship is shown to nobody; the victim finds out when the damage starts.
- **Turrets.** Optimal range, then *falloff* in which the chance to hit halves per falloff
  distance; *tracking speed* (rad/s) against the target's *angular velocity* (transversal
  velocity over distance), scaled by signature resolution against signature radius. Two
  independent terms: standing at the right range and not crossing the target's line of sight
  fast. Small guns track a lot, large ones little, so the weapon class decides the range band
  and the fight is positional, not twitch.
- **Modules** are active with a *cycle*, cost capacitor per cycle, and repeat until the pilot
  stops them or the target goes. The pilot manages a short row of buttons, not a trigger.
  Weapon groups are a convenience.
- **Manoeuvring** is commands: approach, orbit and keep at range at a distance, from the
  overview or a click with a key; warp; webs and scramblers to stop the other doing it. The
  hand skill is choosing speed and range to beat the other's tracking.
- **The overview is the instrument** with sortable columns, among them angular velocity and
  velocity, filter tabs, and brackets. A fleet commander's broadcasts (target, align, in
  position) do the work a minimap would.
- **Drones, ewar, fleets.** Drones are small ships on the same rules; ewar (ECM, damps,
  painters, webs) spends the *sensor* layer; fleets are a squad hierarchy with a broadcast.
- *Cost:* the rules are many and half of them are invisible without a spreadsheet; combat is
  slow and almost nothing is seen of the ships themselves.
- *For us:* the lock, the cycle, the angular hit rule, the overview as the cockpit, and the
  commands are all right. The invisibility is not: every number should be shown.

### EVE Echoes -- EVE on a phone

- The same grammar cut down: fewer locks, quick lock, big module buttons in an arc, auto
  repeat, orbit/keep range as buttons around the target, a slim overview in tabs.
- *For us:* proof that the whole fits in a *small, bounded* set of controls -- a module bar
  and a target card -- which is exactly what a human at a laptop and an agent with a small
  schema both want.

### Elite Dangerous -- direct flight, the other pole

- The pilot flies and aims; weapons are *fixed*, *gimballed* or *turreted*, each progressively
  easier and weaker. **Fire groups** bind modules to triggers. Targets are *selected* with a
  key or lock-on with a hold; locking has a visible progress, and hardpoints are retracted when
  not wanted. Power distribution (pips to SYS/ENG/WEP) and heat are the managed resources;
  shield cell banks and heat sinks are the active defences. The scanner is a radar disc.
- Skill is flying: turn rate, flight-assist-off, keeping fixed guns on a target while the
  target does the same to you.
- *For us:* mount types as the lever between "you aim" and "it aims"; fire groups; a visible
  lock progress; and the lesson that direct-aim combat is a different game from commanded
  combat and cannot be both without choosing -- a fork (D1).

### Starsector -- the best 2D reference

- Top-down, one flagship under your hand and the rest of the fleet on orders and AI.
  Weapons are on **mounts** with arcs: fixed, turret, hidden; ballistic, energy, missile, with
  ranges that make a fight a positioning game; range rings and arcs are drawn.
- **Flux** is the one resource: guns raise it, shields raise it when they absorb, venting
  lowers it, an overloaded ship is dead in the water. One bar says how aggressive you may be.
  Shields are a toggled cone with a facing. Hardened armour and hull soak the rest.
- **Fleets** are commanded from a pausable tactical map with a few *command points* spent on
  orders (go here, attack that, defend, retreat); the AI does what orders leave open.
- *For us:* exactly the 2D readability we want -- mount arcs, range rings, one resource bar;
  and the fleet pattern: a small set of orders over autonomous ships with ordinary ships'
  rules. Pausing is not available to an MMO; the *order list* is.

### Endless Sky and Escape Velocity -- the low-APM 2D classic

- Fixed guns and auto-turrets, a key for the next target (nearest, nearest hostile), one key
  to fire primary, one for secondary (missiles that lock on), outfits chosen in a store, ships
  disabled rather than killed. Escorts take a handful of orders (focus fire, gather, hold).
- Targeting is *selection* -- this is our current model, and it is right for a single-player
  game with a few ships on screen. It does not carry corporations, an overview and a thousand
  systems, but its feel -- one key to engage, one to cycle -- is what a tidy scheme should keep.

### Cosmoteer

- A ship is drawn from parts; every weapon is a part with a range, a rate, a mount; power
  and crew are physical. Auto-turrets pick their own target; fleet commands are formation
  and focus. Everything a ship can do is visible *on the ship*.
- *For us:* our designs are built from the same sort of parts (#279). Weapon behaviour must
  derive from parts, as stats do, and the weapon's state should be visible on the hull.

### Nebulous: Fleet Command -- the fleet extreme

- No hand flying: you command a fleet by waypoints and engagement settings. The game is
  *sensing*: radar emits and can be detected, ESM locates emitters, datalinks share tracks,
  jamming and deception beat locks, signature (size and emission) decides how soon you are
  seen. Missiles and point defence decide hits, not the pilot's reflex.
- *For us:* sensing as a layer worth having *later*: a signature that decides lock time, an
  emission that reveals, a shared track. The key lesson is the commander's view: all fleet
  play is orders over a map, not hands.

### X4: Foundations

- Real-time with direct flight *and* a deep command layer; **turrets have behaviour modes**
  (attack my target, defend, missile defence, none) and groups; a ship's orders, formations
  and a map-driven fleet UI are the main game. Everything is reachable from the map, so a
  player who never flies still plays.
- *For us:* turret modes are our rules of engagement (`set_defence`, patrol `engage`) applied
  to the player's own ship; and the principle matches M9: the map and list suffice.

### Star Citizen

- Direct flight with gimbals and fixed guns, missiles with *lock time driven by the target's
  signatures* (infrared, electromagnetic, cross-section), power allocation, shield faces,
  countermeasures. Scanning and signatures decide who sees whom.
- *For us:* a single "signature" that decides detection and lock time; shield facing is a
  3D affordance we do not need; the amount of UI it takes to state all this is the warning.

### Freelancer

- Mouse flight, click to target, turrets and gimballed guns aim at the target on their own,
  projectiles with travel time, cruise vs combat speed, countermeasures against lock-on.
- *For us:* the minimum a player explains in a minute -- select, shoot -- with the depth in
  fit and in manoeuvring. This is our "lock-lite" end.

### Everspace

- Direct flight; guns aim-assist toward a locked target; one *energy* pool pays for boosting,
  weapons and shields, so every action competes. Secondary weapons use ammunition.
- *For us:* a single energy bar that every active module draws on is the cheapest way to make
  "everything on all the time" wrong, and it reads at a glance.

### What the table says

| Question | EVE | Elite | Starsector | EV / Endless Sky | X4 | Nebulous |
|---|---|---|---|---|---|---|
| Who aims | turret | pilot (fixed) / turret | pilot flagship, AI fleet | pilot (fixed), turret | pilot, turret | missile, no one |
| Hit rule | range x angular | geometry | geometry + shield | geometry | geometry | sensor + missile |
| Target | lock | select, lock for missiles | select | select | select | track |
| Resource | capacitor | heat, power | flux | ammo, energy | none, ammo | ammo, power |
| Scale lever | weapon class | none | mounts, order list | none | turret modes, map | orders on a map |
| Skill is | range, transversal | flight | flux, positioning | flight | fleet | sensing |

Our situation is nearer EVE than the rest on everything that matters for an MMO (many players,
a commander, agents, a system a million units across, a persistent economy), and nearer
Starsector and Endless Sky on how it must *look* (2D, readable, ships you can see). The
proposal below takes EVE's rules, expresses them in the 2D visual grammar Starsector shows,
and keeps Endless Sky's economy of keys.

---

## 3. The proposed whole

### 3.0 The idea in one paragraph

**A pilot *locks* a target, *groups* weapons onto it, and *positions* to make them hit.** Looking
at things (selection) is free and does nothing to anyone. Committing to one (a lock) takes a
little time and a limited slot, tells the target, and is the only thing a gun fires at. A gun
has a range band and a tracking rate; whether and how well it hits is one deterministic number
decided on the server from where both ships are and how fast they cross, so *standing in the
right place at the right speed* is the skill. Modules are a short row of things with cycles
that cost a shared energy bar. Hands are free for flying when a pilot wants to fly, and a pilot
who never wants to can say "orbit at 600, fire group 1" from the overview and win or lose by
choosing. NPCs lock, cycle and spend energy by the same code.

### 3.1 Targeting: select, lock, active

Three states of attention, in order of commitment:

| State | Cost | What it does | Visible to |
|---|---|---|---|
| **Selected** | free, any distance | opens the target panel, offers actions | you |
| **Locked** | `lockTime`, a slot, within `lockRange` | the thing guns and active modules may point at | you; the target is *warned* |
| **Active** | choose one of your locks | what an ungrouped module and the default fire key address | you |

- **Lock range** comes from the hull's sensor (data: `Stats::lockRange`, from the cockpit
  and an optional sensor module). A proposal: 20 000 units for every stock hull, so the
  overview's "k" column is the lock band and a fight begins well outside gun range.
- **Lock count** is a hull property (`Stats::locks`): 2 for an interceptor, 3 for a frigate,
  4 and more up the classes. A pilot's skill adds one at most. More than one lock is what
  makes target switching, support and fleet fire possible; one lock would make the whole of
  this "select with a delay".
- **Lock time** = `LOCK_BASE * sqrt(refSignature / targetSignature) / scanRes`, clamped to
  0.5 to 20 s. **Signature** is the target's `size` (already in the snapshot and already a
  design fact -- a design's hull derives it) with a later multiplier for emissions
  (afterburn, warp, active modules), the one place Nebulous's sensing earns its entry. So a
  station is locked at once, a cruiser quickly, an interceptor takes a few seconds, and *size
  is a trade* the design grammar already exposes.
- **A lock breaks** when range is exceeded for a short grace, the target leaves the system,
  docks, warps, or is hidden from the pilot (#346, #415: anything with a `signature` the
  pilot has not scanned cannot be locked, by the same filter that hides it from layouts).
- **Being locked is shown to the target.** Every instrument gets one new fact: *locked by*,
  a small marker on the aggressor's bracket and a row tag in the overview, while a lock is in
  progress or held. EVE hides this; Nebulous and Elite show it, and a 2D game with no
  cockpit sound has to. It is not allegiance, so it does not break #117: it names a relation
  between two ships, drawn on the instrument.
- **Law.** A lock is not an attack and is never a crime (#431 L2 begins with the first
  shot). Locking a flagged criminal or a pirate costs nothing; locking an innocent shows them
  a warning and nothing else. The existing `IsCrime` runs when a module fires, as today.
- **A structure** may be locked (it has `size`, a signature, and `MarkStaticChanged`); stars
  and planets never are.
- **NPCs** acquire locks the same way: an NPC wanting to shoot needs a lock, takes the lock
  time for the target's signature and loses it on the same rules. That alone gives pilots a
  breath between "the pirate sees me" and "the pirate fires", and gives a small ship a real
  reason to be small.
- **Cycling.** `Tab` selects the nearest hostile in the overview's order (hostile-first,
  then distance); `Shift+Tab` the previous. A selection of a ship that a pilot wants to
  engage is turned into a lock by one key or a double click -- never by a plain click.

### 3.2 Weapons and fire control

#### The weapon as data

A weapon module is a part with (in `data/ships.json`, read by `Ships::Derive`):

| Field | Meaning | Proposed |
|---|---|---|
| `kind` | `weapon` (see 3.5 for the others) | |
| `optimal` | range of full effect | laser 400, railgun 1500 |
| `falloff` | beyond optimal, effect halves per `falloff` | 300 / 1200 |
| `tracking` | rad/s the turret can follow | laser 0.5, railgun 0.12 |
| `signatureRes` | the smallest target signature it hits at full effect | laser small, railgun large |
| `arc` | mount arc in degrees: 360 turret, 0 fixed (see D1) | laser 360 |
| `cycle` | seconds between shots | 0.5 and 1.5 |
| `damage` | per shot, and `mix` of types as today | 3, 6 |
| `energy` | cost per shot, from the capacitor | laser, 0 for railgun |
| `ammo` | a good it uses from the hold, per shot | railgun: `Ammunition` |

The *numbers* are meant to be derived where the world can: a weapon's range band follows from
its damage and mass the way hull follows from parts, so a player-designed gun (#44) lands on a
curve rather than a free pick; the curve is data in `rules`, and a test pins it. A weapon is
not special-cased by name anywhere.

#### The hit rule (the one rule, scale-free)

For a shot from A at B at the tick it fires, all from authoritative state:

```
range term     = 0.5 ^ ( max(0, d - optimal) / falloff )
tracking term  = 0.5 ^ ( (omega / tracking) * (signatureRes / targetSignature) ) ^ 2
quality        = range term * tracking term           (0..1)
damage applied = shot damage * quality
```

`omega = |v_transversal| / d`, the rate the target crosses the weapon's line of sight, from the
two ships' velocities and positions on the server. A turret's `tracking` is how fast it can
slew; a **fixed** gun's term is replaced by an angular error from the nose to the target
(section D1), so one formula expresses all mounts.

- **Deterministic.** Quality *scales damage*; there are no dice. EVE rolls against the
  number and shows a spread; Starsector and Cosmoteer do not roll. For us: a rolled hit is
  not reproducible in a test, is harder for an agent to reason about, and gives the world a
  second source of randomness to be kept consistent across a cold system and a hot one
  (#295). A damage multiplier of 0.5 is just as readable as a coin flip, and strictly less
  frustrating.
- **Scale-free.** Only angular velocity matters: a ship crossing at 300 u/s at 3 000 units is
  0.1 rad/s, a close brawl at 300 units is 1 rad/s. Long guns track slowly and so *want*
  distance; short guns track fast and want the brawl. The weapon class chooses the range band,
  and the same rule works for a pair of interceptors and for a fleet across 20 km.
- **Why not projectiles with travel time.** Starsector and Freelancer have them, and they
  make a game of leading shots. In a 60 Hz authoritative simulation with interpolated
  display and an agent as player, "the shot is in flight" is a state to keep, send and
  compare. It is the better *feel*, and a fork for the future (D3), but not the first slice.
- **Reference engagement.** Sieges, cold fights and patrols reckon dps at **quality 1**: a
  target stationary at optimal range, where `Ships::Dps` is today. A refit therefore moves
  every siege as before, and a live fight is *worse* than the reference, never better.
  This keeps #295's arithmetic and the "numbers come from the world" rule: no new multiplier.

#### Weapon groups and cycles

- A fit's weapons get **groups** by default: every weapon in group 1. A pilot may split them
  in the Fit tab (the weapons shown as slots on the ship, as the hull's drawing already is),
  saved with the design in the library (#279) and reachable through `apply_design`.
- A group is **on** or **off**. On, each weapon in it repeats its cycle at the group's
  assigned lock: the **active** one unless the group has been given another. Off, the cycle
  in progress completes and stops, as in EVE.
- A weapon that cannot fire (no lock in `optimal + 3 * falloff`, no energy, no ammo, out of
  arc for a fixed gun) simply does not, and says why on its slot (3.6).
- Auto-repeat is the default; there is no trigger to hold. A direct-flight pilot may bind
  *hold to fire* (`Space`) for group 1 -- the same server verb, on for as long as the key is
  down. That is the whole fire-control surface for a human, and for an agent it is
  `module` with on/off.

#### Energy and ammunition

**Earns its place: one capacitor, and ammunition from the economy. Not heat.**

- *Capacitor* ("energy"): a ship pool and recharge rate derived from its parts (engines and
  a reactor module provide `energy`, as engines provide `thrust`). A laser costs energy per
  shot, a repairer per cycle, an afterburner per second. Empty means weapons wait and the
  pilot sees why. One bar, and the answer to "why not everything on?".
- *Ammunition*: the railgun spends the good `Ammunition` from the hold, one per shot (a
  proposal: per cycle of a burst). The good already exists, has a price, is shipped by
  haulers and short in the right places. Running dry is then the economy biting a fleet:
  a corporation at war needs a supply line (#574). An NPC fires the same, from its design,
  with ammunition reckoned from the faction's stock by the macro simulation, not tracked per
  shot -- a rule for #464 to decide how to draw from `holds`.
- *Heat* is Elite's and Starsector's (flux is the heat of shields and guns). It is a second
  bar for the same decision the capacitor already forces. Left out; a design can add it later
  as a module's `heat`, reading the same cycles.
- *Shield*: stays per ship but moves from a constant to derived (a `hull.shield` module
  provides it, as `hull.armour` provides armour), with a recharge that waits `shieldDelay`
  seconds after the last hit -- the cheapest rule that makes disengaging mean something.

### 3.3 Manoeuvring

Two ways to fly, one ship, one step (`Sim::StepPlayerShip`, shared, unchanged in kind):

**Direct.** `W/S/A/D`, `X` as today. It remains the dogfight, the docking, and the way a pilot
who enjoys it flies. Its skill content grows from "don't hit a rock" to *angular velocity
management*: a ship crossing a short-range gun's line of sight at 1 rad/s is untouchable by a
slow-tracking big gun, and a fixed gun needs the nose on the target.

**Commanded.** The standing holds of #157 stay and are the *main* way to fight at range:
`Orbit` (around a target at a range -- at constant speed it holds `omega` = `v/r`, i.e. the
pilot chooses their angular velocity by choosing a ring), `Keep at range`, `Follow`,
`Approach`, `Warp`. Added:

- **`Align`** (fly the nose to a bearing or a target and *hold the heading* without thrust):
  the shape of a warp-align, usable alone, and what a fixed-gun pilot wants. A hold or an
  autopilot already steers the nose to its course; this exposes the nose as a target.
- **`face`** option on `Orbit` and `Keep`: hold the *nose* on the target while the course is
  held (strafing), at the cost of thrust efficiency. Direct pilots do this by hand with `A/D`;
  the option lets a pilot or an agent have fixed guns without hands. It is a change to the
  step, so it is predicted, and a `PROTO_VERSION` bump.
- **Speed** as a setting for holds (`full`, `half`, `afterburner`) because `omega` depends on
  it, and a ship that cannot choose its speed cannot choose its transversal. A throttle, not
  a mode: a fraction of `maxSpeed`.
- **Escape.** Warp already needs an alignment time; a pilot who warps is gone. The rule that
  makes PvP and the law's crime meaningful is a **warp disruptor** (a module, short range,
  needs a lock, drains energy) that holds the target's warp off while it cycles. Without it,
  the aggressor never catches the victim; with too much it is EVE's griefing. A proposal for
  slice 7, with a decision for the owner (D5).

**At both scales.**

| | Dogfight (frigates, hundreds of units) | Fleet (kilometres) |
|---|---|---|
| Weapons | fast tracking, short | slow tracking, long, large signature |
| Flying | direct or `Orbit` with `face` | holds and speed from the overview |
| Targeting | `Tab`, `L`, cycle | broadcast from the commander (3.7) |
| Skill | hands, angle, speed | choosing the range band, who is the primary |
| Overview | secondary | the cockpit |

The point of the hit rule is that **one formula yields both**: nothing in a fleet fight is a
different mechanic, and a pilot learns one thing and scales it.

### 3.4 Orientation: what is shown where

The principle, from M9 and #117: *the overview is the instrument, colour belongs to
instruments*, and the world view stays quiet. Each fact has one home, and an object never
carries allegiance.

| Fact | Home | Form |
|---|---|---|
| Who is where, in order | **Overview** | rows, hostile first; new columns below |
| What I have selected | **Target panel** | a card, see below |
| What I have locked | **Lock strip** (top of the target panel; brackets in the world) | one chip per lock: name, shield/armour/hull mini-bars, lock progress |
| What is locking me | **Overview** row tag + **bracket** marker | a small marker on the aggressor |
| My weapons | **Module bar** (3.6) | slots with state |
| Range bands | **World**, around the ship | optimal ring solid, `optimal+falloff` ring faint; one pair per *active group*, not per weapon |
| Where the fight is | **Sensor screen** `V` and radar | engagement ranges as rings; locked targets marked; the instrument colours |
| How a hit went | **Beam** | tint and thickness by quality; damage text off by default, a setting |
| My shield/armour/hull/energy | **Status window** | the four gauges already there plus energy |

**Overview** gains *columns* (EVE's model, chosen from a menu, not all shown): angular
velocity `omega` (the one that tells a pilot why a gun is missing), closing speed, and a
small lock-state glyph. Its right-click menu is the one `Actions::For`, with **Lock**,
**Unlock**, **Set active** added; no new window.

**Target panel** (the SELECTED ITEM window) is today a list of fields. It becomes the
target's *card*: the object, then for ships its shield / armour / hull bars (needing
`shieldFrac` on the entity snapshot, beside `hullFrac` and `armourFrac`), its distance,
closing speed and `omega`, a "hit" figure -- the quality *my active group would have* -- and
the lock chip's state. The same card is the agent's `observe` entry for the target.

**Brackets** (already instruments, already hostile-notched) gain two things only: a *lock ring*
that fills while a lock forms and is solid when held, and a *marker* on a ship that is
locking me. Both are drawn from the viewer's own state, so they are the viewer's colours, not
the object's.

**No clutter rules.**
1. Range rings appear for *the active group* only, and fade with zoom as the sensor grid
   does -- never one ring per weapon.
2. Nothing animates in the world view that the pilot did not cause or suffer: no
   floating combat text, no HUD flashing for events the overview already says.
3. A fact that is not about a *ship the pilot cares about right now* belongs in a window the
   pilot opened, not in the world.
4. A fixed gun's *arc* is drawn only while that group is on, as a faint wedge: the one place
   the world view shows weapons, because it is aim.

### 3.5 Module management

A module is a part with a `kind`: `weapon` (above), `repair` (shield booster, armour
repairer), `hardener` (raises a layer's multiplier for a cycle), `sensor` (lock range, count,
resolution), `utility` (afterburner, tractor, mining laser -- which already toggles, `M`), `ewar`
(later). Common to the active ones: `cycle`, `energy` per cycle, optional `ammo`, a target
requirement (self, a lock, none). Passive modules are the hull's (armour, rcs, cargo) and are
always on.

- One runtime per ship, `Ships::ModuleRuntime` (in `core/`, no sim): per module `on`,
  `cycleLeft`, `target`, `reason` for being idle. It is stepped by the server, once per tick,
  for a `ClientSession` ship and an `NpcShip` alike. A player's state lives in
  `ClientSession` (per-player, #3), an NPC's in the ship; neither in `Simulation`.
- **Cycles complete.** Switching a module off lets the cycle finish; it spent its energy.
  This is what stops "tap the button at the right moment" being skill and makes a pilot
  commit.
- **Repair is a module.** A repairer adds to a layer each cycle (hull never in space, only at
  a station: the loss of hull is the permanent cost of a fight). Same rule for every ship.
- **A module shows its state on the ship**: the part's drawing already exists (the kit
  sockets); an active weapon's mount glows, an idle one is dim. Cosmoteer's lesson, free
  from the render library.
- Groups, as above, are an *editing* convenience; the unit on the wire is a module.

### 3.6 The control scheme

Mouse and keys, designed around: *look free, commit deliberately, one key per intention*.

| Intention | Keys / mouse | Notes |
|---|---|---|
| Select a thing | left click, overview row, sensor cell (as today) | free; opens the card |
| Next / previous hostile | `Tab` / `Shift+Tab` | selection, in overview order |
| Lock selected | `L`, `Ctrl`+left click, double click | a lock begins; the chip shows progress |
| Unlock | `Shift+L`, or `Ctrl+Shift`+click | frees the slot |
| Set active lock | click its chip, or `Ctrl`+digit | |
| Fire the group on the active lock | **`F`** (toggle) | **keeps its meaning, with a lock** |
| Fire while held | `Space` | group 1, direct-flight style |
| Toggle a module slot | `1`-`8` on the module bar | modifier: `Shift` sets its target |
| Stop everything | `Shift+F` | all weapons off, cycles finish |
| Thrust, brake, turn, stabiliser | `W S A D X` | unchanged |
| Orbit / keep range selected | `Q` / `Shift+Q`, range from the card | or the right-click menu |
| Align to selected | `Z` | |
| Approach, warp, dock, jump | right-click menu, overview, `E` | unchanged |
| Windows | `T O R V P I J G` | unchanged |

The **module bar owns the digits** `1`-`8`; locks are chosen by clicking their chip or with
`Ctrl`+digit. `F1` stays the dev money key; `F` and the module bar keep the combat keys under
the left hand.

A **gamepad** is not designed; the module bar and the card make it possible later.

### 3.7 Defences, electronic warfare, drones, fleets: now and later

*Defences.* Now (slices 1-6): the three layers as they are, shield recharge delay, shield and
armour repairers, hardeners. These are modules on the same runtime and need nothing new.

*Electronic warfare.* **Later, and only what the sensors need**: a warp disruptor (slice 7), a
*sensor jammer* (breaks locks on a roll of `jamStrength` against `sensorStrength`: the single
place where chance is allowed, because EW is *supposed* to be a roll), a *signature painter*
(raises the target's signature so it is locked and tracked sooner) and a *tracking disruptor*.
Not before the base is playtested.

*Drones.* A drone is a small NPC ship (`NpcShip`) with an owner session, a design and the same
module runtime, launched from a bay module and ordered with the same orders as any ship
(`Attack`, `Follow`, `Hold`). The model is deliberately unchanged: *a drone is a ship*.
Needs: ownership on `NpcShip`, an order channel from a session, a count limit. Later.

*Fleets and a commander's view (#32).* The commander's tools are three: a fleet **primary
target** that appears as a marker on every member's overview row and in the lock strip; **fleet
orders** that are the existing orders given to many ships (the `Orders::Kind` set, applied to
NPC wings and to members who accept); and the overview's **fleet filter**. Corporation
patrols (#431 slice 12) already fly as `NpcRole::Guard` under `Corps::Patrol` with an `engage`
mode: the same ship, same rules. X4's turret modes and `set_defence` are the same idea, so
the **rules of engagement** (`hold`, `return`, `enemies`, `strangers`) become one enum every
armed thing -- structure, patrol, drone, player's group -- reads. The one-model premise is
what makes this cheap.

### 3.8 How NPCs and econagent use exactly the same rules

**NPC.** `StepNpcCombat` stops being a separate loop and becomes the same step a session's ship
runs: pick a target by the AI's choice -> *lock* (lock time by the target's signature) ->
cycle weapons whose range band reaches -> pay energy -> `Strike`. What differs between a
pirate and a pilot is the fit and the AI's choice of target, never the code. The AI's choices
(whom to hunt, whether to flee, how to orbit) are the existing `NpcHunts`, `FleeFrom`
and holds, now with `omega` to manage, so a pirate in a railgun ship *wants* range and a
cutter wants the brawl, from its design. A **cold** system's fights (#295) are the arithmetic
at **quality 1**, as now, so a pass without anyone present is unchanged: what changes is only
what a *watched* fight looks like.

**econagent.** The rule from M9 and #42: every verb is a command and every fact is in
`observe`. New:

| Tool | Does | Today |
|---|---|---|
| `lock` / `unlock` | begin or end a lock on an id; `active` to choose | -- |
| `module` | list; turn a module or group on/off; set its target | -- |
| `attack` | **stays**, now defined as: approach to the active group's optimal, lock, group on, until it dies | closes and arms |
| `hold_station` | gains `face` and `speed` | range only |
| `observe` | gains: locks (id, state, time left), locked-by, modules (on, cycle, why idle), energy, per target: shield/armour/hull, `omega`, closing, quality | hostiles |

The *order* level (`attack`, `hold_station`) is what an agent thinks in; the *module* level is
there when it wants to do what a pilot does with one key. Nothing is a stream of 60 Hz inputs.
Because every verb is a command on the same step and every fact derives from the shared
`Stats` and `omega`, the **button-without-tool** test `Actions::tool` already enforces extends
to Lock, Unlock, Set active.

---

## 4. Netcode

The principle is the existing one: *the server decides everything that decides a hit; the
client predicts only its own movement.*

**Server-decided (authoritative, never predicted):** locks (state, timers), module cycles,
energy, ammunition, range and tracking terms, quality, damage, repair, law. All of it runs in
the tick from live positions and velocities -- `omega` is computed from both ships' *server*
state, never from interpolated proxies (CLAUDE.md, #157). Cold systems use the arithmetic
at quality 1.

**Predicted (shared step):** the ship's flight. Added to `Sim::StepPlayerShip`: `face` on a
hold, `Align`, a speed fraction. These alter the step both sides run, so they are one change,
one `PROTO_VERSION` bump, and keep the hold-tolerance rule (#157): a hold is a slow loop.
Everything else in this document is not in the step.

**Client display, advisory only:** the `hit` figure on the target card and the `omega` column
are computed on the client from the interpolated snapshot to be readable and *labelled as
estimates*; the server's `FireEvent` carries the real `quality` and is what beams draw from.
The two will differ by the render delay and that is the same tolerance as a hold.

**Wire.**

- `Command` is unchanged in size: `targetId` and `toggleWeapon` retire. Hold range stays in
  `navRange`; `face`, `speed` ride the nav order (`navMode` 3-5 and a new `Align`).
- `Action` (once, ordered with commands, not predicted): `Act::Lock{id}`, `Act::Unlock{id}`,
  `Act::SetActive{id}`, `Act::Module{slot or group, on, target}`. Four small structs, as #354
  intended.
- `PlayerSnapshot`: `energy` (frac), `locks` (id, state, progress, who-locks-me), `modules`
  (on, cycle fraction, reason code), `lockedBy` (ids), small and per-session.
- `EntitySnapshot`: `shieldFrac`, `vel` (for `omega` and closing speed; also what
  `Follow` wants). Only for ships; measured to be a few bytes per ship per snapshot.
- `FireEvent`: weapon index, `quality`, damage dealt.
- `ShipDesign`: `Stats` gains `energy`, `recharge`, `lockRange`, `locks`, `scanRes` and per
  weapon `optimal`, `falloff`, `tracking`, `signatureRes`, `arc`, `cycle`; a module list for
  the Fit tab. `DesignDef` already travels before the first snapshot that names a design.
- **Compatibility.** `PROTO_VERSION` is bumped once, on the first slice that changes a
  message's meaning (slice 1). Saves: module on/off and locks are *not* saved (a login starts
  with weapons off and no locks, as a ship does after a jump); fire groups are saved with the
  design library (a field an older reader may ignore, no `ACCOUNT_VERSION` bump); a hull's
  new fields come from `ships.json`, which is a shared data file under `Compat::SharedFiles`.
- **Load.** Hit evaluation is per module per cycle, not per tick; at 12 pilots and 50 NPCs it
  is a few hundred evaluations a second, and the existing `Sight` grid already gives the
  neighbours in range. `netbench` and `macrobench` are the measures.

---

## 5. Slice plan

Each slice is playable by itself and leaves the game coherent. Smallest first.

| # | Slice | What a pilot gets | Needs |
|---|---|---|---|
| 1 | **Lock before fire** | `L`/Tab to lock a ship, a lock takes a little time, `F` fires only at a lock; selecting alone no longer arms anything; a lock ring on the bracket, the lock chip with the target's shield/armour/hull; locked-by marker; NPCs lock too. The hit rule is **unchanged** (flat range, always hits). | `Act::Lock`/`Unlock`, `ClientSession::locks`, `EntitySnapshot::shieldFrac`, lock time by `size`, `PROTO_VERSION` |
| 2 | **Range and tracking** | Each weapon has its own range band, optimal and falloff rings (active group), `omega` and `hit` on the card and a column; beams scaled by quality; a fight has positions. Reference dps unchanged. | weapon fields in `ships.json`, `Ships::HitQuality`, `EntitySnapshot::vel`, `FireEvent::quality` |
| 3 | **Module bar and groups** | Weapons are modules: slots with state, groups edited in the Fit tab, `1`-`8`, `Shift+F`; cycles complete; `module` tool in econagent. | `Ships::ModuleRuntime`, `Act::Module`, the Fit tab's slots |
| 4 | **Energy and ammunition** | A capacitor gauge; lasers cost energy; railguns spend `Ammunition` from the hold; idle reasons on slots; `shield` derived with a recharge delay. | `Stats::energy`, hold access from the step, `hull.shield`, `hull.reactor` modules |
| 5 | **Manoeuvre** | `Align`, `face` on orbit/keep, a speed fraction, mount arcs and fixed guns with an aim wedge; `Tab` and `Q`/`Z`. | `StepPlayerShip` changes (predicted), `PROTO_VERSION`, weapon `arc`, tests that client and server agree |
| 6 | **Active defence** | Shield booster, armour repairer, hardener as modules; repair is a cycle with an energy cost. | slice 3 and 4 only |
| 7 | **Escape and the law's teeth** | A warp disruptor; ECM later. Resolve D5 first. | module `kind: ewar`, warp refusal, a rule in the law |
| 8 | **Fleets and drones** | A fleet primary target; orders given to NPC wings and drones; the shared rules of engagement; a commander's overview filter. | ownership on `NpcShip`, one `Engage` enum, #32 |

Slice 1 alone fixes the owner's complaint. Slice 2 is where a fight becomes a skill. Slice 3
is where the Fit tab and econagent become combat tools. Slices 1-3 are a good first release's
worth; 4-6 make the economy and fit matter; 7-8 are what corporation wars (#574) need.

Every slice states in its PR why none of the existing components could carry it (CLAUDE.md,
"A new feature extends what is already built"), and the answer is the same each time: locks
extend the session's `selectedId`, modules extend `Stats` and the design, the card extends
the target window, the verbs extend `Action`, the facts extend `observe`. **No new window**:
the module bar is the status window's `WPN` chip grown up.

---

## 6. Decisions for the owner

Only the forks that change how the game *feels*. Each has a recommendation; the rest of the
document is plan on top of the recommendations and is mine to decide.

### D1. Who aims: commanded turrets, direct-aim fixed guns, or both?

- **A. Commanded** (EVE, X4's turrets): every weapon is a turret; the pilot chooses targets
  and manoeuvres; guns track on their own. Easiest for agents and for fleets; the hand
  skill is range and speed. Direct flight has little to do in a fight.
- **B. Direct aim** (Elite, Starsector flagship, Endless Sky, Freelancer): weapons are fixed
  or on a narrow arc; the pilot turns the ship and the nose is the aim. Great in a one-on-one,
  twitchy, harder to translate to fleets, to an MMO with network latency and to an agent.
- **C. Hybrid by mount** (Elite hardpoints, Starsector mounts): every weapon has a `mount`
  and an `arc`: turrets (360, track and hit less), gimbals (limited arc), fixed (0, hit best
  and need the nose). The same hit rule covers all of them; a hull's sockets decide the mix.
  Starter ships are turrets; brawler lines have fixed guns, and skilled direct pilots win
  with them.
- **Recommendation: C.** The data model already has sockets and lines (`brawler` vs `light`),
  so it costs a field and a formula. Agents and fleets get A; players who love flying get B
  where it pays. A pure A makes the stabiliser and `A/D` decorative; a pure B puts a human on
  a stick in front of an authoritative server.

### D2. What a pilot must do before shooting

- **A. Select = fire** (today, Endless Sky, Freelancer): no lock. Simple; the owner's
  complaint, and no handle for a target to be warned of or a fleet to focus.
- **B. Lock-lite**: a lock is a short timer (0.5-3 s), one to three slots, shows on the
  target. Almost the cost of a click, but a commitment.
- **C. EVE's lock**: seconds to tens of seconds, slots a skill, range a fit. A rich game of
  size and sensors, and heavy at the start.
- **Recommendation: B**, tuned so a frigate against a frigate is about 2 s and a cruiser is
  instant, with C's *signature* maths so the numbers can grow if the game wants. It fixes the
  complaint, makes "being targeted" a fact, and costs a new pilot one key.

### D3. How a shot hits

- **A. Deterministic quality** (proposed; Starsector, Cosmoteer): damage scales with range
  and tracking, no dice.
- **B. Rolled** (EVE): the same number as a chance. More variance, fits a long fleet fight,
  harder to test and to explain.
- **C. Projectiles with travel time** (Starsector, Freelancer, Everspace): shots are things
  in flight and can be dodged and led. The best 2D feel; the highest price in netcode, replay
  and agents; every shot becomes state.
- **Recommendation: A for the first release**, with C kept as a possible *weapon class* later
  (a slow projectile on a rule of its own: lead and dodge for skilled players). B only for EW.

### D4. What limits sustained fire

- **A. Capacitor + ammunition** (EVE, Everspace): one energy bar, and the goods in the hold.
- **B. Flux** (Starsector): guns and shields raise one bar; venting; overload.
- **C. Heat** (Elite), **D. Nothing but cooldowns** (today).
- **Recommendation: A.** It ties fire to the economy we already built (`Ammunition` is a
  trade good; a war needs hauliers) and to the parts we already have (a reactor is a
  module). B is a neater *game* and a better readable single bar, and is the alternative if
  the owner would rather have a combat that is its own system than one the economy feeds.
  Heat is not worth a second bar.

### D5. Can a pilot always leave?

- **A. Yes** (today): warp is escape. PvP is a crime but a victim who aligns is gone, and a
  pirate can hold nobody.
- **B. Disruptors** (EVE tackle): a module that holds a warp for a few seconds at short
  range, needing a lock; a victim can fight, flee by sublight, or be held.
- **C. Tackle is cheap and strong** (EVE's): ships are lost routinely.
- **Recommendation: B**, mild: a short range (inside the hit band of a small gun), a drain on
  the aggressor's energy, a warning on the victim's overview. It gives piracy and a
  siege-breaker something to *do* and the law a reason to be there, without making every
  trip a gamble. Decide before slice 7 only; slices 1-6 do not need it.

### D6. How long a fight lasts

Today a frigate dies in seconds, in a straight exchange. Range and tracking stretch that, and
the numbers can be set anywhere between *seconds* (Endless Sky), *tens of seconds* (Starsector,
Elite) and *minutes* (EVE).

- **Recommendation: about 20-60 s for two equal frigates**, longer for cruisers. Long enough
  to lock, position, use a module, and decide to leave; short enough for a player to be
  not bored. It is set by the rules file's `baseHull` and `baseGun` and by weapon `cycle`s,
  and because sieges and cold fights are derived from the same designs, one change rescales
  all of them together. The owner's call is *the target tempo*; the exact numbers are mine.

### Decided in the document (not forks)

- No dice for guns; EW may roll.
- Locks are shown to their target; a lock is never a crime.
- Reference dps is quality 1 so sieges and cold fights are unchanged.
- Modules are one runtime, shared by sessions and NPCs; there is no player-only code.
- Fire groups are an editing convenience on the design, saved with it.
- Heat is out. Drones, EW beyond the disruptor, gamepad and fleets beyond a primary target
  are later.
- Hull is repaired only at a station.
- The module bar is the status window grown, not a new window.

---
