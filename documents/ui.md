# The client's interface

How a window is declared, laid out, themed and given the mouse. The design and its
reasons are in issue #297; this is the part a new screen needs. Everything named here
lives in `src/engine/ui/`.

Status (revised 2026-10-23): **a current reference for what is built.** The framework of
#297 is complete and that issue is closed; what is left is polish from the 0.1 playtest
(#427). Since this file was last rewritten (2026-10-11) the scanner (#414), chat (#410) and
corporation (#416) windows were added the same way, `Game::SetupWindows`, and nothing about
the framework changed.

## The pieces

| Piece | File | What it is for |
|---|---|---|
| Theme | `Theme.h`, `data/ui_themes/*.json` | every colour, gap, radius, font size and typeface, by name; switchable |
| Paint | `Paint.h` | what a theme's renderer tokens draw: cut and rounded corners, glow, shadow, dashes, stripes, the blur |
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
spec.menuLabel = "Missions";       // its button on the menu bar; empty for none
spec.menuTip = "Missions  (J)";    // what the button says when the cursor rests on it
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
  `Body`, `Strong`, `Title`, `Heading`, `Caption`; `.Tint(colour)` changes one.
- **Widgets**: the simple ones are `Bar`, `Field` (label left, value right), `Divider`,
  `Button`, `Chip`; the ones a window lists, chooses and types with are below. A widget is
  a function on `Layout` built from boxes and text, never from coordinates.
- **A form** is `FormRow` lines: `L.FormRow("Resolution", [&] { ...buttons... })` puts the
  label in a column `metrics.formLabel` wide and the controls beside it, so every line of
  the form lines up (a label longer than the column wraps under itself); an empty label puts
  a note under the controls (Settings). Full-width
  buttons under a heading left most of a settings window empty.
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
   reason to add a token to the theme files, not to write a literal.
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
  does not shift sideways. Its children are laid out exactly as wide as its view was the
  frame before: the view clips sideways, and a clipped axis let them be as wide as they liked,
  so a note meant to wrap ran past the window's edge and took every row with it. Clay keeps at most ten clipping elements per layout, so a window
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
`SetRange`) need no tool; every other verb has one (`Investigate` is `salvage`). A new verb
that ships before its tool goes on the test's `owed` list, which is empty now, and adding
the tool is the only way off it.

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

## Where a window opens

A spec's `place` is chosen for 1280 x 720 at a scale of 1, and at another size or scale two
defaults can land on each other -- the settings window on the missions, the hangar on the
missions. So a window that opens where its spec puts it is placed by a rule rather than
trusted (`DeskLayout::Arrange`, once a frame): a resizable one is first shortened to end on
the screen, as far as its minimum lets it; then, if it covers a window already shown on its
layer, it moves to the nearest free place beside, above or below one of them, in line with
an edge, `metrics.gap` away (`FreeSpot`) -- or, with no room anywhere, to where it hides
least (`LeastCovering`). What opened first keeps its place, and where the moved one went is
kept like a move. A window the player has moved or resized is never touched: the rule is for
where the game puts things.

The menu bar names each window in a word (`spec.menuLabel`), and says its key when the
cursor rests on it (`spec.menuTip`); it is laid out like a window. Where the ship is -- the
system, its security, who wants the pilot -- is a plate at the top of the screen drawn
*under* the windows, so a window over it hides it instead of the two running into each
other.

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

A spec with `covers = true` -- the map, the sensor screen -- hides every window on a lower
layer while it is open: they are neither drawn nor under the cursor (`DeskLayout::Covered`).
The map is drawn translucent over the world, and the panels used to show through it. The
menu bar is on a higher layer and stays.

A screen is a picture with furniture around it. The furniture -- the heading, the legend,
the news -- is a `Ui::Layout` over the whole screen, with a box (`Id("graph")`, `Id("grid")`)
that takes whatever room it leaves; the picture is drawn by hand in `BoxOf` that box, as the
radar's scope is in its window. The layout gets its mouse from `desk_.SurfaceFrame(id,
area)`, so the news scrolls only while the map owns the mouse. A side column is
`metrics.sidePanel` wide, and its text wraps: a long line in the news used to run off the
right of the screen. When the picture has to be made before the legend that lists what is in
it (the sensor screen scans, then lists what it found), it uses the box from the frame
before, and a guess on the first frame.

## Where a window belongs

Some windows only mean something in one place: a market is the station's, an overview looks
at space. A spec says so with `context` (`"docked"`, `"space"`; empty for everywhere), and the
game turns the contexts on and off at the start of every frame
(`desk_.SetContext(CTX_DOCKED, docked)`). Out of its context a window is closed, off the menu
bar and never under the cursor, but **whether it was open is remembered**: undocking puts the
market away and docking again brings it back where it was, and the file keeps it as the
player left it, because where the player is is not something they arranged. Every context is
off until it is turned on. Opening a window out of its context (`--open market` while flying)
only promises to open it when the context returns. The rule is `DeskLayout`'s, and
`desk_tests.cpp` holds it.

## Docked

Docked, there is no world to draw: the station's hall stands in for it (`DrawStationHall`,
`colors.backdrop`, the station's name large and quiet), and the windows stand in the hall.
The station is not a screen any more but windows in the `docked` context, each resizable, and
grouped or stacked as tabs like any other:

| Window | Id | What it is for |
|---|---|---|
| Station | `station` | whose it is and what they think of this pilot, the account, a bounty to pay, **Undock** |
| Market | `market` | a table of commodities (price, hold, what the hold is worth here); sell an amount or all |
| Hangar | `hangar` | a table of hulls (flown, owned, or the price here); switch, or buy |
| Missions | `missions` | the same window as in space, opened on the board's offers or on a mission to hand in |

Status, missions, settings and the map are in no context and stay; the selected item, the
overview, the radar and the sensor screen are `space`'s and come back on undocking as they
were. The station window always opens on docking, because it is where undocking is; the
missions window opens when the station has work or a mission can be handed in.

What the market and hangar list is `StationList` (`src/game/core/StationList.h`), tested in
`station_list_tests.cpp`: a sale pays the price times the trading skill times the standing
multiplier, the server's sum, and a hull whose hold is smaller than the cargo, or that costs
more than the account holds, says so instead of offering a button the server would refuse.

## Theme and scale

A theme is one whole file in `data/ui_themes/`, named by its id: colours, metrics, font
sizes, the four faces and how text is set. The player picks one in Settings (Interface,
Theme), and the choice is kept in `ui_settings.json`. Five ship, the four styles the owner
chose from (#297) and the earlier look:

| Id | Name | Faces | What it looks like |
|---|---|---|---|
| `fleet` | Fleet console (**the default**) | Barlow Condensed, Barlow Semi Condensed | dense, steel blue, square, 1 px frames, an accent stripe on each title |
| `glass` | Glass HUD | Oxanium, Exo 2 | cyan on dark glass, chamfered corners, a soft glow on titles and the selection; blur behind windows as a setting |
| `industrial` | Industrial | Chakra Petch, IBM Plex Mono for figures | amber, 2 px frames, solid chosen tabs, dashed dividers, striped bars, a hard shadow |
| `clean` | Clean modern | IBM Plex Sans | rounded, a soft shadow, segmented tabs, titles in sentence case |
| `plain` | Plain | Inter | the earlier look |

A new theme is a new file and nothing else -- Settings lists whatever is in the folder and
loads, the default first.

Switching is a reload with no restart (`Ui::UseTheme`): the file is read, the theme in use
is replaced, and every font is rasterised again. It happens between frames (Settings asks
for it while it is drawn, the game switches at the start of the next one), because a
layout half declared in one theme must not finish in another. **Nothing keeps a theme value
past a frame**: windows ask `Ui::CurrentTheme()` as they draw, the colour names in
`UiTheme.h` are references into the theme in use, and the desk is told the title height,
snap and gap every frame. One thing did keep something: Clay remembers what each word
measured, per layout, and after a switch the same face at the same size is another font --
values were cut short and labels ran together until the window was reopened. A layout now
compares `Ui::FontGeneration()` when it begins and makes Clay forget its measures when the
fonts have changed (`layout_tests.cpp` holds it).

A theme file is read strictly: an unknown field or a missing one is a load error (a theme
that fails keeps the one in use; at start, the default, and failing that the built-in
values in `Theme.h`, which are the same as `fleet.json`). Colours are `"#RRGGBB"` or
`"#RRGGBBAA"`, and a window's `panel` is opaque in every shipped theme but glass, whose
panel lets a few percent through -- translucent, the text of the window behind ran into the
one in front. The `standing` colours are allegiance
as the *instruments* say it -- radar, overview (a chip before each name), sensor screen,
target panel, and the brackets laid over the world view on your own ship and the ships
around it (#361, "Brackets in space" in Settings) -- and never an object's colour in the
world view (#117).

A theme turns a few things in the frame on and off by number: `metrics.titleMark` (an
accent stripe down a title bar), `metrics.tabMark` (how thick the chosen tab's underline
is), `metrics.rowMark` (an accent bar down the left of a table's lit row),
`metrics.radius` (a widget's corner) and `metrics.windowRadius` (a window's). What a number
cannot say is a **renderer token** in `style`: one word from a short list, drawn once in
`Window.cpp` and the widgets through `Paint.h`, so a theme that says "chamfer" gets the same
cut on a window, a button and a tab. A word not on its list is a load error that names the
words that are.

| Token | Words | What it changes |
|---|---|---|
| `corner` | `square`, `rounded`, `chamfer` | every frame and widget: square whatever the radii say; rounded by them; cut at 45 degrees, top left and bottom right, by them |
| `titleBar` | `solid`, `fade`, `clear` | the title bar: `colors.title`; that colour fading out to the right; the panel with a rule under it |
| `tabStyle` | `underline`, `boxed`, `filled`, `segmented` | tabs, in a window and in a stack's title bar: a strip on a rule, the chosen one underlined; separate boxes; boxes in a frame of the border colour, the chosen one solid accent with `colors.onAccent` text; one sunken track with the chosen tab a raised plate |
| `divider` | `solid`, `dashed` | `Layout::Divider` and a table's rule |
| `barStyle` | `solid`, `striped` | `Layout::Bar`: a strip on its track; segments in a frame |
| `shadow` | `none`, `soft`, `offset` | under each window: nothing; one blurred square stretched round it (a nine-patch, so it costs one fill); a hard copy down and to the right |
| `panel` | `flat`, `glass` | glass adds a sheen down from the top, a lit row and a chosen tab that fade, and the cut corners drawn in the accent |
| `glow` | 0 to 1 | a halo on titles, the chosen tab's underline, a lit button and bars; 0 draws none and costs nothing |
| `blurPanel` | a colour, or `null` | the panel over the blurred world; `null` is a theme that never blurs |

A widget asks for its corners with `Box::ThemeCorners()` rather than `Radius(metrics.radius)`,
and a box can ask the painter for more with `Box::Paint(Ui::PAINT_...)` -- the flags travel
through Clay untouched to `DrawCommand::paint`. Fleet console and Plain are these tokens set to
what they drew before, and their frames are pixel for pixel what they were.

**Blur behind windows** is a setting (Settings, Interface; `"blur"` in `ui_settings.json`;
`--blur` for one run), off by default because it is paid for every frame. While it is on, in
flight, in a theme with a `blurPanel`, the game draws the world once into a texture of its
own (`Render::Backdrop`), halves it twice and blurs the quarter-size copy across and down with
bloom's blur shader, then draws that texture where the world would have been drawn. Every
window samples the same small texture under its own shape (`Ui::CurrentBackdrop`) and lays
`blurPanel` over it. The blur is opaque, so the window behind never shows through, only the
world. Without the shader the copy is shrunk to a sixteenth instead and bilinear filtering
blurs it more coarsely -- the setting never needs a shader. Docked, there is no world under
the windows and the setting does nothing.

`Ui::Scale()` is the player's setting (Settings, saved in `ui_settings.json` next to the
executable) times the display's DPI. It sizes every window frame, every window's place
and size on the desk (kept in units, so a layout made at one scale is the same layout at
another), and everything inside a `Ui::Layout`. `--uiscale S` sets it for one run.

Until the player chooses a scale, the setting is **automatic** (#362, `Ui::AutoUserScale`):
at least what the monitor's height suggests -- 1080 lines 1.25, 1440 1.5, 2160 2 -- with the
display's own DPI counted first, so a 1080p laptop at 125% gets 1.25 from Windows and a
factor of 1, and the same laptop at 100% gets the factor 1.25. Settings shows it as the
"Automatic" button. Only a chosen scale is written to `ui_settings.json` (version 2), and
it always wins; a version-1 file wrote 1 whether or not anyone had chosen it, so a 1 in one
reads as no choice.

## Legibility

Held by `theme_tests.cpp` for every theme in the folder, so a new theme that fails them
fails the build (#362):

- **Contrast.** Every colour text is written in -- `text`, `dim`, `accent`, `good`, `warn`,
  `bad`, `money`, each `standing` -- is at least 4.5:1 (WCAG AA) against the panel laid over
  the backdrop, and `titleText` and `dim` against the title bar. `Ui::Contrast` computes it.
- **Size.** `fontSize.body` and `fontSize.title` are at least 13 px at a scale of 1.
- **Standing is never colour alone.** "Own" is cyan in every theme, not the green that a
  deuteranope cannot tell from hostile red, and each standing is a shape as well
  (`Ui::Mark`, `Game::StandingMark`): you a dot in a ring, friendly a circle, neutral a
  diamond, hostile a triangle. The same shapes are the radar's ship blips and the
  overview's chips; a hostile station on the radar and a hostile-held system on the map
  carry the triangle over their own shape, and a hostile ship's bracket in the world view
  has a notch. The radar labels the blip under the cursor with its name, distance and
  standing in words.
- **Colour-blind palette** (Settings, Interface; `"colourBlind"` in `ui_settings.json`;
  `--cblind` for one run): `Ui::ColourBlindStanding()`, orange and blue with a white "own",
  laid over whichever theme is in use (`Ui::AdoptStanding`) and taken off again. It is
  checked against every theme's panel like the theme's own colours.

## Fonts

A theme names four faces (`Ui::Face`), each a file in `data/fonts/` with two settings, its
`case` and its `tracking`:

| Face | For | Fleet console |
|---|---|---|
| `regular` | body text, labels | Barlow Semi Condensed |
| `strong` | a value that matters | Barlow Semi Condensed SemiBold |
| `heading` | window titles, tabs, buttons, column headings, a screen's heading | Barlow Condensed SemiBold, capitals, tracked 0.1 em |
| `numbers` | figures in a column aligned to the end | Barlow Semi Condensed with tabular figures |

`case` is `as-written`, `upper` (capitals, whatever the text says) or `sentence`: the first
letter a capital, and a run written all in capitals -- the code names its windows
"SELECTED ITEM" -- set as a sentence, "Selected item"; a run with any small letter in it is
a name ("Jita IV") and keeps the rest as written (`Ui::SetCase`, tested). `tracking` adds room
between letters (in ems, between letters only -- never after the last, or a centred label sits off
centre). Both belong to the face, so every heading follows without a widget knowing.
`TextStyle::Caption()` is the heading face at the label size: what names a control rather
than a value.

**Sizes are ems, as CSS means them**, whatever the font: 13 px Barlow and 13 px Inter look
the same size, where raylib's own size (ascender to descender) made a font with long
descenders look smaller. A face is rasterised at exactly each size it is drawn at, the first
time, so text is never a scaled bitmap; every glyph is rasterised three times a third of a
pixel apart and each advances by its exact width plus the font's kerning, so the spacing is
the designer's rather than the rounding's, with no filtering to blur it. There is no
hinting; `text.gamma` thickens the edges, which pale text on a dark ground otherwise loses.

**Text is centred on its capitals, from the font's own metrics** (`Ui::CentredTop`): the
middle of the capital height -- read from the outline of an H, since the OS/2 table that
also says it is optional -- sits on the middle of the box. Centring the text's box instead
(ascender to descender) put labels low by however much more room a font keeps below the
baseline than above its capitals; every widget, title bar and menu goes through this one
rule. A line of text is `text.lineHeight` times its size tall.

Barlow's own figures are proportional, and raylib applies no OpenType features, so the
numbers face is a copy with its tabular figures made the default, once, with fontTools:

```sh
pip install opentype-feature-freezer
pyftfeatfreeze -f tnum -S -U Tnum BarlowSemiCondensed-Regular.ttf BarlowSemiCondensedTnum-Regular.ttf
```

The other styles' fonts, and what was done to each:

| File | From | Done to it |
|---|---|---|
| `Oxanium-SemiBold.ttf` | google/fonts `ofl/oxanium`, a variable font | the 600 instance, with `fonttools varLib.instancer Oxanium[wght].ttf wght=600 --update-name-table`; its figures are tabular already |
| `Exo2-Regular.ttf`, `Exo2-SemiBold.ttf` | google/fonts `ofl/exo2`, a variable font | the 400 and 600 instances, the same way |
| `Exo2Tnum-Regular.ttf` | `Exo2-Regular.ttf` | `pyftfeatfreeze -f tnum -S -U Tnum`, as Barlow; Exo 2's tabular four is 615 units wide where the other figures are 620, so its advance was set to 620 and the outline moved 2 units right (fontTools, a few lines) |
| `ChakraPetch-Regular.ttf`, `-SemiBold.ttf` | google/fonts `ofl/chakrapetch` | nothing; Industrial sets figures in Plex Mono |
| `IBMPlexMono-Medium.ttf` | google/fonts `ofl/ibmplexmono` | nothing: monospaced |
| `IBMPlexSans-Regular.ttf`, `-SemiBold.ttf` | IBM's own release, `IBM/plex` `packages/plex-sans/fonts/complete/ttf` | nothing; its figures are tabular already |

Google Fonts has IBM Plex Sans only as a variable font, and an instance cut from it is a
Modified Version under the OFL -- which may not carry the reserved name "Plex". IBM's own
static files are shipped unmodified instead.

The fonts are under the SIL Open Font License: `data/fonts/OFL.txt` (Inter),
`OFL-Barlow.txt`, `OFL-Oxanium.txt`, `OFL-Exo2.txt`, `OFL-ChakraPetch.txt` (none of which
reserves a name, so the instanced and renamed copies may be shipped) and `OFL-IBMPlex.txt`
(Plex Mono and Sans, reserved font name "Plex", shipped unmodified). `Ui::Text(text, x, y, px, colour)` remains for the editor's screens placed
by hand; new code uses `Layout::Text`. World glyphs that grow with the camera use
`Ui::GlyphFont()`, one large filtered atlas.

## Checking a theme

`--theme ID` runs the client in a theme for one run (not saved), `--blur` turns the blur on,
and `--click X Y FRAME` (repeatable) plays a left click into raylib's own input state on that
frame -- a click sent to the window from outside never reaches it, and a shot's window is
hidden. With them a theme switch is checked the way a player makes it: Settings open, a click
on Interface, a click on a theme, a shot. Every theme was switched to and from at runtime that
way, the blur toggled in glass, and back to Fleet console with nothing out of place.

What the HUD costs, measured in a Release build at 1920 x 1080 with the status, selected-item,
overview, missions, settings and radar windows open (median of three runs, CPU and GPU ms of
the HUD phase, and the whole frame): every theme is within a millisecond of Fleet console's on
the CPU; the soft shadow and the glow cost a little GPU; the blur costs one extra copy of the
world and four small passes. The HUD's CPU time is mostly per-frame text work that does not
depend on the theme -- see #297 for the numbers and what to look at.

## Clay

Clay (zlib, pinned in `CMakeLists.txt`) solves the layout. Its header refuses C++ before
C++20, so it is compiled in `ClayBridge.c` and nothing else includes it: `Ui::Layout` talks
to the bridge's plain structs. If Clay ever limits us, the bridge is the seam to replace;
no window is written against Clay itself.

## Still placed by hand

On `Ui::Layout`: status, overview, the selected item, missions, radar (its toolbar; the
scope is a picture in a box the layout gives), the scanner, chat, the corporation,
settings, F10's screen treatment, the
station, market and hangar, and the map's and the sensor screen's heading, legend and news.
The pictures themselves -- the map's nodes and their labels, the sensor grid, the radar's
scope -- are drawn by hand in the box their layout gives them. No window is placed with
coordinates any more.

## A window on the overlay layer

F10's panel is an ordinary placed window -- it moves, resizes, collapses and is kept in
`ui_layout.json` -- on `Layer::Overlay`, which the game draws after the screen treatment:
a settings window seen through the effect it adjusts cannot be read while adjusting it.
Grouping and stacking are per layer, so it never joins a window below it. It has no
`context`: it is there in space and docked alike. Its content is
`Render::TreatmentPanel`, which the editor's gallery draws too, in a box of its own since
the editor has no desk. A change is written to `look.json` when the mouse lets go of it,
not on every frame of a slider's drag.

## Seeing a window

Synthetic input does not reach the client's window, so a picture of one is asked for on the
command line: `econspace connect ... --open radar,missions,settings --uiscale 1.5 --shot
f.png`. `--open` takes the desk's ids, and opening a window this way is remembered in
`ui_layout.json` like opening it by hand. The docked windows are there once the ship is
docked: `--dock` flies to the nearest station and docks, so `--dock --frames 900 --shot
s.png` is a picture of them.
