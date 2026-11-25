# Players paint their own ships (#552)

The culture palette (#516) stays: seven liveries marked `palette: true` in
`data/materials.json`, picked in the Fit tab or with `set_livery`. This adds a pilot's own
colours *inside* a palette livery's structure. Colour in the world view is art, never an
allegiance tag (#117): the limits below exist to keep it so.

## What a livery is today

A `Render::Livery` is a base colour (multiplies the object's colour), an accent (stripes
and bands), four numbers for the built shader's wear/grime/stripe pattern/panel scale and
four for its style (aspect, patchwork, window warmth, window density). The pilot's choice is
one id string: `ClientSession::livery`, saved as `livery` in the account, sent as
`PlayerView::livery` and as `lv` on each of the pilot's ship entities. The server does not
read materials.json and only checks that an id is well formed.

## Decisions

**1. What a pilot chooses.** The livery's *structure* (stripe pattern, panel scale, wear,
windows) stays the palette livery's. The pilot repaints four things:

- `base` -- the hull plating's tint;
- `band` -- the accent, used for the stripes, bands and spine the pattern draws;
- `pattern` -- which stripe layout: 1 end bands, 2 spine, 3 chevrons (0, flat, is not offered);
- `light` -- the window warmth, 0..3 in four steps (cold, white, warm, amber).

Not offered: wear, grime, panel scale. They are craft, not taste, and a clean hull in a
war zone is a lie the palette already tells.

**2. Storage: the existing field, a longer value.** No new store. `livery` in the account,
the session and `PlayerView`, and `lv` on an entity, hold a *livery spec*: a palette id,
optionally followed by `/` and the paint, e.g. `guild` or `guild/c8b89a2f5fa6p2l1`:
two colours as six hex digits each, then `p<pattern>` and `l<light>`. One string means the
save, the snapshot, the design library's livery reference and `Observation` need no new
field. A spec with no paint is exactly today's value, so an old account loads unchanged and
`ACCOUNT_VERSION` does not need a bump (a new reader reads old files; an older reader, given
a spec it cannot resolve, draws the default livery). `PROTO_VERSION` is bumped because an
`lv` now means more than an id.

**3. What crosses the wire.** Only that string, on the pilot's own ship entities and in
`PlayerView`, as now; about 20 more bytes per painted ship in a snapshot. The server
*canonicalises* a request (clamps to the limits, rewrites the string) and the clients draw
what the server kept, so nobody can send a colour the rules do not allow. The command is
the existing `setLivery` carrying a spec; no new `Command`. The renderer resolves a spec
through `Materials::ResolveLivery(spec)` into a `Livery` (palette entry with the four
fields replaced), cached by spec, so `Item::livery` stays an id string and nothing in the
backends changes shape.

**4. Where the limits live.** `core/Paint.h`, in the engine both the server and the client
link, so the Fit tab painter and the server's snap are one rule, never a copy. (They are not
in `world_rules.json`: the client does not read it, and a rule the two sides could disagree on
would be a bug, not a setting.) Colours are *quantised*, not free RGB: a curated grid of hue (24 steps) x lightness
(5) x saturation (3), like a hand-picked swatch set, so every combination is one the art
direction has seen. The grid is what the Fit tab shows; the string stores the exact value
the grid yields, and the server snaps any colour to the nearest grid cell.

Limits that keep it art and never a tag:

- **Hull tint is quiet.** `base` saturation is capped low (about 0.35) and lightness kept in
  a mid band; plating is metal and paint over it, never a flag. It also multiplies the
  object's own colour, so a bright flat hull cannot happen.
- **Never flat.** `pattern` 0 is refused and `band` must differ from `base` by a minimum
  lightness difference, so a ship is always two-toned: a livery, not a colour field that
  reads at a glance as a side. The band may be bold; that is the art.
- **No black or white extremes.** Lightness is held away from 0 and 1, so a hull never
  disappears against space nor glares.
- **Allegiance stays in the instruments.** Radar, overview, target panel and the sensor
  view draw standing colours from the viewer's own rules, never from paint, and the
  instruments' frames sit over any hull. A player in a lookalike red and black is still
  shown as a friend to a friend. No hue is banned: banning red would be the very faction
  colouring the premise forbids, and what a hue means is never the paint's to say.
- **Per pilot, not per faction.** Paint carries no corporation or faction field, so a
  corporation's look is members choosing alike, never something the game asserts.

**5. Not in this issue.** Decals and emblems (a corporation logo is a separate, moderated
asset question), per-hull paint (one livery per pilot, as today), a paint market or unlock
(painting is free; if it becomes a sink, that is an economy decision), painting stations.

## Compared with others

- **EVE (SKINs).** Per hull, bought as licences, from a designer's patterns; visible to
  all. We take: a curated set that cannot clash, a look that is the pilot's, never a
  rule. We leave: the licence economy. Corp and alliance logos are an overlay we defer.
- **Elite Dangerous.** Preset palettes (primary, secondary, wear) picked from a swatch set,
  the closest cousin of this design: our four fields are its three, plus the pattern.
- **Warframe.** Colour picked from a fixed grid of hand-made swatches in four slots; same
  reason as our quantised grid.
- **Star Citizen, X4.** Paints as unlocks or mods; X4 lets players mod freely and fleets
  end up unreadable. The limits above are what keeps a crowded system legible.

## Slices

1. **Storage and wire.** `Materials::ResolveLivery`, the `paint` rules in
   `world_rules.json`, the server's snap in `Simulation::SetLivery`, `PROTO_VERSION` bump,
   tests (snap, round trip, an old save).
2. **The painter in the Fit tab.** Swatch grids for base and band, pattern and light
   buttons, a preview of the hull as it will be drawn, over the existing `livery_*` row.
3. **econagent.** `set_livery` accepts the paint (`base`, `band`, `pattern`, `light`),
   `observe` reports the pilot's livery spec and the allowed ranges.

## Failure rules

A spec from a client that parses is snapped; one that does not is refused with a notice and
the pilot's livery is left as it was. A spec in a save that does not parse costs the paint,
not the palette, with a notice on login. The renderer resolves specs into a ring of 512
slots, so a crowd of distinct paints never grows the cache.

## Built

All three slices: storage and wire (#599), the Fit tab painter (#603), and econagent's
`set_livery` (`hull`, `band`, `pattern`, `light` beside the palette id; the answer says what
the server kept) with `observe` naming the livery on the SHIP line.
