# EconSpace -- Built objects as solids: a rendering spike (#618)

Spike, branch `built-3d-spike`, draft PR. Owner's direction (2026-11-25): try real 3D for built
objects -- parts generated as meshes from the same composition data, drawn top-down, lit by the
system's stars with real shadows and occlusion -- and then the owner decides.

## 1. Approach

The game stays 2D. Same world positions, same `Render::Item`, same camera; only how a built
object is drawn changes, and only when asked (`ECON_BUILT3D=1`, `ECON_BUILT3D=ships`,
`ECON_BUILT3D_QUALITY=0..3` until it has a setting).

**Meshes from the same data.** `Compose` already places every piece (kits, modules, repeats,
mirrors, jitter). `Built3DMesh` turns each placed piece into a bevelled slab: a small disc is a
dome, a large one a plate, a ring or arc a rounded tube, a capsule a cylinder with round ends, a
polygon, bar and chevron plates with a chamfer, a lattice its rails and struts. Seen from
straight above only a slab's top matters (a wall is edge-on), so a piece is its top surface:
concentric loops at increasing inset with a height profile, normals from the profile's slope.
Nothing is written by hand and no data format changed.

**Height by a rule.** A piece stands on the level its `z` says, or on the highest earlier piece
under its centre, plus its own thickness; thickness comes from role and size (`RiseOf`: bigger is
thicker up to a cap, a mast is thin and tall). So a module on a hull is a real step up from it, a
pad on an arm stands on the arm, and a player's design built from the grammar gets heights with
no extra field. `Piece` gained four fields (spin, blink, blink phase, alpha) so the GPU can turn
and blink parts: the mesh is built once per object and size bucket (`Compose` at time 0, heading
0) and nothing is rebuilt per frame (#136 holds: motion is `f(clock)` in the vertex shader).

**Two passes, one thing to read.** A top-down object is a heightfield (slabs on slabs, nothing
overhangs). Pass 1 draws the object's heights into a float target (at most 512 square); pass 2
draws the solid into the frame through the 2D camera (the vertex shader reads `Camera2D`
directly; depth comes from height, and the depth buffer is cleared over the object's box only, so
draw order against the 2D layers is unchanged). The fragment shader then marches a ray toward the
star through the height map for **cast shadows** (soft penumbra growing with distance, 12-32
steps) and samples a ring around the point for **ambient occlusion** where parts meet. One
technique gives both, for any number of parts, with no shadow map and no light-space matrices.
Why not a shadow map: it needs a light-space pass per light and per object, resolution and bias
work, and gives no occlusion; the heightfield is exact for this geometry and costs by covered
pixels.

**Surface.** Blinn-Phong with the livery's colour and accent, panel seams and per-panel tone in
the part's own frame (fading out below a few pixels, as `built.fs` does), lit windows on the side
the star does not reach, emissive and blinking lights with their own solidity (a gate's field
stays a translucent glow). Edges are anti-aliased from the distance to the outline, with no
multisampling. A mild whole-object dome in the normals (`DOME`) gives a station a lit side and a
dark side, as a planet has: the form-level light the diagnosis (`built_look.md`, cause 1) asked
for. It is a cheat, and it is the one the picture most needs.

**Fallback (#120).** If the shaders do not compile or the float target cannot be made,
`Renderer::Available()` is false and the 2D path draws exactly as before. Glows, orbiting parts
and surface marks are not solids and are always drawn by the 2D path over them.

Files: `render/Built3DMesh.*` (geometry, tested), `render/Built3D.*` (GPU), `data/shaders/built3d/`,
a hook in `ShapeBackend::Draw`, `tests/built3d_mesh_tests.cpp`.

## 2. Results

The contact sheet (planets | built now (2D) | built 3D: trade hub, military station, jump gate,
outpost, cruiser with ships on) and the cards are in the PR conversation, not in the repository.

Honest reading:
- Shadows and occlusion do what the diagnosis said was missing: modules sit on hulls, pads on
  arms, bevels catch light, a station has a lit side and a dark side. It reads as an object rather
  than a diagram, most on the military station and the outpost.
- It does not close the gap to a planet alone. A planet fills its frame; the trade hub is still
  mostly empty ring. Mass (C1 and C2 in `built_look.md`) remains the biggest lever, and 3D makes
  it pay more: a massed hull casts and receives.
- Ships work (`ECON_BUILT3D=ships`) but a pale hull washes out; they need their own tuning.
- Exposure is not tuned: the lit side of pale hulls (outpost core, cruiser) clips toward white even
  with a soft shoulder; the light model needs a pass against the planets' tone.
- No in-game frame beside a station was obtained in this spike (a docked pilot's view is covered
  by windows and the station is off screen after undocking); the world renders with the solids on
  in the game binary, but the evidence here is the editor cards, which use the same backend,
  light and treatment.
- Rough edges: seams on large plates are too regular, no surface noise yet, shadows are short
  (the star's elevation is a fixed 38 degrees and heights are capped), the whole-object dome is
  fake.

## 3. Cost

Desktop (RTX 3060 Ti), Release editor `perf`, `gallery shapes card <id> fleet N`, ms per frame in
the world phase, CPU / GPU, 2D now against 3D (quality 2: 20 shadow steps and occlusion):

| card | fleet | 2D | 3D |
|---|---|---|---|
| trade hub | 40 | 3.09 / 1.77 | 1.37 / 0.59 |
| military | 40 | 2.88 / 1.96 | 1.24 / 1.05 |
| jump gate | 40 | 6.76 / 4.25 | 1.36 / 1.00 |
| any of the three | 1 | 0.9-1.4 / 0.7-0.9 | 0.26-0.28 / 0.06-0.10 |

The solid path is cheaper than the 2D path on the desktop because it replaces hundreds of
per-part shader binds and draw calls with two draws per object; CPU inside the solid path is
0.07 ms per object. These cards are fitted, so a fleet of 40 is small objects: it is a count
test, not a fill-rate test.

**Not measured on the Intel UHD 620.** Estimate: the vertex side is trivial; the fragment side
is about 36 texture fetches per covered pixel (20 shadow, 16 occlusion) with some overdraw, so a
station covering 400 by 400 px is roughly 6M fetches, a few milliseconds on that GPU, and a
system has a handful of stations in view, not forty. Expect 3 to 8 ms for a normal view; this
needs the laptop pass before anything lands.

What to cut there, in order: occlusion off (`Quality::Shadows`, 12 steps, about half the
fragment cost); shadow steps 12 to 8; the height map capped at 256 and the object drawn at
reduced resolution as `PaintInto` does for large objects; bake the height map for objects that do
not turn (cache per object, redraw only when something spins); `Quality::Flat` (no height pass
at all, still lit solids).

## 3b. Risks

- A float render target (R32F) is needed; drivers that refuse it get the 2D path.
- Edge anti-aliasing is inside-only and a translucent edge writes depth, so a seam can show where
  a lower part is drawn after a higher one's edge. Parts are drawn low to high, so it is rare.
- Damage (lost parts, scorch) and livery stripes are not ported; the spin and blink clock wraps at
  7200 s (exact for spins that are multiples of 0.05 deg/s).
- Heights and the star's elevation are art constants (`Rise`, `Level`, `LIGHT_ELEVATION_DEG`);
  they will want tuning, and a player's design could build a tall spike (heights are capped).
- Two passes per object: many overlapping objects multiply it; ships should stay 2D until proven.
- A second surface renderer to keep in step with `built.fs` unless the 2D path is retired for
  built objects.

## 4. Slice plan to production

1. **S1** A setting (off / flat / shadows / full) in the treatment panel and `TreatmentConfig`, the
   laptop measurement, a default by GPU class; remove the environment switches.
2. **S2** A height-map cache for static objects; paint-budget scaling for objects filling the
   screen.
3. **S3** Surface parity with `built.fs`: livery stripes, wear and grime, damage (lost parts,
   scorch), the `frame` material.
4. **S4** A tuning pass on every built archetype beside planets (heights, dome, elevation from the
   system's star), with C1 (mass) done in data first.
5. **S5** Ships: their own exposure and a per-ship budget, or leave them 2D.
6. **S6** The editor: a card of the height map; a golden test of the height rule.
7. **S7** Emissive bloom for windows and lights (`built_look.md` D1), now that they are real
   lights.
