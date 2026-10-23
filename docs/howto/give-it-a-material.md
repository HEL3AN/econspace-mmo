# Give it a material

A **material** is a shader plus a list of **bindings**: a uniform name on one side, where its
value comes from on the other ([#121](https://github.com/HEL3AN/econspace-mmo/issues/121)). An
archetype names a material and nothing else changes — an object knows no more about shaders than
it knows about backends.

Everything built in the game is shaded by one material, `hull`. The natural bodies -- planets,
moons, stars, belts, nebulae -- are each *painted* by a material of their own, which draws the
whole surface rather than lighting a shape ([below](#paint-a-whole-body)). This page first
writes a second material for a structure, `glint`, for the relay station from
[Compose its look](compose-a-look.md): the same lighting, plus a sheen that runs along its
solar arrays.

- [Write the shader](#write-the-shader)
- [Declare the material](#declare-the-material)
- [Use it, and see it](#use-it-and-see-it)
- [How a material knows where it is](#how-a-material-knows-where-it-is)
- [Paint a whole body](#paint-a-whole-body)
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

Some things are never drawn through the object's material: a part whose role is `light`, every
part of an object that has a `light` of its own, and any part under about six pixels across, where
there is no surface left to shade. A part with a material of its own is the exception to the
second -- a star's photosphere is exactly that ([next](#paint-a-whole-body)).

---

## Paint a whole body

`hull` lights a shape someone drew. A planet has no shape worth drawing: what makes it read as a
world is coastlines, cloud, ice and air, and a composition of discs can only gesture at those --
rings of circles for islands, stripes for a gas giant's belts
([#426](https://github.com/HEL3AN/econspace-mmo/issues/426)). So a natural body is **painted**: one
part of it -- its disc -- carries a material of its own, and that shader draws the entire surface.

```json
"material": "hull",
"shape": { "tilt": 20, "parts": [
    {"form": "disc", "radius": 1.0, "role": "hull", "spin": 4.0, "material": "planet.oceanic"},
    {"module": "planet.continent", "lat": [20, 28], "lon": 8, "scale": 0.4, "spin": 4.0, "fallback": true},
    {"form": "band", "lat": 85, "width": 10, "role": "trim", "fallback": true},
    {"form": "disc", "radius": 0.14, "role": "trim", "orbitRadius": 1.65, "orbitPeriod": 4600.0, "material": "moon"}
] }
```

- **`material` on a part** paints that part with its own material instead of the object's. Its
  inputs are the part's: its centre, its radius, its `spin` turned into `item.angle` -- which is
  how a planet's surface turns -- and its own seed.
- **`fallback: true`** marks a part as the stand-in for the painting. It is drawn only while the
  body is *not* painted -- the shader would not compile on this driver, materials are switched off
  (`worldeditor gallery shapes noshaders`), or the body is a few pixels across -- and skipped
  while it is. **Every painted body must keep a stand-in that still reads**: whether a shader
  compiles is the player's driver's business (#120). With no shader at all, the painted disc
  itself falls back to the offset discs and rim the backend lit planets with before shaders.
- **A moon is not the body.** An orbiting part's material says nothing about the planet's
  stand-ins, so a moon of rock can circle a planet of sea.

### The shader

`data/shaders/materials/terran.fs` is the one to read first. The pieces every painted body
needs are in a library, `body.glsl`, which a shader includes after its uniforms:

```glsl
uniform float seed;   // item.seed
uniform float turn;   // item.angle
...
out vec4 finalColor;

#include "body.glsl"
```

The material library pastes the file in -- GLSL has no include of its own. It provides noise
(`bNoise`, `bFbm`, `bRidge`, `bCells`), the sphere under the disc (`bNormal`, `bToBody` -- the
body's frame, tilted and turned), light with a soft terminator (`bDiffuse`), air on the limb
inside and out (`bAirInside`, `bAirOutside`), craters that bend the normal (`bCraterCells`) and
an antialiased limb (`bCoverage`). Its noise reads a table rather than hashing: bind it.

```json
{
    "id": "planet.oceanic",
    "shader": "terran",
    "reach": 1.06,
    "bindings": {
        "centre": "item.screenPos", "radius": "item.screenSize",
        "seed": "item.seed", "turn": "item.angle", "noiseTex": "texture.noise",
        "sea": 0.56, "deep": [0.02, 0.07, 0.18], ...
    }
}
```

- **`reach`** is how far past its part the material draws, in the part's radii (1 to 8): a
  planet's air is a haze beyond its limb, a star's corona a few radii out. The backend draws the
  primitive that much larger; the shader is still told the part's own radius, so the limb stays
  at 1 and the shader decides where it is, to the pixel.
- **Seed, not paint.** Where the land is, how much cloud, how cold, which way a palette leans --
  all from `seed`, so two ocean worlds are two places. The palette's ends are constants here.
- **Cost follows size.** `bOctaves(radius, most)` gives a dot on the map two octaves and a planet
  filling the screen seven. Work that only matters somewhere -- a cap's torn edge, a land's grain
  -- is done only there. And a body covering more of the screen than the backend can afford to
  paint each frame (`paintBudget` in `data/look.json`, millions of pixels, default 1) is painted into an offscreen target at
  reduced resolution -- never below half -- and scaled back up.
- **Motion is a loop.** Anything that churns -- cloud, granulation, gas -- goes round `bLoop(time,
  period, ...)` with a period that divides the 16384 s the backend wraps the clock at, and a
  body's turn comes from its part's `spin` through `item.angle`, which the composer wraps.
  Nothing is accumulated, so two clients see the same planet (#136).
- **A star lights itself.** `star.fs` reads no light: it is the light (#119).

---

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
| `light.tint` | `vec4` | the colour of the light arriving: for a rim or a glow |
| `light.surface` | `vec4` | what that light does to an albedo -- its hue capped by `Lighting::SURFACE_SATURATION` (#361) |
| `light.strength` | `float` | 0..1, how much of it arrives |
| `light.ambient` | `float` | the floor — nothing is drawn darker |
| `clock.time` | `float` | seconds |
| `item.seed` | `float` | 0..1, the object's own, from its id: two planets of a kind differ |
| `item.partSeed` | `float` | 0..1, this part's own: one rock in a belt from the next |
| `item.angle` | `float` | radians, this part's own turn with its spin: how far a planet has turned |
| `texture.noise` | `sampler2D` | a fixed 256 x 256 table of random values that `body.glsl` reads noise from |

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
