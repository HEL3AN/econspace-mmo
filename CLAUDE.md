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
- **A system is a million units across, and you travel by choosing from a list**
  (M9). `SYSTEM_RADIUS` is `1000000` (#159).
  Scale was a hostage of travel time -- one speed meant everything had to be within a
  minute of flying. Warp already exists and is server-authoritative; what was missing is
  the interaction, taken from EVE: the overview is the instrument and a selection carries
  actions. A player who never looks out of the window can still fly the ship. **A million
  and not ten** because positions are `float`: at 1e6 the gap between representable values
  is 0.06 of a unit, at 1e7 it is a whole one and a ship at the edge would jitter.
- **The world is generated, and hand-written content is the exception on top** (M7). The
  server generates from a seed and sends each client the `SystemLayout` it already
  sends, so nothing about the wire changes. The seed is part of the save, and changing the
  generator's rules is a migration.
- **Each server generates a whole world that grows** (#342, chosen 2026-10-11; design in
  `documents/world_generation_v2.md`).
  - **Shape:** no base systems are required. A seed lays out a skeleton of region slots up
    to the admin's cap. Generated civilisation centres are the starts.
  - **Growth:** frontier regions open as the "tide" rises with time, players and
    exploration. A mouth stays only if someone builds an **Anchor** on it.
  - **New regions** are alive from the first minute, with native factions, stations and
    markets.
  - **Wormholes** play three roles: Anchor gates, roaming holes and admin links.
  - **Admin content** comes as validated packs, which are woven in, and as pins.
  - **Factions** become a data registry, not an enum (#355).
  - **Until the slices land,** today's world is the old shape: three hand-written systems
    plus one generated region. Do not build new work on that shape.
- **The game begins when a wormhole opens into an unexplored region.** Nothing beyond it
  has a name until somebody goes there and names it. This is why the generation is honest
  rather than a compromise, and why exploration is an activity rather than map-filling.
  In v2 that moment repeats on every server, each time the tide opens a mouth.
- **The world becomes player-mutable** (#44), and player structures feed the existing
  macro simulation. Players design *types* in the same grammar the generator uses (M8), and
  a design's parts decide what it can do -- the rule that derives a shape from an object's
  components (#137), read in the other direction.
- **Knowledge, territory and law have a decided design, being built in slices** (#431,
  `documents/world_knowledge_and_territory.md`): per-pilot and public charts, native names,
  claims, defence without windows, PvP as a crime. None of it is current behaviour until its
  slice lands.

## Build and run

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug   # development and tests; first build fetches raylib + nlohmann/json
cmake --build build
ctest --test-dir build --output-on-failure

cmake -S . -B build-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release   # playtests, published builds, any FPS measurement
cmake --build build-release

./build/bin/server/econserver.exe host 50800        # authoritative server, this machine only
./build/bin/server/econserver.exe host 50800 --public   # ...reachable from other machines (#187): only names in allowlist.txt beside world.json get in (#400)
./build/bin/server/econserver.exe host 50800 --public --open   # ...and anyone may register (allowlist.txt alone turns the list on for a local server too)
./build/bin/server/econserver.exe host 50800 --centres 4 --random-starts  # a new world's civilisations (1-6, 3 by default); deal starts
./build/bin/server/econserver.exe host 50800 --dev      # ...and honour debug commands such as F1's money (#341)
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2   # account + secret; a new pilot chooses a start
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --start random --dock --shot d.png --frames 900  # ...or is given one, then docks
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --shot f.png --frames 120  # save frame 120, exit (--nohud: world only)
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --perf --frames 240  # CPU+GPU ms per phase to stderr (F9 in game; --notreat, --size W H; --treated puts a shot through the treatment)
./build/bin/editor/worldeditor.exe gallery shapes perf frames 200   # the same for the editor (`time T` freezes the clock, `treated` shot)
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --sensor 2000000 --shot s.png  # the sensor screen (V), 2M across
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --theme glass --blur --open settings --click 565 141 60 --shot t.png --frames 90  # a UI theme for one run; a click played into raylib's input on frame 60
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --dock --fit --shot f.png --frames 900  # the hangar's Fit tab, docked
./build/bin/game/econspace.exe connect 127.0.0.1 50800 pilot hunter2 --open scanner --scan 600 --scan 2600 --shot s.png --frames 4200  # two passes of the scanner (P), #346
./build/bin/editor/worldeditor.exe
./build/bin/editor/worldeditor.exe gallery           # every archetype at once, for tuning a look (F3)
./build/bin/editor/worldeditor.exe gallery shapes    # ...on the shape backend (F2 switches; F10 = screen treatment)
./build/bin/editor/worldeditor.exe gallery shapes settings  # ...with F10's panel open: every shader that failed, by name
./build/bin/editor/worldeditor.exe survey 1 shapes   # a centre and its frontier per seed from 1.., flagged empty/thin/twin (F4)
./build/bin/editor/worldeditor.exe gallery shapes card planet.rocky shot out.png  # save that view and exit; works with the screen off
./build/bin/editor/worldeditor.exe gallery shapes card station.trade_hub seed 3 sockets  # one object of a type, its kit sockets shown ([ ] seed, K sockets)
./build/bin/editor/worldeditor.exe region 7 system r22-2-1   # edit a generated system; Ctrl+S saves the difference as a pin in data/pins.json
./build/bin/editor/worldeditor.exe region 7 map shot r.png   # the first centre as a map, pinned systems ringed (Tab: next region; click a system to edit it)
./build/bin/editor/worldeditor.exe galaxy 7 shot g.png       # every slot of seed 7's skeleton, open or not, pins waiting on unopened ones (F5: system -> region -> galaxy)
./build/bin/editor/worldeditor.exe world build/bin/server slot 14   # a saved world: its log, its regions from their seals; slot N / system ID open one
./build/bin/editor/worldeditor.exe galaxy 7 slot 6 export ithath packs /tmp/packs   # that region as a pack (E on the region map), then opened; validate it with econserver validate
./build/bin/editor/worldeditor.exe newpack outpost           # a region pack begun from one system (editpack ID opens one); each save is read back as the server reads it
./build/bin/editor/worldeditor.exe gallery shapes card ship.player thrusting shot s.png  # ...with its engines lit

./build/bin/agent/econagent.exe connect 127.0.0.1 50800 agent hunter2  # MCP for an agent

./build/bin/server/econserver.exe hosttest          # server loop smoke test
./build/bin/server/econserver.exe accttest          # account persistence smoke test
./build/bin/server/econserver.exe worldtest         # galaxy persistence + clock
./build/bin/server/econserver.exe ordertest         # standing orders, routes, journal
./build/bin/server/econserver.exe host 50800 --max-regions 60 --max-systems 1500   # the cap (#342 S3); stdin: tide | open [region] | hole <system|pilot> [to] | holes | drift <region> | destroy <name> | pause | resume | allow/deny/ban/unban <name> | rename <system> <name|-> | access | reload | say <text> | mute <name> [min] | unmute <name>
./build/bin/server/econserver.exe regenerate 14     # with the server stopped: reseal region 14 by today's rules, if no player built there
./build/bin/server/econserver.exe validate          # admin packs in data/packs/ and data/pins.json, checked and trial-woven (#342 S7)
./build/bin/server/econserver.exe validate tests/fixtures/packs/helios   # ...one pack, or a directory of them
./build/bin/server/econserver.exe host 50800 --packs tests/fixtures/packs   # a NEW world weaves the packs there instead of data/packs/
./build/bin/server/econserver.exe 5184000 --news    # 24 h of an empty world: its news, each region as it opens and why, the tide hourly
./build/bin/server/econserver.exe 15552000 --news --players 12 --max-regions 9 --rules r.json   # 72 h with pilots, at a cap: mouths, Anchors, drift, the archive (#342 S4)
./build/bin/server/econserver.exe galaxydump 8     # the galaxy seed 8 makes -- skeleton, starts, regions -- as JSON (never saved)
./build/bin/server/econserver.exe regiondump 8     # ...one region of it, the first centre unless a slot is named
./build/bin/server/econserver.exe galaxydump 8 0 4 --cap 100  # its skeleton of slots, and the regions of slots 0 and 4 (#342)
./build/bin/server/econserver.exe macrobench 1000  # cost per tick/pass with a 1000-system region, nobody on (#295)
```

```sh
cmake --build build-release --target dist   # build-release/econspace-0.1.0-anchor-windows.zip (...-linux.tar.gz): client, server, agent, data/ in one folder
./econserver.exe --version                   # "EconSpace 0.1 Anchor (0.1.0)": project(VERSION) + ECON_CODENAMES in CMakeLists.txt (#400)
./econserver.exe host 50800 --data D:/econ/data   # any binary: data/ from elsewhere (else beside the exe, or one level up)
```

**Data is found at runtime** (#399, `core/DataDir.h`): `--data <dir>`, else `ECONSPACE_DATA`,
else the source `data/` while a non-Release binary runs from its own build tree (so the game reads
what the editor just wrote, no rebuild), else `data/` beside the executable or one level up. Every
binary logs its choice to stderr. The build copies `data/` beside each executable, and the `dist`
target installs the `dist` component (`cmake --install build-release --component dist --prefix X`)
and zips it: copy that folder anywhere and it runs. Release binaries carry no source path
(`-DECON_DEV_DATA=OFF` by default there).

**Two builds, two jobs** (owner, 2026-10-10). `build/` is Debug: development, tests, screenshots
and debugging -- asserts on, symbols in. `build-release/` is Release (`-O3`): playtests, builds
published on GitHub, and **every performance number** -- an unoptimized build is two to five times
slower and points at the wrong costs (#296). A build with no `CMAKE_BUILD_TYPE` is neither: no
optimisation and no symbols, so always name one.

Windows/MinGW and Linux/GCC, both built by CI (#12). The transport picks winsock or
Berkeley sockets at compile time; `ws2_32` is linked only on Windows. On Windows, close a
running executable before rebuilding — it will not let you overwrite it. On Linux the
binaries have no `.exe` suffix.

## Structure and rules

CMake targets: **`engine`** (static library — world, entities, factions, UI, render),
**`netproto`** (the wire protocol and transport, compiled once and linked by everything
that speaks it), the client **`econspace`**, the server **`econserver`**, the MCP bridge
**`econagent`**, and **`worldeditor`**. **`engine` must never depend on any of them.**

- **A new feature extends what is already built; it never stands beside it** (owner,
  2026-10-22). Before adding a component, find the one the feature belongs to and grow it:
  an attack notice is an entry in the pilot's journal and the corporation's record, not a
  notices store; a pilot's knowledge generalises the account's `scanned`, not a second set;
  a rule's number goes in `data/world_rules.json`; a verb is a `Command`/`CorpAct`; what a
  player sees reaches econagent's `observe` too. A PR that adds a new type, store, window or
  message says why no existing one could carry it. Weigh a design against how comparable
  games (EVE first) solve it, then fit it to this game's premises above.
- Authority lives in `Simulation` (`src/game/sim/`), on a fixed `1/60` tick. The client
  never mutates authoritative state.
- The world is data, and generated: nothing hand-written is required (#342). `data/factions.json`,
  `data/archetypes.json` and `data/pins.json` are read; the hand-written format
  (`universe.json`, `systems/*.json`) is the editor's, the test fixture's
  (`tests/fixtures/world/`) and the admin packs'. The editor writes exactly the format the
  runtime reads — keep it that way.
- Follow [CONVENTIONS.md](CONVENTIONS.md): types and methods `PascalCase`, class fields
  `camelCase_`, formatting per `.clang-format`.
- Everything in the repository — code, comments, docs, commit messages — is in **English**.
- The build runs `-Wall -Wextra` and stays warning-clean for our code. Keep it that way.
- Add new `.cpp` files to the correct CMake target.

## Things that will trip you up

- **Never keep a pointer into a returned index.** `KnownUniverse()`, `KnownUniverseFor()`,
  `Actions::For()` and their kind return a vector by value; `const auto* si = Find(sim.KnownUniverseFor(s), id)`
  points into a temporary that is gone by the next line. Copy the vector to a local first. It
  passes on MinGW and fails only under ASan or GCC with garbage values, and three PRs in one
  night (#437, #443, #453) tripped on it.
- **A standing hold is the one place prediction tolerates a difference** (#157). Orbit and
  keep-at-range follow something that *moves*, so the step is told where the target is:
  the server from the live entity, the client from its interpolated proxy. The two differ
  by the render delay, deliberately — a hold is a slow control loop aiming at a ring
  hundreds of units across and cannot see it. Nothing that decides a hit is ever computed
  from an interpolated position.
- **`Sim::StepPlayerShip` (`sim/PlayerStep.h`) has two callers on purpose** — the server
  applies it authoritatively, the client applies it to predict its own ship and to replay
  unacknowledged inputs. Changing it changes both sides at once, which is the point:
  prediction breaks the moment they compute different results from the same input. The
  client does not link `Simulation` at all.
- **A warp is decided by whoever orders it, then only flown** (#160). Its speed comes from
  the ship's own velocity and what is left of the path -- multiplication per fixed tick,
  no `exp()`, so two compilers agree -- and its bend around a star or a planet
  (`sim/WarpPath.h`) travels in the command and in the snapshot. Anything a warp needs that
  a snapshot does not carry is a prediction that will be wrong at 250 000 units a second.
- **The region is generated, and the generator must give the same answer everywhere**
  (#140). `src/engine/gen/` writes the same JSON the loaders read, from a seed saved in
  `world.json`. So: randomness only from
  `Gen::Rng` keyed by *what* is generated (never by order), no `sin`/`cos`/`exp`, no
  `std::` distributions, no unordered containers. A region fills a slot of the galaxy's
  skeleton and every key starts from (seed, slot) (#342), so what is in it never depends on
  which other regions opened first. Golden hashes in `gen_tests.cpp` (the skeleton, and a
  region per slot) run on three compilers in CI; when a rule changes the output, bump
  `Gen::GENERATOR_VERSION` (regions) or `Gen::SKELETON_VERSION` (the skeleton).
- **A region is sealed the moment it opens** (#342 S3). The world is its seed plus the
  opening log in `world.json`; each region in the log is written as generated to
  `regions/<slot>.json` beside it and read from there from then on, so new region rules
  change only regions opened later and a world of older rules still loads. The skeleton is
  never sealed: a world of other skeleton rules is refused. Regions open while the server
  runs (the tide, `Simulation_Tide.cpp`, rules in `data/world_rules.json`): the host sends
  the galaxy index before the delta that adds the wormhole. `econserver regenerate <slot>`
  (server stopped) reseals one with no player structures.
- **A mouth's state is the opening log's, and its gates are dressed from it** (#342 S4).
  A tide-opened region is `unstable` until an Anchor (a structure, blueprint `anchor`) is
  finished at it, then `anchored`; unanchored it collapses after `mouthHours` and is
  `drifted` -- in the galaxy, cut off -- until the next mouth from its parent finds it.
  The gates are never in `changes`: they come from the documents and are dressed (look,
  name, `closesAt`) when a system is built and whenever the state changes, with a
  LayoutDelta, and a collapse cuts the link and both gates. At the cap a region nobody
  visits drifts away into the archive (`regions/archive-<id>.json`) and may come back,
  decayed, into another slot (`Simulation_Drift.cpp`).
- **An admin pack is woven once, into a new world** (#342 S7, `gen/Pack.h`). A region pack
  takes a free slot beside an open region, its systems renamed `r<slot>-<depth>-<k>`, and is
  sealed like any region; a system pack is sealed inside the region it replaced a system of.
  `world.json` records each pack's hash, so a pack edited later is named on startup and never
  woven again -- player structures already stand on its gates. A bad pack stops a new world
  with every problem listed; `econserver validate` says the same without running one.

- **The map is each pilot's own** (#431, `documents/world_knowledge_and_territory.md`). A pilot
  knows the public charts (`SystemAggregate::publicChart`: centres, and what a faction charted,
  `chartedBy`), their own (`"system:<id>"` keys in `ClientSession::known`, beside their scans,
  saved in the account) and their corporation's (`Corps::Corp::charts`, in corps.json, so an
  offline member's finds stay shared). The index and the
  galaxy statistics are cut per session (`KnownUniverseFor`, `GalaxyStateFor`), built only when
  `KnowledgeKey` changes; never send one map to everyone. A route order searches only the
  pilot's map. A first ship in sets `discoverer` but charts the system for that pilot alone.
  Wherever a faction *reaches* a system (survey, move, siege, site, claim, convoy, Anchor) it
  calls `ChartBy`, cold or hot: public, and if nobody from outside was there, first contact --
  the natives' name or the faction's `ChartedName` goes into `givenName` and nobody renames it.
  A pilot is the discoverer only where no faction came first. Restoring charts from a save is
  `MarkCharted`, never `ChartBy`, or every load would make contact again. A region that drifts
  into the archive takes its charts with it as `a<archive>:<id>`, and back under its new ids,
  outdated (slice 15): rename charts only through `RenameCharts`, whose log in world.json is
  what reads an offline account (`chartRenames`) through the renamings it missed.
- **What has a `signature` is hidden per pilot** (#346, #342 S9). A roaming hole's ends and a
  hidden base are left out of a pilot's layout and deltas (`Simulation::BuildLayoutFor`,
  `ForSession`) until their scan resolves them; the find is the account's (`scanned`). A
  reveal is a delta of that pilot's own with the system's revision bumped -- so take it
  (`TakeRevealDeltas`) right after the shared deltas, never between a change and its delta.
  Anything that lists a system's objects for a player goes through that filter; a roaming
  hole is never in `universe_.links`, only in a pilot's own index and routes.
- **`econagent` owns stdout.** It is the JSON-RPC channel; one stray `printf` or raylib
  trace on it corrupts the stream and the client reports a parse error rather than the
  line that caused it. All diagnostics go to stderr, and raylib's logger is redirected
  there explicitly — this bit on the first end-to-end run.
- **A release is a tag, and the version lives in `CMakeLists.txt`** (#400). `project(EconSpace
  VERSION x.y.z)` plus the `ECON_CODENAMES` table make `core/Version.h`; tag `v<that number>`
  and `release.yml` builds, tests and opens a *draft* release (a tag that disagrees stops it).
  A client is let in only by a server of the same number reading the same shared data files
  (`Compat::SharedFiles`); adding a file both sides must agree on means adding it there.
- **Bump `PROTO_VERSION` when a message changes meaning.** Decoding is deliberately
  permissive per field, so without a version bump an older peer silently reads defaults
  instead of failing.
- **The static world changes only through `Simulation`, and a `LayoutDelta` says so** (#38).
  `SystemLayout` is sent when a client enters a system; after that, `AddStatic`,
  `RemoveStatic` and `MarkStaticChanged` are the only ways a station, belt, nebula or wreck
  appears, goes or changes, and the host sends the delta before any snapshot that reflects
  it -- a client never sees a snapshot about an object it has not been told exists. Stars,
  planets and gates are refused: moons find their planet by position and gates are the
  route graph. Changes persist in `world.json` under stable keys (list, name, nth; `+N` for
  added objects) and are reapplied on load (#286).
- **Saves carry a schema version and refuse a newer one** (#20). Field-by-field defaults
  are right for a message from a peer and wrong for a save: a file from a later build would
  load as a plausible-looking wrong account and then be written back over the real one. A
  world file from the future stops the server; an account file from the future lets the
  player in with a fresh account and is never saved over. Bump `Save::WORLD_VERSION` /
  `Save::ACCOUNT_VERSION` when a field changes meaning — adding one an older reader can
  ignore does not need a bump. A save is written with `Save::WriteFileAtomic` (beside the
  file, then renamed over it, older copies kept as `.bakN`), and a *damaged* file is not
  "no file" (#341): a damaged world stops the server, a damaged account refuses its login.
- **Accounts belong to a world** (#392). They are kept in `accounts/` beside `world.json`
  (`Host::AccountsDir`); files left beside the executable by older builds are moved in at
  start, never over one already there. `world.json` carries a `worldId` made when the world
  is first saved, and each account is stamped with it: a pilot from another world keeps
  money, skills and ships but not their place or missions, and is offered a start. System
  ids repeat from seed to seed, so without the stamp a place would be a different system
  under the same name. An unstamped account belongs to whichever world loads it.
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
- **Who may come in is two text files beside `world.json`** (#400). `allowlist.txt` is checked
  when the server is `--public` or the file exists, unless `--open`; `bans.txt` always. A
  refused name gets a `Bye` before any challenge, so it cannot create its account either, and
  a change (console or editor, re-read every two seconds) takes whoever it now refuses off the
  server. Nonces and salts come from the OS generator (`net/OsRandom`), never a seeded PRNG.
- **A hosting server logs through `Log::Printf`, not `printf`** (#400). It goes to stderr with
  an ISO UTC time on every line, plus a `metrics:` line a minute; the same helpers print plain
  to stdout in the batch run and the self-tests.
- **The transport enforces its own limits** (#14). A frame length is four bytes the peer
  chose, so `TcpConnection` caps it and drops the connection rather than buffering; the
  send backlog is capped the same way. The host grants each client a budget of player
  ticks, because one command is one tick of movement and a client that sends faster than
  the simulation runs would simply move faster than everyone else. The server's side is
  tighter (64 KB frames, 1 MB backlog), and `Host::Gate` limits connections in all and per
  address, new accounts per address per hour, and retries after a wrong secret (#341).
  A client that stops acknowledging snapshots is sent one a second, and none while a
  quarter of the backlog is unsent, so a stalled reader is slowed rather than dropped
  (#411); econagent pumps its connection from a thread between tool calls for the same reason.
- **A system nobody is in has no NPC ships** (#295). A system is hot while a player is in it
  and for `Simulation::COOL_AFTER` after the last one leaves; every other system is cold and
  keeps its ships as numbers, advanced by the coarse pass. Entering warms it from those
  numbers before the first snapshot. So a test about ships, combat or per-tick agents in a
  system no session is in must call `SetAllHot(true)` or `Warm` first, and per-tick calls
  such as `StepSystemAgents` do nothing there -- use `MaintainWorld` for what the world does
  on its own. Ships above what a system keeps leave for a neighbour that lacks them
  (`StepMigration`): a migrant is taken off the source's count when it sets out and is counted
  nowhere until it arrives, and a ship turning for the gate (`NpcShip::IsLeaving`) is not
  the system's -- a recount that counted it would read its going as a loss.
- **A siege is arithmetic, and its warships only show it** (#295). The outpost's damage and
  both sides' losses come from the coarse pass in a hot system as in a cold one; the
  `Warship` NPCs a hot system shows hold fire on other NPCs and are not shot by stations, and
  their beams at the outpost take nothing off. A player shooting one down (or the outpost) is
  the one thing that changes a siege's course. Let them fight NPCs and a watched siege goes
  differently from an unwatched one. A siege's force is *lent* by holdings within reach and
  counted nowhere while it travels: `Plan::lent` is what each holding is short of (it does not
  regrow it) until a `Return` plan brings the survivors home. Moving presence without a plan
  that says where it is creates or destroys ships. A finished structure mends at its
  blueprint tier's rate (#431 D1) as f(time) from the instant its damage was set
  (`Structure::GetDamage(now)`), so fire below the rate does nothing, hot or cold; only an
  unsupplied faction outpost stops mending, and upkeep decays it instead. What a besieger
  takes off a wall is its faction's warship design's guns (`Simulation::WarshipDps`, from
  `Ships::WarshipDps` in `core/Fitting`: the volley a player's ship of that hull and fit
  fires, at the player cooldown), never a number in a rules file (owner, 2026-10-22); the
  planner and the pass both read it, so a refit of the doctrine's warship moves every siege.
- **NPCs and players fight by one model** (owner, 2026-10-22, #186). An NPC's guns and hull
  are its design's as a player's ship of it has them (`Ships::Stats::volley`, `combatHull`),
  fired at `Ships::PLAYER_FIRE_INTERVAL` from `Sim::PLAYER_WEAPON_RANGE`; only the shields are
  a player ship's alone. What a cold system's fights and a siege's attrition cost each side is
  the other's dps over its own hull, from the same designs (`Simulation::FighterOf`, i.e.
  `Ships::RoleFighter`); the rates in code and `attrition` in blueprints.json are how often
  ships meet, never what a gun does. A pirate too strong or too weak for a new pilot is a
  change to its fit in `data/ships.json`, never a multiplier. A station is not a design: its
  guns are its archetype's `defensive.damage` a second.
- **Per-player state lives in `ClientSession`, not in `Simulation`** (#3). The ship, the
  account, the missions, the standing order and the event journal belong to a session, and
  every player verb takes the session it acts for. There is no "active system" either: a
  session carries the system it is in, and every system is stepped the same way. Anything
  reintroduced as a member of `Simulation` is shared by every player on the server.
- **A planet and what orbits it are placed by the world clock** (#210). `Orbits::Place`
  puts every planet at `f(world time)` and every satellite (an object with `orbits`) at its
  planet plus a turning offset; nothing integrates them per tick. A satellite is in every
  snapshot like a planet, so the client needs nothing new, and an order approaching one
  is re-aimed after the drop-out because the target has moved since it was issued. Within
  `Orders::CHASE_WITHIN` the re-aim is a follow hold, never another warp (#377): a warp
  goes to a fixed point, and re-warping at a station doing 100 u/s landed short for ever.
- **Anything that moves is `f(time, seed)`** (#136), never integrated per frame. A part
  whose angle accumulated would drift between clients and two players would see the same
  station turned differently; as written nothing about motion crosses the wire. The turn is
  wrapped, because a float counting degrees for a week has no precision left.
- **An object is a composition, and it is written in data** (#122). Seven primitives with
  roles, offsets, rotational `repeat` and bilateral `mirror` -- different symmetries, and
  using `repeat: 2` for a pair of wings puts the second one in front of the nose. The
  vocabulary is narrow on purpose: an open-ended set of arbitrary shapes reads as
  programmer art. An unknown form is a **load error**, not a skipped part -- and so is an
  unknown *field*, in a part or anywhere else in an archetype (#191): read with
  `value(key, default)`, a misspelling is indistinguishable from an absence.
- **A planet's surface is on the sphere** (#166). A part with `lat`/`lon` is projected:
  carried across the face by the planet's turn, foreshortened at the limb, hidden round
  the back, and lit as the body rather than as itself. A `band` is the visible part of a
  latitude strip, solved exactly per screen column, so it never needs clipping. Turning a
  feature about the disc's centre like a wheel -- what surfaces did before -- is not what
  a planet does.
- **A material is bound per part, not per object** (#135). One sphere at the centre is the
  truth about a planet and a lie about a station. An elongated part is lit as a cylinder
  from `item.axis`; without that, an arm two radii long gets a bright band across its
  middle and both ends in shadow.
- **A material is a shader plus bindings, and an object never knows either** (#121). An
  archetype names a material; a material maps uniform names to sources -- a field of the
  `Render::Item`, the scene's light, the clock, or a constant. The backend resolves them
  per item. Unlike a screen-treatment pass, an **unknown source is a load error**: losing a
  pass makes the picture plainer, losing a uniform makes a material draw something actively
  wrong and do it silently.
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
- **The mouse has one owner per frame** (#297). `Ui::Desk` hit-tests every window, screen
  and popup by layer and z-order and picks one; everything else is told so through
  `Ui::Frame` or `Ui::MouseScope`. A widget that reads `IsMouseButtonPressed` against its
  own rectangle reacts to clicks meant for the window in front of it -- use
  `Ui::MouseOver`/`MousePressed` or the frame's `Hovered`/`Pressed`. Esc is the desk's too:
  a new window or screen is registered with an `EscRule`, not added to a hand-written chain.
- **A faction is a registry entry, not an enum** (#355). `FactionId` is an index into the
  registry `data/factions.json` fills and a generated region or a pack extends at runtime,
  so it means nothing outside the process: saves and the wire name a faction by its string
  id (`Factions::Id`). The code names no faction -- who owns what nobody owns, who the
  pirates are, whose police patrol a lawless system are `roles` in the file. Store a value
  per faction in `PerFaction<T>` and iterate `Factions::All()`.
- **A player corporation is not a faction** (#347). It has its own registry
  (`sim/Corporation.h`, saved as `corps.json` beside the world), is named by a string id and
  shown by its ticker. Standings stay each pilot's own. What a corporation shares is trust:
  a member's armed structures never fire first on a member and guard members' ships, and
  the CEO and directors set members' rules of engagement. Its verbs are a `CorpAct`, not a
  `Command`, and they are answered with a chat notice.
- **One side holds a system: a faction or a corporation** (#431 slice 5,
  `Simulation_Territory.cpp`). A corporation holds one with the most influence (its members'
  structures by tier, `world_rules.json` → `influence`) and a claim (a finished outpost or
  above), held for `ContestPasses()`; then `agg.holderCorp` is its id and `claimed` is false,
  so no faction's garrison, income or law is there. The faction rule in `StepControl` skips
  such a system; `StepClaims` contests it. A market's fee is `FeeAt`'s: the holder's tax, or
  the market's owner (a player's market pays its owner's corporation or the pilot, #438).
  A held system is besieged or claimed only from next door (#431 D3, `FrontOpen(system, side)`,
  side = a faction's `InfluenceMap::Key` or a corporation's id): a gate neighbour the side or a
  faction's ally holds, or nobody's. The faction siege planner, the Anchor siege, `StepControl`
  and `StepClaims` all read it; a ship's guns never do. Roaming holes are not neighbours, so
  they open no front.
- **Building in a held system takes a licence** (#431 slice 6, `Simulation_Territory.cpp`).
  `LicenceHolder` is the holder corporation or a lawful faction (not the unaffiliated); the
  party is the owner's corporation or the pilot (`PartyOf`). Leave is a standing (a
  corporation's = its members' summed) or the holder corporation's docking policy; the rent
  (`world_rules.json` → `licences`) is kept on `Corps::Corp::licences` or the pilot's
  `OwnerBook`, in world time, and taken in the coarse pass from the treasury or the pilot's
  money -- offline, within the money noted when last on. A structure's `LicenceState` is
  derived each pass (`RefreshLicences`), never saved: `Trespass` makes `LawOver` lawless and
  puts it in `SystemState::trespass`, which `Think` may besiege (`Plan::evicting`). A player
  structure found without a licence gets one asked from now, i.e. a grace day, not trespass.
- **A ship flies a design, and a player's design is named by its content** (#279). A refit
  files `Ships::PlayerFit`'s design under `Ships::ContentId` and the ship flies that id; the
  snapshot carries only the id, so the host sends a `DesignDef` before the first snapshot
  that names one, the way a `LayoutDelta` goes first (#38). The fit rules and the price are
  `core/Fitting`, read by the server, the Fit tab and `preview_fit` alike -- never a copy.
  Refits happen only at a station whose archetype declares `shipyard`. A player's named
  library (`ClientSession::designLibrary`, name -> id) is a reference like a ship's: it is
  in the account save and the snapshot, gets its `DesignDef` first, and keeps the design in
  `designs.json` through pruning. Applying a saved design *is* `RefitShip` with its fit.
- **Load `Archetypes` before building a world.** Entity constructors look themselves up
  in the registry, so an entity built before the load has no components and no glyph —
  it would silently be undockable and invisible rather than fail.
- **Comments referencing "M4f", "L2", "M0"** are historical milestone markers from the
  living-galaxy and netcode tracks. They describe *when* something was built, not what is
  planned.

## Planning

Work is organized into milestones (M4–M10 are open; the order is in
[ROADMAP.md](ROADMAP.md)) and three track epics: **#42** agents/MCP, **#43** the
data-driven world and its generated presentation, **#44** the player-mutable world. Check
the milestone an issue belongs to before proposing sequencing. Ideas proposed by an agent
carry the `proposal` label and are not built until the owner says yes.
