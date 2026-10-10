# The client's interface

How a window is declared, laid out, themed and given the mouse. The design and its
reasons are in issue #297; this is the part a new screen needs. Everything named here
lives in `src/engine/ui/`.

## The pieces

| Piece | File | What it is for |
|---|---|---|
| Theme | `Theme.h`, `data/ui_theme.json` | every colour, gap, radius and font size, by name |
| Scale | `Theme.h` (`Ui::Scale`, `Ui::Px`) | how many pixels a layout unit is |
| Fonts | `Fonts.h` | the typeface, rasterised at each size it is used at |
| Desk | `Desk.h`, `DeskLayout.h` | which windows exist, where, which is in front, who has the mouse, Esc |
| Window | `Window.h` | the frame: title bar, close button, resize grip |
| Layout | `Layout.h` | rows, columns, text and widgets inside a window, solved by Clay |

## Declaring a window

A window is registered once, in `Game::SetupWindows`, with a spec and a content function:

```cpp
Ui::WindowSpec spec;
spec.id = "missions";              // stable: saved layouts, the menu bar and agents use it
spec.menuLabel = "MIS";            // its button on the menu bar; empty for none
spec.anchor = Ui::Anchor::TopRight;
spec.place = { 16, 360, 264, 264 };  // offset from the anchor, and size -- in units
spec.persist = true;               // where it is and whether it is open are kept per account
spec.resizable = true;             // the corner grip resizes it; the size is kept too
spec.minSize = { 200, 150 };       // units
desk_.AddWindow(spec, "MISSIONS", false, [this](Ui::Frame& f) { DrawMissions(f); });
```

The desk draws the frame and hands the content a `Ui::Frame`: the area to fill, and the
mouse as far as this window is concerned.

## Laying out its content

Each window keeps one `Ui::Layout` (it remembers hover and scroll between frames) and
declares its whole content every frame:

```cpp
void Game::DrawMissions(const Ui::Frame& f)
{
    using Ui::Box;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = missionsLayout_;

    L.Begin(f);
    L.Column(Box().Grow().ScrollY().Id("list").Gap(t.metrics.gap), [&] {
        for (const Mission& m : missions_.Active())
            L.Column(Box().GrowX().Gap(t.metrics.rowGap), [&] {
                L.Text(m.title, Ui::TextStyle::Strong());
                L.Text(m.description, Ui::TextStyle::Body().Wrap());
                L.Bar(ProgressOf(m), t.colors.good);  // a model function, not drawing
            });
    });
    L.End();
    L.Draw();
}
```

- **Boxes.** `Box` is a row by default and a column with `.Column()` (or use `L.Row` /
  `L.Column`). Each axis is `Size::Fit()` (as big as its children), `Size::Grow()` (what
  the parent has left, shared with growing siblings), `Size::Fixed(u)` or
  `Size::Percent(f)`. `.Pad`, `.Gap`, `.Align`, `.Fill`, `.Border`, `.Radius` do what they
  say. `.ScrollY()` clips the children and scrolls them with the wheel.
- **Text** is measured in our font, so a row knows how wide its words are. `.Wrap()` breaks
  at words to the width the text is given. Styles come from the theme: `Small`, `Label`,
  `Body`, `Strong`, `Title`, `Heading`; `.Tint(colour)` changes one.
- **Widgets** so far: `Bar`, `Field` (label left, value right), `Divider`, `Button`, `Chip`.
  More arrive as windows need them; a widget is a function on `Layout` built from boxes
  and text, never from coordinates.
- **Adapting.** A layout follows the window, so a window can be any size. For a real change
  of arrangement, branch on the area: the status window puts its two halves side by side
  when it is wide enough (`f.Area().width >= Ui::Px(420)`).

Rules:

1. **No coordinates and no numbers.** A size or a colour that is not from the theme is a
   reason to add a token to `ui_theme.json`, not to write a literal.
2. **Everything is in units.** A unit is a pixel at scale 1; the layout scales it. Never
   multiply by `Ui::Scale()` inside a layout; outside one (a frame, a hand-placed overlay)
   use `Ui::Px(units)`.
3. **Ids are for asking.** An element needs an id only to be asked about later (`Hovered`,
   `Clicked`, `BoxOf`) or to scroll. Ids are unique within one layout; in a loop, pass an
   index: `Box().Id("row", i)`.
4. **The model has no drawing in it.** What a window lists comes from a pure function a
   test can hold and an agent can read (`Overview::Build` is the pattern). The layout only
   draws what that returns.

## The mouse

One thing owns the mouse each frame: the window in front under the cursor, a screen, a
popup, or the world (#297, slice 1). Inside a window's content:

- `Layout::Button` / `Clicked(id)` / `Hovered(id)` answer only when the window owns the
  mouse, against where the previous frame put the element -- one frame behind, invisible
  at sixty frames a second.
- Hand-written widgets read `f.Pressed()`, `f.Hovered(rect)`, `f.Wheel()`; the older
  `Ui::Slider`/`Toggle`/`Button` read `Ui::MouseScope`, which the window sets.
- Nothing reads `IsMouseButtonPressed` directly: that is how a click went through a window.

## Theme and scale

`data/ui_theme.json` is read strictly: an unknown field or a missing one is a load error
(and the built-in defaults, identical to the shipped file, are used instead). Colours are
`"#RRGGBB"` or `"#RRGGBBAA"`. The `standing` colours are allegiance as the *instruments*
say it -- radar, overview, sensor screen, target panel -- and never an object's colour in
the world view (#117).

`Ui::Scale()` is the player's setting (Settings, saved in `ui_settings.json` next to the
executable) times the display's DPI. It sizes every window frame, every window's place
and size on the desk (kept in units, so a layout made at one scale is the same layout at
another), and everything inside a `Ui::Layout`. `--uiscale S` sets it for one run.

## Fonts

Inter (SIL Open Font License, `data/fonts/OFL.txt`), regular and semibold. A face is
rasterised at exactly each pixel size it is drawn at, the first time, so text is never a
scaled bitmap. `Ui::Text(text, x, y, px, colour)` remains for the screens still placed by
hand; new code uses `Layout::Text`. World glyphs that grow with the camera use
`Ui::GlyphFont()`, one large filtered atlas.

## Clay

Clay (zlib, pinned in `CMakeLists.txt`) solves the layout. Its header refuses C++ before
C++20, so it is compiled in `ClayBridge.c` and nothing else includes it: `Ui::Layout` talks
to the bridge's plain structs. If Clay ever limits us, the bridge is the seam to replace;
no window is written against Clay itself.

## Still placed by hand

Every window except status, and the screens (map, sensor, station), still draw with
coordinates. They take the font and the theme's colours already; they move onto
`Ui::Layout` one at a time, and then follow the scale inside as well as outside.
