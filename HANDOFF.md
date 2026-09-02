# Session handoff — 2026-09-03

Where the work stands and what to pick up. Keep this current; it is the first thing to read.

## State

Nothing is in flight. `main` is green on both platforms: warning-clean build, unit suite,
all four `econserver` self-tests, `econagent selftest` against a live server, and a
two-client step that also checks a wrong secret is refused.

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
3. **#120 screen treatment** — bloom, pixels, scanlines, noise; tunable and switchable in
   game, separately for the HUD. Before silhouettes on purpose: it changes the most for the
   least, on the shapes that already exist.
4. **#121 materials** — a shader per object fed by `Render::Item` state (`intensity`,
   `heading`, `thrusting`) and the scene's lights.
5. **#122 silhouettes** — shapes from `archetypes.json`, replacing `ShapeBackend`'s switch
   on `EntityKind`, whose `Unknown` case is why a player-invented object is a circle.
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
