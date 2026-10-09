# Compose its look

An object is a **composition**, not a figure
([#122](https://github.com/HEL3AN/econspace-mmo/issues/122)). A trade hub is a hexagonal core,
three arms with pads on them, a docking ring, a row of lamps and a mast — and none of that is in
C++. It is a list of parts in the archetype's `shape`, and the backend assembles them.

This page gives the relay station from [Add an archetype](add-an-archetype.md) a body, one idea
at a time, then does the same for a planet, whose surface works differently.

- [The finished relay](#the-finished-relay)
- [Built up part by part](#built-up-part-by-part)
- [The parts](#the-parts)
- [A planet: parts on a sphere](#a-planet-parts-on-a-sphere)
- [Rules learned by getting them wrong](#rules-learned-by-getting-them-wrong)

---

## The finished relay

Add `shape` to the `station.relay` entry:

```json
"shape": [
    {"form": "polygon", "sides": 8, "radius": 0.45, "role": "hull"},
    {"form": "polygon", "sides": 8, "radius": 0.3, "role": "panel", "angle": 22.5, "minPixels": 40},
    {"form": "lattice", "at": [0.0, 0.6], "angle": 90, "length": 0.4, "width": 0.22, "count": 3, "role": "trim", "mirror": true, "minPixels": 40},
    {"form": "bar", "at": [0.0, 1.15], "angle": 90, "length": 0.75, "width": 0.5, "role": "panel", "mirror": true},
    {"form": "capsule", "at": [0.75, 0.0], "length": 0.6, "width": 0.07, "role": "antenna", "minPixels": 50},
    {"form": "chevron", "at": [1.1, 0.0], "angle": 180, "length": 0.3, "width": 0.55, "role": "trim"},
    {"form": "ring", "radius": 0.62, "width": 0.05, "role": "trim", "spin": -8.0, "minPixels": 40},
    {"form": "disc", "at": [1.25, 0.0], "radius": 0.06, "role": "light", "blink": 1.6, "minPixels": 50},
    {"form": "disc", "at": [0.32, 0.32], "repeat": 4, "radius": 0.04, "role": "light", "blink": 2.2, "minPixels": 70}
]
```

Relaunch `worldeditor gallery shapes`. The relay is now an octagonal core inside a slowly
turning ring, a solar array above and below on short trusses, and a mast with a dish pointing
right, its lamp blinking.

**Every measurement is a fraction of the object's own radius.** `"radius": 0.45` is 45% of
whatever size this relay turns out to be, so the same shape works for a four-hundred-unit relay and a
four-thousand-unit one, and a shape is written once.

Parts are drawn **in the order they are written**, later on top — which is why the darker inner
octagon comes after the hull it sits on.

---

## Built up part by part

### The core: roles, not colours

```json
{"form": "polygon", "sides": 8, "radius": 0.45, "role": "hull"},
{"form": "polygon", "sides": 8, "radius": 0.3, "role": "panel", "angle": 22.5, "minPixels": 40}
```

A part never has a colour of its own. It has a **role**, and the role is its relationship to the
object's colour:

| Role | Drawn as |
|---|---|
| `hull` | the object's colour |
| `panel` | darker — recessed, plating, in shadow |
| `trim` | lighter — edges and highlights |
| `light` | emissive: never shaded by a star, dimmed only by damage and its own blink |
| `antenna` | thin and dim — masts, aerials, struts |

This is why the same shape works whatever colour the object ends up — and colour is an art
decision that is not the object's to keep
([#117](https://github.com/HEL3AN/econspace-mmo/issues/117)). Turning the inner octagon by
22.5° sets its corners between the outer one's.

### The arrays: `mirror`, not `repeat: 2`

```json
{"form": "lattice", "at": [0.0, 0.6], "angle": 90, ..., "mirror": true},
{"form": "bar", "at": [0.0, 1.15], "angle": 90, ..., "mirror": true}
```

There are two symmetries, and they are not interchangeable:

- **`repeat: n`** draws the part *n* times, each turned a further 360/*n* degrees **about the
  object's centre**. That is what a station is: arms, pads, lamps, vents.
- **`mirror: true`** draws it again **reflected across the object's own axis** (its *x* axis,
  the way its nose points). That is what a ship is.

For the relay's arrays, straight up and straight down, the two happen to agree. For anything
at an angle they do not. The player's ship has a swept wing at `"at": [-0.5, 0.62], "angle":
145` with `mirror: true`; write it as `repeat: 2` instead and the second wing is the first one
rotated half a turn — **in front of the nose, pointing forward**. That is what it did the first
time a ship was written that way. Try it in the gallery on `ship.player`; it is a quick way to
see the difference for good.

### The mast and the dish: placing one part

```json
{"form": "capsule", "at": [0.75, 0.0], "length": 0.6, "width": 0.07, "role": "antenna", "minPixels": 50},
{"form": "chevron", "at": [1.1, 0.0], "angle": 180, "length": 0.3, "width": 0.55, "role": "trim"}
```

`at` is the part's centre, in radii from the object's. `angle` turns the part about its own
centre, in degrees. An elongated part runs along its angle: a chevron's tip points that way, so
`180` turns this one's point back toward the core and leaves its wide base facing out — a dish.

### The ring and the lamps: things that move

```json
{"form": "ring", "radius": 0.62, "width": 0.05, "role": "trim", "spin": -8.0, "minPixels": 40},
{"form": "disc", "at": [1.25, 0.0], "radius": 0.06, "role": "light", "blink": 1.6, "minPixels": 50},
{"form": "disc", "at": [0.32, 0.32], "repeat": 4, "radius": 0.04, "role": "light", "blink": 2.2, "minPixels": 70}
```

- `spin` is degrees per second about the object's centre; negative turns the other way.
- `blink` is seconds per cycle, for a `light`. Each lamp takes its **phase from its own seed**,
  so the four lamps are a sequence rather than a pulse — lamps in step read as a screensaver. A
  blink never reaches zero, because a lamp that goes out reads as a part that fell off.
- `onlyThrusting: true` (not used here) makes a part exist only while the engine burns — a
  ship's exhaust.

All of it is a function of the clock and the seed, never of anything accumulated frame by frame
([#136](https://github.com/HEL3AN/econspace-mmo/issues/136)). An angle that were integrated per
frame would drift between clients, and two players would see the same station turned
differently. As written, the picture depends on nothing but the clock and the object, and nothing
about motion crosses the wire. (The clock today is each client's own time since it started, so
two clients agree on how a ring turns but not yet on where it is in its turn.)

### Detail with distance: `minPixels`

A part with `minPixels` is drawn only once the **whole object** is at least that many pixels
across on screen. On the system map a station is eight pixels, and a dozen parts resolved into
eight pixels is a smudge, not a small station. So: the silhouette always, the panels from about
forty pixels, the lamps from sixty or seventy.

**The gallery shows less than you might expect.** A card draws an object about 77 pixels across
when its shape stays inside its radius, and less when it reaches beyond — the card frames how far
the shape actually reaches. The relay reaches 1.5 radii, so its card is about 50 pixels across,
and the four lamps at `minPixels: 70` never appear in it. To check a fine part, lower its
`minPixels` while you look, and put it back.

---

## The parts

Eight forms. Each reads only the measurements listed for it; the rest are ignored.

| `form` | Reads | Notes |
|---|---|---|
| `disc` | `radius` | bodies, pads, lamps |
| `ring` | `radius`, `width` | `radius` is the outer edge; `width` is the thickness, inward |
| `polygon` | `sides`, `radius`, `filled`, `width` | `radius` reaches the corners; `filled: false` draws the outline `width` thick |
| `capsule` | `length`, `width` | a rounded bar along `angle` — arms, masts, booms |
| `chevron` | `length`, `width` | a triangle, tip along `angle`, base `width` wide — noses, fins, flare |
| `bar` | `length`, `width` | a rectangle, `length` along `angle` — panels, plating |
| `lattice` | `length`, `width`, `count` | two rails `width` apart and `count` struts between — trusses |
| `band` | `lat`, `width` | a latitude band on a sphere — planets only, below |

And what every part may carry:

| Field | Default | |
|---|---|---|
| `role` | `hull` | see above |
| `at` | `[0, 0]` | centre, in radii |
| `angle` | 0 | own rotation, degrees |
| `repeat` | 1 | copies turned evenly about the centre |
| `mirror` | false | a copy reflected across the object's axis |
| `minPixels` | 0 | drawn only from this many pixels across |
| `alpha` | 1 | how solid; a corona is a glow, not a ring |
| `jitterAngle`, `jitterScale` | 0 | how far the object's id may turn (degrees) and resize (fraction) this part, so two of a kind differ |
| `spin`, `blink`, `onlyThrusting` | 0, 0, false | movement, above |
| `orbitRadius`, `orbitPeriod`, `orbitPhase`, `orbitTilt` | 0, 60, 0, 0.35 | the part travels round the object instead of sitting on it, below |
| `lat`, `lon` | — | giving either puts the part on a sphere, below |

Size defaults are `radius` 1, `width` 0.1, `length` 1, `sides` 6, `count` 3. An unknown `form`
or `role` refuses the whole archetype file, and so does an unknown *field*
([#191](https://github.com/HEL3AN/econspace-mmo/issues/191)).

**The vocabulary is narrow on purpose.** A few primitives with strict proportions make a family
of objects that looks intentional; an open-ended set of arbitrary shapes reads as programmer
art. Like a font: few strokes, many letters. A new form is a backend change and a design
decision, not a convenience.

---

## A planet: parts on a sphere

A station is flat. A planet is a ball that turns, and its surface goes across the face, round
the limb and out of sight ([#166](https://github.com/HEL3AN/econspace-mmo/issues/166)). Here is a
small gas world:

```json
{
    "id": "planet.storm",
    "name": "Storm World",
    "kind": "Planet",
    "glyph": "O",
    "color": [120, 150, 190, 255],
    "layer": 1,
    "size": 20000,
    "material": "hull",
    "shape": { "tilt": 24, "parts": [
        {"form": "disc", "radius": 1.0, "role": "hull"},
        {"form": "band", "lat": 38, "width": 12, "role": "panel", "mirror": true, "minPixels": 30},
        {"form": "band", "lat": 0, "width": 16, "role": "trim", "alpha": 0.5, "minPixels": 30},
        {"form": "disc", "lat": 18, "lon": 0, "repeat": 3, "radius": 0.14, "role": "trim", "spin": 8.0, "minPixels": 40},
        {"form": "disc", "radius": 0.1, "role": "trim", "orbitRadius": 1.6, "orbitPeriod": 40.0, "orbitTilt": 0.3, "minPixels": 40}
    ] },
    "components": {}
}
```

What is different:

- **The shape is an object, not a list.** `tilt` is how far the north pole leans toward the
  viewer, in degrees, and it belongs to the body because every feature on one planet shares one
  axis. It is what makes a band **curve**: edge-on, latitudes are straight chords and the planet
  reads as a disc with stripes; tipped a little, they bow and it reads as a ball. A station has
  no axis and stays a bare list. (Without one, `tilt` is 18.)
- **`lat` or `lon` puts a part on the sphere.** It is projected: carried across the face by the
  planet's turn, squashed into an ellipse near the limb, and absent while it is round the back.
  On the sphere, `spin` means degrees of **longitude** per second — how fast the planet turns.
- **A `band` is always on the sphere.** `lat` is its middle and `width` is in **degrees** of
  latitude. It is the visible part of a latitude strip, solved exactly, so it narrows to nothing
  at the poles and never crosses the limb. It does not move when the planet turns, and that is
  right: a gas giant's belts are the same all the way round, and it is the storms that go by.
- **`mirror` reflects across the equator**, so one line gives the band at 38° north and its twin
  at 38° south. **`repeat` spreads copies evenly in longitude**, so the three storms are a third
  of the way round from each other.
- **A surface feature is lit as the planet**, with the planet's centre and radius, so it falls
  into the planet's night side. Shaded as itself, a storm would carry a small lit side of its own
  into the dark.
- **An orbiting part** — `orbitRadius` above zero — leaves the surface for the space around it:
  a moon ([#165](https://github.com/HEL3AN/econspace-mmo/issues/165)). It travels an ellipse
  squashed by `orbitTilt` (0 edge-on, 1 seen from above), passing behind the body on the far half
  and in front on the near half. `at` is ignored.

That this is a `Planet` changes nothing about how it is drawn; the shape does all of it. To see
it in the game it needs a planet type, by the same route as [a station
role](add-an-archetype.md#making-the-game-build-one).

---

## Rules learned by getting them wrong

Each of these was a picture that looked wrong first.

- **A pair of wings is `mirror`, not `repeat: 2`.** Rotated half a turn, the second wing ends up
  in front of the nose ([#122](https://github.com/HEL3AN/econspace-mmo/issues/122)).
- **A material is bound per part, not per object.** One sphere at the centre is the truth about a
  planet and a lie about a station: an arm two radii out was being shaded by whatever slice of
  that sphere it happened to stand in. Each part is now lit at its own centre and its own cross
  section, and an elongated part as a cylinder along its axis — lit as a ball, a truss had both
  ends in shadow ([#135](https://github.com/HEL3AN/econspace-mmo/issues/135)).
- **A part less than about six pixels across is drawn flat** (its radius, or half its width,
  under three). There is no surface left to shade, and a rail two pixels wide is *entirely* the side of a cylinder that faces away — it came out the
  darkest thing on screen.
- **Motion is `f(time, seed)`.** Never accumulated, and the turn is wrapped, because a float
  counting degrees for a week has no precision left
  ([#136](https://github.com/HEL3AN/econspace-mmo/issues/136)).
- **Craters that moved read as moons** — so moons became a thing of their own, and the depth is
  nothing but draw order. That means a moon larger than its body, or orbiting inside it, will
  *pop* rather than slide behind. Keep an orbit outside the body
  ([#165](https://github.com/HEL3AN/econspace-mmo/issues/165)).
- **A planet's surface turns about an axis in the picture, not like a wheel.** Spinning a crater
  about the disc's centre is not what a planet does, and spinning a band in two dimensions made
  it a stripe sweeping across the planet. Hence `lat`/`lon`, and bands that stay put
  ([#166](https://github.com/HEL3AN/econspace-mmo/issues/166)).
- **A shape may reach past its radius**, and a docking ring at 1.55 is the point of a docking
  ring. Anything that frames an object — a gallery card, a selection highlight — asks
  `Render::Extent` instead of assuming one radius.

---

**Next:** [Give it a material](give-it-a-material.md).
