# Session handoff — 2026-09-03

Where the work stands and what to pick up. Keep this current; it is the first thing to read.

## Direction, settled 2026-09-04

**The world is generated; hand-written content is the exception on top.** The game begins
when a wormhole opens into an unexplored region, and nothing beyond has a name until
somebody goes there. Read the 2026-09-04 entry in `DECISIONS.md` before planning anything
that touches the world — it inverts the premise the repository was built on, and the
architecture already supports it: the server sends a `SystemLayout` on entry, so where that
layout came from is invisible on the wire.

Two milestones carry it: **M7** (#140–#147) generates the region, **M8** (#148–#153) lets a
player design a type and build it. M8 waits on M4's construction track.

## State

Nothing is in flight. `main` is green on both platforms: warning-clean build, unit suite,
all four `econserver` self-tests, `econagent selftest` against a live server, and a
two-client step that also checks a wrong secret is refused.

**M6 is five issues in.** The gallery (#118), the lighting (#119), the screen treatment
(#120), materials (#121) and silhouettes (#122) are done; the milestone resumes at
**#123, demoting glyphs to a sensor screen**, then **#117** colour semantics. **#133**
(a composition for an object nobody drew) is the remaining piece of #122. Everything M6
touches is judged by looking, so start by looking: `worldeditor gallery shapes` puts the
whole registry on one screen under the current light and the current treatment, and F10
opens the treatment's settings.

**M3 "Multiplayer core" is closed** — 15 issues. Each connection has its own
`ClientSession` (#3); players see each other in a system (#4); an account remembers where
it was, what it carried, which missions it took and which ships it owns (#49, #5), refuses
a save from a newer build (#20), has a secret it proves with a nonce rather than sending
(#106), and can only be played by one connection at a time (#105). The transport stopped
believing its peer (#14), a snapshot costs a tenth of what it did (#16, #97), and the
server builds and plays on Linux as well as Windows (#12).

## Next up: M6 — The look

The visual direction was settled on 2026-09-03 after playing the build. Glyphs are no
longer the primary look; generated art replaces them. `DECISIONS.md` has the reasoning and
what it costs. Work in this order — the order is part of the decision:

1. ~~**#118 gallery**~~ — **done**. `worldeditor gallery` (or F3 from any view) shows every
   archetype at once, drawn through the same backend the game uses, with the state that
   changes a look — intensity, heading, thrusting — on sliders, and the archetype's own
   look fields editable beside it. Ctrl+S writes them back into `data/archetypes.json`
   **in place**: `ArchetypeEdit::SetField` replaces one value in the file text and leaves
   every other byte alone, because a parse-edit-dump turns a two-character change into a
   two-hundred-line diff. That part *is* unit-tested even though the picture is not.
2. ~~**#119 lighting**~~ — **done**. A light is an archetype field (`"light": { "radius",
   "intensity" }`), not a test for `EntityKind::Star`: two-star systems work today and a
   beacon a player builds becomes a light with no renderer change. `Render::Lighting`
   holds the list and answers "what reaches this point"; falloff goes to **zero** at the
   radius rather than inverse-square, so a star across the system cannot decide which way
   something near you is lit. No lights means *unlit* — full colour — not black. The
   gallery tunes it against a synthetic source and saves the star's reach back to data.
3. ~~**#120 screen treatment**~~ — **done**. The world is drawn into a texture and put
   through an ordered chain of full-screen passes from `data/look.json`: bloom, pixelate,
   scanlines, noise, fringe, vignette. **F10** opens its settings in the game *and* in the
   gallery — one panel, drawn by both — and closing them writes the file. The order is
   reorderable because it is a decision: bloom before pixelation gives soft fat pixels,
   after it gives hard pixel edges that glow.

   The ambient floor from #119 still has no home in that panel; it is a look decision that
   is still a compiled default. Worth folding in when something else touches the settings.

   Note what M6 has cost so far and keep paying it: **every number in the look was set by
   eye, not derived** — the ambient floor, how far a star reaches, how hard the terminator
   ramps, every `amount` and `scale` in the chain. They are the owner's call. Anything new
   that is tuned this way belongs in a panel and in a data file it can write, not in a
   constant.
4. ~~**#121 materials**~~ — **done**. An archetype names a material; a material is a
   shader plus bindings from `data/materials.json`, each mapping a uniform to a source
   (`item.intensity`, `light.dir`, `clock.time`, or a constant). The `hull` shader gives a
   sphere normal, a curved terminator, a rim and damage that reads without a health bar --
   which is what lets the silhouettes underneath stay simple enough for #122.

   Watch the coordinate flip in `ShapeBackend::BeginMaterial`: `gl_FragCoord` counts y from
   the bottom and everything else counts it from the top, so **both** the screen position
   and the light direction are flipped there, together. Flipping one without the other
   lights an object from the mirror image of where its star is.
5. ~~**#122 silhouettes**~~ — **done for the shipped archetypes.** Thirteen of the
   eighteen are now compositions written in `archetypes.json` -- seven primitives, roles,
   rotational `repeat` and bilateral `mirror`, detail by `minPixels`, variation seeded
   from the object's id. Stars and regions deliberately keep their figure: a star is a
   light rather than a structure.

   **`EntityKind::Unknown` is still a circle** -- deriving a composition for an object
   nobody wrote one for is **#133**, split out so one pull request did not do two things.
6. **#123** — glyphs become a sensor screen over `GridBackend`, which already does that
   correctly and is used by nothing.
7. **#117** — colour in the world stops meaning allegiance; allegiance moves to the
   instruments, where it is per-observer and therefore correct.

**No GPU in CI.** None of M6 is covered by a test. A shader that will not compile is a
runtime, driver-specific fact: log it loudly, fall back to drawing without the chain, keep
playing. The server and the unit tests must never need a shader.

## Also open

- **#34** — the last of M2, deliberately staged. `data/systems/*.json` still groups objects
  by category instead of naming an archetype id, which is what a player-built object will
  need in order to be written to a file at all (#39). Its progress comment lists the rest.
- **M4 construction** — #38 (world mutation and `LayoutDelta`) first; #97 made it
  load-bearing, because a layout-backed object can no longer disappear on a client.
- **M5** — #109 among others: an agent still cannot take a mission or buy a ship.

## Things to know before touching this code

- `Archetypes::Load()` must run **before** any entity is constructed. Entity constructors
  look themselves up in the registry, so one built earlier has no components and no glyph —
  silently undockable and invisible rather than failing.
- Numbers that both sides need live in `sim/PlayerStep.h` — `SIM_DT`,
  `MAX_CATCHUP_SECONDS`, `PLAYER_WEAPON_RANGE`. Each of them was two copies once, and each
  time the copies drifted it looked like a gameplay bug (#115).
- Every reach — docking, mining, salvage, a gate — belongs to the object being reached and
  comes from its archetype, never a constant beside the rule (#34, #17).
- Two archetype switches are deliberately total, with no `default:` — the layout builder in
  `Simulation_Snapshot.cpp` and the context menu in `Game.cpp`.
- `documents/world_format.md` is the contract for `data/`. Anything added to an archetype
  belongs in its table.
- `Render::FromArchetype` is the only mapping from an archetype's `Visual` to a
  `Render::Item`; `Entity::Describe()` goes through it and so does the gallery. A second
  mapping is how a tool starts showing a picture the game does not.
- **A backend outlives the scene it drew.** `Present` states the lighting every time,
  including when there is none, because otherwise a preview drawn beside a system view is
  lit by that system's star. Anything calling `IBackend::Draw` directly inherits the last
  lights set — the palette icon and the gallery card go through `Present` for that reason.
- **A shader that will not compile is a property of the machine, not of the build.** There
  is no GPU on a CI runner, so `Render::Treatment` is untestable by construction — which is
  why `TreatmentConfig` is a separate file holding everything that decides what the picture
  will be. That half is tested; the picture is not.
- **`Render::Compose` takes a `Pose`**, not eight positional arguments. The list grew twice
  before it became a struct; add to the struct rather than to the signature.
- **Nothing in a composition may be measured in raw world units.** Truss rails were `1.5f`
  and went sub-pixel the moment a card framed a larger object, so the trusses stopped being
  drawn at all. Every measurement comes from the part's own size.
- **raylib culls a clockwise triangle.** `DrawTriangle` wants its vertices
  counter-clockwise *in screen space*, where y points down. Wound the other way the part
  is simply not there, which is what every ship looked like the first time a chevron was
  drawn.
- A rim highlight needs a **round** silhouette. Traced at the radius of a triangle it is a
  crescent floating in empty space beside the ship; that is what it looked like the first
  time. Shapes that are not discs get theirs with real silhouettes (#122).

## Environment

- `cmake` and `clang-format` are not on `PATH`. Prepend `C:\msys64\mingw64\bin`.
- `gh` is not on `PATH`. Prepend `C:\Program Files\GitHub CLI`.
- Close `econserver` / `econspace` / `econagent` / `worldeditor` before rebuilding —
  Windows will not let the linker overwrite a running executable.
- Never run `gh run watch` or anything else that blocks; poll once with `gh pr checks <n>`.
  `Analyze C++` (CodeQL) takes about eleven minutes, so a PR is not mergeable before then.
- Heredocs mangle `\n` and `\"` inside inline Python. Write scripts to the scratchpad with
  the Write tool and run them by path.
- **Do not stack pull requests.** Merging a base with `--delete-branch` closes the one
  stacked on it, and GitHub will neither reopen nor retarget a closed PR (#85 → #86).
  Parallel branches off `main` are fine; branches off an unmerged branch are not.
