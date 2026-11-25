#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/InventoryList.h"
#include "sim/Observation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

#include "goods_named.h"
#include <nlohmann/json.hpp>

#include <fstream>
#include <memory>
#include <set>
#include <string>

// A corporation structure's storage in divisions, and a hangar each for its members (#472
// slice 5): Shared, Stock and Command, each with the lowest role that may see, put and take,
// every take a line in the corporation's journal; a pilot's own structure keeps one storage.

namespace
{
constexpr double NOW = 1.8e9;

std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

struct World
{
    Simulation        sim;
    const std::string sys = "core";
    int               depot = 0;

    World()
    {
        REQUIRE(Factions::Load(DataFile("factions.json")));
        REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
        REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
        REQUIRE(r.Join("boris", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "dina", { "dina" }, NOW).Ok());
        REQUIRE(r.Join("dina", "DH", NOW).Ok());
        REQUIRE(r.SetRole("anna", "dina", Corps::Role::Director).Ok());
        const Blueprint* bp = Blueprints::Find("outpost");
        REQUIRE(bp != nullptr);
        auto t = std::make_unique<Structure>(Vector2{ 120000.0f, -300000.0f }, 0.0f, "Depot",
                                             bp->archetype);
        t->SetBlueprint(bp->id);
        depot = sim.AddStatic(sys, std::move(t), Corps::OwnerOf(r.IdOf("anna")));
        REQUIRE(depot != 0);
    }
    Structure& Depot()
    {
        for (const auto& e : sim.SystemById(sys)->entities)
            if (e->GetId() == depot)
                return static_cast<Structure&>(*e);
        FAIL("the depot is gone");
        return static_cast<Structure&>(*sim.SystemById(sys)->entities[0]);
    }
    // A pilot docked at the depot with some Iron in the hold.
    ClientSession& Docked(const std::string& account)
    {
        ClientSession& s = sim.CreateSession(sys, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = account;
        s.ship->ClearCargo();
        REQUIRE(s.ship->AddCargo(Good::Iron(), 30));
        s.dockedStationId = depot;
        return s;
    }
    int Move(ClientSession& s, const char* from, const char* to, int n)
    {
        return sim.MoveGoods(s, from, to, Good::Iron().Index(), n);
    }
};
}  // namespace

TEST_CASE("a corporation's storage has three divisions with a minimum role per right (#472)")
{
    World          w;
    ClientSession& boris = w.Docked("boris");  // a member
    ClientSession& dina = w.Docked("dina");    // a director
    ClientSession& anna = w.Docked("anna");    // the CEO

    // Shared: members put and take.
    CHECK(w.Move(boris, "hold", "storage", 10) == 10);
    CHECK(w.Depot().GetStoredAmount(Good::Iron()) == 10);
    CHECK(w.Move(boris, "storage", "hold", 4) == 4);
    // Stock: members put, directors take.
    CHECK(w.Move(boris, "hold", "storage:stock", 8) == 8);
    CHECK(w.Move(boris, "storage:stock", "hold", 1) == 0);
    CHECK(w.Move(dina, "storage:stock", "hold", 3) == 3);
    // Command: directors only -- a member does not even see it.
    CHECK(w.Move(boris, "hold", "storage:command", 1) == 0);
    CHECK(w.Move(dina, "hold", "storage:command", 5) == 5);
    CHECK(w.Move(anna, "storage:command", "hold", 2) == 2);
    CHECK(w.Depot().Division("command")->Amount(Good::Iron()) == 3);
    CHECK(w.Depot().GetStoredUsed() == (6 + 5 + 3) * 10);

    // What each role sees is in its snapshot, and a container per division.
    const Proto::PlayerView mine = w.sim.BuildSnapshot(boris, w.sys).player;
    REQUIRE(mine.divisions.size() == 2);
    CHECK(mine.divisions[1].id == "stock");
    CHECK_FALSE(mine.divisions[1].canTake);
    const auto boxes = InventoryList::Reachable(mine);
    CHECK(InventoryList::Find(boxes, "storage:stock") != nullptr);
    CHECK(InventoryList::Find(boxes, "storage:command") == nullptr);
    CHECK(InventoryList::Find(boxes, "storage:stock")->putOnly);
    CHECK(w.sim.BuildSnapshot(dina, w.sys).player.divisions.size() == 3);
    // ...and survives the wire.
    Proto::Snapshot snap = w.sim.BuildSnapshot(dina, w.sys);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    CHECK(back.player.divisions.size() == 3);
    CHECK(back.player.divisions[2].id == "command");
}

TEST_CASE("every take from a corporation's division is a line in its journal (#472)")
{
    World          w;
    ClientSession& boris = w.Docked("boris");
    CHECK(w.Move(boris, "hold", "storage", 10) == 10);
    const Corps::Corp* c = w.sim.Corporations().Find(w.sim.Corporations().IdOf("anna"));
    const size_t       before = c->journal.size();
    CHECK(w.Move(boris, "hold", "storage", 1) == 1);  // a put is not logged
    CHECK(c->journal.size() == before);
    CHECK(w.Move(boris, "storage", "hold", 6) == 6);
    REQUIRE(c->journal.size() == before + 1);
    CHECK(c->journal.back().text.find("boris took 6 Iron from Shared in Depot") !=
          std::string::npos);
}

TEST_CASE("the CEO or a director renames a division and moves its rights (#472)")
{
    World            w;
    Corps::Registry& r = w.sim.Corporations();
    auto act = [&](const std::string& by, const std::string& div, const std::string& label,
                   const std::string& right, const std::string& role)
    {
        Corps::Act a;
        a.verb = "division";
        a.name = div;
        a.value = label;
        a.right = right;
        a.role = role;
        return w.sim.ApplyCorpAct(by, a, {}, NOW);
    };
    CHECK_FALSE(act("boris", "stock", "Vault", "", "").Ok());  // a member may not
    CHECK(act("dina", "stock", "Vault", "take", "member").Ok());
    CHECK(r.DivisionLabel("boris", "stock") == "Vault");
    CHECK(r.MayDivision("boris", "boris", "stock", "take"));
    CHECK_FALSE(act("anna", "attic", "x", "", "").Ok());
    CHECK_FALSE(act("anna", "stock", "", "borrow", "member").Ok());
    CHECK_FALSE(act("anna", "stock", "", "view", "janitor").Ok());
    CHECK_FALSE(act("anna", "stock", "", "", "").Ok());
    // Back to the defaults leaves nothing to save.
    CHECK(act("anna", "stock", "Stock", "take", "director").Ok());
    CHECK(r.Find(r.IdOf("anna"))->divisions.empty());

    // What it set is saved with the corporations.
    REQUIRE(act("anna", "command", "War chest", "view", "ceo").Ok());
    Corps::Registry again;
    std::string     why;
    REQUIRE(again.FromJson(r.ToJson(), why));
    CHECK(again.DivisionLabel("anna", "command") == "War chest");
    CHECK_FALSE(again.MayDivision("anna", "dina", "command", "view"));
    CHECK(again.MayDivision("anna", "anna", "command", "view"));
}

TEST_CASE("a member has a hangar of their own at a corporation's structure (#472)")
{
    World          w;
    ClientSession& boris = w.Docked("boris");
    ClientSession& dina = w.Docked("dina");
    CHECK(w.Move(boris, "hold", "hangar", 7) == 7);
    CHECK(w.Depot().FindHangar("boris")->Amount(Good::Iron()) == 7);
    // Nobody else's: dina has her own, empty.
    CHECK(w.Move(dina, "hangar", "hold", 1) == 0);
    CHECK(w.Move(dina, "hold", "hangar", 3) == 3);
    CHECK(w.Depot().FindHangar("boris")->Amount(Good::Iron()) == 7);
    // It is in the snapshot, and counts against the structure's one capacity.
    CHECK(w.sim.BuildSnapshot(boris, w.sys).player.hangar.goods[Good::Iron().Index()] == 7);
    CHECK(w.Depot().GetStoredUsed() == 100);
    Structure& d = w.Depot();
    d.AddStored(Good::Ice(), (d.GetStorageCapacity() - d.GetStoredUsed()) / 10);
    CHECK(w.Move(boris, "hold", "hangar", 1) == 0);  // the room is shared
    CHECK(w.Move(boris, "hangar", "hold", 7) == 7);

    // A pilot who is not of the corporation has none of it.
    ClientSession& cara = w.Docked("cara");
    CHECK(w.Move(cara, "hold", "storage", 1) == 0);
    CHECK(w.Move(cara, "hold", "hangar", 1) == 0);
}

TEST_CASE("divisions and hangars are saved with the structure (#472)")
{
    World          w;
    ClientSession& boris = w.Docked("boris");
    ClientSession& dina = w.Docked("dina");
    CHECK(w.Move(boris, "hold", "storage", 5) == 5);
    CHECK(w.Move(boris, "hold", "storage:stock", 6) == 6);
    CHECK(w.Move(dina, "hold", "storage:command", 7) == 7);
    CHECK(w.Move(boris, "hold", "hangar", 8) == 8);
    const TempDir     tmp;
    const std::string path = tmp.Path("world_divisions.json");
    REQUIRE(w.sim.SaveWorld(path));
    std::ifstream     in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("\"version\": " + std::to_string(Save::WORLD_VERSION)) != std::string::npos);
    CHECK(Save::WORLD_VERSION >= 19);
    CHECK(text.find("\"divisions\"") != std::string::npos);
    CHECK(text.find("\"hangars\"") != std::string::npos);
}

TEST_CASE("the structure's room is said once and each division says its own use (#555)")
{
    World          w;
    ClientSession& dina = w.Docked("dina");  // a director sees all three divisions
    REQUIRE(w.Move(dina, "hold", "storage", 10) == 10);
    REQUIRE(w.Move(dina, "hold", "storage:stock", 5) == 5);
    REQUIRE(w.Move(dina, "hold", "hangar", 3) == 3);
    const int used = w.Depot().GetStoredUsed();  // every division and hangar, in volume

    Proto::Snapshot snap = w.sim.BuildSnapshot(dina, w.sys);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    CHECK(back.player.storageUsed == used);
    CHECK(back.player.storageCap == w.Depot().GetStorageCapacity());
    const auto boxes = InventoryList::Reachable(back.player);
    for (const char* id : { "storage", "storage:stock", "storage:command" })
    {
        const InventoryList::Container* c = InventoryList::Find(boxes, id);
        REQUIRE(c != nullptr);
        CHECK(c->pool == back.player.storageCap);
        CHECK(c->poolUsed == used);
    }

    // The agent's report: the whole room once, the divisions by use, no claim that only the
    // hold keeps goods, and the member hangar is free rather than "0 cr every 0 days".
    Obs::View view;
    view.snapshot = &back;
    const std::string text = Obs::DescribeInventory(view);
    const std::string total =
        "Storage " + std::to_string(used) + " / " + std::to_string(back.player.storageCap);
    CHECK(text.find(total) != std::string::npos);
    CHECK(text.find(total, text.find(total) + 1) == std::string::npos);
    CHECK(text.find("Shared (storage) 100") != std::string::npos);
    CHECK(text.find("Stock (storage:stock) 50") != std::string::npos);
    CHECK(text.find("Nothing here keeps goods for you") == std::string::npos);
    CHECK(text.find("free: no rent here") != std::string::npos);
    CHECK(text.find("every 0 days") == std::string::npos);
}
