# Add a screen pass

After the world is drawn, it is drawn **again**, through an ordered chain of full-screen passes:
bloom, pixelation, grain, scanlines, vignette, colour fringe
([#120](https://github.com/HEL3AN/econspace-mmo/issues/120)). That treatment is most of what makes
simple shapes read as a place rather than as a diagram, and it costs nothing per object: an
object nobody drew is dressed by the same passes as everything else.

This page adds a seventh pass, `phosphor`, which turns the picture toward the single colour of an
old monochrome screen.

- [What a pass is](#what-a-pass-is)
- [Write the shader](#write-the-shader)
- [Name the pass](#name-the-pass)
- [Put it in the chain](#put-it-in-the-chain)
- [See it](#see-it)
- [When a shader fails](#when-a-shader-fails)

---

## What a pass is

One fragment shader, `data/shaders/<pass>.fs`, that reads the picture so far and writes the next
one. Every pass has the same **two knobs**:

- **`amount`**, 0..1 — how much of the effect. Zero is off; one is the effect as designed.
- **`scale`** — the one other thing the effect has: how far the glow reaches, how large a pixel
  is, how far apart the lines sit. What it means is written at the top of the shader and shown
  beside its slider.

Two knobs named the same for every pass, rather than a struct per effect, is what lets one
settings screen tune any pass, including one that did not exist when it was written.

Unlike archetypes and materials, **the list of passes is code** — a small enum — so that the
settings screen can name each one and say what its `scale` means. Adding one is a shader, three
short edits in C++ and one entry in `data/look.json`.

---

## Write the shader

Create `data/shaders/phosphor.fs`:

```glsl
#version 330

// Phosphor: the picture is shown in one colour, as a monochrome screen shows it.
//
// scale -- how hard the contrast is pushed. 1.0 leaves brightness alone; larger crushes
// the darks and keeps only what is bright.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float amount;
uniform float scale;

out vec4 finalColor;

const vec3 PHOSPHOR = vec3(0.45, 1.0, 0.6);

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    vec3 mono = PHOSPHOR * pow(lum, max(scale, 0.1));
    finalColor = vec4(mix(c, mono, amount), 1.0) * fragColor;
}
```

What a pass is handed (see also `data/shaders/_header.txt`):

| Uniform | |
|---|---|
| `texture0` | the picture so far — the world, or what the previous pass made of it |
| `amount`, `scale` | the two knobs |
| `resolution` | the render target, pixels (`vec2`) |
| `time` | seconds, for anything that moves |
| `scene` | the picture before *any* pass. Only bloom uses it, to composite its glow over the original |

Declare the ones you use; the rest are skipped. Make `amount` a blend, as here, so the effect
can be dialled down rather than only switched off.

## Name the pass

Three edits, in `src/engine/render/TreatmentConfig.h` and `TreatmentConfig.cpp`:

```cpp
// TreatmentConfig.h -- the kind, at the end of the enum
enum class PassKind
{
    ...
    Vignette,   // the corners fall off
    Phosphor    // the picture is shown in one colour, as a monochrome screen shows it
};
```

```cpp
// TreatmentConfig.cpp -- its name in data, and what its scale means
const Named NAMES[] = {
    ...
    { PassKind::Vignette, "vignette", "how far in the corners reach" },
    { PassKind::Phosphor, "phosphor", "how hard the contrast is pushed" },
};
```

```cpp
// TreatmentConfig.cpp, TreatmentConfig::Default() -- the chain a player gets back from
// "reset to default". Before the vignette, which stays last.
{ PassKind::Fringe, true, 0.30f, 1.0f },    { PassKind::Phosphor, false, 0.60f, 1.5f },
{ PassKind::Vignette, true, 0.50f, 1.0f },
```

The four values are kind, enabled, amount, scale. The name is also the shader's file name, so
`"phosphor"` loads `data/shaders/phosphor.fs`. Run `clang-format -i` on both files.

The tests hold you to a few things here: every pass has a non-empty `scale` meaning, the default
chain contains every pass, bloom comes before pixelation, and the vignette is **last** — it should
see everything the others did, or a bright corner survives a treatment meant to darken it.

## Put it in the chain

Add an entry to `chain` in `data/look.json`:

```json
{
    "pass": "phosphor",
    "enabled": true,
    "amount": 0.6,
    "scale": 1.5
}
```

**The chain runs in the order it is written, and the order is a decision, not a detail.** Bloom
before pixelation gives soft fat pixels; after it, hard pixel edges that glow. Those are
different games to look at, which is why the order is data, reorderable in game, rather than a
sequence of calls. `enabled: false` skips a pass without losing its place.

The test suite also reads the shipped `look.json`: it must list every pass, with `amount` in 0..1
and `scale` above zero.

`look.json` is written by the game — closing the **F10** settings saves it — so it is
machine-formatted and nobody needs to hand-format it. `treatHud` beside the chain decides whether
the interface goes through it too; it is off, because the HUD carries numbers people fly by and a
pixelated fuel gauge is a worse game.

---

## See it

```sh
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/editor/worldeditor.exe gallery shapes
```

The terminal says `Treatment: 7 of 7 passes loaded`, and every card has gone green. The cards
go through the chain exactly as the game's world does; the panels are drawn after it and stay
literal, which is the same rule the game follows with its HUD. **F10** shows `phosphor` with an
`amount` slider and one labelled *how hard the contrast is pushed*, and the arrows to move it up
and down the chain.

**Before you propose shipping a pass**, remember it changes what every player sees. The look is
an art decision: open an issue with a screenshot before a pull request.

---

## When a shader fails

Break it — misspell `dot` — and launch again:

```text
WARNING: SHADER: [ID 4] Compile error: ERROR: 0:22: 'dott' : undeclared identifier
WARNING: Treatment: phosphor would not compile -- pass dropped
INFO: Treatment: 6 of 7 passes loaded
```

The gallery is back in colour with the other six passes, and F10 lists the pass as
*phosphor (unavailable)*, with the reason at the top of the panel. A shader that compiles but
fails to **link** says `would not link` instead, and a missing file says where it looked
([#190](https://github.com/HEL3AN/econspace-mmo/issues/190)). If **no** pass loads, the world is
drawn straight to the screen.

That is not leniency. **Whether a shader compiles is a property of the player's machine, not of
the build** — it depends on their driver — so it cannot be a build error, and the game must not
need a shader to run. The server and the unit tests never load one, and a CI runner has no GPU at
all. That is also why `TreatmentConfig` (which passes, in what order, with what numbers) is a
separate file from `Treatment` (the half that talks to the GPU): the first is ordinary data and is
tested; nothing about the second can be, so it has to be looked at.

An unknown `pass` name in `look.json` is skipped the same way, for the same reason: losing one
pass makes the picture plainer, and a look setting is not worth refusing to start over. A
material is the opposite case — [an unknown source there refuses the
file](give-it-a-material.md#when-it-goes-wrong), because a material that loses a uniform draws
something wrong rather than something plainer.
