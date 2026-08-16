# Session handoff — 2026-08-16

Where the work stopped and what to pick up next. Delete this file once M2 is closed and
the next milestone is under way.

## State

Nothing is in flight. `main` is green: warning-clean build, 32 doctest cases / 525
assertions, all four `econserver` self-tests, and `econagent selftest` against a live
server.

**M2 "Glyph world" is done bar two issues.** Merged this session: **#81** (entity kind
tag), **#82 / #19** (all RTTI dispatch gone), **#83** (archetype registry), **#84 / #35**
(presentation layer), **#86 / #37** (editor on the same layer, palette generated from the
registry), **#87 / #36** (glyphs as the game's look).

## Next up

- **#17 — split `Simulation.cpp` and `Editor.cpp`.** The only mechanical M2 issue left.
  Both grew again this session. Use the method from the `Game` split (#11): separate
  translation units of one class, and diff the function inventory before and after — that
  check caught an over-deletion last time.
- **#34 is deliberately still open**, and the progress comment on the issue says exactly
  what remains. The short version: `data/systems/*.json` still groups objects by category
  instead of naming an archetype id; the mining, market, salvage and jump passes still
  narrow to a class; the hierarchy is deleted last, not first.
- Then **M3 Multiplayer core**, starting with **#3** (multi-client). That is the
  foundational one — the server still accepts a single client at a time.

## Things to know before touching this code

- `Archetypes::Load()` must run **before** any entity is constructed. Entity constructors
  look themselves up in the registry, so an entity built earlier has no components and no
  glyph — silently undockable and invisible rather than failing.
- The `world` block in `data/archetypes.json` is a transitional bridge to the world format
  and is documented as such. It disappears when system files name archetypes by id.
- Two archetype switches are deliberately total, with no `default:` — the layout builder in
  `Simulation.cpp` and the context menu in `Game.cpp`. Omitting a kind there is an entity
  the client never draws, or a right-click menu with nothing in it.
- `documents/world_format.md` is the contract for `data/`. Anything added to an archetype
  belongs in its table.

## Environment

- `cmake` and `clang-format` are not on `PATH`. Prepend `C:\msys64\mingw64\bin`.
- `gh` is not on `PATH`. Prepend `C:\Program Files\GitHub CLI`.
- Close `econserver` / `econspace` / `econagent` / `worldeditor` before rebuilding —
  Windows will not let the linker overwrite a running executable.
- Never run `gh run watch` or anything else that blocks in the foreground; poll once with
  `gh pr checks <n>`. `Analyze C++` (CodeQL) takes about ten minutes, so a PR is not
  mergeable for at least that long.
- PowerShell's `Set-Content` writes CRLF. Normalise anything written that way, or git
  reports the whole file as changed.
- **Do not stack pull requests.** Merging a base PR with `--delete-branch` closes the one
  stacked on it, and GitHub will neither reopen nor retarget a closed PR — it has to be
  recreated from scratch, losing its number and its description. That happened this
  session (#85 → #86). Wait for CI on a PR before branching the next piece of work off it.
