#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "render/Scene.h"
#include "render/TextBackend.h"
#include <string>

// Before the presentation seam existed, "does the world look right?" was answerable only
// by a human looking at a window. These tests answer it from a scene description, which
// is the point of separating what an object looks like from who draws it (#35).

namespace
{
void LoadRegistry()
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
}
}  // namespace

TEST_CASE("an entity describes itself without drawing anything")
{
    LoadRegistry();

    Station hub({ 100.0f, 0.0f }, 90.0f, "Aurora Hub", FactionId::TradersGuild,
                StationRole::TradeHub);
    hub.SetId(7);

    Render::Item it = hub.Describe();
    CHECK(it.id == 7);
    CHECK(it.kind == EntityKind::Station);
    CHECK(it.label == "Aurora Hub");
    CHECK(it.glyph == "#");
    CHECK(it.size == doctest::Approx(90.0f));
    CHECK(it.pos.x == doctest::Approx(100.0f));

    SUBCASE("a subclass adds only what it alone knows")
    {
        Planet p(3000.0f, 300.0f, 0.0f, 140.0f, WHITE, AllResourceTypes()[0], PlanetType::Rocky);
        CHECK(p.Describe().ring == doctest::Approx(3000.0f));  // the orbit guide
        CHECK(hub.Describe().ring == doctest::Approx(0.0f));   // a station has no orbit

        AsteroidField belt({ 0.0f, 0.0f }, 500.0f, "Belt", AllResourceTypes()[0], 100);
        CHECK(belt.Describe().intensity == doctest::Approx(1.0f));
        belt.Extract(75);
        CHECK(belt.Describe().intensity == doctest::Approx(0.25f));

        Derelict    wreck({ 0.0f, 0.0f }, 40.0f, "Wreck", 1000.0);
        const float intact = wreck.Describe().intensity;
        wreck.SetLooted();
        CHECK(wreck.Describe().intensity < intact);  // dimmer in every backend, not just one
    }
}

TEST_CASE("the text backend renders a scene with no window")
{
    LoadRegistry();

    Station  hub({ 0.0f, -1000.0f }, 90.0f, "Aurora Hub", FactionId::TradersGuild,
                 StationRole::TradeHub);
    JumpGate gate({ 2000.0f, 0.0f }, 150.0f, "Gate to Sigma Reach", "reach");

    Render::TextBackend text({ 0.0f, 0.0f });  // observer at the origin
    Render::Present({ hub.Describe(), gate.Describe() }, text);

    REQUIRE(text.Lines().size() == 2);
    const std::string all = text.Text();
    CHECK(all.find("Aurora Hub") != std::string::npos);
    CHECK(all.find("Gate to Sigma Reach") != std::string::npos);

    SUBCASE("distance and bearing are measured from the observer")
    {
        // Looked up by name rather than by index: Present() orders by layer, not by the
        // order the caller happened to pass things in.
        auto LineFor = [&](const char* name) -> std::string
        {
            for (const std::string& l : text.Lines())
                if (l.find(name) != std::string::npos)
                    return l;
            return std::string();
        };
        // World y grows downward, so an object at -y is north. Getting this backwards
        // produces directions that are wrong without anything failing.
        const std::string station = LineFor("Aurora Hub");
        REQUIRE_FALSE(station.empty());
        CHECK(station.find("1000m") != std::string::npos);
        CHECK(station.find(" N") != std::string::npos);

        const std::string gate = LineFor("Gate to Sigma Reach");
        REQUIRE_FALSE(gate.empty());
        CHECK(gate.find("2000m") != std::string::npos);
        CHECK(gate.find(" E") != std::string::npos);
    }
}

TEST_CASE("layers decide what covers what, in every backend at once")
{
    LoadRegistry();

    // A station and a nebula in the same place. The station sits on a higher layer, so it
    // is the one you see -- the rule that used to be implicit in the order entities
    // happened to be stored in.
    Station  hub({ 0.0f, 0.0f }, 90.0f, "Aurora Hub", FactionId::TradersGuild,
                 StationRole::TradeHub);
    JumpGate gate({ 0.0f, 0.0f }, 150.0f, "Gate", "reach");
    REQUIRE(hub.Describe().layer > gate.Describe().layer);

    Render::GridBackend grid(9, 9, 900.0f);

    SUBCASE("the higher layer wins regardless of the order given")
    {
        Render::Present({ hub.Describe(), gate.Describe() }, grid);
        CHECK(grid.At(4, 4) == '#');

        Render::Present({ gate.Describe(), hub.Describe() }, grid);
        CHECK(grid.At(4, 4) == '#');
    }

    SUBCASE("position maps to a cell, and off-grid objects are dropped rather than clamped")
    {
        Station far({ 400.0f, 0.0f }, 90.0f, "Far", FactionId::Independent, StationRole::TradeHub);
        Station away({ 90000.0f, 0.0f }, 90.0f, "Away", FactionId::Independent,
                     StationRole::TradeHub);
        Render::Present({ far.Describe(), away.Describe() }, grid);
        CHECK(grid.At(8, 4) == '#');  // 400 units east at 100 units per cell
        CHECK(grid.At(4, 4) == ' ');  // nothing at the centre
        // "Away" is far outside the grid: it must not smear onto the edge, which would
        // read as an object being somewhere it is not.
        CHECK(grid.Text().find_first_not_of(" #\n") == std::string::npos);
    }

    SUBCASE("a scene is cleared between presentations, not accumulated")
    {
        Render::Present({ hub.Describe() }, grid);
        REQUIRE(grid.At(4, 4) == '#');
        Render::Present({}, grid);
        CHECK(grid.At(4, 4) == ' ');
    }
}

// #127: the player's own ship shipped for months with no archetype at all, because the
// client built it before Archetypes::Load ran. Nothing failed -- it simply drew as the
// fallback glyph, and the one object affected was the one nobody inspects.
//
// The ordering itself lives in a constructor and cannot be reached from here. What can be
// reached is the thing that made it invisible: an entity whose archetype is missing looks
// exactly like one that never wanted an archetype. So this pins the two states apart.
TEST_CASE("an entity built without the registry has no look, and that is distinguishable")
{
    LoadRegistry();

    Station withRegistry({ 0.0f, 0.0f }, 90.0f, "Hub", FactionId::Independent,
                         StationRole::TradeHub);
    REQUIRE(withRegistry.GetArchetype() != nullptr);
    CHECK(withRegistry.Describe().glyph == "#");

    SUBCASE("every archetype an entity constructor asks for actually exists")
    {
        // The other half of #127: a null archetype can also mean a typo in an id, and one
        // of these ids being wrong would be just as silent. They are checked here rather
        // than trusted, because they are string literals spread across nine files.
        const char* ids[] = { "field.asteroid",    "derelict.wreck",
                              "gate.jump",         "nebula.cloud",
                              "star.yellow",       "star.red",
                              "star.blue",         "planet.rocky",
                              "planet.gas",        "planet.ice",
                              "planet.lava",       "planet.oceanic",
                              "station.trade_hub", "station.mining_outpost",
                              "station.shipyard",  "station.military",
                              "ship.player",       "ship.npc" };
        for (const char* id : ids)
        {
            INFO("archetype id: ", id);
            CHECK(Archetypes::Find(id) != nullptr);
        }
    }
}
