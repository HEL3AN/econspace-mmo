# Give it a material

A **material** is a shader plus a list of **bindings**: a uniform name on one side, where its
value comes from on the other ([#121](https://github.com/HEL3AN/econspace-mmo/issues/121)). An
archetype names a material and nothing else changes — an object knows no more about shaders than
it knows about backends.

Everything in the game that is shaded uses one material, `hull`. This page writes a second one,
`glint`, for the relay station from [Compose its look](compose-a-look.md): the same lighting,
plus a sheen that runs along its solar arrays.

- [Write the shader](#write-the-shader)
- [Declare the material](#declare-the-material)
- [Use it, and see it](#use-it-and-see-it)
- [How a material knows where it is](#how-a-material-knows-where-it-is)
- [The sources](#the-sources)
- [When it goes wrong](#when-it-goes-wrong)

---

## Write the shader

A material's shader is a **fragment shader** in `data/shaders/materials/<name>.fs`, GLSL 330.
raylib supplies the vertex stage. Create `data/shaders/materials/glint.fs`:

```glsl
#version 330

// glint -- the hull's lighting, plus a sheen that runs along every elongated part.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;       // item.screenPos  -- this part's centre, pixels, origin bottom left
uniform float radius;       // item.screenSize -- this part's cross section, pixels
uniform vec2  axis;         // item.axis       -- which way the part runs; zero if it is round
uniform vec2  lightDir;     // light.dir
uniform float lightAmount;  // light.strength
uniform float ambient;      // light.ambient
uniform float time;         // clock.time
uniform float speed;        // a constant in materials.json
uniform vec3  sheen;        // a constant in materials.json: the colour of the glint

out vec4 finalColor;

void main()
{
    vec4 texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if (texel.a <= 0.001)
        discard;

    vec2 local = (radius > 0.5) ? (gl_FragCoord.xy - centre) / radius : vec2(0.0);
    bool elongated = dot(axis, axis) > 0.0001;

    // Lit as a cylinder across its axis, or as a sphere if it is round -- the same rule as
    // the hull material, which is what keeps a long part from going dark at both ends.
    vec2  bend = elongated ? vec2(-axis.y, axis.x) * dot(local, vec2(-axis.y, axis.x)) : local;
    float r = min(length(bend), 1.0);
    vec3  normal = normalize(vec3(bend, max(sqrt(max(0.0, 1.0 - r * r)), 0.001)));
    float facing = dot(lightDir, lightDir) < 0.0001
                       ? 1.0
                       : max(0.0, dot(normal, normalize(vec3(lightDir, 0.55))));
    vec3  col = texel.rgb * (ambient + (1.0 - ambient) * facing * lightAmount);

    // The glint: narrow bright stripes travelling along the part. Measured in units of the
    // part's own cross section, so a thin arm and a wide panel get the same pattern.
    if (elongated)
    {
        float along = dot(local, axis);
        float stripe = smoothstep(0.92, 1.0, sin(along * 0.6 - time * speed * 6.2832));
        col += sheen * stripe * 0.6;
    }

    finalColor = vec4(col, texel.a);
}
```

Three habits worth copying from it, and from `hull.fs`, which is the longer, commented original:

- **Start from what raylib would have drawn** — `texture0 * colDiffuse * fragColor` — and only
  modulate it. `fragColor` is the part's colour after its role; the alpha is its coverage and
  any fade. Discarding where it is transparent keeps a fragment outside the shape outside it.
- **Do the lighting yourself.** When a part is drawn through a material, the CPU leaves its
  colour unlit, so that it is not darkened twice. A material that ignores the light draws its
  objects at full brightness on their night side.
- **No direction means lit from everywhere**, not from nowhere: a system with no light, or an
  object exactly between two equal stars, has `lightDir` of zero.

## Declare the material

Add an entry to the `materials` array in `data/materials.json`:

```json
{
    "id": "glint",
    "shader": "glint",
    "bindings": {
        "centre": "item.screenPos",
        "radius": "item.screenSize",
        "axis": "item.axis",
        "lightDir": "light.dir",
        "lightAmount": "light.strength",
        "ambient": "light.ambient",
        "time": "clock.time",
        "speed": 0.4,
        "sheen": [0.55, 0.85, 1.0]
    }
}
```

`id` is how an archetype names it. `shader` is the file stem under `data/shaders/materials/`;
leave it out and it defaults to the `id`. Each binding's **key is a uniform name in the shader**
and its **value is where the value comes from**:

- a **string** is a source — one of the names in [the table below](#the-sources);
- a **number** or a **bool** is a constant `float` (a bool is 0 or 1);
- an **array of one to four numbers** is a constant `float`, `vec2`, `vec3` or `vec4`.

So `speed` and `sheen` are tuned here, in the data, and the same shader can be bound twice under
two ids with different constants. A binding the shader does not declare is skipped, so a
material may list more than one particular shader uses.

## Use it, and see it

In the relay's archetype:

```json
"material": "glint",
```

Relaunch `worldeditor gallery shapes`. The terminal says `Materials: 2 of 2 have a shader`. The
relay's core is lit like everything else — a bright side, a terminator — and pale blue stripes
travel along its two solar arrays.

You can also try a material on anything without editing JSON: select a card, and the
**material** row in the panel has a button per material in `materials.json`. Ctrl+S writes the
choice into `archetypes.json`.

---

## How a material knows where it is

A material does not get geometry of its own. It runs over whatever primitive the backend drew —
a disc, a hexagon, a triangle — and works out where each fragment sits **from `gl_FragCoord` and
the part's centre and radius in pixels**. That is what lets any material sit on any object.

Two things about those numbers:

- **They are per part, not per object**
  ([#135](https://github.com/HEL3AN/econspace-mmo/issues/135)). A composition is shaded one part
  at a time: `item.screenPos` is *this part's* centre, `item.screenSize` its cross section — the
  radius of a round part, half the width of an elongated one — and `item.axis` the direction an
  elongated part runs. An effect "from the middle of the station" would come out once per part.
  A part on a planet's surface is the exception: it gets the **planet's** centre and radius,
  so it shades as part of the ball.
- **They are already in OpenGL's convention.** `gl_FragCoord` counts from the bottom of the
  window, raylib from the top. The backend flips `item.screenPos`, `light.dir` and `item.axis`
  together before handing them over, so a shader uses them as they come. (Flipping the position
  without the light is how the first version lit everything from the mirror image of its star.)

Some things are never drawn through a material at all: a part whose role is `light`, every part
of an object that has a `light` of its own, and any part under about six pixels across, where
there is no surface left to shade.

---

## The sources

| Source | Type | What it is |
|---|---|---|
| `item.color` | `vec4` | the object's colour, 0..1 (the whole object's, not the part's role colour) |
| `item.intensity` | `float` | what is left of it: hull on a ship, ore in a belt, loot in a wreck; 1 is whole |
| `item.heading` | `float` | radians |
| `item.thrusting` | `float` | 1 while its engine burns, else 0 |
| `item.size` | `float` | the object's radius, world units |
| `item.screenPos` | `vec2` | this part's centre on screen, pixels, origin bottom left |
| `item.screenSize` | `float` | this part's radius on screen, pixels |
| `item.axis` | `vec2` | unit vector along an elongated part; zero for a round one |
| `light.dir` | `vec2` | unit vector toward the strongest light at this part; zero if none |
| `light.tint` | `vec4` | the colour of the light arriving |
| `light.strength` | `float` | 0..1, how much of it arrives |
| `light.ambient` | `float` | the floor — nothing is drawn darker |
| `clock.time` | `float` | seconds |

The gallery's **STATE** sliders feed the `item.*` values a static picture never shows:
**intensity** for damage, **heading**, **thrusting**. Drag intensity down to see what `hull`
does to a wreck.

---

## When it goes wrong

**An unknown source refuses the whole file.** Write `"speed": "clock.seconds"` and the terminal
says

```text
WARNING: Materials: material 'glint': unknown source 'clock.seconds' for uniform 'speed'
```

and **every** object in the gallery is drawn plain — not only the relay, because the file is
refused whole. That is deliberate, and it is the opposite of how a screen pass is treated
([next page](add-a-screen-pass.md)). Losing a pass makes the picture plainer. A material that
lost a uniform would be fed zero and draw something actively *wrong* — black, or lit from the
wrong side — and nothing would say why. A loud failure at load is cheaper than that.

**A shader that will not compile costs that material and nothing else.** Whether a shader
compiles is a property of the player's driver, not of the build
([#120](https://github.com/HEL3AN/econspace-mmo/issues/120)), so it cannot be a build error and
must never stop the game:

```text
WARNING: SHADER: [ID 4] Compile error: ERROR: 0:50: '}' : syntax error syntax error
WARNING: Material 'glint': shader would not compile -- drawing plain
INFO: Materials: 1 of 2 have a shader
```

The relay is drawn the way it was before it had a material, and everything on `hull` is
untouched. A shader that compiles but will not **link** -- a function declared and never
defined, say -- is caught the same way, though raylib hands back its default shader rather than
nothing ([#190](https://github.com/HEL3AN/econspace-mmo/issues/190)):

```text
WARNING: SHADER: [ID 5] Link error: ERROR: Undefined function call: "undefinedHelper(f1;)".  Function not found.
WARNING: SHADER: Failed to load custom shader code, using default shader
WARNING: Material 'hull': shader would not link -- drawing plain
INFO: Materials: 0 of 1 have a shader
```

A **missing** `.fs` file is named too (`Material 'glint': no shader at ... -- drawing plain`).
Every one of these is also listed in the F10 panel, under *materials drawn plain* --
`worldeditor gallery shapes settings` opens straight onto it.

**An archetype naming a material that does not exist** is drawn plain, and the client and the
editor say which when they load
([#191](https://github.com/HEL3AN/econspace-mmo/issues/191)):

```text
ERROR: Materials: archetype 'station.relay' names material 'glnit', which is not defined -- drawing it plain
```

It cannot be an archetype load error, because the server and the tests never load materials;
the test suite cross-checks the shipped files instead. A misspelled `material` *field* is
refused when the archetypes load, like any other unknown field.

---

**Next:** [Add a screen pass](add-a-screen-pass.md).
