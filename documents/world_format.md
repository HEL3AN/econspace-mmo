# EconSpace — World Data Format

Specification for the game world's JSON data. This is the contract between the game
and the future **visual world editor**: the editor reads and writes exactly these
files.

Parsing — `src/engine/core/WorldLoader.cpp`. Entities — `src/engine/entities/`.

This is the reference. To add something step by step — an archetype, its shape, a material,
a screen pass — start with the worked how-tos in [docs/howto/](../docs/howto/README.md).

## File Layout

```
data/
  universe.json        galaxy index: systems, links, starting system
  archetypes.json      what each kind of object is and can do
  systems/
    <id>.json          one star system (objects)
  textures/            sprites (PNG), optional
```

After a build, `data/` is copied next to `econspace.exe` by the `copy_data` target
(on every build). The server loads `universe.json`, then the systems. Clients and
`econagent` never read it: the server sends them the index at login as a `universe`
message (#206), without the `file` names, so a system that exists only on the server is
on their map too.

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

---

## universe.json — galaxy index

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
| `scale` | the module's radius as a fraction of the object's (default 0.1) |

`at`, `angle`, `repeat`, `mirror` and `row` place the module as a whole. A module's own
parts may use `row` but not `repeat`, `mirror`, orbits or the sphere (those place things
about the object's centre). A module is drawn once it is about 10 px across on screen
(its parts' own `minPixels` count in the module's pixels), so it fills in as you approach.
`tags` and `sockets` are for the automatic placement of phase 3 of #240.

**On a planet.** A module part with `lat`/`lon` is laid on the sphere (#166) instead of the
disc: a crater, a base, a city. Its own frame is a small patch of the surface at that
latitude and longitude, measured in the body's radius, and every part of it becomes a surface
part -- carried round by `spin`, foreshortened at the limb, hidden on the far side. `repeat`
spreads copies round the planet in longitude and `mirror` reflects across the equator. Only a
`disc` is foreshortened at the limb today, so surface modules are written in discs.

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
along its axis and an `end` at each end; a disc or polygon has an `edge` rim on its outline and
an inner `top` ring; a ring or arc has a `ring` rim; a chevron's tail is an `end`. Sockets face
outward.

Each kit line names a module (`"of": "id"`, or `"#tag"` for any module carrying the tag), a
`count` (a range: every whole number in it equally likely), the socket type `on` (default: the
module's first `sockets` entry), and optionally `in` (only on that section, by index), `scale`
(module radius, default from the pitch), `turn` (degrees added to the outward direction),
`mount` (how the module meets the socket, measured by the module's own box rather than its
origin: `"on"`, the default, lies wholly on the hull with its edge at the hull's; `"out"` starts
at the edge and stands out from it -- docks, engines, dishes on booms; `"centre"` is centred on
the socket; a `top` socket always centres), `z` (draw order, default 1: over the hull), `prefer` (`"out"`, the default, or `"in"`: which of
two otherwise equal lines -- the two long edges of an arm, its two ends -- is taken first, the one
facing away from the object's centre or the one facing it), `when` (a component the archetype must have for the
line to apply -- `"when": "defensive"` puts turrets only on stations that can fight, `"!market"`
only on those without a market; an unknown component name is a load error), and
`variant` (pinned; otherwise the seed picks one for the whole line, so a row is a row of the
same thing).

**The seed decides what and how many; rules decide where.** A kit line goes on the longest free
line of sockets of its kind; on a straight line its modules are spread evenly and symmetric
about the middle, on a closed rim they spread from the socket that faces furthest out from the
object (a pod's dock is on its far side, not facing the hub); a placed module covers the
sockets under it, so nothing is put on top of it; each section keeps `plain` (default 0.4) of its sockets empty; with
`"symmetry": "bilateral"` every module on one side is mirrored onto the other, its shape
reflected, and with `"radial"` a placement on one copy of a repeated section is repeated on
every copy (the count is then per copy). An unknown module or a tag nobody carries is a load
error. A module placed on a socket shares its section's `spin`, so it turns with it.

## pins.json — hand-written exceptions to the generated region (#147)

The generated region is the default, not a monopoly. A starting point, a set piece, or
anything a mission depends on is a **pin**. **Order: generate, then apply pins, always.**
A pin that cannot apply says so in the server log, by number and system, rather than
quietly losing to the generator.

```json
{ "pins": [
    { "system": "w1-1", "document": { "derelicts": [ { "name": "The First Expedition",
                                                       "pos": [500000, 500000], "size": 60, "reward": 1500 } ] } },
    { "system": "w2-1", "seed": 7, "mode": "replace", "document": { "star": { "type": "Blue", "size": 120000 } } }
] }
```

| Field | Meaning |
|------|----------|
| `system` | The generated system's id (`w3-2`). Required. |
| `seed` | Apply only in the region this seed makes. If absent, the pin applies for every seed. |
| `mode` | `merge` (the default) or `replace`; see below. |
| `document` | System JSON, in the same format as `systems/<id>.json`. |
| `note` | Free text for whoever reads the file. |

**`merge`** keeps the generated system and adds to it:
- A list (`planets`, `stations`, `asteroidFields`, `nebulae`, `derelicts`, `gates`) appends its objects.
- An object with `"replaces": "<name>"` takes the place of the generated object of that name. With `"remove": true` as well, that object is removed instead.
- Any other key (`star`, `stars`, `character`) is set to the pin's value.

**`replace`** makes the document the whole system. Its gates are still kept from the generated system unless the document lists its own, because the gates are the region's topology.

Any other field is an error, as everywhere else (#191). Pins are data, like the systems: a
pin added later changes the region the next time the server starts.

## factions.json — who is in the galaxy, and how they act

```json
{ "id": "Pirates", "name": "Pirates", "color": [255, 161, 0], "lawful": false, "kind": "Pirate",
  "temperament": { "appetite": 1.4, "capacity": 6, "growth": 0.08,
                   "values": { "traffic": 1.0, "salvage": 0.6, "unclaimed": 0.4 } } }
```

`relations` gives each pair a stance (`War`, `Hostile`, `Neutral`, `Friendly`, `Ally`), and
`repTiers` the reputation thresholds.

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
| `material` | string | **optional** — the material that shades it, by id from `data/materials.json`. Omit it and the object is drawn plain. An id `materials.json` does not define also draws plain, and the client and editor log it by name at startup |
| `light` | object | **optional** — this object lights the system: `radius` (world units at which its light has fallen to nothing) and `intensity` (default 1.0). Omit it and the object emits nothing. See below |
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
"light": { "radius": 2480000, "intensity": 1.00 }
```

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

Stars, nebulae and belts deliberately have no shape: a star is a light rather than a
structure, and a region is not a surface. They keep the figure the backend draws for them.

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
| `light.tint` | vec4 | the colour of the light arriving |
| `light.strength` | float | 0..1 |
| `light.ambient` | float | the floor |
| `clock.time` | float | seconds |

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
| `storage` | `capacity` | *reserved for #44*: holds cargo that is not aboard a ship |
| `jumpLink` | `range` | connects this system to another |
| `hazard` | `radius`, `hidesShips` | changes conditions inside it; `radius` 0 means the object's own radius |
| `salvageable` | `range` | pays out once to whoever reaches it first |
| `buildable` | `cost`, `buildSeconds` | *reserved for #44*: a player can construct one |

Every `range` is added to the object's own radius, and every verb that reaches for
something reads it from the object being reached rather than from a constant beside the
rule. That is what lets a player-built dock, belt or gate work at its own distance without
the docking, mining or jump pass being edited (#44).

Hostile, for `defensive`, means what it means to an NPC of the station's faction: a player
by their own account (wanted by it, or at a hostile reputation), an NPC by the relation
matrix. A docked player or one hidden in cover is not shot at. Only a station fires today,
because who a battery shoots for is a question of ownership, and ownership is still a
station's own field (#41).

`storage` and `buildable` are **reserved**: they are parsed and checked, so data written
with them now stays valid, but no pass reads them yet. Holding cargo off a ship and building
an object are the player-mutable world (#44), and they arrive with it. Declaring either on
an archetype today changes nothing in game.

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

A system beyond the wormhole is not a file: the server generates it from the world's seed
(#140) in exactly this format, so everything below describes both.

```json
{
    "star":  { "type": "Yellow", "size": 150000 },
    "planets":        [ … ],
    "stations":       [ … ],
    "asteroidFields": [ … ],
    "nebulae":        [ … ],
    "derelicts":      [ … ],
    "gates":          [ … ]
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
| `archetype` | string | Which archetype **of its kind** this object is, in place of its kind's ordinary one (#142): a derelict that is `derelict.leviathan`, a belt that is `field.motherlode`. A name that is not an archetype, or is one of another kind, is logged by name and ignored -- the object is built as usual. |

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

### stations[]
| Field | Type | Values |
|------|-----|----------|
| `name` | string | name |
| `pos` | [x,y] | position |
| `size` | number | radius |
| `faction` | string | `Independent` (default), `TradersGuild`, `Syndicate` |
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

---

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
