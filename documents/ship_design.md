# EconSpace — Ship Designs (`data/ships.json`)

A ship is a **design**, and what it can do is read off its parts (#279, M8 groundwork). This
file holds the parts' function and the designs built from them; `Ships::Derive`
(`src/engine/core/ShipDesign.h`) turns a design into stats, cost and build time.

**Status — first step.** Nothing in the game reads these numbers yet. `GetShipCatalog()`
(`src/game/entities/ShipType.cpp`) still holds the hand-typed stats of Scout, Courier,
Hauler and Miner; `tests/ship_design_tests.cpp` holds the four designs here within 10% of
them, so the switch (step 4 below) changes no ship by more than that. The designs do not
yet decide what a ship *looks* like either: that is the grammar step.

## The model

```
design = frame + { bow, mid × N, stern } sections + fit (modules on sockets, pinned counts)
```

- A **frame** is a hull class — Proposal A's interceptor, frigate, hauler, barge, raider,
  cruiser. It is the spine the sections hang on and says how many mid sections it takes.
- A **section** stands at one position — `bow`, `mid` or `stern` — and exposes **sockets**:
  a kind and how many modules of it the section takes.
- The **fit** puts modules on those sockets, each line with a whole **count**. A count that
  carries function is pinned in the design, never drawn from the seed.

### Why function lives here and not on the module library

`data/modules*.json` is the *look*: a module in several **variants**, and which variant an
object wears is its seed's choice (#139); a kit line's count may be the seed's too. Neither
may change what a ship can do — two Haulers of one design carry the same hold whichever
hatches they wear. So function is written **once per module, never per variant**, in this
file, keyed by the module's id. The render library and this file name the same modules (a
test checks every id and socket against it), and stats are computable on a server that
draws nothing.

## Format

```json
{
    "rules": { "speedBase": 30, "speedPerAccel": 1.2, "rcsPerTurn": 183,
               "buildSecondsPerMass": 4, "buildSecondsPerPart": 6 },
    "modules": [
        { "id": "hull.engine", "mass": 5, "provides": { "thrust": 8400 }, "cost": { "Iron": 10, "Crystal": 4 } }
    ],
    "sections": [
        { "id": "stern.twin", "position": "stern", "mass": 4, "cost": { "Iron": 16 },
          "sockets": { "end": 2, "side": 2, "edge": 2 } }
    ],
    "frames": [
        { "id": "interceptor", "class": "interceptor", "mass": 8, "mids": [1, 1],
          "provides": { "mining": 2 }, "cost": { "Iron": 30 } }
    ],
    "designs": [
        {
            "id": "courier", "name": "Courier", "frame": "interceptor",
            "bow": "bow.needle", "mid": ["mid.slim"], "stern": "stern.twin",
            "fit": [ { "module": "hull.engine", "in": "stern", "on": "end", "count": 2 } ]
        }
    ]
}
```

| Field | Where | Description |
|------|------|----------|
| `mass` | module, section, frame | required, more than 0 |
| `provides` | module, section, frame | optional; `thrust`, `rcs`, `cargo`, `mining` — each summed over the ship |
| `cost` | module, section, frame | required; resource → positive whole amount, as a blueprint's (#39) |
| `position` | section | `bow`, `mid` or `stern` |
| `sockets` | section | socket kind → capacity; kinds: `top`, `edge`, `end`, `front`, `side`, `spine`, `bottom` |
| `class` | frame | the hull class it is (Proposal A) |
| `mids` | frame | `[least, most]` mid sections |
| `frame`, `bow`, `mid`, `stern` | design | the frame and one section per position; `mid` is a list |
| `fit[].module` | design | a `modules` id |
| `fit[].in` / `on` | design | the position whose sections carry it, and the socket kind |
| `fit[].count` | design | a whole number ≥ 1 — never a range |

`provides` takes only what a stat is derived from. Shields, hull points and sensors arrive
with the stat that reads them; accepted before that, they would be numbers nothing uses.

## Derivation

All sums run over the frame, every section and every fitted module × its count.

| Stat | Rule | Game field (`ShipStats`) |
|------|------|----------|
| mass | Σ mass | — |
| acceleration | Σ thrust / mass | `thrustPower` |
| top speed | `speedBase + speedPerAccel × acceleration` | `maxSpeed` |
| stabiliser | Σ rcs / mass | `rcsAccel` |
| turn rate | stabiliser / `rcsPerTurn` (rad/s) | `turnSpeed` |
| hold | Σ cargo | `cargoCapacity` |
| mining | Σ mining | `miningRate` |
| cost | Σ cost per resource | — |
| build time | `buildSecondsPerMass × mass + buildSecondsPerPart × parts` | — |

The constants are in `rules` so they are tuned with the parts. The hangar price in credits
stays the catalog's; what a station charges for a design is the market's question, not
the part list's.

## Load errors

As everywhere in content (#191), a mistake is an error that names it, never a default:
an unknown field anywhere (including inside `provides`, `rules` and a fit line), a missing
or non-positive mass, a missing or empty cost, a resource that does not exist, a socket
kind that does not exist, a part defined twice, and every design that does not validate.

A design **validates** when every name exists, each section stands at the position it is
put in, the frame's mid count holds, and:

- **every module is placed** — the lines sharing a position and a socket kind fit in the
  capacity of that kind summed over the sections at that position. A fit that runs out of
  sockets is an invalid design, never a quietly weaker ship;
- **drives sit in the stern** — a module that provides `thrust` is only fitted `in: stern`,
  so the engine mass is always astern (bow ≠ stern);
- **what is mirrored comes in pairs** — a count on a `side` or `edge` socket is even, since
  ships are bilateral.

## How it maps to the hull classes (Proposal A)

| Ship | Frame (class) | Bow | Mid | Stern | What makes it its class |
|------|------|------|------|------|------|
| Scout | frigate | `bow.blunt` | `mid.spine` | `stern.single` | one drive and a pair of side thrusters; one pod; a tractor |
| Courier | interceptor | `bow.needle` | `mid.slim` | `stern.twin` | the lightest hull on two drives; one pod |
| Hauler | hauler | `bow.cab` | `mid.keel` × 2 | `stern.cluster` | a long keel with four pods along it; no wings |
| Miner | barge | `bow.hammerhead` | `mid.hopper` | `stern.twin` | the one bow-heavy class: the hammerhead carries the cutters, two tractors on it |

Every frame provides `mining: 2` — the cutting beam every hull carries today — and the
barge's bow and tractors add to it. Raider and cruiser have no frame yet: nothing in the
catalog flies one.

## Next steps

1. **Grammar** (render, its own PR): `bow`/`stern` sockets in the kit instead of a bare
   `end`; sections offering the `front`, `side`, `spine` and `bottom` sockets the hull
   modules already declare; kit lines with fixed counts for what carries function.
2. **Frames per class** in the shape data: a section here gets the shape it is drawn with,
   so a design resolves to the shape JSON the renderer already reads, and the frame enforces
   its silhouette rule (length:width, where the widest point falls) as a load error.
3. **NPC roles → designs**: a faction doctrine picks a design per `NpcRole`; `ship.npc` is
   retired.
4. **Switch `ShipStats` to the derived stats**: `GetShipCatalog()` reads designs, and the
   gallery card shows the numbers next to the ship so look and stats are tuned together.
