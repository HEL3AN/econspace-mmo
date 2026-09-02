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
