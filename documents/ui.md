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
| Window | `Window.h` | the frame: title bar or tab strip, pin, collapse and close buttons, resize grip |
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
- **Widgets**: the simple ones are `Bar`, `Field` (label left, value right), `Divider`,
  `Button`, `Chip`; the ones a window lists, chooses and types with are below. A widget is
  a function on `Layout` built from boxes and text, never from coordinates.
- **Adapting.** A layout follows the window, so a window can be any size. For a real change
  of arrangement, branch on the area: the status window puts its two halves side by side
  when it is wide enough (`f.Area().width >= Ui::Px(420)`); the missions window puts its
  list beside the chosen mission at 520, and above it otherwise.
- **A list and its details.** The list keeps a share of the room (`Size::Percent`) and the
  details scroll in a pane of their own, so a long description never pushes the list out of
  the window. Something is always chosen while the list has rows: an empty details pane asks
  for a click nobody knows to make.

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

## The widget library

Every widget takes an id, answers on the frame something happened, and keeps whatever it
must remember between frames (a drag, a number half typed, how long the cursor has rested)
inside the layout. None of them knows what it is showing: the rows, the labels and the
values come from the window's model.

| Widget | Call | Gives back |
|---|---|---|
| Scrolling column | `L.Scroll(Box().Id("list").Grow(), [&]{ ... })` | -- |
| Table | `L.Table(spec)` | `TableEvents`: `hovered`, `clicked`, `rightClicked`, `sorted` |
| Tabs | `L.Tabs("tabs", labels, selected)` | true when `selected` changed |
| Text field | `L.TextField("name", text, options)` | `EditResult`: `changed`, `submitted` |
| Number field | `L.NumberField("range", value, lo, hi, "%.0f")` | true when `value` changed |
| Slider | `L.Slider("zoom", value, lo, hi, logarithmic)` | true while it moves `value` |
| Tooltip | `L.Tooltip("orbit", "Circle it at this range")` | -- |

- **Scroll** lays its children out top to bottom, scrolls them with the wheel, and puts an
  indicator beside them that shows how much there is and where the view is; it can be
  dragged. Its room is kept even when everything fits, so a list that grows past the window
  does not shift sideways. Clay keeps at most ten clipping elements per layout, so a window
  scrolls a list, not every cell of it.
- **Table** is headings, a divider and rows in a `Scroll`:

  ```cpp
  Ui::TableSpec table;
  table.id = "overview";
  table.columns = { { "name", Size::Grow(), Align::Start, true },  // sortable
                    { "dist", Size::Fixed(56.0f), Align::End, true } };
  table.rows = (int)rows.size();
  table.cell = [&](int r, int c) { return Ui::Cell{ ..., t.colors.text }; };
  table.rowFill = [&](int r) { return r == selected ? t.colors.selected : Color{}; };
  table.sort = &sort_;       // a click on a heading changes it; the model reads it
  table.tooltip = [&](int r) { return rows[r].name; };  // the whole of a name cut short
  const Ui::TableEvents ev = L.Table(table);
  ```

  A cell too long for its column ends in `..` (`Ui::Ellipsize`). Clicking the sorted
  heading again reverses the order only if `reversible` -- the overview's model always
  lists hostiles first, which a reversed list would put last.
- **TextField** edits a `std::string` in place. A click takes the keyboard; Enter submits
  and lets go; Esc lets go. `TakeFocus(id)` hands it the keyboard without a click (N on the
  map).
- **NumberField** is text while it is being typed in, starting empty with the old value
  shown dim. Enter, or a click anywhere else, takes the number, clamped to `[lo, hi]`; Esc
  puts the old one back. Paired with a `Slider` on the same value, it is how a window asks
  for a distance (the selected-item window's range).
- **Tooltip** shows its text once the cursor has rested on the element a moment. It is drawn
  by `Ui::DrawTooltip()` at the end of the HUD, after the popups, so nothing covers it.

## A picture inside a window

The radar is a picture, not a layout, and it is still a proper window: its toolbar is laid
out, and the picture gets a box that takes whatever room is left --
`L.Row(Box().Id("scope").Grow(), [] {})`. After `End`, `L.BoxOf("scope")` is where to draw
it, by hand, clipped to that box; input on it reads the frame (`f.Hovered(box)`,
`f.Pressed`, `f.Down`, `f.Wheel`), never raylib directly, and a drag keeps going outside the
window because a press keeps its owner until release. Sizes drawn by hand follow the scale
through `Ui::Scale()`/`Ui::Px`, and colours come from the theme like everything else -- on an
instrument, `standing.*` for whose a thing is, and shape rather than colour for what it is.

## A setting that resizes the interface

The UI scale's slider moves a draft and applies it on release: applied while held, every
window grows under the cursor and the slider runs away from it. Anything a control changes
that moves the control itself works the same way.

## The keyboard

One text field in the whole client holds the keyboard at a time (`Ui::Focus`, owned by the
desk). A field takes it when it is clicked, keeps it while it is drawn, and loses it to
Enter, Esc, a press anywhere, its window closing, or not being drawn any more. Esc goes to
the field before it goes to any window: `Desk::Escape` lets go of the field and closes
nothing. While a field holds it, keys are text: the game asks `desk_.KeyboardTaken()` before
reading a hotkey or a flight key, so typing a W into a name does not fly the ship.

A field needs a frame from the desk to take the keyboard: a window's content gets one; a
surface that lays out a field of its own (the map's name field) asks for
`desk_.SurfaceFrame(id, area)`.

## Actions

What a player can do about a thing is one list, `Actions::For(target, from)` in
`src/game/core/Actions.h`, and every place that offers an action reads it: the right-click
menu, the selected-item window's buttons, the overview's right click. `Game::Perform`
turns a chosen action into a command. Each action names the econagent tool that does the
same thing, and `actions_tests.cpp` checks those tools exist -- a window cannot offer what
an agent cannot do. The verbs that only change what the interface shows (`Select`,
`SetRange`) need no tool; the ones still owed one (`Attack`, `Investigate`) are listed in
the test, and adding the tool is the only way off the list.

A mission is a thing too: `Actions::ForMission` offers `Accept` (an offer, docked, with room)
and `Hand in` (one the server says is ready), matched by `accept_mission` and
`complete_mission`. Where a mission ends is a station, and the missions window offers that
station's own actions from `Actions::For` -- target it, fly there, dock -- rather than
inventing a "set destination" of its own.

## The mouse

One thing owns the mouse each frame: the window in front under the cursor, a screen, a
popup, or the world (#297, slice 1). Inside a window's content:

- `Layout::Button` / `Clicked(id)` / `Hovered(id)` answer only when the window owns the
  mouse, against where the previous frame put the element -- one frame behind, invisible
  at sixty frames a second.
- Hand-written widgets read `f.Pressed()`, `f.Hovered(rect)`, `f.Wheel()`; the older
  `Ui::Slider`/`Toggle`/`Button` read `Ui::MouseScope`, which the window sets.
- Nothing reads `IsMouseButtonPressed` directly: that is how a click went through a window.

## Window behaviour

A window placed on the desk (a spec with a `place`) gets all of this without asking: there is
no flag to turn it on and no code to write in the window. The rules live in `DeskLayout`,
which draws nothing, so `desk_tests.cpp` holds every one of them; `Desk` only turns the
mouse into calls.

| The player | What happens | `DeskLayout` |
|---|---|---|
| drags a title bar near an edge | it snaps to the screen, the menu bar, or another window -- beside it or in line with it, each axis on its own (`metrics.snap` units) | `BeginMove`, `MoveTo`, `SnapOffset` |
| lets go touching another window | the two are a **group**: dragging either moves both, and a press on one brings the group forward | `EndMove`, `Group` |
| holds Shift while dragging | the window leaves its group and goes alone | `BeginMove(h, true)`, `Ungroup` |
| lets go on another window's title bar | it becomes a **tab** of that window: one frame, one place, the tab strip in the title bar | `DropTarget`, `Stack` |
| pulls a tab out of the title bar | it comes away under the cursor as a window of its own | `Unstack` |
| clicks the pin | the window is **pinned**: no drag, no resize, no tab torn out, and Esc passes over it. A group with a pinned window in it stays put; dragging one of the others takes that one out | `SetPinned` |
| clicks the bar button, or double-clicks the title | the window **collapses** to its title bar, keeping its size for when it opens out; the title stays where the top was | `SetCollapsed` |

- **A frame and a group are remembered, not inferred.** Two windows that merely touch are
  not a group, and a group stays one when a collapsed window no longer reaches the next.
- **Stacks share a frame.** Its tabs have one place, one pin and one collapse; the tab in
  front is the open one raised last (`ActiveTab`), and only it is drawn and under the cursor
  (`Shown`). Closing it shows the next; a closed tab drops out of the strip and comes back
  in front when it is opened again. A whole group is not stacked onto anything.
- **A group has one anchor**, chosen from where the whole group is when it is let go, so a
  change of resolution moves it as one rather than pulling the left half left and the right
  half right. Places are in units, so it keeps together at another UI scale too.
- **The menu bar** brings forward a window hidden behind another tab of its stack rather
  than closing it (`Desk::Toggle`).
- **Saved** in `ui_layout.json` with the rest: `pinned`, `collapsed`, and a `group` and a
  `stack` named by the id of one of their windows (plus `tab` order and the `front` tab). A
  group or a stack that comes back with one window in it is forgotten. Reset layout undoes
  all of it.
- A surface is an edge to snap against only if its spec says `snapTarget` (the menu bar);
  it never joins a group.

## Screens cover the windows

A spec with `covers = true` -- the map, the sensor screen, the station -- hides every window
on a lower layer while it is open: they are neither drawn nor under the cursor
(`DeskLayout::Covered`). The map is drawn translucent over the world, and the panels used to
show through it. The menu bar is on a higher layer and stays.

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

On `Ui::Layout`: status, overview, the selected item, missions, radar (its toolbar; the
scope is a picture in a box the layout gives), settings, and the map's name field. F10's
panel and the screens (map, sensor, station) still draw with coordinates. They take the font
and the theme's colours already; they move onto `Ui::Layout` one at a time, and then follow
the scale inside as well as outside.

## Seeing a window

Synthetic input does not reach the client's window, so a picture of one is asked for on the
command line: `econspace connect ... --open radar,missions,settings --uiscale 1.5 --shot
f.png`. `--open` takes the desk's ids, and opening a window this way is remembered in
`ui_layout.json` like opening it by hand.
