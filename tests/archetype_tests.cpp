#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "entities/Station.h"
#include "entities/AsteroidField.h"
#include "entities/Nebula.h"
#include "entities/JumpGate.h"
#include "entities/Planet.h"
#include "entities/Star.h"
#include "economy/Resource.h"
#include <cstdio>
#include <fstream>
#include <string>

// The registry is what makes a player-built object possible (#44) and what lets an agent
// reason about an object it was never told about (#42). Both of those depend on the
// shipped data/archetypes.json being loadable and complete, not just on the parser
// compiling — so these tests read the real file rather than a fixture.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

// Writes a throwaway archetype file and loads it. Returns Load()'s verdict. The file goes
// to the working directory (the build tree under ctest), never into data/ — a crashed
// test must not leave a bogus archetype where the game would read it.
bool LoadInline(const std::string& body)
{
    const std::string path = "archetypes_test_tmp.json";
    {
        std::ofstream f(path);
        f << body;
    }
    bool ok = Archetypes::Load(path);
    std::remove(path.c_str());
    return ok;
}
}  // namespace

TEST_CASE("the shipped registry loads and covers every kind the world contains")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    CHECK(Archetypes::Error().empty());
    CHECK(Archetypes::All().size() > 0);

    // Every kind that data/systems/*.json can produce needs somewhere to look itself up.
    // A kind with no archetype is an object that cannot be asked what it can do.
    for (EntityKind k :
         { EntityKind::Star, EntityKind::Planet, EntityKind::Station, EntityKind::Field,
           EntityKind::Gate, EntityKind::Nebula, EntityKind::Derelict })
        CHECK(Archetypes::OfKind(k).size() > 0);

    SUBCASE("ids are how world data refers to an archetype")
    {
        const Archetype* hub = Archetypes::Find("station.trade_hub");
        REQUIRE(hub != nullptr);
        CHECK(hub->kind == EntityKind::Station);
        CHECK(hub->name == "Trade Hub");
        CHECK(hub->Has(Component::Dockable));
        CHECK(hub->Has(Component::Market));
        CHECK(hub->dockRange > 0.0f);
    }

    SUBCASE("an unknown id is a null, not a substituted default")
    {
        CHECK(Archetypes::Find("station.does_not_exist") == nullptr);
    }

    SUBCASE("components can be queried across the registry")
    {
        // This is the query shape the simulation passes use.
        CHECK(Archetypes::With(Component::Dockable).size() >= 4);  // the four station roles
        CHECK(Archetypes::With(Component::Mineable).size() >= 1);
        CHECK(Archetypes::With(Component::JumpLink).size() >= 1);

        // A component that says what can be done but not from how far is a component the
        // simulation cannot act on: the pass would compare against zero and never match.
        for (const Archetype* a : Archetypes::With(Component::Dockable))
            CHECK(a->dockRange > 0.0f);
        for (const Archetype* a : Archetypes::With(Component::Mineable))
        {
            CHECK(a->extractRange > 0.0f);
            CHECK(a->extractRate > 0.0f);
        }
        for (const Archetype* a : Archetypes::With(Component::Salvageable))
            CHECK(a->salvageRange > 0.0f);
        for (const Archetype* a : Archetypes::With(Component::JumpLink))
            CHECK(a->jumpRange > 0.0f);
    }

    SUBCASE("every archetype has a glyph, which is what the primary renderer needs")
    {
        // Glyphs are the game's look (#36), not a fallback. An archetype without one is
        // an object that cannot be drawn at all, so this is a completeness check rather
        // than a style preference.
        for (const Archetype& a : Archetypes::All())
        {
            CHECK_FALSE(a.visual.glyph.empty());
            CHECK(a.visual.color.a > 0);  // fully transparent means invisible
        }
    }

    SUBCASE("the glyph grammar matches what the objects actually are")
    {
        // An area drawn as one character scaled to fit would put a 3000-unit `~` over
        // everything inside it, so regions are marked as such in data rather than being
        // special-cased by the backend.
        REQUIRE(Archetypes::Find("nebula.cloud") != nullptr);
        CHECK(Archetypes::Find("nebula.cloud")->visual.style == GlyphStyle::Region);
        CHECK(Archetypes::Find("field.asteroid")->visual.style == GlyphStyle::Region);

        // Ships turn; scenery does not.
        CHECK(Archetypes::Find("ship.player")->visual.style == GlyphStyle::Directional);
        CHECK(Archetypes::Find("ship.npc")->visual.style == GlyphStyle::Directional);
        CHECK(Archetypes::Find("station.trade_hub")->visual.style == GlyphStyle::Point);
        CHECK(Archetypes::Find("star.yellow")->visual.style == GlyphStyle::Point);
    }
}

TEST_CASE("entities pick up their archetype on construction")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));

    Station hub({ 0.0f, 0.0f }, 90.0f, "Aurora Hub", FactionId::TradersGuild,
                StationRole::TradeHub);
    CHECK(hub.GetArchetype() != nullptr);
    CHECK(hub.Has(Component::Dockable));
    CHECK(hub.Has(Component::Market));
    CHECK_FALSE(hub.Has(Component::Mineable));

    // A military station is the same class and a different archetype — which is the whole
    // point: what it can do comes from data, not from its type.
    Station fort({ 0.0f, 0.0f }, 80.0f, "Vyse Bastion", FactionId::Independent,
                 StationRole::Military);
    CHECK(fort.Has(Component::Defensive));
    CHECK_FALSE(hub.Has(Component::Defensive));

    AsteroidField belt({ 0.0f, 0.0f }, 500.0f, "Inner Iron Belt", AllResourceTypes()[0], 300);
    CHECK(belt.Has(Component::Mineable));
    CHECK_FALSE(belt.Has(Component::Dockable));

    Nebula veil({ 0.0f, 0.0f }, 3000.0f, "Veil Nebula");
    REQUIRE(veil.GetArchetype() != nullptr);
    CHECK(veil.Has(Component::Hazard));
    CHECK(veil.GetArchetype()->hazardHidesShips);  // the server's cover rule reads this

    JumpGate gate({ 0.0f, 0.0f }, 150.0f, "Gate to Sigma Reach", "reach");
    CHECK(gate.Has(Component::JumpLink));

    Star sun({ 0.0f, 0.0f }, 600.0f, StarType::Yellow);
    REQUIRE(sun.GetArchetype() != nullptr);
    CHECK(sun.GetArchetype()->id == "star.yellow");
}

TEST_CASE("placeable archetypes carry enough to write the world file")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));

    // The editor palette is generated from this set (#37). A regression here is not a
    // crash — it is an object type quietly disappearing from the editor, or one that
    // places but writes JSON the loader reads back as something else.
    int placeable = 0;
    for (const Archetype& a : Archetypes::All())
    {
        if (!a.Placeable())
            continue;
        placeable++;
        CHECK(a.defaultSize > 0.0f);  // an object placed at size 0 is unselectable

        // The category has to be one the loader actually reads.
        const std::string& c = a.worldCategory;
        CHECK((c == "planets" || c == "stations" || c == "asteroidFields" || c == "nebulae" ||
               c == "derelicts" || c == "gates"));

        // Categories with a type key need the value; the others must not invent one.
        if (c == "planets")
            CHECK(PlanetTypeName(PlanetTypeFromString(a.worldSubType)) != "Planet");
        else if (c == "stations")
            CHECK(StationRoleName(StationRoleFromString(a.worldSubType)) != "Station");
        else
            CHECK(a.worldSubType.empty());
    }
    CHECK(placeable >= 12);

    SUBCASE("a subtype survives the round trip through the world file's spelling")
    {
        // The editor writes worldSubType into the JSON and the loader parses it back.
        // If those two disagree, every military station placed becomes a trade hub.
        const Archetype* mil = Archetypes::Find("station.military");
        REQUIRE(mil != nullptr);
        CHECK(StationRoleFromString(mil->worldSubType) == StationRole::Military);

        const Archetype* ice = Archetypes::Find("planet.ice");
        REQUIRE(ice != nullptr);
        CHECK(PlanetTypeFromString(ice->worldSubType) == PlanetType::Ice);
    }

    SUBCASE("things that are not placed by hand say so")
    {
        // A star is one per system and a ship is not scenery. Both would otherwise show
        // up in the palette and write a JSON category nothing reads.
        for (const Archetype* a : Archetypes::OfKind(EntityKind::Star))
            CHECK_FALSE(a->Placeable());
        for (const Archetype* a : Archetypes::OfKind(EntityKind::PlayerShip))
            CHECK_FALSE(a->Placeable());
        for (const Archetype* a : Archetypes::OfKind(EntityKind::Npc))
            CHECK_FALSE(a->Placeable());
    }
}

TEST_CASE("a broken registry fails loudly instead of loading half a world")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const size_t before = Archetypes::All().size();

    SUBCASE("a missing file is reported, not silently empty")
    {
        CHECK_FALSE(Archetypes::Load(DataFile("no_such_archetypes.json")));
        CHECK_FALSE(Archetypes::Error().empty());
    }

    SUBCASE("malformed JSON")
    {
        CHECK_FALSE(LoadInline("{ not json"));
    }

    SUBCASE("an entry without an id")
    {
        CHECK_FALSE(LoadInline(R"({"archetypes":[{"kind":"Station"}]})"));
    }

    SUBCASE("an unknown kind")
    {
        CHECK_FALSE(LoadInline(R"({"archetypes":[{"id":"x","kind":"Wormhole"}]})"));
    }

    SUBCASE("an unknown glyph style")
    {
        CHECK_FALSE(
            LoadInline(R"({"archetypes":[{"id":"x","kind":"Station","style":"sparkle"}]})"));
    }

    SUBCASE("an unknown component — a typo would otherwise be an object that does nothing")
    {
        CHECK_FALSE(LoadInline(
            R"({"archetypes":[{"id":"x","kind":"Station","components":{"dockible":{}}}]})"));
    }

    SUBCASE("a duplicate id, where Find() would otherwise pick one of them arbitrarily")
    {
        CHECK_FALSE(LoadInline(R"({"archetypes":[{"id":"x","kind":"Station"},
                                                 {"id":"x","kind":"Field"}]})"));
    }

    // Whatever went wrong above, the previous registry is still intact: a failed reload
    // must not leave the process with a world it cannot describe.
    CHECK(Archetypes::All().size() == before);
    CHECK(Archetypes::Find("station.trade_hub") != nullptr);
}
