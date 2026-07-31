# Contributing to EconSpace

Thanks for your interest! EconSpace is an open-source, engineering-driven space-sim prototype, and contributions of all kinds are welcome — code, world content, art, docs, and bug reports.

## Ways to help

- **Art** — the biggest gap. Sprites are currently placeholder shapes; real textures would transform the project. Asset prompts/specs live in [`documents/texture_assets.md`](documents/texture_assets.md).
- **World content** — build systems and galaxy links with the visual editor (`worldeditor`) and submit the JSON.
- **Gameplay & code** — see the [issue tracker](../../issues) and [ROADMAP.md](ROADMAP.md).
- **Docs** — improvements to the design docs in [`documents/`](documents/) and the top-level guides.
- **Bug reports** — open an issue with clear repro steps.

## Building

Requirements: MinGW-w64 g++ (MSYS2), CMake 3.16+, and internet on the first build (raylib and nlohmann/json are fetched via CMake `FetchContent`).

```sh
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

See [README.md](README.md) for run instructions and networked play.

## Code style

- Follow [CONVENTIONS.md](CONVENTIONS.md): types/methods `PascalCase`, class fields `camelCase_`, formatting per `.clang-format`.
- The build runs with `-Wall -Wextra` and stays **warning-clean** for our code — please keep it that way (third-party headers are marked SYSTEM and don't count).
- All code, comments, and documentation in the repository are in **English**.
- Keep the engine independent: `engine` must not depend on `game` or `editor`.
- Add new `.cpp` files to the correct CMake target (`engine` / `econspace` / `worldeditor`).

## Tests

- Unit tests use [doctest](https://github.com/doctest/doctest) (the `tests` target, run via `ctest`). The wire protocol is covered by round-trip tests — if you touch `Protocol.*`, run `ctest`.
- The server loop has a smoke test: `econserver hosttest` (and `econserver accttest` for account persistence).

## Pull requests

1. Fork and create a topic branch.
2. Keep changes focused; make sure the build is green and `-Wall -Wextra` is clean.
3. Run `ctest` and the relevant smoke tests.
4. Write a clear PR description of *what* and *why*.

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE).
