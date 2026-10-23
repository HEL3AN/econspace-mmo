# Contributing to EconSpace

Thanks for your interest! EconSpace is an open-source, engineering-driven space-sim prototype, and contributions of all kinds are welcome — code, world content, bots, docs, art and bug reports. Contributions from AI agents are welcome too; [CLAUDE.md](CLAUDE.md) is the operating context written for them.

## Where to start

- **A first change:** issues labelled [`good first issue`](https://github.com/HEL3AN/econspace-mmo/issues?q=is%3Aopen+label%3A%22good+first+issue%22) are small and each says which files to open and how to test. [`help wanted`](https://github.com/HEL3AN/econspace-mmo/issues?q=is%3Aopen+label%3A%22help+wanted%22) are larger but well scoped. Say in the issue that you are taking one.
- **A question or an idea:** [Discussions](https://github.com/HEL3AN/econspace-mmo/discussions) — Q&A for how-to, Ideas for something you want to talk over before it becomes an issue.
- **Content without C++:** the how-tos in [docs/howto/](docs/howto/README.md) add a kind of object from data, step by step.
- **A bot:** [docs/agents/](docs/agents/README.md) and the example in [examples/agents/](examples/agents/README.md).
- **Before proposing a big change:** the settled premises in [CLAUDE.md](CLAUDE.md#settled-premises--do-not-re-litigate-these) and the reasons in [DECISIONS.md](DECISIONS.md) are not re-opened; build on top of them.

## Ways to help

- **Bots** — build one on the game's MCP server and tell us what it could not do. Start with [docs/agents/](docs/agents/README.md).
- **World content** — new kinds of object, composed from parts in `data/archetypes.json` and judged in the gallery (`worldeditor gallery shapes`); systems and galaxy links with the editor. Start with the worked how-tos in [docs/howto/](docs/howto/README.md); the format they rest on is [`documents/world_format.md`](documents/world_format.md).
- **Art** — welcome, though no longer what the project is blocked on: an object's look is generated from its archetype, and a hand-made sprite wins wherever one exists. See [`documents/texture_assets.md`](documents/texture_assets.md).
- **Gameplay & code** — see the [issue tracker](../../issues) and [ROADMAP.md](ROADMAP.md).
- **Docs** — improvements to the design docs in [`documents/`](documents/) and the top-level guides.
- **Bug reports and playtest feedback** — open an issue; the forms ask for what is needed.

## Building

Requirements:
- **Windows:** MinGW-w64 g++ from [MSYS2](https://www.msys2.org/), and CMake 3.16+.
- **Linux:** GCC, CMake 3.16+, and raylib's build dependencies:
  `sudo apt install libasound2-dev libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev`.
- Internet on the first build: raylib, nlohmann/json, doctest, PicoSHA2 and Clay are fetched by CMake `FetchContent`.

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug   # on Linux: no -G
cmake --build build
ctest --test-dir build --output-on-failure
```

Always name a build type. `Debug` (`build/`) is for development and tests; `Release` (`build-release/`) is for playtests, published builds and any performance number, because an unoptimized build is several times slower and points at the wrong costs.

CI builds with warnings as errors (`-DWARNINGS_AS_ERRORS=ON`) and checks formatting with `clang-format --dry-run --Werror`; run `clang-format -i` on the files you touched.

See [README.md](README.md) for run instructions and networked play.

## Code style

- Follow [CONVENTIONS.md](CONVENTIONS.md): types/methods `PascalCase`, class fields `camelCase_`, formatting per `.clang-format`.
- The build runs with `-Wall -Wextra` and stays **warning-clean** for our code — please keep it that way (third-party headers are marked SYSTEM and don't count).
- All code, comments, and documentation in the repository are in **English**.
- Keep the engine independent: `engine` must not depend on any other target.
- Add new `.cpp` files to the correct CMake target — `engine`, `netproto`, `econspace`, `econserver`, `econagent`, `worldeditor` or `tests`. Code shared by more than one executable belongs in `engine` (or `netproto`, if it is about the wire).

## Tests

- Unit tests use [doctest](https://github.com/doctest/doctest) (the `tests` target, run via `ctest`). The wire protocol is covered by round-trip tests — if you touch `Protocol.*`, run `ctest`.
- The server has five smoke tests: `econserver hosttest` (the server loop), `accttest` (accounts), `worldtest` (galaxy persistence and the clock), `ordertest` (standing orders, routes and the journal) and `opstest` (server operations: `init`, the console, the admin channel).
- The agent seam is tested end to end against a live server: `econagent selftest <host> <port> <name> <secret>`, and the example bot in `examples/agents/`. If you change a tool, regenerate [docs/agents/reference.md](docs/agents/reference.md) with `python tools/gen_agent_reference.py ./build/bin/agent/econagent.exe`; CI fails on a stale copy.
- The generator is pinned by golden hashes in `tests/gen_tests.cpp`. A change to what a seed generates bumps `Gen::GENERATOR_VERSION` (or `Gen::SKELETON_VERSION`) and updates the hashes.
- Admin packs are checked with `econserver validate`.
- **Anything visual has to be looked at.** No test can see a picture. Open `worldeditor gallery shapes`, or the game, and put a screenshot in the pull request. Every rendering bug in the look so far was found that way.

## Pull requests

1. Fork and create a topic branch off `main`, one per issue.
2. Keep changes focused; make sure the build is green and `-Wall -Wextra` is clean.
3. Run `ctest` and the relevant smoke tests.
4. Write a clear PR description of *what* and *why*. A PR that adds a new type, store, window or message says why no existing one could carry it: a feature extends what is already built ([CLAUDE.md](CLAUDE.md#structure-and-rules)).
5. If a player, a host or a bot author would notice the change, add one line under **Unreleased** in [CHANGELOG.md](CHANGELOG.md), with the issue or PR number.
6. Never commit saves: `world.json`, `regions/`, `accounts/`, `corps.json`, `designs.json`, `allowlist.txt`, `bans.txt`.

Ideas from agents are filed with the `proposal` label and wait for the owner before anything is built.

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE).
