# EconSpace — The look of gates and wormholes (#618)

Design first, nothing built. The owner likes the procedural materials of #516 on stations and
ships and does not like how jump gates, Anchor gates and wormholes look; the feeling is that
they need a structural rework, not a tweak. This note surveys what is there, what comparable
games do, and offers three directions with data-only mock-ups for the owner to choose from.

The premises that bound it (CLAUDE.md): the look is generated and an object is a composition
written in data (#122); a material is a shader plus bindings (#121); anything that moves is
`f(time, seed)` (#136); colour in the world view is art, not allegiance (#117); gates are the
route graph and are never added or removed through `AddStatic`, but their look is dressed from
the opening log (`DressCrossing`, holes' `BuildHoleGates`), so state may show.

---

## 1. Survey: what is there now

### Which archetypes, and who picks them

| archetype | when the game uses it | size |
|---|---|---|
| `gate.jump` | a gate as data wrote it, **and** an *anchored* mouth (`DressCrossing` renames it "<anchorer> Gate") and any other crossing that is not unstable | 900 |
| `gate.ancient` | the generator's ancient gate (`Region.cpp`); has its own livery and a hexagon halo | 1300 |
| `gate.mouth` | a tide-opened mouth while `unstable`; carries `closesAt` | 900 |
| `gate.roaming` | both ends of a roaming hole; carries `closesAt` and a `signature` | 700 |

So **an Anchor gate is not an object of its own**: it is `gate.jump` with a name. Nothing on
the screen says "someone pinned this open", and nothing says "this one will be gone in an
hour": `closesAt` reaches the client (`expiresAt` in the snapshot, the observation text reports the hours left) but no look reads it. The roaming hole's *name* already says fresh / ending / mass low
(`HoleName`); its picture does not.

### How they are drawn

- `gate.jump`: four plated arcs with gaps between them, four pylons, four buoys floating off
  to the sides, a `gate.field` module in the middle, one thin spinning trim ring. Material
  `plating` on the `gate` livery.
- `gate.mouth` and `gate.roaming`: the same five parts each, two soft discs, two thin rings
  and a row of chevrons; **only the tint and the numbers differ**. No material of their own.

### What reads badly (renders: editor gallery cards, `gate.jump`, `gate.ancient`, `gate.mouth`,
`gate.roaming`, lit, true scale)

1. **A ring with a dark disc is a planet.** Every round thing in a top-down 2D system is a
   star, a planet, a moon or a hub; a gate is also a circle. Without a legend nothing marks
   it as a different class of object. The jump gate's interior is a near-black disc with a
   faint ripple, which makes it a *hole in the picture* rather than a surface of light.
2. **The jump gate is the weakest #516 object.** Its ring is four short arcs with air between
   them and the buoys float unattached 1.7 radii away; at 238 px across the plating has
   nothing to read as, the way a station's decks do. It looks like four sprites of a gate
   rather than one structure.
3. **A mouth and a hole are the same picture** in violet and in green. A ring of the same
   hardware round a soft glow reads as a bubble or a lens flare, not as something torn open
   in space. Nothing in them is built, nothing in them is natural: they sit between the two.
4. **The soft discs are flat.** `blink` fades an alpha; there is no swirl, no depth, no
   brighter heart. The one-pixel `spin` on thin rings is invisible at any distance at which
   the object is still bigger than a ship.
5. **State is not in the picture.** Unstable, anchored, ending, mass low: four facts a pilot
   cares about, none visible. EVE has the same weakness (see below) and players pay for it
   with scanner windows; we can do better in a 2D view where an object's whole silhouette is
   ours.
6. **It is not one world with #516.** Stations are lit by the system's stars, shaded per part
   and worn; the gate's interior and halos are glows painted over nothing. The nebula's
   shader already does fractal gas in a soft disc as a part-level material (#426); it is
   not used for anything that moves like a gate's field should.

An in-game view near a gate was not rendered for this note (the editor cards were enough to
see the above, and the mock-ups are cards). Before building, a frame from a running server
beside a gate, a station and a planet should be taken to check point 1, which is a statement
about how the object sits among its neighbours.

---

## 2. How others draw them, and what makes each read

| game | gate / wormhole | what makes it read |
|---|---|---|
| **EVE Online** | Stargates: enormous faction-styled structures (each empire's architecture), still, with a flash when used. Wormholes: a small coloured vortex, its colour a class. | Gates read by *scale and architecture* and are unmistakable among rocks and stations. Wormhole *state* (mass, age) is **not in the picture**: it is a scanner text ("end of its life", "critically disrupted"). The look of a wormhole is a sprite; it is the information that makes it a game. |
| **Freelancer** | Jump gates: a big ring with a membrane that wakes when you approach. Jump holes: a natural swirling cloud, no structure. | The two kinds are different *classes of thing* (built versus natural), and that alone tells a player what to expect (a hole is faster and rougher). Closest relation to this game: our mouths and holes are the natural kind, our gates the built. |
| **X series** | Gates: a ring with animated segments of light and a shimmering membrane; sector-edge gates are the biggest objects in a sector. | Motion *inside the ring* is the signature; a ring with a still disc would read as a ring. |
| **Stellaris** | Hyperlane gateways: a ring-and-claw structure on a star; ancient L-Gates: a monumental shell, dormant until opened; wormholes: a natural vortex, destinations hidden. | The same three classes this game has (a built gate, an ancient gate, a natural hole), and the dormant versus active difference is the *story*, drawn as lights going on. |
| **Mass Effect** | The relay: one fixed, recognisable geometry (a rotating ring of coils round a spire) that is the same everywhere in the galaxy. | **One silhouette for the whole class** -- you identify a relay from the thumbnail -- with the *activation* (rings spinning up, a beam) as the only variation. Dormant relays are the same shape, dark. |
| **Homeworld 2** | Hyperspace gate: a hoop with a lens of light. | A recognisable silhouette that is *not a planet* in an RTS full of round things. |
| **Stargate (SG-1)** | A ring of chevrons that lock one by one; the event horizon appears as a lit pool. | State is *progress on the hardware*: chevrons lit, ring spinning, then the pool. A viewer reads "dialling / open / closed" from where the lights are. |
| **Interstellar / Star Citizen** | The wormhole as a sphere that lenses the background; Star Citizen's jump points are natural vortices with no structure. | Lensing: the interior is *not space*; it shows a different thing. At a distance a dark core with a bright rim is the read that needs no explaining. |
| **Elite Dangerous** | No gates in the usual sense; hyperspace is a tunnel you ride, structures and Thargoid vortices are the set pieces. | Not a model for the object, a reminder that the *effect on use* (a flash, a tunnel) can carry the wonder so the object does not have to. |

What the readable ones share, in order of weight:

1. **A class has a silhouette.** You know a relay, a gateway or a hole before you read a name,
   and it is not a circle that a planet also is, or it is a circle with something a planet
   never has.
2. **Built and natural are visibly different.** Hardware on one, an unframed phenomenon on the
   other.
3. **The interior is not space.** A surface that moves, lenses or glows.
4. **State is on the object.** Dormant to active is a change of lamps and motion, not a label.
5. **One hue per class, state by value and motion, not by a new hue.** (Here additionally:
   never by allegiance, #117; allegiance belongs to the overview.)

---

## 3. What the data can already do

The mock-ups below use **only what is in the tree**:

- every primitive and the composition grammar (`ring`, `arc` with rows and `taper`, `chevron`,
  `lattice`, `polygon`, `bar`, soft `disc`; `repeat`, `row`, `jitterAngle`, `blink`, `spin`);
- a **part-level material** (#426): a part may carry `"material"`. The `nebula` shader paints
  a fractal gas with warp, dust lanes and a brighter heart, in the part's own tint plus two
  emissions given in the material's bindings; that is what fills the portals in the renders.
  Five variants (`portal.cyan`, `.violet`, `.red`, `.teal`, `.gold`) are five rows in
  `materials.json` that differ only in two colours, a dust amount and a density;
- the `glass` surface of `built.fs` (dark glass, a sheen by day) for the jump gate's
  mirror in direction B.

It cannot do: a *rotating* swirl (the nebula's gas drifts on a very long loop, it does not
turn, so the arms are separate arcs on a `spin`), a lensed background, an interior that
reacts to a ship's approach, or an axis for the object that depends on its destination.
Where a direction would like those it is named under cost.

State is not in the mock-ups' archetypes. The renders show it as **separate archetypes**
(`gate.mouth.closing`) because the gallery card shows one archetype; in the game it is a
dressing decision, see "State" below.

---

## 4. Three directions

All three change the same five things: a jump gate, an Anchor gate, an unstable mouth, a mouth
about to collapse, a roaming hole. They differ in *where the difference between them lives*.

Renders (kept out of the repository, shown to the owner): one contact sheet per direction, each with the five
cards as in the gallery (`worldeditor --data <copy> gallery shapes card <id> shot f.png`).

### Direction A — One built family, a living portal

*Gates, Anchor gates and mouths are the same kind of thing: a built ring with a painted field.
State is how much of the ring exists and what the field does.*

- **Jump gate.** A continuous plated ring (one `ring` part, not four arcs), six clamps, a row of
  twelve emitter lamps on the inner edge, a `portal.cyan` field inside with a bright heart.
  It wears #516's plating and the `gate` livery, so it is the same family as a station.
- **Anchor gate.** The same ring plus three heavy anchor clamps reaching beyond it, with
  lattice legs and hex feet that blink amber, an outer trim ring, and a **gold** field. The
  hardware that was pinned on is what the Anchor structure *is*.
- **Unstable mouth.** Only three arcs of the ring exist, with amber warning lamps blinking
  quickly, and a violet field that spills past the missing parts, with loose sparks.
- **Mouth about to collapse.** One arc left, a red field half the size, the sparks fading,
  a single red lamp at a slow beat.
- **Roaming hole.** No ring at all: a teal field with a thin ring of light on it and five
  orbiting motes. Natural, small, easy to miss -- which is what it is (it is hidden until
  scanned).

*Reads:* the family resemblance is strong and a "ring with a lit centre" is a gate at any scale;
state is carried by *how much is built*, which is a natural reading of "unstable".
*Weakness:* it is still a circle among circles (point 1 above), and a mouth looks like a
broken gate more than like something natural. The roaming hole is the only object that is not.

*Cost:* data only. About 40 lines of materials (five `portal.*` rows) and five archetypes; the
ring is the same `ring`/`bar`/lamps vocabulary the stations use. The dressing already
chooses `gate.jump` / `gate.mouth`; add `gate.anchor` and `gate.mouth.closing` in
`DressCrossing` (an anchored mouth is already told apart there), and re-dress when
`closesAt - now` crosses a threshold, as holes already do for their name (`endingHours`).
No new primitive, no new shader. Smallest and safest of the three.

### Direction B — Built gates, natural wormholes, an Anchor that is a cage

*Two classes with different looks, as in Freelancer and Stellaris: a gate is hardware; a mouth
and a hole are a phenomenon that is not built. An Anchor is the hardware that **pins a
phenomenon**, and that is what an Anchor gate is.*

- **Jump gate.** The ring of A, with the interior turned into a dark glass mirror under a
  faint cyan field and a small inner ring: inert, a surface, a door that is shut. (It wakes to
  the A-style bright field when something uses it, see cost.)
- **Wormhole, calm.** A natural vortex: a **black core with a bright thin rim** (the one read
  that needs no explanation), a painted field round it, two sets of spiral arms that turn,
  and a halo. No hardware at all.
- **Anchor gate = the same vortex, calm and symmetric, caged.** Three slim clamps with
  amber hex joints and amber arcs round it, the arms in a fixed, tidy count. You can *see*
  that someone tied it down; take the cage away and it is a mouth.
- **Unstable mouth.** The vortex with five ragged arms, a bigger, brighter, violet field and
  loose sparks: the same object, thrashing.
- **About to collapse.** The vortex shrinks to a small black core, red, arms thinning to three,
  the field half its size.
- **Roaming hole.** The calm vortex in teal, smaller; same object as the mouth, a different
  hue and size, because it is the same phenomenon -- which is the truth of the world: a hole
  and a mouth are both wormholes, one pinned by the tide, one drifting.

*Reads:* the strongest at a glance of the three for the wormholes: **a black core with a
bright ring is a different class from every planet and star**, and it is the picture every
player already has for a wormhole. The Anchor-as-cage ties the look to the game's own
fiction (the tide opens mouths, an Anchor keeps one) and makes *Anchor gate* a thing a pilot
can understand from the picture alone; and it makes state progressive: the same vortex, calmer
with a cage, wilder without, smaller at the end.
*Weakness:* the jump gate is then a *different* picture from the Anchor gate, though in the
fiction an anchored mouth is simply "a gate" (the world calls it "<name> Gate"), and gates the
generator places in a pre-existing region are not wormholes at all. The dark mirror inside
the jump gate is the least resolved part of the mock-up.

*Cost:* data only for what is shown; five `portal.*` materials and five archetypes. To make it
good rather than adequate, a **`vortex` shader** would be worth having: the nebula's gas does
not turn, so the arms are separate arcs on `spin` and read as rings broken into pieces; a
shader that spins a log-spiral of the same fractal gas about the core, with the core's radius
a binding fed by `closesAt` (time left), would do in one part what takes eleven here, and make
the collapse a real shrinking rather than a different archetype. One shader and one
`Render::Item` source (time left as 0..1), contained in `Material`/`built`-style files and
with a fallback to today's discs (#120). Medium.

### Direction C — Silhouette first: not a circle

*The class is told from the thumbnail, as a relay is in Mass Effect. Gates are a pair of
spires with a lens of light between them; a mouth is a slit, not a hoop.*

- **Jump gate.** Two spires facing each other across a gap, joined by two bowed arcs of light
  into a **lens** (a vesica) with a painted field in it and a thin bright seam down the middle.
  Long axis, narrow waist: nothing else in a system looks like it.
- **Anchor gate.** The same pair standing in a hexagonal base frame, amber, with a cross-bar
  and anchor stubs: the hexagon is a mark of its own, so an Anchor gate differs from a jump gate by shape as well as hue.
- **Unstable mouth.** A rift: a long thin dark slit with a bright edge, a violet cloud
  spilling out of it sideways and a few crossing flickers.
- **About to collapse.** The slit shortens to half, red, the cloud smaller.
- **Roaming hole.** A short teal slit.

*Reads:* the best at separating gates from *planets*, and it is the only direction in which a
gate is not a circle (point 1). The lens-between-spires is an unusual thing to look at; it is
also the best at working at the long distances (a million-unit system): a vertical sliver
survives being a few pixels wide where a ring's interior is lost.
*Weakness:* the mock-up is the roughest of the three; the rift came out a flat diamond with
the cloud hanging off one side, the geometry would need real art direction, and a lens
needs an **axis**: if every gate points the same way the world looks like a parade ground; a
gate that faces its destination (or a seeded angle) is better, and the client does not know
the destination's direction for hidden holes. It also leaves the #516 ring-and-plating
language behind for gates, which the owner's feeling that it should be "one world with the
stations" may not want.

*Cost:* data only for what is shown, but the best result needs two things not in the tree: a
gate's **orientation** (`Entity::angle` from the route's destination or the seed, saved with
the layout) and a **shape the grammar does not have** -- a true lens or a tapered slit
(`chevron` with `tip` approximates one, which is what the mock-up uses and why it looks like
a diamond). Largest.

---

## 5. State on the object (all directions)

The gate's state already exists in the data and on the wire; it is only not read by the look.
Whatever is chosen, the mapping should be the same, and **by value, motion and how much is
built, never by a new hue** (a hue would be allegiance; #117):

| state | where it is known | what the look does |
|---|---|---|
| gate, stable | archetype `gate.jump` | the calm portal |
| anchored mouth | `RegionOpening::state == "anchored"`, `DressCrossing` | the Anchor variant (cage / gold) |
| unstable mouth | `unstable`, `closesAt` | the thrashing variant |
| ending (< `endingHours`) | `closesAt - now` | the shrinking variant; for a hole this is already in its name |
| mass low | `RoamingHole::spent / budget` | a dimmer field, slower motion; not a hue |

Two ways to carry it: dress an archetype per state (a change of `SetArchetype` plus a
`LayoutDelta` on each re-dress, which is what `DressCrossing` already does on a state change
and holes would do on `ending`), or bind `item.intensity` (today damage) to a 0..1 "life left"
and let one archetype interpolate. The first needs no engine change and is crisp; the second
is smoother and needs the `vortex` shader or equivalent. Both are compatible with all three
directions.

The hue per class is one value in the art, not in code: a jump gate cyan, an Anchor gate
amber, a natural mouth violet, a roaming hole teal -- as the game already has them, since the
existing archetype colours are these and the overview carries standing separately.

---

## 6. Recommendation

**Direction B, built in two steps, with A's gate ring as the first.**

- It is the only one that fixes what the owner's complaint is most plausibly about: a mouth
  and a hole *being the same picture as a gate in a different colour*. B makes the world's
  own fiction visible: a gate is something built; a mouth is something that happened; an
  Anchor is the thing that holds one. It makes "unstable / anchored / collapsing" one object
  changing, which A does with fragments of ring and C with a different silhouette, and it
  costs nothing in the premises (the data does it; allegiance is untouched).
- Step 1 (data only, one PR): the A ring for gates (a continuous plated ring that wears #516's
  plating; the existing four-arc gate is retired), B's vortex for mouths and holes built from
  parts and `portal.*` materials, the Anchor cage, four archetypes dressed from state in
  `DressCrossing` (and one for a hole's `ending`). Hand-checked at truescale next to a
  station and a planet, with a frame from a running server.
- Step 2 (only if the owner likes step 1): the `vortex` shader and a life-left binding, to
  replace the arcs and the per-state archetypes with one smooth one.
- **Do not take C as the base** but take its one idea cheaply: if the owner's complaint is
  the gate-vs-planet silhouette (point 1), a pair of spires on the gate ring (as the
  Anchor's cage does for the anchored one) or a seeded long axis on the jump gate gives most
  of the gain for none of the grammar work; a pure-C rift/lens needs a shape and an axis
  the engine does not have, and is the one to schedule only if both A and B disappoint.

The decision for the owner is *which fiction the picture should tell*: one family of built
things with state in the ring (A), built gates beside natural wormholes with the Anchor as a
cage (B), or a new silhouette for the whole class (C).
