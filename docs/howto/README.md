# How to add a thing

In EconSpace a kind of object is **data**. What it is, what it can do, what it is made of,
how it is shaded and what the screen does to it afterwards all live in `data/`, and a new
kind of station needs no artist and, for most of the work, no compiler.

[`documents/world_format.md`](../../documents/world_format.md) is the **specification** of
those files: every field, every value, and the rules. These pages are the **tutorial**. Each one
builds a single example from nothing and ends with you looking at it in the gallery.

| | How-to | You touch | C++? |
|---|---|---|---|
| 1 | [Add an archetype](add-an-archetype.md) | `data/archetypes.json` | only to make the game build one, today |
| 2 | [Compose its look](compose-a-look.md) | the archetype's `shape` | no |
| 3 | [Give it a material](give-it-a-material.md) | `data/materials.json`, `data/shaders/materials/*.fs` | no |
| 4 | [Add a screen pass](add-a-screen-pass.md) | `data/shaders/*.fs`, `data/look.json`, one enum | yes, three lines |
| 5 | [Tune the generator](tune-the-generator.md) | the rules in `src/engine/gen/` | yes -- the rules are code |

They are written in that order and build on each other: the example is a small **relay
station**, first as a bare entry, then with a shape, then with its own material. The screen
pass stands on its own, and so does the generator: it is about the rules that place things,
judged on fifty systems at once.

---

## The loop: change, launch, look

```sh
./build/bin/editor/worldeditor.exe gallery shapes
```

`gallery` opens on the archetype gallery instead of a star system; `shapes` starts on the
shape backend instead of the glyph one. The gallery shows **every archetype in the registry at
once**, lit and treated through the same code the game draws with, which is the point of it:
a look is judged by eye, and the loop it replaced was build, start a server, connect, fly there,
look ([#118](https://github.com/HEL3AN/econspace-mmo/issues/118)).

Things worth knowing before you start:

- **There is no hot reload.** The editor reads `data/` once, when it starts. Edit the JSON,
  close the editor, launch it again.
- **The editor reads the repository's own `data/`**, not the copy next to the executable, so
  there is nothing to rebuild after a data change. (A shader is data too.)
- **Run it from a terminal and read what it prints.** Every load problem is a log line, and
  most of them are nowhere else: `Archetypes: ...`, `Materials: ...`, `Treatment: ...`.
- **Keys:** **F2** switches shapes and glyphs, **F3** switches the gallery and the system view,
  **F4** opens the survey of generated systems ([tune the generator](tune-the-generator.md)),
  **F10** opens the screen treatment's settings (and *saves* `data/look.json` when it closes),
  **Esc** leaves the gallery. Click a card to select it.
- **The panel on the right edits the selected archetype's look** — colour, size, layer, light,
  glyph, material, style — and **Ctrl+S** writes the changed values back into
  `data/archetypes.json`, touching nothing else in the file. Everything else, including the
  shape, is edited by hand.
- **The test suite loads the shipped data.** `ctest --test-dir build --output-on-failure` (or
  `./build/bin/tests/tests.exe`) fails on an archetype file the game could not load, a look file
  it could not read, and a few rules the data has to keep. Run it before you open a pull request.
- **No test can see a picture.** Put a screenshot of the gallery in the pull request
  ([CONTRIBUTING](../../CONTRIBUTING.md#tests)).

## Loud and silent mistakes

The loaders are deliberately strict about some mistakes and deliberately forgiving about
others. Knowing which is which saves the most time:

| Mistake | What happens |
|---|---|
| unknown `kind`, `style`, component, `form` or `role`; duplicate or missing `id` | the **whole** archetype file is refused; the gallery is empty |
| a material binding names an unknown source | the **whole** materials file is refused; every object is drawn plain |
| a material's shader is missing or does not compile | that one material is dropped; its objects are drawn plain |
| a screen pass's shader is missing or does not compile | that one pass is dropped and marked *unavailable* in F10 |
| unknown pass name in `look.json` | that entry is skipped |
| a misspelled field name anywhere (`minPixel`, `matrial`) | **ignored without a word** — the default is used |
| an archetype names a material id that does not exist | **drawn plain without a word** |

The last two are the ones that cost an afternoon. If a change "does nothing", check the
spelling first.

## For AI agents

These pages are written to be followed literally, by people and by agents. Field names and
commands are checked against the code; when they disagree, the code is right and the page is a
bug worth an issue. The settled premises an agent must plan on top of are in
[CLAUDE.md](../../CLAUDE.md), and the reasons behind them in [DECISIONS.md](../../DECISIONS.md).
