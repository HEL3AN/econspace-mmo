# Decisions

A dated log of direction calls, with the reasoning behind each. The point is that a
settled question stays settled: if you want to revisit one, argue against the reasoning
recorded here rather than re-deriving the decision from scratch.

Newest first. Each entry states the decision, why it was made, and what it costs.

---

## 2026-08-13 — EconSpace is an MMO; single-player is not a mode

**Decision.** The client always connects to an authoritative server. There is no offline
mode in the target design.

**Why.** The project began as a single-player management game, became a single-player
space sim, and has been moving toward an authoritative server ever since. The
client–server work (track M) was never a route to "networked multiplayer as an option" —
it was the destination. Keeping an offline mode alive would mean maintaining two games.

**Cost.** `src/game/core/Game.cpp` branches on `networked_` in 37 places, keeps a
client-side account (`player_`, `ownedShips_`, `missions_`) persisted to `savegame.json`,
and embeds a whole `Simulation`. All of it goes (#23). This is deletion, not refactoring —
a large share of the `Game` god-object (#11) disappears rather than needing to be split.

**Note.** `LocalTransport` survives as a *test* transport (`econserver hosttest`, the
doctest suite). It is no longer a game mode.

---

## 2026-08-13 — Glyph (ASCII) rendering is the primary look

**Decision.** The world is drawn as glyphs. Sprites become an optional alternative
backend, not the thing the project is waiting on.

**Why.** Three arguments pointing the same way:

1. It closes the art gap honestly. The world is currently placeholder shapes waiting for
   sprites that do not exist; a glyph world is a coherent, finished-looking aesthetic on
   day one.
2. It makes a player-mutable world possible without an artist. A new structure type is a
   glyph, not a commissioned asset.
3. It is the format an AI agent reads. What the player sees and what an LLM consumes
   become the same artifact.

It also suits what this project is actually good at — the simulation, not the
presentation.

**Cost.** Rendering must move behind a presentation layer with pluggable backends (#35),
which in turn wants object types described as data rather than as a C++ class hierarchy
(#34). That is the largest single refactor on the plan, and it has to be staged so the
game runs at every step.

**Open.** What happens to the windowed HUD — keep the current panels over a glyph world,
or go fully terminal — is deliberately undecided until the glyph world is on screen and
the combination can be judged rather than guessed (#36).

---

## 2026-08-13 — AI agents are first-class players

**Decision.** The game ships an MCP server so an LLM agent can play it: *"fly to Reach,
find ore, sell it where it pays, and if something shoots at you, decide for yourself."*
One ship first, fleets later.

**Why.** It is the project's genuine differentiator, and the architecture is already
shaped for it: the server is authoritative, the protocol is explicit, and the transport
is swappable. An agent is simply another client.

**Consequence — hierarchical control.** The simulation ticks at 60Hz and the command path
is per-tick. An LLM thinks in seconds and costs money per decision. So the strategic layer
(the agent, seconds) sits above a tactical layer (the server, 60Hz), which means the bulk
of the work is a **server-side standing-order layer** (#26), not MCP plumbing. That layer
pays for itself regardless: it is also a real autopilot for human players.

**Two rules that are part of the design, not extras.**

- *The agent can do nothing a human cannot.* Every tool maps to a `Proto::Command` or a
  standing order and is validated server-side. Otherwise it is not a player, it is a cheat
  channel.
- *Orders carry fallback policies.* An agent can stall for thirty seconds or crash
  outright; the server must react on its own, or agent play is a lottery.

**Note.** Botting is normally banned in this genre. Here it is the feature — which makes
"is an agent-piloted ship weaker, equal, or more expensive to run than a human-piloted
one?" a game-design question to answer deliberately (#32).

---

## 2026-08-13 — `econagent` is written in C++, inside this repository

**Decision.** The MCP server is a C++ target in this repo, not a TypeScript or Python
process using an official MCP SDK.

**Why.** A non-C++ bridge would have to reimplement `Protocol.cpp`, creating a second
source of truth for the wire format — the exact drift this project avoids everywhere
else: one `Simulation` for every mode, one JSON format for the editor and the runtime, a
shared `PLAYER_WEAPON_RANGE` constant carrying an explicit comment about not letting the
values diverge. Linking the extracted `netproto` library (#25) keeps the protocol
permanently in sync. MCP over stdio is JSON-RPC 2.0 with roughly five methods, and
`nlohmann/json` is already a dependency.

**Counterarguments considered.** An SDK would give spec compliance for free — but the
surface is small and isolatable, and this repository already has the habit of covering
protocol code with round-trip tests. An `npx` one-liner would be a lower-friction install
for a stranger — but playing EconSpace means building this C++ repository anyway, so the
binary is already there.

**Cost.** The MCP layer is maintained by hand, and stdio hygiene becomes a real constraint
(stdout carries JSON-RPC only; every log goes to stderr).

**Not a one-way door.** Once the protocol carries a version (#15), anyone can write an
external bridge in any language against the same TCP protocol.

---

## 2026-08-13 — The world becomes player-mutable, in two tiers

**Decision.** Players build in the world: cheap personal *deployables* and expensive
shared *structures*, designed together as one system.

**Why.** The macro layer already exists — per-system prosperity and security, a gate-line
economy, controllers, territory capture and reclaim, an event feed. Player structures plug
into it (#40), which is the difference between "players can place objects" and "players
change the galaxy".

**Cost.** `SystemLayout` is currently sent once, when a client enters a system, so a
structure built by one player is invisible to everyone else until they re-enter. That
protocol gap (#38) sits under the entire track. Persistence stops being a small aggregate
snapshot, which makes save schema versioning (#20) load-bearing.

**Non-negotiable.** Ownership, permissions, limits and upkeep (#41) ship *with* tier-2
structures, not afterwards. Upkeep and decay is the single most important rule: it keeps
the galaxy self-cleaning instead of monotonically accumulating abandoned junk.
