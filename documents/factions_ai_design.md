# Factions and AI — Design Document

This document describes the target faction and AI system for EconSpace. It is a
**design**, not an implementation. The code implementation is split into stages F1–F6
(see the end). The aim is a future MMO: data lives in JSON, behavior is deterministic
and server-friendly, with no client-side hacks.

## Current State (starting point)

- 4 hardcoded factions (`FactionId`: Independent, TradersGuild, Syndicate, Pirates) —
  `src/engine/core/Faction.{h,cpp}`. **There are no relations between factions.**
- `NpcShip` (`src/game/entities/NpcShip`) — two modes: peaceful patrol between points,
  and a pirate that chases the player (aggro radius `PIRATE_AGGRO = 3500`).
- Combat `Game::ResolveCombat` — only "player ↔ selected NPC" and "pirates → player";
  a nebula hides the player. Constants: PLAYER_WEAPON_RANGE 280 / DAMAGE 16 /
  INTERVAL 0.5; PIRATE_RANGE 230 / DAMAGE 7.
- Spawn `Game::PopulateNpcs`: 6 peaceful (TradersGuild/Syndicate/Independent), and
  pirates at far belts by `security` (≥0.7 → 0, ≥0.4 → 1, ≥0.2 → 2, otherwise 3).
- Reputation `Player::reputation_[4]`: earned from missions and trade, read only for
  display (it does **not** affect access or prices).

---

## 1. Faction Model (data/factions.json)

Factions move out of the enum and into **data**; the `Factions` registry is loaded
from JSON.

Faction fields:
- `id` (string, e.g. `"traders_guild"`), `name`, `color` [r,g,b]
- `lawful` (bool) — law-abiding (has police)
- `kind`: Major / Pirate / Independent
- `homeSecurity` — the typical security of its territories (a guideline)
- `description`

**Relations** — a matrix of "faction × faction" pairs → stance:
`Ally / Friendly / Neutral / Hostile / War` (or a number −100..+100). Symmetric by
default, with asymmetry allowed. Pirates are Hostile/War toward all lawful factions;
lawful factions are Neutral/Friendly toward each other.

**Player reputation tiers** (shared thresholds): Hated / Hostile / Neutral / Liked /
Allied with numeric ranges (e.g. −100 / −50 / −10 / +10 / +50 / +100).

Engine: the `Factions` registry (id↔index, name, color, `Relation(a,b)`,
`StanceToPlayer(faction, playerRep)`). It replaces the `FactionId` enum with a string
id plus an internal index. Affects: `Player` (reputation by id), `Station::faction`,
`NpcShip::faction`, `Mission::faction`, all `FactionColor/FactionName`.

## 2. Reputation and Its Effects

- Player reputation is per faction (float, −100..+100), stored in the save.
- **Gains:** missions (+ toward the client), destroying a faction's enemies, trading at
  its stations. **Losses:** attacking its ships/structures, missions for its enemies,
  crimes in its space. Through relations: helping A against B raises A and lowers B.
- **Effects:**
  1. **Station prices/access** — discounts/markups and access to services (hangar,
     repair, better ships) by tier; at Hostile — a fee or refusal to dock.
  2. **NPC hostility** — a faction's ships attack the player below the Hostile
     threshold; they assist/protect at Allied.
  3. **Mission access** — the composition and rewards of the board grow with the tier;
     rare/story missions are gated by reputation.
  4. **Wanted status (bounty)** — a crime (attacking a civilian/police in their space)
     sets "wanted" with the faction; the bounty grows; patrols/hunters give chase;
     cleared by payment or over time.

## 3. AI Ships: Roles and Behavior

`NpcShip` moves from a binary mode to **roles + a finite state machine**.

**Roles:**
- **Trader/Hauler** (lawful/independent): carries cargo station→station; flees when
  attacked; prey for pirates; brings the backdrop to life.
- **Police/Patrol** (lawful): patrols territory/high-sec; attacks hostiles (pirates, a
  wanted player, ships of hostile factions); stronger in more secure systems.
- **Pirate raider** (pirate): ambushes near belts/gates/trade lanes; attacks traders
  and the player; operate in **wings**; flee when outnumbered.
- **Miner** (industrial): mines at a belt; flees when attacked (loot target).
- **Faction warship/fleet**: patrols borders; fights ships of hostile factions (wars);
  in groups.

**FSM states:** Patrol, Travel, Pursue, Attack, Flee, Mine, Dock. Transitions driven
by: nearby threats (hostility via relations), health (Flee at low hull), the presence
of a target, and role.

**Target hostility:** an NPC is hostile toward X if `Relation(self, X.faction)` is
hostile OR X is wanted with self's faction OR X recently attacked self. The player
participates as a "faction-like" entity through reputation.

**Generalizing combat:** currently only pirate→player and player→target fire. Make
`ResolveCombat` general: every combat-capable NPC picks the nearest hostile target (an
NPC or the player) in range and fires. This enables police-vs-pirates, faction wars,
and pirates-vs-traders.

## 4. Faction Macrodynamics

- **Territories:** systems "belong" to a faction (`owner` in `universe.json`) — this
  determines whose police patrol and their strength (together with `security`).
- **Relations** determine who fights whom in space.
- **Faction wars** (later): stances shift; skirmishes occur in border systems.
- **Tie-in with security:** high-sec = strong police presence (more/stronger patrols
  suppressing pirates); low/null-sec = pirates dominate.
- **Economy (later):** hauler traders carrying goods affect stock/prices.

## 5. Data and Integration

- `data/factions.json` — factions + relations + reputation tiers.
- `universe.json` systems: add `owner` (territory faction).
- `NpcShip`: add a role and an FSM; faction by id.
- `Station`: faction (already present) + services/prices by reputation.
- `Player`: reputation by id; wanted flags.
- `Game`: generalized combat and hostility resolution; role-based spawn by owner/security.
- Editor: editing factions/relations, a system's owner.

## 6. Implementation Roadmap (stages F)

| Stage | Content |
|------|-----------|
| **F1** | Data-driven factions + relations (`factions.json`, the `Factions` registry, migration off the enum) |
| **F2** | Reputation and effects (tiers, prices/access, hostility threshold, wanted/bounty) |
| **F3** | AI roles + a behavior FSM; generalized combat (anyone-vs-hostile) |
| **F4** | Role-based spawn by owner/security (police/traders/miners/pirates) |
| **F5** | Macrodynamics (territories, wars, security-driven patrols), economy hooks |
| **F6** | Editor support (faction/relation editor, a system's owner) |

Each stage is a working build of the game and editor plus a commit. End-to-end
scenarios: police in high-sec attack pirates; a reputation drop makes a faction's ships
hostile; a crime gives a wanted status and hunters; station prices depend on reputation;
traders fly around and become pirate prey; hostile factions skirmish in the borderlands.

## Starter Faction Set (proposal)

- **Traders Guild** (lawful, Major) — controls high-sec, trade; Friendly to Independent,
  Neutral to Syndicate, War with Pirates.
- **Syndicate** (lawful-neutral, Major) — shadow trade; Neutral to the other lawful
  factions, Hostile to Pirates (competitors) or Neutral.
- **Independent** (neutral) — free agents; Neutral to everyone.
- **Pirates** (non-lawful, Pirate) — War with all lawful factions, prey on traders and
  the player.
- (for the future) **Military/Police** — as a role of lawful factions, not a separate
  faction; or a dedicated high-sec defender faction.
