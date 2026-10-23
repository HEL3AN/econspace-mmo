# Start here — 2026-10-23

The first thing to read on a new machine or in a new session. It says how to get a working
build, how work is done here, where it stands and what comes next. Keep it current.

Read next, in this order: [CLAUDE.md](CLAUDE.md) (the settled premises and the rules that
trip people up), [ROADMAP.md](ROADMAP.md) (done and next), [DECISIONS.md](DECISIONS.md)
(why), [ARCHITECTURE.md](ARCHITECTURE.md) (how the code fits). Design documents for work in
progress are in [documents/](documents/).

## Toolchain (Windows)

- **MSYS2 with mingw64**, installed at `C:/msys64`. `g++`, `mingw32-make`, `cmake` and
  `clang-format` are all in `C:/msys64/mingw64/bin`, which is **not** on `PATH` by default.
  Prepend it in the same call that needs it:
  - bash: `export PATH=/c/msys64/mingw64/bin:$PATH`
  - PowerShell: `$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"`. Each PowerShell call is a
    fresh shell; without the `PATH` a launched executable dies on a missing DLL.
- **Python 3** (`python`), for `tools/gen_agent_reference.py` and the example bot.
- **GitHub CLI** (`gh`), logged in to the repository `HEL3AN/econspace-mmo`. If it is not on
  `PATH`, it is in `C:\Program Files\GitHub CLI`. Wrap calls in `timeout 90`; the network can
  hang.
- Linux works too (GCC; raylib's X11/OpenGL headers, see [CONTRIBUTING.md](CONTRIBUTING.md)).
  CI builds Windows, Linux, Linux under ASan and UBSan, and macOS.

## Builds and tests

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug     # development, tests, screenshots
cmake --build build -j 8
ctest --test-dir build --output-on-failure
./build/bin/server/econserver.exe hosttest   # and accttest, worldtest, ordertest, opstest

cmake -S . -B build-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release   # playtests, releases, every FPS number
cmake --build build-release -j 8
```

- The first configure fetches raylib, nlohmann/json, doctest, PicoSHA2 and Clay.
- CI builds with `-DWARNINGS_AS_ERRORS=ON` and checks formatting with `clang-format`. Run
  `clang-format -i` on what you touched.
- After changing an econagent tool, regenerate the reference:
  `python tools/gen_agent_reference.py ./build/bin/agent/econagent.exe` (CI runs it with
  `--check`).
- A change to the generator's output needs a `Gen::GENERATOR_VERSION` bump and new golden
  hashes in `tests/gen_tests.cpp`.
- Close `econserver`, `econspace`, `econagent` and `worldeditor` before rebuilding: Windows
  will not let the linker overwrite a running executable. Kill a process only by its PID.
- Every command-line mode and flag is listed in [CLAUDE.md](CLAUDE.md#build-and-run).

## How work is done here

- **One pull request per issue, branched off `main`.** The author merges it with a squash
  once CI is green (`gh pr merge --squash --delete-branch`). Poll CI with
  `gh pr checks <n>` once in a while; never `gh run watch` or anything else that blocks.
  CodeQL runs once a day on `main`, not on pull requests. A change that touches only
  documentation (`*.md`, `documents/`, `media/`) skips the build and test jobs; they report
  as skipped, which branch protection accepts (#475).
- **Do not stack pull requests.** Merging a base with `--delete-branch` closes the one
  stacked on it, and GitHub will neither reopen nor retarget it. Do not branch off `main`
  for work that depends on an unmerged PR, either.
- **Owner decisions are logged as comments on #186.** Read the latest ones before planning.
  A call the agent makes where a design is silent goes there too, for review.
- **The `proposal` label means: wait for the owner.** Nothing labelled `proposal` is built
  until the owner says yes. Releases and tags are the owner's decision.
- **Extend, never bolt on** (CLAUDE.md): a feature grows what exists, and a PR that adds a
  new store, type, window or message says why nothing existing could carry it.
- Several agents often work at once, each in its own worktree under `.claude/worktrees/`.
  Do not clean up worktrees while agents are running.
- **Never commit saves or accounts.** `world.json`, `regions/`, `accounts/`,
  `account_*.json`, `corps.json`, `designs.json`, `allowlist.txt`, `bans.txt`, the `.bakN`
  copies and `documents/private/` stay out of the repository. Only some of them are in
  `.gitignore`, so check `git status` before every commit and add files by name.
- Everything in the repository is in English.

## Where it stands

- **0.1 Anchor** was tagged on 2026-10-17 (GitHub Releases and itch.io). `main` has moved far
  past it: `PROTO_VERSION` 67, `Save::WORLD_VERSION` 18, `Save::ACCOUNT_VERSION` 9,
  `Gen::GENERATOR_VERSION` 10, `Gen::SKELETON_VERSION` 1. Clients of 0.1 cannot join, and so
  much of the world has changed that **the next playtest starts on a fresh world**.
- **World generation v2** (#342) slices S1-S9 are built; S10 (scale to 10,000) is not.
- **Knowledge, territory and law** (#431) slices 1-16 are built, including the slice 12
  follow-ups (patrols weighed by sieges, replacements bought, offline members counted) and
  chart sales to factions (slice 16, with #346).
- **One combat model** for NPCs and players (#463); siege damage from the warship designs
  (#458).
- Structure ownership the EVE way (#468): what a member builds is the corporation's, one verb
  hands a structure over with its licence, and a disbanded corporation's are abandoned.
- Player outposts with storage, refunds and corporation docking (#433, #438); routes and
  shipyards ported from the playtester fork (#432, ideas rebuilt rather than merged).

## Next

In order; the details are in [ROADMAP.md](ROADMAP.md#next).

1. #464, NPC fits from the living world: slices 1-4 built (variants, fleets, region style,
   buying); next slice 5, refit and drift ([the design](documents/npc_fits_living_world.md)).
2. #466, a corporation's enemy list, with war declarations.
3. #425-#427, the 0.1 playtest polish.
4. A playtest on a fresh world, then 0.2 if the owner decides to tag it.

## Things to know before touching this code

The rules that have bitten more than once are in CLAUDE.md under "Things that will trip you
up". A few more, from the work on the look:

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
  by that system's star.
- **`Render::Compose` takes a `Pose`**, not positional arguments. Add to the struct.
- **Nothing in a composition may be measured in raw world units.** Every measurement comes
  from the part's own size, or it goes sub-pixel the moment a card frames a larger object.
- **A part belongs either to the body or to the space around it.** A surface feature turns
  with the surface; an orbiting part goes round and passes behind. Orbiting is draw order,
  not occlusion, so keep an orbit outside the body.
- **raylib culls a clockwise triangle.** `DrawTriangle` wants its vertices counter-clockwise
  *in screen space*, where y points down.
- **A hold ends only when the ship is told to do something else** (#157). Anything new that
  steers the ship must release the hold, or it will fight one.
- Two archetype switches are deliberately total, with no `default:` — the layout builder in
  `Simulation_Snapshot.cpp` and the context menu in `Game.cpp`.

## Seeing a frame

**Every visual defect so far was found by looking at a screenshot, never by reading code.**
Synthetic input (`SendKeys`, `mouse_event`) does not reach a raylib window, so the game and
the editor take their view from arguments and save it themselves: `--shot f.png --frames N`
on the client, `shot out.png` in the editor, `--click X Y FRAME` for a click (examples in
CLAUDE.md). Screenshot runs open their window hidden. For a built surface (#516) a gallery
shot also takes `light A` (radians: put the night side in view), `livery ID`, `heading H`,
`damage D`, `fleet N` (N of the card at once, for `perf`) and `budget MP` (the paint budget in
place of look.json's). The owner works in VS Code, where file
cards do not render — open a screenshot with `Start-Process` rather than only sending it.

The owner plays on the same laptop the agents run on (Intel UHD 620): performance numbers
measured there are real, and a playtest needs a Release build and no heavy agent work.
