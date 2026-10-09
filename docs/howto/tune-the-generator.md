# Tune the generator

The region beyond the wormhole is generated from a seed by `Gen::GenerateRegion`
(`src/engine/gen/Region.cpp`). A rule there is code, so changing one means a rebuild -- but
judging one does not mean starting a server and flying around. A single good system proves
nothing about a generator, because a single good system can be arranged
([#141](https://github.com/HEL3AN/econspace-mmo/issues/141)). The survey screen shows fifty of
them at once.

```sh
./build/bin/editor/worldeditor.exe survey 1 shapes        # seeds 1, 2, 3: every system of each region
./build/bin/editor/worldeditor.exe survey 40 shapes card 7 # from seed 40, with the eighth card enlarged
```

Inside the editor, **F4** opens and closes it from any view.

## What a card shows

Each card is one generated system, built by `WorldLoader::BuildSystem` and drawn through the
same `Render::Present` the game uses, at **one scale for every card** -- the whole million
units edge to edge -- so "how far out does this go" can be compared across the grid. Bodies
are drawn no smaller than a few pixels (a planet is a hundredth of a system's width), and the
small things worth counting carry a marker on top: a ring for a belt, a cross for a wreck, a
square for a gate, a circle for a cloud, and a **gold diamond for any kind the survey has no
marker for** -- a new kind of thing is exactly what you want to notice. **M** hides them.

On the card: the designation and the seed on top, the depth (ring) and star type under it,
and the counts at the bottom: `p` planets, `b` belts, `w` wrecks, `n` clouds, `g` gates, plus
any other array the document has. A `character` is shown when the generator writes one.

## What the screen flags

| Flag | Means |
|---|---|
| **EMPTY** (red) | nothing but a star, planets and gates -- the failure a rule hides best |
| **thin** (amber) | exactly one thing worth stopping for |
| **= W-3.2 s1** (violet) | the same as an earlier system: same star, same character, same number of each kind (gates are left out -- they are the map, not the system). Hover a card to outline every card like it |

"Worth stopping for" is anything in the document other than the star, planets and gates, so a
new kind of thing counts without a change to the survey. An object with an `"archetype"`
field is counted as that archetype rather than as its array, so a rare find is not just
another belt.

The panel on the right is the **distribution** of the whole page: star types, characters,
planets per system, the outermost orbit, how much is worth stopping for, and one histogram per
kind. A gap in a row (a value no system had) is drawn as an empty bar on purpose.

## Keys

| Key | |
|---|---|
| **PgDn / PgUp** | the next / previous run of seeds |
| **+ / -** | regions per page (each region is 18 systems) |
| **R** | generate the page again (after nothing has changed it must look identical) |
| **click** | enlarge a card, with names on everything; **← / →** step through, click or **Esc** returns |
| **M** | markers on and off |
| **F2 / F10** | backend, screen treatment -- the same as in the gallery |
| **Esc** | back to the system view |

## The loop

1. Change a rule in `src/engine/gen/Region.cpp`.
2. `cmake --build build`, then `./build/bin/tests/tests.exe` -- the golden test in
   `tests/gen_tests.cpp` fails when the output for a seed changes, which is your reminder to
   bump `Gen::GENERATOR_VERSION`.
3. `worldeditor survey 1 shapes`, look at the three numbers at the top of the panel, then page
   through a few batches. A rule is judged on its worst page, not its best card.

The counting itself (`Gen::Analyse` in `src/engine/gen/Survey.cpp`) has no window and is tested
in `tests/survey_tests.cpp`; if you change what "empty" or "the same" means, change it there.
