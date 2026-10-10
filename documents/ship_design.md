# EconSpace — Ship Designs (`data/ships.json`)

A ship is a **design**, and what it can do is read off its parts (#279, M8 groundwork). This
file holds the parts' function and the designs built from them; `Ships::Derive`
(`src/engine/core/ShipDesign.h`) turns a design into stats, cost and build time.

**Status — step 4 of #279.** A design decides what its ship looks like (each section is
drawn, a frame lays them end to end and holds the hull to its class's silhouette) and, for
NPCs, what it can do: a faction's **doctrine** names the designs each NPC role flies, and an
NPC's speed, hull and guns are its design's. `ship.npc`, the one shape every role used to
share, is gone. The player's ships still use the hand-typed `GetShipCatalog()`
(`src/game/entities/ShipType.cpp`), and `tests/ship_design_tests.cpp` holds the four player
designs within 10% of it until that switch.

## The model

```
design = frame + { bow, mid × N, stern } sections + fit (modules on sockets, pinned counts)
```

- A **frame** is a hull class — Proposal A's interceptor, frigate, hauler, barge, raider,
  cruiser. It is the spine the sections hang on, says how many mid sections it takes, and
  holds every design on it to the class's **silhouette rule** (below).
- A **section** stands at one position — `bow`, `mid` or `stern` — exposes **sockets** (a
  kind and how many modules of it the section takes) and is **drawn** as a piece of hull.
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
               "buildSecondsPerMass": 4, "buildSecondsPerPart": 6, "plain": 0.5 },
    "modules": [
        { "id": "hull.engine", "mass": 5, "provides": { "thrust": 8400 }, "cost": { "Iron": 10, "Crystal": 4 },
          "look": { "stern": { "mount": "out", "turn": 180, "scale": 0.14, "z": -1 } } }
    ],
    "sections": [
        { "id": "stern.twin", "position": "stern", "mass": 4, "cost": { "Iron": 16 },
          "sockets": { "stern": 2, "side": 2, "edge": 2 },
          "shape": { "sections": [
              {"form": "bar", "length": 0.3, "width": 0.34, "role": "hull", "pitch": 0.12},
              {"form": "bar", "at": [-0.03, 0.2], "length": 0.36, "width": 0.15, "role": "hull", "mirror": true} ] } }
    ],
    "frames": [
        { "id": "interceptor", "class": "interceptor", "mass": 8, "mids": [1, 1], "minAspect": 1.6,
          "provides": { "mining": 2 }, "cost": { "Iron": 30 } }
    ],
    "designs": [
        {
            "id": "courier", "name": "Courier", "frame": "interceptor",
            "bow": "bow.needle", "mid": ["mid.slim"], "stern": "stern.twin",
            "fit": [ { "module": "hull.engine", "in": "stern", "on": "stern", "count": 2, "scale": 0.14 } ]
        }
    ]
}
```

| Field | Where | Description |
|------|------|----------|
| `mass` | module, section, frame | required, more than 0 |
| `provides` | module, section, frame | optional; `thrust`, `rcs`, `cargo`, `mining`, `hull`, `damage` — each summed over the ship |
| `cost` | module, section, frame | required; resource → positive whole amount, as a blueprint's (#39) |
| `position` | section | `bow`, `mid` or `stern` |
| `sockets` | section | socket kind → capacity; kinds: `top`, `edge`, `bow`, `stern`, `front`, `side`, `spine`, `bottom` — the kit's own words. A ship's end is a `bow` at the bow and a `stern` at the stern and nowhere else, so there is one stern |
| `shape` | section | required; what it is drawn as, +x forward — see "Drawing a design" |
| `look` | module | optional; socket kind → how it meets that socket when drawn: `mount`, `turn`, `scale`, `z`, as on a kit line |
| `class` | frame | the hull class it is (Proposal A) |
| `mids` | frame | `[least, most]` mid sections |
| `minAspect` | frame | required; the hull is at least this long for its width |
| `bowHeavy` | frame | optional, default false; the class may carry its mass forward of the middle |
| `plain` | rules | the share of a drawn ship's sockets its trim leaves empty |
| `cruise` | rules | the share of its top speed an NPC flies its rounds at |
| `fit[].scale` | design | optional; how big the module is drawn, over its `look` |
| `frame`, `bow`, `mid`, `stern` | design | the frame and one section per position; `mid` is a list |
| `fit[].module` | design | a `modules` id |
| `fit[].in` / `on` | design | the position whose sections carry it, and the socket kind |
| `fit[].count` | design | a whole number ≥ 1 — never a range |

`provides` takes only what a stat is derived from. `hull` and `damage` arrived with the NPCs
that read them (step 4); shields and sensors arrive with the stat that reads them -- accepted
before that, they would be numbers nothing uses.

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
| structure | Σ hull | `NpcShip` hull |
| volley | Σ damage — what one volley takes off a ship | `NpcShip` damage |
| cruise | top speed × `cruise` | `NpcShip` speed |
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
  ships are bilateral;
- **the hull keeps its class's silhouette** — laid out, it is at least `minAspect` times as
  long as it is wide (no capsule or oval as a main body; 1.6 or more for every class but the
  barge), and the middle of its area is aft of the middle of its length — the drives are the
  heaviest thing on a ship — unless the frame is `bowHeavy`, which only the barge is.

And a section's drawing is held to what a ship is: its hull parts are on the axis and square
to it, or `mirror`ed (bilateral); never `repeat`ed (a repeat turns about the centre and puts
the second wing in front of the nose); and fixed — no module, range, palette or chance — so
the class rule holds for every ship of a design. What the seed varies is the trim on the hull.

## Drawing a design

A section's `shape` is `{ "sections": [...], "kit": [...], "parts": [...] }` in the grammar of
`world_format.md`, in the section's own frame with +x forward: `sections` its hull, `kit` the
trim that goes on it (a kit line's `in` counts this section's own hull parts; absent, any of
them), `parts` anything else — lights, panel lines. `Ships::ShapeOf` turns a design into the
shape an archetype carries:

1. the sections are laid end to end along x, stern to bow, each by how far its hull reaches,
   and the whole is centred on the middle of its length;
2. every fit line becomes a kit line that carries function (`"fit": true`) with the design's
   count, on the hull parts of every section at its position (`"in": [..]`), looking as the
   module's `look` for that socket kind says, at the line's own `scale` if it gives one;
3. the sections' trim follows, after the function, and the kit is bilateral.

So the count on the hull is the count in the design, and a test holds every drawn design to
placing every fit line in full at every seed. A wing is a **handed** module: one of a pair,
placed as the +y one as drawn and the other as its reflection (`world_format.md`).

An archetype draws a design by naming it — `"design": "courier"` instead of a `shape`.

## Doctrines: what NPCs fly

```json
"doctrines": {
    "default":   { "trader": ["hauler"], "miner": ["miner"], "patrol": ["cutter"],
                   "pirate": ["raider"], "warship": ["cruiser"] },
    "Syndicate": { "patrol": ["cruiser", "cutter"] }
}
```

A doctrine is a faction's (by its id in `factions.json`) or the `default`, and maps an NPC role
(`trader`, `miner`, `patrol`, `pirate`, `warship` -- the game's `NpcRole`) to the designs it
flies. A faction's own doctrine wins where it names a role; the default names one for every
role, so no NPC goes without a ship. The server picks one of a role's designs once, when the
ship is made, so a role with several flies all of them and a ship keeps its design for life.
A system warming from its aggregate (#295) picks **by the ship's place in its role** -- the
n-th patrol flies the n-th pick -- so the same system looks the same every time it warms,
whatever warmed before it; a ship sent in later picks by its id. The pick travels in the
snapshot (`design`, `PROTO_VERSION` 18), so the client draws the ship the server simulates
instead of guessing it from the role.

An NPC takes from its design what it can do -- its speed is the design's top speed times
`rules.cruise` (a ship on its rounds is not racing), its hull is the design's `hull`, a volley
of its guns takes off the design's `damage` (a player is hit a sixth harder, as before) -- and
it is drawn as the archetype that names the design, at that archetype's size. A design a
client does not know falls back to its doctrine's first pick for the role, never to a shape
that stands for nothing.

Load errors: a faction that does not exist, a role that is not one, a design that is not in
the catalogue, a default that leaves a role out, an armed role (`patrol`, `pirate`,
`warship`) flown by a design with no guns -- a patrol that cannot shoot is a trader in a
uniform -- and a doctrine design that no `Npc` archetype draws.

**Before and after.** Every NPC used to have a hull of 60, a volley of 6 (7 against a
player) and a speed rolled between 120 and 180. A test holds every design a doctrine flies
within 15% of that:

| Role | Design (class) | Speed | Hull | Volley |
|------|------|------|------|------|
| before, every role | `ship.npc` | 120-180, mean 150 | 60 | 6 |
| trader | hauler (hauler) | 142 | 60 | -- |
| miner | miner (barge) | 162 | 60 | -- |
| patrol | cutter (frigate) | 161 | 60 | 6 |
| patrol, Syndicate | cruiser (cruiser) / cutter | 151 / 161 | 66 / 60 | 6 |
| pirate | raider (raider) | 158 | 60 | 6 |
| warship | cruiser (cruiser) | 151 | 66 | 6 |

Fire rate (0.9 s) and range (230) stay the server's, the same for every NPC: the parts do
not say anything about them yet.

## How it maps to the hull classes (Proposal A)

| Ship | Frame (class, min. aspect) | Bow | Mid | Stern | What makes it its class |
|------|------|------|------|------|------|
| Scout | frigate, 2.0 | `bow.blunt` | `mid.spine` | `stern.single` | a pointed nose, a straight spine, a wider drive block with a thruster each side; one pod; a tractor |
| Courier | interceptor, 1.6 | `bow.needle` | `mid.slim` | `stern.twin` | a dart: a long needle widening aft, swept wings on the rear third, two drives on nacelles |
| Hauler | hauler, 3.0 | `bow.cab` | `mid.keel` × 2 | `stern.cluster` | a small cab ahead of a long thin keel with four pods along it; no wings |
| Miner | barge, 1.2, bow-heavy | `bow.hammerhead` | `mid.hopper` | `stern.twin` | the one bow-heavy class: a hammerhead with two arms reaching forward, a tractor at each tip; a hopper amidships |
| Cutter | frigate, 2.0 | `bow.blunt` | `mid.spine` × 2 | `stern.single` | the scout's frigate stretched by a second spine, a gun on each, armour plates on the bow |
| Raider | raider, 1.6 | `bow.fang` | `mid.slim` | `stern.single` | a split bow -- two prongs either side of a short nose with a railgun between them -- swept wings, armour plates on the bow, one drive with a thruster each side |
| Cruiser | cruiser, 2.0 | `bow.wedge` | `mid.belt` | `stern.row` | a broad wedge widest aft, a railgun on the bow, the bridge amidships, an armour belt along a wide mid, three nozzles on a split stern |

Every frame provides `mining: 2` — the cutting beam every hull carries today — and the
barge's bow and tractors add to it. A frame provides the structure of its class (`hull`), and
armour plates add to it. The raider's asymmetry budget from Proposal A (one bolted-on part on
one side) is not in the grammar yet: a kit is bilateral, and a raider reads as a raider by its
split bow.

## Next steps

1. **Grammar** -- done (#316). A kit line can go on a `bow`, `stern`, `front`, `side`, `spine` or
   `bottom`, which name places a long section already has (`world_format.md`, "A ship's
   socket kinds"), and a line that carries function says `"fit": true`: one whole count, a
   module rather than a tag, placed before the trim and past `plain`. A test holds every ship
   archetype to wearing what `provides` something only on such lines.
2. **Frames per class** -- done. A section here has the shape it is drawn with, a design
   resolves to the shape JSON the renderer reads (`Ships::ShapeOf`), and the frame enforces
   its silhouette rule as a load error. A section's sockets say `bow`/`stern` where they said
   `end`, the grammar's words. The gallery card shows the stats.
3. **NPC roles → designs** -- done. A faction doctrine picks a design per `NpcRole`, an
   NPC's speed, hull and guns are its design's, the snapshot carries the design, and
   `ship.npc` is retired (not kept as a fallback: an unknown design falls back to the
   doctrine's pick). Raider and cruiser frames, with their sections.
4. **Switch `ShipStats` to the derived stats**: `GetShipCatalog()` reads designs.
