# EconSpace — Why built things look cheap, and a wormhole that is weather (#618)

Design plus prototypes. The owner's verdict on `gates_and_wormholes_look.md`: none of the three
directions; a "living portal" fill reads as a stock effect; gates as built structures is right in
principle, but **built things in general -- gates and stations alike -- look cheap next to the
planets**; a wormhole should be a natural phenomenon with no structure, ticks or lines.

The prototypes are on branch `built-look-proto` (not for merge): shader and data changes, plus the
renders referred to below under `documents/built_look_proto/` on that branch (`contact_sheet.png`,
`before_after_more.png`, `wormhole_states.png`).

The premises hold (CLAUDE.md): generated look (M6), composition in data with seven primitives
(#122), a material is a shader plus bindings (#121), per part (#135), light is a property of
objects (#119), motion is `f(time, seed)` (#136), a shader may fail and the game goes on (#120),
colour is art not allegiance (#117). Nothing below adds a primitive.

---

## 1. Diagnosis

Method: editor gallery cards (`worldeditor gallery shapes card <id> treated time 100`) of five
planets next to stations, gates, outposts and a ship, same light, same treatment, plus a read of
`terran.fs`, `body.glsl`, `built.fs` and the backend's part loop (`ShapeBackend::DrawComposition`).
The contact sheet "planets | built now | built after" shows it at a glance. (A frame from a running
server beside a station was not taken: the editor's cards use the same backend, light and
treatment as the game, and the card was enough to separate the causes.)

### What a planet has, in order of how much it matters

1. **It is one solid, and the picture says so.** `bNormal` gives every fragment a sphere normal;
   one light gives a continuous gradient from full day through a terminator to a dark limb, and
   the whole disc is a single tonal range from near-white to near-black. The eye reads *form*
   before it reads anything else.
2. **It fills its frame.** A card of a planet is covered by the planet: 60-70% of the pixels are
   surface. The same card of a trade hub is under 15% surface, the rest is a hair-thin ring and
   black.
3. **Detail at five scales at once.** Continents, coasts, cloud, grain, and (when close) stones:
   `bFbm` with octaves chosen by size on screen, a warp so nothing is a blob, a `fine` term past
   120 px. Nothing is a flat fill at any distance.
4. **Edges are not drawn.** Coasts, caps and cloud are torn by noise; the limb is a falloff and
   an atmosphere, not an outline.
5. **It lights itself.** City lights on the night side, a glint on the sea, a glow on the lit
   limb: emission and specular are part of the object, so it has a day side, a night side and a
   rim, three different pictures in one.
6. **It is one palette made of few colours with variance**, so its colour is rich without being
   loud.

### What a built object has, and why each reads as cheap

1. **No form, only parts.** Each part is shaded alone as a chamfered slab, lit a little from above
   so that it never has a night side (`built.fs` says so: "every part is lit a little from above,
   so a part's own shading never has a night side"). The station as a whole therefore has no day
   and night, no terminator, no limb. It reads as a stack of stickers, evenly lit.
2. **No part shades another.** The loop draws parts in z order, each over the last. A pylon, a
   module on a ring, a dish over a deck cast nothing and receive nothing. There is no ambient
   occlusion where things meet. This is the single largest reason a station looks like a diagram
   and not a thing: depth in a top-down view is almost entirely *shadow*.
3. **Sparse, thin silhouettes.** The trade hub's ring is 0.08 radii wide, the gate is four arcs
   with gaps and buoys that float; the outpost's arms are bars. At any size the objects are 80-90%
   empty. Planets, gas clouds and the ship are the only things that fill their bounding disc. A
   built thing in games that look expensive is *massed*: it has a body, then structure on the body,
   then detail on the structure.
4. **Symmetry and uniform scale of detail.** Radial and mirror symmetry with identical parts reads
   as generated. Real structures have one or two large masses and many small things at very
   different sizes; the kit sockets (`scale` 0.05-0.16) make detail all one small size, and
   `plain: 0.45-0.5` leaves half the sockets empty.
5. **Surface detail is faint and fits the part, not the object.** Plating seams are a few percent
   of albedo and exist only when a panel is several pixels wide. At a station's normal size on
   screen (200-400 px) the plating shader is working, but its output is below the contrast the
   screen treatment's grain and blur leave standing.
6. **Emission is timid.** Windows are small lit dots and nav lights tiny discs. Nothing glows into
   the dark around it. A planet's night side is a feature; a station's is the same grey, dimmer.
7. **The screen treatment is neutral to planets and hostile to small crisp things.** Grain and
   vignette hide planets' low-frequency gradients' banding; on a 1-2 px seam they eat the detail
   that was there.
8. **Gates specifically** are the worst case of all of the above: the field is a flat dark disc
   (the one big surface, with nothing on it), the ring is arcs with air between, and mouths and
   holes add rings and chevrons -- a *drawn* object where a phenomenon was needed.

### How others make built things look expensive

All of these are 2D or top-down; none uses a planet-grade shader. They lean on the same four
levers, which map one to one to the causes above.

| game | what carries it | maps to |
|---|---|---|
| **Starsector** | Ships are painted, then lit by a normal-map-ish pass; shapes are **massed** (hull plus armour skirts plus turret mounts) with detail at three sizes, and the dev blog's "Trouble With Greebles" is about *restraint*: detail only where function says so. Battlestations are drawn as one big silhouette per faction, and a post titled "The Circle Can't Be Trusted" on avoiding plain rings. | causes 3, 4 |
| **Endless Sky** (remastered sprites) | The artist found the dark shadows he had avoided were needed to define form; recommends a key light, a dim fill and a back light that outlines the silhouette. | causes 1, 2 |
| **Cosmoteer** | Every module is a tile with its own relief, **decals** put in empty areas, and a glow/emissive layer for every powered thing. Structure reads from tile shadows. | causes 2, 3, 6 |
| **Highfleet** (hand painted, not verified in detail here) | Hulls are large dark masses with a few warm lights, sooty wear gradients over large areas, thin bright rim. | causes 1, 6 |
| **FTL** | Small flat sprites made to read by hard shadow lines and one bright accent colour. | cause 2 |
| **EVE** (stations and gates) | Real 3D, lit by the system's star; stations are *huge massed* structures with thousands of lit windows. Gates are monumental. | causes 1, 3, 6 |
| **Homeworld / Nebulous** | Dark hulls with a bright rim light on the star's side and tight emissives; silhouette first. | causes 1, 6 |
| **Stellaris** | Stations are 3D with strong directional light and emissive strips. | causes 1, 6 |

Sources read for this: the Starsector dev blog (the "Let Me Draw You A Starsector Ship" posts,
"The Trouble With Greebles", "The Circle Can't Be Trusted"), Endless Sky's ship sprite posts, the
Cosmoteer wiki on making Cosmoteer-style visuals. Highfleet and Nebulous are from the games
themselves, not a source I could cite here.

**Honest summary:** the game's materials (#516) are good; what is missing is everything *above*
the material. A planet is one object lit as one object; a station is twenty well-surfaced parts
lit as twenty objects, with nothing between them. A better plating shader will not fix that.

---

## 2. Proposal

Four changes, ordered by how much of the gap each closes, all in data or a shader or one backend
loop. No new primitive.

### A. Shade the object as one body (shader, data-bound) -- "depth cue"

A uniform `depthCue` on the `built` shader (0 = today). The object's frame is already in
`frame`; each fragment is lit by where it is along the light across the whole object: day to
night over the object, and each part's own edge falls into shadow where it meets what is under it.
Result: a terminator and a limb for stations, ships and gates. One extra `mix` and one
`smoothstep` per fragment: free.

### B. Parts shade each other (backend, one loop) -- cast shadows

Before a shaded part is drawn, its silhouette is drawn three times offset away from the light,
**multiplied** into the picture, not painted over it. Multiplying means a shadow that falls on
the empty sky changes nothing, and one that falls on plating darkens exactly the plating: no
depth buffer and no new pass. Height is the part's `z` and size (nothing new in data; a design
players write gets it free). Offset and strength are capped at a fraction of the object radius so
a shadow never reads as a second object.

### C. Mass and scale of detail (data, with a generator rule)

The kit grammar already has sockets and modules; what is missing is a rule for *mass*. For
hand-authored archetypes: a hull has a body, a skirt (a lower, larger polygon or ring with
`z: -1`, `-2`), structure on the body (bars on a ring, plates between arms), and the kit. For
the generator and players' designs (M8): a **fill rule** -- every built object is required to
reach a minimum surface fraction of its bounding disc, filled by adding skirts and plates before
modules, and kit module scales drawn from a spread (0.04 to 0.3) rather than a band. The
prototype does it by hand on two stations (a habitat ring with 12 modules and a second structural
ring on the trade hub; an armoured octagonal skirt and diagonal bars on the military station).

### D. Emission that matters (shader, not prototyped)

Windows, nav lights and engines write into a wider soft glow that the screen treatment's bloom
(already there) can catch, on the night side as a *feature*: a trade hub's night side should be
a constellation of warm windows the way a planet's is a scatter of cities. This depends on how
the treatment's bloom threshold is tuned; not prototyped here.

### E. The wormhole as weather (new shader `wormhole`, data)

`gate.mouth` and `gate.roaming` lose the rings, chevrons and discs. They are one soft disc with
material `wormhole`, plus one `fallback` soft disc that is drawn only when the shader is not
(#120's rule, exactly as the planets' stand-ins).

What the shader draws, and what it does not:

- gas wound into a spiral around the throat (angle shifted by the log of the radius, noise read
  in the wound coordinates so clumps follow the arms, plus ridged filaments), turning on a 64 s
  loop (divides the clock's wrap), brighter on the side turning toward the viewer, hot and pale
  inward, the object's own colour outward;
- the sky behind it bent: a star field read at a radius pulled inward as 1/r, so stars crowd
  and streak toward the throat (a lens), not the system's real backdrop (see the caveat below);
- the throat, a soft-edged window onto a different, tinted sky; a dark seam, not a line, where
  the bending is greatest; an edge that is wherever the gas thins out;
- **state in the picture**: `item.intensity` is its life. Fresh: wide arms, wide throat. Ending:
  the arms thin and shorten, the throat narrows, the gas cools to red and gutters. Mouth and
  hole differ by colour only; an anchored mouth is the gate, not the hole (below).

Caveat: the starfield it bends is its own, not the real backdrop: a fragment shader painting one
part cannot read the framebuffer without a copy pass. The stars inside it are few and small enough
to pass; a true lens (read the backdrop texture) needs the treatment chain's target as an input,
which is slice W3 below and an owner call on cost.

**Landed (W1).** Tuned after the owner's review: the throat is small and dark with only a thin,
uneven rim of light (no bright window, so no eye); one to three arms, wound tight and torn by a
warp, drifting inward along their own spiral on a 32 s loop instead of turning like a wheel; the
seed picks arm count, winding, handedness and turbulence. Life is `Item::endsAt` (set by
`JumpGate::Describe` from `closesAt`, which was already in the layout) turned into `intensity` by
`Present` on the world clock -- `Render::MouthLife`, one from 12 h out to nothing at the end -- so
no wire field was added. A drifted mouth's gate is removed by `DressCrossings` as before.

**W2 not done, and why.** A lens that reads the real backdrop needs the frame behind the hole as a
texture: a copy of the target (or a second render of the starfield) every frame a hole is on
screen, a full-screen bandwidth cost on exactly the weak GPUs the game must keep playable, and the
stars are drawn on the CPU per pixel, not in a pass a shader could be given. The hole keeps its
own few lensed stars; it stays an owner call on cost.

### F. What this means for gates

Gates stay structures, as the owner said, and get A, B and C: a jump gate that is a mass (a
thick ring with housings and a skirt, not four arcs with air) lit as one body with shadows
between its parts, over a **dark, still field** (no portal swirl). The Anchor gate is the same
object with livery and a state: `DressCrossing` already knows it is anchored; the look can read
it (lit stays, e.g. a lit strip of the ring). Not prototyped: a card of `gate.jump` after A+B
shows the lighting gain only (see `before_after_more.png` and the contact sheet's gate row).

---

## 3. What the prototypes show

Renders on `built-look-proto` (gallery cards, treated, same light and clock):

- **Contact sheet** (`contact_sheet.png`): planets | built now | built after, five rows: trade hub,
  military station, jump gate, outpost, cruiser.
- **Before/after more** (`before_after_more.png`): Anchor structure, ancient gate, scout, shipyard.
- **Wormhole** (`wormhole_states.png`): the old mouth and hole; the new fresh mouth, fresh hole,
  and ending mouth.

Honest reading of them:

- **Stations with C (mass) look like built things; those with only A and B change subtly.** The
  trade hub and military station, which got C, show the largest gain; the jump gate, outpost and
  cruiser, which got only A and B, show a visible terminator and contact shadows, and not more.
  So the order is: C first, B second, A third.
- A and B alone do not close the gap to a planet. The gap is mostly mass and tone, not surface.
- The wormhole is the one that reads as a phenomenon straight away. It still needs tuning: the
  throat reads a little like an eye at some seeds; the brightest arm should be tonemapped less.
- Ending vs fresh is clear at a glance, without any text.

### Cost

Measured with the Release editor, 40 objects per card, 240 frames, desktop (RTX 3060 Ti): ms per
frame, world phase, CPU / GPU, base (the old data, shadows off) against prototype.

| card | base | prototype | per object |
|---|---|---|---|
| trade hub | 3.44 / 2.03 | 5.85 / 2.86 | +0.06 CPU, +0.02 GPU |
| military | 2.49 / 2.05 | 3.73 / 2.71 | +0.03 CPU, +0.02 GPU |
| mouth | 1.44 / 1.04 | 0.65 / 0.35 | cheaper: seven parts became two |

The shadows' CPU cost is mostly extra pieces (three draws per shaded part) and the denser
composition; the shader costs are within noise. A system has a handful of stations in view, so it
is well under a millisecond. **The Intel UHD 620 has not been measured**: the wormhole shader is
of the nebula's class (a few octaves and a few hash reads) and runs only where a hole is on
screen; the shadows add vertex work and overdraw of the parts' own area, three times. Both want a
laptop pass before they land, as #516's slice 6 did.

---

## 4. Slice plan

Each slice is its own PR, one issue, a before/after card pair in the description.

| slice | what | needs |
|---|---|---|
| **W1** | `wormhole` material and shader, `gate.mouth` / `gate.roaming` reduced to one part plus a fallback, tuned (throat, brightness); `DressCrossing` closing state feeds `item.intensity` so ending reads in the world. Done first: it answers the owner's clearest complaint and is self-contained. | `item.intensity` for a gate from `closesAt` (a small sim-to-render field; today intensity is hull) |
| **B1** | Cast shadows in `DrawComposition`, behind the settings panel's switch and a Quality setting (off on weak GPUs); keep `ECON_NOSHADOW`-style kill in the editor for perf runs | the laptop pass |
| **A1** | `depthCue` in `built.fs`; the plating family sets it; tune on liveries | none |
| **C1** | Hand-massed pass on the stations, gates, outposts and structures: skirts, structure on the body, spread of module scales. Judged on the contact sheet against planets | owner's eye |
| **C2** | The fill rule in the generator and in the Fit/design validator (M8): a minimum surface fraction and a spread of scales; bump `GENERATOR_VERSION` | the golden hashes |
| **D1** | Emission: bloom-ready windows and lights, night side as a feature | treatment tuning |
| **W2** | A lens that reads the real backdrop (treatment target as an input) | owner's decision on cost |
| **G1** | Gate redesign as a mass over a still dark field; Anchored versus not, as a lit strip | after C1 |

Open questions for the owner: whether the wormhole's own sky is enough or W2's real lens is wanted;
whether cast shadows may cost a Quality setting on the laptop; how far the fill rule should reach
into players' designs (a rule that rejects a design is a different thing from one that
adds to it).
