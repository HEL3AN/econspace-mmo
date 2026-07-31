# EconSpace — Textures: Prompts and Sizes

A reference for generating sprites (Nano Banana and the like) and assembling them in
Photoshop. The project's style is **pixel art**.

## General Rules (important for all files)

- **Format:** PNG with an alpha channel (PNG-32), **fully transparent background**.
- **Canvas:** square, size a power of two (see the table below).
- **Object centered**, filling ~85–90% of the canvas, with a small transparent margin
  (~5%) around the edges. An object that is too small will look tiny in-game.
- **Strictly top-down view**.
- **No background, no text, no captions, no frames, no shadow cast on a background.**
- Crisp pixels, no anti-aliasing.
- If the generator outputs a larger size — downscale in Photoshop using **Nearest
  Neighbour** (so the pixels stay crisp).
- **The file name — exactly as in the table, in lowercase**, placed in
  `data/textures/`. There is no need to rebuild the game — textures are loaded from
  `data/`.

## Base Prompt Fragment (append to every prompt)

```
Pixel art, top-down view, single object centered and filling about 90%
of a square canvas. Fully transparent background (PNG alpha), no
background, no text, no labels, no frame, no drop shadow. Crisp pixels,
no anti-aliasing, limited dark sci-fi color palette.
```

---

## 1. NEEDED NOW (the code can already use them)

The file names are hardwired into the code — do not change them.

| File | Size | Object |
|------|--------|--------|
| `star.png` | 256×256 | Star |
| `planet.png` | 256×256 | Planet |
| `station.png` | 256×256 | Station |
| `asteroids.png` | 256×256 | Asteroid belt |
| `ship.png` | 128×128 | Ship (player and NPC) |

### star.png

```
A single glowing star seen from above, perfectly circular. A bright
near-white core with a soft warm corona radiating outward. Neutral
white-yellow tones (it will be color-tinted in engine). The corona is
included inside the canvas.
```
> Tinted in code (yellow/red/blue) — keep the color a neutral white.

### planet.png

```
A single planet seen from above, a clean sphere with soft spherical
shading. Desaturated neutral grey-blue surface with subtle craters and
light surface detail. Kept low-saturation so it can be color-tinted.
```
> Also tinted — do not make it brightly colored.

### station.png

```
A space station seen from above: an angular modular sci-fi structure
with docking arms, solar panels and small warm window lights. Metallic
grey and steel-blue tones. Symmetrical, reads clearly at small size.
```
> Not tinted — draw it in its own colors.

### asteroids.png

```
A cluster of asteroids seen from above: a loose group of 6 to 10 grey
rocks of varied sizes, scattered in a roughly circular formation.
Rough cratered stone, grey and grey-brown tones.
```

### ship.png

```
A small spaceship seen from above (top-down), NOSE POINTING UP toward
the top edge of the canvas. A compact agile fighter: metallic grey hull
with subtle panel detail, small engine nozzles at the bottom rear.
Neutral grey (it will be color-tinted in engine).
```
> **Nose strictly up** — the code rotates the sprite along the heading from this
> orientation. Tinted (the player — white, NPCs — the faction color).

---

## 2. USEFUL FOR THE FUTURE (the code does not use them yet — tell me when to hook them up)

### Planet Variants

Instead of tinting a single planet — separate types. All 256×256.

| File | Prompt addition |
|------|---------------|
| `planet_rocky.png` | `a rocky barren planet, brown and grey craters` |
| `planet_gas.png` | `a gas giant with swirling cloud bands, warm orange-tan` |
| `planet_ice.png` | `an icy planet, pale blue-white with frozen cracks` |
| `planet_lava.png` | `a volcanic planet, dark crust with glowing red lava veins` |

### Ship Variants (per the catalog: Scout / Courier / Hauler / Miner)

All **nose up**, neutral grey, top-down.

| File | Size | Prompt addition |
|------|--------|---------------|
| `ship_scout.png` | 128×128 | `a tiny nimble scout ship, minimal and sleek` |
| `ship_courier.png` | 128×128 | `a fast courier ship, elongated aerodynamic hull` |
| `ship_hauler.png` | 192×192 | `a bulky cargo hauler, boxy hull with container pods` |
| `ship_miner.png` | 160×160 | `an industrial mining ship with drilling arms and tanks` |

### Menu Panel Icons (Neocom)

Currently the panel uses text (STA/TGT/OVR/RAD/SET) — it can be replaced with icons.
All **64×64**, monochrome/light, simple readable silhouettes.

| File | Prompt addition |
|------|---------------|
| `icon_status.png` | `a ship status / heartbeat icon` |
| `icon_target.png` | `a targeting reticle icon` |
| `icon_overview.png` | `a list / rows icon` |
| `icon_radar.png` | `a radar sweep / sonar icon` |
| `icon_settings.png` | `a gear / cog icon` |

### Effects

Engine exhaust, muzzle flashes, and explosions I plan to do **in code** (particles) —
there is no need to generate them. But if you want them as a sprite:

| File | Size | Prompt addition |
|------|--------|---------------|
| `projectile.png` | 32×64 | `a glowing energy bolt, vertical, bright core` |

---

## 3. How to Lay Them Out in Photoshop

- **Each object — a separate file** of its own size from the table.
- Create a document of the required size (for example 256×256), with the background
  **transparent** (not white, not black).
- The object — centered, ~90% of the area, with a transparent margin around it.
- Downscale a generation from a high resolution to the target size via **Image Size →
  Resample: Nearest Neighbour**.
- Save: `File → Export → PNG`, with transparency, name strictly from the table.
- The finished files — in `data/textures/`.

> The atlas (`atlas.png`) is no longer needed — separate files take priority and load
> without a chroma key. If you place both an atlas and separate files — the code will
> take the separate ones.
