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
  authoritative server; it cannot even be constructed without a live connection. Never
  treat "works offline too" as a constraint, and never propose designs that preserve
  offline parity. `LocalTransport` survives only as a *test* transport.
- **The look is generated, not drawn** (M6). Objects are lit by the system's own stars,
  shaded by a material, and built from silhouettes described in data, under a screen
  treatment that can be tuned and switched off in game. A hand-made sprite wins where one
  exists; the generator covers everything else, which is what a world players can build
  needs. **Glyphs are no longer the primary look** (#123): they become a sensor screen over
  a fixed grid, which is the one thing they are genuinely good at.
- **Colour in the world view is an art decision, not a faction tag** (#117). Allegiance
  depends on who is looking — the same station is a friend to one player and a target to
  another — so it belongs to the instruments (radar, overview, target panel, sensor view),
  never to the object.
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
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2   # account + secret
./build/bin/editor/worldeditor.exe
./build/bin/editor/worldeditor.exe gallery           # every archetype at once, for tuning a look (F3)
./build/bin/editor/worldeditor.exe gallery shapes    # ...on the shape backend (F2 switches; F10 = screen treatment)

./build/bin/agent/econagent.exe connect 127.0.0.1 50800 agent hunter2  # MCP for an agent

./build/bin/server/econserver.exe hosttest          # server loop smoke test
./build/bin/server/econserver.exe accttest          # account persistence smoke test
./build/bin/server/econserver.exe worldtest         # galaxy persistence + clock
./build/bin/server/econserver.exe ordertest         # standing orders, routes, journal
```

Windows/MinGW and Linux/GCC, both built by CI (#12). The transport picks winsock or
Berkeley sockets at compile time; `ws2_32` is linked only on Windows. On Windows, close a
running executable before rebuilding — it will not let you overwrite it. On Linux the
binaries have no `.exe` suffix.

## Structure and rules

CMake targets: **`engine`** (static library — world, entities, factions, UI, render),
**`netproto`** (the wire protocol and transport, compiled once and linked by everything
that speaks it), the client **`econspace`**, the server **`econserver`**, the MCP bridge
**`econagent`**, and **`worldeditor`**. **`engine` must never depend on any of them.**

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

- **`Sim::StepPlayerShip` (`sim/PlayerStep.h`) has two callers on purpose** — the server
  applies it authoritatively, the client applies it to predict its own ship and to replay
  unacknowledged inputs. Changing it changes both sides at once, which is the point:
  prediction breaks the moment they compute different results from the same input. The
  client does not link `Simulation` at all.
- **`econagent` owns stdout.** It is the JSON-RPC channel; one stray `printf` or raylib
  trace on it corrupts the stream and the client reports a parse error rather than the
  line that caused it. All diagnostics go to stderr, and raylib's logger is redirected
  there explicitly — this bit on the first end-to-end run.
- **Bump `PROTO_VERSION` when a message changes meaning.** Decoding is deliberately
  permissive per field, so without a version bump an older peer silently reads defaults
  instead of failing.
- **`SystemLayout` is sent once**, when a client enters a system. Anything that changes
  the static world mid-session is invisible until re-entry (#38).
- **Saves carry a schema version and refuse a newer one** (#20). Field-by-field defaults
  are right for a message from a peer and wrong for a save: a file from a later build would
  load as a plausible-looking wrong account and then be written back over the real one. A
  world file from the future stops the server; an account file from the future lets the
  player in with a fresh account and is never saved over. Bump `Save::WORLD_VERSION` /
  `Save::ACCOUNT_VERSION` when a field changes meaning — adding one an older reader can
  ignore does not need a bump.
- **A login is a challenge and an answer, not a name** (#106). The server sends a nonce
  and the salt; the client replies with `H(nonce ‖ H(salt ‖ secret))`, so the secret itself
  never crosses the wire after the account is created. There is no transport encryption, so
  that is the only thing protected — and a wrong answer ends the connection, which is what
  keeps guessing expensive.
- **One account, one session** (#105). A second `Hello` for a name that is already playing
  displaces the earlier connection: its account is saved first, then it is told why with a
  `Bye` and dropped. `Bye` is the only way the server can end something on purpose and say
  so — dropping the socket is indistinguishable from a crash — so a future refusal (#106)
  belongs on that message too.
- **The transport enforces its own limits** (#14). A frame length is four bytes the peer
  chose, so `TcpConnection` caps it and drops the connection rather than buffering; the
  send backlog is capped the same way. The host grants each client a budget of player
  ticks, because one command is one tick of movement and a client that sends faster than
  the simulation runs would simply move faster than everyone else.
- **Per-player state lives in `ClientSession`, not in `Simulation`** (#3). The ship, the
  account, the missions, the standing order and the event journal belong to a session, and
  every player verb takes the session it acts for. There is no "active system" either: a
  session carries the system it is in, and every system is stepped the same way. Anything
  reintroduced as a member of `Simulation` is shared by every player on the server.
- **A shader failing is a runtime fact, not a build error** (#120). Whether one compiles
  depends on the player's driver. `Render::Treatment` drops the pass, says why in the
  settings screen as well as the log, and keeps playing; with no passes at all it draws
  straight to the screen. Never make the game require a shader — the server and the tests
  load none, and there is no GPU on a CI runner, so nothing about the chain is covered by
  a test except `TreatmentConfig`, which is why that half is a separate file.
- **A light is a property of an object, not of a system** (#119). Anything whose archetype
  has a `light` lights the system it is in, so two stars work and a beacon a player builds
  needs no renderer change. The falloff reaches **zero** at the radius on purpose — an
  inverse square never does, and a star on the far side of a system would then decide which
  way something near you is lit. An empty light list means *unlit*, which is full colour,
  not black; every caller that draws straight into a backend must state its own lighting,
  because a backend keeps the last one it was given.
- **Entities no longer draw themselves.** `Entity::Draw()` is gone; an entity returns a
  `Render::Item` from `Describe()` and a backend draws it (#35). Adding a shape means
  editing a backend, not a class. What an object *looks like* (glyph, sprite, colour,
  layer) and what it *can do* (components) both come from `data/archetypes.json` — a new
  kind of object needs no new C++ at all.
- **Load `Archetypes` before building a world.** Entity constructors look themselves up
  in the registry, so an entity built before the load has no components and no glyph —
  it would silently be undockable and invisible rather than fail.
- **Comments referencing "M4f", "L2", "M0"** are historical milestone markers from the
  living-galaxy and netcode tracks. They describe *when* something was built, not what is
  planned.

## Planning

Work is organized into milestones M0–M5 and three track epics: **#42** agents/MCP,
**#43** data-driven world and glyph presentation, **#44** the player-mutable world.
Check the milestone an issue belongs to before proposing sequencing.
