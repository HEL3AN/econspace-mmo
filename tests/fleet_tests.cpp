#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <string>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Simulation.h"
#include "factions_named.h"
#include "temp_dir.h"
#include <nlohmann/json.hpp>

// Fleets (#464, slice 2): every side in a system keeps which designs it flies, and every fight
// and the spawn director read that rather than the doctrine.

namespace
{
const std::string DATA = TEST_DATA_DIR;

std::unique_ptr<Simulation> FleetWorld()
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    return sim;
}

float Sum(const std::map<std::string, float>& fleet)
{
    float n = 0.0f;
    for (const auto& kv : fleet)
        n += kv.second;
    return n;
}

void Strip(nlohmann::json& j, const char* key)
{
    if (j.is_object())
    {
        j.erase(key);
        for (auto& kv : j.items())
            Strip(kv.value(), key);
    }
    else if (j.is_array())
        for (auto& e : j)
            Strip(e, key);
}
}  // namespace

TEST_CASE("fleets are seeded from the doctrines and kept to the numbers they describe (#464)")
{
    auto sim = FleetWorld();
    sim->MaintainWorld(2.0f);  // one coarse pass
    int fleets = 0;
    for (const auto& kv : sim->Systems())
    {
        const SystemAggregate& a = kv.second.agg;
        CAPTURE(kv.first);
        for (const auto& [key, fleet] : a.fleets)
        {
            fleets++;
            CHECK(Sum(fleet) > 0.0f);
        }
        auto has = [&](FactionId f, NpcRole r, float n)
        {
            const auto it = a.fleets.find(Simulation::FleetKey(f, r));
            if (n <= 1e-3f)
                return it == a.fleets.end();
            return it != a.fleets.end() && std::fabs(Sum(it->second) - n) < 1e-3f;
        };
        CHECK(has(Fac::Pirates(), NpcRole::Pirate, a.pirates));
        CHECK(has(a.policeFaction, NpcRole::Police, a.police));
        CHECK(has(Factions::Unaffiliated(), NpcRole::Miner, a.miners));
        for (FactionId f : Factions::All())
            CHECK(has(f, NpcRole::Warship, a.presence[f]));
        // Seeded from the doctrine, a fleet fights as the doctrine does: the same numbers as
        // before fleets.
        for (NpcRole r : { NpcRole::Pirate, NpcRole::Police, NpcRole::Warship })
            for (FactionId f : Factions::All())
            {
                const Ships::Fighter in = sim->FighterIn(kv.second, f, r);
                const Ships::Fighter of = sim->FighterOf(f, r);
                CHECK(in.dps == doctest::Approx(of.dps));
                CHECK(in.hull == doctest::Approx(of.hull));
            }
    }
    CHECK(fleets > 0);
}

TEST_CASE("a fleet's mix is what its side fights with and what the director sends (#464)")
{
    auto              sim = FleetWorld();
    SystemState&      st = *sim->SystemById(sim->Universe().startId);
    SystemAggregate&  a = st.agg;
    const std::string key = Simulation::FleetKey(Fac::Pirates(), NpcRole::Pirate);
    a.pirates = 8.0f;
    a.fleets[key] = { { "raider", 1.0f }, { "raider~laser", 3.0f } };
    sim->ReconcileFleets(st);
    REQUIRE(a.fleets.count(key) == 1);
    CHECK(a.fleets[key]["raider"] == doctest::Approx(2.0f));
    CHECK(a.fleets[key]["raider~laser"] == doctest::Approx(6.0f));

    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    const Ships::Fighter    rail = Ships::DesignFighter(c, "raider");
    const Ships::Fighter    laser = Ships::DesignFighter(c, "raider~laser");
    REQUIRE(rail.dps != doctest::Approx(laser.dps));
    const Ships::Fighter mix = sim->FighterIn(st, Fac::Pirates(), NpcRole::Pirate);
    CHECK(mix.dps == doctest::Approx((2.0f * rail.dps + 6.0f * laser.dps) / 8.0f));
    CHECK(mix.hull == doctest::Approx((2.0f * rail.hull + 6.0f * laser.hull) / 8.0f));

    // The director draws by count, and by key: the same key, the same design.
    int lasers = 0;
    for (unsigned k = 0; k < 1000; k++)
    {
        const std::string d = sim->FleetDesign(st, Fac::Pirates(), NpcRole::Pirate, k);
        CHECK((d == "raider" || d == "raider~laser"));
        lasers += d == "raider~laser" ? 1 : 0;
        CHECK(d == sim->FleetDesign(st, Fac::Pirates(), NpcRole::Pirate, k));
    }
    CHECK(lasers > 720);
    CHECK(lasers < 780);

    // A loss the arithmetic does not know the design of takes from every design alike.
    a.pirates = 4.0f;
    sim->ReconcileFleets(st);
    CHECK(a.fleets[key]["raider"] == doctest::Approx(1.0f));
    CHECK(a.fleets[key]["raider~laser"] == doctest::Approx(3.0f));

    // A design this catalogue no longer has is the role's stock design now, counted.
    a.fleets[key] = { { "raider~plasma", 4.0f } };
    sim->ReconcileFleets(st);
    CHECK(a.fleets[key].size() == 1);
    CHECK(a.fleets[key]["raider"] == doctest::Approx(4.0f));

    // No ships, no fleet; and a side with none flies its doctrine's pick.
    a.pirates = 0.0f;
    sim->ReconcileFleets(st);
    CHECK(a.fleets.count(key) == 0);
    CHECK(sim->FleetDesign(st, Fac::Pirates(), NpcRole::Pirate, 3) ==
          c.Pick(Factions::Id(Fac::Pirates()), "pirate", 3));
}

TEST_CASE("where somebody is, a ship shot down comes off its own design's count (#464)")
{
    auto              sim = FleetWorld();
    const std::string start = sim->Universe().startId;
    sim->CreateSession(start, sim->SafeArrival(start), GetShipCatalog()[0].stats);
    SystemState& st = *sim->SystemById(start);
    REQUIRE(Simulation::IsHot(st));
    NpcShip* victim = nullptr;
    for (auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc && victim == nullptr)
        {
            NpcShip* n = static_cast<NpcShip*>(e.get());
            if (n->GetTask() == 0 && !n->IsLeaving() && n->GetRole() != NpcRole::Warship &&
                n->GetRole() != NpcRole::Guard)
                victim = n;
        }
    REQUIRE(victim != nullptr);
    const std::string key = Simulation::FleetKey(victim->GetFaction(), victim->GetRole());
    sim->RecountFleets(st);
    REQUIRE(st.agg.fleets.count(key) == 1);
    const float before = st.agg.fleets[key][victim->GetDesign()];
    CHECK(before >= 1.0f);
    victim->TakeDamage(1e9f);
    sim->RecountFleets(st);
    const float after =
        st.agg.fleets.count(key) == 0 ? 0.0f : st.agg.fleets[key][victim->GetDesign()];
    CHECK(after == doctest::Approx(before - 1.0f));
}

TEST_CASE("fleets survive a restart, and a world saved without them is given the doctrine's")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("fleet_world.json");
    auto              a = FleetWorld();
    a->MaintainWorld(2.0f);
    SystemState&      st = *a->SystemById(a->Universe().startId);
    const std::string key = Simulation::FleetKey(Fac::Pirates(), NpcRole::Pirate);
    st.agg.pirates = 4.0f;
    st.agg.fleets[key] = { { "raider", 1.0f }, { "raider~laser~heavy", 3.0f } };
    a->SaveWorld(path);

    {
        Simulation b;
        b.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        b.AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
        REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
        b.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        // A fleet that is its doctrine's mix is not written: the first pass seeds it again.
        int written = 0;
        for (const auto& kv : a->Systems())
            for (const auto& [k, fleet] : kv.second.agg.fleets)
            {
                const auto& back = b.SystemById(kv.first)->agg.fleets;
                if (Simulation::DoctrineMix(k, fleet))
                    CHECK(back.count(k) == 0);
                else
                {
                    written++;
                    REQUIRE(back.count(k) == 1);
                    CHECK(back.at(k) == fleet);
                }
            }
        CHECK(written == 1);  // the start system's pirates, three in four lasers on heavy hulls
    }

    // An older world has none: it loads, and the first pass gives each side its doctrine's.
    nlohmann::json j;
    {
        std::ifstream in(path);
        in >> j;
    }
    Strip(j, "fleets");
    {
        std::ofstream out(path);
        out << j.dump();
    }
    Simulation c;
    c.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    c.AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    REQUIRE(c.LoadWorld(path) == Save::Result::Ok);
    c.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    SystemState& back = *c.SystemById(c.Universe().startId);
    CHECK(back.agg.fleets.empty());
    c.MaintainWorld(2.0f);
    REQUIRE(back.agg.pirates > 0.0f);
    REQUIRE(back.agg.fleets.count(key) == 1);
    CHECK(back.agg.fleets[key].count("raider") == 1);
    CHECK(Sum(back.agg.fleets[key]) == doctest::Approx(back.agg.pirates));
}
