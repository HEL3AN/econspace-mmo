#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/Faction.h"
#include "entities/AsteroidField.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/Orders.h"
#include "sim/Simulation.h"

// The authoritative rules, tested where they live rather than through a socket.
//
// The smoke tests in econserver prove the loop runs end to end; these pin individual
// behaviours, which is what catches a rule quietly changing meaning. Everything here
// builds its own world in memory: no window, no server, no data files beyond the galaxy
// index and the faction table.
namespace
{

// A simulation with the real galaxy loaded and a player ship, but no NPCs -- so a test
// controls exactly what is in the system it is exercising.
struct Fixture
{
    Simulation sim;

    Fixture()
    {
        Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
        sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.Activate(sim.Universe().startId);
        sim.CreatePlayer(Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    }

    SystemState& World() { return sim.Active(); }
};

}  // namespace

TEST_CASE("the player only fires at a target in range, with the weapon on")
{
    Fixture f;
    auto    npc = std::make_unique<NpcShip>(Vector2{ 100.0f, 0.0f }, FactionId::Pirates,
                                            NpcRole::Pirate, std::vector<Vector2>{});
    npc->SetId(77);
    NpcShip* target = npc.get();
    f.World().entities.push_back(std::move(npc));

    Simulation::PlayerCombatEvents ev;
    const float                    dt = 1.0f / 60.0f;

    SUBCASE("weapon off means no shot, however close the target")
    {
        CHECK_FALSE(f.sim.StepPlayerFire(f.World(), 77, dt, &ev));
        CHECK(target->GetHull() == doctest::Approx(target->GetMaxHull()));
    }

    SUBCASE("weapon on and in range hits")
    {
        f.sim.ToggleWeapon();
        CHECK(f.sim.StepPlayerFire(f.World(), 77, dt, &ev));
        CHECK(target->GetHull() < target->GetMaxHull());
    }

    SUBCASE("out of range is not a hit")
    {
        f.sim.ToggleWeapon();
        target->SetPosition({ Sim::PLAYER_WEAPON_RANGE * 3.0f, 0.0f });
        CHECK_FALSE(f.sim.StepPlayerFire(f.World(), 77, dt, &ev));
        CHECK(target->GetHull() == doctest::Approx(target->GetMaxHull()));
    }

    SUBCASE("a cooldown separates shots")
    {
        f.sim.ToggleWeapon();
        REQUIRE(f.sim.StepPlayerFire(f.World(), 77, dt, &ev));
        // The very next tick is inside the cooldown, so the second shot must not land.
        CHECK_FALSE(f.sim.StepPlayerFire(f.World(), 77, dt, &ev));
    }

    SUBCASE("no target means no shot")
    {
        f.sim.ToggleWeapon();
        CHECK_FALSE(f.sim.StepPlayerFire(f.World(), 0, dt, &ev));
    }
}

TEST_CASE("mining fills the hold from a field in range")
{
    Fixture f;
    auto    field = std::make_unique<AsteroidField>(Vector2{ 20.0f, 0.0f }, 30.0f, "Belt",
                                                    AllResourceTypes()[0], 1000);
    field->SetId(55);
    f.World().entities.push_back(std::move(field));

    const float dt = 1.0f / 60.0f;

    SUBCASE("the mining module has to be on")
    {
        Simulation::PlayerMiningResult r = f.sim.StepPlayerMining(f.World(), 1.0f, dt);
        CHECK(r.fieldId == 0);
        CHECK(r.minedUnits == 0);
    }

    SUBCASE("with it on, ore accumulates into the hold")
    {
        f.sim.PlayerShip()->SetMiningOn(true);
        int mined = 0;
        for (int i = 0; i < 600; i++)  // ten simulated seconds
            mined += f.sim.StepPlayerMining(f.World(), 1.0f, dt).minedUnits;
        CHECK(mined > 0);
        CHECK(f.sim.PlayerShip()->GetCargoUsed() == mined);
    }

    SUBCASE("a full hold stops mining rather than losing the ore silently")
    {
        f.sim.PlayerShip()->SetMiningOn(true);
        for (int i = 0; i < 60000; i++)  // long enough to fill anything
            f.sim.StepPlayerMining(f.World(), 4.0f, dt);
        const int cap = f.sim.PlayerShip()->GetCargoCapacity();
        CHECK(f.sim.PlayerShip()->GetCargoUsed() == cap);
    }
}

TEST_CASE("docking is refused at range and granted up close")
{
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 5000.0f, 0.0f }, 60.0f, "Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(33);
    f.World().entities.push_back(std::move(station));

    CHECK(f.sim.StepPlayerDock(f.World()) == 0);  // far away
    CHECK_FALSE(f.sim.IsPlayerDocked());

    f.sim.PlayerShip()->Teleport({ 5000.0f, 0.0f });
    CHECK(f.sim.StepPlayerDock(f.World()) == 33);
    CHECK(f.sim.IsPlayerDocked());

    SUBCASE("undocking releases it and is recorded")
    {
        const int before = f.sim.LastEventSeq();
        f.sim.StepPlayerUndock();
        CHECK_FALSE(f.sim.IsPlayerDocked());
        CHECK(f.sim.EventsSince(before).size() == 1);
    }
}

TEST_CASE("a hostile reputation closes the station door")
{
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(34);
    f.World().entities.push_back(std::move(station));

    // Hated is the tier that refuses; Hostile still admits. Pinning both directions keeps
    // a threshold change from silently locking players out or letting everyone in.
    f.sim.Account().SetReputation(FactionId::TradersGuild, -100.0f);
    REQUIRE(Factions::TierOf(f.sim.Account().GetReputation(FactionId::TradersGuild)) ==
            RepTier::Hated);
    CHECK(f.sim.StepPlayerDock(f.World()) == 0);
    CHECK_FALSE(f.sim.IsPlayerDocked());

    f.sim.Account().SetReputation(FactionId::TradersGuild, 0.0f);
    CHECK(f.sim.StepPlayerDock(f.World()) == 34);
}

TEST_CASE("selling moves ore out of the hold and money into the account")
{
    Fixture            f;
    const ResourceType ore = AllResourceTypes()[0];
    f.sim.PlayerShip()->AddCargo(ore, 10);
    REQUIRE(f.sim.PlayerShip()->GetCargoAmount(ore) == 10);

    const double                 before = f.sim.Account().GetMoney();
    Simulation::PlayerSellResult r = f.sim.StepPlayerSell(f.World(), (int)ore, 10);

    CHECK(r.sold == 10);
    CHECK(r.gross > 0.0);
    CHECK(f.sim.PlayerShip()->GetCargoAmount(ore) == 0);
    CHECK(f.sim.Account().GetMoney() > before);

    SUBCASE("selling more than you carry sells what you have, not what you asked for")
    {
        f.sim.PlayerShip()->AddCargo(ore, 3);
        Simulation::PlayerSellResult over = f.sim.StepPlayerSell(f.World(), (int)ore, 999);
        CHECK(over.sold == 3);
        CHECK(f.sim.PlayerShip()->GetCargoAmount(ore) == 0);
    }
}

TEST_CASE("a route order across the galaxy is planned, not guessed")
{
    Fixture           f;
    const std::string start = f.sim.Universe().startId;

    for (const WorldLoader::SystemInfo& si : f.sim.Universe().systems)
    {
        std::vector<std::string> path = f.sim.PlanRoute(start, si.id, false);
        REQUIRE_FALSE(path.empty());
        CHECK(path.front() == start);
        CHECK(path.back() == si.id);
        // Consecutive hops must be genuinely linked, or the "route" is a list of wishes.
        for (size_t i = 1; i < path.size(); i++)
        {
            std::vector<std::string> nbrs = f.sim.Neighbors(path[i - 1]);
            CHECK(std::find(nbrs.begin(), nbrs.end(), path[i]) != nbrs.end());
        }
    }

    CHECK(f.sim.PlanRoute(start, "nowhere", false).empty());
    CHECK(f.sim.PlanRoute("nowhere", start, false).empty());
}

TEST_CASE("the world clock advances with maintenance")
{
    Fixture     f;
    const float dt = 1.0f / 60.0f;
    CHECK(f.sim.Time() == doctest::Approx(0.0));
    for (int i = 0; i < 120; i++)
        f.sim.MaintainWorld(dt, std::string(), nullptr);
    CHECK(f.sim.Time() == doctest::Approx(2.0));
}
