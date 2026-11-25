#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Market.h"
#include "economy/Resource.h"
#include "entities/AsteroidField.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

// Fields with a life (#617, documents/mining_and_fields.md, slice 1): a cap that follows the
// field's age, a yield that follows its density, ore that leaves it unmined as a sink, a crew
// whose rate is the miner design's, and an end that every system sees on the same clock.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void StartServer(Simulation& sim, const std::string& worldPath = std::string())
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Goods::Load(DATA + "goods.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.SetAllHot(false);
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

// A system with a market and a crew of miners: where an NPC crew has a shelf to fill.
std::string TradingSystem(Simulation& sim)
{
    for (auto& kv : sim.Systems())
        if (kv.second.profile.markets > 0 && kv.second.agg.miners >= 1.0f)
            return kv.first;
    return std::string();
}

AsteroidField* FieldIn(SystemState& st, int id)
{
    for (auto& e : st.entities)
        if (e->GetId() == id)
            return static_cast<AsteroidField*>(e.get());
    return nullptr;
}
}  // namespace

TEST_CASE("a field's cap rises, holds, thins and ends with its age (#617)")
{
    const FieldCurve c;  // the shipped shares: 0.10, 0.40, 0.35, 0.80, 0.25
    CHECK(c.Cap(-0.1) == 0.0);
    CHECK(c.Cap(0.0) == doctest::Approx(0.4));
    CHECK(c.Cap(0.05) == doctest::Approx(0.7));
    CHECK(c.Cap(0.10) == doctest::Approx(1.0));
    CHECK(c.Cap(0.30) == doctest::Approx(1.0));
    CHECK(c.Cap(0.575) == doctest::Approx(0.625));  // half way down to a quarter
    CHECK(c.Cap(0.80) == doctest::Approx(0.25));
    CHECK(c.Cap(0.90) == doctest::Approx(0.125));
    CHECK(c.Cap(1.0) == 0.0);
}

TEST_CASE("what a field holds is its cap less what was taken, and the rest disperses (#617)")
{
    REQUIRE(Goods::Load(DATA + "goods.json"));
    AsteroidField f({ 0.0f, 0.0f }, 500.0f, "Loose Iron", Good::Iron(), 1000);
    f.SetLife(1000.0, 10000.0, FieldCurve());
    CHECK(f.HasLife());
    CHECK(f.EndsAt() == doctest::Approx(11000.0));

    // Not yet born: nothing to take. At a fifth of its life it is full.
    CHECK(f.OreAt(900.0) == 0.0);
    f.Age(3000.0);
    CHECK(f.GetOre() == doctest::Approx(1000.0));

    // What is taken is taken for good: a field does not grow back.
    CHECK(f.Take(300.0) == doctest::Approx(300.0));
    f.Regrow(0.5, 100.0);
    CHECK(f.GetOre() == doctest::Approx(700.0));
    CHECK(f.Taken() == doctest::Approx(300.0));

    // Thinning: at 0.575 of its life the cap fell from 1000 to 625: 375 left it unmined.
    const double lost = f.Age(1000.0 + 0.575 * 10000.0);
    CHECK(lost == doctest::Approx(375.0));
    CHECK(f.GetOre() == doctest::Approx(325.0));  // 625 - 300
    // Mined below the cap, the cap's fall takes nothing more than there is.
    CHECK(f.Take(325.0) == doctest::Approx(325.0));
    CHECK(f.Age(1000.0 + 0.9 * 10000.0) == doctest::Approx(0.0));
    CHECK(f.GetOre() == 0.0);

    // Slower as it thins, never slower than the floor.
    AsteroidField g({ 0.0f, 0.0f }, 500.0f, "Loose Iron", Good::Iron(), 1000);
    g.SetLife(0.0, 10000.0, FieldCurve());
    g.Age(2000.0);
    CHECK(g.Density(2000.0, 0.3) == doctest::Approx(1.0));
    g.Take(500.0);
    g.Age(2000.0);
    CHECK(g.Density(2000.0, 0.3) == doctest::Approx(0.65));
    g.Take(500.0);
    g.Age(2000.0);
    CHECK(g.Density(2000.0, 0.3) == doctest::Approx(0.3));
}

TEST_CASE("a field of a world file is as it was described, taken and all (#617)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("world.json");
    std::string       system;
    int               id = 0;
    double            taken = 0.0;
    {
        Simulation sim;
        StartServer(sim);
        system = TradingSystem(sim);
        REQUIRE_FALSE(system.empty());
        id = sim.PlaceField(system, Good::Iron(), 2000, 24.0, 600.0, { 4000.0f, 0.0f });
        REQUIRE(id != 0);
        // A delta tells the clients before any snapshot, and the field says when it is gone.
        bool told = false;
        for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
            for (const Proto::EntityLayout& el : d.added)
                told = told || (el.id == id && el.expiresAt > sim.Time());
        CHECK(told);
        AsteroidField* f = FieldIn(*sim.SystemById(system), id);
        REQUIRE(f != nullptr);
        for (int i = 0; i < 1800; i++)  // an hour
            sim.MaintainWorld(2.0f);
        f->Take(100.0);
        taken = f->Taken();
        REQUIRE(sim.SaveWorld(path));
    }
    Simulation again;
    StartServer(again, path);
    const SystemState* st = again.SystemById(system);
    REQUIRE(st != nullptr);
    const AsteroidField* back = nullptr;
    for (const auto& e : st->entities)
        if (e->GetKind() == EntityKind::Field && static_cast<const AsteroidField&>(*e).HasLife())
            back = static_cast<const AsteroidField*>(e.get());
    REQUIRE(back != nullptr);
    CHECK(back->Taken() == doctest::Approx(taken).epsilon(1e-3));
    CHECK(back->Life() == doctest::Approx(24.0 * 3600.0));
    CHECK(back->GetOreMax() == 2000);
    CHECK(back->Curve() == WorldRules::FieldRules().curve);
}

TEST_CASE("the crew mines a field at its design's rate, never past its ore, and it is gone at its "
          "end (#617)")
{
    Simulation sim;
    StartServer(sim);
    const std::string system = TradingSystem(sim);
    REQUIRE_FALSE(system.empty());
    SystemState&       st = *sim.SystemById(system);
    const ResourceType iron = Good::Iron();
    const int          id = sim.PlaceField(system, iron, 20000, 10.0, 600.0, { 4000.0f, 0.0f });
    REQUIRE(id != 0);
    AsteroidField* f = FieldIn(st, id);
    REQUIRE(f != nullptr);

    // An hour of the crew: some, and no more than the design can carry in an hour.
    const Ships::Miner m =
        Ships::RoleMiner(Archetypes::ShipCatalogue(), Factions::Id(Factions::Unaffiliated()));
    REQUIRE(m.rate > 0.0f);
    st.market.SetStock(iron, 0.0);
    for (int i = 0; i < 1800; i++)
        sim.MaintainWorld(2.0f);
    const double mined = st.market.FlowOf(iron).mined;
    CHECK(mined > 0.0);
    CHECK(mined <=
          WorldRules::FieldRules().crew * m.rate * f->GetArchetype()->extractRate * 3600.0 * 1.01);
    CHECK(f->Taken() > 0.0);

    // Over its life it thins: ore left unmined is counted as dispersed, and then it is gone.
    for (int i = 0; i < 18000 + 10; i++)  // another ten hours: past its end
        sim.MaintainWorld(2.0f);
    CHECK(FieldIn(st, id) == nullptr);
    CHECK(st.market.FlowOf(iron).dispersed > 0.0);
    bool told = false;
    for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
        told = told || std::find(d.removed.begin(), d.removed.end(), id) != d.removed.end();
    CHECK(told);
}

TEST_CASE("a full shelf is not mined past its cap (#617)")
{
    Simulation sim;
    StartServer(sim);
    const std::string system = TradingSystem(sim);
    REQUIRE_FALSE(system.empty());
    SystemState&       st = *sim.SystemById(system);
    const ResourceType iron = Good::Iron();
    const int          id = sim.PlaceField(system, iron, 20000, 10.0, 600.0, { 4000.0f, 0.0f });
    REQUIRE(id != 0);
    st.market.SetStock(iron, st.market.Target(iron) * 100.0);  // clipped to the cap by the step
    for (int i = 0; i < 900; i++)
        sim.MaintainWorld(2.0f);
    CHECK(FieldIn(st, id)->Taken() == doctest::Approx(0.0));
}
