# EconSpace — Built Surfaces (#516)

How stations, outposts, structures and ships get a finished surface — plating, armour,
radiators, solar cells, glass, livery, wear, lit windows, engine heat, scorch — at the level
the natural bodies reached in #501, without giving up a shape written in data.

**Status (2026-10-23): design + prototype, on hold for the owner's eye.** The prototype is one
station (`station.trade_hub`) and one ship design (`scout`: `ship.player`, `ship.scout`),
plus the modules they carry. Everything else still wears `hull` and is drawn as before. The
rollout is the slice plan at the end.

---

## 1. What changes, and what does not

The premises hold (CLAUDE.md):

- **Shape stays data** (#122). Nothing here draws a silhouette in a shader. A part is still a
  disc, ring, polygon, capsule, chevron, bar, lattice or arc with a role, placed by `repeat`,
  `mirror`, rows and kits. A player's design (M8) is drawn exactly as written.
- **The surface becomes a material per part** (#121, #135), lit by the system's stars (#119),
  moving only as `f(time, seed)` (#136).
- **Every material has a fallback** (#120): no shader at all is today's flat composition;
  a driver that compiles `hull` but not the new shader draws today's shaded composition.
- **Colour is art, not allegiance** (#117). A livery is a *people's* palette — the culture
  that built the thing — and says nothing about whether the viewer should shoot it.

What #501 did for a planet — one shader paints the whole disc — is wrong for a built thing:
a station is twenty parts, and its look is *in* the composition. So the method changes:
**each part keeps its outline, and its surface is finished by a material chosen for what the
part is.**

## 2. The approach

### One shader, many surfaces

`data/shaders/materials/built.fs` (includes `body.glsl` for its noise table, octave rule and
hashes — nothing duplicated). A constant `surface` selects what the part is:

| `surface` | material id | what it draws |
|---|---|---|
| 0 | `plating` | hull plates in rows that fit the part; bevelled seams; a third of the plates cut finer; access panels; brushed grain; livery stripes; lit windows on stations |
| 1 | `plating.recess` | the same, darker, coarser, no livery — recessed decks, panel-role parts |
| 2 | `plating.trim` | machined bright edges, segments round a ring, livery marks on the ring |
| 3 | `truss` | dark worked metal with ribs across it — antenna-role parts, lattices |
| 4 | `armour` | heavy plates, deep seams, strong bevels (`hull.armour`; rollout) |
| 5 | `radiator` | pale ceramic fins with heat pipes, a dull red glow that breathes on a 64 s loop |
| 6 | `solar` | small square cells under a silver grid; glint in the light, black out of it |
| 7 | `glass` | dark glass with the light's sheen by day, lit from inside on the night side |
| 8 | `nozzle` | heat-tinted metal (straw to blue) toward the throat; the throat glows at idle and white-hot under thrust |

The materials share one compiled program (the library now compiles a shader file once,
however many materials bind it), so eight surfaces cost one compile at startup.

### Chosen by role, refined by the part

Two existing mechanisms carry the choice, plus one small addition:

1. **The object's material maps roles to surfaces** — new field `roles` on a material:

   ```json
   { "id": "plating", "shader": "built", "fallback": "hull",
     "roles": { "panel": "plating.recess", "trim": "plating.trim", "antenna": "truss" }, ... }
   ```

   An archetype says `"material": "plating"` and every hull-role part is plating, every
   panel-role part recessed plating, and so on. The five roles stay the five roles; no new
   role is needed, so nothing about the composition grammar changes.

2. **A part names its own** with `material` (#426, already in the grammar) where its
   *function* is more than its role: the fins of `radiator`, the cells of `station.solar`,
   the panes of `windows` and `hull.cockpit`, the nozzles of `hull.engine` and
   `hull.thruster`. These live in the **module library**, so every object that wears the
   module gets the surface — which is how a player's design gets it (§6).

3. **`fallback`** (new) names what to draw when a material has no shader on this driver:
   `radiator → hull`, `armour → plating → hull`. The backend follows the chain. `glass` has
   none: without its shader a window is drawn as the light it was before.

Unknown names stay load errors: a `roles` key that is not a role, a role or fallback naming a
material that is not in the file, a misspelt livery field — each refuses the whole file, as an
unknown source always has. A module part naming an undefined material is now reported at load
like an archetype's (`Materials::UnknownInArchetypes` checks the module library too).

### What a part knows about itself

Two new sources (`documents/world_format.md` lists all of them):

- `item.partShape` (vec4): the part's half length along its axis, its half width (radius if
  round; a ring's thickness in `x`), its sides and its form — all in the **object's radii**.
- `item.frame` (vec4): the whole object on screen — centre, radius in pixels, heading.

With them a pattern is laid out **in the part's own frame and to its own size**, never in raw
world or screen units:

- **Long flat parts** (bar, chevron: hull plating): a whole number of rows across the width, so
  a seam meets the edge instead of being cut by it; plates `aspect` times as long as wide, each
  row its own length and offset, like a hull.
- **Long round parts** (capsule, lattice: modules, arms): a barrel — sections that run all the way
  round, so the seams across line up from row to row like a pressurised module's frames.
- **Polygons**: rings of plates parallel to the sides, cut along each side.
- **Discs**: concentric rings round a cap, cut into segments.
- **Rings and arcs**: segments round the ring, a whole number of bands across its thickness.

The panel pitch is a fraction of the object's radius, made finer on a bigger object by
`(size / 100)^-0.35`: a station is plated in more panels than a fighter, though not in
proportion to its size, or a station's plates would be specks and a fighter's a handful of
slabs.

Grime and scorch are laid in the **object's frame** (`item.frame`, turned by its heading), so
a stain runs on across the joint between two parts and turns with the ship.

### Night side, lights, engines

- **Night** is the half of the *object* turned from its light, measured across it from its
  centre — the rule the CPU's `onlyDark` parts already use. A part's own shading never has a
  night side (everything is lit a little from above so ships stay findable), so it cannot be
  the test.
- **Windows**: rows of panes on some panel rows only ("decks"), some lit, by the panel's roll;
  dark panes by day, warm or cool light (livery `warmth`) at night. Too small to see one by
  one they become a faint glow across the night half — the city-lights effect of #501 at
  station scale. Only on objects over ~120 world units: a fighter has a cockpit, not decks.
- **Running lights** are the existing `light` parts (nav lights, beacons), unchanged.
- **Engine glow**: the nozzle's throat (its wide end, the module's -x) glows dim orange at
  idle and blue-white under `item.thrusting`; the plume parts (`onlyThrusting`) are unchanged.

### Damage hooks (#138)

Scorch now, from `item.intensity`: soot patches in the object's frame that spread as the
hull drops, embers flickering at their edges when damage is heavy, and the old hull flicker.
Parts *taken off* are #138's: when a part can be lost, the backend skips it and draws the
exposed edge with a `surface` for torn structure (a new kind, `frame`: dark ribs, sparks).
Per-part health on the wire is #138's question, not this one's.

## 3. Parameters in data

### On a material (constants in `materials.json`)

| constant | meaning |
|---|---|
| `surface` | which kind (table above) |
| `pitch` | plate size, in object radii, before the size rule and the livery's `panels` |
| `relief` | how far neighbouring plates tilt against each other |
| `shine` | 0 matt .. 1 glass |
| `paint` | 1: livery stripes are painted on it |
| `windows` | how many lit windows, times the livery's |
| `heat` | self-glow: radiators, nozzles, a pane's interior light |

### On a livery (`liveries` in `materials.json`)

| field | meaning |
|---|---|
| `base` | multiplies the object's colour — applied on the CPU, so it holds with no shader too |
| `accent` | stripe and band colour |
| `wear` | 0..1, paint chipped back to metal at plate edges |
| `grime` | 0..1, soot in seams, streaks along parts |
| `stripes` | 0 none, 1 a band at each part's forward end, 2 a line down the spine, 3 hazard chevrons |
| `panels` | plate size multiplier: mended short plates, or long seamless ones |
| `aspect` | plate length over width |
| `patchwork` | share of plates replaced from another batch |
| `warmth` | window light, cool white (0) to sodium (1) |
| `windows` | share of windows lit |

A livery is picked by the **culture** of the owning faction (`factions.json`), or by the
faction's id when it has no culture. Wired for stations in the prototype
(`Station::Describe`); NPC ships and structures are slice 3.

## 4. Palettes: kin but distinct

The same hub and the same scout under each, from the contact sheet:

| livery | base | accent | character |
|---|---|---|---|
| `guild` | cream | deep blue | clean, a blue band at each end, long plates, warm lit decks |
| `syndicate` | graphite | amber | corporate, an amber line down every spine, cold white light |
| `colonists` | weathered off-white | rust orange | mended short plates, grime, homely windows |
| `clan` | oxide brown | hazard yellow | heavy square plates, chevrons, patched and filthy |
| `sect` | pale | teal | spotless, very long seamless plates, teal spines |
| `automata` | dark graphite | cyan | seamless, no windows, no wear |
| `traders` | sand | wine red | busy lit decks |
| `Pirates` | soot | bone white | salvaged, hazard marks, the most grime |
| `default` | none | — | plain plating for anything unowned |

None uses its faction's instrument colour: the Guild's overview green and the Syndicate's red
appear nowhere in their liveries.

## 5. LOD and cost

The rules, cheapest path first (ships are many and small):

1. **A part under 3 px in cross section is drawn flat** — the existing `MIN_SHADED_PIXELS`.
   At flying zoom most of a ship's modules are here: they cost what they always did.
2. **Plates fade in by their own size**: seams and bevels from ~4 px a plate, fully by ~11;
   finer cuts from 10–18 px, access panels from 12–22 px; window panes from ~1 px high,
   otherwise a glow. A ship 40 px long is lit plating and its livery — no seams to alias.
3. **Grime octaves follow the object's size** (`bOctaves(R, 4)`), and none under 10 px.
4. **One noise lookup is four `texelFetch`es** of #501's table. A plating fragment does a
   grime fBm of at most three octaves; the streak noise only where there is grime, the chip
   noise only near a plate's edge; the brushed grain is a hash per line, not a noise.
5. **One program, many materials**, and `Begin` sets the program once and its uniforms by
   index (it was a `glUseProgram` and a map lookup per uniform), and skips a material's
   constants when that material set them last. Parts of one object bind per part (as `hull`
   already did, #135); the `readsPart` flag keeps planets' crater marks sharing one bind
   (#296), because `hull` reads no part inputs.
6. **The paint budget covers built things too.** A built object whose box on screen covers
   more than `paintBudget` (look.json, shipped 0.5 MP) is drawn whole into #426's offscreen
   target at reduced resolution — never below half — and scaled back up, exactly as a planet
   flown over is. That bounds what a station filling a 1080p screen costs on an integrated GPU,
   at the price of softness close up there; a desktop raises the budget in F10.

**Measured** (desktop: i5-12600K / RTX 3060 Ti, Release, vsync at 120 Hz; GPU time of the
world phase, median of 3–5 runs of 300 frames; other agents shared the GPU, so ±0.2 ms):

| scene (editor card, 1280×720) | before (`hull`) | after |
|---|---|---|
| hub fitted (~0.1 MP) | 0.59 ms | 0.71 ms |
| hub zoom 3 (fills the card, ~0.5 MP) | 0.72 | 0.96 |
| hub zoom 6 (the core fills the card) | 0.29 | 0.68 |
| 100 scouts at ~31 px (flying size) | 4.2 | 2.7 |
| 100 scouts at ~90 px | 7.6 | 8.5 |
| scout zoom 2 (fills the card) | 0.61 | 0.72 |

The fleet rows are draw-call bound (100–300 binds, the same number before and after), which
is why they are noisy and do not grow with the shader. With the shader swapped for `hull.fs`
under the same bindings, the hub at zoom 6 costs what `hull` does (0.35 ms): the difference is
shading, not binding — about **0.6 ms per megapixel** of built surface on this desktop, close to
#501's rocky planet (0.5 ms per megapixel). In the client at 1920×1080 by a Syndicate exchange:
world 0.70 ms GPU at zoom 0.35, 0.46 ms at zoom 1.5 (the station across the screen, painted at
reduced resolution); frames held the 120 Hz cap throughout.

**UHD 620 estimate** (not measured): by the same reckoning #501 used for planets (the laptop
some 25–40× slower in fill-bound shading), a megapixel of built surface costs 15–25 ms there.
Hence the budget: a station across the whole screen is bounded to 0.5 MP of shading, ~8–12 ms
plus the pass — tight, and the first thing slice 6 measures on the laptop. At flying zoom a
station is 0.1–0.2 MP (2–5 ms), and a hundred ships at flying size are ~0.02 MP of shading:
what they cost is their draw calls, as before.

## 6. Player designs (M8)

Nothing extra is needed for a design to be finished:

- its **sections** are hull-role parts → `plating`, via the archetype's material;
- its **modules** carry their function surfaces from the library (a radiator a player fits is a
  radiator, glowing);
- its **livery** is its owner's culture — for a player, their corporation's (question below).

A player who designs a new module writes parts with roles, and may name a `material` from the
same list; an unknown name is a load error as for any data, which is the right answer for the
M8 editor too (refuse, say why).

## 7. Fallback

| driver | what is drawn |
|---|---|
| everything compiles | the finished surfaces |
| `hull` compiles, `built` does not (or `nopaint` in the editor) | every built material falls down its chain to `hull`: today's shaded composition, with the livery's base colour |
| nothing compiles (`noshaders`) | today's flat composition, with the livery's base colour applied on the CPU |

The contact sheets show all three. Shader failures are logged and listed in the F10 panel as
before; nothing stops the game.

## 8. Slice plan for the rollout

Each slice is one PR with before/after contact sheets; cheap agents can take them in order.

1. **Stations** — `station.mining_outpost`, `station.shipyard`, `station.military` to
   `plating`; check each kit's modules for function surfaces (`station.hab`, `station.dome` →
   `glass` panes; `station.dock` berth ring; `station.reactor` → `radiator`-like glow;
   `station.refinery`, `station.ore`, `station.crane`, `station.shield`, `station.sign`).
2. **Outposts and structures** — `structure.outpost*`, `structure.outpost_market`,
   `structure.anchor`, `structure.site`, `structure.beacon`, `structure.buoy`,
   `structure.battery`; wire `Structure::Describe` to the owner's livery.
3. **Ships** — every ship archetype to `plating`; `hull.armour` → `armour`, `hull.cargo`
   containers (keep their tints), `hull.hangar`, `hull.wing`, `hull.fin`, `hull.keel`,
   weapons; wire `NpcShip::Describe` to its faction's livery; the player's ship to their
   corporation's (needs the owner's answer).
4. **Derelicts and gates** — `derelict.*` on a `derelict` livery (wear 1, grime 1, no lit
   windows, scorch baked by seed); `gate.*` on `plating.trim` with their own markings.
5. **Damage** — the `frame` surface and the part-loss hook with #138.
6. **Laptop pass** — measure on the UHD 620 (and check that `built.fs` compiles on Intel's
   driver: it was only compiled on NVIDIA here; a failure falls back to `hull`). If a station
   at full screen is still over ~4 ms there, a cheaper tier: plates without grime noise above
   a size, or the shipped budget lowered to 0.35.
7. **Tuning** — a gallery sheet per culture across every archetype, with the owner.

## 9. Questions for the owner

1. **Direction**: is plating-with-livery the right register — clean industrial (Elite,
   Homeworld) rather than painterly? More grime and wear by default, or less?
2. **Livery on the player's own ship**: their corporation's culture, a palette they pick from a
   small set, or free choice of base and accent (still art, never allegiance)?
3. **NPC ships**: wear their faction's livery (slice 3), so a Syndicate trader *looks*
   Syndicate? That is information a player can read from the window — intended?
4. **Windows**: only on stations and big structures (now), or on capital ships too?
5. **The night side** in a dim system is dark (the built surfaces use 0.75 of the scene's
   ambient floor, so shadows read). Keep, or back to the full floor so nothing is ever dark?
6. **Damage**: scorch with embers now — too much fire for a hull at 40%?
7. **The paint budget** now also softens a station seen close on the shipped 0.5 MP (the
   contact sheet shows both). Keep one budget for planets and built things, or a separate one?
