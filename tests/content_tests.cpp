#include <doctest/doctest.h>
#include "core/Archetype.h"
#include "core/Faction.h"
#include "entities/ShipType.h"

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
