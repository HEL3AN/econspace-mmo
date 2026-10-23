#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Derelict.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/InventoryList.h"
#include "sim/Observation.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include <map>
#include <memory>
#include <string>

// Cans and wrecks as containers (#472): a pilot jettisons goods into a can beside the ship --
// their own within reach, or a new one -- anyone takes them out again, a wreck holds every good
// of the ship it was, and a search takes all of them that fit.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

void StartServer(Simulation& sim, const std::string& worldPath = std::string())
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

bool Told(const ClientSession& s, int seq, const std::string& text)
{
    for (const Ev::Event& e : s.EventsSince(seq))
        if (e.text.find(text) != std::string::npos)
            return true;
    return false;
}

const Derelict* CanById(Simulation& sim, const std::string& sys, int id)
{
    for (const auto& e : sim.SystemById(sys)->entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Derelict)
            return static_cast<const Derelict*>(e.get());
    return nullptr;
}

ClientSession& Pilot(Simulation& sim, const std::string& name, Vector2 at)
{
    ClientSession& s = sim.CreateSession(sim.Universe().startId, at, GetShipCatalog()[0].stats);
    s.accountName = name;
    return s;
}

int NewestCan(Simulation& sim, const std::string& sys)
{
    int id = 0;
    for (const auto& e : sim.SystemById(sys)->entities)
        if (e->GetArchetype() != nullptr && e->GetArchetype()->id == "derelict.can")
            id = std::max(id, e->GetId());
    return id;
}
}  // namespace

TEST_CASE("Cans: a jettison puts goods in a can beside the ship, and adds to one's own")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim);
    ClientSession& s = Pilot(sim, "hunter", { 300000.0f, 300000.0f });
    REQUIRE(s.ship->AddCargo(Good::Iron(), 20));
    REQUIRE(s.ship->AddCargo(Good::Ice(), 6));

    int seq = s.LastEventSeq();
    CHECK(sim.MoveGoods(s, "hold", "can", Good::Iron().Index(), 12) == 12);
    const int can = NewestCan(sim, s.systemId);
    REQUIRE(can != 0);
    const Derelict* d = CanById(sim, s.systemId, can);
    CHECK(d->GetOwner() == "hunter");
    CHECK(d->Contents().Amount(Good::Iron()) == 12);
    CHECK(d->GetExpiresAt() > sim.Time());
    CHECK(Told(s, seq, "Moved 12 Iron from the hold to Cargo can of"));
    CHECK(s.ship->GetCargoAmount(Good::Iron()) == 8);

    // A second jettison goes into the same can: one can, two goods.
    CHECK(sim.MoveGoods(s, "hold", "can", Good::Ice().Index(), 6) == 6);
    CHECK(NewestCan(sim, s.systemId) == can);
    CHECK(d->Contents().Amount(Good::Ice()) == 6);

    // The layout carries every good of it, and the window lists it, within reach.
    const Proto::SystemLayout          layout = sim.BuildLayout(s.systemId);
    std::map<int, Proto::EntityLayout> byId;
    for (const Proto::EntityLayout& l : layout.entities)
        byId[l.id] = l;
    REQUIRE(byId.count(can) == 1);
    CHECK(InventoryList::Describe(byId[can].holds) == "12 Iron, 6 Ice");
    Proto::Snapshot snap = sim.BuildSnapshot(s, s.systemId);
    Proto::CompleteFromLayout(snap, byId);
    const auto  all = InventoryList::Reachable(snap, byId, "hunter");
    const auto* c = InventoryList::Find(all, "can:" + std::to_string(can));
    REQUIRE(c != nullptr);
    CHECK_FALSE(c->takeOnly);  // one's own: it takes more
    CHECK(InventoryList::Find(InventoryList::Reachable(snap, byId, "ann"),
                              "can:" + std::to_string(can))
              ->takeOnly);  // to anyone else it only gives

    // Out of it again, by the same verb.
    CHECK(sim.MoveGoods(s, "can:" + std::to_string(can), "hold", Good::Iron().Index(), 5) == 5);
    CHECK(s.ship->GetCargoAmount(Good::Iron()) == 13);

    // Docked, there is nothing to jettison into.
    s.dockedStationId = 1;
    seq = s.LastEventSeq();
    CHECK(sim.MoveGoods(s, "hold", "can", Good::Iron().Index(), 1) == 0);
    CHECK(Told(s, seq, "undock first"));
    s.dockedStationId = 0;
}

TEST_CASE("Cans: another pilot takes from a can, puts nothing in it, and a search takes it all")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim);
    const Vector2  at{ 300000.0f, 300000.0f };
    ClientSession& a = Pilot(sim, "hunter", at);
    ClientSession& b = Pilot(sim, "ann", at);
    REQUIRE(a.ship->AddCargo(Good::Iron(), 10));
    REQUIRE(a.ship->AddCargo(Good::Crystal(), 4));
    REQUIRE(sim.MoveGoods(a, "hold", "can", Good::Iron().Index(), 10) == 10);
    REQUIRE(sim.MoveGoods(a, "hold", "can", Good::Crystal().Index(), 4) == 4);
    const int         can = NewestCan(sim, a.systemId);
    const std::string id = "can:" + std::to_string(can);

    // The hand-over: the other pilot takes some by the verb...
    CHECK(sim.MoveGoods(b, id, "hold", Good::Iron().Index(), 3) == 3);
    // ...puts nothing in a can that is not theirs...
    REQUIRE(b.ship->AddCargo(Good::Ice(), 2));
    int seq = b.LastEventSeq();
    CHECK(sim.MoveGoods(b, "hold", id, Good::Ice().Index(), 2) == 0);
    CHECK(Told(b, seq, "nothing can be put into"));
    // ...and a search takes every good that fits, at once.
    seq = b.LastEventSeq();
    sim.StepPlayerLoot(b, *sim.SystemOf(b), can);
    CHECK(b.ship->GetCargoAmount(Good::Iron()) == 10);
    CHECK(b.ship->GetCargoAmount(Good::Crystal()) == 4);
    CHECK(Told(b, seq, "Took 7 Iron, 4 Crystal from Cargo can of"));
    CHECK(CanById(sim, a.systemId, can)->IsLooted());

    // Out of reach, it is out of reach.
    REQUIRE(a.ship->AddCargo(Good::Ice(), 1));
    REQUIRE(sim.MoveGoods(a, "hold", "can", Good::Ice().Index(), 1) == 1);
    const int far = NewestCan(sim, a.systemId);
    b.ship->Teleport({ at.x + 50000.0f, at.y });
    seq = b.LastEventSeq();
    CHECK(sim.MoveGoods(b, "can:" + std::to_string(far), "hold", Good::Ice().Index(), 1) == 0);
    CHECK(Told(b, seq, "out of reach"));
}

TEST_CASE("Cans: a can and what it holds are saved with the world")
{
    LoadRegistries();
    Simulation sim;
    StartServer(sim);
    ClientSession& s = Pilot(sim, "hunter", { 300000.0f, 300000.0f });
    REQUIRE(s.ship->AddCargo(Good::Iron(), 9));
    REQUIRE(s.ship->AddCargo(Good::Ice(), 3));
    REQUIRE(sim.MoveGoods(s, "hold", "can", Good::Iron().Index(), 9) == 9);
    REQUIRE(sim.MoveGoods(s, "hold", "can", Good::Ice().Index(), 3) == 3);

    const TempDir     tmp;
    const std::string path = tmp.Path("world_cans.json");
    REQUIRE(sim.SaveWorld(path));
    Simulation b;
    StartServer(b, path);
    const Derelict* found = nullptr;
    for (const auto& e : b.SystemById(s.systemId)->entities)
        if (e->GetKind() == EntityKind::Derelict && e->GetOwner() == "hunter")
            found = static_cast<const Derelict*>(e.get());
    REQUIRE(found != nullptr);
    CHECK(found->GetArchetype()->id == "derelict.can");
    CHECK(found->Contents().Amount(Good::Iron()) == 9);
    CHECK(found->Contents().Amount(Good::Ice()) == 3);
}
