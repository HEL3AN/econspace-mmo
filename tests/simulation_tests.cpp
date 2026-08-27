#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Faction.h"
#include "entities/AsteroidField.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
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
    Simulation     sim;
    ClientSession& s;  // the one player these tests fly

    Fixture() : s(MakeSim()) {}

    // The world has to exist before a session can be put in it, and a reference member
    // must be bound in the initializer list -- so the setup lives here.
    ClientSession& MakeSim()
    {
        Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
        // Before InitGalaxy: entity constructors look themselves up in the registry, and
        // passes like docking ask for a component rather than for a class (#34).
        Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json");
        sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        return sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                 GetShipCatalog()[0].stats);
    }

    SystemState& World() { return *sim.SystemOf(s); }
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
        CHECK_FALSE(f.sim.StepPlayerFire(f.s, f.World(), 77, dt, &ev));
        CHECK(target->GetHull() == doctest::Approx(target->GetMaxHull()));
    }

    SUBCASE("weapon on and in range hits")
    {
        f.s.ToggleWeapon();
        CHECK(f.sim.StepPlayerFire(f.s, f.World(), 77, dt, &ev));
        CHECK(target->GetHull() < target->GetMaxHull());
    }

    SUBCASE("out of range is not a hit")
    {
        f.s.ToggleWeapon();
        target->SetPosition({ Sim::PLAYER_WEAPON_RANGE * 3.0f, 0.0f });
        CHECK_FALSE(f.sim.StepPlayerFire(f.s, f.World(), 77, dt, &ev));
        CHECK(target->GetHull() == doctest::Approx(target->GetMaxHull()));
    }

    SUBCASE("a cooldown separates shots")
    {
        f.s.ToggleWeapon();
        REQUIRE(f.sim.StepPlayerFire(f.s, f.World(), 77, dt, &ev));
        // The very next tick is inside the cooldown, so the second shot must not land.
        CHECK_FALSE(f.sim.StepPlayerFire(f.s, f.World(), 77, dt, &ev));
    }

    SUBCASE("no target means no shot")
    {
        f.s.ToggleWeapon();
        CHECK_FALSE(f.sim.StepPlayerFire(f.s, f.World(), 0, dt, &ev));
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
        Simulation::PlayerMiningResult r = f.sim.StepPlayerMining(f.s, f.World(), 1.0f, dt);
        CHECK(r.fieldId == 0);
        CHECK(r.minedUnits == 0);
    }

    SUBCASE("with it on, ore accumulates into the hold")
    {
        f.s.ship->SetMiningOn(true);
        int mined = 0;
        for (int i = 0; i < 600; i++)  // ten simulated seconds
            mined += f.sim.StepPlayerMining(f.s, f.World(), 1.0f, dt).minedUnits;
        CHECK(mined > 0);
        CHECK(f.s.ship->GetCargoUsed() == mined);
    }

    SUBCASE("a full hold stops mining rather than losing the ore silently")
    {
        f.s.ship->SetMiningOn(true);
        for (int i = 0; i < 60000; i++)  // long enough to fill anything
            f.sim.StepPlayerMining(f.s, f.World(), 4.0f, dt);
        const int cap = f.s.ship->GetCargoCapacity();
        CHECK(f.s.ship->GetCargoUsed() == cap);
    }
}

TEST_CASE("docking is refused at range and granted up close")
{
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 5000.0f, 0.0f }, 60.0f, "Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(33);
    f.World().entities.push_back(std::move(station));

    CHECK(f.sim.StepPlayerDock(f.s, f.World()) == 0);  // far away
    CHECK_FALSE(f.s.IsDocked());

    f.s.ship->Teleport({ 5000.0f, 0.0f });
    CHECK(f.sim.StepPlayerDock(f.s, f.World()) == 33);
    CHECK(f.s.IsDocked());

    SUBCASE("undocking releases it and is recorded")
    {
        const int before = f.s.LastEventSeq();
        f.sim.StepPlayerUndock(f.s);
        CHECK_FALSE(f.s.IsDocked());
        CHECK(f.s.EventsSince(before).size() == 1);
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
    f.s.account.SetReputation(FactionId::TradersGuild, -100.0f);
    REQUIRE(Factions::TierOf(f.s.account.GetReputation(FactionId::TradersGuild)) == RepTier::Hated);
    CHECK(f.sim.StepPlayerDock(f.s, f.World()) == 0);
    CHECK_FALSE(f.s.IsDocked());

    f.s.account.SetReputation(FactionId::TradersGuild, 0.0f);
    CHECK(f.sim.StepPlayerDock(f.s, f.World()) == 34);
}

TEST_CASE("selling moves ore out of the hold and money into the account")
{
    Fixture            f;
    const ResourceType ore = AllResourceTypes()[0];
    f.s.ship->AddCargo(ore, 10);
    REQUIRE(f.s.ship->GetCargoAmount(ore) == 10);

    const double                 before = f.s.account.GetMoney();
    Simulation::PlayerSellResult r = f.sim.StepPlayerSell(f.s, f.World(), (int)ore, 10);

    CHECK(r.sold == 10);
    CHECK(r.gross > 0.0);
    CHECK(f.s.ship->GetCargoAmount(ore) == 0);
    CHECK(f.s.account.GetMoney() > before);

    SUBCASE("selling more than you carry sells what you have, not what you asked for")
    {
        f.s.ship->AddCargo(ore, 3);
        Simulation::PlayerSellResult over = f.sim.StepPlayerSell(f.s, f.World(), (int)ore, 999);
        CHECK(over.sold == 3);
        CHECK(f.s.ship->GetCargoAmount(ore) == 0);
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
        f.sim.MaintainWorld(dt);
    CHECK(f.sim.Time() == doctest::Approx(2.0));
}

TEST_CASE("a dock order flies the ship in and docks it")
{
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 4000.0f, 0.0f }, 60.0f, "Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(42);
    f.World().entities.push_back(std::move(station));

    Orders::Order dock;
    dock.kind = Orders::Kind::Dock;
    dock.targetId = 42;
    REQUIRE(f.sim.GiveOrder(f.s, dock) > 0);

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60 * 120 && f.s.HasRunningOrder(); i++)  // two simulated minutes
        f.sim.StepPlayerOrder(f.s, f.World(), dt);

    // The failure this pins is the quiet one. The executor stops the ship at whatever
    // distance it believes a dock admits at; if that is further than the dock really
    // admits, the ship parks just outside the door and the order finishes having docked
    // nothing. Asking the dock for its own range is what keeps the two agreed.
    CHECK_FALSE(f.s.HasRunningOrder());
    CHECK(f.s.orderStatus == Orders::Status::Done);
    CHECK(f.s.IsDocked());
}

TEST_CASE("two players in one galaxy are two players")
{
    // The whole point of #3. Before sessions existed this test could not be written: a
    // second player would have flown the first one's ship and spent their money.
    Fixture        f;
    ClientSession& other = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 900.0f, 0.0f },
                                               GetShipCatalog()[0].stats);

    CHECK(other.id != f.s.id);
    CHECK(other.ship.get() != f.s.ship.get());

    SUBCASE("money is not shared")
    {
        const double before = other.account.GetMoney();
        f.s.account.AddMoney(1000.0);
        CHECK(other.account.GetMoney() == doctest::Approx(before));
    }

    SUBCASE("one player docking leaves the other in flight")
    {
        auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                 FactionId::TradersGuild, StationRole::TradeHub);
        station->SetId(91);
        f.World().entities.push_back(std::move(station));

        CHECK(f.sim.StepPlayerDock(f.s, f.World()) == 91);
        CHECK(f.s.IsDocked());
        CHECK_FALSE(other.IsDocked());  // 900 units away, and a different player besides
    }

    SUBCASE("the journal is per player, not per world")
    {
        f.s.RecordEvent(Ev::Kind::Notice, "something happened to me");
        CHECK(f.s.EventsSince(0).size() == 1);
        CHECK(other.EventsSince(0).empty());  // not news to anyone else
    }

    SUBCASE("each player is in a system of their own choosing")
    {
        // Two players in different systems is the case that made "the active system" a
        // property of the world untenable.
        const std::vector<std::string> nbrs = f.sim.Neighbors(f.s.systemId);
        REQUIRE_FALSE(nbrs.empty());
        other.systemId = nbrs[0];
        CHECK(f.sim.SystemOf(other) != f.sim.SystemOf(f.s));
        CHECK(f.sim.SystemOf(other) != nullptr);
    }
}
