# Contributing to EconSpace

Thanks for your interest! EconSpace is an open-source, engineering-driven space-sim prototype, and contributions of all kinds are welcome — code, world content, bots, docs, art and bug reports. Contributions from AI agents are welcome too; [CLAUDE.md](CLAUDE.md) is the operating context written for them.

## Ways to help

- **Bots** — build one on the game's MCP server and tell us what it could not do. Start with [docs/agents/](docs/agents/README.md).
- **World content** — new kinds of object, composed from parts in `data/archetypes.json` and judged in the gallery (`worldeditor gallery shapes`); systems and galaxy links with the editor. The format is [`documents/world_format.md`](documents/world_format.md).
- **Art** — welcome, though no longer what the project is blocked on: an object's look is generated from its archetype, and a hand-made sprite wins wherever one exists. See [`documents/texture_assets.md`](documents/texture_assets.md).
- **Gameplay & code** — see the [issue tracker](../../issues) and [ROADMAP.md](ROADMAP.md).
- **Docs** — improvements to the design docs in [`documents/`](documents/) and the top-level guides.
- **Bug reports** — open an issue with clear repro steps.

## Building

Requirements:
- **Windows:** MinGW-w64 g++ from [MSYS2](https://www.msys2.org/), and CMake 3.16+.
- **Linux:** GCC, CMake 3.16+, and raylib's build dependencies:
  `sudo apt install libasound2-dev libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libx11-dev libxcursor-dev libxi-dev libxinerama-dev libxkbcommon-dev libxrandr-dev`.
- Internet on the first build: raylib, nlohmann/json, doctest and PicoSHA2 are fetched by CMake `FetchContent`.

```sh
cmake -S . -B build -G "MinGW Makefiles"   # on Linux: cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CI builds with warnings as errors; to check that locally, configure a second build directory with `-DWARNINGS_AS_ERRORS=ON`.

See [README.md](README.md) for run instructions and networked play.

## Code style

- Follow [CONVENTIONS.md](CONVENTIONS.md): types/methods `PascalCase`, class fields `camelCase_`, formatting per `.clang-format`.
- The build runs with `-Wall -Wextra` and stays **warning-clean** for our code — please keep it that way (third-party headers are marked SYSTEM and don't count).
- All code, comments, and documentation in the repository are in **English**.
- Keep the engine independent: `engine` must not depend on any other target.
- Add new `.cpp` files to the correct CMake target — `engine`, `netproto`, `econspace`, `econserver`, `econagent`, `worldeditor` or `tests`. Code shared by more than one executable belongs in `engine` (or `netproto`, if it is about the wire).

## Tests

- Unit tests use [doctest](https://github.com/doctest/doctest) (the `tests` target, run via `ctest`). The wire protocol is covered by round-trip tests — if you touch `Protocol.*`, run `ctest`.
- The server has four smoke tests: `econserver hosttest` (the server loop), `accttest` (accounts), `worldtest` (galaxy persistence and the clock) and `ordertest` (standing orders, routes and the journal).
- The agent seam is tested end to end against a live server: `econagent selftest <host> <port> <name> <secret>`, and the example bot in `examples/agents/`.
- **Anything visual has to be looked at.** No test can see a picture. Open `worldeditor gallery shapes`, or the game, and put a screenshot in the pull request. Every rendering bug in the look so far was found that way.

## Pull requests

1. Fork and create a topic branch.
2. Keep changes focused; make sure the build is green and `-Wall -Wextra` is clean.
3. Run `ctest` and the relevant smoke tests.
4. Write a clear PR description of *what* and *why*.

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE).
