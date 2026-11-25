#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/DataDir.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/AsteroidField.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/EventKinds.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

// The event engine (#617 slice 2): kinds read from data, an ore bank that bounds what a region
// may spend, births decided from the seed and the clock and kept in the save, an Impact that is
// a satellite of its planet, a Collision that moves a belt's ore, and a new region with a past.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadData()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Goods::Load(DATA + "goods.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    REQUIRE(FieldEvents::Load(DATA + "events.json"));
}

Gen::WorldParams Params(uint64_t seed)
{
    Gen::WorldParams p;
    p.seed = seed;
    p.centres = 1;
    p.owners = Factions::MajorIds();
    p.pirates = Factions::Id(Factions::Pirates());
    p.unaffiliated = Factions::Id(Factions::Unaffiliated());
    return p;
}

// A world as the server makes it: one centre and a frontier region beside it.
void MakeWorld(Simulation& sim, uint64_t seed, const std::string& seal = std::string(),
               const std::vector<Simulation::RegionOpening>& log = {})
{
    sim.SetSealDir(seal);
    REQUIRE(sim.OpenGalaxy(Params(seed), DATA, log));
    sim.Seed(0xC0FFEEu);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
}

using Row = std::tuple<std::string, std::string, int, long long>;

// Every field with a life in the world: where, of what, how much it was made with and when it
// was born (rounded), in a fixed order.
std::vector<Row> Lived(const Simulation& sim)
{
    std::vector<Row> out;
    for (const auto& kv : sim.Systems())
        for (const auto& e : kv.second.entities)
            if (e->GetKind() == EntityKind::Field &&
                static_cast<const AsteroidField&>(*e).HasLife())
            {
                const AsteroidField& f = static_cast<const AsteroidField&>(*e);
                out.emplace_back(kv.first, Goods::Id(f.GetResource()), f.GetOreMax(),
                                 (long long)(f.BornAt() * 10.0));
            }
    std::sort(out.begin(), out.end());
    return out;
}

void Run(Simulation& sim, double seconds)
{
    for (double t = 0.0; t < seconds; t += 2.0)
        sim.MaintainWorld(2.0f);
}
}  // namespace

TEST_CASE("events.json is read strictly (#617)")
{
    LoadData();
    CHECK(FieldEvents::Kinds().size() >= 2);
    REQUIRE(FieldEvents::Find("impact") != nullptr);
    CHECK(FieldEvents::Find("impact")->needs == "planet");
    REQUIRE(FieldEvents::Find("collision") != nullptr);
    CHECK(FieldEvents::Find("collision")->moves > 0.0);

    const TempDir     tmp;
    const std::string before = FieldEvents::Hash();
    auto              write = [&](const std::string& text)
    {
        const std::string path = tmp.Path("events.json");
        std::ofstream(path) << text;
        return path;
    };
    // A misspelling is not an absence.
    CHECK_FALSE(
        FieldEvents::Load(write(R"({"kinds":[{"id":"x","needs":"belt","moves":0.1,"hoursz":[1,2],
        "archetypes":{"public":"field.asteroid"}}]})")));
    CHECK(FieldEvents::Error().find("hoursz") != std::string::npos);
    // An archetype that is not a field is no way to leave one.
    CHECK_FALSE(FieldEvents::Load(write(R"({"kinds":[{"id":"x","needs":"belt","moves":0.1,
        "archetypes":{"public":"station.trade_hub"}}]})")));
    CHECK_FALSE(FieldEvents::Load(write(R"({"kinds":[{"id":"x","needs":"sky","moves":0.1}]})")));
    CHECK_FALSE(FieldEvents::Load(write(R"({"kinds":[{"id":"x","needs":"planet","ores":{"Iron":1},
        "hours":[5,2],"archetypes":{"public":"field.asteroid"}}]})")));
    CHECK(FieldEvents::Hash() == before);  // nothing changed by a file that did not read
}

TEST_CASE("the same seed and clock make the same fields, and nobody needs to be there (#617)")
{
    LoadData();
    std::vector<Row> a, b;
    {
        Simulation sim;
        MakeWorld(sim, 3);
        Run(sim, 6.0 * 3600.0);
        a = Lived(sim);
    }
    {
        Simulation sim;
        MakeWorld(sim, 3);
        Run(sim, 6.0 * 3600.0);
        b = Lived(sim);
    }
    CHECK_FALSE(a.empty());  // the pre-roll alone gives a region some
    CHECK(a == b);
}

TEST_CASE("a region has a past when it opens: fields at every stage of life (#617)")
{
    LoadData();
    Simulation sim;
    MakeWorld(sim, 5);
    Run(sim, 4.0);
    int born = 0, young = 0, old = 0;
    for (const auto& kv : sim.Systems())
        for (const auto& e : kv.second.entities)
            if (e->GetKind() == EntityKind::Field &&
                static_cast<const AsteroidField&>(*e).HasLife())
            {
                const AsteroidField& f = static_cast<const AsteroidField&>(*e);
                const double         age = (sim.Time() - f.BornAt()) / f.Life();
                born++;
                young += age < 0.3 ? 1 : 0;
                old += age >= 0.3 ? 1 : 0;
                CHECK(age >= 0.0);
                CHECK(age < 1.0);  // a field that is over is not a field
            }
    CHECK(born > 0);
    CHECK(young > 0);
    CHECK(old > 0);
}

TEST_CASE("a region never spends more ore than its bank earned (#617)")
{
    LoadData();
    Simulation sim;
    MakeWorld(sim, 7);
    Run(sim, 3.0 * 3600.0);
    const WorldRules::Events& r = WorldRules::EventRules();
    for (int slot : sim.OpenSlots())
        for (int k = 0; k < 3; k++)
        {
            const double bank = sim.FieldBank(slot, k);
            CHECK(bank >= 0.0);
            // Never more than bankHours of the flow it accrues by.
            CHECK(bank <= sim.FieldFlowOf(slot, k) * r.bankHours + 1e-6);
        }
}

TEST_CASE("an Impact is a satellite of a planet; a Collision moves a belt's ore (#617)")
{
    LoadData();
    Simulation sim;
    MakeWorld(sim, 9);
    Run(sim, 12.0 * 3600.0);
    int impacts = 0;
    for (const auto& kv : sim.Systems())
        for (const auto& e : kv.second.entities)
            if (e->GetKind() == EntityKind::Field &&
                static_cast<const AsteroidField&>(*e).HasLife())
                if (e->GetOrbit().has_value())
                    impacts++;
    CHECK(impacts > 0);

    // A collision on its own: the belt gives up exactly what the fragments hold.
    SystemState*   home = nullptr;
    AsteroidField* belt = nullptr;
    for (auto& kv : sim.Systems())
        for (auto& e : kv.second.entities)
            if (belt == nullptr && e->GetKind() == EntityKind::Field &&
                !static_cast<AsteroidField&>(*e).HasLife() && Simulation::SignatureOf(*e) <= 0.0f &&
                static_cast<AsteroidField&>(*e).GetOre() > 800.0)
            {
                home = &kv.second;
                belt = static_cast<AsteroidField*>(e.get());
            }
    REQUIRE(belt != nullptr);
    const double           before = belt->GetOre();
    Simulation::FieldBirth b;
    b.slot = Gen::SlotOf(home->id);
    b.kind = "collision";
    b.system = home->id;
    b.t0 = sim.Time();
    b.hours = 12.0;
    b.size = 500.0f;
    b.seed = 42;
    CHECK(sim.BirthField(b));
    double fragments = 0.0;
    for (const auto& e : home->entities)
        if (e->GetKind() == EntityKind::Field && static_cast<AsteroidField&>(*e).HasLife() &&
            static_cast<AsteroidField&>(*e).GetName() == FieldEvents::Find("collision")->field)
            fragments = static_cast<AsteroidField&>(*e).GetOreMax();
    CHECK(fragments > 0.0);
    CHECK(before - belt->GetOre() == doctest::Approx(fragments).epsilon(0.01));
}

TEST_CASE("the bank and the births are kept by the save (#617)")
{
    LoadData();
    TempDir                 dir;
    const std::string       seal = dir.MakeDir("regions");
    const std::string       path = dir.Path("world.json");
    std::vector<double>     bank;
    size_t                  births = 0;
    std::vector<Row>        lived;
    Simulation::WorldHeader h;
    {
        Simulation sim;
        MakeWorld(sim, 11, seal);
        Run(sim, 2.0 * 3600.0);
        for (int slot : sim.OpenSlots())
            for (int k = 0; k < 3; k++)
                bank.push_back(sim.FieldBank(slot, k));
        births = sim.FieldBirths().size();
        lived = Lived(sim);
        REQUIRE(sim.SaveWorld(path));
        REQUIRE(Simulation::ReadWorldHeader(path, h));
    }
    Simulation again;
    again.SetSealDir(seal);
    REQUIRE(again.OpenGalaxy(Params(11), DATA, h.log));
    REQUIRE(again.LoadWorld(path) == Save::Result::Ok);
    again.MaterializeAllSystems(DATA + "systems/");
    size_t i = 0;
    for (int slot : again.OpenSlots())
        for (int k = 0; k < 3; k++)
            CHECK(again.FieldBank(slot, k) == doctest::Approx(bank[i++]).epsilon(1e-9));
    CHECK(again.FieldBirths().size() == births);
    // What was alive is alive: the same fields, from the save's added objects.
    CHECK(Lived(again) == lived);
}

TEST_CASE("a small Impact is a signature: not in a pilot's snapshot until a scan finds it (#617)")
{
    LoadData();
    Simulation sim;
    MakeWorld(sim, 13);
    // A system with a planet, and a pilot in it.
    std::string system;
    for (const auto& kv : sim.Systems())
        for (const auto& e : kv.second.entities)
            if (system.empty() && e->GetKind() == EntityKind::Planet)
                system = kv.first;
    REQUIRE_FALSE(system.empty());
    Simulation::FieldBirth b;
    b.slot = Gen::SlotOf(system);
    b.kind = "impact";
    b.system = system;
    b.t0 = sim.Time();
    b.hours = 24.0;
    b.size = 600.0f;
    b.seed = 5;
    b.ore = "Iron";
    b.amount = (int)(WorldRules::EventRules().publicOre) - 100;  // small
    REQUIRE(sim.BirthField(b));
    int id = 0;
    for (const auto& e : sim.SystemById(system)->entities)
        if (e->GetKind() == EntityKind::Field && e->GetOrbit().has_value())
            id = e->GetId();
    REQUIRE(id != 0);
    ClientSession& s = sim.CreateSession(system, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    bool           seen = false;
    for (const Proto::EntitySnapshot& e : sim.BuildSnapshot(s, system).entities)
        seen = seen || e.id == id;
    CHECK_FALSE(seen);
    for (const Proto::EntityLayout& e : sim.BuildLayoutFor(s, system).entities)
        CHECK(e.id != id);
}

TEST_CASE(
    "an event is news and a pilot who is there has it in the journal; a past is silent (#617)")
{
    LoadData();
    Simulation sim;
    MakeWorld(sim, 15);
    Run(sim, 4.0);
    // The pre-roll made fields days old, and nobody was told of them.
    const bool quiet = std::none_of(sim.Chronicle().begin(), sim.Chronicle().end(),
                                    [](const ChronicleEntry& e) { return e.kind == "field"; });
    CHECK(quiet);
    std::string system;
    for (const auto& kv : sim.Systems())
        for (const auto& e : kv.second.entities)
            if (system.empty() && e->GetKind() == EntityKind::Planet)
                system = kv.first;
    REQUIRE_FALSE(system.empty());
    ClientSession& s = sim.CreateSession(system, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    Simulation::FieldBirth b;
    std::string            why;
    REQUIRE(sim.BirthFieldNow("impact", system, b, why));
    CHECK(std::any_of(sim.Chronicle().begin(), sim.Chronicle().end(), [&](const ChronicleEntry& e)
                      { return e.kind == "field" && e.system == system; }));
    bool told = false;
    for (const Ev::Event& e : s.journal)
        told = told || e.text.find("stream of rock") != std::string::npos;
    CHECK(told);
}
