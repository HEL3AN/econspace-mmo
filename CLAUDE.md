# CLAUDE.md

Operating context for AI assistants working in this repository. Read
[DECISIONS.md](DECISIONS.md) for the reasoning behind the premises below and
[ARCHITECTURE.md](ARCHITECTURE.md) for how the code fits together.

## What this is

EconSpace is a 2D EVE-like space-sim **MMO** in C++17 on top of raylib. The server owns
the world; the client renders snapshots and sends commands.

## Settled premises — do not re-litigate these

These are decisions, not open questions. Plan on top of them.

- **This is an MMO. Single-player is not a mode.** The client always connects to an
  authoritative server. The single-player path still in the code is residue from an
  earlier stage and is being deleted (#23) — never treat "works offline too" as a
  constraint, and never propose designs that preserve offline parity. `LocalTransport`
  survives only as a *test* transport.
- **Glyph (ASCII) rendering is the primary look** (#36). Sprites are an optional
  alternative backend. Whether the windowed HUD survives alongside it is genuinely open.
- **AI agents are first-class players** (#42). The game ships its own MCP server,
  `econagent`, written in C++ so the wire protocol stays a single source of truth.
- **The world becomes player-mutable** (#44), and player structures feed the existing
  macro simulation.

## Build and run

```sh
cmake -S . -B build -G "MinGW Makefiles"   # first build fetches and builds raylib + nlohmann/json
cmake --build build
ctest --test-dir build --output-on-failure

./build/bin/server/econserver.exe host 50800        # authoritative server
./build/bin/game/econspace.exe connect 127.0.0.1 50800
./build/bin/editor/worldeditor.exe

./build/bin/server/econserver.exe hosttest          # server loop smoke test
./build/bin/server/econserver.exe accttest          # account persistence smoke test
```

Windows/MinGW only for now — the transport is winsock and `ws2_32` is linked
unconditionally (#12). Close a running executable before rebuilding; Windows will not let
you overwrite it.

## Structure and rules

Three CMake targets: **`engine`** (static library — world, entities, factions, UI,
render), **`game`** (the client, the authoritative `Simulation`, and `econserver`), and
**`editor`**. `game` and `editor` link `engine`; **`engine` must never depend on either.**

- Authority lives in `Simulation` (`src/game/sim/`), on a fixed `1/60` tick. The client
  never mutates authoritative state.
- The world is data: `data/universe.json`, `data/systems/*.json`, `data/factions.json`.
  The editor writes exactly the format the runtime reads — keep it that way.
- Follow [CONVENTIONS.md](CONVENTIONS.md): types and methods `PascalCase`, class fields
  `camelCase_`, formatting per `.clang-format`.
- Everything in the repository — code, comments, docs, commit messages — is in **English**.
- The build runs `-Wall -Wextra` and stays warning-clean for our code. Keep it that way.
- Add new `.cpp` files to the correct CMake target.

## Things that will trip you up

- **`Protocol.cpp` and `Tcp.cpp` are compiled three times over** — listed as sources in
  `econspace`, `econserver` and `tests` separately. Being extracted into a `netproto`
  library (#25); until then, a change to those source lists means editing three places.
- **The protocol has no version field**, despite the docs calling it "versioned JSON".
  Every `Decode*` uses `value(key, default)`, so a mismatch silently yields defaults
  rather than an error (#15).
- **`SystemLayout` is sent once**, when a client enters a system. Anything that changes
  the static world mid-session is invisible until re-entry (#38).
- **Comments referencing "M4f", "L2", "M0"** are historical milestone markers from the
  living-galaxy and netcode tracks. They describe *when* something was built, not what is
  planned.
- **Some code is dead scaffolding and says so** — `TickCold`, `StepAggregate`, `StepMacro`
  (note: the live method is `StepWorldMacro`, an easy trap), `DehydrateActive`. Being
  removed in #18.

## Planning

Work is organized into milestones M0–M5 and three track epics: **#42** agents/MCP,
**#43** data-driven world and glyph presentation, **#44** the player-mutable world.
Check the milestone an issue belongs to before proposing sequencing.
