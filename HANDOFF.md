# Session handoff — 2026-09-04

Where the work stands and what to pick up. Keep this current; it is the first thing to read.

## Direction, and read this before planning anything

Two premises were **inverted**, on 2026-09-03 and 2026-09-04. Both are in `DECISIONS.md` in
full, and both contradict how the repository was originally built — so a plan made from the
old assumptions will look reasonable and be wrong.

**The look is generated, not drawn** (2026-09-03). Glyphs are no longer the primary
presentation. Objects are lit by their system's stars, shaded by a material, and built from
compositions described in data.

**The world is generated, and hand-written content is the exception on top** (2026-09-04).
`data/systems/*.json` becomes a set of pins rather than the source of truth. The game begins
when a wormhole opens into an unexplored region; nothing beyond has a name until somebody
goes there and names it. The architecture was already ready: the server sends a
`SystemLayout` on entry, so where that layout came from is invisible on the wire.

**How that disagreement surfaced is worth remembering.** The owner and I worked from
opposite premises for weeks without either of us noticing, because both produce the same
immediate work — a shape grammar, lighting and materials are needed either way. They diverge
on the step after. Agreement on the next task is not agreement on the direction.

## State

`main` is green on both platforms: warning-clean build, the unit suite (87 cases), all four
`econserver` self-tests, `econagent selftest` against a live server, and a two-client step
that also checks a wrong secret is refused.

**M3 "Multiplayer core" is closed** — 15 issues.

**M6 "The look"** — everything in it is done except #123 (glyphs become a sensor screen) and
#117 (colour stops meaning allegiance). Built: the gallery (#118), lighting (#119), the
screen treatment (#120), materials (#121), silhouettes as compositions (#122), per-part
lighting (#135), motion (#136).

**M9 "The scale of a system"** is the live milestone, and it comes **before M7** because the
generator will bake in whatever scale exists when it is written. Done in it: #157's standing
verbs, #161 life for the things nobody built, #165 orbiting parts.

## Next up

1. **#166 — a planet's surface should look like a surface.** Bands are rectangles and
   craters are circles of constant size wherever they sit; both read as flat shapes laid on
   a flat shape. A latitude band on a ball is a **lens**, half-width `sqrt(1 - y²)` of the
   radius — no clipping and no new maths, only the right shape. A crater near the limb
   foreshortens into an ellipse. One multiply, and it is most of what makes a sphere look
   spherical.
2. **#158 — the camera is the player's, not the ship's.** Zoom from a hull to a whole
   system, pan away and snap back, and do something sensible during warp.
3. **#159 — the scale change itself.** A million units instead of twenty-five thousand; a
   station thirty to forty-five times a ship. **Not before #157 and #158 are finished**: at
   forty times the distance, finding things by looking at them is impossible, and the scale
   change on its own makes the game worse.
4. Then **#160** warp tuned for the new distances, and the M6 leftovers (#123, #117).

Also queued: **#137** shape derived from what an object does (it supersedes the closed
#133), **#138** damage that removes parts, **#139** variation that changes a silhouette
rather than nudging it.

**M7** (#140–#147) generates the region; **M8** (#148–#153) is the in-game builder and waits
on M4's construction track (#38).

## Things to know before touching this code

- `Archetypes::Load()` must run **before** any entity is constructed. The client walked into
  this for months (#127): its own player ship was the one object with no archetype.
- Numbers both sides need live in `sim/PlayerStep.h` — `SIM_DT`, `MAX_CATCHUP_SECONDS`,
  `PLAYER_WEAPON_RANGE`. Each was two copies once, and each time they drifted it looked like
  a gameplay bug (#115).
- Every reach — docking, mining, salvage, a gate — belongs to the object being reached and
  comes from its archetype, never a constant beside the rule (#34, #17).
- `documents/world_format.md` is the contract for `data/`. Anything added to an archetype
  belongs in its table.
- **`Render::FromArchetype` is the only mapping** from a `Visual` to a `Render::Item`. A
  second mapping is how a tool starts showing a picture the game does not.
- **A backend outlives the scene it drew.** `Present` states the lighting *and* the view
  every time, including when there is none, or a preview drawn beside a system view is lit
  by that system's star. Anything calling `IBackend::Draw` directly inherits the last ones.
- **`Render::Compose` takes a `Pose`**, not positional arguments. The list grew three times
  before it became a struct; add to the struct.
- **Nothing in a composition may be measured in raw world units.** Truss rails were `1.5f`
  and went sub-pixel the moment a card framed a larger object, so the trusses stopped being
  drawn at all. Every measurement comes from the part's own size.
- **Anything that moves is `f(time, seed)`**, never integrated per frame — a part whose
  angle accumulated would drift between clients, and two players would see the same station
  turned differently. The turn is wrapped, because a float counting degrees for a week of
  uptime has no precision left.
- **A part belongs either to the body or to the space around it.** A surface feature turns
  with the surface; an orbiting part goes round and passes behind. Rotating a *latitude*
  feature — a band, a polar cap — turns it into a stripe sweeping across the planet.
  Orbiting is draw order and not occlusion, so keep an orbit outside the body.
- **A material is bound per part, not per object.** One sphere at the centre is the truth
  about a planet and a lie about a station. An elongated part is lit as a cylinder from
  `item.axis`; below a few pixels a part is drawn flat, because a rail two pixels wide is
  *entirely* the part of a cylinder that turns away from the viewer.
- **raylib culls a clockwise triangle.** `DrawTriangle` wants its vertices counter-clockwise
  *in screen space*, where y points down. Wound the other way the part is simply not there.
- **A rim highlight needs a round silhouette.** On a triangle it is a crescent floating in
  empty space.
- **A hold ends only when the ship is told to do something else** (#157). Orbit and
  keep-at-range are standing; manual control, an autopilot order and a warp all release
  them. Anything new that steers the ship must release the hold, or it will fight one.
- **A standing hold is the one place prediction tolerates a difference.** The server
  resolves the hold target from the live entity, the client from its interpolated proxy, so
  they differ by the render delay. Deliberate, and only for a slow control loop aiming at a
  ring hundreds of units across. Nothing that decides a hit is ever computed from an
  interpolated position.
- **A shader failing is a runtime fact, not a build error.** Whether one compiles depends on
  the player's driver. The pass or the material is dropped, the reason is shown in the
  settings screen as well as in the log, and the game keeps playing.
- Two archetype switches are deliberately total, with no `default:` — the layout builder in
  `Simulation_Snapshot.cpp` and the context menu in `Game.cpp`.

## How this milestone was actually worked

**Every visual defect was found by looking at a screenshot, never by reading code.** raylib's
scissor not nesting, a star shaded by its own light, a slider delivering a third of what it
said, ships culled for clockwise winding, trusses gone sub-pixel, a rail coming out as the
darkest thing on screen, a gas giant with a rod through it. Run it and look at it.

**Each piece split into a data half and a GPU half, and the data half is tested even though
the picture cannot be.** `TreatmentConfig` apart from `Treatment`, `Material` apart from
`MaterialLibrary`, composition geometry apart from drawing. Tests in that half caught real
defects — an `Extent` that counted a `radius` a capsule never reads, among others.

## Environment

- `cmake` and `clang-format` are not on `PATH`. Prepend `C:\msys64\mingw64\bin`. **Each
  PowerShell call is a fresh shell**: set `$env:PATH` in the same call that needs it, or a
  launched executable dies on a missing DLL.
- `gh` is not on `PATH`. Prepend `C:\Program Files\GitHub CLI`.
- Close `econserver` / `econspace` / `econagent` / `worldeditor` before rebuilding — Windows
  will not let the linker overwrite a running executable.
- Never run `gh run watch` or anything else that blocks; poll once with `gh pr checks <n>`.
  `Analyze C++` (CodeQL) takes about twelve minutes, so a PR is not mergeable before then.
- Heredocs mangle `\n` and `\"` inside inline Python. Write scripts to the scratchpad with
  the Write tool and run them by path.
- **Do not stack pull requests.** Merging a base with `--delete-branch` closes the one
  stacked on it, and GitHub will neither reopen nor retarget a closed PR (#85 → #86).
  Parallel branches off `main` are fine. Watch for the quieter version: branching off `main`
  for work that *depends* on an unmerged PR — it compiles against the wrong base and fails
  confusingly.
- **Screenshots are how the visual work is checked.** `worldeditor gallery shapes` opens
  straight into the gallery on the shape backend; **F2** switches backends, **F3** opens the
  gallery, **F10** opens the screen-treatment settings. Synthetic input (`SendKeys`,
  `mouse_event`) does **not** reach a raylib window, which is why those command-line
  arguments exist. The owner works in VS Code, where file cards do not render — open a
  screenshot with `Start-Process` rather than only sending it.
