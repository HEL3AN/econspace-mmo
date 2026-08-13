# data/textures/

Sprite PNGs for the optional sprite rendering backend. The folder is empty apart from
this file and `.gitkeep` — no art has been contributed yet.

**What belongs here:** PNG files with an alpha channel, one object per file, square
canvas, transparent background. The full specification — sizes, prompts, and layout
rules — is in [`documents/texture_assets.md`](../../documents/texture_assets.md).

**File names must match the table in that document exactly, in lowercase**
(`star.png`, `planet.png`, `station.png`, `asteroids.png`, `ship.png`, ...). The names
are hardwired into the code; a misspelled or differently-cased file is simply not
found.

**No rebuild is needed.** Textures are loaded from `data/` at runtime, lazily and
cached (`src/engine/render/Textures.h`). Add a file, run the game, and it is drawn.
If a file is missing, the entity falls back to its vector shape and the game keeps
running — so a partial set is fine.

Note that the project's primary presentation is moving to glyph/ASCII rendering
(issue #36). Sprites are an optional alternative backend, not a prerequisite for
playing.
