#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Planet.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

// Construction, first slice (#39): a blueprint is data, a site is laid down by a player
// verb the server checks, it finishes at an instant of the world clock, it reaches everyone
// as a LayoutDelta, and it outlives a restart.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

// The registries once per test, before any world: an entity built before a reload points
// into the registry it replaced.
void LoadRegistries()
{
    Factions::Load(DataFile("factions.json"));
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
}

void StartServer(Simulation& sim, const std::string& worldPath)
{
    sim.LoadUniverse(DataFile("universe.json"));
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(DataFile("systems/"));
}

ClientSession& Builder(Simulation& sim, const std::string& account)
{
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = account;
    return s;
}

// Enough for any blueprint the data has, in a hold of any size the catalog has.
void Stock(ClientSession& s)
{
    s.ship->ClearCargo();
    REQUIRE(s.ship->AddCargo(ResourceType::Iron, 20));
    REQUIRE(s.ship->AddCargo(ResourceType::Crystal, 5));
}

// Somewhere in open space the blueprint may go, with the ship moved there. Found by asking
// the rule rather than by knowing the map, so a change to the data does not break the test.
Vector2 FreeSpot(Simulation& sim, ClientSession& s, const Blueprint& bp, float from = 0.0f)
{
    for (int i = 0; i < 60; i++)
        for (int j = 0; j < 60; j++)
        {
            const Vector2 p = { 120000.0f + from + 9100.0f * (float)i,
                                -270000.0f + 9300.0f * (float)j };
            s.ship->Teleport(p);
            if (sim.PlacementProblem(s, bp, p).empty())
                return p;
        }
    FAIL("no free spot in the start system");
    return {};
}

Structure* StructureById(Simulation& sim, const std::string& sys, int id)
{
    for (auto& e : sim.SystemById(sys)->entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<Structure*>(e.get());
    return nullptr;
}

bool LoadBlueprintsInline(const std::string& body)
{
    const std::string path = "blueprints_test_tmp.json";
    {
        std::ofstream f(path);
        f << body;
    }
    const bool ok = Blueprints::Load(path);
    std::remove(path.c_str());
    return ok;
}
}  // namespace

TEST_CASE("a blueprint is data, and a wrong one does not load (#39)")
{
    LoadRegistries();
    const Blueprint* beacon = Blueprints::Find("beacon");
    REQUIRE(beacon != nullptr);
    const Archetype* a = Archetypes::Find(beacon->archetype);
    REQUIRE(a != nullptr);
    CHECK(a->kind == EntityKind::Structure);
    CHECK(a->Has(Component::Buildable));
    CHECK_FALSE(beacon->cost.empty());
    CHECK(beacon->buildSeconds > 0.0f);
    CHECK(Blueprints::PerAccount() > 0);
    // The site every blueprint is dressed in until it is done.
    REQUIRE(Archetypes::Find(Structure::SITE_ARCHETYPE) != nullptr);

    const std::string ok = R"("archetype": "structure.beacon", "cost": { "Iron": 1 },
                               "placement": { "reach": 100 })";
    CHECK(LoadBlueprintsInline(R"({ "blueprints": [ { "id": "x", )" + ok + " } ] }"));
    CHECK(Blueprints::Find("x") != nullptr);

    SUBCASE("a misspelt field")
    {
        CHECK_FALSE(LoadBlueprintsInline(R"({ "blueprints": [ { "id": "x", "buildSecs": 4, )" + ok +
                                         " } ] }"));
        CHECK(Blueprints::Error().find("buildSecs") != std::string::npos);
    }
    SUBCASE("a resource that does not exist, rather than iron by default")
    {
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.beacon",
                 "cost": { "Gold": 1 }, "placement": { "reach": 100 } } ] })"));
        CHECK(Blueprints::Error().find("Gold") != std::string::npos);
    }
    SUBCASE("an archetype that is not a buildable structure")
    {
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "station.trade_hub",
                 "cost": { "Iron": 1 }, "placement": { "reach": 100 } } ] })"));
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.site",
                 "cost": { "Iron": 1 }, "placement": { "reach": 100 } } ] })"));
    }
    SUBCASE("a blueprint that costs nothing, or may go nowhere")
    {
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.beacon",
                 "cost": {}, "placement": { "reach": 100 } } ] })"));
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.beacon",
                 "cost": { "Iron": 1 } } ] })"));
    }
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));  // as the other tests expect it
}

TEST_CASE("a site is laid down only where, when and by whom the rules allow (#39)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Builder(sim, "hunter");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    Stock(s);
    const Vector2 free = FreeSpot(sim, s, beacon);

    SUBCASE("it costs what the blueprint says, from the hold")
    {
        s.ship->ClearCargo();
        CHECK(sim.PlacementProblem(s, beacon, free).find("the hold has") == 0);
        CHECK(sim.Deploy(s, "beacon", free, "") == 0);
        CHECK(s.journal.back().text.find("Cannot build: the hold has") == 0);
        Stock(s);
        const int ironBefore = s.ship->GetCargoAmount(ResourceType::Iron);
        const int id = sim.Deploy(s, "beacon", free, "Lantern");
        REQUIRE(id != 0);
        for (const auto& c : beacon.cost)
            if (c.first == ResourceType::Iron)
                CHECK(s.ship->GetCargoAmount(ResourceType::Iron) == ironBefore - c.second);
        Structure* t = StructureById(sim, sys, id);
        REQUIRE(t != nullptr);
        CHECK(t->GetName() == "Lantern");
        CHECK(t->GetOwner() == "hunter");
        CHECK(t->IsBuilding());
        CHECK(t->GetArchetype()->id == Structure::SITE_ARCHETYPE);
        CHECK(t->GetCompletesAt() == doctest::Approx(sim.Time() + beacon.buildSeconds));
    }

    SUBCASE("not from a station, and not out of reach")
    {
        s.dockedStationId = 1;
        CHECK(sim.PlacementProblem(s, beacon, free).find("undock") == 0);
        s.dockedStationId = 0;
        const Vector2 far = { free.x + beacon.reach + 10.0f, free.y };
        CHECK(sim.PlacementProblem(s, beacon, far).find("too far") == 0);
    }
    SUBCASE("not in the way of what others use")
    {
        const Station* hub = nullptr;
        for (const auto& e : sim.SystemById(sys)->entities)
            if (e->GetKind() == EntityKind::Station && !e->GetOrbit())
                hub = static_cast<const Station*>(e.get());
        REQUIRE(hub != nullptr);
        const Vector2 next = { hub->GetPosition().x + hub->GetSize() + 5.0f, hub->GetPosition().y };
        s.ship->Teleport(next);
        CHECK(sim.PlacementProblem(s, beacon, next).find("too close to " + hub->GetName()) == 0);
    }
    SUBCASE("not inside a star, nor where a planet will come round")
    {
        const Planet* planet = nullptr;
        for (const auto& e : sim.SystemById(sys)->entities)
            if (e->GetKind() == EntityKind::Planet &&
                static_cast<const Planet*>(e.get())->GetOrbitRadius() > 0.0f)
                planet = static_cast<const Planet*>(e.get());
        REQUIRE(planet != nullptr);
        // On its orbit, but on the far side from where it is now: empty today, not for long.
        const Vector2 p = planet->GetPosition();
        const float   r = planet->GetOrbitRadius();
        const float   len = std::sqrt(p.x * p.x + p.y * p.y);
        REQUIRE(len > 0.0f);
        const Vector2 across = { -p.x / len * r, -p.y / len * r };
        s.ship->Teleport(across);
        CHECK(sim.PlacementProblem(s, beacon, across).find("in the path of") == 0);
        s.ship->Teleport({ 10.0f, 10.0f });
        CHECK(sim.PlacementProblem(s, beacon, { 10.0f, 10.0f }) == "too close to the star");
    }
    SUBCASE("not crowded together, and not without end")
    {
        REQUIRE(sim.Deploy(s, "beacon", free, "") != 0);
        CHECK(sim.PlacementProblem(s, beacon, free).find("too close to Beacon") == 0);

        // The per-account cap holds across the galaxy.
        const Blueprint& buoy = *Blueprints::Find("buoy");
        const Vector2    spare = FreeSpot(sim, s, buoy, 500000.0f);
        float            along = 0.0f;
        int              built = 1;
        while (built < Blueprints::PerAccount())
        {
            along += 3000.0f;
            const Vector2 p = FreeSpot(sim, s, buoy, along);
            Stock(s);
            REQUIRE(sim.Deploy(s, "buoy", p, "") != 0);
            built++;
        }
        CHECK(sim.StructuresOwnedBy("hunter") == Blueprints::PerAccount());
        s.ship->Teleport(spare);
        CHECK(sim.PlacementProblem(s, buoy, spare).find("you already have") == 0);
    }
    SUBCASE("a structure needs an owner")
    {
        ClientSession& nobody = Builder(sim, "");
        Stock(nobody);
        nobody.ship->Teleport(free);
        CHECK(sim.PlacementProblem(nobody, beacon, free) == "only an account can own a structure");
    }
}

TEST_CASE("a site is finished by the clock, and everyone is told (#39)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&   s = Builder(sim, "hunter");
    ClientSession&   other = Builder(sim, "ann");
    const Blueprint& beacon = *Blueprints::Find("beacon");
    Stock(s);
    const Vector2 at = FreeSpot(sim, s, beacon);
    sim.TakeLayoutDeltas();  // whatever the world's start said

    // Another player's client, kept current the way both clients keep theirs.
    Proto::LayoutMirror mirror;
    mirror.Reset(sim.BuildLayout(s.systemId));
    auto deliver = [&]()
    {
        for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
        {
            Proto::LayoutDelta wire;
            REQUIRE(Proto::DecodeLayoutDelta(Proto::EncodeLayoutDelta(d), wire));
            mirror.Apply(wire);
        }
    };

    const int id = sim.Deploy(s, "beacon", at, "");
    REQUIRE(id != 0);
    deliver();
    REQUIRE(mirror.byId.count(id) == 1);
    const Proto::EntityLayout& site = mirror.byId.at(id);
    CHECK(site.kind == EntityKind::Structure);
    CHECK(site.owner == "hunter");
    CHECK(site.archetype == "structure.beacon");  // what it will be
    CHECK(site.completesAt == doctest::Approx(sim.Time() + beacon.buildSeconds));
    CHECK(site.expiresAt > site.completesAt);
    const double done = site.completesAt;

    // Not a moment early, whatever the ticks were.
    sim.SetTime(done - 0.01);
    sim.StepStructures();
    deliver();
    CHECK(StructureById(sim, s.systemId, id)->IsBuilding());

    const int seq = s.LastEventSeq();
    sim.SetTime(done);
    sim.StepStructures();
    deliver();
    Structure* t = StructureById(sim, s.systemId, id);
    CHECK_FALSE(t->IsBuilding());
    CHECK(t->GetArchetype()->id == "structure.beacon");
    CHECK(mirror.byId.at(id).completesAt == 0.0);  // built, for the other player too
    bool told = false;
    for (const Ev::Event& e : s.EventsSince(seq))
        told = told || e.kind == Ev::Kind::Built;
    CHECK(told);
    for (const Ev::Event& e : other.journal)
        CHECK(e.kind != Ev::Kind::Built);  // news to its owner only

    // And when its time is up, it is gone for everyone.
    sim.SetTime(t->GetExpiresAt());
    sim.StepStructures();
    deliver();
    CHECK(StructureById(sim, s.systemId, id) == nullptr);
    CHECK(mirror.byId.count(id) == 0);
    CHECK(sim.StructuresOwnedBy("hunter") == 0);
}

TEST_CASE("a site outlives a restart and still finishes on time (#39)")
{
    LoadRegistries();
    const std::string path = "world_build_tmp.json";
    Simulation        a;
    StartServer(a, "");
    ClientSession&    s = Builder(a, "hunter");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    const Blueprint&  buoy = *Blueprints::Find("buoy");
    Stock(s);
    const int site = a.Deploy(s, "beacon", FreeSpot(a, s, beacon), "Halfway");
    REQUIRE(site != 0);
    const int built = a.Deploy(s, "buoy", FreeSpot(a, s, buoy, 40000.0f), "");
    REQUIRE(built != 0);
    a.SetTime(a.Time() + buoy.buildSeconds + 1.0);
    a.StepStructures();
    REQUIRE_FALSE(StructureById(a, sys, built)->IsBuilding());
    const Structure before = *StructureById(a, sys, site);
    a.SaveWorld(path);

    Simulation b;
    StartServer(b, path);
    std::remove(path.c_str());
    const Proto::SystemLayout lay = b.BuildLayout(sys);
    int                       found = 0;
    for (const Proto::EntityLayout& e : lay.entities)
    {
        if (e.kind != EntityKind::Structure)
            continue;
        found++;
        CHECK(e.owner == "hunter");
        if (e.name == "Halfway")
        {
            CHECK(e.completesAt == doctest::Approx(before.GetCompletesAt()));
            CHECK(e.expiresAt == doctest::Approx(before.GetExpiresAt()));
            Structure* t = StructureById(b, sys, e.id);
            REQUIRE(t != nullptr);
            CHECK(t->IsBuilding());
            b.SetTime(before.GetCompletesAt());
            b.StepStructures();
            CHECK_FALSE(t->IsBuilding());
        }
        else
            CHECK(e.completesAt == 0.0);  // the buoy was built, and still is
    }
    CHECK(found == 2);
    CHECK(b.StructuresOwnedBy("hunter") == 2);  // the cap counts what came back
}
