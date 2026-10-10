#include <doctest/doctest.h>

#include "ui/DeskLayout.h"
#include "ui/Input.h"

#include <nlohmann/json.hpp>
#include <string>

// The window manager's rules (#297): which window is in front, who owns the mouse, what Esc
// closes, and where windows are found again after a restart. All of it is DeskLayout, which
// draws nothing, so it is held here; there is no GPU on a CI runner.

using Ui::Anchor;
using Ui::DeskLayout;
using Ui::EscRule;
using Ui::Layer;
using Ui::WindowSpec;

namespace
{
WindowSpec Panel(const char* id, Anchor anchor, Rectangle place)
{
    WindowSpec s;
    s.id = id;
    s.layer = Layer::Panels;
    s.anchor = anchor;
    s.place = place;
    s.persist = true;
    return s;
}

WindowSpec Surface(const char* id, Layer layer, EscRule esc = EscRule::Close)
{
    WindowSpec s;
    s.id = id;
    s.layer = layer;
    s.esc = esc;
    return s;
}

// The game's desk at 1280 x 720, as Game::SetupWindows builds it.
struct GameDesk
{
    DeskLayout d;
    int        status, target, overview, radar, map, sensor, station, bar, context, look;

    GameDesk()
    {
        status = d.Add(Panel("status", Anchor::TopLeft, { 56, 16, 264, 312 }), true);
        target = d.Add(Panel("target", Anchor::TopRight, { 16, 16, 264, 196 }), false);
        overview = d.Add(Panel("overview", Anchor::TopRight, { 16, 224, 264, 400 }), true);
        radar = d.Add(Panel("radar", Anchor::TopLeft, { 56, 344, 264, 288 }), false);
        map = d.Add(Surface("map", Layer::Screen), false);
        sensor = d.Add(Surface("sensor", Layer::Screen), false);
        station = d.Add(Surface("station", Layer::Screen, EscRule::Block), false);
        bar = d.Add(Surface("menubar", Layer::Modal, EscRule::Ignore), true);
        context = d.Add(Surface("context", Layer::Popup), false);
        look = d.Add(Surface("look", Layer::Overlay), false);
        d.SetRect(map, { 0, 0, 1280, 720 });
        d.SetRect(sensor, { 46, 0, 1234, 720 });
        d.SetRect(station, { 0, 0, 1280, 720 });
        d.SetRect(bar, { 0, 0, 46, 720 });
        d.SetRect(context, { 600, 300, 172, 104 });
        d.SetRect(look, { 944, 60, 320, 400 });
    }
};

bool Equal(Rectangle a, Rectangle b)
{
    return a.x == doctest::Approx(b.x) && a.y == doctest::Approx(b.y) &&
           a.width == doctest::Approx(b.width) && a.height == doctest::Approx(b.height);
}
}  // namespace

TEST_CASE("Desk: a window is placed from its anchor, and keeps to it at another size")
{
    GameDesk g;
    // The places the windows have always had at 1280 x 720.
    CHECK(Equal(g.d.Rect(g.status), { 56, 16, 264, 312 }));
    CHECK(Equal(g.d.Rect(g.target), { 1000, 16, 264, 196 }));
    CHECK(Equal(g.d.Rect(g.overview), { 1000, 224, 264, 400 }));

    g.d.SetScreen(1920, 1080);
    CHECK(Equal(g.d.Rect(g.status), { 56, 16, 264, 312 }));       // left stays left
    CHECK(Equal(g.d.Rect(g.overview), { 1640, 224, 264, 400 }));  // right stays right
}

TEST_CASE("Desk: z-order is by layer first, then by what was raised last")
{
    DeskLayout d;
    const int  a = d.Add(Panel("a", Anchor::TopLeft, { 0, 0, 100, 100 }), true);
    const int  b = d.Add(Panel("b", Anchor::TopLeft, { 50, 50, 100, 100 }), true);
    const int  pop = d.Add(Surface("pop", Layer::Popup), true);
    d.SetRect(pop, { 0, 0, 10, 10 });
    CHECK(d.Add(Panel("a", Anchor::TopLeft, { 0, 0, 1, 1 }), true) == DeskLayout::NONE);

    CHECK(d.Order() == std::vector<int>{ a, b, pop });
    d.Raise(a);
    CHECK(d.Order() == std::vector<int>{ b, a, pop });  // a window never rises above a popup
}

TEST_CASE("Desk: the mouse belongs to the window in front, and nothing behind it")
{
    DeskLayout d;
    const int  back = d.Add(Panel("back", Anchor::TopLeft, { 0, 0, 200, 200 }), true);
    const int  front = d.Add(Panel("front", Anchor::TopLeft, { 100, 100, 200, 200 }), true);

    // Where they overlap, only the front one is under the cursor -- the overview's rows
    // under another window used to take the click as well.
    CHECK(d.UpdateOwner({ 150, 150 }, false) == front);
    CHECK(d.UpdateOwner({ 50, 50 }, false) == back);
    CHECK(d.UpdateOwner({ 500, 500 }, false) == DeskLayout::NONE);  // the world

    d.Raise(back);
    CHECK(d.UpdateOwner({ 150, 150 }, false) == back);

    d.SetOpen(back, false);
    CHECK(d.UpdateOwner({ 150, 150 }, false) == front);  // a closed window is not there
}

TEST_CASE("Desk: a screen covers the windows, and the menu bar is above the screen")
{
    GameDesk g;
    CHECK(g.d.UpdateOwner({ 1100, 300 }, false) == g.overview);
    g.d.SetOpen(g.map, true);
    CHECK(g.d.UpdateOwner({ 1100, 300 }, false) == g.map);
    CHECK(g.d.UpdateOwner({ 20, 100 }, false) == g.bar);
    g.d.SetOpen(g.context, true);
    CHECK(g.d.UpdateOwner({ 650, 320 }, false) == g.context);
}

TEST_CASE("Desk: a press keeps the mouse until every button is up")
{
    DeskLayout d;
    const int  a = d.Add(Panel("a", Anchor::TopLeft, { 0, 0, 100, 100 }), true);
    const int  b = d.Add(Panel("b", Anchor::TopLeft, { 200, 0, 100, 100 }), true);

    CHECK(d.UpdateOwner({ 50, 50 }, true) == a);    // pressed in a
    CHECK(d.UpdateOwner({ 250, 50 }, true) == a);   // dragged over b: still a's
    CHECK(d.UpdateOwner({ 250, 50 }, false) == b);  // released

    // The world's drag (a pan) is the world's over a window too.
    CHECK(d.UpdateOwner({ 500, 500 }, true) == DeskLayout::NONE);
    CHECK(d.UpdateOwner({ 50, 50 }, true) == DeskLayout::NONE);
    CHECK(d.UpdateOwner({ 50, 50 }, false) == a);

    // An owner that goes away while held lets go.
    CHECK(d.UpdateOwner({ 50, 50 }, true) == a);
    d.SetOpen(a, false);
    CHECK(d.UpdateOwner({ 250, 50 }, true) == b);
}

TEST_CASE("Desk: Esc closes from the top layer down, and a station stops it")
{
    GameDesk g;
    g.d.SetOpen(g.target, true);
    g.d.SetOpen(g.map, true);
    g.d.SetOpen(g.context, true);
    g.d.SetOpen(g.look, true);

    CHECK(g.d.Escape() == g.look);      // F10's panel
    CHECK(g.d.Escape() == g.context);   // a popup
    CHECK(g.d.Escape() == g.map);       // the screen; the menu bar is passed over
    CHECK(g.d.Escape() == g.overview);  // the window in front: the last registered
    g.d.Raise(g.status);
    CHECK(g.d.Escape() == g.status);  // ...or the last raised
    CHECK(g.d.Escape() == g.target);
    CHECK(g.d.Escape() == DeskLayout::NONE);
    CHECK(g.d.IsOpen(g.bar));  // never closed by Esc

    // Docked: Esc reaches the station screen and stops there, and the windows behind it
    // stay open. Leaving a station is an order to the server.
    GameDesk h;
    h.d.SetOpen(h.station, true);
    CHECK(h.d.Escape() == DeskLayout::NONE);
    CHECK(h.d.IsOpen(h.overview));
    CHECK(h.d.IsOpen(h.station));
    h.d.SetOpen(h.look, true);
    CHECK(h.d.Escape() == h.look);  // above the station, F10's panel still closes
}

TEST_CASE("Desk: a moved window is where it was left after a restart at another size")
{
    GameDesk before;
    // Dragged to the bottom right, then let go: it takes the bottom-right corner.
    before.d.SetRect(before.radar, { 1000, 400, 264, 288 });
    before.d.Settle(before.radar);
    before.d.SetOpen(before.radar, true);
    before.d.SetOpen(before.overview, false);
    CHECK(before.d.TakeChanged());
    CHECK_FALSE(before.d.TakeChanged());

    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", before.d);
    CHECK(file["version"] == DeskLayout::VERSION);
    CHECK(file["accounts"]["pilot"]["radar"]["anchor"] == "bottom_right");

    // Through text, as the file is.
    const nlohmann::json reread = nlohmann::json::parse(file.dump());

    GameDesk    after;
    std::string error;
    REQUIRE(DeskLayout::ReadFile(reread, "pilot", after.d, error));
    CHECK(Equal(after.d.Rect(after.radar), { 1000, 400, 264, 288 }));
    CHECK(after.d.IsOpen(after.radar));
    CHECK_FALSE(after.d.IsOpen(after.overview));

    // A bigger screen: still 16 from the right and 32 from the bottom.
    after.d.SetScreen(1920, 1080);
    CHECK(Equal(after.d.Rect(after.radar), { 1640, 760, 264, 288 }));

    // Another account on the same machine has a layout of its own.
    GameDesk other;
    REQUIRE(DeskLayout::ReadFile(reread, "someone_else", other.d, error));
    CHECK(Equal(other.d.Rect(other.radar), { 56, 344, 264, 288 }));
}

TEST_CASE("Desk: a saved place that is off the screen comes back reachable")
{
    GameDesk             g;
    const nlohmann::json windows = {
        { "status", { { "anchor", "top_left" }, { "offset", { 5000, -300 } } } },
        { "overview", { { "anchor", "nowhere" }, { "offset", "wrong" }, { "open", 3 } } },
        { "unknown_window", { { "open", true } } },
    };
    g.d.Load(windows);
    const Rectangle s = g.d.Rect(g.status);
    CHECK(s.x <= 1280 - 60);  // enough of it on the screen to drag back
    CHECK(s.y >= 0);
    // Fields of the wrong type are ignored, not guessed at.
    CHECK(Equal(g.d.Rect(g.overview), { 1000, 224, 264, 400 }));
    CHECK(g.d.IsOpen(g.overview));

    // Reset puts everything back.
    g.d.Reset();
    CHECK(Equal(g.d.Rect(g.status), { 56, 16, 264, 312 }));
}

TEST_CASE("Desk: a resizable window keeps its corner, its minimum, and its size after a restart")
{
    DeskLayout d;
    WindowSpec spec = Panel("status", Anchor::TopLeft, { 56, 16, 264, 312 });
    spec.resizable = true;
    spec.minSize = { 200, 150 };
    const int fixed = d.Add(Panel("fixed", Anchor::TopLeft, { 400, 16, 200, 200 }), true);
    const int h = d.Add(spec, true);

    d.Resize(h, 480, 360);
    CHECK(Equal(d.Rect(h), { 56, 16, 480, 360 }));
    d.Resize(h, 10, 10);  // never below its minimum
    CHECK(Equal(d.Rect(h), { 56, 16, 200, 150 }));
    d.Resize(h, 5000, 5000);  // never above the screen
    CHECK(d.Rect(h).width <= 1280.0f);
    CHECK(d.Rect(h).height <= 720.0f);
    d.Resize(fixed, 500, 500);  // a window that is not resizable is not
    CHECK(Equal(d.Rect(fixed), { 400, 16, 200, 200 }));

    d.Resize(h, 480, 360);
    d.Settle(h);
    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", d);

    DeskLayout after;
    after.Add(Panel("fixed", Anchor::TopLeft, { 400, 16, 200, 200 }), true);
    const int   h2 = after.Add(spec, true);
    std::string error;
    REQUIRE(DeskLayout::ReadFile(nlohmann::json::parse(file.dump()), "pilot", after, error));
    CHECK(Equal(after.Rect(h2), { 56, 16, 480, 360 }));
}

TEST_CASE("Desk: at a larger UI scale the windows are larger, and the layout is kept in units")
{
    GameDesk g;
    g.d.SetUnit(1.5f);
    CHECK(Equal(g.d.Rect(g.status), { 84, 24, 396, 468 }));
    // Right stays right: 16 units from the edge is 24 pixels.
    CHECK(Equal(g.d.Rect(g.target), { 1280 - 24 - 396, 24, 396, 294 }));

    // A drag at 1.5 is saved in units, so the same layout comes back at 1.
    g.d.SetRect(g.radar, { 150, 60, 396, 432 });
    g.d.Settle(g.radar);
    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", g.d);
    GameDesk    after;
    std::string error;
    REQUIRE(DeskLayout::ReadFile(nlohmann::json::parse(file.dump()), "pilot", after.d, error));
    CHECK(Equal(after.d.Rect(after.radar), { 100, 40, 264, 288 }));
}

TEST_CASE("Desk: a layout file from a newer build is refused, and other accounts survive a write")
{
    GameDesk             g;
    std::string          error;
    const nlohmann::json future = { { "version", DeskLayout::VERSION + 1 },
                                    { "accounts", nlohmann::json::object() } };
    CHECK_FALSE(DeskLayout::ReadFile(future, "pilot", g.d, error));
    CHECK(error.find("newer") != std::string::npos);
    CHECK(DeskLayout::ReadFile(nlohmann::json::object(), "pilot", g.d, error));  // nothing yet

    nlohmann::json file = {
        { "version", 1 }, { "accounts", { { "bob", { { "status", { { "open", false } } } } } } }
    };
    DeskLayout::WriteFile(file, "pilot", g.d);
    CHECK(file["accounts"].contains("bob"));
    CHECK(file["accounts"].contains("pilot"));
    // Only what a player arranges is kept: screens and popups are not.
    CHECK_FALSE(file["accounts"]["pilot"].contains("map"));
}

TEST_CASE("Desk: a widget under a window that does not own the mouse sees no hover and no click")
{
    // Without a window raylib reports the cursor at the origin, so a rectangle around the
    // origin is "under the cursor". Outside any scope the mouse is everyone's -- the editor
    // has no desk.
    const Rectangle r{ -10, -10, 20, 20 };
    CHECK(Ui::MouseOver(r));
    {
        Ui::MouseScope behind(false);
        CHECK_FALSE(Ui::MouseOver(r));
        CHECK_FALSE(Ui::MouseOwned());
        {
            Ui::MouseScope front(true);
            CHECK(Ui::MouseOver(r));
        }
        CHECK_FALSE(Ui::MouseOver(r));  // scopes nest
    }
    CHECK(Ui::MouseOver(r));

    // A window's content gets the same answer through its frame.
    CHECK(Ui::Frame(r, true).Hovered(r));
    CHECK_FALSE(Ui::Frame(r, false).Hovered(r));

    // And the owner is decided by the layout: the front window's frame owns the point, the
    // one behind it does not.
    DeskLayout d;
    const int  back = d.Add(Panel("back", Anchor::TopLeft, { -50, -50, 100, 100 }), true);
    const int  front = d.Add(Panel("front", Anchor::TopLeft, { -20, -20, 100, 100 }), true);
    const int  owner = d.UpdateOwner({ 0, 0 }, false);
    CHECK_FALSE(Ui::Frame(d.Rect(back), owner == back).Hovered(r));
    CHECK(Ui::Frame(d.Rect(front), owner == front).Hovered(r));
}

// --- Window behaviour (#297): snapping, groups, stacks, pinning, collapsing ---------------

TEST_CASE("Desk: a dragged edge snaps to the screen and to another window, not from afar")
{
    const std::vector<Rectangle> other{ { 100, 100, 200, 200 } };
    // 6 right of the other's right edge and 5 below its top: beside it, in line with it.
    Vector2 d = Ui::SnapOffset({ { 306, 105, 200, 200 } }, other, 1280, 720, 12);
    CHECK(d.x == doctest::Approx(-6));
    CHECK(d.y == doctest::Approx(-5));
    // Out of reach on both axes: nothing.
    d = Ui::SnapOffset({ { 330, 160, 200, 200 } }, other, 1280, 720, 12);
    CHECK(d.x == doctest::Approx(0));
    CHECK(d.y == doctest::Approx(0));
    // Into a corner of the screen.
    d = Ui::SnapOffset({ { 1075, 515, 200, 200 } }, {}, 1280, 720, 12);
    CHECK(d.x == doctest::Approx(5));
    CHECK(d.y == doctest::Approx(5));
    // A window far below is not an edge to snap beside, however near its x.
    d = Ui::SnapOffset({ { 304, 600, 100, 50 } }, other, 1280, 720, 12);
    CHECK(d.x == doctest::Approx(0));

    CHECK(Ui::Touching({ 100, 100, 200, 200 }, { 300, 150, 100, 100 }));  // side by side
    CHECK(Ui::Touching({ 100, 100, 200, 200 }, { 150, 300, 100, 100 }));  // one above the other
    CHECK_FALSE(Ui::Touching({ 100, 100, 200, 200 }, { 300, 300, 100, 100 }));  // corners only
    CHECK_FALSE(Ui::Touching({ 100, 100, 200, 200 }, { 304, 100, 100, 100 }));  // a gap
}

TEST_CASE("Desk: a window snapped to another joins it, they move together, and Shift parts them")
{
    DeskLayout d;
    const int  a = d.Add(Panel("a", Anchor::TopLeft, { 100, 100, 200, 200 }), true);
    const int  b = d.Add(Panel("b", Anchor::TopLeft, { 500, 100, 200, 200 }), true);
    CHECK(d.Group(a) == std::vector<int>{ a });

    REQUIRE(d.BeginMove(b, false));
    d.MoveTo({ 306, 105 });  // let go near a: it snaps flush
    CHECK(Equal(d.Rect(b), { 300, 100, 200, 200 }));
    CHECK(d.EndMove({ 350, 110 }) == DeskLayout::NONE);  // not onto a title: not stacked
    CHECK(d.Group(a) == std::vector<int>{ a, b });

    // Either one drags both.
    REQUIRE(d.BeginMove(a, false));
    d.MoveTo({ 150, 300 });
    d.EndMove({ 160, 310 });
    CHECK(Equal(d.Rect(a), { 150, 300, 200, 200 }));
    CHECK(Equal(d.Rect(b), { 350, 300, 200, 200 }));
    // Raising one brings its group forward.
    const int c = d.Add(Panel("c", Anchor::TopLeft, { 0, 0, 900, 700 }), true);
    d.Raise(a);
    CHECK(d.Order() == std::vector<int>{ c, b, a });

    // Shift: b goes alone, and the group of one left behind is no group.
    REQUIRE(d.BeginMove(b, true));
    CHECK(d.Group(a) == std::vector<int>{ a });
    d.MoveTo({ 800, 300 });
    d.EndMove({ 810, 310 });
    CHECK(Equal(d.Rect(a), { 150, 300, 200, 200 }));
    CHECK(d.Group(b) == std::vector<int>{ b });
}

TEST_CASE("Desk: a group keeps together after a restart, at another screen size and UI scale")
{
    auto build = [](DeskLayout& d, int& a, int& b)
    {
        a = d.Add(Panel("a", Anchor::TopRight, { 16, 16, 264, 200 }), true);
        b = d.Add(Panel("b", Anchor::TopLeft, { 100, 400, 264, 200 }), true);
    };
    DeskLayout before;
    int        a = 0, b = 0;
    build(before, a, b);
    REQUIRE(before.BeginMove(b, false));
    before.MoveTo({ 1003, 220 });  // under a, on the right
    before.EndMove({ 1100, 230 });
    CHECK(Equal(before.Rect(b), { 1000, 216, 264, 200 }));
    REQUIRE(before.Group(a).size() == 2);
    // One anchor for the group, from where the group is: b came from the left, and on a
    // wider screen would otherwise be left behind there.
    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", before);
    CHECK(file["accounts"]["pilot"]["b"]["anchor"] == "top_right");
    CHECK(file["accounts"]["pilot"]["b"]["group"] == "a");

    DeskLayout  after;
    std::string error;
    build(after, a, b);
    REQUIRE(DeskLayout::ReadFile(nlohmann::json::parse(file.dump()), "pilot", after, error));
    CHECK(after.Group(b) == std::vector<int>{ a, b });
    after.SetScreen(1920, 1080);
    CHECK(Equal(after.Rect(b), { 1640, 216, 264, 200 }));
    CHECK(Ui::Touching(after.Rect(a), after.Rect(b)));
    after.SetUnit(1.5f);
    CHECK(Ui::Touching(after.Rect(a), after.Rect(b)));
}

TEST_CASE("Desk: a pinned window is not moved, resized or closed by Esc, and anchors its group")
{
    DeskLayout d;
    WindowSpec spec = Panel("a", Anchor::TopLeft, { 100, 100, 200, 200 });
    spec.resizable = true;
    const int a = d.Add(spec, true);
    const int b = d.Add(Panel("b", Anchor::TopLeft, { 500, 100, 200, 200 }), true);
    REQUIRE(d.BeginMove(b, false));
    d.MoveTo({ 300, 100 });
    d.EndMove({ 350, 110 });
    REQUIRE(d.Group(a).size() == 2);

    d.SetPinned(a, true);
    CHECK_FALSE(d.BeginMove(a, false));
    d.Resize(a, 400, 400);
    CHECK(Equal(d.Rect(a), { 100, 100, 200, 200 }));
    // Esc passes over it to the window behind.
    d.Raise(a);
    CHECK(d.Escape() == b);
    CHECK(d.IsOpen(a));
    d.SetOpen(b, true);

    // Dragging the other one takes it out of the group; the pinned one stays.
    REQUIRE(d.BeginMove(b, false));
    d.MoveTo({ 700, 400 });
    d.EndMove({ 710, 410 });
    CHECK(Equal(d.Rect(a), { 100, 100, 200, 200 }));
    CHECK(Equal(d.Rect(b), { 700, 400, 200, 200 }));
    CHECK(d.Group(a) == std::vector<int>{ a });

    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", d);
    DeskLayout after;
    after.Add(spec, true);
    std::string error;
    REQUIRE(DeskLayout::ReadFile(nlohmann::json::parse(file.dump()), "pilot", after, error));
    CHECK(after.Pinned(0));
}

TEST_CASE("Desk: a collapsed window is its title bar, and opens out to the size it had")
{
    DeskLayout d;
    WindowSpec top = Panel("top", Anchor::TopLeft, { 100, 100, 200, 300 });
    top.resizable = true;
    const int t = d.Add(top, true);
    const int low = d.Add(Panel("low", Anchor::BottomLeft, { 400, 16, 200, 300 }), true);

    d.SetCollapsed(t, true);
    CHECK(Equal(d.Rect(t), { 100, 100, 200, 26 }));
    d.Resize(t, 500, 500);  // no grip while collapsed
    CHECK(Equal(d.Rect(t), { 100, 100, 200, 26 }));
    // Moved while collapsed, it opens out where it was moved to.
    REQUIRE(d.BeginMove(t, false));
    d.MoveTo({ 150, 200 });
    d.EndMove({ 160, 210 });
    d.SetCollapsed(t, false);
    CHECK(Equal(d.Rect(t), { 150, 200, 200, 300 }));

    // From the bottom too, the title stays where the top of the window was.
    d.SetCollapsed(low, true);
    CHECK(Equal(d.Rect(low), { 400, 404, 200, 26 }));
    // Under the cursor only where it is drawn.
    CHECK(d.HitTest({ 450, 500 }) == DeskLayout::NONE);

    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", d);
    DeskLayout after;
    after.Add(top, true);
    const int   low2 = after.Add(Panel("low", Anchor::BottomLeft, { 400, 16, 200, 300 }), true);
    std::string error;
    REQUIRE(DeskLayout::ReadFile(nlohmann::json::parse(file.dump()), "pilot", after, error));
    CHECK(after.Collapsed(low2));
    CHECK(Equal(after.Rect(low2), { 400, 404, 200, 26 }));
    after.SetCollapsed(low2, false);
    CHECK(Equal(after.Rect(low2), { 400, 404, 200, 300 }));
}

TEST_CASE("Desk: a window let go on another's title becomes a tab of it, and comes out again")
{
    DeskLayout d;
    const int  one = d.Add(Panel("one", Anchor::TopLeft, { 100, 100, 250, 300 }), true);
    const int  two = d.Add(Panel("two", Anchor::TopLeft, { 500, 100, 250, 300 }), true);
    const int  three = d.Add(Panel("three", Anchor::TopLeft, { 800, 400, 200, 200 }), true);

    REQUIRE(d.BeginMove(two, false));
    d.MoveTo({ 150, 108 });
    CHECK(d.DropTarget({ 160, 115 }) == one);               // over one's title bar
    CHECK(d.DropTarget({ 160, 200 }) == DeskLayout::NONE);  // over its body
    CHECK(d.EndMove({ 160, 115 }) == one);

    CHECK(d.FrameOf(one) == std::vector<int>{ one, two });
    CHECK(Equal(d.Rect(two), d.Rect(one)));
    CHECK(d.ActiveTab(one) == two);  // what was dropped is in front
    CHECK_FALSE(d.Shown(one));
    CHECK(d.HitTest({ 200, 200 }) == two);

    d.Raise(one);  // its tab clicked
    CHECK(d.Shown(one));
    CHECK_FALSE(d.Shown(two));
    // Closing the tab in front shows the next; a closed tab is not in the strip.
    CHECK(d.Escape() == one);
    CHECK(d.Shown(two));
    CHECK(d.Tabs(one) == std::vector<int>{ two });
    d.SetOpen(one, true);
    d.Raise(one);

    // The stack moves, pins and collapses as one.
    REQUIRE(d.BeginMove(two, false));
    d.MoveTo({ 300, 300 });
    d.EndMove({ 310, 310 });
    CHECK(Equal(d.Rect(one), { 300, 300, 250, 300 }));
    d.SetPinned(two, true);
    CHECK(d.Pinned(one));
    d.SetPinned(one, false);
    d.SetCollapsed(one, true);
    CHECK(d.Collapsed(two));
    d.SetCollapsed(two, false);

    // Saved and read back: the same stack, the same tab in front.
    nlohmann::json file;
    DeskLayout::WriteFile(file, "pilot", d);
    {
        DeskLayout  after;
        std::string error;
        const int   o = after.Add(Panel("one", Anchor::TopLeft, { 100, 100, 250, 300 }), true);
        const int   t = after.Add(Panel("two", Anchor::TopLeft, { 500, 100, 250, 300 }), true);
        after.Add(Panel("three", Anchor::TopLeft, { 800, 400, 200, 200 }), true);
        REQUIRE(DeskLayout::ReadFile(nlohmann::json::parse(file.dump()), "pilot", after, error));
        CHECK(after.FrameOf(t) == std::vector<int>{ o, t });
        CHECK(after.ActiveTab(t) == o);
        CHECK(Equal(after.Rect(t), { 300, 300, 250, 300 }));
    }

    // Pulled out, it is a window of its own where it was.
    d.Unstack(two);
    CHECK(d.FrameOf(one) == std::vector<int>{ one });
    CHECK(d.Shown(one));
    CHECK(d.Shown(two));

    // A group is not stacked onto anything: only one frame can become tabs.
    REQUIRE(d.BeginMove(two, false));
    d.MoveTo({ 700, 50 });  // out of the way
    d.EndMove({ 710, 60 });
    REQUIRE(d.BeginMove(three, false));
    d.MoveTo({ 550, 400 });  // beside one
    d.EndMove({ 560, 410 });
    REQUIRE(d.Group(three).size() == 2);
    REQUIRE(d.BeginMove(three, false));
    d.MoveTo({ 150, 108 });
    CHECK(d.DropTarget({ 160, 115 }) == DeskLayout::NONE);
    d.EndMove({ 160, 115 });
    CHECK(d.FrameOf(one) == std::vector<int>{ one });
}

TEST_CASE("Desk: a stack or a group that comes back with one window in it is forgotten")
{
    GameDesk             g;
    const nlohmann::json windows = {
        { "status", { { "stack", "status" }, { "tab", 1 }, { "front", true } } },
        { "overview", { { "group", "nothing_else" } } },
    };
    g.d.Load(windows);
    CHECK(g.d.FrameOf(g.status) == std::vector<int>{ g.status });
    CHECK(g.d.Group(g.overview) == std::vector<int>{ g.overview });
    CHECK(g.d.Shown(g.status));
}

TEST_CASE("Desk: a dragged window snaps against the menu bar, and is not grouped with it")
{
    WindowSpec bar = Surface("menubar", Layer::Modal, EscRule::Ignore);
    bar.snapTarget = true;
    DeskLayout d;
    const int  menu = d.Add(bar, true);
    d.SetRect(menu, { 0, 0, 46, 720 });
    const int w = d.Add(Panel("w", Anchor::TopLeft, { 300, 300, 200, 200 }), true);
    REQUIRE(d.BeginMove(w, false));
    d.MoveTo({ 52, 300 });
    d.EndMove({ 60, 310 });
    CHECK(d.Rect(w).x == doctest::Approx(46));
    CHECK(d.Group(w) == std::vector<int>{ w });  // an edge, not a group
}
