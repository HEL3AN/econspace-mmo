# EconSpace -- Built objects as solids (#618)

Stations, structures, outposts and derelicts are drawn as lit solids: meshes generated from the
same composition data, seen from above with a slight lean, lit by the system's own stars, with
shadows and occlusion from their own heights. Owner's verdicts (2026-11-26): "far better"; the
stations are liked as they are (tilt 25 degrees, generated mass, the amber built palette).
Ships stay on the 2D path for now, and gates are a separate track (`gates_and_wormholes_look.md`,
#618).

## 1. Approach

The game stays 2D. Same world positions, same `Render::Item`, same camera; only how a built object
is drawn changes. If the driver cannot, nothing changes at all (#120).

**Meshes from the same data.** `Compose` already places every piece (kits, modules, repeats,
mirrors, jitter). `Built3DMesh` turns each placed piece into a bevelled slab: a small disc is a
dome, a large one a plate, a ring or arc a rounded tube, a capsule a cylinder with round ends, a
polygon, bar and chevron plates with a chamfer, a lattice its rails and struts. Concentric loops
at increasing inset carry a height profile; normals come from the profile's slope. Walls hang
from every outline to what the piece stands on. Nothing is written per object.

**Height by a rule, or from the data.** A piece stands on the level its `z` says, or on the
highest earlier piece under its centre, plus its own thickness; thickness comes from role and
size (`RiseOf`: bigger is thicker up to a cap, a mast is thin and tall). A part may say so itself:
`height` (how far it stands above what it lies on, in radii) and `lift` (a part that floats: its
underside's height, with `height` its thickness) are in the grammar (`world_format.md`, "Height"),
validated like every other field. The mesh is built once per object and size bucket (`Compose` at
time 0, heading 0); motion is `f(clock)` in the vertex shader (#136), nothing is rebuilt per frame.

**Two passes, one thing to read.** A top-down object is a heightfield (slabs on slabs, nothing
overhangs). Pass 1 draws the heights into a float target (at most 512 square); pass 2 draws the
solid into the frame through the 2D camera (the vertex shader reads `Camera2D`; the depth buffer
is cleared over the object's box only, so draw order against the 2D layers is unchanged). The
fragment shader marches a ray toward the star through the height map for **cast shadows** (soft
penumbra growing with distance, 12-32 steps) and samples a ring round the point for **ambient
occlusion** where parts meet. One technique gives both, for any number of parts, with no shadow
map and no light-space matrices.

**Surface.** Blinn-Phong with the livery's colour and accent, panel seams and per-panel tone in
the part's own frame on tops and in courses on walls (fading out below a few pixels), lit
windows on the side the star does not reach, emissive and blinking lights. Edges are
anti-aliased from the distance to the outline. A mild whole-object dome in the normals (`DOME`)
gives a station a lit side and a dark side, as a planet has. Colour is art (#117): the object's
own colour and its livery, never a side.

**Fallback (#120).** If the shaders do not compile or the float target cannot be made,
`Renderer::Available()` is false and the 2D path draws as it always did. Glows, orbiting parts
and surface marks are not solids and are always drawn by the 2D path over them; a glow over a
part that stands up is lifted with it by `Renderer::LiftShift`, from a coarse height field the
mesh keeps (`HeightField`).

Files: `render/Built3DMesh.*` (geometry, tested), `render/Built3DDetail.*` (mass, tested),
`render/Built3D.*` (GPU), `data/shaders/built3d/`, a hook in `ShapeBackend::Draw`,
`tests/built3d_mesh_tests.cpp`.

## 2. Tilt

Built solids are drawn with an **oblique** lean: a height is carried up the screen by
`tan(tilt)` of itself and the ground does not move. A footprint is where the 2D world puts it at
every tilt, so picking, docking, labels and brackets are untouched; only the visible mass rises
above that footprint. Depth is `height * cos + (down the screen) * sin`, so what is higher and
what is nearer both win. Compared at 15, 25 and 35 degrees:
- **15**: side walls are a sliver; the object reads as lit and shadowed rather than as a volume.
- **25**: walls and the steps of plating read at once, towers and masts show their height, and
  the shape is still a plan one can recognise. Chosen.
- **35**: strongest volume, but tall things lean over what is behind them and the footprint under
  a tall object is hard to find.

The star's height over the plane is 26 degrees, so shadows are long enough to read. Exposure is a
filmic roll-off with one scale, so pale hulls do not clip.

## 3. Mass from the data

`Built3DDetail` adds what a composition implies, never by object: a truss under every long
member with collars along it, stepped plating and corner buttresses on a large plate, ribs,
habitat blocks of three sizes and lamps on a ring, **spokes** from the hub to a ring that would
otherwise hang in space (placed between the arms, with radiator fins on some), tanks, platforms
and lamps round a pad, masts and dishes and modules of several sizes on a roof. Choices come
from the object's id and the source piece's index through an integer hash, so they are the same
on every client and compiler; the tests pin the piece counts for a seed. More is added as the
object is bigger on screen (levels 0, 1, 2).

## 4. Cost and the setting

See the PR that made this a setting (`Built objects` in Settings, graphics): frame cost per
quality level, the default, and the automatic drop.

## 5. Known edges

Piece budget: level 2 on a large station is several hundred solids; the 16-bit index limit stops
a mesh at 60000 vertices. Damage (lost parts, scorch) and livery stripes are not ported from the
2D surface shader; the spin and blink clock wraps at 7200 s (exact for spins that are multiples
of 0.05 deg/s). Heights and the star's elevation are art constants (`Rise`, `Level`,
`LIGHT_ELEVATION_DEG`); a player's design is capped in height.
