#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/World.h"
#include "render/TextBackend.h"
#include "sim/Sensor.h"
#include "factions_named.h"
#include <cstring>
#include <map>
#include <string>

// The sensor screen (#123) is the world projected onto a grid, coloured by allegiance as the
// viewer sees it (#117). Both halves are decided without a window, so they are tested here.

namespace
{
Render::Item Thing(int id, EntityKind kind, float x, float y, char glyph, int layer = 0)
{
    Render::Item it;
    it.id = id;
    it.kind = kind;
    it.pos = { x, y };
    it.glyph = std::string(1, glyph);
    it.layer = layer;
    // A colour that would be wrong for every allegiance: the screen must never show it.
    it.color = { 255, 0, 255, 255 };
    return it;
}

Proto::EntitySnapshot Snap(Proto::EntityKind kind, FactionId f)
{
    Proto::EntitySnapshot e;
    e.kind = kind;
    e.faction = f;
    return e;
}

bool Same(Color a, Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
}  // namespace

TEST_CASE("the grid remembers what is in a cell, not only its character")
{
    Render::GridBackend grid(9, 9, 900.0f);
    Render::Present({ Thing(42, EntityKind::Station, 200.0f, -300.0f, '#') }, grid);
    CHECK(grid.At(6, 1) == '#');
    CHECK(grid.IdAt(6, 1) == 42);
    CHECK(grid.IdAt(4, 4) == 0);
    CHECK(grid.IdAt(-1, 0) == 0);  // off the grid is empty, not a crash

    int x = 0, y = 0;
    CHECK(grid.CellOf({ 200.0f, -300.0f }, x, y));
    CHECK(x == 6);
    CHECK(y == 1);
    CHECK_FALSE(grid.CellOf({ 5000.0f, 0.0f }, x, y));

    Render::Present({}, grid);
    CHECK(grid.IdAt(6, 1) == 0);  // cleared with the characters
}

TEST_CASE("allegiance is a reading of the viewer's standing, not a property of the object")
{
    // One station, two viewers: the same snapshot reads differently to each, which is the
    // whole reason the colour lives in the instrument and not on the object.
    const Proto::EntitySnapshot hub = Snap(Proto::EntityKind::Station, Fac::TradersGuild());

    Sensor::Standing friendOfGuild;
    friendOfGuild.hostile = [](FactionId f) { return f == Fac::Pirates(); };
    friendOfGuild.tier = [](FactionId f)
    { return f == Fac::TradersGuild() ? RepTier::Allied : RepTier::Neutral; };

    Sensor::Standing wantedByGuild;
    wantedByGuild.hostile = [](FactionId f)
    { return f == Fac::Pirates() || f == Fac::TradersGuild(); };
    wantedByGuild.tier = [](FactionId) { return RepTier::Neutral; };

    CHECK(Sensor::Classify(hub, friendOfGuild) == Sensor::Allegiance::Friendly);
    CHECK(Sensor::Classify(hub, wantedByGuild) == Sensor::Allegiance::Hostile);

    SUBCASE("ships read the same way stations do")
    {
        const Proto::EntitySnapshot pirate = Snap(Proto::EntityKind::Npc, Fac::Pirates());
        CHECK(Sensor::Classify(pirate, friendOfGuild) == Sensor::Allegiance::Hostile);
        const Proto::EntitySnapshot miner = Snap(Proto::EntityKind::Npc, Fac::Independent());
        CHECK(Sensor::Classify(miner, friendOfGuild) == Sensor::Allegiance::Neutral);
    }

    SUBCASE("a thing with no side reads as unowned, whoever looks")
    {
        for (Proto::EntityKind k :
             { Proto::EntityKind::Star, Proto::EntityKind::Planet, Proto::EntityKind::Field,
               Proto::EntityKind::Gate, Proto::EntityKind::Nebula, Proto::EntityKind::Derelict })
        {
            CHECK(Sensor::Classify(Snap(k, Fac::Pirates()), wantedByGuild) ==
                  Sensor::Allegiance::Unowned);
        }
    }

    SUBCASE("another pilot is neutral until players have standings")
    {
        CHECK(Sensor::Classify(Snap(Proto::EntityKind::PlayerShip, Fac::Pirates()),
                               friendOfGuild) == Sensor::Allegiance::Neutral);
    }

    SUBCASE("a viewer with no standing at all sees everything with a side as neutral")
    {
        CHECK(Sensor::Classify(hub, Sensor::Standing{}) == Sensor::Allegiance::Neutral);
    }
}

TEST_CASE("every allegiance has its own colour and word")
{
    const Sensor::Allegiance all[] = { Sensor::Allegiance::Own, Sensor::Allegiance::Friendly,
                                       Sensor::Allegiance::Neutral, Sensor::Allegiance::Hostile,
                                       Sensor::Allegiance::Unowned };
    for (Sensor::Allegiance a : all)
        for (Sensor::Allegiance b : all)
            if (a != b)
            {
                CHECK_FALSE(Same(Sensor::ColorOf(a), Sensor::ColorOf(b)));
                CHECK(std::strcmp(Sensor::Word(a), Sensor::Word(b)) != 0);
            }
}

TEST_CASE("the screen is the world around the ship, coloured by who is looking")
{
    // 11 cells across 1100 units: 100 units a cell, the ship in the centre cell (5, 5).
    Render::Item own = Thing(0, EntityKind::PlayerShip, 5000.0f, 5000.0f, 'A');

    std::vector<Render::Item> scene = {
        Thing(1, EntityKind::Station, 5300.0f, 5000.0f, '#', 2),  // 3 cells east
        Thing(2, EntityKind::Npc, 5000.0f, 4600.0f, 'v', 3),      // 4 cells north
        Thing(3, EntityKind::Nebula, 5300.0f, 5000.0f, '~', 0),   // under the station
        Thing(4, EntityKind::Star, 90000.0f, 0.0f, '*', 1),       // far outside the grid
        Thing(5, EntityKind::Npc, 5000.0f, 5000.0f, 'v', 3),      // on top of the viewer
    };
    std::map<int, Sensor::Allegiance> reads = { { 1, Sensor::Allegiance::Friendly },
                                                { 2, Sensor::Allegiance::Hostile },
                                                { 3, Sensor::Allegiance::Unowned },
                                                { 4, Sensor::Allegiance::Unowned },
                                                { 5, Sensor::Allegiance::Neutral } };
    const Sensor::Picture             p =
        Sensor::Scan(scene, own, 11, 11, 1100.0f, [&](int id) { return reads[id]; });

    REQUIRE(p.width == 11);
    REQUIRE(p.height == 11);
    CHECK(p.UnitsPerCell() == doctest::Approx(100.0f));

    // Centred on the ship rather than the system: the ship is far from the origin.
    CHECK(p.At(8, 5).glyph == '#');
    CHECK(p.At(8, 5).id == 1);
    CHECK(p.At(8, 5).allegiance == Sensor::Allegiance::Friendly);  // the higher layer won
    CHECK(p.At(5, 1).glyph == 'v');
    CHECK(p.At(5, 1).allegiance == Sensor::Allegiance::Hostile);

    // The viewer always holds the centre, even with something parked on top of it -- which
    // moves aside rather than vanishing (#366).
    CHECK(p.At(5, 5).glyph == 'A');
    CHECK(p.At(5, 5).allegiance == Sensor::Allegiance::Own);
    CHECK_FALSE(p.At(5, 5).offset);
    CHECK(p.At(6, 5).id == 5);
    CHECK(p.At(6, 5).offset);
    // ...and so does the nebula under the station.
    CHECK(p.At(9, 5).id == 3);
    CHECK_FALSE(p.At(8, 5).offset);

    // Off the grid is dropped rather than smeared onto the edge.
    int marks = 0;
    for (const Sensor::Cell& c : p.cells)
        if (c.glyph != ' ')
            marks++;
    CHECK(marks == 5);

    // The legend lists what is on the screen and nothing else: no star, the viewer first.
    REQUIRE_FALSE(p.legend.empty());
    CHECK(p.legend[0].glyph == 'A');
    CHECK(p.legend[0].kind == "you");
    bool star = false, station = false, ship = false;
    for (const Sensor::LegendEntry& l : p.legend)
    {
        star |= l.glyph == '*';
        station |= (l.glyph == '#' && l.kind == "station");
        ship |= (l.glyph == 'v' && l.kind == "ship");
    }
    CHECK_FALSE(star);
    CHECK(station);
    CHECK(ship);
}

TEST_CASE("two things in one cell are both on the screen, each on its own side (#366)")
{
    // 11 cells of 100 units; the ship at the origin, the centre cell (5, 5).
    const Render::Item own = Thing(0, EntityKind::PlayerShip, 0.0f, 0.0f, 'A');
    const auto         scan = [&](std::vector<Render::Item> scene)
    {
        return Sensor::Scan(std::move(scene), own, 11, 11, 1100.0f,
                            [](int) { return Sensor::Allegiance::Unowned; });
    };

    SUBCASE("a moon lies east of its planet: it is drawn east of it")
    {
        const Sensor::Picture p = scan({ Thing(1, EntityKind::Planet, 300.0f, 0.0f, 'o', 1),
                                         Thing(2, EntityKind::Station, 330.0f, 0.0f, '#', 4) });
        // The station is the higher layer and keeps the cell; the planet gives way on the
        // side it lies from the station -- west.
        CHECK(p.At(8, 5).id == 2);
        CHECK(p.At(7, 5).id == 1);
        CHECK(p.At(7, 5).offset);
    }
    SUBCASE("south of it: south")
    {
        const Sensor::Picture p = scan({ Thing(1, EntityKind::Planet, 300.0f, 0.0f, 'o', 1),
                                         Thing(2, EntityKind::Station, 300.0f, -30.0f, '#', 4) });
        CHECK(p.At(8, 5).id == 2);
        CHECK(p.At(8, 6).id == 1);  // +y is down the screen
    }
    SUBCASE("a neighbour already taken is passed over for the next best")
    {
        const Sensor::Picture p = scan({ Thing(3, EntityKind::Field, 200.0f, 0.0f, '%', 2),
                                         Thing(1, EntityKind::Planet, 270.0f, 0.0f, 'o', 1),
                                         Thing(2, EntityKind::Station, 300.0f, 0.0f, '#', 4) });
        CHECK(p.At(8, 5).id == 2);
        CHECK(p.At(7, 5).id == 3);
        // West is the belt's, so the planet takes a corner on the west side.
        const int nw = p.At(7, 4).id, sw = p.At(7, 6).id;
        CHECK((nw == 1 || sw == 1));
    }
    SUBCASE("a cell hemmed in on every side keeps the one on top and hides the rest")
    {
        std::vector<Render::Item> scene;
        for (int i = 0; i < 10; i++)
            scene.push_back(Thing(10 + i, EntityKind::Npc, 300.0f, 300.0f, 'v', 5));
        const Sensor::Picture p = scan(scene);
        int                   shown = 0;
        for (const Sensor::Cell& c : p.cells)
            if (c.id >= 10)
                shown++;
        CHECK(shown == 9);
    }
}

TEST_CASE("the legend has one line per glyph and never one word for two (#366)")
{
    const Render::Item own = Thing(0, EntityKind::PlayerShip, 0.0f, 0.0f, 'A');
    // A rocky planet, an ice planet, a gas giant and a buoy that shares the planets' glyph.
    const std::vector<Render::Item> scene = {
        Thing(1, EntityKind::Planet, 300.0f, 0.0f, 'o', 1),
        Thing(2, EntityKind::Planet, -300.0f, 0.0f, 'o', 1),
        Thing(3, EntityKind::Planet, 0.0f, 300.0f, 'O', 1),
        Thing(4, EntityKind::Structure, 0.0f, -300.0f, 'o', 4),
        Thing(5, EntityKind::Gate, 400.0f, 400.0f, 'H', 3),
    };
    const std::map<int, std::string> types = { { 1, "Rocky Planet" },
                                               { 2, "Ice Planet" },
                                               { 3, "Gas Giant" },
                                               { 4, "Buoy" },
                                               { 5, "Jump Gate" } };
    const Sensor::Picture            p = Sensor::Scan(
        scene, own, 11, 11, 1100.0f, [](int) { return Sensor::Allegiance::Unowned; },
        [&](int id) { return types.at(id); });

    std::map<char, std::string> byGlyph;
    for (const Sensor::LegendEntry& l : p.legend)
    {
        CHECK_MESSAGE(byGlyph.count(l.glyph) == 0, "glyph listed twice: ", l.glyph);
        byGlyph[l.glyph] = l.kind;
    }
    CHECK(byGlyph['A'] == "you");
    CHECK(byGlyph['H'] == "gate");
    // 'o' is several things, and says so; 'O' would have been "planet" too, so it is named.
    CHECK(byGlyph['o'] == "structure, planet");
    CHECK(byGlyph['O'] == "gas giant");

    SUBCASE("with only planets of one glyph each, the word is enough")
    {
        const Sensor::Picture q = Sensor::Scan(
            { Thing(1, EntityKind::Planet, 300.0f, 0.0f, 'o', 1) }, own, 11, 11, 1100.0f,
            [](int) { return Sensor::Allegiance::Unowned; }, [&](int id) { return types.at(id); });
        REQUIRE(q.legend.size() == 2);
        CHECK(q.legend[1].kind == "planet");
    }
}

TEST_CASE("a gate and a planet are different characters on the sensor screen (#366)")
{
    // '0' and 'O' are one shape in most faces, and a gate read as a planet is a pilot
    // warping to the wrong thing. No two kinds share a glyph that another kind's could be
    // mistaken for, and a gate shares none with anything.
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const char* confusable[][2] = { { "0", "O" }, { "0", "o" }, { "1", "l" },
                                    { "1", "I" }, { "l", "|" }, { "I", "|" } };
    std::map<std::string, EntityKind> kindOf;
    for (const Archetype& a : Archetypes::All())
        kindOf.emplace(a.visual.glyph, a.kind);
    for (const auto& pair : confusable)
    {
        const bool both = kindOf.count(pair[0]) > 0 && kindOf.count(pair[1]) > 0;
        CHECK_MESSAGE(!both, "'", pair[0], "' and '", pair[1], "' are both in use");
    }
    for (const Archetype* gate : Archetypes::OfKind(EntityKind::Gate))
        for (const Archetype& a : Archetypes::All())
            if (a.kind != EntityKind::Gate)
                CHECK_MESSAGE(a.visual.glyph != gate->visual.glyph, a.id, " looks like a gate");
}

TEST_CASE("the range steps through fixed scales and stops at the ends")
{
    const std::vector<float>& r = Sensor::Ranges();
    REQUIRE(r.size() >= 3);
    for (size_t i = 1; i < r.size(); i++)
        CHECK(r[i] > r[i - 1]);

    // The outermost step takes in a whole system, edge to edge (#159).
    CHECK(r.back() >= 2.0f * World::SYSTEM_RADIUS);
    CHECK(Sensor::StepRange(Sensor::DefaultRange(), 0) == Sensor::DefaultRange());

    CHECK(Sensor::StepRange(r[1], 1) == r[2]);
    CHECK(Sensor::StepRange(r[1], -1) == r[0]);
    CHECK(Sensor::StepRange(r[0], -5) == r[0]);
    CHECK(Sensor::StepRange(r.back(), 3) == r.back());
    // A range between steps moves from the nearest one.
    CHECK(Sensor::StepRange(r[2] * 1.01f, 1) == r[3]);
}
