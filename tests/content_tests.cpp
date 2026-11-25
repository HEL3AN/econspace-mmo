#include <doctest/doctest.h>
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/ShipType.h"
#include "render/Material.h"
#include "render/Modules.h"

#include <string>

// Content registries are instances a scope switches between (#356): two worlds in one process
// no longer share a hangar.
TEST_CASE("Two archetype registries are read one at a time, and a scope puts the first back")
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    const std::string path = std::string(TEST_DATA_DIR) + "archetypes.json";
    REQUIRE(Archetypes::Load(path));
    const size_t full = GetShipCatalog().size();
    const size_t archetypes = Archetypes::All().size();
    REQUIRE(full >= 2);
    const unsigned outer = Archetypes::Generation();

    Archetypes::Registry other;
    {
        Archetypes::Scope in(other);
        CHECK(Archetypes::All().empty());  // a registry of its own starts empty
        CHECK(GetShipCatalog().empty());
        REQUIRE(Archetypes::Load(path));
        CHECK(Archetypes::All().size() == archetypes);
        CHECK(GetShipCatalog().size() == full);
        CHECK(Archetypes::Generation() != outer);  // a cache built from the first is rebuilt

        // This world sells one hull fewer; the other is not told.
        other.ships.hangar.pop_back();
        other.generation++;
        CHECK(GetShipCatalog().size() == full - 1);
    }
    CHECK(GetShipCatalog().size() == full);
    CHECK(Archetypes::Generation() == outer);
    CHECK(Archetypes::All().size() == archetypes);
}

TEST_CASE("Two blueprint registries are read one at a time")
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string path = std::string(TEST_DATA_DIR) + "blueprints.json";
    REQUIRE(Blueprints::Load(path));
    const size_t all = Blueprints::All().size();
    REQUIRE(all > 0);

    Blueprints::Registry other;
    {
        Blueprints::Scope in(other);
        CHECK(Blueprints::All().empty());
        REQUIRE(Blueprints::Load(path));
        CHECK(Blueprints::All().size() == all);
        other.forPlayers.pop_back();  // this world lets players build one thing fewer
        CHECK(Blueprints::All().size() == all - 1);
    }
    CHECK(Blueprints::All().size() == all);
}

TEST_CASE("Two material and module registries are read one at a time")
{
    const std::string dir = std::string(TEST_DATA_DIR);
    REQUIRE(Archetypes::Load(dir + "archetypes.json"));  // loads the modules beside it
    REQUIRE(Render::Materials::Load(dir + "materials.json"));
    const size_t materials = Render::Materials::All().size();
    const size_t modules = Render::Modules::All().size();
    REQUIRE(materials > 0);

    Render::Materials::Registry otherMaterials;
    Render::Modules::Registry   otherModules;
    {
        Render::Materials::Scope m(otherMaterials);
        Render::Modules::Scope   d(otherModules);
        CHECK(Render::Materials::All().empty());
        CHECK(Render::Modules::All().empty());
        REQUIRE(Render::Materials::Load(dir + "materials.json"));
        CHECK(Render::Materials::All().size() == materials);
    }
    CHECK(Render::Materials::All().size() == materials);
    CHECK(Render::Modules::All().size() == modules);
}

TEST_CASE("Two goods registries and two faction registries are read one at a time")
{
    const std::string dir = std::string(TEST_DATA_DIR);
    REQUIRE(Goods::Load(dir + "goods.json"));
    REQUIRE(Factions::Load(dir + "factions.json"));
    const int goods = Goods::Count();
    const int factions = Factions::Count();
    REQUIRE(goods > 3);

    Goods::Content    otherGoods;
    Factions::Content otherFactions;
    {
        Goods::Scope    g(otherGoods);
        Factions::Scope f(otherFactions);
        CHECK(Goods::Count() == 3);  // a registry of its own starts as the builtin one
        CHECK(Factions::Count() == 4);
        FactionDef d;
        d.id = "ThisWorldOnly";
        d.name = "This world only";
        Factions::Add(d);
        CHECK(Factions::Count() == 5);
    }
    CHECK(Goods::Count() == goods);
    CHECK(Factions::Count() == factions);
    FactionId none;
    CHECK_FALSE(Factions::Find("ThisWorldOnly", none));
}
