#include <doctest/doctest.h>

#include "ui/DeskLayout.h"
#include "ui/Focus.h"
#include "ui/Layout.h"
#include "ui/Units.h"

#include <string>
#include <string_view>
#include <vector>

// The widget library on Ui::Layout (#297): tables, tabs, scrolling, fields, sliders,
// tooltips, and the keyboard focus the desk routes. Held without a window -- text is measured
// by a stand-in, half the font size per character, since a real font needs a GPU.

using Ui::Box;
using Ui::Focus;
using Ui::KeyInput;
using Ui::Layout;
using Ui::Size;

namespace
{
void Monospaced(Layout& l)
{
    l.SetMeasure([](Ui::Face, float px, std::string_view text)
                 { return Vector2{ 0.5f * px * (float)text.size(), px }; });
}

Vector2 Centre(Rectangle r)
{
    return { r.x + r.width * 0.5f, r.y + r.height * 0.5f };
}

Layout::Pointer At(Vector2 p, bool pressed = false, bool down = false)
{
    Layout::Pointer ptr;
    ptr.owned = true;
    ptr.pos = p;
    ptr.pressed = pressed;
    ptr.down = down || pressed;
    return ptr;
}

const Rectangle AREA{ 0.0f, 0.0f, 300.0f, 200.0f };

// A table of `n` rows, laid out once with the given pointer.
struct TableHarness
{
    Layout                   l;
    Ui::TableSort            sort;
    std::vector<std::string> names;
    bool                     reversible = false;
    Ui::TableEvents          last;

    explicit TableHarness(int n)
    {
        Monospaced(l);
        for (int i = 0; i < n; i++)
            names.push_back("row " + std::to_string(i));
    }
    Ui::TableEvents Frame(const Layout::Pointer& p = Layout::Pointer())
    {
        Ui::TableSpec s;
        s.id = "t";
        s.columns = { { "name", Size::Grow(), Ui::Align::Start, true },
                      { "dist", Size::Fixed(60.0f), Ui::Align::End, true } };
        s.rows = (int)names.size();
        s.cell = [&](int r, int c)
        { return Ui::Cell{ c == 0 ? names[r] : std::to_string(r * 10), WHITE }; };
        s.sort = &sort;
        s.reversible = reversible;
        s.empty = "nothing here";
        l.Begin(AREA, 1.0f, p);
        last = l.Table(s);
        l.End();
        CHECK_MESSAGE(l.LastError().empty(), l.LastError());
        return last;
    }
};
}  // namespace

// --- The keyboard focus -------------------------------------------------------------------

TEST_CASE("Focus: one field holds the keyboard, and says why it lost it")
{
    Focus f;
    CHECK_FALSE(f.Taken());
    f.Take("map", "name");
    CHECK(f.Taken());
    CHECK(f.Holds("map", "name"));
    CHECK_FALSE(f.Holds("target", "name"));  // a field is known by its window as well

    f.Take("target", "range");  // another field takes it: the first one has lost it
    CHECK(f.TakeBlur("map", "name") == Focus::Blur::Elsewhere);
    CHECK(f.TakeBlur("map", "name") == Focus::Blur::None);  // said once

    f.Release(Focus::Blur::Cancel);  // Esc
    CHECK_FALSE(f.Taken());
    CHECK(f.TakeBlur("target", "range") == Focus::Blur::Cancel);
}

TEST_CASE("Focus: a field that stops being drawn lets the keyboard go")
{
    Focus f;
    f.Take("map", "name");
    f.EndFrame();  // taken this frame: as good as seen
    CHECK(f.Taken());
    f.Seen("map", "name");
    f.EndFrame();
    CHECK(f.Taken());
    f.EndFrame();  // not drawn: the map closed under it, or the system was named elsewhere
    CHECK_FALSE(f.Taken());
}

TEST_CASE("Focus: a press on the field that holds it is not a loss")
{
    Focus f;
    f.Take("target", "range");
    f.Release(Focus::Blur::Elsewhere);  // the desk: a press somewhere
    f.Take("target", "range");          // ...on the field itself
    CHECK(f.Holds("target", "range"));
    CHECK(f.TakeBlur("target", "range") == Focus::Blur::None);
}

TEST_CASE("Focus: closing a window lets go only if the field is in it")
{
    Focus f;
    f.Take("map", "name");
    f.ReleaseOwner("target", Focus::Blur::Elsewhere);
    CHECK(f.Taken());
    f.ReleaseOwner("map", Focus::Blur::Elsewhere);
    CHECK_FALSE(f.Taken());
}

TEST_CASE("EditText: typing, a limit, backspace, a filter, and Enter")
{
    std::string s;
    KeyInput    k;
    k.chars = "Alpha";
    CHECK(Ui::EditText(s, k, 24).changed);
    CHECK(s == "Alpha");

    k.chars = "beta";
    Ui::EditText(s, k, 7);
    CHECK(s == "Alphabe");  // no further than the limit

    KeyInput back;
    back.backspace = 2;
    Ui::EditText(s, back, 24);
    CHECK(s == "Alpha");

    std::string n;
    KeyInput    mixed;
    mixed.chars = "1a2.5";
    Ui::EditText(n, mixed, 24, [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    CHECK(n == "12.5");

    KeyInput enter;
    enter.enter = true;
    const Ui::EditResult r = Ui::EditText(n, enter, 24);
    CHECK(r.submitted);
    CHECK_FALSE(r.changed);
}

// --- Table, scrolling and tabs -------------------------------------------------------------

TEST_CASE("Table: a click on a heading sorts by it, and again turns it round only if allowed")
{
    TableHarness t(3);
    t.sort.column = 1;
    t.Frame();
    const Rectangle name = t.l.BoxOf("t~h", 0);
    REQUIRE(name.width > 0.0f);

    CHECK(t.Frame(At(Centre(name), true)).sorted);
    CHECK(t.sort.column == 0);
    CHECK_FALSE(t.sort.descending);

    // Again, on a table whose model always has the same order (the overview: hostiles first).
    CHECK_FALSE(t.Frame(At(Centre(name), true)).sorted);
    CHECK_FALSE(t.sort.descending);

    t.reversible = true;
    CHECK(t.Frame(At(Centre(name), true)).sorted);
    CHECK(t.sort.descending);
}

TEST_CASE("Table: a row takes a left and a right click, and the columns line up")
{
    TableHarness t(5);
    t.Frame();
    const Rectangle row2 = t.l.BoxOf("t~r", 2);
    REQUIRE(row2.height > 0.0f);

    const Ui::TableEvents hover = t.Frame(At(Centre(row2)));
    CHECK(hover.hovered == 2);
    CHECK(hover.clicked == -1);

    CHECK(t.Frame(At(Centre(row2), true)).clicked == 2);

    Layout::Pointer right = At(Centre(row2));
    right.rightPressed = true;
    CHECK(t.Frame(right).rightClicked == 2);

    // The headings sit over their columns: the indicator's room is kept beside both.
    const Rectangle head = t.l.BoxOf("t~h", 1);
    CHECK(head.x + head.width <= AREA.width - t.l.ScrollGutter() + 0.5f);
}

TEST_CASE("Table: more rows than fit scroll with the wheel, and the indicator drags")
{
    TableHarness t(40);  // 40 rows of 20 units in a 200-unit window
    t.Frame();
    t.Frame();  // the second frame knows how tall the content is
    const Rectangle bar = t.l.BoxOf("t~b~bar");
    REQUIRE(bar.height > 0.0f);
    const float first = t.l.BoxOf("t~r", 0).y;

    // The wheel, over the rows.
    Layout::Pointer wheel = At(Centre(t.l.BoxOf("t~b")));
    wheel.wheel = -2.0f;
    t.Frame(wheel);
    t.Frame(At(Centre(t.l.BoxOf("t~b"))));
    CHECK(t.l.BoxOf("t~r", 0).y < first);

    // Dragging the indicator to the bottom shows the last row.
    t.Frame(At({ bar.x + bar.width * 0.5f, bar.y + 4.0f }, true));
    t.Frame(At({ bar.x + bar.width * 0.5f, bar.y + bar.height + 50.0f }, false, true));
    t.Frame(At({ bar.x + bar.width * 0.5f, bar.y + bar.height + 50.0f }, false, true));
    const Rectangle body = t.l.BoxOf("t~b");
    const Rectangle last = t.l.BoxOf("t~r", 39);
    CHECK(last.y + last.height <= body.y + body.height + 0.5f);
    CHECK(last.y >= body.y);
}

TEST_CASE("Table: a row scrolled out of sight does not take a click meant for the headings")
{
    TableHarness t(40);
    t.Frame();
    t.Frame();
    Layout::Pointer wheel = At(Centre(t.l.BoxOf("t~b")));
    wheel.wheel = -3.0f;
    t.Frame(wheel);
    t.Frame();
    const Rectangle       head = t.l.BoxOf("t~h", 1);
    const Ui::TableEvents ev = t.Frame(At(Centre(head), true));
    CHECK(ev.clicked == -1);
    CHECK(ev.sorted);
}

TEST_CASE("Table: a cell too long for its column is cut short with ..")
{
    TableHarness t(1);
    t.names[0] = std::string(200, 'x');
    t.Frame();
    t.Frame();  // the second frame knows how wide the column is
    bool found = false;
    for (const Ui::DrawCommand& c : t.l.End())  // the commands of the frame just laid out
    {
        if (c.kind == Ui::DrawCommand::Kind::Text && c.text.size() > 2 &&
            c.text.substr(c.text.size() - 2) == "..")
        {
            found = true;
            const float room = t.l.BoxOf("t~h", 0).width;
            CHECK(0.5f * c.px * (float)c.text.size() <= room);
        }
    }
    CHECK(found);
}

TEST_CASE("Table: a Fit column is as wide as its widest cell, on every row (#259)")
{
    Layout l;
    Monospaced(l);
    std::vector<std::string> dist = { "9", "12.5k", "480" };
    auto                     frame = [&]
    {
        Ui::TableSpec s;
        s.id = "t";
        s.columns = { { "name", Size::Grow(), Ui::Align::Start, true },
                      { "d", Size::Fit(), Ui::Align::End, true } };
        s.rows = (int)dist.size();
        s.cell = [&](int r, int c) { return Ui::Cell{ c == 0 ? "row" : dist[r], WHITE }; };
        l.Begin(AREA, 1.0f, Layout::Pointer());
        l.Table(s);
        const std::vector<Ui::DrawCommand> out = l.End();
        CHECK_MESSAGE(l.LastError().empty(), l.LastError());
        return out;
    };
    frame();
    for (const Ui::DrawCommand& c : frame())
        if (c.kind == Ui::DrawCommand::Kind::Text)
            CHECK(c.text.find("..") == std::string::npos);  // nothing cut short
    const float narrow = l.BoxOf("t~h", 1).width;
    CHECK(narrow > 0.0f);

    dist[1] = "12345.6k";  // a wider value widens the whole column, heading included
    frame();
    const float wide = l.BoxOf("t~h", 1).width;
    CHECK(wide > narrow);
    CHECK(wide < AREA.width / 2.0f);
}

TEST_CASE("Distance: exact to 9999, then three figures with k or M (#259)")
{
    CHECK(Ui::Distance(0.0f) == "0");
    CHECK(Ui::Distance(9599.0f) == "9599");
    CHECK(Ui::Distance(12500.0f) == "12.5k");
    CHECK(Ui::Distance(99960.0f) == "100k");  // decided on the printed value, not "100.0k"
    CHECK(Ui::Distance(480000.0f) == "480k");
    CHECK(Ui::Distance(999600.0f) == "1.00M");
    CHECK(Ui::Distance(1250000.0f) == "1.25M");
    CHECK(Ui::Distance(25000000.0f) == "25.0M");
    for (float d = 1.0f; d < 1e9f; d *= 1.37f)
        CHECK(Ui::Distance(d).size() <= 5);
}

TEST_CASE("Ellipsize: a short text is left alone, a long one ends in ..")
{
    auto width = [](std::string_view s) { return 7.0f * (float)s.size(); };
    CHECK(Ui::Ellipsize("Alpha", 100.0f, width) == "Alpha");
    const std::string cut = Ui::Ellipsize("Alpha Centauri Station", 70.0f, width);
    CHECK(cut == "Alpha Ce..");  // ten characters' room, the dots included
    CHECK(Ui::Ellipsize("Alpha Centauri", 56.0f, width) == "Alpha..");  // no space before them
    CHECK(width(cut) <= 70.0f);
    CHECK(Ui::Ellipsize("Alpha", 0.0f, width) == "Alpha");  // not laid out yet: nothing to cut to
}

TEST_CASE("Tabs: a click on a tab selects it")
{
    Layout l;
    Monospaced(l);
    const std::vector<std::string> tabs{ "All", "Ships", "Hostile" };
    int                            sel = 0;
    auto                           frame = [&](const Layout::Pointer& p)
    {
        l.Begin(AREA, 1.0f, p);
        const bool changed = l.Tabs("tabs", tabs, sel);
        l.End();
        return changed;
    };
    frame(Layout::Pointer());
    const Rectangle hostile = l.BoxOf("tabs", 2);
    REQUIRE(hostile.width > 0.0f);
    CHECK(frame(At(Centre(hostile), true)));
    CHECK(sel == 2);
    CHECK_FALSE(frame(At(Centre(hostile), true)));  // already selected: nothing changed
}

// --- Fields and sliders --------------------------------------------------------------------

TEST_CASE("TextField: a click takes the keyboard, Enter submits and lets it go")
{
    Layout l;
    Monospaced(l);
    Focus focus;
    l.UseFocus(&focus, "map");
    std::string    name;
    Ui::EditResult r;
    auto           frame = [&](const Layout::Pointer& p, const KeyInput* keys)
    {
        l.Begin(AREA, 1.0f, p);
        if (keys)
            l.FeedKeys(*keys);
        r = l.TextField("name", name);
        l.End();
    };
    frame(Layout::Pointer(), nullptr);
    const Rectangle box = l.BoxOf("name");
    REQUIRE(box.width > 0.0f);

    KeyInput typed;
    typed.chars = "Vela";
    frame(Layout::Pointer(), &typed);
    CHECK(name.empty());  // without the keyboard, typing goes nowhere

    frame(At(Centre(box), true), nullptr);
    CHECK(focus.Holds("map", "name"));
    frame(Layout::Pointer(), &typed);
    CHECK(name == "Vela");
    CHECK(r.changed);

    KeyInput enter;
    enter.enter = true;
    frame(Layout::Pointer(), &enter);
    CHECK(r.submitted);
    CHECK_FALSE(focus.Taken());
}

TEST_CASE("TextField: a field under another window cannot be clicked into")
{
    Layout l;
    Monospaced(l);
    Focus focus;
    l.UseFocus(&focus, "map");
    std::string name;
    l.Begin(AREA, 1.0f);
    l.TextField("name", name);
    l.End();
    Layout::Pointer p = At(Centre(l.BoxOf("name")), true);
    p.owned = false;
    l.Begin(AREA, 1.0f, p);
    l.TextField("name", name);
    l.End();
    CHECK_FALSE(focus.Taken());
}

TEST_CASE("NumberField: Enter or a click elsewhere takes the number, clamped; Esc does not")
{
    Layout l;
    Monospaced(l);
    Focus focus;
    l.UseFocus(&focus, "target");
    float value = 500.0f;
    bool  changed = false;
    auto  frame = [&](const KeyInput* keys)
    {
        l.Begin(AREA, 1.0f);
        if (keys)
            l.FeedKeys(*keys);
        changed = l.NumberField("range", value, 30.0f, 60000.0f);
        l.End();
    };
    frame(nullptr);

    // Enter.
    KeyInput typed;
    typed.chars = "1250";
    focus.Take("target", "range");
    frame(&typed);
    CHECK(value == 500.0f);  // still being typed
    KeyInput enter;
    enter.enter = true;
    frame(&enter);
    CHECK(changed);
    CHECK(value == doctest::Approx(1250.0f));
    CHECK_FALSE(focus.Taken());

    // Esc puts the old value back.
    focus.Take("target", "range");
    typed.chars = "9";
    frame(&typed);
    focus.Release(Focus::Blur::Cancel);
    frame(nullptr);
    CHECK_FALSE(changed);
    CHECK(value == doctest::Approx(1250.0f));

    // A click elsewhere takes it, and a number out of range is brought into it.
    focus.Take("target", "range");
    typed.chars = "999999";
    frame(&typed);
    focus.Release(Focus::Blur::Elsewhere);
    frame(nullptr);
    CHECK(changed);
    CHECK(value == doctest::Approx(60000.0f));

    // Nothing typed: nothing changes.
    focus.Take("target", "range");
    frame(nullptr);
    focus.Release(Focus::Blur::Elsewhere);
    frame(nullptr);
    CHECK_FALSE(changed);
    CHECK(value == doctest::Approx(60000.0f));
}

TEST_CASE("Slider: dragged along its track, linear or by ratio")
{
    Layout l;
    Monospaced(l);
    float v = 0.0f;
    auto  frame = [&](const Layout::Pointer& p, bool log, float lo, float hi)
    {
        l.Begin(AREA, 1.0f, p);
        const bool changed = l.Slider("s", v, lo, hi, log);
        l.End();
        return changed;
    };
    frame(Layout::Pointer(), false, 0.0f, 100.0f);
    const Rectangle track = l.BoxOf("s");
    REQUIRE(track.width > 0.0f);
    const float y = track.y + track.height * 0.5f;

    CHECK(frame(At({ track.x + track.width * 0.75f, y }, true), false, 0.0f, 100.0f));
    CHECK(v == doctest::Approx(75.0f));
    // Held, it follows the cursor -- even past the end, where it stops.
    frame(At({ track.x + track.width * 2.0f, y }, false, true), false, 0.0f, 100.0f);
    CHECK(v == doctest::Approx(100.0f));
    // Released, it does not.
    CHECK_FALSE(frame(At({ track.x, y }), false, 0.0f, 100.0f));
    CHECK(v == doctest::Approx(100.0f));

    // By ratio: halfway between 100 and 10 000 is 1 000.
    frame(At({ track.x + track.width * 0.5f, y }, true), true, 100.0f, 10000.0f);
    CHECK(v == doctest::Approx(1000.0f).epsilon(0.01));
}

TEST_CASE("Tooltip: shown once the cursor has rested, and only then")
{
    Layout l;
    Monospaced(l);
    auto frame = [&](const Layout::Pointer& p)
    {
        Ui::ClearTooltip();
        l.Begin(AREA, 1.0f, p);
        l.Button("b", "Orbit");
        l.Tooltip("b", "Circle it at this range");
        l.End();
    };
    frame(Layout::Pointer());
    Layout::Pointer p = At(Centre(l.BoxOf("b")));
    p.dt = 0.1f;
    frame(p);
    CHECK(Ui::PendingTooltip().empty());
    for (int i = 0; i < 6; i++)
        frame(p);
    CHECK(Ui::PendingTooltip() == "Circle it at this range");

    frame(Layout::Pointer());  // away
    p.dt = 0.1f;
    frame(p);
    CHECK(Ui::PendingTooltip().empty());  // and back: it waits again
    Ui::ClearTooltip();
}

// --- What a screen covers --------------------------------------------------------------------

TEST_CASE("Desk: a screen that covers the view hides the windows under it")
{
    Ui::DeskLayout d;
    Ui::WindowSpec panel;
    panel.id = "overview";
    panel.layer = Ui::Layer::Panels;
    panel.place = { 16, 16, 300, 300 };
    Ui::WindowSpec map;
    map.id = "map";
    map.layer = Ui::Layer::Screen;
    map.covers = true;
    Ui::WindowSpec bar;
    bar.id = "menubar";
    bar.layer = Ui::Layer::Modal;
    const int p = d.Add(panel, true);
    const int m = d.Add(map, false);
    const int b = d.Add(bar, true);
    d.SetRect(m, { 0, 0, 1280, 720 });
    d.SetRect(b, { 0, 0, 46, 720 });

    CHECK_FALSE(d.Covered(p));
    d.SetOpen(m, true);
    CHECK(d.Covered(p));        // the map: the overview is not drawn through it
    CHECK_FALSE(d.Covered(b));  // the menu bar is above the screens
    CHECK_FALSE(d.Covered(m));
    d.SetOpen(m, false);
    CHECK_FALSE(d.Covered(p));
}
