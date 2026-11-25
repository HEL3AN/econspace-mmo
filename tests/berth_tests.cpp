#include <doctest/doctest.h>

#include "factions_named.h"
#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Prediction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

// Ships have places (#472 slice 8): the ship a pilot leaves stays where it was, with its own
// hold, and is switched to only from there.

namespace
{
const std::string DATA = TEST_DATA_DIR;

struct World
{
    Simulation     sim;
    ClientSession* s = nullptr;
    SystemState*   st = nullptr;

    World()
    {
        REQUIRE(Factions::Load(DATA + "factions.json"));
        REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
        REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        s = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
        s->accountName = "hunter";
        st = sim.SystemOf(*s);
        Add(990001, "North Dock");
        Add(990002, "South Dock");
        s->account.SetMoney(1.0e6);
        s->dockedStationId = 990001;
    }
    void Add(int id, const char* name)
    {
        auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, name,
                                                 Fac::TradersGuild(), StationRole::TradeHub);
        station->SetId(id);
        st->entities.push_back(std::move(station));
    }
};

int OtherHull()
{
    for (int i = 1; i < (int)GetShipCatalog().size(); i++)
        return i;
    return 0;
}
}  // namespace

TEST_CASE("Berths: the ship left behind stays where it was, with its own hold (#472)")
{
    World     w;
    const int other = OtherHull();
    REQUIRE(other != 0);
    REQUIRE(w.s->ship->AddCargo(Good::Iron(), 3));

    REQUIRE(w.sim.BuyShip(*w.s, other));
    CHECK(w.s->currentShip == other);
    CHECK(w.s->ship->GetCargoAmount(Good::Iron()) == 0);  // the new hold is empty
    REQUIRE(w.s->berths.count(0) == 1);
    CHECK(w.s->berths.at(0).hold.Amount(Good::Iron()) == 3);  // the starter keeps its goods
    CHECK(w.sim.PlaceName(w.s->berths.at(0).place) == "North Dock, " + w.sim.SystemName(w.st->id));
    REQUIRE(w.s->ship->AddCargo(Good::Ice(), 2));

    // Docked elsewhere: it is not here, and the refusal says where it is.
    w.s->dockedStationId = 990002;
    const int seq = w.s->LastEventSeq();
    CHECK_FALSE(w.sim.SwitchShip(*w.s, 0));
    bool said = false;
    for (const Ev::Event& e : w.s->EventsSince(seq))
        said = said || e.text.find("North Dock") != std::string::npos;
    CHECK(said);
    const Proto::PlayerView away = w.sim.BuildSnapshot(*w.s, w.st->id).player;
    REQUIRE(away.shipHere.size() == away.ownedShips.size());
    for (size_t k = 0; k < away.ownedShips.size(); k++)
        if (away.ownedShips[k] == 0)
        {
            CHECK(away.shipHere[k] == 0);
            CHECK(away.shipPlaces[k].find("North Dock") != std::string::npos);
        }

    // Back there: switching swaps the holds, and leaves the one flown parked.
    w.s->dockedStationId = 990001;
    REQUIRE(w.sim.SwitchShip(*w.s, 0));
    CHECK(w.s->ship->GetCargoAmount(Good::Iron()) == 3);
    CHECK(w.s->ship->GetCargoAmount(Good::Ice()) == 0);
    CHECK(w.s->berths.at(other).hold.Amount(Good::Ice()) == 2);
    CHECK(w.s->berths.count(0) == 0);

    // In space there is no switching at all.
    w.s->dockedStationId = 0;
    CHECK_FALSE(w.sim.SwitchShip(*w.s, other));
}

TEST_CASE("Berths: places and holds are saved with the account (#472)")
{
    World     w;
    const int other = OtherHull();
    REQUIRE(w.s->ship->AddCargo(Good::Iron(), 4));
    REQUIRE(w.sim.BuyShip(*w.s, other));
    const TempDir     tmp;
    const std::string path = tmp.Path("hunter_berths.json");
    REQUIRE(w.sim.SaveAccount(*w.s, path));

    ClientSession& again = w.sim.CreateSession(w.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                               GetShipCatalog()[0].stats);
    again.accountName = "hunter";
    REQUIRE(w.sim.LoadAccount(again, path) == Save::Result::Ok);
    CHECK(again.currentShip == other);
    REQUIRE(again.berths.count(0) == 1);
    CHECK(again.berths.at(0).hold.Amount(Good::Iron()) == 4);
    CHECK(again.berths.at(0).place == w.s->berths.at(0).place);
    CHECK(Save::ACCOUNT_VERSION >= 10);
}

TEST_CASE("Berths: an account from before places has its ships wherever the pilot docks (#472)")
{
    World     w;
    const int other = OtherHull();
    REQUIRE(w.sim.BuyShip(*w.s, other));
    w.s->berths.clear();  // as an older save would have it
    w.s->berths[0] = ClientSession::Berth();
    CHECK(w.sim.ShipIsHere(*w.s, 0));
    w.s->dockedStationId = 990002;
    CHECK(w.sim.ShipIsHere(*w.s, 0));
    CHECK(w.sim.SwitchShip(*w.s, 0));
    CHECK(w.s->berths.at(other).place == w.sim.PlaceKeyHere(*w.s));
}

TEST_CASE("A second ship bought while docked arrives as the flown one, through the delta (#554)")
{
    World     w;
    const int other = OtherHull();
    REQUIRE(other != 0);

    Proto::SnapshotSender   out;
    Proto::SnapshotReceiver in;
    Proto::Snapshot         first, second;
    REQUIRE(in.Decode(out.Encode(w.sim.BuildSnapshot(*w.s, w.s->systemId), {}), first));
    out.Acknowledge(1);

    REQUIRE(w.sim.BuyShip(*w.s, other));
    REQUIRE(in.Decode(out.Encode(w.sim.BuildSnapshot(*w.s, w.s->systemId), {}), second));
    CHECK(second.player.shipIndex == other);
    REQUIRE(second.player.ownedShips.size() == 2);
    REQUIRE(second.player.shipDesigns.size() == 2);
    REQUIRE(second.player.shipHere.size() == 2);
    CHECK(second.player.shipDesigns[1] == GetShipCatalog()[other].stats.design);
    CHECK(second.player.maxHull == doctest::Approx(w.s->ship->GetMaxHull()));

    // The client's ship, reconciled to it, is that hull: its stats are the new design's.
    Ship       client(Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    Prediction prediction;
    prediction.Reconcile(client, second.player, nullptr);
    CHECK(client.GetStats().design == GetShipCatalog()[other].stats.design);
    CHECK(client.GetMaxHull() == doctest::Approx(second.player.maxHull));
}

TEST_CASE("A delivery on its way does not change the hull the client flies (#554)")
{
    // The shipments and the flown hull once shared a wire key: with one in transit, the
    // client read hull 0 and flew the starter's stats over another ship's hull.
    Proto::Snapshot s;
    s.systemId = "core";
    s.player.ownedShips = { 0, 2 };
    s.player.shipDesigns = { "scout", "hauler" };
    s.player.shipIndex = 2;
    Proto::ShipmentView sh;
    sh.from = "A";
    sh.to = "B";
    sh.hours = 2.0;
    s.player.shipments.push_back(sh);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), back));
    CHECK(back.player.shipIndex == 2);
    REQUIRE(back.player.shipments.size() == 1);
    CHECK(back.player.shipments[0].to == "B");
}

TEST_CASE("Several ships of one hull: each its own key, hold, fit and policy, and saved (#572)")
{
    World w;
    REQUIRE(w.s->ship->AddCargo(Good::Iron(), 3));
    // A second scout: the starter is key 0, this one a thousand more.
    REQUIRE(w.sim.BuyShip(*w.s, 0));
    const int second = ShipKey(0, 1);
    REQUIRE(w.s->Owns(0));
    REQUIRE(w.s->Owns(second));
    CHECK(w.s->Count(0) == 2);
    CHECK(w.s->currentShip == second);
    CHECK(w.s->ship->GetCargoAmount(Good::Iron()) == 0);  // its hold is its own
    CHECK(w.s->berths.at(0).hold.Amount(Good::Iron()) == 3);
    // A third takes the next free key, whichever of the two is flown.
    REQUIRE(w.sim.BuyShip(*w.s, 0));
    CHECK(w.s->Owns(ShipKey(0, 2)));

    // Switching names the ship, not the hull; insuring does too.
    REQUIRE(w.sim.SwitchShip(*w.s, 0));
    CHECK(w.s->currentShip == 0);
    CHECK(w.s->ship->GetCargoAmount(Good::Iron()) == 3);
    CHECK_FALSE(w.sim.SwitchShip(*w.s, ShipKey(0, 5)));  // not theirs

    // Lost: the one flown goes, the others stay; a starter that is lost is issued again.
    REQUIRE(w.sim.SwitchShip(*w.s, second));
    w.sim.ServerRespawnPlayer(*w.s);
    CHECK_FALSE(w.s->Owns(second));
    CHECK(w.s->Owns(0));
    CHECK(w.s->Owns(ShipKey(0, 2)));
    CHECK(w.s->currentShip == 0);

    // Saved and read back, with the account version that says what a key is.
    const TempDir     tmp;
    const std::string path = tmp.Path("hunter_two_scouts.json");
    REQUIRE(w.sim.SaveAccount(*w.s, path));
    ClientSession& again = w.sim.CreateSession(w.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                               GetShipCatalog()[0].stats);
    again.accountName = "hunter";
    REQUIRE(w.sim.LoadAccount(again, path) == Save::Result::Ok);
    CHECK(again.Count(0) == 2);
    CHECK(again.Owns(0));
    CHECK(again.Owns(ShipKey(0, 2)));
    CHECK(Save::ACCOUNT_VERSION >= 13);

    // A ship is named by its id, never by its place in the hangar list (#356).
    std::ifstream        in(path);
    const nlohmann::json saved = nlohmann::json::parse(in);
    REQUIRE(saved["ships"].size() == 2);
    CHECK(saved["ships"][0]["hull"] == GetShipCatalog()[0].stats.design);
    CHECK(saved["ships"][1]["hull"] == GetShipCatalog()[0].stats.design + "#2");
    CHECK(saved["ship"] == GetShipCatalog()[0].stats.design);
}

TEST_CASE("A ship id round-trips through the catalog, and an old index save still reads (#356)")
{
    int key = -1;
    REQUIRE(ShipKeyFromId(ShipIdOf(ShipKey(1, 0)), key));
    CHECK(key == ShipKey(1, 0));
    REQUIRE(ShipKeyFromId(ShipIdOf(ShipKey(1, 3)), key));
    CHECK(key == ShipKey(1, 3));
    CHECK_FALSE(ShipKeyFromId("no such design", key));
    CHECK_FALSE(ShipKeyFromId(GetShipCatalog()[0].stats.design + "#0", key));
    CHECK_FALSE(ShipKeyFromId(GetShipCatalog()[0].stats.design + "#x", key));

    World             w;
    const TempDir     tmp;
    const std::string path = tmp.Path("hunter_old.json");
    {
        std::ofstream out(path);
        out << R"({"version":13,"ships":[{"hull":0,"design":""},{"hull":1,"design":""}],"ship":1})";
    }
    ClientSession& s = w.sim.CreateSession(w.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                           GetShipCatalog()[0].stats);
    s.accountName = "hunter";
    REQUIRE(w.sim.LoadAccount(s, path) == Save::Result::Ok);
    CHECK(s.Owns(0));
    CHECK(s.Owns(1));
    CHECK(s.currentShip == 1);
}

TEST_CASE("The snapshot names ships by key, and the hangar lists each one (#572)")
{
    World w;
    REQUIRE(w.sim.BuyShip(*w.s, 0));
    const Proto::PlayerView p = w.sim.BuildSnapshot(*w.s, w.st->id).player;
    REQUIRE(p.ownedShips.size() == 2);
    CHECK(p.ownedShips[0] == 0);
    CHECK(p.ownedShips[1] == ShipKey(0, 1));
    CHECK(p.shipIndex == ShipKey(0, 1));
    CHECK(ShipHull(p.shipIndex) == 0);
}
