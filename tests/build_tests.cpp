#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Derelict.h"
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
#include <vector>

// Construction (#39). First slice: a blueprint is data, a site is laid down by a player verb
// the server checks, it finishes at an instant of the world clock, it reaches everyone as a
// LayoutDelta, and it outlives a restart. Second: taking things down -- the owner dismantles
// for the refund the data states, anyone may shoot one down under the law of where it stands,
// and what is left is a wreck.

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

    const std::string ok = R"("archetype": "structure.beacon", "cost": { "Iron": 1 }, "hull": 10,
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
    SUBCASE("something that cannot be destroyed, or leaves a wreck that is no wreck (#39)")
    {
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.beacon",
                 "cost": { "Iron": 1 }, "placement": { "reach": 100 } } ] })"));
        CHECK(Blueprints::Error().find("hull") != std::string::npos);
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "wreck": "station.trade_hub", )" + ok + " } ] }"));
        CHECK(Blueprints::Error().find("wreck") != std::string::npos);
    }
    SUBCASE("rules of taking down that are not shares")
    {
        CHECK_FALSE(LoadBlueprintsInline(R"({ "blueprints": [ { "id": "x", )" + ok +
                                         R"( } ], "dismantle": { "refund": 1.5 } })"));
        CHECK(Blueprints::Error().find("refund") != std::string::npos);
        CHECK_FALSE(LoadBlueprintsInline(R"({ "blueprints": [ { "id": "x", )" + ok +
                                         R"( } ], "destruction": { "wreckshare": 0.1 } })"));
        CHECK(LoadBlueprintsInline(R"({ "blueprints": [ { "id": "x", )" + ok +
                                   R"( } ], "dismantle": { "refund": 0.3 } })"));
        CHECK(Blueprints::Rules().refund == doctest::Approx(0.3f));
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

// --- Taking things down (#39, slice 2) -----------------------------------------------------

namespace
{
// One shot from `s` at `id`, the cooldown spent: what a held trigger does every half second.
bool Shoot(Simulation& sim, ClientSession& s, int id)
{
    s.weaponOn = true;
    s.fireTimer = 0.0f;
    Simulation::PlayerCombatEvents ev;
    return sim.StepPlayerFire(s, *sim.SystemOf(s), id, 0.5f, &ev);
}

// The derelicts standing in a system, by id.
std::vector<const Derelict*> Wrecks(Simulation& sim, const std::string& sys)
{
    std::vector<const Derelict*> out;
    for (const auto& e : sim.SystemById(sys)->entities)
        if (e->GetKind() == EntityKind::Derelict)
            out.push_back(static_cast<const Derelict*>(e.get()));
    return out;
}

// The system's law, set rather than assumed from the data.
void Law(Simulation& sim, const std::string& sys, bool claimed, FactionId holder)
{
    sim.SystemById(sys)->agg.claimed = claimed;
    sim.SystemById(sys)->agg.controller = holder;
}

int Held(const ClientSession& s, ResourceType r)
{
    return s.ship->GetCargoAmount(r);
}
}  // namespace

TEST_CASE("what dismantling returns is the rule in the data (#39)")
{
    LoadRegistries();
    const Blueprint& beacon = *Blueprints::Find("beacon");  // 10 Iron, 2 Crystal
    const float      refund = Blueprints::Rules().refund;
    REQUIRE(refund > 0.0f);
    REQUIRE(refund < 1.0f);

    auto amount = [&](const std::vector<std::pair<ResourceType, int>>& v, ResourceType r)
    {
        for (const auto& c : v)
            if (c.first == r)
                return c.second;
        return 0;
    };
    // A site cancelled the moment it went down: all of it, crated.
    CHECK(amount(Blueprints::Refund(beacon, 0.0f, 1.0f), ResourceType::Iron) == 10);
    CHECK(amount(Blueprints::Refund(beacon, 0.0f, 1.0f), ResourceType::Crystal) == 2);
    // Finished: the rule's share, rounded down -- never more than went in.
    CHECK(amount(Blueprints::Refund(beacon, 1.0f, 1.0f), ResourceType::Iron) ==
          (int)(10 * refund + 1e-4));
    // Falling in between, and less again for what was shot off.
    const int half = amount(Blueprints::Refund(beacon, 0.5f, 1.0f), ResourceType::Iron);
    CHECK(half < 10);
    CHECK(half >= amount(Blueprints::Refund(beacon, 1.0f, 1.0f), ResourceType::Iron));
    CHECK(amount(Blueprints::Refund(beacon, 1.0f, 0.5f), ResourceType::Iron) <
          amount(Blueprints::Refund(beacon, 1.0f, 1.0f), ResourceType::Iron));
    CHECK(Blueprints::Refund(beacon, 1.0f, 0.0f).empty());
    // And dismantling always beats being shot down: the wreck holds less.
    CHECK(Blueprints::Rules().wreckShare < refund);
}

TEST_CASE("the owner takes a site or a structure apart, and nobody else can (#39)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Builder(sim, "hunter");
    ClientSession&    other = Builder(sim, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    Stock(s);
    const Vector2 at = FreeSpot(sim, s, beacon);
    sim.TakeLayoutDeltas();
    Proto::LayoutMirror mirror;
    mirror.Reset(sim.BuildLayout(sys));

    SUBCASE("a site cancelled at once returns everything")
    {
        const int id = sim.Deploy(s, "beacon", at, "");
        REQUIRE(id != 0);
        CHECK(Held(s, ResourceType::Iron) == 10);
        CHECK(sim.Dismantle(s, id));
        CHECK(Held(s, ResourceType::Iron) == 20);
        CHECK(Held(s, ResourceType::Crystal) == 5);
        CHECK(StructureById(sim, sys, id) == nullptr);
        CHECK(sim.StructuresOwnedBy("hunter") == 0);
        CHECK(s.journal.back().text.find("Dismantled Beacon") == 0);
        for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
            mirror.Apply(d);
        CHECK(mirror.byId.count(id) == 0);  // gone for everyone, through RemoveStatic
    }
    SUBCASE("a finished one returns the rule's share")
    {
        const int id = sim.Deploy(s, "beacon", at, "");
        REQUIRE(id != 0);
        sim.SetTime(sim.Time() + beacon.buildSeconds);
        sim.StepStructures();
        REQUIRE_FALSE(StructureById(sim, sys, id)->IsBuilding());
        CHECK(sim.Dismantle(s, id));
        const int back = (int)(10 * Blueprints::Rules().refund + 1e-4);
        CHECK(Held(s, ResourceType::Iron) == 10 + back);
    }
    SUBCASE("refused: someone else's, too far, docked, or no room for it")
    {
        const int id = sim.Deploy(s, "beacon", at, "");
        REQUIRE(id != 0);
        other.ship->Teleport(at);
        CHECK(sim.DismantleProblem(other, id) == "Beacon is not yours");
        CHECK_FALSE(sim.Dismantle(other, id));
        CHECK(other.journal.back().text == "Cannot dismantle: Beacon is not yours");

        s.ship->Teleport({ at.x + beacon.reach + 200.0f, at.y });
        CHECK(sim.DismantleProblem(s, id).find("too far from Beacon") == 0);
        s.ship->Teleport(at);
        s.dockedStationId = 1;
        CHECK(sim.DismantleProblem(s, id).find("undock") == 0);
        s.dockedStationId = 0;

        CHECK(s.ship->AddCargo(ResourceType::Iron,
                               s.ship->GetCargoCapacity() - s.ship->GetCargoUsed()));
        CHECK(sim.DismantleProblem(s, id).find("the hold has room for 0") == 0);
        CHECK(StructureById(sim, sys, id) != nullptr);  // nothing lost to a full hold
        CHECK(sim.DismantleProblem(s, 999999).find("there is no structure") == 0);
    }
    SUBCASE("a faction's outpost is not a player's to take apart")
    {
        const Blueprint& outpost = *Blueprints::Find("outpost");
        auto t = std::make_unique<Structure>(at, 0.0f, "Guild Outpost", outpost.archetype);
        t->SetBlueprint(outpost.id);
        const int id = sim.AddStatic(sys, std::move(t), Outposts::OwnerOf(FactionId::TradersGuild));
        REQUIRE(id != 0);
        CHECK(sim.DismantleProblem(s, id) == "Guild Outpost is not yours");
    }
}

TEST_CASE("a structure is shot down, leaves a wreck, and the law decides the price (#39, #41)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Builder(sim, "hunter");
    ClientSession&    ann = Builder(sim, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    const Blueprint&  buoy = *Blueprints::Find("buoy");
    Stock(s);
    const Vector2 at = FreeSpot(sim, s, beacon);
    const int     id = sim.Deploy(s, "beacon", at, "Lantern");
    REQUIRE(id != 0);
    ann.ship->Teleport({ at.x + 100.0f, at.y });
    sim.TakeLayoutDeltas();
    Proto::LayoutMirror mirror;
    mirror.Reset(sim.BuildLayout(sys));
    auto deliver = [&]()
    {
        for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
        {
            Proto::LayoutDelta wire;
            REQUIRE(Proto::DecodeLayoutDelta(Proto::EncodeLayoutDelta(d), wire));
            mirror.Apply(wire);
        }
    };

    // A site just laid down is the rule's share of the finished thing, and grows.
    Structure* t = StructureById(sim, sys, id);
    REQUIRE(t != nullptr);
    CHECK(t->MaxHull(sim.Time()) == doctest::Approx(beacon.hull * Blueprints::Rules().siteHull));
    CHECK(t->MaxHull(t->GetCompletesAt()) == doctest::Approx(beacon.hull));

    SUBCASE("under a lawful holder's law, every hit and the kill are crimes")
    {
        Law(sim, sys, true, FactionId::TradersGuild);
        const size_t wrecksBefore = Wrecks(sim, sys).size();
        REQUIRE(Shoot(sim, ann, id));
        deliver();
        CHECK(mirror.byId.at(id).damage > 0.0f);  // everyone sees the hull go down
        CHECK(mirror.byId.at(id).blueprint == "beacon");
        CHECK(ann.account.GetBounty(FactionId::TradersGuild) == doctest::Approx(5.0));
        CHECK(ann.account.GetReputation(FactionId::TradersGuild) < 0.0f);

        int shots = 1;
        while (StructureById(sim, sys, id) != nullptr && shots < 100)
        {
            Shoot(sim, ann, id);
            shots++;
        }
        deliver();
        CHECK(StructureById(sim, sys, id) == nullptr);
        CHECK(mirror.byId.count(id) == 0);
        CHECK(ann.account.GetBounty(FactionId::TradersGuild) ==
              doctest::Approx(5.0 * shots + 50.0));
        CHECK(sim.StructuresOwnedBy("hunter") == 0);
        CHECK(s.journal.back().text.find("The site of Lantern") == 0);

        // The wreck its blueprint names, where it stood, worth the rule's share of the cost.
        const std::vector<const Derelict*> wrecks = Wrecks(sim, sys);
        REQUIRE(wrecks.size() == wrecksBefore + 1);
        const Derelict* w = wrecks.back();
        CHECK(w->GetArchetype()->id == beacon.wreck);
        CHECK(w->GetPosition().x == doctest::Approx(at.x));
        CHECK(w->GetBaseName() == "Wreck of Lantern");
        const double worth = 10 * 10.0 + 2 * 40.0;  // the base prices of Iron and Crystal
        CHECK(w->GetReward() ==
              doctest::Approx(std::floor(worth * Blueprints::Rules().wreckShare)));
        CHECK(w->GetOwner().empty());  // nobody's: whoever gets there first
    }
    SUBCASE("in a system nobody holds, and on your own, there is no price")
    {
        Law(sim, sys, false, FactionId::Independent);
        Shoot(sim, ann, id);
        CHECK(ann.account.GetBounty(FactionId::Independent) == 0.0);
        Law(sim, sys, true, FactionId::Pirates);  // a pirate's system has no law to break
        Shoot(sim, ann, id);
        for (int f = 0; f < FACTION_COUNT; f++)
            CHECK(ann.account.GetBounty((FactionId)f) == 0.0);
        Law(sim, sys, true, FactionId::TradersGuild);
        s.ship->Teleport(ann.ship->GetPosition());
        Shoot(sim, s, id);
        CHECK(s.account.GetBounty(FactionId::TradersGuild) == 0.0);
        CHECK(StructureById(sim, sys, id)->GetDamage() == doctest::Approx(48.0f));
    }
    SUBCASE("something too small for a wreck leaves none")
    {
        REQUIRE(buoy.wreck.empty());
        Stock(s);
        const Vector2 p = FreeSpot(sim, s, buoy, 40000.0f);
        const int     b = sim.Deploy(s, "buoy", p, "");
        REQUIRE(b != 0);
        ann.ship->Teleport(p);
        const size_t before = Wrecks(sim, sys).size();
        for (int i = 0; i < 50 && StructureById(sim, sys, b) != nullptr; i++)
            Shoot(sim, ann, b);
        CHECK(StructureById(sim, sys, b) == nullptr);
        CHECK(Wrecks(sim, sys).size() == before);
    }
    SUBCASE("a lawful faction's outpost is that faction's, wherever it stands; a pirate's is not")
    {
        Law(sim, sys, false, FactionId::Independent);
        const Blueprint& outpost = *Blueprints::Find("outpost");
        auto             put = [&](FactionId f, Vector2 p)
        {
            auto o = std::make_unique<Structure>(p, 0.0f, "Outpost", outpost.archetype);
            o->SetBlueprint(outpost.id);
            return sim.AddStatic(sys, std::move(o), Outposts::OwnerOf(f));
        };
        const int guild = put(FactionId::TradersGuild, { at.x, at.y + 300.0f });
        const int gang = put(FactionId::Pirates, { at.x, at.y - 300.0f });
        Shoot(sim, ann, guild);
        CHECK(ann.account.GetBounty(FactionId::TradersGuild) == doctest::Approx(5.0));
        Shoot(sim, ann, gang);
        CHECK(ann.account.GetBounty(FactionId::Pirates) == 0.0);
    }
}

TEST_CASE("an attack order closes in and fires until the target is gone (#39)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Builder(sim, "hunter");
    ClientSession&    ann = Builder(sim, "ann");
    const std::string sys = s.systemId;
    Stock(s);
    const Vector2 at = FreeSpot(sim, s, *Blueprints::Find("beacon"));
    const int     id = sim.Deploy(s, "beacon", at, "");
    REQUIRE(id != 0);
    ann.ship->Teleport({ at.x + 1500.0f, at.y });

    Orders::Order o;
    o.kind = Orders::Kind::Attack;
    o.targetId = id;
    sim.GiveOrder(ann, o);
    bool fired = false;
    for (int i = 0; i < 60 * 60 && ann.orderStatus == Orders::Status::Running; i++)
    {
        sim.StepPlayerOrder(ann, *sim.SystemOf(ann), 1.0f / 60.0f);
        fired = fired || !ann.orderShots.empty();
        ann.orderShots.clear();
    }
    CHECK(ann.orderStatus == Orders::Status::Done);
    CHECK(ann.orderDetail == "target destroyed");
    CHECK(fired);  // the host draws these as beams
    CHECK(StructureById(sim, sys, id) == nullptr);
    CHECK_FALSE(ann.weaponOn);  // armed for the order, and only for it

    // Something that is neither a ship nor a structure is not a target.
    int station = 0;
    for (const auto& e : sim.SystemById(sys)->entities)
        if (e->GetKind() == EntityKind::Station)
            station = e->GetId();
    o.targetId = station;
    sim.GiveOrder(ann, o);
    sim.StepPlayerOrder(ann, *sim.SystemOf(ann), 1.0f / 60.0f);
    CHECK(ann.orderStatus == Orders::Status::Failed);
}

TEST_CASE("damage and wrecks outlive a restart; a command and a layout carry them (#39)")
{
    LoadRegistries();
    const std::string path = "world_teardown_tmp.json";
    Simulation        a;
    StartServer(a, "");
    ClientSession&    s = Builder(a, "hunter");
    ClientSession&    ann = Builder(a, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    Stock(s);
    const Vector2 p1 = FreeSpot(a, s, beacon);
    const int     hit = a.Deploy(s, "beacon", p1, "Dented");
    REQUIRE(hit != 0);
    const Vector2 p2 = FreeSpot(a, s, beacon, 40000.0f);
    const int     gone = a.Deploy(s, "beacon", p2, "Gone");
    REQUIRE(gone != 0);
    ann.ship->Teleport(p1);
    Shoot(a, ann, hit);
    Shoot(a, ann, hit);
    const float damage = StructureById(a, sys, hit)->GetDamage();
    REQUIRE(damage > 0.0f);
    ann.ship->Teleport(p2);
    for (int i = 0; i < 100 && StructureById(a, sys, gone) != nullptr; i++)
        Shoot(a, ann, gone);
    REQUIRE(StructureById(a, sys, gone) == nullptr);
    a.SaveWorld(path);

    Simulation b;
    StartServer(b, path);
    std::remove(path.c_str());
    bool dented = false, wreck = false, ghost = false;
    for (const Proto::EntityLayout& e : b.BuildLayout(sys).entities)
    {
        if (e.kind == EntityKind::Structure && e.name == "Dented")
        {
            dented = true;
            CHECK(e.damage == doctest::Approx(damage));
            CHECK(e.blueprint == "beacon");
        }
        ghost = ghost || (e.kind == EntityKind::Structure && e.name == "Gone");
        wreck = wreck || (e.kind == EntityKind::Derelict && e.name == "Wreck of Gone");
    }
    CHECK(dented);
    CHECK(wreck);
    CHECK_FALSE(ghost);

    // On the wire: the verb, and what a client needs to show the hull.
    Proto::Command c;
    c.dismantleId = 42;
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.dismantleId == 42);
    Proto::SystemLayout lay = b.BuildLayout(sys), wire;
    REQUIRE(Proto::DecodeLayout(Proto::EncodeLayout(lay), wire));
    bool seen = false;
    for (const Proto::EntityLayout& e : wire.entities)
        if (e.name == "Dented")
        {
            seen = true;
            CHECK(e.damage == doctest::Approx(damage));
            CHECK(e.blueprint == "beacon");
        }
    CHECK(seen);
}
