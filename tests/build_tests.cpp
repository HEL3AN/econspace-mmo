#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/Derelict.h"
#include "entities/NpcShip.h"
#include "entities/Planet.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include "raymath.h"
#include "factions_named.h"
#include <nlohmann/json.hpp>

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
    REQUIRE(Factions::Load(DataFile("factions.json")));
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
}

void StartServer(Simulation& sim, const std::string& worldPath)
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

ClientSession& Builder(Simulation& sim, const std::string& account)
{
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = account;
    sim.GrantLicence(account, s.systemId);  // the start is the Guild's space (#431 slice 6)
    return s;
}

// Enough for any blueprint the data has, in a hold of any size the catalog has.
void Stock(ClientSession& s)
{
    s.ship->ClearCargo();
    REQUIRE(s.ship->AddCargo(Good::Iron(), 20));
    REQUIRE(s.ship->AddCargo(Good::Crystal(), 5));
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
    const TempDir     tmp;
    const std::string path = tmp.Path("blueprints_test.json");
    {
        // Every blueprint names a tier (#431 D1); the file's one tier, unless the body has its own.
        std::ofstream f(path);
        if (body.find("\"tiers\"") == std::string::npos && body.rfind("{ ", 0) == 0)
            f << "{ \"tiers\": { \"t\": { \"mend\": 0.01 } }, " << body.substr(2);
        else
            f << body;
    }
    const bool ok = Blueprints::Load(path);
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
                               "tier": "t", "placement": { "reach": 100 })";
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
    SUBCASE("a tier it does not name, or one the file does not have (#431 D1)")
    {
        CHECK(Blueprints::Find("x")->mend == doctest::Approx(0.01f));
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.beacon",
                 "cost": { "Iron": 1 }, "hull": 10, "placement": { "reach": 100 } } ] })"));
        CHECK(Blueprints::Error().find("tier") != std::string::npos);
        CHECK_FALSE(LoadBlueprintsInline(
            R"({ "blueprints": [ { "id": "x", "archetype": "structure.beacon", "tier": "big",
                 "cost": { "Iron": 1 }, "hull": 10, "placement": { "reach": 100 } } ] })"));
        CHECK(Blueprints::Error().find("big") != std::string::npos);
        CHECK_FALSE(LoadBlueprintsInline(R"({ "tiers": { "t": { "mend": 2 } }, "blueprints": [
                 { "id": "x", )" + ok + " } ] }"));
        CHECK_FALSE(LoadBlueprintsInline(R"({ "tiers": { "t": { "mends": 0.1 } }, "blueprints": [
                 { "id": "x", )" + ok + " } ] }"));
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
        const int ironBefore = s.ship->GetCargoAmount(Good::Iron());
        const int id = sim.Deploy(s, "beacon", free, "Lantern");
        REQUIRE(id != 0);
        for (const auto& c : beacon.cost)
            if (c.first == Good::Iron())
                CHECK(s.ship->GetCargoAmount(Good::Iron()) == ironBefore - c.second);
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
    const TempDir     tmp;
    const std::string path = tmp.Path("world_build.json");
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
    CHECK(amount(Blueprints::Refund(beacon, 0.0f, 1.0f), Good::Iron()) == 10);
    CHECK(amount(Blueprints::Refund(beacon, 0.0f, 1.0f), Good::Crystal()) == 2);
    // Finished: the rule's share, rounded down -- never more than went in.
    CHECK(amount(Blueprints::Refund(beacon, 1.0f, 1.0f), Good::Iron()) ==
          (int)(10 * refund + 1e-4));
    // Falling in between, and less again for what was shot off.
    const int half = amount(Blueprints::Refund(beacon, 0.5f, 1.0f), Good::Iron());
    CHECK(half < 10);
    CHECK(half >= amount(Blueprints::Refund(beacon, 1.0f, 1.0f), Good::Iron()));
    CHECK(amount(Blueprints::Refund(beacon, 1.0f, 0.5f), Good::Iron()) <
          amount(Blueprints::Refund(beacon, 1.0f, 1.0f), Good::Iron()));
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
        CHECK(Held(s, Good::Iron()) == 10);
        CHECK(sim.Dismantle(s, id));
        CHECK(Held(s, Good::Iron()) == 20);
        CHECK(Held(s, Good::Crystal()) == 5);
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
        CHECK(Held(s, Good::Iron()) == 10 + back);
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

        CHECK(s.ship->AddCargo(Good::Iron(), s.ship->GetCargoCapacity() - s.ship->GetCargoUsed()));
        CHECK(sim.DismantleProblem(s, id).find("the hold has room for 0") == 0);
        CHECK(StructureById(sim, sys, id) != nullptr);  // nothing lost to a full hold
        CHECK(sim.DismantleProblem(s, 999999).find("there is no structure") == 0);
    }
    SUBCASE("a faction's outpost is not a player's to take apart")
    {
        const Blueprint& outpost = *Blueprints::Find("outpost");
        auto t = std::make_unique<Structure>(at, 0.0f, "Guild Outpost", outpost.archetype);
        t->SetBlueprint(outpost.id);
        const int id = sim.AddStatic(sys, std::move(t), Outposts::OwnerOf(Fac::TradersGuild()));
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
        Law(sim, sys, true, Fac::TradersGuild());
        const size_t wrecksBefore = Wrecks(sim, sys).size();
        REQUIRE(Shoot(sim, ann, id));
        deliver();
        CHECK(mirror.byId.at(id).damage > 0.0f);  // everyone sees the hull go down
        CHECK(mirror.byId.at(id).blueprint == "beacon");
        CHECK(ann.account.GetBounty(Fac::TradersGuild()) == doctest::Approx(5.0));
        CHECK(ann.account.GetReputation(Fac::TradersGuild()) < 0.0f);

        int shots = 1;
        while (StructureById(sim, sys, id) != nullptr && shots < 100)
        {
            Shoot(sim, ann, id);
            shots++;
        }
        deliver();
        CHECK(StructureById(sim, sys, id) == nullptr);
        CHECK(mirror.byId.count(id) == 0);
        CHECK(ann.account.GetBounty(Fac::TradersGuild()) == doctest::Approx(5.0 * shots + 50.0));
        CHECK(sim.StructuresOwnedBy("hunter") == 0);
        // Told as an attack, in one entry for the whole of it (#431 C2).
        CHECK(s.journal.back().kind == Ev::Kind::StructureAttacked);
        CHECK(s.journal.back().text.find("Your Lantern (being built)") == 0);
        CHECK(s.journal.back().text.find("destroyed") != std::string::npos);

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
        Law(sim, sys, false, Fac::Independent());
        Shoot(sim, ann, id);
        CHECK(ann.account.GetBounty(Fac::Independent()) == 0.0);
        Law(sim, sys, true, Fac::Pirates());  // a pirate's system has no law to break
        Shoot(sim, ann, id);
        for (FactionId f : Factions::All())
            CHECK(ann.account.GetBounty(f) == 0.0);
        Law(sim, sys, true, Fac::TradersGuild());
        s.ship->Teleport(ann.ship->GetPosition());
        Shoot(sim, s, id);
        CHECK(s.account.GetBounty(Fac::TradersGuild()) == 0.0);
        CHECK(StructureById(sim, sys, id)->GetDamage(sim.Time()) == doctest::Approx(48.0f));
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
        Law(sim, sys, false, Fac::Independent());
        const Blueprint& outpost = *Blueprints::Find("outpost");
        auto             put = [&](FactionId f, Vector2 p)
        {
            auto o = std::make_unique<Structure>(p, 0.0f, "Outpost", outpost.archetype);
            o->SetBlueprint(outpost.id);
            return sim.AddStatic(sys, std::move(o), Outposts::OwnerOf(f));
        };
        const int guild = put(Fac::TradersGuild(), { at.x, at.y + 300.0f });
        const int gang = put(Fac::Pirates(), { at.x, at.y - 300.0f });
        Shoot(sim, ann, guild);
        CHECK(ann.account.GetBounty(Fac::TradersGuild()) == doctest::Approx(5.0));
        Shoot(sim, ann, gang);
        CHECK(ann.account.GetBounty(Fac::Pirates()) == 0.0);
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
    const TempDir     tmp;
    const std::string path = tmp.Path("world_teardown.json");
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
    const float damage = StructureById(a, sys, hit)->GetDamage(a.Time());
    REQUIRE(damage > 0.0f);
    ann.ship->Teleport(p2);
    for (int i = 0; i < 100 && StructureById(a, sys, gone) != nullptr; i++)
        Shoot(a, ann, gone);
    REQUIRE(StructureById(a, sys, gone) == nullptr);
    a.SaveWorld(path);

    Simulation b;
    StartServer(b, path);
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

TEST_CASE("a build order flies to the spot and lays the site down there (#39)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&   s = Builder(sim, "hunter");
    const Blueprint& beacon = *Blueprints::Find("beacon");
    Stock(s);
    const Vector2 at = FreeSpot(sim, s, beacon);
    s.ship->Teleport({ at.x + 20000.0f, at.y });
    REQUIRE_FALSE(sim.PlacementProblem(s, beacon, at).empty());   // out of reach from here
    REQUIRE(sim.PlacementProblem(s, beacon, at, false).empty());  // ...and nothing else
    const int iron = Held(s, Good::Iron());

    Orders::Order o;
    o.kind = Orders::Kind::Build;
    o.point = at;
    o.blueprint = "beacon";
    o.name = "Waypoint";
    auto run = [&]()
    {
        sim.GiveOrder(s, o);
        for (int i = 0; i < 60 * 180 && s.orderStatus == Orders::Status::Running; i++)
            sim.StepPlayerOrder(s, *sim.SystemOf(s), 1.0f / 60.0f);
    };

    SUBCASE("it arrives and builds, and the site is the deploy verb's")
    {
        run();
        CHECK(s.orderStatus == Orders::Status::Done);
        CHECK(s.orderDetail.find("laid down") != std::string::npos);
        const Structure* site = nullptr;
        for (const auto& e : sim.SystemById(s.systemId)->entities)
            if (e->GetKind() == EntityKind::Structure && e->GetName() == "Waypoint")
                site = static_cast<const Structure*>(e.get());
        REQUIRE(site != nullptr);
        CHECK(site->GetOwner() == "hunter");
        CHECK(site->IsBuilding());
        CHECK(site->GetPosition().x == doctest::Approx(at.x));
        CHECK(site->GetPosition().y == doctest::Approx(at.y));
        CHECK(Held(s, Good::Iron()) == iron - 10);
    }
    SUBCASE("a hold that cannot pay is refused before it flies")
    {
        s.ship->ClearCargo();
        const Vector2 from = s.ship->GetPosition();
        run();
        CHECK(s.orderStatus == Orders::Status::Failed);
        CHECK(s.orderDetail.find("the hold has") != std::string::npos);
        CHECK(s.ship->GetPosition().x == doctest::Approx(from.x));
    }
    SUBCASE("a blueprint that does not exist, or is a faction's, is refused")
    {
        o.blueprint = "cathedral";
        run();
        CHECK(s.orderStatus == Orders::Status::Failed);
        o.blueprint = "outpost";
        run();
        CHECK(s.orderStatus == Orders::Status::Failed);
        CHECK(sim.StructuresOwnedBy("hunter") == 0);
    }

    // On the wire: the order names its blueprint and what to call the site.
    Proto::Command c;
    c.orderKind = (int)Orders::Kind::Build;
    c.orderPoint = at;
    c.orderBlueprint = "beacon";
    c.orderName = "Waypoint";
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.orderKind == (int)Orders::Kind::Build);
    CHECK(back.orderBlueprint == "beacon");
    CHECK(back.orderName == "Waypoint");
    CHECK(std::string(Orders::KindName(Orders::Kind::Build)) == "build");
}

TEST_CASE("an armed structure fires back, and the police answer an attack on one (#39)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Builder(sim, "hunter");
    ClientSession&    ann = Builder(sim, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    const Blueprint&  outpost = *Blueprints::Find("outpost");
    REQUIRE(Archetypes::Find(outpost.archetype)->Has(Component::Defensive));
    Stock(s);
    const Vector2 at = FreeSpot(sim, s, beacon);
    const float   dt = 1.0f / 60.0f;

    Simulation::PlayerPresence p;
    p.session = &ann;
    p.ship = ann.ship.get();
    const std::vector<Simulation::PlayerPresence> players{ p };
    std::vector<FireEvent>                        fires;
    auto put = [&](const std::string& owner, Vector2 where, bool site)
    {
        auto o = std::make_unique<Structure>(where, 0.0f, "Outpost", outpost.archetype);
        o->SetBlueprint(outpost.id);
        if (site)
            o->StartBuilding(sim.Time(), sim.Time() + 600.0);
        return sim.AddStatic(sys, std::move(o), owner);
    };
    auto defend = [&]()
    {
        for (int i = 0; i < 120; i++)
            sim.StepStationDefence(*sim.SystemById(sys), players, &fires, dt);
    };

    SUBCASE("a player's armed structure answers only whoever shot at it, for a while")
    {
        Law(sim, sys, false, Fac::Independent());
        const int id = put("hunter", at, false);
        ann.ship->Teleport({ at.x + 400.0f, at.y });
        defend();
        CHECK(fires.empty());  // in range, and nobody's enemy

        REQUIRE(Shoot(sim, ann, id));
        const float before = ann.ship->GetShields() + ann.ship->GetHull();
        defend();
        REQUIRE_FALSE(fires.empty());
        CHECK(fires[0].targetSessionId == ann.id);
        CHECK(ann.ship->GetShields() + ann.ship->GetHull() < before);

        fires.clear();
        sim.SetTime(sim.Time() + Simulation::PROVOKED_SECONDS + 1.0);
        defend();
        CHECK(fires.empty());
    }
    SUBCASE("a faction's fires on its enemies unprovoked; a site has no guns yet")
    {
        put(Outposts::OwnerOf(Fac::Pirates()), at, true);
        ann.ship->Teleport({ at.x + 400.0f, at.y });
        defend();
        CHECK(fires.empty());
        put(Outposts::OwnerOf(Fac::Pirates()), { at.x, at.y + 600.0f }, false);
        defend();
        REQUIRE_FALSE(fires.empty());
        CHECK(fires[0].shooterFaction == Fac::Pirates());
    }
    SUBCASE("the police come by warp to a crime against a structure, where it is hot")
    {
        const int id = sim.Deploy(s, "beacon", at, "Lantern");
        REQUIRE(id != 0);
        ann.ship->Teleport({ at.x + 100.0f, at.y });
        sim.Warm(*sim.SystemById(sys));
        FactionId law = Fac::Independent();
        bool      patrols = false;
        for (const auto& e : sim.SystemById(sys)->entities)
            if (e->GetKind() == EntityKind::Npc &&
                static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police)
            {
                law = static_cast<const NpcShip&>(*e).GetFaction();
                patrols = true;
            }
        REQUIRE(patrols);
        Law(sim, sys, true, law);
        auto near = [&]()
        {
            int n = 0;
            for (const auto& e : sim.SystemById(sys)->entities)
                if (e->GetKind() == EntityKind::Npc &&
                    static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police &&
                    Vector2Distance(e->GetPosition(), ann.ship->GetPosition()) < 3500.0f)
                    n++;
            return n;
        };
        REQUIRE(near() == 0);
        sim.SystemById(sys)->agg.security = 0.9f;  // the high band: no delay

        const int seq = ann.LastEventSeq();
        REQUIRE(Shoot(sim, ann, id));
        // How many is the security band's (#431 slice 9, law_tests.cpp); here, some.
        const int came = near();
        CHECK(came >= 1);
        bool told = false;
        for (const Ev::Event& e : ann.EventsSince(seq))
            told = told || e.text.find("police answer") != std::string::npos;
        CHECK(told);
        Shoot(sim, ann, id);  // one call brings them; the next shot brings no more
        CHECK(near() == came);
    }
    SUBCASE("no law broken, nobody comes")
    {
        const int id = sim.Deploy(s, "beacon", at, "Lantern");
        REQUIRE(id != 0);
        ann.ship->Teleport({ at.x + 100.0f, at.y });
        sim.Warm(*sim.SystemById(sys));
        Law(sim, sys, false, Fac::Independent());
        const int seq = ann.LastEventSeq();
        REQUIRE(Shoot(sim, ann, id));
        for (const Ev::Event& e : ann.EventsSince(seq))
            CHECK(e.text.find("police answer") == std::string::npos);
    }
}

TEST_CASE(
    "a structure's wreck decays on the clock, cold, and keeps its time across a restart (#41)")
{
    LoadRegistries();
    const TempDir     tmp;
    const std::string path = tmp.Path("world_wreck_decay.json");
    Simulation        a;
    StartServer(a, "");
    ClientSession&    s = Builder(a, "hunter");
    ClientSession&    ann = Builder(a, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  beacon = *Blueprints::Find("beacon");
    REQUIRE(Blueprints::Rules().wreckSeconds > 0.0f);
    Stock(s);
    const Vector2 at = FreeSpot(a, s, beacon);
    const int     id = a.Deploy(s, "beacon", at, "Gone");
    REQUIRE(id != 0);
    ann.ship->Teleport(at);
    for (int i = 0; i < 100 && StructureById(a, sys, id) != nullptr; i++)
        Shoot(a, ann, id);
    REQUIRE(StructureById(a, sys, id) == nullptr);
    const Derelict* w = Wrecks(a, sys).back();
    REQUIRE(w->GetBaseName() == "Wreck of Gone");
    const double end = w->GetExpiresAt();
    CHECK(end == doctest::Approx(a.Time() + Blueprints::Rules().wreckSeconds));
    for (const Derelict* d : Wrecks(a, sys))  // the world's own wrecks are the map's, and stay
        if (d != w)
            CHECK(d->GetExpiresAt() == 0.0);
    a.SaveWorld(path);

    auto wreckIn = [](Simulation& sim, const std::string& in) -> const Derelict*
    {
        for (const Derelict* d : Wrecks(sim, in))
            if (d->GetBaseName() == "Wreck of Gone")
                return d;
        return nullptr;
    };

    SUBCASE("a restart keeps the instant, and the system goes on with nobody in it")
    {
        Simulation b;
        StartServer(b, path);
        const Derelict* back = wreckIn(b, sys);
        REQUIRE(back != nullptr);
        CHECK(back->GetExpiresAt() == doctest::Approx(end));
        CHECK_FALSE(Simulation::IsHot(*b.SystemById(sys)));
        b.SetTime(end - 1.0);
        b.StepStructures();
        CHECK(wreckIn(b, sys) != nullptr);
        b.SetTime(end);
        b.StepStructures();
        CHECK(wreckIn(b, sys) == nullptr);
        b.SaveWorld(path);  // ...and it does not come back with the next restart
        Simulation c;
        StartServer(c, path);
        CHECK(wreckIn(c, sys) == nullptr);
    }
    SUBCASE("a wreck from a save before the rule is given its time from the load")
    {
        nlohmann::json j;
        {
            std::ifstream in(path);
            j = nlohmann::json::parse(in);
        }
        j["version"] = 8;
        for (auto& added : j["changes"][sys]["added"])
            if (added["array"] == "derelicts")
                added["object"].erase("expiresAt");
        {
            std::ofstream out(path);
            out << j.dump();
        }
        Simulation b;
        StartServer(b, path);
        const Derelict* back = wreckIn(b, sys);
        REQUIRE(back != nullptr);
        CHECK(back->GetExpiresAt() == doctest::Approx(b.Time() + Blueprints::Rules().wreckSeconds));
    }
}

// --- A player's outpost, and what it grows into (#433) -------------------------------------

namespace
{
// A builder whose hold takes any chain of blueprints the data has.
ClientSession& Founder(Simulation& sim, const std::string& account)
{
    ShipStats stats = GetShipCatalog()[0].stats;
    stats.cargoCapacity = 2000;
    ClientSession& s = sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, stats);
    s.accountName = account;
    sim.GrantLicence(account, s.systemId);  // (#431 slice 6)
    return s;
}

// What `bp` costs, in the hold.
void Pay(ClientSession& s, const char* blueprint)
{
    const Blueprint* bp = Blueprints::Find(blueprint);
    REQUIRE(bp != nullptr);
    for (const auto& c : bp->cost)
        REQUIRE(s.ship->AddCargo(c.first, c.second));
}

// Beside a structure, within its dock and the blueprints' reach.
void Beside(ClientSession& s, const Structure& t)
{
    s.ship->Teleport({ t.GetPosition().x + t.GetSize() + 60.0f, t.GetPosition().y });
}

// Finishes whatever `id` is building.
void Finish(Simulation& sim, const std::string& sys, int id)
{
    Structure* t = StructureById(sim, sys, id);
    REQUIRE(t != nullptr);
    REQUIRE(t->IsBuilding());
    sim.SetTime(t->GetCompletesAt());
    sim.StepStructures();
    REQUIRE_FALSE(StructureById(sim, sys, id)->IsBuilding());
}

bool Told(const ClientSession& s, int seq, const std::string& text)
{
    for (const Ev::Event& e : s.EventsSince(seq))
        if (e.text.find(text) != std::string::npos)
            return true;
    return false;
}

// A structure of a loaded world, by name: ids are the load's own.
const Structure* Named(Simulation& sim, const std::string& sys, const std::string& name)
{
    for (const auto& e : sim.SystemById(sys)->entities)
        if (e->GetKind() == EntityKind::Structure && e->GetName() == name)
            return static_cast<const Structure*>(e.get());
    return nullptr;
}
}  // namespace

TEST_CASE("a player's outpost is one of the structures the account's cap counts (#433)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&   s = Founder(sim, "hunter");
    const Blueprint& buoy = *Blueprints::Find("buoy");
    const Blueprint& outpost = *Blueprints::Find("outpost");
    float            along = 0.0f;
    while (sim.StructuresOwnedBy("hunter") < Blueprints::PerAccount())
    {
        along += 3000.0f;
        Pay(s, "buoy");
        REQUIRE(sim.Deploy(s, "buoy", FreeSpot(sim, s, buoy, along), "") != 0);
    }
    Pay(s, "outpost");
    bool asked = false;
    for (int i = 0; i < 60 && !asked; i++)
    {
        const Vector2 p = { -150000.0f, -270000.0f + 9300.0f * (float)i };
        if (!sim.SpotProblem(*sim.SystemOf(s), outpost, p).empty())
            continue;
        s.ship->Teleport(p);
        CHECK(sim.PlacementProblem(s, outpost, p).find("you already have") == 0);
        CHECK(sim.Deploy(s, "outpost", p, "") == 0);
        asked = true;
    }
    CHECK(asked);
}

TEST_CASE("a player builds an outpost, docks at it and grows it along the chain (#433)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Founder(sim, "hunter");
    ClientSession&    ann = Founder(sim, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  outpost = *Blueprints::Find("outpost");
    const Blueprint&  market = *Blueprints::Find("outpost_market");
    REQUIRE(outpost.byPlayers);
    Pay(s, "outpost");
    const Vector2 at = FreeSpot(sim, s, outpost);
    const int     id = sim.Deploy(s, "outpost", at, "Hearth");
    REQUIRE(id != 0);
    CHECK(StructureById(sim, sys, id)->GetOwner() == "hunter");
    CHECK(sim.StructuresOwnedBy("hunter") == 1);

    // Not grown before it stands.
    Pay(s, "outpost_market");
    CHECK(sim.PlacementProblem(s, market, at) == "Hearth is still being built");
    Finish(sim, sys, id);

    SUBCASE("one outpost to a system, whoever's")
    {
        // Another plain one is refused by its blueprint's own cap; another kind, by this rule.
        const Blueprint& mining = *Blueprints::Find("outpost_mining");
        REQUIRE(mining.byPlayers);
        Pay(ann, "outpost_mining");
        bool asked = false;
        for (int i = 0; i < 3600 && !asked; i++)
        {
            const Vector2 p = { -150000.0f - 9100.0f * (float)(i / 60),
                                -270000.0f + 9300.0f * (float)(i % 60) };
            if (!sim.SpotProblem(*sim.SystemById(sys), mining, p).empty())
                continue;
            ann.ship->Teleport(p);
            CHECK(sim.PlacementProblem(ann, mining, p) ==
                  "this system has an outpost already: Hearth");
            asked = true;
        }
        CHECK(asked);
    }

    SUBCASE("its dock takes its owner and the owner's corporation, nobody else")
    {
        const Structure& t = *StructureById(sim, sys, id);
        Beside(s, t);
        CHECK(sim.StepPlayerDock(s, *sim.SystemOf(s)) == id);

        Beside(ann, t);
        const int seq = ann.LastEventSeq();
        CHECK(sim.StepPlayerDock(ann, *sim.SystemOf(ann)) == 0);
        CHECK(Told(ann, seq, "Docking denied: Hearth is hunter's"));

        Corps::Registry& corps = sim.Corporations();
        REQUIRE(corps.Create("hunter", "Hearth Holders", "HH", 1.8e9).Ok());
        REQUIRE(corps.InviteTo("hunter", "ann", { "ann" }, 1.8e9).Ok());
        REQUIRE(corps.Join("ann", "HH", 1.8e9).Ok());
        CHECK(sim.StepPlayerDock(ann, *sim.SystemOf(ann)) == id);
    }

    SUBCASE("only its owner grows it")
    {
        Pay(ann, "outpost_market");
        Beside(ann, *StructureById(sim, sys, id));
        CHECK(sim.PlacementProblem(ann, market, at).find("Hearth is not yours") == 0);
        const int seq = ann.LastEventSeq();
        CHECK(sim.Deploy(ann, "outpost_market", at, "") == 0);
        CHECK(Told(ann, seq, "Cannot build: Hearth is not yours"));
        CHECK(StructureById(sim, sys, id)->GetBlueprintId() == "outpost");
    }

    SUBCASE("the cap counts structures, and an upgrade adds none")
    {
        const Blueprint& buoy = *Blueprints::Find("buoy");
        float            along = 0.0f;
        while (sim.StructuresOwnedBy("hunter") < Blueprints::PerAccount())
        {
            along += 3000.0f;
            Pay(s, "buoy");
            REQUIRE(sim.Deploy(s, "buoy", FreeSpot(sim, s, buoy, along), "") != 0);
        }
        Beside(s, *StructureById(sim, sys, id));
        CHECK(sim.PlacementProblem(s, market, at).empty());
    }

    SUBCASE("market, then station, at the factions' costs and times, and still the owner's")
    {
        Beside(s, *StructureById(sim, sys, id));
        const int markets = sim.SystemById(sys)->profile.markets;
        const int iron = s.ship->GetCargoAmount(Good::Iron());
        REQUIRE(sim.Deploy(s, "outpost_market", at, "") == id);
        for (const auto& c : market.cost)
            if (c.first == Good::Iron())
                CHECK(s.ship->GetCargoAmount(Good::Iron()) == iron - c.second);
        Structure* t = StructureById(sim, sys, id);
        CHECK(t->IsBuilding());
        CHECK(t->GetBlueprintId() == "outpost_market");
        CHECK(t->GetName() == "Hearth");
        CHECK(t->GetOwner() == "hunter");
        CHECK(t->GetCompletesAt() == doctest::Approx(sim.Time() + market.buildSeconds));
        CHECK(sim.StructuresOwnedBy("hunter") == 1);

        // Saved half-grown, it comes back half-grown and the owner's.
        const TempDir     tmp;
        const std::string path = tmp.Path("world_upgrade.json");
        REQUIRE(sim.SaveWorld(path));
        {
            Simulation b;
            StartServer(b, path);
            const Structure* back = Named(b, sys, "Hearth");
            REQUIRE(back != nullptr);
            CHECK(back->GetOwner() == "hunter");
            CHECK(back->IsBuilding());
            CHECK(back->GetBlueprintId() == "outpost_market");
            CHECK(back->GetCompletesAt() == doctest::Approx(t->GetCompletesAt()));
        }

        // A market joins the system's economy, and is open to every pilot.
        Finish(sim, sys, id);
        t = StructureById(sim, sys, id);
        CHECK(t->GetArchetype()->id == market.archetype);
        CHECK(sim.SystemById(sys)->profile.markets == markets + 1);
        Beside(ann, *t);
        CHECK(sim.StepPlayerDock(ann, *sim.SystemOf(ann)) == id);
        sim.StepPlayerUndock(ann);

        // ...and then a station, which stays a structure of the player's own.
        Pay(s, "trade_station");
        Beside(s, *t);
        REQUIRE(sim.Deploy(s, "trade_station", at, "Hearth Exchange") == id);
        Finish(sim, sys, id);
        t = StructureById(sim, sys, id);
        REQUIRE(t != nullptr);
        CHECK(t->GetArchetype()->id == "station.trade_hub");
        CHECK(t->GetOwner() == "hunter");
        CHECK(t->GetName() == "Hearth Exchange");
        CHECK(t->Has(Component::Market));
        CHECK(sim.SystemById(sys)->profile.markets == markets + 1);

        REQUIRE(sim.SaveWorld(path));
        Simulation c;
        StartServer(c, path);
        const Structure* back = Named(c, sys, "Hearth Exchange");
        REQUIRE(back != nullptr);
        CHECK(back->GetOwner() == "hunter");
        CHECK_FALSE(back->IsBuilding());
        CHECK(back->GetBuilds() == "station.trade_hub");
        CHECK(c.StructuresOwnedBy("hunter") == 1);
    }
}

// --- A player's shipyard (#432) -----------------------------------------------------------

namespace
{
// The fit of the hull `s` flies with its tractor swapped for a laser: a refit that changes it.
std::vector<Ships::FitLine> Rearmed(const ClientSession& s)
{
    const Ships::Design* stock = Archetypes::ShipCatalogue().FindDesign(
        GetShipCatalog()[(size_t)s.currentShip].stats.design);
    REQUIRE(stock != nullptr);
    std::vector<Ships::FitLine> fit = Ships::Normalized(stock->fit);
    bool                        swapped = false;
    for (Ships::FitLine& l : fit)
    {
        l.scale = 0.0f;
        if (l.module == "weapons.tractor")
        {
            l.module = "weapons.laser";
            swapped = true;
        }
    }
    REQUIRE(swapped);
    return fit;
}

void EmptyHold(ClientSession& s)
{
    for (ResourceType rt : AllResourceTypes())
        if (s.ship->GetCargoAmount(rt) > 0)
            s.ship->RemoveCargo(rt, s.ship->GetCargoAmount(rt));
}
}  // namespace

TEST_CASE("a player grows their own station into a shipyard and refits there; nobody else may "
          "(#432)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    s = Founder(sim, "hunter");
    ClientSession&    ann = Founder(sim, "ann");
    const std::string sys = s.systemId;
    const Blueprint&  yard = *Blueprints::Find("shipyard");
    REQUIRE(yard.byPlayers);
    Pay(s, "outpost");
    const Vector2 at = FreeSpot(sim, s, *Blueprints::Find("outpost"));
    const int     id = sim.Deploy(s, "outpost", at, "Hearth");
    REQUIRE(id != 0);
    Finish(sim, sys, id);

    // Not from a bare outpost: a yard is grown from a market or a station.
    Pay(s, "shipyard");
    Beside(s, *StructureById(sim, sys, id));
    CHECK(sim.PlacementProblem(s, yard, at).find("Hearth is not one") != std::string::npos);
    Pay(s, "outpost_market");
    REQUIRE(sim.Deploy(s, "outpost_market", at, "") == id);
    Finish(sim, sys, id);
    Pay(s, "trade_station");
    REQUIRE(sim.Deploy(s, "trade_station", at, "") == id);
    Finish(sim, sys, id);

    // A trade station docks and trades, and refits nothing.
    s.account.SetMoney(5000.0);
    const std::vector<Ships::FitLine> rearmed = Rearmed(s);
    REQUIRE(sim.StepPlayerDock(s, *sim.SystemOf(s)) == id);
    int seq = s.LastEventSeq();
    CHECK_FALSE(sim.RefitShip(s, rearmed));
    CHECK(Told(s, seq, "has no shipyard"));
    sim.StepPlayerUndock(s);

    // Nobody grows another's station.
    Pay(ann, "shipyard");
    Beside(ann, *StructureById(sim, sys, id));
    CHECK(sim.PlacementProblem(ann, yard, at).find("Hearth is not yours") == 0);
    seq = ann.LastEventSeq();
    CHECK(sim.Deploy(ann, "shipyard", at, "") == 0);
    CHECK(Told(ann, seq, "Cannot build: Hearth is not yours"));
    CHECK(StructureById(sim, sys, id)->GetBlueprintId() == "trade_station");

    // Its owner does, at the factions' cost and time; it stays theirs.
    const int yardsBefore = sim.SystemById(sys)->profile.shipyards;
    Beside(s, *StructureById(sim, sys, id));
    REQUIRE(sim.Deploy(s, "shipyard", at, "Hearth Yards") == id);
    Structure* t = StructureById(sim, sys, id);
    CHECK(t->IsBuilding());
    CHECK(t->GetCompletesAt() == doctest::Approx(sim.Time() + yard.buildSeconds));
    CHECK(sim.SystemById(sys)->profile.shipyards == yardsBefore);  // not one while going up
    Finish(sim, sys, id);
    t = StructureById(sim, sys, id);
    REQUIRE(t != nullptr);
    CHECK(t->GetArchetype()->id == yard.archetype);
    CHECK(t->GetOwner() == "hunter");
    CHECK(t->GetName() == "Hearth Yards");
    CHECK(t->Has(Component::Market));
    CHECK(t->Has(Component::Storage));
    CHECK(sim.SystemById(sys)->profile.shipyards == yardsBefore + 1);
    CHECK(sim.StructuresOwnedBy("hunter") == 1);

    // Docked there, the ship is refitted and paid for; it is no hull-issuing station to wake at.
    EmptyHold(s);
    Beside(s, *t);
    const int home = s.homeStationId;
    REQUIRE(sim.StepPlayerDock(s, *sim.SystemOf(s)) == id);
    REQUIRE(sim.RefitShip(s, rearmed));
    CHECK(s.account.GetMoney() < 5000.0);
    CHECK(Ships::IsPlayerDesign(s.DesignOf(s.currentShip)));
    CHECK(s.homeStationId == home);
    // ...and it is open to every pilot, like any market.
    Beside(ann, *t);
    CHECK(sim.StepPlayerDock(ann, *sim.SystemOf(ann)) == id);
}

TEST_CASE("defences mend by tier: one frigate cannot bring an outpost down, enough cruisers can "
          "(#431 slice 7)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    owner = Builder(sim, "hunter");
    const std::string sys = owner.systemId;
    const Blueprint&  outpost = *Blueprints::Find("outpost");
    Stock(owner);
    const Vector2 at = FreeSpot(sim, owner, *Blueprints::Find("beacon"));  // open space
    auto          made = std::make_unique<Structure>(at, 0.0f, "Keep", outpost.archetype);
    made->SetBlueprint(outpost.id);
    const int id = sim.AddStatic(sys, std::move(made), "hunter");  // a player's, finished
    REQUIRE(id != 0);
    Law(sim, sys, false, Fac::Independent());  // nobody's law: only the guns count here
    const float mend = StructureById(sim, sys, id)->MendRate();
    REQUIRE(mend == doctest::Approx(outpost.mend * outpost.hull));

    ShipStats frigate, cruiser;
    REQUIRE(StatsOfDesign("cutter", frigate));
    REQUIRE(StatsOfDesign("cruiser", cruiser));
    const float shotsPerSecond = 2.0f;  // a player's gun fires every half second
    auto        guns = [&](const ShipStats& stats, int n)
    {
        std::vector<ClientSession*> out;
        for (int i = 0; i < n; i++)
        {
            ClientSession& g = sim.CreateSession(sys, at, stats);
            g.accountName = "gun" + std::to_string(out.size()) + stats.design;
            out.push_back(&g);
        }
        return out;
    };
    // Every gun fires as fast as it can for `seconds` of world time, or until it is down.
    auto fire = [&](const std::vector<ClientSession*>& by, double seconds)
    {
        const double until = sim.Time() + seconds;
        while (sim.Time() < until && StructureById(sim, sys, id) != nullptr)
        {
            for (ClientSession* g : by)
                Shoot(sim, *g, id);
            sim.SetTime(sim.Time() + 1.0 / shotsPerSecond);
        }
    };

    // One frigate's fire is below the mend: ten minutes of it, and the outpost has taken no
    // more than one shot's worth.
    CHECK(frigate.damage * shotsPerSecond < mend);
    fire(guns(frigate, 1), 600.0);
    REQUIRE(StructureById(sim, sys, id) != nullptr);
    CHECK(StructureById(sim, sys, id)->GetDamage(sim.Time()) <= frigate.damage + 0.01f);

    // A few cruisers' is above it, and it comes down.
    int n = 1;
    while ((float)n * cruiser.damage * shotsPerSecond < mend * 1.5f)
        n++;
    CHECK(n <= 4);  // an outpost falls to a few pilots...
    fire(guns(cruiser, n), 600.0);
    CHECK(StructureById(sim, sys, id) == nullptr);

    // ...a station's frame only to a fleet, and a beacon to anyone.
    const Blueprint& station = *Blueprints::Find("trade_station");
    CHECK(station.mend * station.hull / (cruiser.damage * shotsPerSecond) >= 8.0f);
    const Blueprint& beacon = *Blueprints::Find("beacon");
    CHECK(beacon.mend * beacon.hull < frigate.damage * shotsPerSecond);
}

TEST_CASE("the front line binds claims and sieges, never guns (#431 D3)")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim, "");
    ClientSession&    owner = Builder(sim, "hunter");
    const std::string sys = owner.systemId;
    const Blueprint&  outpost = *Blueprints::Find("outpost");
    Stock(owner);
    const Vector2 at = FreeSpot(sim, owner, *Blueprints::Find("beacon"));
    auto          made = std::make_unique<Structure>(at, 0.0f, "Keep", outpost.archetype);
    made->SetBlueprint(outpost.id);
    const int id = sim.AddStatic(sys, std::move(made), "hunter");
    REQUIRE(id != 0);
    // Deep Haul holds the system, deep in the Guild's space: every neighbour is the Guild's.
    REQUIRE(sim.Corporations().Create("hunter", "Deep Haul", "DH", sim.Time()).Ok());
    REQUIRE(sim.Corporations().Create("raider", "Kestrel Union", "KX", sim.Time()).Ok());
    const std::string dh = sim.Corporations().IdOf("hunter");
    const std::string kx = sim.Corporations().IdOf("raider");
    Law(sim, sys, false, Factions::Unaffiliated());
    sim.SystemById(sys)->agg.holderCorp = dh;
    REQUIRE_FALSE(sim.Neighbors(sys).empty());
    for (const std::string& n : sim.Neighbors(sys))
        Law(sim, n, true, Fac::TradersGuild());
    CHECK_FALSE(sim.FrontOpen(sys, kx));
    CHECK_FALSE(sim.FrontOpen(sys, Fac::Pirates()));

    // Behind the line, guns are still guns: a few cruisers bring the outpost down.
    ShipStats cruiser;
    REQUIRE(StatsOfDesign("cruiser", cruiser));
    std::vector<ClientSession*> guns;
    for (int i = 0; i < 4; i++)
    {
        ClientSession& g = sim.CreateSession(sys, at, cruiser);
        g.accountName = i == 0 ? std::string("raider") : "raider" + std::to_string(i);
        guns.push_back(&g);
    }
    const double until = sim.Time() + 600.0;
    while (sim.Time() < until && StructureById(sim, sys, id) != nullptr)
    {
        for (ClientSession* g : guns)
            Shoot(sim, *g, id);
        sim.SetTime(sim.Time() + 0.5);
    }
    CHECK(StructureById(sim, sys, id) == nullptr);
}
