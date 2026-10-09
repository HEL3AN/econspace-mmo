# Add an archetype

An **archetype** says what a kind of object *is*: its name, how it looks, and what it can do.
It is one entry in `data/archetypes.json`, read once at startup into a registry every executable
shares. A system file then only has to say *where* an object is and which particular one it is
([#34](https://github.com/HEL3AN/econspace-mmo/issues/34)).

This page adds a small **relay station**: somewhere a ship can dock and trade, smaller than a
trade hub. By the end it is a card in the gallery. [Compose its look](compose-a-look.md) gives
it a shape.

- [Write the entry](#write-the-entry)
- [See it](#see-it)
- [What each field is for](#what-each-field-is-for)
- [Components: what it can do](#components-what-it-can-do)
- [Where it shows up](#where-it-shows-up)
- [Making the game build one](#making-the-game-build-one)
- [The registry loads first](#the-registry-loads-first)

---

## Write the entry

Add this to the `archetypes` array in `data/archetypes.json`. Cards appear in file order, so
next to the other stations is the natural place:

```json
{
    "id": "station.relay",
    "name": "Relay Station",
    "kind": "Station",
    "glyph": "#",
    "color": [170, 190, 200, 255],
    "layer": 4,
    "size": 400,
    "components": {
        "dockable": { "range": 250 },
        "market": {}
    }
}
```

Mind the comma between it and its neighbours — a JSON syntax error refuses the whole file.

## See it

```sh
./build/bin/editor/worldeditor.exe gallery shapes
```

Find the card titled **Relay Station**. Under the name it says `station.relay   r 400` and
`dockable, market` — the components, read from the registry rather than from a description
somebody has to keep true. An archetype with no components says `scenery`.

The picture is a plain outline, because the entry has no `shape` yet: with none, the backend
draws the figure it has compiled in for the object's `kind`. That is the next page.

If there is no card at all, look at the terminal. `Archetypes: ...` names the problem, and
while there is one the gallery has **no** cards, because a broken registry is refused whole
rather than loaded half.

---

## What each field is for

| Field | Required | What it is for |
|---|---|---|
| `id` | yes | How world data and code refer to this kind. Unique; a duplicate refuses the file. By convention `kind.name`. |
| `name` | | What a player is shown. Defaults to the `id`. |
| `kind` | yes | Which of `Star`, `Planet`, `Station`, `Field`, `Gate`, `Nebula`, `Derelict`, `Npc`, `PlayerShip` it is. Anything else refuses the file. |
| `glyph` | | One character for the sensor view. Defaults to `?`; the tests require one. |
| `style` | | How the glyph occupies space: `point` (default), `region` for areas, `directional` for things that turn to face their heading. Anything else refuses the file. |
| `color` | | `[r, g, b, a]`, 0..255, `a` defaults to 255. The object's colour, which every part of its shape is coloured relative to. See the note below. |
| `layer` | | Draw order, lowest first. The shipped values: stars and nebulae 0, planets 1, fields 2, gates and wrecks 3, stations 4, NPC ships 5, the player 6. |
| `size` | | Radius in world units, used when an instance does not give its own: by the editor when you place one, and by the gallery. Missing means 100 there. |
| `shape` | | What it is made of. Next page: [Compose its look](compose-a-look.md). |
| `material` | | Which material shades it, by id. [Give it a material](give-it-a-material.md). |
| `light` | | `{ "radius": ..., "intensity": ... }` makes the object a light that shines on everything else in its system ([#119](https://github.com/HEL3AN/econspace-mmo/issues/119)). `intensity` defaults to 1. |
| `sprite` | | A texture name in `data/textures/`. None ship today, and a `shape` takes precedence over it in the shape backend. |
| `world` | | Where this kind lives in a system file. See [Making the game build one](#making-the-game-build-one). |
| `components` | | What it can do. Next section. |

**A misspelled field is ignored, not reported.** `"matrial": "hull"` loads without a word and
the object is simply unshaded. If an edit seems to do nothing, check the spelling first.

**Colour in the game is the instance's, not the archetype's — today.** The gallery and the
editor's previews draw the archetype's `color`. In the game, an object is drawn in the colour
its C++ constructor gives it: a station in its faction's paint, a planet in its type's colour,
every other kind in a constant. Faction paint is going away, because allegiance depends on who is
looking and belongs to the instruments, not the object
([#117](https://github.com/HEL3AN/econspace-mmo/issues/117)); until then, tune colour in the
gallery knowing the game may not show it yet.

**A light is never shaded by a light.** Give the relay a `light` and it will light its
neighbours — and every part of it is drawn emissive, at full colour, with no material. That is
what stops a star looking like a moon; it also means a beacon that shines is a beacon whose own
hull is flat.

---

## Components: what it can do

A simulation pass asks for a **component**, not a class: "everything dockable within range",
not "every `Station`". A new kind of object joins a pass by declaring the component, and the pass
is not edited. That is what lets a player-built dock work without the docking code knowing it
exists ([#44](https://github.com/HEL3AN/econspace-mmo/issues/44)).

Every `range` is **added to the object's own radius**, and it is read from the object being
reached rather than from a constant beside the rule, so each kind works at its own distance.
A component with no parameters is still written as `{}` — presence is what matters. An unknown
component name refuses the file.

What each one does **today**:

| Component | Parameters | What reads it |
|---|---|---|
| `dockable` | `range` | docking: a ship within radius + `range` can dock |
| `market` | — | buying and selling while docked here |
| `mineable` | `range`, `extractRate` | mining reaches radius + `range`. `extractRate` is loaded but nothing reads it yet |
| `jumpLink` | `range` | the jump: a ship within radius + `range` can use it |
| `salvageable` | `range` | salvage: whoever reaches radius + `range` first is paid |
| `hazard` | `radius`, `hidesShips` | the server hides a player inside it from NPCs; `radius` 0 means the object's own |
| `defensive` | `range`, `damage` | loaded and listed, **not acted on yet** |
| `storage` | `capacity` | loaded and listed, **not acted on yet** |
| `buildable` | `cost`, `buildSeconds` | loaded and listed, **not acted on yet** — it is for the player-built world (#44) |

The test suite insists that every `dockable`, `mineable`, `jumpLink` and `salvageable` archetype
has a `range` above zero, and every `mineable` an `extractRate`: a capability with no reach is
one the pass compares against zero and never matches.

---

## Where it shows up

| Where | When |
|---|---|
| **The gallery** | always — every archetype in the registry is a card |
| **The editor's creation palette** | only with a `world` block; the palette is generated from the registry, so no editor change is needed ([#37](https://github.com/HEL3AN/econspace-mmo/issues/37)) |
| **The game** | only when something *builds* an entity of this archetype — see the next section |
| **An agent's `observe`** | as its kind and name (`station  Relay Hub  ...`). The details on the line come from the kind too: every station is listed as `dockable`, whatever its components say |

---

## Making the game build one

This is the honest part. The registry is data, but **which archetype an entity uses is still
chosen in C++**, from its kind and a subtype: a station's `role`, a planet's `type`, a star's
`type`. The generic path — a system file that names an archetype by id — is
[#34](https://github.com/HEL3AN/econspace-mmo/issues/34), and until it lands a genuinely new kind
needs a few lines of code.

**The data half** is a `world` block, which says which array of a system file the object is
written to and what that array calls its type:

```json
"world": { "category": "stations", "subType": "Relay" }
```

With only that, the relay is in the editor's palette — and **placing one is a trap**: the editor
writes `"role": "Relay"`, the loader does not know that role, falls back to a trade hub, and the
object comes back as `station.trade_hub`. The tests do not catch it.

**The code half**, for a new station role (the compiler's `-Wswitch` warnings find the switches
for you):

1. `src/engine/entities/Station.h` — add `Relay` to `enum class StationRole`, **at the end**.
   The role crosses the wire as a number (`EntityLayout::subType`), and inserting one in the
   middle renumbers every role after it.
2. `src/engine/entities/Station.cpp` — the three mappings: `StationRoleName` (what a player is
   shown), `ArchetypeIdForStationRole` (`"station.relay"`) and `StationRoleFromString`
   (`"Relay"`, the spelling in the system file).
3. `src/game/missions/MissionSystem.cpp` — which missions a relay's job board leans toward.
4. `src/editor/Editor_Panel.cpp` — the `role` dropdown, a list of spellings.

A planet type is the same shape of change in `Planet.h`/`Planet.cpp` and the editor's `type`
dropdown.

What you do **not** have to touch is anything that draws, docks, trades or mines. Those read the
archetype.

---

## The registry loads first

Every entity looks its archetype up **in its constructor**. An entity built before
`Archetypes::Load` gets nothing — no look, no components — and fails silently: it is undockable
and invisible rather than broken. The client shipped exactly that way with the player's own
ship ([#127](https://github.com/HEL3AN/econspace-mmo/issues/127)). Every executable now loads
the registry before it builds a world; if you write a new tool or test, do the same, and watch
the log for `Entity: no archetype '...'`, which is printed once per missing id.

---

**Next:** [Compose its look](compose-a-look.md).
