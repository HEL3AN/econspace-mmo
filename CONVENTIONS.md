# EconSpace — coding standard

Shared rules for writing code. The goal is consistency: the code should look as
though a single person wrote it.

## Project structure

```
src/
  engine/      shared core (static library `engine`)
    core/      world loading, factions
    entities/  game objects (Entity and its subclasses)
    economy/   resources
    render/    texture store and drawing helpers
    ui/        interface elements
  game/        the client, the authoritative Simulation, and the headless server
    core/      the client game loop
    sim/       Simulation, wire protocol, headless server entry point
    net/       transports (loopback, TCP)
    entities/  ships
    player/    the player account, skills
    economy/   market
    missions/  the mission system
    main.cpp   client entry point
  editor/      the visual world editor
data/          game data (JSON), edited without recompiling
documents/     project documentation
tests/         doctest unit tests
```

The code is split into three modules: `engine` (a static library) and the two
executables that link it, `game` and `editor`. **`engine` never depends on `game` or
`editor`** — anything shared belongs in `engine`, anything that knows about gameplay
does not.

- The `.h` and `.cpp` of a class live side by side, in the same folder.
- `src/engine` is the include root for engine headers: paths like
  `#include "entities/Entity.h"`. Within `game` and `editor`, their own module root
  works the same way.
- A new `.cpp` is added to the correct CMake target in `CMakeLists.txt` — `engine`
  (`add_library`), `econspace`, `econserver`, or `worldeditor` (`add_executable`).
  Code shared by more than one executable goes into `engine`.
- One class — one file; the file name matches the class name.

## Naming

| What | Style | Example |
|------|-------|---------|
| Types (class, struct, enum class) | `PascalCase` | `AsteroidField` |
| Methods and functions | `PascalCase` | `GetSpeed`, `LoadSystem` |
| Class fields | `camelCase_` (trailing `_`) | `orbitRadius_`, `mineOwner_` |
| Local variables, parameters | `camelCase` | `worldMouse`, `dt` |
| enum class values | `PascalCase` | `ResourceType::Iron` |
| Constants (global/static) | `UPPER_SNAKE` | `MAX_SPEED` |
| Namespaces | lowercase | `WorldLoader` |
| Folders | lowercase | `entities` |

The trailing `_` on fields immediately sets them apart from local variables and
parameters — no need to guess when you're touching object state.

## Files and headers

- Every `.h` starts with `#pragma once`.
- `#include` order:
  1. the class's own header (in the `.cpp`);
  2. project headers;
  3. third-party libraries (`raylib.h`, `nlohmann/json.hpp`);
  4. the standard library (`<vector>`, `<string>`).
- Use forward declarations (`class Foo;`) instead of `#include` when a pointer or
  reference is enough.

## C++ practices

- The standard is C++17.
- Resource ownership goes through RAII and smart pointers (`std::unique_ptr`),
  with no manual `new`/`delete`.
- Large objects are passed by `const&`, not by copy.
- const-correctness: a method that doesn't modify the object is marked `const`.
- Fields are initialized in the constructor's initializer list.
- An overridden virtual method is marked `override`.
- Short getters may be defined right in the header (`{ return x_; }`).

## Comments

- Language — English, like everything else in the repository (code, comments, and
  documentation).
- They explain the **"why"**, not the "what": the code already shows what it does.
- Only where the logic isn't obvious. Obvious code needs no comment.
- Commented-out code is not kept in the repository.

## Formatting

Defined by the `.clang-format` file (4 spaces, braces on their own line, width 100).
In VS Code — format the document with `Shift+Alt+F` or on save.

## Commits

- The message is in English, in the imperative mood, capturing the gist of the change.
- Roadmap milestones: prefix with the milestone id (e.g. `R3: docking with a station`,
  `M4: TCP transport`).
