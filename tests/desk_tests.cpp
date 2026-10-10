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
