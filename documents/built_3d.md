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


## 5. Second round (owner's verdict: "really more interesting"; tilt, mass, gates)

### 5.1 Tilt

Built solids are drawn with an **oblique** lean: a height is carried up the screen by
`tan(tilt)` of itself and the ground does not move. A footprint is where the 2D world puts it at
every tilt, so picking, docking, labels and brackets are untouched; only the visible mass rises
above that footprint (a tall object's brackets sit at its feet, which reads correctly). Depth is
`height * cos + (down the screen) * sin`, so what is higher and what is nearer both win. Walls
are real now: every piece hangs a vertical skirt from its outline to what it stands on, with
lit window rows on a station's walls. A star's height over the plane is 26 degrees (was 38), so
shadows are longer. Exposure is a filmic roll-off with one scale; pale hulls no longer clip.
`ECON_BUILT3D_TILT`, `ECON_BUILT3D_ELEV`, `ECON_BUILT3D_EXPOSURE` until they are settings.

Honest reading of the three tilts (contact sheet, trade hub, military station, shipyard, outpost):
- **15**: side walls are a sliver; the object reads as lit and shadowed rather than as a volume.
  The safest, and the least change over the 2D look.
- **25**: walls and the steps of plating read at once, towers and masts show their height, and
  the shape is still a plan one can recognise. The recommendation.
- **35**: strongest volume, but tall things start to lean over what is behind them, a ring
  tilts into an oval-looking stack, and the footprint under a tall object is hard to find.

### 5.2 Mass from the data

`Built3DDetail` adds what a composition implies, never by object: a truss under every long member
with collars along it, stepped plating and corner buttresses on a large plate, ribs, habitat
blocks of three sizes and lamps on a ring, **spokes** from the hub to a ring that would otherwise
hang in space (placed between the arms, with radiator fins on some), tanks, platforms and lamps
round a pad, masts and dishes and modules of several sizes on a roof. Choices come from the
object's id and the source piece's index through an integer hash, so they are the same on every
client; more is added as the object is bigger on screen (levels 0, 1, 2). The trade hub is no
longer an empty thin ring. Gates are not embellished: their composition is intent.
Two data fields were added to a part: `height` (how far it stands above what it lies on, in
radii; zero leaves it to the rule) and `lift` (a part that floats: its underside's height, with
`height` its thickness: a lintel, a claw).

### 5.3 Gates

What went wrong with the old gate was intent, not detail. Three things make a gate read at a
glance as something to fly through: an **axis** that faces where it leads (a gate is turned to
face away from the system's centre, toward the system it joins: the generator lays gates out on
that bearing), an **aperture the structure is built around** (towers and a lintel; the field is a
sheet standing only between them), and an **approach lane** (a funnel of blinking lights that
chase toward the threshold, and chevrons on the ground). The tilt is what lets an aperture stand
upright. Colour: structure is warm alloy (the stations' own family) with an amber line, light
is warm amber-white; **amber means built**. A natural wormhole is violet (the existing
`wormhole` material), so the Anchor gate, a built frame clamped round a natural hole, is amber
hardware around a violet swirl, which is the whole of its meaning. No teal: it belongs to the
nebula and the wormhole family, and a cold hue on a gate competed with both.

The three concepts (`gate.a|b|c.jump|anchor`, generated by a script from the same grammar; the
mock-up archetypes are not in the shipped data):
- **A, the doorway.** Two towers, a lintel, a lit sheet between. Best read of "a door", and the
  worst of the three when its axis runs along the screen's horizontal: the aperture then lies
  along the screen's vertical and foreshortens to a line.
- **B, the hoops.** Six portals in a row, each smaller and lower than the last, with a faint sheet
  in each and a rail and chevrons on the ground: a tunnel with a far end, whose perspective taper
  carries the direction. Strongest read of "through to somewhere" with the axis down the screen;
  with the axis across it is a comb of poles with a taper, weaker but still directional.
  The Anchor version is three heavy hoops round the swirl.
- **C, the crown.** Spires round a lit disc with pearls of light stepping down toward it, open on
  the approach side, with a boom on the exit side. Reads the same at any bearing (the one to pick
  if bearing-independence matters), but with less intent: a ring of lamps round a disc is a
  chandelier until the lane lights are seen. The Anchor version is five claws reaching over the
  swirl.

Recommendation: B for the jump gate, C's lit core added to B's far end, and A's tower mass at the
entrance; the Anchor gate as the claw cradle of A/C round the swirl. A gate seen with its axis
across the screen is the case to keep testing.

### 5.4 Cost

Release, desktop RTX 3060 Ti, game client near a trade hub at zoom 0.2 (one station in view,
vsync-bound at 120 fps), ms per frame in the world phase, CPU / GPU: 2D path 0.87 / 0.59; solids
quality Flat 0.30 / 0.07; Shadows 0.46 / 0.19; Full 0.34 / 0.28; High 0.79 / 0.34. Mesh building
(with detail) happens once per object and size bucket. Not measured on the Intel UHD 620: the
fragment work is unchanged by the tilt (a taller box is a few more covered pixels). Cut in this
order there: High to Full, then occlusion off (Shadows), shadow steps to 8, height map 256, and
no generated detail below level 1 (`ECON_BUILT3D_DETAIL=0` turns it off to compare).

### 5.5 Known edges

Gate heading is set only in the 3D path (from the bearing); in production it belongs on the
`Item` for every renderer. The 2D glows over a lifted part are drawn at ground level. Walls carry
panel seams only as vertical lines. Piece budget: level 2 on a large station is several hundred
solids; the 16-bit index limit stops a mesh at 60000 vertices.
