# Decisions

A dated log of direction calls, with the reasoning behind each. The point is that a
settled question stays settled: if you want to revisit one, argue against the reasoning
recorded here rather than re-deriving the decision from scratch.

Appended as they are made, so the file reads in the order the project actually went.
Each entry states the decision, why it was made, and what it costs.

---

## 2026-08-14 — An order decides the command; it never moves the ship

**Decision.** The standing-order layer (#26) does not touch the ship. It decides what this
tick's `Proto::Command` should be and drives that through `Sim::StepPlayerShip` — the same
function a human client's command goes through.

**Why.** The obvious implementation is for an order to set velocity and heading directly.
That would give two implementations of "how a ship moves", and the moment they disagree,
client prediction breaks for an ordered ship — the client predicts with one and the server
runs the other. Routing orders through the shared step means an ordered ship and a flown
ship are the same ship.

**Cost.** Orders are slightly more roundabout to write: an approach is a nav order plus
ticks, not a straight line. That is the correct trade.

**Consequence.** Orders execute on the server's world tick rather than in the input path.
An agent issues one order and then sends nothing; driven from the input path, the ship
would simply sit there.

---

## 2026-08-14 — An order gives up while there is still a ship to give up with

**Decision.** A running order fails and stops the ship below a quarter hull, reporting why.

**Why.** An agent may be thirty seconds from its next decision, or may have crashed. An
order that keeps flying a dying ship into whatever is killing it turns agent play into a
lottery — the outcome depends on model latency rather than on the plan. This is part of
the design, not a safety extra bolted on.

**Cost.** An order can end for a reason the agent did not ask about, so every agent has to
read outcomes rather than assume success. That is true of real orders too.

---

## 2026-08-14 — The agent sees exactly what a player sees

**Decision.** The text projection (#27) is built from the snapshot, the layout and the
local galaxy index — the same three inputs the renderer uses. Every MCP tool maps to a
`Proto::Command` or a standing order that the server validates.

**Why.** The alternative — letting the bridge read `Simulation` directly — is easier and
would make the agent a cheat channel rather than a player. "AI agents are first-class
players" only means something if an agent is bound by what a player is bound by.

**Cost.** Some things an agent would find useful are not visible: which asteroid field the
server thinks is being mined, for instance, is not in the snapshot. Adding them means
adding them for every client, which is the right pressure.

---

## 2026-08-14 — Version the wire before the bridge exists

**Decision.** `PROTO_VERSION` and a rejecting envelope check (#15) landed before
`econagent`, not after.

**Why.** Every `Decode*` reads fields with `value(key, default)`, so without an envelope
check a mismatched peer decodes *successfully* into defaults. The failure then surfaces
much later as a ship that will not move or an account reading zero, with nothing pointing
at the cause. A bridge is exactly the kind of separately-built component that goes stale.

**Proof it was worth it.** Two versions were consumed within a day: the event journal (#29)
changed `Snapshot`, and order fields on the wire (#72) changed `Command`. Both would have
been silent breakages.

**Also.** It is what makes an external bridge in another language possible later without
reintroducing a second source of truth for the format — such a bridge can now detect a
mismatch instead of guessing.

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

---

## 2026-08-16 — An archetype says what an object can do; the class says nothing

**Decision.** Object types move into `data/archetypes.json`: an id, a display name, a
kind, a glyph, and a set of **components** — dockable, mineable, market, defensive,
storage, jumpLink, hazard, salvageable, buildable. Simulation passes query components.
The class hierarchy stays for now and is migrated onto the registry pass by pass.

**Why.** The hierarchy answers "what can this do?" by identity: a thing is dockable
because it is a `Station`. Both new tracks break on that. A player cannot build a new
kind of object (#44) if a new kind means a new class and a recompile, and an agent (#42)
cannot reason about an object whose capabilities live in the C++ type system rather than
in anything it can read.

**What lives where.** The archetype describes a *kind* of object; the instance describes
*which particular one*. Where a gate leads, what a wreck pays out and how much ore a belt
still holds vary between two objects of the same kind, so they stay on the instance. This
line is why the registry can be shared, immutable and loaded once per process.

**Typed fields, not a property bag.** Component parameters are flat named fields rather
than `map<string, double>`. A property bag would have been shorter and would have moved
the guessing rather than removed it — the point of putting object types in data is that
what a thing can do stops being something the reader has to infer.

**A malformed registry is fatal.** Unknown kind, unknown component, duplicate or missing
id: the load fails and the previous registry stays in place. This is deliberately the
opposite of the wire protocol's permissive per-field decoding. A snapshot field arrives
from a peer that may be older; an archetype file is content in this repository, and a
typo in it would otherwise ship as an object that silently does nothing.

**Cost.** Two vocabularies coexist until the migration finishes — an entity has both a
kind and an archetype, and a few passes still narrow to a class for state the components
do not describe yet (who owns a station, #41). That is the price of keeping the game
runnable at every step of the largest refactor in the plan.

---

## 2026-08-16 — An entity describes itself; a backend decides what that looks like

**Decision.** `Entity::Draw()` is gone. An entity returns a `Render::Item` — position,
size, colour, glyph, sprite, layer, and a few facts only it knows — and a backend turns
that into pixels, characters or text. Three backends ship: **shapes** (the look the game
had, moved out of the entity classes), **glyph**, and **text/grid**, which calls no
raylib drawing function at all.

**Why.** Drawing was baked into each class, so the same world could not be presented in
two ways. Three consumers need exactly that: the player in glyphs (#36), an agent as
compact text (#42), and tests with no window. Under the old shape, adding the glyph look
would have meant a second `Draw()` per class and two paths to keep in step.

**What this buys the constructible world.** An object a player invents needs no new art
and no new drawing code: it has an archetype, the archetype has a glyph and a layer, and
every backend can already draw it. That is the difference between #44 being feasible and
being a rewrite.

**Layer is data, not storage order.** `Present()` sorts by layer before dispatching, so
"a station covers a nebula" means the same thing in every backend. Previously it meant
"whichever entity happened to be later in the vector".

**The editor draws through the same seam.** It had its own call to `Entity::Draw()`; it
now builds a scene and presents it exactly as the game does, which is most of what #37
asks for. An editor with a second drawing path is an editor that lies about what the
player will see.

**Cost, and what is deliberately not decided.** Shapes remain the default and F2 toggles
to glyphs. Whether glyphs become the default — and what happens to the windowed HUD
alongside them — is #36, and is a judgement about how the game feels that should be made
after living with both, not while moving code.

---

## 2026-08-16 — The editor's palette is generated from the registry, not written down

**Decision.** `worldeditor`'s creation palette comes from `Archetypes::All()`. An archetype
declares a `world` block — the system-file category it belongs to and the value of that
category's type key — and that is what makes it placeable. Nothing about the palette is
hard-coded any more; the description under each entry is derived from the archetype's
components rather than written by hand.

**Why.** The editor held its own list of six object types, its own default sizes and its
own descriptions, all of which had to be kept in step with the world by hand. The whole
argument for data-driven archetypes (#34) is that a new kind of object costs no code, and
an editor that still needs a code change to show one contradicts that at the first test.

**Consequence.** The palette went from six entries to fifteen — every station role and
every planet type is now its own entry, because they genuinely are different archetypes
with different components. That no longer fits on screen, so the list scrolls. Silently
clipping would have reintroduced exactly the failure this change removes: an archetype
that exists but the editor never shows.

**The `world` block is transitional.** It exists because system files still group objects
by category rather than naming an archetype id. When they do (#34, remaining work), the
block goes away and the mapping becomes the identity.

**Parsing moved next to naming.** `PlanetTypeFromString` and `StationRoleFromString` were
private to `WorldLoader.cpp` while their `…Name` counterparts were public. Having only one
direction public is how a second copy of the other gets written; both now live beside the
enum they belong to, and a test pins the round trip — if the editor's spelling and the
loader's disagreed, every military station placed would silently load as a trade hub.

---

## 2026-08-16 — Glyphs are the look, and the grammar is narrow on purpose

**Decision.** The glyph backend is the default renderer in both the game and the editor.
Shapes and sprites stay reachable with F2 as the alternative backend, not as a fallback
waiting to be promoted.

**The grammar.** Three channels, and only three:

- **glyph** — what class of thing this is. A station is `#` whatever it trades in.
- **colour** — whose it is: faction paint, star type, how much ore a belt has left.
- **size** — how big it actually is, taken from the world rather than from a category.

**What is deliberately not encoded.** Capability. It is tempting to give a shipyard its
own letter, but then a structure a player invents needs a new letter assigned by hand —
and needing new art per object type is exactly what glyphs are here to avoid. What an
object can do is answered by the overview panel and the component list, both of which
already read it from the archetype.

**Areas are not objects.** A nebula three thousand units across, drawn as one character
scaled to fit, would put a `~` over everything inside it. `GlyphStyle` therefore has
three values — `point`, `region`, `directional` — and it is data on the archetype rather
than a special case in the backend, so a player-built minefield can be a region without
the renderer learning what a minefield is.

**Ships turn.** A ship's heading is the most useful thing about it at a glance — whether
it is coming at you. The glyph rotates rather than a separate marker being drawn beside
it.

**Still open, still deliberately.** What happens to the windowed HUD — status, target,
overview, radar, missions, the galaxy map — over a glyph world. The panels are unchanged
here. That judgement wants the glyph world actually on screen to react to, which is now
possible, and it is a question about how the game feels rather than about how it is
built.

---

## 2026-08-27 — One class, several translation units, named after what they decide

**Decision.** `Simulation` and `Editor` each stay a single class with a single header, and
their implementations are spread over files named for the rules they hold (#17):
`Simulation_World`, `Simulation_Agents`, `Simulation_Player`, `Simulation_Orders`,
`Simulation_Snapshot`; `Editor_Palette`, `Editor_Panel`, `Editor_Galaxy`,
`Editor_Universe`. The base file keeps only the spine.

**Why not split the class instead.** The tempting move is to carve out `NpcDirector`,
`PlayerSession`, `SnapshotBuilder` and so on. Every one of them would need most of
`Simulation`'s private state, so the split would be paid for in accessors and back
pointers — the same coupling, now spelled out across four headers instead of one. The
files were unreadable because of their length, not because the class was wrong, and
length is the thing a translation unit split actually fixes.

**What it costs.** A reader can no longer find a method by opening one file, and a
constant used by two of them has nowhere obvious to live. The second cost is real and
showed up immediately: `MINING_RANGE` is read by the mining verb and by the Mine order.
It moved to `SimTuning.h`, which exists for exactly that case and says so — a constant
used in one unit stays next to the rule it tunes.

**What fell out of the move.** `ORDER_DOCK_RANGE = 90.0f` in the order executor, carrying
a comment promising it matched what docking enforces. It did, by coincidence: every dock
in `data/archetypes.json` declares 90. The executor now asks the target for its own
`dockRange`, because the first player-built dock with a different one (#44) would
otherwise be approached to the wrong distance and refused entry, with nothing failing
anywhere. Duplication like this is what a 1800-line file hides.

**How the move was checked.** Function by function, comparing each body against the
previous file: 54 of 56 in `Simulation` byte-identical and all 33 in `Editor`, with the
two exceptions being the changes described above. A refactor that claims "no behaviour
change" should be able to demonstrate it rather than assert it.

---

## 2026-08-27 — A player is a session the server holds, not a member of the world

**Decision.** Everything that belongs to one player — the ship, the account, the missions,
the standing order, the dock/weapon/mining state, the event journal, and the system they
are standing in — lives in a `ClientSession`. `Simulation` owns the sessions and keeps
owning the world; every player verb takes the session it acts for (#3).

**Why not a `PlayerSession` class with the rules inside it.** Docking, mining and NPC
aggro are rules about the world, and a session that owned them would need the world's
private state to run. The split that works is the other one: the world keeps the rules,
the session keeps the state they act on.

**There is no active system any more.** `Simulation` used to have one, and the world step
ran that system "with the player" and the rest "without". Two players in different systems
makes that unstateable, so a session carries its own system id and every system is stepped
identically — the only thing that distinguishes one is who happens to be standing in it. A
system with nobody in it is the normal case, not a lesser kind of step.

**Identity comes from the client, and is not proof of anything.** A `Hello` names the
account; progress is stored under that name. Authentication is a separate problem and this
is the hook it will hang on. Names are restricted to what is safe as a file name and
*refused* rather than mangled: two names differing only in punctuation would otherwise
share one account file, which is one player spending another's money.

**What this deliberately does not do.** Players cannot see each other. Each client is sent
its own system's beams and entities, and another player's ship is not among them — that is
#4, and it is a rendering and interest-management question rather than a question of who
owns what. It looks odd and it is the honest scope line: the server is genuinely
multi-player before the client is.

**What it costs.** Every player verb grew an argument, and the tests and the host loop
were rewritten around that. The compensation is that the compiler now refuses code that
assumes there is one player, which is exactly the assumption that was everywhere.

