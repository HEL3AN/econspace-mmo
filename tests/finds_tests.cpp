#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "gen/Galaxy.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "factions_named.h"

#include <string>

// What the scanner finds besides holes and hidden bases (#346): rich belts, hidden until a
// pilot's scans resolve them -- then minable like any belt, and a find a faction pays for.

namespace
{

const std::string SYSTEMS = std::string(TEST_WORLD_DIR) + "systems/";

// Known space and the region of seed 7 beyond its wormhole, as the chart-sale tests have it.
struct World
{
    WorldRules::Scan   scan = WorldRules::ScanRules();
    WorldRules::Charts charts = WorldRules::ChartRules();
    Simulation         sim;
    std::string        home;

    World()
    {
        WorldRules::SetScanRules(WorldRules::Scan{});
        WorldRules::SetChartRules(WorldRules::Charts{});
        REQUIRE(Factions::Load(std::string(TEST_DATA_DIR) + "factions.json"));
        REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
        REQUIRE(Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json"));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.AttachRegion(7, SYSTEMS);
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(SYSTEMS);
        home = sim.Universe().startId;
    }
    ~World()
    {
        WorldRules::SetScanRules(scan);
        WorldRules::SetChartRules(charts);
    }

    // The first object of `archetype` anywhere in the galaxy, and the system it is in.
    const Entity* Find(const std::string& archetype, std::string& system) const
    {
        for (const auto& si : sim.Universe().systems)
            if (const SystemState* st = sim.SystemById(si.id))
                for (const auto& e : st->entities)
                    if (e->GetArchetype() != nullptr && e->GetArchetype()->id == archetype)
                    {
                        system = si.id;
                        return e.get();
                    }
        return nullptr;
    }

    ClientSession& Pilot(const std::string& name)
    {
        ClientSession& s =
            sim.CreateSession(home, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = name;
        s.ship->SetPilotName(name);
        return s;
    }

    // One whole pass of the scanner, started now and run to its end.
    void Pass(ClientSession& s)
    {
        REQUIRE(sim.StartScan(s));
        sim.SetTime(sim.Time() + WorldRules::ScanRules().seconds + 0.1);
        sim.StepPlayerScan(s);
    }

    // Ten seconds at the belt with the mining laser on: how much came aboard.
    int Mine(ClientSession& s, const Entity& belt)
    {
        s.ship->SetPosition(belt.GetPosition());
        s.ship->SetMiningOn(true);
        int mined = 0;
        for (int i = 0; i < 600; i++)
            mined += sim.StepPlayerMining(s, *sim.SystemOf(s), 1.0f, 1.0f / 60.0f).minedUnits;
        return mined;
    }
};

bool InLayout(const Proto::SystemLayout& lay, int id)
{
    for (const Proto::EntityLayout& e : lay.entities)
        if (e.id == id)
            return true;
    return false;
}

}  // namespace

TEST_CASE("a rich belt is hidden until scanned, then mined like any belt and sold as a find "
          "(#346)")
{
    World         w;
    std::string   at;
    const Entity* belt = w.Find("field.rich", at);
    REQUIRE(belt != nullptr);
    const int          id = belt->GetId();
    const SystemState& st = *w.sim.SystemById(at);
    const std::string  key = w.sim.SignatureKey(st, *belt);
    CHECK(key.rfind(at + "/", 0) == 0);  // a save key: the find outlives a restart

    // Nobody's picture of the system has it: no faction counts it, and no crew works it.
    int fields = 0;
    for (const auto& e : st.entities)
        fields += e->GetKind() == EntityKind::Field ? 1 : 0;
    CHECK(Simulation::ProfileOf(st).belts == fields - 1);

    ClientSession& ann = w.Pilot("ann");
    w.sim.ServerEnterSystem(ann, at, ann.systemId);
    REQUIRE(ann.systemId == at);
    CHECK_FALSE(InLayout(w.sim.BuildLayoutFor(ann, at), id));
    CHECK(w.sim.ScanViewOf(ann).sigs.empty());
    // Flying into it blind mines nothing, and the belt's ore is not in her snapshot.
    CHECK(w.Mine(ann, *belt) == 0);
    for (const Proto::Snapshot::BeltOre& b : w.sim.BuildSnapshot(ann, at).belts)
        CHECK(b.id != id);

    // Strength 0.35: three passes.
    w.Pass(ann);
    w.Pass(ann);
    CHECK_FALSE(InLayout(w.sim.BuildLayoutFor(ann, at), id));
    w.Pass(ann);
    CHECK(InLayout(w.sim.BuildLayoutFor(ann, at), id));
    CHECK(ann.known.count(key) == 1);
    const Proto::ScanView v = w.sim.ScanViewOf(ann);
    REQUIRE(v.sigs.size() >= 1);
    bool shown = false;
    for (const Proto::SignatureView& g : v.sigs)
        shown |= g.id == id && g.kind == "rich belt";
    CHECK(shown);

    // Found: minable, and richer than an ordinary belt would be.
    CHECK(w.Mine(ann, *belt) > 0);

    // Another pilot has not found it.
    ClientSession& bob = w.Pilot("bob");
    w.sim.ServerEnterSystem(bob, at, bob.systemId);
    CHECK_FALSE(InLayout(w.sim.BuildLayoutFor(bob, at), id));

    // A faction that never heard of it pays for it as a hidden find.
    for (FactionId f : Factions::All())
        w.sim.MindOf(f).intel.erase(at);
    const Simulation::ChartOffer o = w.sim.ChartOfferFor(ann, Fac::TradersGuild(), at);
    CHECK(o.finds >= 1);
}

TEST_CASE("a derelict cache is hidden until scanned, then searched for its goods and sold as a "
          "find (#346)")
{
    World         w;
    std::string   at;
    const Entity* e = w.Find("derelict.cache", at);
    REQUIRE(e != nullptr);
    const int          id = e->GetId();
    Derelict&          cache = *const_cast<Derelict*>(static_cast<const Derelict*>(e));
    const SystemState& st = *w.sim.SystemById(at);
    const std::string  key = w.sim.SignatureKey(st, cache);
    const int          held = cache.GetCargoAmount();
    REQUIRE(held > 0);

    ClientSession& ann = w.Pilot("ann");
    w.sim.ServerEnterSystem(ann, at, ann.systemId);
    REQUIRE(ann.systemId == at);
    CHECK_FALSE(InLayout(w.sim.BuildLayoutFor(ann, at), id));

    // Standing on it, she cannot search what she has not found.
    ann.ship->SetPosition(cache.GetPosition());
    w.sim.StepPlayerLoot(ann, *w.sim.SystemOf(ann), id);
    CHECK(cache.GetCargoAmount() == held);
    CHECK(ann.ship->GetCargoUsed() == 0);

    // Strength 0.25: four passes.
    for (int i = 0; i < 3; i++)
        w.Pass(ann);
    CHECK_FALSE(InLayout(w.sim.BuildLayoutFor(ann, at), id));
    w.Pass(ann);
    CHECK(InLayout(w.sim.BuildLayoutFor(ann, at), id));
    CHECK(ann.known.count(key) == 1);

    // Found, it gives up what it holds (as much as the hold takes).
    w.sim.StepPlayerLoot(ann, *w.sim.SystemOf(ann), id);
    CHECK(ann.ship->GetCargoUsed() > 0);
    CHECK(cache.GetCargoAmount() < held);

    // Another pilot has not found it, and a faction pays for the find.
    ClientSession& bob = w.Pilot("bob");
    w.sim.ServerEnterSystem(bob, at, bob.systemId);
    CHECK_FALSE(InLayout(w.sim.BuildLayoutFor(bob, at), id));
    for (FactionId f : Factions::All())
        w.sim.MindOf(f).intel.erase(at);
    const Simulation::ChartOffer o = w.sim.ChartOfferFor(ann, Fac::TradersGuild(), at);
    CHECK(o.finds >= 1);
}

TEST_CASE("a probe module adds to a design's scan stat, and a pass multiplies by it (#346)")
{
    World                   w;
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    REQUIRE(c.FindModule("gate.probe") != nullptr);

    // The stock scout has none; a probe in place of its tractor gives half again.
    Ships::Design d = *c.FindDesign("scout");
    Ships::Stats  before, after;
    std::string   error;
    REQUIRE(Ships::Derive(c, d, before, error));
    CHECK(before.scan == doctest::Approx(0.0f));
    for (Ships::FitLine& l : d.fit)
        if (l.module == "weapons.tractor")
            l.module = "gate.probe";
    REQUIRE_MESSAGE(Ships::Validate(c, d, error), error);
    REQUIRE(Ships::Derive(c, d, after, error));
    CHECK(after.scan == doctest::Approx(0.5f));
    ShipStats flown;
    REQUIRE(PlayerStats(d, flown, error));
    CHECK(flown.scan == doctest::Approx(0.5f));

    // The price follows from its parts: a priced module has a cost.
    CHECK(c.FindModule("gate.probe")->cost.size() >= 1);

    // A signature of 0.35 (a rich belt, as a hidden base) takes three passes bare-handed and two
    // with one probe.
    std::string   at;
    const Entity* base = w.Find("field.rich", at);
    REQUIRE(base != nullptr);
    const std::string key = w.sim.SignatureKey(*w.sim.SystemById(at), *base);

    ClientSession& bare = w.Pilot("bare");
    w.sim.ServerEnterSystem(bare, at, bare.systemId);
    CHECK(w.sim.ScanViewOf(bare).power == doctest::Approx(1.0));
    w.Pass(bare);
    w.Pass(bare);
    CHECK(bare.known.count(key) == 0);
    w.Pass(bare);
    CHECK(bare.known.count(key) == 1);

    ClientSession& probed = w.sim.CreateSession(w.home, Vector2{ 0.0f, 0.0f }, flown);
    probed.accountName = "probed";
    probed.ship->SetPilotName("probed");
    w.sim.ServerEnterSystem(probed, at, probed.systemId);
    CHECK(w.sim.ScanViewOf(probed).power == doctest::Approx(1.5));
    w.Pass(probed);
    CHECK(probed.known.count(key) == 0);
    w.Pass(probed);
    CHECK(probed.known.count(key) == 1);
}
