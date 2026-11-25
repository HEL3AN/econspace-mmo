#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Simulation.h"

#include "goods_named.h"
#include "temp_dir.h"

#include <fstream>
#include <iterator>

#include <memory>
#include <regex>
#include <string>

// A coarse pass spread over ticks (#350): the same work in the same order, so a spread world
// is the unspread one once the pass is done.

namespace
{
std::unique_ptr<Simulation> World(int spread, bool hot)
{
    SetRandomSeed(0xC0FFEEu);
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->SetAllHot(hot);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    sim->SetPassSpread(spread);
    const std::string& start = sim->Universe().startId;
    sim->CreateSession(start, sim->SafeArrival(start), GetShipCatalog()[0].stats);
    return sim;
}

// What the world would write down now.
std::string Saved(const Simulation& sim, const TempDir& tmp, const char* name)
{
    const std::string path = tmp.Path(name);
    REQUIRE(sim.SaveWorld(path));
    std::ifstream in(path, std::ios::binary);
    std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // When a system was last looked at is the time of the tail of the pass: a few ticks later
    // where the pass was spread, and that is the one thing that is meant to differ.
    return std::regex_replace(text, std::regex("\"seenAt\": [^,\n]*"), "\"seenAt\": 0");
}
}  // namespace

TEST_CASE("a spread pass leaves the world as the whole pass does (#350)")
{
    const std::string data = TEST_DATA_DIR;
    REQUIRE(Factions::Load(data + "factions.json"));
    REQUIRE(Archetypes::Load(data + "archetypes.json"));
    REQUIRE(Blueprints::Load(data + "blueprints.json"));
    REQUIRE(Goods::Load(data + "goods.json"));

    for (const bool hot : { false, true })
    {
        auto whole = World(0, hot);
        auto spread = World(8, hot);
        REQUIRE(whole->Systems().size() > 1);
        const float dt = 1.0f / 60.0f;
        // Whole passes, and a little more than the last one needs to be spread over.
        for (int i = 0; i < 120 * 6 + 40; i++)
        {
            whole->MaintainWorld(dt);
            spread->MaintainWorld(dt);
        }
        const TempDir     tmp;
        const std::string a = Saved(*whole, tmp, "whole.json");
        CHECK(a.size() > 1000);
        CHECK(Saved(*spread, tmp, "spread.json") == a);
    }
}

TEST_CASE("a spread pass puts no more than a slice of it in one tick (#350)")
{
    const std::string data = TEST_DATA_DIR;
    REQUIRE(Factions::Load(data + "factions.json"));
    REQUIRE(Archetypes::Load(data + "archetypes.json"));
    REQUIRE(Blueprints::Load(data + "blueprints.json"));
    REQUIRE(Goods::Load(data + "goods.json"));
    auto                     sim = World(8, false);
    Simulation::MaintainCost cost;
    int                      busy = 0;
    for (int i = 0; i < 120 * 3; i++)
    {
        const int before = cost.passes;
        sim->MaintainWorld(1.0f / 60.0f, &cost);
        busy += cost.passes != before ? 1 : 0;
    }
    CHECK(cost.passes == 3);  // started, once each
    CHECK(busy == 3);
}
