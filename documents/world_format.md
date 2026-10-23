# EconSpace — World Data Format

Specification for the game world's JSON data. This is the contract between the game
and the **world editor** (`worldeditor`): the editor reads and writes exactly these
files.

Status (revised 2026-10-23): **a current reference.** Every format below is read by the
code on main, the server's rule numbers in `data/world_rules.json` included. Not covered
here: the save files (`world.json`, accounts, `corps.json`), which are the server's, not
content.

Parsing — `src/engine/core/WorldLoader.cpp`. Entities — `src/engine/entities/`.

This is the reference. To add something step by step — an archetype, its shape, a material,
a screen pass — start with the worked how-tos in [docs/howto/](../docs/howto/README.md).

## File Layout

```
data/
  archetypes.json      what each kind of object is and can do
  blueprints.json      how a player builds something: cost, time, placement (#39)
  goods.json           what markets trade, and what stations make and use (#343)
  ships.json           ship designs and what their parts do (#279; see ship_design.md)
  pins.json            hand-written exceptions to generated systems (#147)
  factions.json        the faction registry, relations, roles and cultures (#355)
  modules.json         the module library parts are built from (#240); modules/*.json more
  materials.json       materials: a shader and its bindings (#121)
  look.json            the screen treatment's passes (#120)
  shaders/             the shaders materials and the treatment name
  world_rules.json     the server's rule numbers: how the world grows, what rules cost
  ui_themes/           the client's interface themes (see ui.md)
  fonts/               the interface's typefaces, with their licences
  textures/            sprites (PNG), optional
  packs/               admin packs, woven into a new world (#342 S7); absent by default
```

**Nothing hand-written is needed** (#342 S2). The server generates the whole galaxy from the
world's seed: its civilisation centres and a frontier region beside each (see
[systems](#systemsidjson--star-system) below). The hand-written format -- `universe.json`
and `systems/<id>.json` -- is still read: it is what the editor's galaxy mode writes, what
the tests use as a small fixture world (`tests/fixtures/world/`), and what admin packs
(#342 S7) are made of. The server does not read `data/universe.json` today.

**Admin packs** (#342 S7) are directories under `data/packs/<id>/`: a `pack.json` with the
index entries of `universe.json` (`id`, `file`, `map` relative to the pack's middle, `name`,
`security`, `owner`), `links`, optional `factions` (factions.json's entries) and `relations`,
`start`, and `weave` -- plus one system document per system, in exactly this file's format.
Its gates name the pack's own system ids. A new world weaves them in; `econserver validate`
checks them. The full format and its rules: `src/engine/gen/Pack.h` and
`documents/world_generation_v2.md` (S7); a worked example: `tests/fixtures/packs/`.

After a build, `data/` is copied next to each executable by the `copy_data` target
(on every build), and every binary finds it at runtime (#399, `core/DataDir.h`): `--data`,
`ECONSPACE_DATA`, the source tree for a non-Release build, or beside the executable. Clients and `econagent` read no galaxy at all: the server sends them the
index at login as a `universe` message (#206), without the `file` names, so a system that
exists only on the server is on their map too.

## Coordinate System and Scale

- Units are game "units". The origin `(0,0)` is the center of the system (the star).
- Constants — `src/engine/core/World.h`:
  - `SYSTEM_RADIUS = 1000000` — soft boundary: the ship is not let out beyond it. A million
    and not more because positions are `float` (#159).
  - `MID_RADIUS = 775000` — beyond it a belt is a pirate haunt (see below).
  - `WARP_WORTH_IT = 20000` — a leg longer than this is crossed at warp by standing orders
    and NPCs.
- Objects farther than `MID_RADIUS` from the center are "hot spots": belts beyond this
  threshold spawn pirates (see `Game::PopulateNpcs`).
- A planet's `orbitSpeed` is a linear speed; the angular speed = `orbitSpeed / orbitRadius`.
- A planet has no `name`: it is called after its system and its place counted from the star
  out, "Helios Core III" (#259). Naming a system names its planets.

---

## universe.json — galaxy index

Hand-written content only (see above): a generated galaxy has no such file.

```json
{
    "start": "core",
    "systems": [
        { "id": "core",  "name": "Helios Core", "file": "core.json",  "map": [0, 0] },
        { "id": "reach", "name": "Sigma Reach",  "file": "reach.json", "map": [320, -60] }
    ],
    "links": [
        ["core", "reach"]
    ]
}
```

| Field | Type | Description |
|------|-----|----------|
| `start` | string | id of the starting system (if empty — the first in the list) |
| `systems[].id` | string | unique identifier; referenced by a gate's `destination` and by `links` |
| `systems[].name` | string | display name |
| `systems[].file` | string | file name in `data/systems/` |
| `systems[].map` | [x, y] | node position on the star map (arbitrary units) |
| `systems[].security` | number | security level 0..1 (1 — safe); affects police/pirate spawn and trader density. Default 0.5 |
| `systems[].owner` | string | id of the owning faction (from `factions.json`); its police keep the law in the system. Empty/disloyal — falls back to the station's faction. Optional |
| `links` | [[idA, idB], …] | links drawn on the map (do not affect jumps) |

> Jumps are determined by the `destination` field of gates inside systems, whereas
> `links` are only a map visualization. For consistency, add a corresponding `links`
> entry for each gate pair.

---

## modules.json — the module library (#240)

Recognisable things -- a hatch, a turret, a dish, a tank, a row of windows -- written once,
in several **variants**, and placed on objects by name. Loaded before `archetypes.json`
(from the same directory). `worldeditor modules` shows every variant side by side.

```json
{ "modules": [
    { "id": "hatch", "tags": ["opening"], "sockets": ["top", "edge"],
      "variants": [ { "id": "round", "shape": [ ...parts, in the module's own unit, radius 1... ] } ] } ] }
```

A part in an archetype's shape uses one instead of a form:

| Field | Meaning |
|------|----------|
| `module` | the module's id; unknown is a load error |
| `variant` | pin one variant; absent, the object's **seed** picks (so two of a kind differ) |
| `variants` | the seed picks only among these ids: `["bell", "cluster"]` |
| `except` | the seed picks among all but these ids: `["ion"]` |
| `scale` | the module's radius as a fraction of the object's (default 0.1) |

`at`, `angle`, `repeat`, `mirror` and `row` place the module as a whole. A module's own
parts may use `row` but not `repeat`, `mirror`, orbits or the sphere (those place things
about the object's centre). A module is drawn once it is about 10 px across on screen
(its parts' own `minPixels` count in the module's pixels), so it fills in as you approach.
A part that is the whole of what a module is -- the body of a rock, which a field is made of --
says `"minPixels": 2`, so a field reads as a field of rocks from about twenty pixels across
rather than as one (#426).
`tags` and `sockets` are for the automatic placement of phase 3 of #240. `handed` marks one of a left/right pair (see Handed modules below).

`variants` and `except` narrow the seed's choice without taking it away: an engine that may
be a bell or a cluster but never an ion drive. Every id must be one of the module's variants,
`variant` with either list is refused (one pins, the others choose), and a list that leaves
nothing to choose is a load error. Without them every variant is in play, in the module's
order, so the lists change nothing for a part that does not use them.

A module's parts may be `soft` like any part: a glow in a module -- a corona streamer's haze,
a nebula's puff -- stays a glow wherever the module is placed, on the disc or on a sphere.

**On a planet.** A module part with `lat`/`lon` is laid on the sphere (#166) instead of the
disc: a crater, a base, a city. Its own frame is a small patch of the surface at that
latitude and longitude, measured in the body's radius, and every part of it becomes a surface
part -- carried round by `spin`, foreshortened at the limb, hidden on the far side. `repeat`
spreads copies round the planet in longitude and `mirror` reflects across the equator. Only a
`disc` is foreshortened at the limb today, so surface modules are written in discs. A disc on
the surface is cut to the planet's outline, so a storm near the limb ends at the limb rather
than bulging past it; a `soft` one is too, and is lit as the body -- dim on the night side --
while keeping its fade to the rim.

**Packs.** Further modules live in `data/modules/*.json`, one file per domain (weapons,
planet, wrecks...), in the same format. They are loaded after `modules.json` in file-name
order; a module remembers its file as its `pack`. An id is global: the same id in two files
is a load error naming both. `worldeditor modules pack weapons seeds 4` shows one pack with
each variant at four seeds, which is how ranges and palettes are judged.

**Generated variety (any part, in a module or not).** A number may be written `[min, max]`
and the object's seed picks a value (`radius`, `width`, `length`, `angle`, `at`'s
components, `alpha`, `scale`, `sides`, `count`, `row.count`, `lat`, `lon`, `spin`, `blink`, an arc's `from`/`to`); `tint` may be a list of
colours, a palette to pick from; `"chance": 0.4` keeps the part on 40% of objects. Within a
module, a part is resolved once per object, not per copy, so a row of hatches stays a row
of the same hatch.

**Variables (shared rolls).** A shape object may declare `"vars"`: each one is `[min, max]`
or a list of colours, rolled once per object. A part names one with `"$name"` wherever it
would take a number or a `tint` (`"count": "$tubes"`, `"angle": "$sweep"`, `"tint": "$paint"`),
so the tubes and their caps, or a wing's hull and its trim, always agree. In a module (its
variant's `shape` written as `{ "vars": {...}, "parts": [...] }`) the roll is per placed copy:
two pods on one hull may differ, and the parts of one pod do not. An undeclared name is a load
error.

A variable may be used as a straight line of itself: `"-$a"` (a pair of jaws opening
together), `"$len*0.5"`, `"$r+0.05"` (a rim a fixed step outside its crater), `"-$len/2+0.1"`
(a pivot at the end of a ranged length). Negation, then one `*` or `/` by a number, then one
`+` or `-` a number; nothing else. The step may be a range, `"$r+[0.02,0.06]"`, which each part
rolls on its own: rims that all follow a shared radius, each a different step outside it.

**Rows.** `row.step` takes ranges and variables like any number. `"centred": true` puts the
middle of the row at `at` rather than its first copy, so a row with a ranged count stays
balanced. `"turn": 30` bends the row by that many degrees per copy (and turns each copy with
it): twelve copies at 30 close into a ring, a few at 15 make a crescent or a spiral arm.
`"taper": 0.8` makes each copy that fraction of the one before: rays, tongues of lava, a
glacier narrowing to its snout. Both take ranges and variables. `"taperStep": true` shrinks the
gaps with the copies, so a tapering row stays touching and a bent one spirals inward.

`"ring": 0.6` lays the row round `at` instead of along a step: the copies sit at that radius,
spread evenly all the way round, each turned to face out, and the count may be a range and
the ring still closes. `"spread": 90` makes it a fan of that many degrees centred on the part's
own direction. A `pivot` on a ring row swings each copy about its own joint.

**Pivot.** `"pivot": [x, y]` is the point `angle` turns the part about, in the part's own frame
before it is turned (`[-0.5, 0]` is the left end of a bar of length 1). A crane jib or a clamp
jaw with a ranged angle then swings about its joint instead of drifting off its mount.

**Draw order.** `"z"` on any part: parts are drawn by depth (orbits), then by `z`, then as
written. Below 0 is under the hull -- wings beneath a fuselage, an engine block whose nozzle
shows behind the tail -- above 0 is on it. A module's parts take its placement's `z` plus their
own.

**Night side.** `"onlyDark": true` fades a part out where its body faces the light, across
the terminator rather than at it: a city's lights, an aurora, a station's lit windows. On a
planet the body is the planet; elsewhere it is the object itself.

**Shapes the catalogue kept faking.** `"tip": 0.4` on a `chevron` makes it a trapezoid, its
narrow end that fraction of the base: a nozzle bell, a wing chord, a fairing. `"jagged": 0.3`
on a `polygon` pulls each corner in by up to that fraction of the radius, by the object's seed:
rocks, torn edges, shards. `"soft": true` on a `disc` draws a glow fading to nothing at its rim,
never shaded: gas, haze, a corona, a nebula wisp. `tip` and `jagged` take ranges.

**Painted bodies (#426).** `"material": "planet.oceanic"` on a part paints that part with a
material of its own instead of the object's: a planet's disc that a shader draws whole --
continents, cloud, ice, the air on its limb -- a moon of rock round a planet of sea, the rocks
of a belt, a star's photosphere and corona, a nebula's gas. `"fallback": true` marks a part
as the **stand-in** for such a painting: it is drawn only while the body is *not* painted --
on a driver that would not compile the shader, with materials switched off, or while the body
is under a few pixels -- and skipped while it is, because the shader already drew the real
thing. A module placed as a fallback is a fallback in every part. What counts as "the body"
is any part with a material of its own that is neither a fallback nor orbiting, so a moon's
material says nothing about its planet's stand-ins. The material's id is checked where
materials are loaded, not here: one that does not exist is named at startup and the part is
drawn as the object.

**Groups.** Parts with the same `"group": "name"` share the roll their `chance` is compared
with: a lamp and its housing come and go together. Equal chances agree exactly; a smaller
chance in the same group is a detail that only ever appears with the larger one.

**Sections and kits (phase 3).** Instead of placing every module by hand, a shape object
may give `"sections"` -- parts like any other, drawn first -- and a `"kit"` that says what
goes on them:

```json
"shape": {
  "sections": [ {"form": "polygon", "sides": 6, "radius": 0.55, "pitch": 0.2},
                {"form": "capsule", "at": [0.85, 0], "repeat": 3, "length": 0.75, "width": 0.16} ],
  "kit": { "symmetry": "radial", "plain": 0.45, "modules": [
    {"of": "windows", "in": 1, "on": "edge", "count": [1, 2], "turn": 90},
    {"of": "#opening", "in": 0, "on": "edge", "count": [2, 3]},
    {"of": "station.dock", "on": "edge", "count": 1, "inset": -0.6} ] },
  "parts": [ ...anything else, as before... ]
}
```

A section exposes **sockets** from its own geometry, `pitch` apart (default: its width, or a
third of its radius): a bar or capsule has an `edge` row along each long side, a `top` row
along its axis and an `end` at each end (a capsule's at the tip of its round cap); a disc or polygon has an `edge` rim on its outline and
an inner `top` ring; a ring or arc has a `ring` rim along the middle of its band; a chevron has
an `edge` row along each slanted side and its tail is an `end`. Every section also has one
`middle` socket at its centre, facing the section's own `angle` -- the place for the one thing
a section is built round (a hub's tower, a hull's bridge, a beacon in a ring); an arc's middle
is the middle of its band. Sockets face outward.

**A ship's socket kinds (#279)** are not more places but names for some of these, read off a
*long* section (a bar, capsule, lattice or chevron) and the way a socket faces; +x is an
object's forward, the way a ship flies:

| Kind | Which places | For |
|------|------|------|
| `bow` / `stern` | an `end` facing forward / aft (within 45°) | a gun or a ram / the drives |
| `front` | the foremost `top` of a long section lying along x | the bridge, a hangar mouth |
| `side` | the middle of what is open of a long section's flank (of two, the one nearer the section's own start) | wings, sponsons, pods |
| `spine` | a long section's `top` row | a keel, cargo, a lance laid along it |
| `bottom` | any `top`; the line is drawn beneath the hull (`z` -1) unless it says | landing gear, a belly pod |

The rule between `end` and the two names: **say `bow` or `stern` when it matters which end,
`end` when it does not** -- a shipyard's lights at both ends of its slip, a wreck's cable
from whichever end is broken. Only a bilateral kit has a forward, so `bow`, `stern` and
`front` anywhere else are load errors; so is any kind not in this list. Being names, they
leave a section with the sockets it had, and `plain` counts the same.

**A socket under another section is not offered.** What decides is the topmost other section
covering the socket's point (by `z`, then as written): if it is drawn over the socket's own
section, a module there would be hidden or poke out from under it; if it is drawn beneath, a
module there would hang over a neighbour -- the inner end of an arm inside the hub, the hub's
rim under the arm, a pod's rim against its arm. The exception is a section that sits *on*
another, its centre inside it (a panel on a hull, a ring round a hub): its sockets stay, since
that is what a section on a section is for. Covering is measured with a sliver of slack, so a
socket exactly where two sections meet still belongs to its own.

Each kit line names a module (`"of": "id"`, or `"#tag"` for any module carrying the tag), a
`count` (a range: every whole number in it equally likely), the socket type `on` (default: the
module's first `sockets` entry), and optionally `in` (only on that section, by index, or on any
of a list of them: `[2, 3]` -- a hold spread over two keel sections is one line, not two that
each think they are all of it), `scale`
(module radius, default from the pitch), `turn` (degrees added to the outward direction),
`mount` (how the module meets the socket, measured by the module's own box rather than its
origin -- the box of what its parts draw: a circle, ring or polygon by its radius, an arc by
the span `from`..`to` actually covers, a bar, lattice or chevron by the corners of its turned
outline, a capsule by its caps, never a circle drawn round a part: `"on"`, the default, lies wholly on the hull with its edge at the hull's; `"out"` starts
at the edge and stands out from it -- docks, engines, dishes on booms; `"centre"` is centred on
the socket; a `top` or `middle` socket always centres), `z` (draw order, default 1: over the hull), `prefer` (`"out"`, the default, or `"in"`: which of
two otherwise equal lines -- the two long edges of an arm, its two ends -- is taken first, the one
facing away from the object's centre or the one facing it), `when` (a component the archetype must have for the
line to apply -- `"when": "defensive"` puts turrets only on stations that can fight, `"!market"`
only on those without a market; an unknown component name is a load error), and
`variant` (pinned; otherwise the seed picks one for the whole line, so a row is a row of the
same thing), and `variants` / `except` (the seed picks only among, or never, these ids, as on a
module part). On a line that names a tag, each id must be a variant of at least one module
carrying it, and a tagged module with none of the allowed variants is not a candidate for the
line.

**The seed decides what and how many; rules decide where.** A kit line goes on the longest free
line of sockets of its kind; on a straight line its modules are spread evenly and symmetric
about the middle, on a closed rim they spread from the socket that faces furthest out from the
object (a pod's dock is on its far side, not facing the hub); a placed module covers the
sockets under its **footprint** -- its box, turned with it and with the corners rounded off, not a circle round it: a pod laid along a keel covers the
keel and leaves the flanks beside it -- so nothing is put on top of it; each section keeps
`plain` (default 0.4) of its sockets empty; with `"symmetry": "bilateral"` every module on one
side is mirrored onto the other, its shape reflected and drawn from the same part, so the two
share every roll (a pod of three tanks is not mirrored by a pod of two), and with `"radial"` a
placement on one copy of a repeated section is repeated on every copy (the count is then per
copy). An unknown module or a tag nobody carries is a load error. A module placed on a socket
shares its section's `spin`, so it turns with it.

**Pairs (#279).** On a bilateral object an **even count is pairs**: lines off the axis are
tried before lines on it, so two engines are the pair of nacelles rather than one on the block
between them and one beside it. A straight line that crosses the axis -- the face of a
crossbar -- is two half lines meeting there: its -y half is placed and the other is the
mirror, and the place on the axis takes the odd one of an odd count and nothing else.

**Handed modules (#279).** A module that says `"handed": true` is one of a left/right pair:
it is drawn as the one on the +y side (a wing, root at the hull and tip outward), and a
bilateral kit places that drawing on the +y socket and its reflection on the -y one, as one
mirrored part. A line's `turn` is measured from the drawing as it stands there, so no turn is
the wing as drawn, swept the way it was drawn on both sides. Never `repeat: 2`, which puts the
second wing in front of the nose.

**Except where it carries function (#279).** A kit line that says `"fit": true` is a drive, a
hold, a weapon: what the object can do depends on it, so the seed may choose how it looks but
never how many. Its `count` is one whole number and it names a module rather than a tag (both
load errors otherwise), and it is placed before every line that is only looks, and regardless
of `plain`: function first, trim in what is left. On a ship, every module `data/ships.json`
says provides something is on a `fit` line, or written by hand without a `chance`; a test holds
every ship archetype to that, and to every `fit` line being placed in full at every seed.

## pins.json — hand-written exceptions to the generated region (#147)

The generated region is the default, not a monopoly. A starting point, a set piece, or
anything a mission depends on is a **pin**. **Order: generate, then apply pins, always.**
A pin that cannot apply says so in the server log, by number and system, rather than
quietly losing to the generator.

```json
{ "pins": [
    { "system": "r0-1-1", "document": { "derelicts": [ { "name": "The First Expedition",
                                                         "pos": [500000, 500000], "size": 60, "reward": 1500 } ] } },
    { "system": "r0-2-1", "slot": 0, "seed": 7, "mode": "replace", "document": { "star": { "type": "Blue", "size": 120000 } } }
] }
```

| Field | Meaning |
|------|----------|
| `system` | The generated system's id, `r<slot>-<depth>-<k>` (`r0-3-2`: slot 0, ring 3, second system) (#342). Required. |
| `slot` | The skeleton slot of the region the pin is for. If absent, the slot the system's id names -- and slot 0 for an id that names none, which is where every pin was before slots. A pin for a slot that is not open waits for it; a `slot` that contradicts the id is refused. |
| `seed` | Apply only in the world this seed makes. If absent, the pin applies for every seed. |
| `mode` | `merge` (the default) or `replace`; see below. |
| `document` | System JSON, in the same format as `systems/<id>.json`. |
| `note` | Free text for whoever reads the file. |

**`merge`** keeps the generated system and adds to it:
- A list (`planets`, `stations`, `asteroidFields`, `nebulae`, `derelicts`, `gates`) appends its objects.
- An object with `"replaces": "<name>"` takes the place of the generated object of that name. With `"remove": true` as well, that object is removed instead.
- `"replaces": N` names the N-th object of the list (from 0) instead of a name: a generated planet has none.
- Every `replaces` is read against the list as it stood **before this pin**, so the order of the entries does not matter: replaced objects keep their place, removed ones go, added ones come after.
- Any other key (`star`, `stars`, `character`) is set to the pin's value.
- `replaces` and `remove` are the only words a merge adds to an object; anything else in the
  document is held to the system format like a file (#357), and a pin with a mistake in it is
  refused whole.

**`replace`** makes the document the whole system. Its gates are still kept from the generated system unless the document lists its own, because the gates are the region's topology.

Two rules hold whatever a pin says (#237). A pin that would break either is **refused
whole** -- not applied at all, and the log says why:

- **Gates are the region's topology.** A pin may move a gate, rename it or change its look,
  but the system's gates must still lead to exactly the systems they led to before it: none
  removed (the far side would keep a gate back to a system that no longer comes to it), none
  added (a link only one side knows), none re-aimed, and never at a system the region does
  not have.
- **A satellite belongs to its planet.** An object with `orbits` finds its planet by its
  place in `planets` (#210), so removing a planet would otherwise hand its moons -- and every
  later planet's -- to the wrong one. When a merge removes a planet, every object the pin
  does not mention that orbits a planet is re-pointed at that planet's new place, or
  **removed with it** if its own planet is the one gone. An object the pin writes (a
  replacement or an addition) is read as written, and its `orbits.planet` counts the planets
  **after** the pin. A satellite left orbiting a planet the system does not have refuses the
  pin.

Any other field is an error, as everywhere else (#191). Pins are data, like the systems: a
pin added later changes the region the next time the server starts.

### Writing pins with the world editor (#237)

`worldeditor region 7` opens the system through the wormhole of the region seed 7 makes
(`region 7 system r0-2-1` opens another; in the survey, **E** on an enlarged card opens that
one). It is the system the server would build: generated, then every pin on top. The
ordinary tools work on it -- drag, the palette, the property panel, Del -- and an object a
pin changes is ringed amber, one a pin adds green; everything else is the generator's and
is written nowhere.

**Ctrl+S writes only the difference**, as one pin for that system **and that seed**, which
replaces the editor's earlier pin for the pair and goes last in the file (the editor showed
it on top of every other pin). Pins without a `seed`, or for another system, are written by
hand and never touched. The difference is a `merge` -- changed objects as `replaces`,
removed ones as `remove`, new ones appended, keys that differ set -- unless it cannot be
said as one (a key taken away), and then it is a `replace` of the whole system. No
difference left removes the pin. Opening the system again gives back exactly what was
saved; a test holds the editor to that.

The editor keeps the same two rules. A generated system's gates can be moved but not
deleted or added, and the property panel shows where one leads instead of offering a
choice. **Del on a planet** -- in any system, generated or not -- removes what orbits it
too, says how many went, and re-points the satellites of the planets after it; the saved
pin then only says which planet and which objects were removed. A save that would still
break a rule is refused with the reason, rather than written for the server to refuse.

### The world and packs in the editor (#342 S8)

**F5 goes up a level**: system -> region -> galaxy, and back down from the galaxy.
`worldeditor galaxy 7` opens the galaxy of the world seed 7 makes: every slot of its
skeleton, which are open and how, the mouths they opened through, and the slots a pin
waits on (a pin for a slot that is not open is kept and applies when the tide opens it).
A click opens any slot's region on the region map, opened or not. `worldeditor world
build/bin/server/world.json` does the same for a **saved world**: its seed, centres, cap
and opening log, each region read from its seal in `regions/` beside it, so what is shown
is what the server has, not what today's rules would make. With either, `slot 14` opens a
region straight away and `system r14-2-3` a system. A pin saved there has the system's
`slot` and the world's `seed`, as everywhere.

**E on the region map writes that region as a pack** (`export ID` on the command line;
`region-<slot>` without one) into `data/packs/<id>/` -- or `packs DIR` -- and opens it:
its systems under ids of the pack's own (`r14-2-3` -> `s2-3`, the way in first), the map
moved round its middle, its links and gates among its own systems and nothing that leads
out, its natives as a faction of the pack's (`<id>-natives`) with their stances, a centre's
capital as its start. `newpack ID` begins one from a single system with a station, and
`editpack ID` opens one. A pack is edited on the index map (systems, links, owner, start)
and in the system view; **every save reads it back as the server does** and says the first
problem on screen and every one in the log. `econserver validate data/packs/<id>` is the
full check, woven into a trial world.

## factions.json — who is in the galaxy, and how they act

```json
{ "id": "Pirates", "name": "Pirates", "color": [255, 161, 0], "lawful": false, "kind": "Pirate",
  "temperament": { "appetite": 1.4, "capacity": 6, "growth": 0.08,
                   "values": { "traffic": 1.0, "salvage": 0.6, "unclaimed": 0.4 } } }
```

`relations` gives each pair a stance (`War`, `Hostile`, `Neutral`, `Friendly`, `Ally`), and
`repTiers` the reputation thresholds. A pair nobody lists is allied with itself, hostile
to the pirates, and neutral otherwise.

Factions are data (#355): the code names none of them. The parts the world gives a faction
to play are `roles`, each naming a faction by `id`:

```json
"roles": { "unaffiliated": "Independent", "pirates": "Pirates",
           "police": "TradersGuild", "traders": "TradersGuild" }
```

| Role | Meaning |
|------|---------|
| `unaffiliated` | owns what nobody owns: a station with no `faction`, a system nobody holds. **Listed first.** |
| `pirates` | spawned as the pirates; what a lawless system's unrest feeds |
| `police` | patrols a system whose holder is not `lawful` |
| `traders` | trades on every lane, beside the system's own faction and the unaffiliated |

A faction's `id` is what saves, the wire and other data files call it, and never changes;
its order in the file is not something any of them depends on. A region's natives and an
admin's pack add their own factions at runtime, after the file's.

Loading is strict (#191, #357): a missing or unparsable file, an unknown field anywhere, a
faction listed twice, a relation or role naming a faction that is not listed, or a stance
that is not one of the five is a load error. The server stops on it; the client and the
editor log it and keep the factions they had.

### temperament — the same machinery for every faction (#231)

Every faction acts on the galaxy by one rule. Only its temperament differs, so a new
faction's behaviour is data.

| Field | Meaning |
|------|----------|
| `appetite` | How bold. A faction reaches into a neighbouring system when *value − risk / appetite* > 0. `0` never reaches out: it holds what it has. |
| `capacity` | The strength, in ships, one of its systems sustains. |
| `growth` | How fast its strength recovers in a system it holds, per period (a minute). |
| `values.traffic` | Weight on the trade passing through: prey to a pirate, customers to a guild. |
| `values.ore` | Weight on belts to mine. |
| `values.salvage` | Weight on wrecks, ruins and finds. |
| `values.unclaimed` | Weight on a system nobody holds. |

How it plays out:
- **Risk is the same for everyone:**
  - enemies' strength in the target;
  - defensive stations (`defensive`) of other owners;
  - for a faction that is not `lawful`, the system's security.
- **One move per faction per period.** A system it holds may send what it has above 60% of `capacity` into **one** neighbour by gate, never further.
- **What a faction may enter:** a system nobody holds, or one held by a faction it is at `War` or `Hostile` with. Never a friend's or a neutral power's.
- **When a system changes hands:** when a challenger has held the upper hand there for three minutes. That means more than 1.5× the holder's strength plus one, and at least half its own `capacity`, which the local trouble of a lawless system never reaches.

A system beyond the wormhole starts held by **nobody**: its controller reads Independent, but no garrison grows there and any faction may claim it. The spawn director makes the armed ships from this committed strength, and ships destroyed in battle are strength lost.

## archetypes.json — what a kind of object is

A system file says *where* an object is and which particular one it is. The archetype
registry says *what it is and what it can do*, once, for every object of that kind.

```json
{
    "archetypes": [
        {
            "id": "station.trade_hub",
            "name": "Trade Hub",
            "kind": "Station",
            "glyph": "#",
            "color": [200, 200, 210, 255],
            "layer": 3,
            "size": 90,
            "components": {
                "dockable": { "range": 90 },
                "market": {},
                "storage": { "capacity": 5000 }
            }
        }
    ]
}
```

| Field | Type | Description |
|------|-----|----------|
| `id` | string | unique; how world data and the editor refer to this archetype |
| `name` | string | display name |
| `kind` | string | one of `Star`, `Planet`, `Station`, `Field`, `Gate`, `Nebula`, `Derelict`, `Npc`, `PlayerShip` |
| `glyph` | string | the character the ASCII presentation draws |
| `sprite` | string | texture name in `data/textures/`; omit for shapes only |
| `style` | string | `point` (default), `region` or `directional` — see below |
| `color` | [r, g, b, a] | 0..255; `a` defaults to 255 |
| `layer` | int | draw order, lowest first; the same number means the same thing in every backend |
| `shape` | array | **optional** — what the object is made of, as a list of parts. Omit it and the backend falls back to the figure it used to compile in for this kind. See below |
| `design` | string | **optional**, ships only — the ship design it is drawn from, by id in `data/ships.json` (#279). Its shape is then the design's, laid out from the design's sections when the archetypes load, and an archetype that names a design may not also write a `shape` (`documents/ship_design.md`) |
| `material` | string | **optional** — the material that shades it, by id from `data/materials.json`. Omit it and the object is drawn plain. An id `materials.json` does not define also draws plain, and the client and editor log it by name at startup |
| `light` | object | **optional** — this object lights the system: `radius` (world units at which its light has fallen to nothing), `intensity` (default 1.0) and `self` (default false: whether it lights itself). Omit it and the object emits nothing. See below |
| `size` | number | default radius when the instance does not give its own |
| `world` | object | where this archetype lives in a system file — see below |
| `components` | object | what the object can do — see below |

The look fields — `glyph`, `style`, `color`, `layer`, `size` — are editable in the world
editor's gallery (`worldeditor gallery`, or F3), which shows every archetype at once and
writes the changed values straight back into this file. It edits the text in place rather
than reformatting it, so what lands in the diff is the value that changed and nothing
else. Everything else in an archetype is still edited by hand.

### `light` — what lights a system

A system's lighting is built from the objects in it, not from a field on the system (#119).
Anything whose archetype has a `light` is a light: the three stars carry one, and a beacon
a player builds becomes one the moment its archetype says so — no renderer change, which is
the same bargain the rest of the archetype makes.

That it is a **list** matters. A system with two stars is something this format can already
describe, and an object between two equal stars has no dark side at all. Objects light from
the strongest few sources; a light past its radius contributes nothing rather than a little,
so a star on the far side of a system cannot decide which way something near you is lit.

```json
"light": { "radius": 2480000, "intensity": 1.00, "self": true }
```

**`self`** says whether the light lights the object it belongs to, and it is `false` unless
written. A light stands at its object's centre, so if it lit that object it would reach
every part of the hull at full strength from inside: no lit side, no dark side, flat paint.
So by default an object's light lights everything else in the system and not the object,
whose hull is lit by the system's other lights like any hull — a beacon keeps its volume
under the star. `"self": true` takes the object out of shading altogether and draws it at
its own colour, which is what a star is: a light, not a surface. The three stars set it.
Either way, parts with the role `light` glow; they are never shaded.

A reach well past `SYSTEM_RADIUS` (1 000 000) is normal: the falloff is quadratic, so a star
that only just covers its system leaves the outskirts almost unlit. These numbers were set
by looking at a whole system in the editor, not derived.

Ambient light is a floor, not a property of the data: nothing is ever drawn fully black,
because a space sim that is honestly black is unreadable, and this one is played zoomed out
where an object is a few pixels and its dark side is most of them. A system whose star has
no `light` is drawn unlit — at full colour — rather than dark.

The gallery edits `light` like any other look field and writes it back here, because how
far a star reaches is the one number that decides whether a system reads as lit or as a
dark map with a lamp in the middle, and that is only decidable by looking.

### `shape` — what an object is made of

An object is a **composition**, not a figure (#122). A trade hub is a hexagonal core, a
ring, three arms with pads on them, a mast and a row of lamps — and none of that appears in
C++.

```json
"shape": [
    {"form": "polygon", "sides": 6, "radius": 0.55, "role": "hull"},
    {"form": "capsule", "at": [0.85, 0.0], "repeat": 3, "length": 0.75, "width": 0.14,
     "role": "hull", "jitterAngle": 4},
    {"form": "ring", "radius": 1.55, "width": 0.08, "role": "trim", "minPixels": 34},
    {"form": "disc", "at": [1.55, 0.0], "repeat": 6, "radius": 0.055, "role": "light",
     "minPixels": 60}
]
```

**Every measurement is a fraction of the object's own radius**, so a shape is written once
and works at any size — a ninety-unit station and a sixteen-unit ship use the same grammar.

| Field | Type | Description |
|------|-----|----------|
| `form` | string | `disc`, `ring`, `polygon`, `capsule`, `chevron`, `bar`, `lattice`, `band`, `arc` |
| `role` | string | `hull` (the object's colour), `panel` (darker), `trim` (lighter), `light` (emissive, never shaded), `antenna` (thin and dim) |
| `at` | [x, y] | offset from the centre, in radii |
| `angle` | number | the part's own rotation, degrees |
| `radius` | number | `disc`, `ring`, `polygon` |
| `width` | number | ring thickness; the across-measure of the elongated forms |
| `length` | number | `capsule`, `chevron`, `bar`, `lattice` |
| `sides` | int | `polygon` |
| `filled` | bool | `polygon`: solid or outline |
| `count` | int | `lattice`: how many struts |
| `repeat` | int | drawn n times, each turned a further 360/n about the centre |
| `mirror` | bool | drawn again, reflected across the object's own axis |
| `minPixels` | number | this part appears only once the object is at least this many pixels across |
| `jitterAngle` | number | degrees the seed may turn this part |
| `jitterScale` | number | fraction the seed may resize it |
| `alpha` | number | 0..1, how solid this part is; a corona is a glow rather than a ring |
| `lat`, `lon` | number | degrees; giving either puts the part **on the sphere** instead of on the disc (see below) |
| `orbitRadius` | number | in radii; non-zero makes this part **orbit** the body instead of sitting on it, and `at` is then ignored |
| `orbitPeriod` | number | seconds for one lap |
| `tint` | [r,g,b] | the part's own colour instead of the object's: a red lamp on a grey hull, hot cracks on a dark world (#214) |
| `from`, `to` | number | `arc` only: the segment's start and end, degrees, measured like `angle` |
| `row` | object | `{ "count": n, "step": [dx, dy] }`: n copies along a line, each `step` further (in radii, before the object turns). Ribs, ports, containers (#214) |
| `orbitPhase` | number | 0..1, where in the lap it starts |
| `orbitTilt` | number | 0 is edge-on (a line across the body), 1 is seen from above; in between is where it passes behind |
| `spin` | number | degrees per second this part turns about the object's centre |
| `blink` | number | seconds per cycle for a `light`; 0 is steady |
| `onlyThrusting` | bool | the part exists only while the object's engine is burning |

**The vocabulary is narrow on purpose, and that is the feature.** Seven primitives with
strict proportions produce a family of objects that looks intentional; an open-ended set of
arbitrary shapes reads as programmer art. Like a font: few strokes, many letters.

**`repeat` and `mirror` are different symmetries.** `repeat` turns about the centre, which
is what a station is: arms, lamps, pads, vents. `mirror` reflects, which is what a ship is:
a pair of wings is not a wing rotated half a turn — that puts the second one in front of
the nose.

**Detail arrives with distance.** `minPixels` keeps a hundred parts from being resolved
into eight pixels on the system map, where the result is a smudge rather than a small
object.

**Two of the same thing differ.** `jitterAngle` and `jitterScale` are perturbed from the
object's id, so two trade hubs are not identical and neither shimmers between frames —
repeated identical assets are the other way procedural art gives itself away.

**A part belongs either to the body or to the space around it.** A surface feature sits on
the sphere and turns with it. An **orbiting** part travels around the object, passes behind
it and in front of it, and the whole of that depth is draw order: `Compose` returns pieces
sorted back to front, so the body hides what is behind it and covers nothing in front. A
backend draws them in order and never has to know why.

This is draw order, not occlusion. A part larger than the body, or one orbiting closer than
the body's own radius, will pop rather than slide — keep an orbit outside the body.

**A planet's surface is on a sphere, not on a disc** (#166). A part that gives `lat` or
`lon` is projected onto the body: it travels across the face as the planet turns (`spin`
is then degrees of longitude a second), is squashed into an ellipse near the limb, and is
simply absent while it is round the back. A `band` is always on the sphere: `lat` is its
middle and `width` is in **degrees** of latitude. It is computed, not drawn — the visible
part of a latitude strip — so it narrows to nothing at the poles and never crosses the limb.

A body can say how far its north pole is tipped toward the viewer. That makes the shape an
object rather than a bare list:

```json
"shape": { "tilt": 20, "parts": [
    {"form": "disc", "radius": 1.0, "role": "hull"},
    {"form": "band", "lat": -6, "width": 14, "role": "panel"},
    {"form": "disc", "lat": -16, "lon": 40, "radius": 0.15, "role": "trim", "spin": 9.0}
]}
```

The tilt is what makes a band **curve**. Exactly edge-on, latitudes are straight chords and
the planet reads as a disc with stripes; tipped a little, they bow, and it reads as a ball.
It belongs to the body rather than to a part because every feature on one planet shares one
axis. A shape with no axis — a station — stays a bare list.

A surface feature is lit **as the body**, with the body's centre and radius, so it falls into
the planet's terminator; shaded as itself, a crater would carry a small lit side of its own
into the night. A band does not move when the planet turns, and that is right — a gas
giant's belts are the same all the way round, and it is its *storms* that go by.

**A part turns only if it is a surface feature.** A crater, a storm or a lava crack belongs
to the surface and goes round with it. A latitude band or a polar cap does not: rotating
one in two dimensions turns it into a stripe sweeping across the planet, which was the
first thing that went wrong when planets were given a spin.

**Everything that moves is a function of the clock and the seed** — never of anything
accumulated per frame. A part whose angle were integrated would drift between clients, and
two players would see the same station turned differently. As written, every client
computes the same answer from the same time and nothing about motion crosses the wire.

A blink takes its **phase** from the part's own seed, so six lamps on a ring are a sequence
rather than a pulse; in step they read as a screensaver. A blink never reaches zero, because
a lamp that goes out reads as a part that has fallen off.

**A shape may reach past its radius**, and a docking ring at 1.55 is the point of a docking
ring. Anything framing an object asks `Render::Extent` rather than assuming.

Stars, nebulae and belts have shapes like everything else, and the body of each -- a star's
photosphere, a nebula's gas, a belt's rocks -- is a part painted by a material of its own
(above). What they had before is kept as their stand-ins.

### `material` — a shader and what feeds it

`data/materials.json` names materials; an archetype names one of them (#121). A material is
a shader plus a list of **bindings**: uniform name on one side, where its value comes from
on the other.

```json
{
    "id": "hull",
    "shader": "hull",
    "bindings": {
        "centre": "item.screenPos",
        "radius": "item.screenSize",
        "lightDir": "light.dir",
        "damage": "item.intensity",
        "time": "clock.time",
        "grit": 0.35
    }
}
```

A binding's value is either a **source name** (a string) or a **constant** (a number, a
bool, or one to four numbers). The sources:

| Source | Type | What it is |
|------|-----|----------|
| `item.color` | vec4 | the object's own colour, 0..1 |
| `item.intensity` | float | hull left, ore left, how looted a wreck is |
| `item.heading` | float | radians |
| `item.thrusting` | float | 0 or 1 |
| `item.size` | float | world units |
| `item.screenPos` | vec2 | where its centre landed, in pixels |
| `item.screenSize` | float | its radius after the camera, in pixels |
| `item.axis` | vec2 | which way an elongated part runs; zero means it is round |
| `light.dir` | vec2 | unit vector toward the strongest light |
| `light.tint` | vec4 | the colour of the light arriving: for a rim or a glow |
| `light.surface` | vec4 | what that light does to an albedo -- its hue capped (#361) |
| `light.strength` | float | 0..1 |
| `light.ambient` | float | the floor |
| `clock.time` | float | seconds |
| `item.seed` | float | 0..1, the object's own, from its id -- two planets of a kind differ |
| `item.partSeed` | float | 0..1, this part's own -- one rock in a belt from the next |
| `item.angle` | float | radians, the part's own turn with its spin -- how far a planet has turned |
| `texture.noise` | sampler2D | a fixed table of random values noise is read from (#426) |
| `item.partShape` | vec4 | the part's half length, half width (a ring's thickness), sides and form, in the object's radii (#516) |
| `item.frame` | vec4 | the whole object on screen: centre (pixels), radius (pixels), heading (#516) |
| `livery.accent` | vec4 | the livery's stripe colour (#516) |
| `livery.params` | vec4 | the livery's wear, grime, stripe pattern, panel scale |
| `livery.style` | vec4 | the livery's panel aspect, patchwork, window warmth, windows lit |

**An unknown source is a load error**, unlike an unknown pass in `look.json`, which is
skipped. Losing a pass makes the picture plainer; losing a uniform makes a material draw
something actively wrong, and it would do it in silence.

A material is bound **per part** of a composition rather than per object (#135): each part
gets its own centre, its own cross section and its own axis, so an arm is lit as an arm
rather than as the slice of a sphere it happens to be standing in. A part narrower than a
few pixels is drawn flat instead — below that there is no surface left to shade, and a rail
two pixels wide is *entirely* the part of a cylinder that turns away from the viewer.

The shader lives at `data/shaders/materials/<shader>.fs` and works out where a fragment sits
on the object from `gl_FragCoord` and the object's centre and radius in pixels — so it sits
on top of whatever primitive the backend drew, a disc or a hexagon or a triangle, rather
than needing geometry of its own. As with the screen treatment, a shader that will not
compile costs that material and nothing else: the object is drawn the way it was drawn
before materials existed.

**Painting a whole body (#426).** A material may also say how far past its part it draws,
`"reach": 1.06` (1 to 8, in the part's radii; default 1): a planet's air beyond its limb, a
star's corona over a few radii. The shader is still told the part's own radius. Such shaders
share a library, `data/shaders/materials/body.glsl` -- noise, the sphere under a disc, the
light, the air -- pasted in by a line `#include "body.glsl"`; a missing include costs the
material its shader like a compile error. Its noise reads `texture.noise`, so a material that
includes it binds `"noiseTex": "texture.noise"`. A body that covers more of the screen than
the backend can afford to paint every frame (`paintBudget` in `data/look.json`, millions of pixels, default 1) is painted
at reduced resolution -- never below half -- and scaled back up. `worldeditor ... noshaders`
compiles no materials at all, which is how the stand-ins are judged.

**Finishing a built thing (#516).** Two more fields on a material, and a list beside them
(`documents/built_surfaces.md` is the design):

- `"roles": { "panel": "plating.recess", "antenna": "truss" }` -- an object on this material
  draws its parts of those roles on those materials; any other role stays on this one. A key
  that is not a role, or a material not in the file, is a load error.
- `"fallback": "hull"` -- what to draw with when this material has no shader on the player's
  driver, followed down the chain. Without one the part is drawn plain. A material naming one
  that is not in the file is a load error. `worldeditor ... nopaint` switches off every
  material with a fallback, which is how the fallen-back look is judged.
- `"liveries": [ { "id": "guild", "base": [242, 236, 222], "accent": [38, 78, 150],
  "wear": 0.2, "grime": 0.2, "stripes": 1, "panels": 1.0, "aspect": 2.4, "patchwork": 0.03,
  "warmth": 0.7, "windows": 0.5 }, ... ]` -- a people's palette, picked by the culture of the
  faction that owns the object (`factions.json`), or by the faction's id when it has none; the
  one called `default` for anything else. `base` multiplies the object's colour before it is
  drawn, shader or not; the rest reach a material through the `livery.*` sources. An unknown
  field is a load error (`note` is allowed).

A built object whose surface reads its parts (`item.partShape`, `item.partSeed`, `item.angle`)
is bound by `paintBudget` as a painted body is: covering more of the screen than the budget, it
is drawn whole at reduced resolution and scaled back up.

### `style` — the glyph grammar

Glyphs were the game's primary look until 2026-09-03 and are being demoted to a sensor
view (#123); the grammar below is what they still draw, and it is deliberately narrow so a
readout is legible at a glance:

- **glyph** = what class of thing this is (a station is `#` whatever it trades in)
- **colour** = whose it is (faction paint, star type, ore remaining)
- **size** = how big it actually is, straight from the world

What an object *can do* is deliberately **not** encoded in the character — that is what
the overview panel and the component list are for. Otherwise a player-built structure
would need a new letter, and needing new art per object type is what glyphs exist to
avoid.

| `style` | Drawn as | Used by |
|------|---------|--------|
| `point` | one character, sized from the object | star, planet, station, gate, derelict |
| `region` | the character scattered around the extent | nebula, asteroid belt |
| `directional` | the character turned to face the heading | ships |

`region` exists because an area is not an object: a nebula three thousand units across,
drawn as a single character scaled to fit, would cover everything inside it.

### `world` — how the archetype maps onto a system file

```json
"world": { "category": "stations", "subType": "Military" }
```

`category` is the array in `data/systems/<id>.json` this object is written to; `subType`
is the value of that category's type key (`role` for stations, `type` for planets). An
archetype **without** a `world` block is not something the editor places — a star is one
per system, a ship is not scenery.

The editor's creation palette is generated from every archetype that has a `world` block,
so **adding an archetype puts it in the editor with no editor change at all**. This block
is transitional: it disappears once system files name archetypes by id directly.

### Components

Simulation passes look for a component, not for a class: "everything dockable within
range" rather than "everything that is a `Station`". An object gains a behaviour by
declaring the component, without the pass being edited.

| Component | Parameters | Meaning |
|------|----------|--------|
| `dockable` | `range` | a ship can dock; `range` is added to the object's radius |
| `mineable` | `extractRate`, `range` | holds a deposit; `extractRate` multiplies the mining ship's own rate (1 is an ordinary belt, and the default) |
| `market` | — | buys and sells resources |
| `defensive` | `range`, `damage` | a station fires once a second on the nearest hostile ship within reach; `damage` is per second |
| `storage` | `capacity` | units of goods a player's structure keeps off any ship (#438) |
| `jumpLink` | `range` | connects this system to another |
| `hazard` | `radius`, `hidesShips` | changes conditions inside it; `radius` 0 means the object's own radius |
| `salvageable` | `range` | pays out once to whoever reaches it first |
| `buildable` | — | a player can construct one; how is its blueprint's (`blueprints.json`, #39) |
| `signature` | `strength` | hidden from a pilot until their scans resolve it (#346); each pass of the scanner adds `strength` times its power (above 0, at most 1). Its owner always sees it. Roaming holes 0.5, a hidden base and a rich belt (`field.rich`) 0.35, a derelict cache (`derelict.cache`) 0.25 |
| `shipyard` | `markup` | a docked ship can be refitted here: a module costs its ore at the system's prices times `markup` (at least 1; default 1.5), and one taken out refunds half its ore (#279) |

Every `range` is added to the object's own radius, and every verb that reaches for
something reads it from the object being reached rather than from a constant beside the
rule. That is what lets a player-built dock, belt or gate work at its own distance without
the docking, mining or jump pass being edited (#44).

Hostile, for `defensive`, means what it means to an NPC of the station's faction: a player
by their own account (wanted by it, or at a hostile reputation), an NPC by the relation
matrix. A docked player or one hidden in cover is not shot at. A faction's outpost fires the
same way, for its faction. A player's armed structure -- the `battery` blueprint -- has no
side to take, so it fires as its owner's rules of engagement say (#340): hold, return fire
(the default), enemies on sight, or everyone not on the allow-list. Return fire answers
whoever shot at the structure in the last minute, and whoever shot its owner's ship inside
its reach. The allow-list is sent to the owner only; everyone else's layout carries the mode.

`storage` is what a player's structure keeps (#438): its owner and the members of the owner's
corporation, docked at it, move goods between the hold and it, up to `capacity` units. What it
keeps is state, saved with the structure (`stored`), and survives an upgrade; a structure
that keeps anything is not dismantled until it is emptied. A station's archetype may declare
it too, for when a player's structure grows into one. `buildable` is a marker: a blueprint may only name an archetype that declares
it, and what building one costs and takes is the blueprint's, not the archetype's, because
one kind of object may be made more than one way.

Components carrying no parameters are still written as `{}` — presence is what matters.

Some values deliberately live on the *instance* rather than on the archetype, because
they vary between two objects of the same kind: where a gate leads, what a wreck pays
out, how much ore a belt still holds.

**A malformed registry is a hard failure, not a degraded load.** An unknown `kind`, an
unknown component name, a duplicate `id` or a missing `id` aborts the load and leaves
the previous registry in place. So does a field the reader does not know -- at the top
level, in `light`, in `world`, in a component's parameters or in a part of the `shape` --
because a misspelled field would otherwise be read as absent and take its default. So
does a `world.category` no system file has, and a `world.subType` the world loader would
read back as something else: an unknown station role becomes a trade hub, an unknown
planet type becomes rocky (#191). A typo would otherwise produce an object that silently
does nothing. `econserver` refuses to start on a registry that will not load.

---

## systems/<id>.json — star system

Every section is optional (an entire array may be omitted). `pos` is `[x, y]` in units.
`color` is `[r, g, b]` (0–255).

**A document written by hand is read strictly** (#191, #357): a file here, a pin's
`document` and a pack's systems. A field that is not in the tables below, a star type, planet
type, station role or faction nobody knows, or an archetype that is not of the object's kind
is a load error that names it and where it is. A file that fails does not load at all; a pin
that fails is not applied, and the server log says why; `econserver validate` says the same
for packs and pins without running a world. What the generator and the server write -- a
region's seal, the objects a save added -- is read as it stands, so a world saved by another
build still loads.

A system beyond the wormhole is not a file: the server generates it from the world's seed
(#140) in exactly this format, so everything below describes both. Each generated region
fills one **slot** of the galaxy's skeleton and is made from (seed, slot) alone (#342); its
systems' ids are `r<slot>-<depth>-<k>`. `econserver galaxydump <seed> [slot ...]` prints the
skeleton, the starts and the regions a new world opens with (plus any named), and
`econserver regiondump <seed> [slot]` one region -- the first centre unless a slot is named.

A slot is one of two kinds (#342 S2):

- **A civilisation centre** -- 3 by default, 1 to 6 by `econserver host --centres N`, placed
  on one ring of the skeleton so that each is four to six slots from its nearest neighbour.
  A centre is 8 to 14 systems round a **heart** (`r<slot>-1-1`): every system is named at
  generation and owned by the centre's major faction (`kind: "Major"` in factions.json;
  pirates have none), security is 1.0 at the heart and falls to about 0.5 at the edge, and
  every system has a station. The heart has one of each role -- `TradeHub`, `Shipyard`,
  `Military`, `MiningOutpost` -- and is where a new pilot may start. Nothing leads into a
  centre: it has no wormhole.
- **A frontier region** -- what the generator made before, opened through a wormhole from
  a neighbouring region's edge. Its law and its riches follow its lattice distance to the
  nearest centre (`hops`): next to one it is as it always was, and each step further out
  starts it with less security, more ore in its belts, likelier finds and more pirates.

A new world opens every centre and one frontier slot beside each, so there is somewhere to
go from every start. The world file records the seed, the rules and the number of centres;
the regions themselves are remade.

```json
{
    "star":  { "type": "Yellow", "size": 150000 },
    "planets":        [ … ],
    "stations":       [ … ],
    "asteroidFields": [ … ],
    "nebulae":        [ … ],
    "derelicts":      [ … ],
    "gates":          [ … ],
    "structures":     [ … ]
}
```

### star, or stars[]
| Field | Type | Values |
|------|-----|----------|
| `type` | string | `Yellow` (default), `Red`, `Blue` |
| `size` | number | radius |
| `pos` | [x,y] | `stars[]` only: where this one stands |

`star` stands at `(0,0)`. A system with several -- a binary (#142) -- lists them in `stars`
instead, each at its `pos`; each lights the system (#119). Planets orbit `(0,0)` either way.

### What every object may also carry

| Field | Type | Meaning |
|------|-----|----------|
| `archetype` | string | Which archetype **of its kind** this object is, in place of its kind's ordinary one (#142): a derelict that is `derelict.leviathan`, a belt that is `field.motherlode`. In a hand-written document a name that is not an archetype, or is one of another kind, is a load error; in what the server wrote it is logged by name and the object built as usual. |

### orbits — a planet's satellite

A station, belt, nebula or derelict may orbit a planet instead of standing still (#210):

```json
"orbits": { "planet": 0, "radius": 19700, "speed": 30, "phase": 0.245 }
```

| Field | Type | Meaning |
|------|-----|----------|
| `planet` | int | index into this system's `planets[]` |
| `radius` | number | distance from the planet's centre |
| `speed` | number | along the circle, units per second (like `orbitSpeed`) |
| `phase` | number | angle round the planet at world time zero, radians |

`pos` is then left out: the position is the planet's plus a turning offset, a function of
world time computed the same way by the server, the editor and the tests. **The rule is
that an object in a planet's path belongs to that planet**: the generator makes it a
satellite rather than moving it, and anything static keeps clear of the planet's whole
path -- the planet and what orbits it. A planet's `orbitSpeed` plus its satellite's
`speed` stays well under the slowest ship's top speed; a station that outruns every hull
is a station nobody docks at. Gates never orbit: they are the topology.

### character (generated systems)
A string the generator writes to say what kind of system it made (#142): `ordinary`,
`binary`, `belt cluster`, `shrouded`, `graveyard`, `giants`, `frozen`, `barren`. Nothing
at runtime reads it; the survey screen (#141) and a reader of the document do.

### planets[]
| Field | Type | Values / description |
|------|-----|---------------------|
| `orbitRadius` | number | orbit radius |
| `orbitSpeed` | number | linear orbital speed, units per second; tens, not hundreds, if anything orbits it |
| `angle` | number | starting angle, radians |
| `size` | number | planet radius |
| `type` | string | `Rocky` (default), `Gas`, `Ice`, `Lava`, `Oceanic` |
| `deposit` | string | subsurface resource: `Iron`, `Ice`, `Crystal` |
| `color` | [r,g,b] | **optional** — if absent, the color from `type` is used |
| `name` | string | written by the generator for a rogue world; not shown -- every planet is named after its system and its place from the star (#259) |

### stations[]
| Field | Type | Values |
|------|-----|----------|
| `name` | string | name |
| `pos` | [x,y] | position |
| `size` | number | radius |
| `faction` | string | a faction `id` from `factions.json` (or a pack's own); default the `unaffiliated` one. An unknown id is a load error in a hand-written document |
| `role` | string | `TradeHub` (default), `MiningOutpost`, `Shipyard`, `Military` |

> `Pirates` as a station/object faction cannot be set in JSON — pirates are spawned by
> the engine. `role` influences the bias of mission generation.

### asteroidFields[]
| Field | Type | Values |
|------|-----|----------|
| `name` | string | name |
| `pos` | [x,y] | position |
| `size` | number | cluster radius |
| `resource` | string | `Iron`, `Ice`, `Crystal` |
| `ore` | int | ore reserve |

> Belts farther than `MID_RADIUS` (775 000) from the center — with pirates.

### nebulae[]
| Field | Type | Description |
|------|-----|----------|
| `name` | string | name |
| `pos` | [x,y] | center |
| `radius` | number | cloud radius; inside it the player is hidden from pirates |

### derelicts[]
| Field | Type | Description |
|------|-----|----------|
| `name` | string | name |
| `pos` | [x,y] | position |
| `size` | number | radius (optional, default 16) |
| `reward` | number | credits for salvaging (optional, default 500) |
| `cargo` | object | what it holds, good id -> whole amount, e.g. `{ "Circuits": 20, "Medicine": 8 }` (#346). A search takes what the hold has room for before the credits are paid |

### gates[]
| Field | Type | Description |
|------|-----|----------|
| `name` | string | name |
| `pos` | [x,y] | position |
| `size` | number | ring radius |
| `destination` | string | **destination system id** (from `universe.json`) |

Jump: the player arrives at the gate of the target system whose `destination` points
back to the system they left. For the return trip to work, each pair of systems must
have gates pointing to each other.

### structures[]
What players built (#39). Written by the server into `world.json` as an added object, in
this format, and read back by the same loader; a hand-written system may carry one too, and
it then belongs to the world. Unlike the arrays above an entry must name its archetype --
there is no ordinary structure to fall back on -- and one that names anything but a
`Structure` archetype is skipped with a warning.

| Field | Type | Description |
|------|-----|----------|
| `archetype` | string | **required**: the `Structure` archetype it is, or will be once built |
| `blueprint` | string | the blueprint it was made from: what dismantling returns and what destroys it; absent -- the first blueprint that builds its archetype |
| `name` | string | what its builder called it (default: the archetype's name) |
| `pos` | [x,y] | position |
| `size` | number | radius (optional, default the archetype's) |
| `startedAt`, `completesAt` | number | world seconds: a construction site between the two, wearing `structure.site`; absent -- built |
| `expiresAt` | number | world seconds at which it is taken away by itself; absent -- it stays |
| `orbits` | object | a satellite of a planet (#210), as on any other object: a faction's orbital outpost (#318) |

The time line is instants of the world clock, not a progress figure (#136): nothing
accumulates, so a site is as far along on every client as on the server, and a server that
was down when a site was due finishes it on its first tick back.

---

## goods.json — what markets trade (#343)

The goods are a registry, as the factions are: `ResourceType` is a handle into this file, a
save names a good by its `id`, and the wire sends goods in the file's order -- the server,
the client and `econagent` all read it. Strict like the other content files: an unknown
field, a good named twice or an amount naming a good that is not there is a load error.

```json
{
    "market": { "fee": 0.02, "spread": 0.1, "elasticity": 0.7, "floor": 0.35, "ceiling": 3.0,
                "beltMined": 0.15, "beltRegrow": 0.1,
                "haulEvery": 120, "haulCargo": 30, "haulMargin": 0.1 },
    "goods": [
        { "id": "Iron", "name": "Iron", "tier": 0, "price": 10, "stock": 400,
          "supply": "local", "mined": true }
    ],
    "planets": { "planet.oceanic": { "Food": 40 } },
    "industries": {
        "station.mining_outpost": {
            "recipes": [ { "in": { "Iron": 3 }, "out": { "Steel": 1 }, "rate": 20 } ],
            "uses": { "Food": 8 }
        }
    }
}
```

| Field | Meaning |
|---|---|
| `goods[].tier` | 0 raw, 1 refined, 2 components and consumables |
| `goods[].price` | a civilisation centre's price at the target stock |
| `goods[].stock` | what one market keeps: the target its price is set by; a system with two markets keeps twice |
| `goods[].supply` | `local` (cheaper further from a centre) or `imported` (dearer), #342 S5 |
| `goods[].mined` | an asteroid belt can hold it |
| `market.fee` | share of every trade's value, paid by whoever trades, human or agent (#348) |
| `market.spread` | between what a market asks and what it bids |
| `market.elasticity`, `floor`, `ceiling` | price = base x clamp((target / stock)^elasticity, floor, ceiling) |
| `market.beltMined` | per hour: the share of what is in each belt the system's miners take for its market (two miners is a full crew) (#348) |
| `market.beltRegrow` | per hour: the share of a full belt that grows back; a belt only the miners work settles at `beltRegrow / beltMined` of full |
| `market.haulEvery` | seconds: how often at most a system with a market sends a trader next door with goods (0: nobody hauls) |
| `market.haulCargo` | units a hauler carries at most |
| `market.haulMargin` | how much more than the ask here, fees both ways included, the bid next door must pay for a unit to be carried |
| `planets` | what a planet of that archetype grows for a market in its system, per hour |
| `industries` | per archetype with a market: `recipes` (cycles an hour, each taking `in` and giving `out`, as far as the inputs are in stock) and `uses` (per hour) |

A system has one market, kept by the coarse pass in every system, hot or cold: belts and
planets add, recipes convert and uses take away. Nothing pulls a stock toward its target:
what one system has and another lacks is carried by NPC haulers (#343) -- the system's own
traders, setting out with the good whose bid next door beats the ask here by the margin, as
much of it as still pays. The goods leave the market when the hauler sets out and arrive with
it, and are counted nowhere on the way (`world.json` keeps them on the trip, under
`migrants[].cargo`). Where somebody is, the hauler is a real ship: shot down, it leaves its
cargo in a wreck. The stock is saved in `world.json` per system (`galaxy.<id>.stock`, by good
id); a belt mined below full keeps what is left as `ore` in its system's changes, and the fees
a faction's markets have taken -- from pilots and from NPC haulers alike -- are `treasuries`, by
faction id. A market short of a good posts a delivery contract (#407, rules in the `contracts`
section of `world_rules.json`); the open ones are `contracts` (`id`, `faction`, `system`, the
`station` by its key, `good`, `amount`, `reward`, `postedAt`, `deadline`, `taken`) with
`nextContract`, and what a contract pays was taken out of its owner's treasury when it was posted.

---

## blueprints.json — how a player builds something (#39)

```json
{
    "blueprints": [
        {
            "id": "beacon",
            "name": "Beacon",
            "archetype": "structure.beacon",
            "cost": { "Iron": 10, "Crystal": 2 },
            "buildSeconds": 45,
            "lifetime": 86400,
            "hull": 400,
            "wreck": "derelict.wreck",
            "placement": { "reach": 600, "clearance": 400, "bodyClearance": 5000, "perSystem": 8 }
        }
    ],
    "limits": { "perAccount": 12 },
    "dismantle": { "refund": 0.5 },
    "destruction": { "siteHull": 0.25, "wreckShare": 0.25 }
}
```

| Field | Description |
|------|----------|
| `id` | what the deploy command and `econagent`'s `deploy` name |
| `name` | what a player is shown, and what a new one is called unless its builder names it |
| `archetype` | what it becomes: a `Structure` archetype that declares `buildable` -- or, for an upgrade (`onto`), a `Station` archetype that does |
| `cost` | resources taken from the hold when the site is laid down, all at once; at least one |
| `buildSeconds` | world seconds from site to finished object |
| `lifetime` | world seconds it stands once finished; 0 or absent -- until removed |
| `hull` | **required**, more than 0: the damage that destroys the finished thing. Anything buildable can be destroyed |
| `wreck` | the `Derelict` archetype it leaves when destroyed; absent -- nothing is left (a buoy) |
| `placement.reach` | at most this far from the ship laying it down; must be more than 0 |
| `placement.clearance` | gap to the edge of any station, gate, wreck or structure |
| `placement.bodyClearance` | gap to a star's surface, and to the band a planet -- or a satellite with its planet -- sweeps on its orbit |
| `placement.perSystem` | how many of what this blueprint builds one system may hold, everyone's together; 0 -- no cap |
| `builders` | who may build one: any of `"player"`, `"faction"`; absent -- `["player"]`. A blueprint no player may build is listed in no menu or tool (#295) |
| `onto` | an upgrade (#342 S6): the blueprints of the finished structure it is built onto. That structure becomes its site -- keeping its place, its owner and the claim it holds -- instead of a new one going down beside it; finished, an upgrade into a `Station` archetype is a station of its builder's faction. Absent -- laid down on its own. A player builds an upgrade only onto a finished structure of their own (#433), at the same cost and time; theirs stays a structure, still theirs, when it becomes a station |
| `limits.perAccount` | how many structures one account may have standing, in the whole galaxy; 0 -- no cap |
| `dismantle.refund` | share of the cost an owner gets back for dismantling a finished structure (default 0.5) |
| `destruction.siteHull` | a site's hull at the moment it goes down, as a share of `hull`; it grows to all of it at completion (default 0.25) |
| `destruction.wreckShare` | share of the cost, in credits at the base price, a destroyed structure's wreck pays whoever searches it (default 0.25) |

**Taking things down (#39).** The owner dismantles their own site or structure from within
`placement.reach` of it, and the hold gets back, per resource and rounded down,
`cost x (1 - progress x (1 - refund)) x hull left`: all of it for a site cancelled the moment
it went down, falling to `refund` at completion, and less for whatever has been shot off.
Half is the default because a build-and-take-down cycle must cost something -- otherwise a
structure is a free way to block a spot and move on -- while misplacing one should not cost
it all; and it is more than the wreck holds, so having your own shot down never pays better
than taking it apart. What the hold has no room for is a refusal, not a loss.

Anyone may shoot a site or a structure; a site is weaker the earlier it is. Hitting one is a
crime where it is someone's property under a law: a lawful faction's own structure is that
faction's, and a player's (or the world's) is under the law of a lawful system controller.
The price is the one for attacking that faction's ships -- reputation and bounty per hit,
more for the kill. A pirate outpost, anything in a lawless system, and your own are not
crimes. A destroyed structure leaves its `wreck` where it stood (on its orbit, if it had one), and a faction's outpost is
lost from the chronicle; the claim it made stands until the system is taken (#295).

A faction builds an outpost (#295): its stock pays the cost, counted as the sum of the
amounts, and `limits.perAccount` does not apply. The structure's owner is `faction:<id>`,
and when it is finished the system, if nobody held it, is the faction's.

An outpost is built **for** something (#318), and each purpose is its own blueprint and
archetype, all at the cost of `outpost`. The purpose is what the faction valued most in the
system, by its temperament's weights; the place follows from it, always under the same
placement rules as a player's site except `reach`:

| Purpose | Chosen when | Blueprint | Where it goes |
|------|------|------|------|
| mining | ore: `values.ore` × belts | `outpost_mining` | beside the richest belt (by what it was made with) |
| watch | traffic: `values.traffic` × the system's prosperity, or two gates (a corridor) or three (a junction) | `outpost_watch` | on the way between the gate it came by and another, or at the junction; never within 40 000 of a gate |
| orbital | a planet to hold: `values.unclaimed` × planets | `outpost_orbital` | a satellite of the largest planet with room (`orbits`, #210), in the next free orbit within the planet's reach |
| salvage | wrecks: `values.salvage` × wrecks | `outpost_salvage` | beside the wreck with the most others near it |
| hidden | the faction is not `lawful`, whatever it came for | `outpost_hidden` | inside the largest cloud, or out at the edge (80–95% of the radius), as far from the lines between gates as it can |
| — | nothing above 0.2 | `outpost` | an open point between 15% and 70% of the radius, never within 60 000 of a gate |

Beside a belt or a wreck that orbits a planet, the outpost orbits that planet too. A purpose
that has nowhere to go in the system falls back to an open point. The place is drawn from
`Gen::Rng` keyed by faction, system and purpose, so the same world settles the same places.
A structure that orbits carries `orbits` in `world.json` like any other satellite; the damage
it has taken is state beside it, and a destroyed one leaves its wreck on the same orbit.

A player's only armed blueprint is the **defence battery** (`battery`, #340): a small gun
platform, below an outpost in reach (1200), damage (4 a second) and hull (1500), at about two
and a half beacons' cost. It is not an outpost: it claims nothing and no faction builds it.

Every outpost leaves a `derelict.outpost_ruin` when destroyed. Their hulls differ by what they
are for: a watch post and a salvage post are light (3000), an ordinary, mining or orbital
outpost is 4000, and a pirate's hidden base, dug in where nobody looks, is 5000.

The server checks every rule again whatever a client says; a refusal is a journal notice
that names the rule ("too close to Aurora Hub (keep 400 clear)", "the hold has 0 Crystal of
the 2 it takes"). A belt or a cloud is a place, not an obstacle: marking one is what a buoy
is for. The caps are anti-spam until ownership and limits arrive properly (#41).

Load it after `archetypes.json`: every blueprint is checked against the archetype it names.
Like the archetype registry, a malformed file is a hard failure -- an unknown field, a
resource that does not exist (rather than iron by default), an archetype that is not a
buildable structure, a blueprint that costs nothing -- and `econserver` refuses to start
on one. Clients read the file only to offer what can be built.

---

## world_rules.json — the server's rule numbers (#342 S3)

How fast the world grows and what its rules cost, in one file the server reads at start:
how often the tide opens a region, how long a mouth stays, what police and licences cost,
what a job pays. A host who wants a faster or slower world changes a number here and
restarts; no rule's number lives in the C++ (CLAUDE.md, "a rule's number goes in
`data/world_rules.json`"). The reader is `src/game/sim/WorldRules.{h,cpp}`, whose comments say
how each rule works in more words than these tables.

It is strict like every other data file (#191): an unknown section or field is a load error,
every number must be zero or more, a whole number where the table says *count*, and some
fields are checked against each other (named in each table). A file that does not read stops
the server (`FATAL: ...` in its log) rather than run a world by rules nobody chose.

- `tide` is required. Every other section may be left out, and so may any field: what is
  missing takes the **default** below, which is what `WorldRules.h` declares. The shipped
  file sets every field; where it differs from the default, the table says so.
- `schema` is the file's format version, 1. It is not read yet.
- Times are **world time**, which stops while the server is down. "A pass" is the coarse pass
  that steps the whole galaxy, cold systems included.
- `econserver <ticks> --news --rules my_rules.json` runs an empty world under another file, to
  see what a number changes before a server runs on it (`--players K` puts pilots in it).

### tide — how regions open

Every region on the edge gathers tide once a `period`, whether anybody is there or not: from
time, shared out over the edge by each region's strain; from players in it; from how much of
it has been explored; and from factions surveying it. At `threshold × (1 + open / maxRegions)`
-- slower toward the cap -- it opens a mouth into its next slot.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `period` | 60 | seconds | between two steps of the tide; more than 0 |
| `threshold` | 100 | tide | what a region gathers before it opens a mouth; more than 0 |
| `base` | 0.0278 | tide a period | from time alone, for the whole edge: about one region in 60 h |
| `perPlayer` | 0.15 | tide a period | for each player in the region this period... |
| `playerCap` | 10 | count | ...counting at most this many |
| `perExplore` | 0.93 | tide a period | for exploring it, bent by the curve below |
| `exploreKnee` | 0.6 | share | the explored share where it starts to count fast; more than 0 |
| `exploreCurve` | 3 | power | how sharply: below the knee it counts for little |
| `surveyedWeight` | 0.25 | share | a system only a faction has surveyed counts this much of one a player has been in |
| `perSurvey` | 0.2 | tide | for each faction survey of the region this period |
| `minGapHours` | 6 | hours | a region opens no second mouth sooner |
| `mouthHours` | 24 | hours | an unstable mouth's life before it collapses, unless anchored |
| `playersPerMouth` | 20 | count | the world allows one young mouth, and one more for every this many players on; more than 0 |

### mouths — until a mouth is anchored (#342 S4)

| Field | Default | Unit | What it does |
|---|---|---|---|
| `anchors` | true | true/false | false: every mouth is permanent at once, and no Anchor is needed |
| `maxMass` | 45 | mass | the heaviest ship an unstable mouth lets through: a scout or a courier |
| `warnHours` | 2 | hours | before a collapse, the chronicle says the mouth is failing |

### archive — regions nobody comes to (#342 S4)

At the cap, a frontier region nobody has been in for `deadDays`, with no player structure in
it, drifts into the archive and frees its slot.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `deadDays` | 7 | days | without a visit before it drifts away; more than 0 |
| `warnDays` | 2 | days | the chronicle warns this long before |
| `returnChance` | 0.35 | chance | when a slot opens, an archived region comes back instead of a fresh one; at most 1 |

### expeditions — a mouth opening is news (#342 S6)

| Field | Default | Unit | What it does |
|---|---|---|---|
| `reach` | 3 | jumps | every faction holding a system this close to a new mouth learns of it |
| `curiosity` | 0.8 | worth | how much more it values going that way |
| `hours` | 8 | hours | for this long |

### growth — stations grown by need (#342 S6, #432)

| Field | Default | Unit | What it does |
|---|---|---|---|
| `stationsPerRegion` | 4 | count | no region has more stations, whoever built them |
| `marketReach` | 3 | jumps | an outpost becomes an outpost market where the nearest market is this far; more than 0 |
| `marketWorth` | 1.2 | worth | what a market is worth to a faction, times the need |
| `stationWorth` | 1.2 | worth | the same for a trade or mining station |
| `stationNeed` | 0.45 | need | an outpost market becomes a station at this much trade or mining |
| `shipyardAfter` | 1 | count | a shipyard only once the region has this many other stations |
| `shipyardReach` | 4 | jumps | ...and none stands this close |
| `shipyardWorth` | 1.4 | worth | what a shipyard is worth, times the need: more than another station |
| `shipyardNeed` | 0.45 | need | the trade or mining a shipyard needs |

### convoys — what an Anchor or an upgrade is built from (#342 S6, #407)

A convoy's fate follows from its own strength against the dangers of its way. Checks:
`hop`, `reach`, `hauler` and `calm` more than 0; `policed` and `turnBack` at most 1;
`escort` at most `escortMax`.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `hop` | 120 | seconds | spent in each system it passes |
| `reach` | 6 | jumps | it sets out from the faction's holdings this close |
| `hauler` | 1 | ships | the hauler's own worth |
| `escort` | 0.25 | share | of the faction's capacity its holding lends as escort... |
| `escortPerDanger` | 1 | ships | ...plus this many for each ship of the worst danger on the way... |
| `escortMax` | 0.6 | share | ...up to this much of its capacity |
| `calm` | 3 | ships | where the danger is this many ships, it is attacked as likely as not |
| `policed` | 0.6 | share | that chance is this much less at security 1 |
| `harm` | 1.5 | factor | freight lost by how far the danger outweighs it |
| `bite` | 0.5 | share | escort lost, and strength taken off the attackers, in a fight |
| `turnBack` | 0.5 | share | with less freight than this left, it turns back |
| `balk` | 4 | factor | it turns back before a system whose danger is this many times its strength |
| `avoid` | 0.5 | jumps | a ship of danger counts as this many jumps when it picks a way |
| `wait` | 0.35 | share | it is held back while it expects to lose more freight than this... |
| `maxWaits` | 3 | count | ...at most this many times; then it goes anyway |
| `retryMinutes` | 10 | minutes | the next sets out this long after one is lost |

### jobs — what faction plans post (#345, #425)

Checks: `supplyShare` and `supplyCut` at most 1; `creditsPerStock` and `offerLife` more than 0.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `reach` | 4 | jumps | a station of the poster this close to a plan shows its jobs |
| `perBoard` | 3 | count | at most this many on a board, nearest first |
| `creditsPerStock` | 50 | credits | a unit of the faction's stock pays this; it posts nothing it cannot pay for |
| `supplyShare` | 0.5 | share | a supply job asks for this much of the largest line of the site's cost |
| `supplyMarkup` | 1.6 | factor | paid at this times the good's price |
| `supplyRange` | 1500 | units | handed in this close to the site's edge |
| `supplyCut` | 0.35 | share | each delivery takes this off the time the site has left |
| `supplyMax` | 2 | count | deliveries a site at most |
| `escortBase` | 400 | credits | an escort job pays this... |
| `escortPerJump` | 150 | credits | ...and this for each jump of the convoy's way |
| `escortForce` | 2 | ships | a pilot with the convoy counts as this much escort |
| `escortSlack` | 600 | seconds | due this long after the convoy would arrive |
| `siegeKills` | 3 | count | a siege job asks for this many of the besiegers shot down... |
| `siegePerKill` | 250 | credits | ...paying this each |
| `rep` | 3 | standing | done, a job is worth this with the poster |
| `failRep` | 1.5 | standing | failed by the pilot, this less |
| `offerLife` | 1800 | seconds | a station's own work (bounty, mining, delivery) stays on its board this long |
| `stationOffers` | 4 | count | ...this many at a time |

### contracts — markets short of a good (#343, #407)

Checks: `every`, `hours` and `minAmount` more than 0; `maxAmount` at least `minAmount`;
`shortAt` at most `fillTo` at most 1; `premium + premiumShort` less than 1.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `every` | 600 | seconds | how often each market looks at its stock |
| `shortAt` | 0.5 | share | a good below this of its target is short |
| `fillTo` | 0.8 | share | the contract asks for what brings it back to this |
| `minAmount` | 10 | count | units, at least... |
| `maxAmount` | 40 | count | ...and at most |
| `hours` | 2 | hours | due this long after it is posted |
| `premium` | 0.5 | share | the price lies this far from what selling here fetches toward what buying here costs... |
| `premiumShort` | 0.4 | share | ...and this much further as the stock empties |
| `sourceReach` | 3 | jumps | posted only if a market this close sells the units... |
| `sourceMargin` | 0.1 | share | ...for this much less than the contract pays |
| `perMarket` | 1 | count | open at one market at most |
| `perBoard` | 2 | count | on one station's board at most, nearest first |
| `rep` | 2 | standing | done, one is worth this with the poster |

### holes — roaming wormholes (#342 S9)

At the start of every epoch each live region throws for a hole to another region. Checks:
`epochHours`, `minHours` and `budgetMin` more than 0; `maxHours` and `budgetMax` at least
their minima; the chances, `smallWeight + mediumWeight` and `lowMass` at most 1.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `epochHours` | 4 | hours | the schedule's step |
| `perRegion` | 0.3 | chance | a region gets a hole this epoch... |
| `atCap` | 0.6 | chance | ...this, once the galaxy is at its cap and the tide opens nothing new... |
| `drifted` | 1 | chance | ...and this for a region cut off by a collapsed mouth, so the stranded have a way out |
| `minHours` | 4 | hours | a hole lives at least... |
| `maxHours` | 24 | hours | ...and at most this long |
| `smallMass` | 45 | mass | the heaviest ship a small hole lets through: a scout or a courier |
| `mediumMass` | 60 | mass | ...a medium one: and a cutter or a raider |
| `largeMass` | 100 | mass | ...a large one: anything the hangar sells |
| `smallWeight` | 0.4 | share | of holes that are small |
| `mediumWeight` | 0.35 | share | of holes that are medium; large is the rest |
| `budgetMin` | 4 | factor | it collapses after at least this times its class's mass has passed... |
| `budgetMax` | 10 | factor | ...and at most this |
| `endingHours` | 4 | hours | it is called ending this long before it closes |
| `lowMass` | 0.5 | share | its mass is called low below this of its budget |

### scan — the ship's scanner (#346)

| Field | Default | Unit | What it does |
|---|---|---|---|
| `seconds` | 10 | seconds | a pass takes this long; more than 0 |
| `power` | 1 | factor | a pass adds this times a signature's strength to what the pilot has resolved; at 1 it is found. More than 0 |

### law — the police's answer to a crime (#431 slices 9-10)

A system is in the high band from `highFrom`, the middle from `midFrom`, the low from
`lowFrom`; below that nobody comes. Checks: `callSeconds`, `stay` and `flagSeconds` more than
0; `lowFrom` at most `midFrom` at most `highFrom` at most 1; `gunsFrom` and `onSightFrom` at
most 1.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `highFrom` / `midFrom` / `lowFrom` | 0.8 / 0.5 / 0.1 | security | where each band begins |
| `highLocal` / `midLocal` / `lowLocal` | 99 / 4 / 2 | count | the system's own free police that warp to the crime, at most |
| `highLaunch` / `midLaunch` / `lowLaunch` | 4 / 2 / 0 | count | patrols a station of the law launches on top, at most |
| `highReach` / `midReach` / `lowReach` | 2 / 1 / 0 | jumps | police come from systems this close... |
| `highCalled` / `midCalled` / `lowCalled` | 8 / 3 / 0 | count | ...this many at most in all, lent like a siege's force |
| `highDelay` / `midDelay` / `lowDelay` | 0 / 20 / 45 | seconds | before any of it comes |
| `hop` | 10 | seconds | police from next door take this much longer a jump |
| `stay` | 180 | seconds | police from elsewhere stay this long, then go home |
| `launchCost` | 0.1 | share | a launched patrol not in the faction's presence is bought at this of an outpost's cost |
| `callSeconds` | 60 | seconds | one call holds this long: the next shot in that time brings nobody more |
| `dropDistance` | 1500 | units | police drop out of warp this far from the criminal |
| `gunsFrom` | 0.5 | security | a station's or a faction structure's guns fire on a criminal only from here up |
| `flagSeconds` | 900 | seconds | a crime flags the pilot this long, renewed by each crime, running down only while they play |
| `flagLinger` | 60 | seconds | a flagged pilot who drops the connection leaves the ship in space this long |
| `statusCrime` | 1 | status | security status lost for a crime... |
| `statusKill` | 2.5 | status | ...and this more for a kill |
| `statusPirateKill` | 0.1 | status | gained for each pirate shot down... |
| `statusPerDay` | 0.5 | status | ...and for each day in the world without a flag, back up to zero |
| `onSightBelow` | 5 | status | at minus this or below, police engage the pilot on sight... |
| `onSightFrom` | 0.5 | security | ...in systems of this security and above |

### influence — who holds a system (#431 slice 5)

| Field | Default | Unit | What it does |
|---|---|---|---|
| `outpost` | 1 | influence | what an outpost weighs toward its owner |
| `market` | 2 | influence | an outpost market |
| `station` | 4 | influence | a station: a faction's, or one grown from a market |
| `anchor` | 2 | influence | an Anchor |
| `perShip` | 0.25 | influence | each ship of a faction's presence there |
| `taxMax` | 0.1 | share | the most a corporation that holds a system may take of a trade at its markets; at most 1 |

### licences — building in held space (#431 slice 6, #509)

The rent follows the system: `share` of a period of its economy's output at base prices,
times `securityBase + securitySlope × security`, never below `rent` nor above `ceiling`.
Checks: `periodDays` more than 0; a `ceiling` other than 0 is at least `rent`.

| Field | Default | Shipped | Unit | What it does |
|---|---|---|---|---|
| `rent` | 2000 | 3000 | credits a period | the floor; with `share` 0 it is the whole rent |
| `share` | 0 | 0.01 | share | of the system's output in a period |
| `ceiling` | 0 | 30000 | credits a period | the most a rent is; 0, no ceiling |
| `securityBase` | 1 | 0.5 | factor | the rent's factor at security 0... |
| `securitySlope` | 0 | 1 | factor | ...plus this times the security: a safe harbour is worth more |
| `periodDays` | 7 | — | days | how often the rent is due |
| `graceHours` | 24 | — | hours | unpaid, a licence holds this much longer, then lapses into trespass |
| `standing` | 10 | — | standing | a faction gives leave from this standing (a corporation's is its members' sum) |
| `evict` | 1 | — | worth | what evicting a trespasser is worth to the holding faction, weighed against the risk |

### hangars — personal hangars at NPC stations (#472)

| Field | Default | Unit | What it does |
|---|---|---|---|
| `freeUnits` | 100 | units | free to every pilot at every station whose archetype keeps `storage` |
| `rentPerUnit` | 2 | credits a period | a unit rented beyond that, up to the archetype's `storage` |
| `periodDays` | 30 | days | how often the rent is due; more than 0 |

### security — what police cost a holder, and what security does (#431 slice 11)

A corporation sets a police budget in credits an hour for each system it holds; while it is
paid, the system's security target rises by `policeSwing × budget / (budget + policeHalf)`.
Checks: `policeSwing` and `pirateHide` at most 1; `policeHalf` and `shipHour` more than 0;
`payAtZero` and `oreAtZero` at least 1.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `policeSwing` | 0.3 | security | the most a budget can raise the target |
| `policeHalf` | 3000 | credits an hour | the budget that buys half of it |
| `shipHour` | 600 | credits an hour | the world's police fly one ship for each this much... |
| `shipsMax` | 8 | count | ...at most this many |
| `budgetMax` | 30000 | credits an hour | no budget above this is taken |
| `pirateHide` | 0.6 | share | of the pirates' strength that lies low at security 1, less below |
| `payAtZero` | 1.6 | factor | a mission's, job's or contract's premium at security 0 against security 1 |
| `oreAtZero` | 1.5 | factor | how much faster ore comes out of a belt at security 0 |

### patrols — a corporation's own AI patrols (#431 slice 12)

Checks: `regrowHours` more than 0; `systemMax` at most `shipsMax`.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `shipsMax` | 12 | count | ships of one corporation's patrols, in all |
| `systemMax` | 6 | count | ...and in one system |
| `shipHour` | 150 | credits an hour | upkeep of a ship of the default doctrine's `guard` design; a design that fights harder costs more |
| `regrowHours` | 0.5 | hours | a ship lost is made good this long after, while the upkeep is paid |

### charts — charts sold to factions (#346, #431 slice 16)

A faction pays for how much a pilot's chart changes its own picture of a system. All of it
is divided by `1 + rarity ×` the number of other factions with intel on the system.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `system` | 300 | credits | a system it has no intel on at all; nothing for a centre |
| `station` | 150 | credits | each station more or fewer than it believes stands there... |
| `structure` | 60 | credits | ...and each structure, when the pilot looked more recently than it did... |
| `halfLifeHours` | 24 | hours | ...halved for every this long since the pilot looked; more than 0 |
| `find` | 400 | credits | each hidden thing the pilot's scans resolved there that it was never told of |
| `rarity` | 0.25 | factor | how much each other faction that knows the system takes off |

### styles — what a region's NPCs fly, and how factions pay for it (#464)

A region's style is a share per weapon family and hull line, proportional to how much of each
one's goods its markets hold and make, with its people's culture as a prior. Checks: `culture`
and `defensiveBelow` at most 1; `dearAt` at least 1.

| Field | Default | Unit | What it does |
|---|---|---|---|
| `culture` | 0.25 | share | how much the culture weighs against the economy; 0 is the economy alone |
| `hours` | 24 | hours | a market's supply is its stock plus this much of its production |
| `dearAt` | 2 | factor | a good dearer than this times its base price is not bought |
| `holdingIncome` | 1500 | credits an hour | what each holding pays its holder at full prosperity, half at none |
| `defensiveBelow` | 0.4 | share | a faction whose holdings stand below this of what they keep plans no outposts, sieges or Anchors |
| `foundingHours` | 24 | hours | a new world's factions begin with this much of their income in the treasury |
| `cultures` | none | | culture id (a native theme, a major's `culture`) → weight by weapon family or hull line id; a weight is a number of 0 or more. The shipped file weighs seven cultures |

## Minimal System Example

```json
{
    "star": { "type": "Blue", "size": 500 },
    "stations": [
        { "name": "Depot", "pos": [3000, 0], "size": 80, "faction": "Syndicate", "role": "TradeHub" }
    ],
    "gates": [
        { "name": "Gate to Core", "pos": [12000, 0], "size": 150, "destination": "core" }
    ]
}
```

## Notes for the Editor

- Unknown enum strings fall back to the default value (a broken
  `type`/`role`/`faction` does not break loading). An unknown station `role` or planet
  `type` is logged by name, because the object it becomes is a different object.
- Broken JSON in a system → empty entity lists (silently). Check the game's output
  (TraceLog) when debugging.
- Object sizes scale with the system's scale; inter-object distances
  (docking/mining/weapons) are relative to `size`.
- The editor must keep both files consistent: system ids in `universe.json` ↔ gate
  `destination`s ↔ `links` entries.

## `look.json` — the screen treatment

`data/look.json` says what the world is put through after it is drawn (#120): an ordered
chain of full-screen passes, each with two knobs.

```json
{
    "enabled": true,
    "treatHud": false,
    "chain": [
        { "pass": "bloom", "enabled": true, "amount": 0.85, "scale": 2.2 },
        { "pass": "pixelate", "enabled": true, "amount": 1.0, "scale": 2.0 }
    ]
}
```

| Field | Type | Description |
|------|-----|----------|
| `enabled` | bool | the whole treatment; false draws the raw picture |
| `treatHud` | bool | whether the interface goes through the chain with the world |
| `paintBudget` | number | millions of pixels a painted body (#426) may cover before it is painted at reduced resolution and scaled up; 1 if absent; the shipped look.json sets 0.5 for integrated GPUs such as the UHD 620 (owner, 2026-10-23); never below 0.25. Raise it on a strong GPU for sharper close-ups |
| `chain` | array | the passes, **in the order they run** |

| Pass field | Type | Description |
|------|-----|----------|
| `pass` | string | `bloom`, `pixelate`, `scanlines`, `noise`, `fringe`, `vignette` |
| `enabled` | bool | skip this pass without losing where it sat in the order |
| `amount` | number | 0..1 — how much of the effect. Zero is off, one is the effect as designed |
| `scale` | number | the pass's own second knob; what it means is written at the top of its shader and shown in the settings screen |

**The order is a decision, not a detail.** Bloom before pixelation gives soft fat pixels;
after it gives hard pixel edges that glow. They are different games to look at, so the
order is data and is reorderable in game rather than being a sequence of calls.

**`treatHud` is a game decision, not a technical one.** Duskers can pixelate everything
because everything there *is* the terminal. Here the HUD carries numbers people fly by, and
a pixelated fuel gauge is a worse game — so it defaults to false.

The file is written by the game (F10 opens the settings, closing them saves), so unlike
`archetypes.json` it is machine-formatted and nobody hand-edits it.

### `data/shaders/`

One fragment shader per pass, named after it. Each is handed `amount`, `scale`,
`resolution` and `time`, and uses the ones it has a use for; `bloom.fs` additionally gets
`scene`, the picture before any pass, because it is the only one that composites against
what came before it.

**A missing or uncompilable shader is not an error.** Whether a shader compiles is a
property of the player's driver, not of the build. The pass is dropped, the reason is
logged and shown in the settings screen, and the game keeps playing. If none load, the
world is drawn straight to the screen. The server and the unit tests never load one.
