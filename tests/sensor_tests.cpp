#include <doctest/doctest.h>

#include "core/World.h"
#include "render/TextBackend.h"
#include "sim/Sensor.h"
#include <cstring>
#include <map>

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
    const Proto::EntitySnapshot hub = Snap(Proto::EntityKind::Station, FactionId::TradersGuild);

    Sensor::Standing friendOfGuild;
    friendOfGuild.hostile = [](FactionId f) { return f == FactionId::Pirates; };
    friendOfGuild.tier = [](FactionId f)
    { return f == FactionId::TradersGuild ? RepTier::Allied : RepTier::Neutral; };

    Sensor::Standing wantedByGuild;
    wantedByGuild.hostile = [](FactionId f)
    { return f == FactionId::Pirates || f == FactionId::TradersGuild; };
    wantedByGuild.tier = [](FactionId) { return RepTier::Neutral; };

    CHECK(Sensor::Classify(hub, friendOfGuild) == Sensor::Allegiance::Friendly);
    CHECK(Sensor::Classify(hub, wantedByGuild) == Sensor::Allegiance::Hostile);

    SUBCASE("ships read the same way stations do")
    {
        const Proto::EntitySnapshot pirate = Snap(Proto::EntityKind::Npc, FactionId::Pirates);
        CHECK(Sensor::Classify(pirate, friendOfGuild) == Sensor::Allegiance::Hostile);
        const Proto::EntitySnapshot miner = Snap(Proto::EntityKind::Npc, FactionId::Independent);
        CHECK(Sensor::Classify(miner, friendOfGuild) == Sensor::Allegiance::Neutral);
    }

    SUBCASE("a thing with no side reads as unowned, whoever looks")
    {
        for (Proto::EntityKind k :
             { Proto::EntityKind::Star, Proto::EntityKind::Planet, Proto::EntityKind::Field,
               Proto::EntityKind::Gate, Proto::EntityKind::Nebula, Proto::EntityKind::Derelict })
        {
            CHECK(Sensor::Classify(Snap(k, FactionId::Pirates), wantedByGuild) ==
                  Sensor::Allegiance::Unowned);
        }
    }

    SUBCASE("another pilot is neutral until players have standings")
    {
        CHECK(Sensor::Classify(Snap(Proto::EntityKind::PlayerShip, FactionId::Pirates),
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

    // The viewer always holds the centre, even with something parked on top of it.
    CHECK(p.At(5, 5).glyph == 'A');
    CHECK(p.At(5, 5).allegiance == Sensor::Allegiance::Own);

    // Off the grid is dropped rather than smeared onto the edge.
    int marks = 0;
    for (const Sensor::Cell& c : p.cells)
        if (c.glyph != ' ')
            marks++;
    CHECK(marks == 3);

    // The legend lists what is on the screen and nothing else: no star, no nebula (it is
    // hidden under the station), the viewer first.
    REQUIRE_FALSE(p.legend.empty());
    CHECK(p.legend[0].glyph == 'A');
    bool star = false, station = false, ship = false;
    for (const Sensor::LegendEntry& l : p.legend)
    {
        star |= l.glyph == '*';
        station |= (l.glyph == '#' && std::strcmp(l.kind, "station") == 0);
        ship |= (l.glyph == 'v' && std::strcmp(l.kind, "ship") == 0);
    }
    CHECK_FALSE(star);
    CHECK(station);
    CHECK(ship);
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
