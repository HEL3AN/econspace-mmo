# EconSpace — World Data Format

Specification for the game world's JSON data. This is the contract between the game
and the future **visual world editor**: the editor reads and writes exactly these
files.

Parsing — `src/core/WorldLoader.cpp`. Entities — `src/entities/`.

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
(on every build). The game loads `universe.json`, then the starting system.

## Coordinate System and Scale

- Units are game "units". The origin `(0,0)` is the center of the system (the star).
- Constants — `src/core/World.h`:
  - `SYSTEM_RADIUS = 25000` — soft boundary: the ship is not let out beyond it.
  - `CORE_RADIUS = 9000`, `MID_RADIUS = 18000` — ring thresholds (see below).
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
| `material` | string | **optional** — the material that shades it, by id from `data/materials.json`. Omit it and the object is drawn plain |
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
"light": { "radius": 62000, "intensity": 1.00 }
```

A reach well past `SYSTEM_RADIUS` (25 000) is normal: the falloff is quadratic, so a star
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
| `form` | string | `disc`, `ring`, `polygon`, `capsule`, `chevron`, `bar`, `lattice` |
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
| `mineable` | `extractRate`, `range` | holds a deposit; `extractRate` is units per second at skill 1 |
| `market` | — | buys and sells resources |
| `defensive` | `range`, `damage` | fires on hostiles; `damage` is per second |
| `storage` | `capacity` | holds cargo that is not aboard a ship |
| `jumpLink` | `range` | connects this system to another |
| `hazard` | `radius`, `hidesShips` | changes conditions inside it; `radius` 0 means the object's own radius |
| `salvageable` | `range` | pays out once to whoever reaches it first |
| `buildable` | `cost`, `buildSeconds` | a player can construct one |

Every `range` is added to the object's own radius, and every verb that reaches for
something reads it from the object being reached rather than from a constant beside the
rule. That is what lets a player-built dock, belt or gate work at its own distance without
the docking, mining or jump pass being edited (#44).

Components carrying no parameters are still written as `{}` — presence is what matters.

Some values deliberately live on the *instance* rather than on the archetype, because
they vary between two objects of the same kind: where a gate leads, what a wreck pays
out, how much ore a belt still holds.

**A malformed registry is a hard failure, not a degraded load.** An unknown `kind`, an
unknown component name, a duplicate `id` or a missing `id` aborts the load and leaves
the previous registry in place. A typo would otherwise produce an object that silently
does nothing.

---

## systems/<id>.json — star system

All sections except `star` are optional (an entire array may be omitted). `pos` is
`[x, y]` in units. `color` is `[r, g, b]` (0–255).

```json
{
    "star":  { "type": "Yellow", "size": 600 },
    "planets":        [ … ],
    "stations":       [ … ],
    "asteroidFields": [ … ],
    "nebulae":        [ … ],
    "derelicts":      [ … ],
    "gates":          [ … ]
}
```

### star (required)
| Field | Type | Values |
|------|-----|----------|
| `type` | string | `Yellow` (default), `Red`, `Blue` |
| `size` | number | radius |

The star's position is always `(0,0)`.

### planets[]
| Field | Type | Values / description |
|------|-----|---------------------|
| `orbitRadius` | number | orbit radius |
| `orbitSpeed` | number | linear orbital speed |
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

> Belts farther than `MID_RADIUS` (18000) from the center — with pirates.

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

- Unknown enum strings safely fall back to the default value (a broken
  `type`/`role`/`faction` does not break loading).
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
