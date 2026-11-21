#include <doctest/doctest.h>

#include "factions_named.h"
#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include <memory>
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
