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
#include "sim/Observation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"

#include <memory>
#include <string>

// The assets view (#472, slice 9): everything a pilot keeps, place by place, sent once on
// request -- hangars (the ones out of reach marked), parked ships with their holds, goods in
// transit.

namespace
{
const std::string DATA = TEST_DATA_DIR;
}

TEST_CASE("Assets: the places a pilot keeps things, once, when asked for (#472)")
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = "hunter";
    SystemState* st = sim.SystemOf(s);
    for (int i = 0; i < 2; i++)
    {
        auto station = std::make_unique<Station>(
            Vector2{ 0.0f, 0.0f }, 60.0f, i == 0 ? "Old Depot" : "Annex",
            i == 0 ? Fac::TradersGuild() : Fac::Syndicate(), StationRole::TradeHub);
        station->SetId(990001 + i);
        st->entities.push_back(std::move(station));
    }
    s.account.SetMoney(1.0e6);
    s.dockedStationId = 990001;
    REQUIRE(s.ship->AddCargo(Good::Iron(), 5));
    REQUIRE(sim.MoveGoods(s, "hold", "hangar", Good::Iron().Index(), 5) == 5);
    REQUIRE(sim.BuyShip(s, 1));  // the starter is parked here
    REQUIRE(s.ship->AddCargo(Good::Ice(), 2));

    // Nothing is sent until it is asked for.
    CHECK_FALSE(sim.BuildSnapshot(s, st->id).player.hasAssets);
    s.assetsDue = true;
    const Proto::PlayerView p = sim.BuildSnapshot(s, st->id).player;
    REQUIRE(p.hasAssets);
    CHECK_FALSE(sim.BuildSnapshot(s, st->id).player.hasAssets);  // once

    bool hangar = false, ship = false;
    for (const Proto::AssetView& a : p.assets)
    {
        if (a.kind == "hangar" && a.name.find("Old Depot") != std::string::npos)
        {
            hangar = true;
            CHECK(a.goods[(size_t)Good::Iron().Index()] == 5);
            CHECK_FALSE(a.locked);
            CHECK(a.system == st->id);
        }
        if (a.kind == "ship")
            ship = true;
    }
    CHECK(hangar);
    CHECK(ship);

    // A faction that now refuses the pilot: its hangar is marked out of reach.
    s.account.SetReputation(Fac::TradersGuild(), -100.0f);
    for (const Proto::AssetView& a : sim.AssetsFor(s))
        if (a.kind == "hangar")
            CHECK(a.locked);

    // The wire, and the agent's text.
    Proto::Command c;
    c.requestAssets = true;
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.requestAssets);
    s.assetsDue = true;
    Proto::Snapshot snap = sim.BuildSnapshot(s, st->id);
    Proto::Snapshot got;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), got));
    REQUIRE(got.player.hasAssets);
    CHECK(got.player.assets.size() == snap.player.assets.size());
    const std::string text = Obs::DescribeAssets(got.player.assets);
    CHECK(text.find("Old Depot") != std::string::npos);
    CHECK(text.find("out of reach") != std::string::npos);
}
