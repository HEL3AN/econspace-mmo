# Session handoff — 2026-08-16

Where the work stopped and what to do first on resuming. Delete this file once the open
PRs below are merged and the next milestone is under way.

## The one open pull request

**#87 — "Make glyphs the game's look"** (`feat/36-glyphs-primary`, closes #36). Based on
`main`, rebased and verified after #86 merged. Merge it when CI is green, then delete both
`feat/36-glyphs-primary` and the now-merged `feat/37-editor-archetypes`.

Verified locally on the rebased tip: warning-clean build, 32 doctest cases / 525
assertions, all four `econserver` self-tests, and `econagent selftest` against a live
server.

> A hazard worth remembering: two PRs were stacked, and merging the base with
> `--delete-branch` closed the one on top of it. GitHub will not reopen or retarget a
> closed PR, so it has to be recreated from scratch — #85 became #86 that way. Merge a
> base PR with `--squash` alone, retarget and rebase the stacked one, and only then delete
> the branch.

## What M2 "Glyph world" delivered

Merged this session: **#81** (entity kind tag), **#82 / #19** (all RTTI dispatch gone),
**#83** (archetype registry), **#84 / #35** (presentation layer), **#86 / #37** (editor on
the same layer, palette from the registry). In flight: **#87 / #36**.

After #87 merges, M2 is complete except for the two issues below.

## Next up

- **#17 — split `Simulation.cpp` and `Editor.cpp`.** The only M2 issue left. Both grew
  again this session. Use the same method as the `Game` split (#11): separate translation
  units of one class, and diff the function inventory before and after — that check caught
  an over-deletion last time.
- **#34 is deliberately still open.** See the progress comment on the issue for exactly
  what remains. The short version: `data/systems/*.json` still groups objects by category
  instead of naming an archetype id; mining, market, salvage and jump passes still narrow
  to a class; and the hierarchy is deleted last, not first.
- Then **M3 Multiplayer core**, starting with **#3** (multi-client). That is the
  foundational one — the server still accepts a single client.

## Things to know before touching this code

- `Archetypes::Load()` must run **before** any entity is constructed. Entity constructors
  look themselves up in the registry, so an entity built earlier has no components and no
  glyph — it is silently undockable and invisible rather than failing.
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
  `gh pr checks <n>` instead. `Analyze C++` (CodeQL) takes about ten minutes.
- PowerShell's `Set-Content` writes CRLF. Normalise JSON and source written that way, or
  git will complain and the diff will be the whole file.
