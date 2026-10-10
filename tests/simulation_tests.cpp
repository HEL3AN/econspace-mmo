#include <doctest/doctest.h>
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Orbits.h"
#include "entities/Planet.h"
#include "raymath.h"
#include "core/Faction.h"
#include "core/Archetype.h"
#include "entities/AsteroidField.h"
#include "gen/Region.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/SaveSchema.h"
#include "missions/Mission.h"
#include "missions/MissionSystem.h"
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

    // Docked at something that trades. The market is a component now, and asking for one
    // is what makes it mean anything (#34).
    auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                             FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(64);
    f.World().entities.push_back(std::move(station));
    REQUIRE(f.sim.StepPlayerDock(f.s, f.World()) == 64);

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

    SUBCASE("there is nobody to sell to in open space")
    {
        // The client only offers the trade screen while docked, so this was never
        // reachable by playing -- but it was reachable by asking, and the server is the
        // thing that decides.
        f.sim.StepPlayerUndock(f.s);
        f.s.ship->AddCargo(ore, 5);
        Simulation::PlayerSellResult adrift = f.sim.StepPlayerSell(f.s, f.World(), (int)ore, 5);
        CHECK(adrift.sold == 0);
        CHECK(f.s.ship->GetCargoAmount(ore) == 5);  // still aboard
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

TEST_CASE("a dock order across the system warps there, and docks once out of warp (#159)")
{
    // A station a third of the system away. At sublight that is a twenty-minute flight, so
    // an order that is not told to warp has to decide to; and a ship in warp sweeps
    // through the outer part of the dock's range before it drops out, where the dock
    // refuses it -- an order that acted the moment it was in range failed right there.
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 300000.0f, 0.0f }, 600.0f, "Far Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(43);
    f.World().entities.push_back(std::move(station));

    Orders::Order dock;
    dock.kind = Orders::Kind::Dock;
    dock.targetId = 43;
    REQUIRE_FALSE(dock.useWarp);
    REQUIRE(f.sim.GiveOrder(f.s, dock) > 0);

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60 * 60 && f.s.HasRunningOrder(); i++)  // one simulated minute
        f.sim.StepPlayerOrder(f.s, f.World(), dt);

    CHECK(f.s.orderStatus == Orders::Status::Done);
    CHECK(f.s.IsDocked());
}

TEST_CASE("a dock order catches a station that orbits a planet (#210)")
{
    // The station moves the whole time the ship is on its way: a warp aimed where it was
    // drops out where it no longer is, and the approach has to follow it by id. Before
    // that, the order sat out of range forever with its one nav command spent.
    Fixture f;
    // As fast as the generator makes a planet, and far enough out that the trip is a warp.
    f.World().entities.push_back(std::make_unique<Planet>(300000.0f, 52.0f, 0.0f, 15000.0f, WHITE,
                                                          ResourceType::Iron, PlanetType::Rocky));
    auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 600.0f, "Moon Dock",
                                             FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(44);
    Orbit o;
    o.planet = 0;
    o.radius = 30000.0f;
    o.speed = 50.0f;
    station->SetOrbit(o);
    f.World().entities.push_back(std::move(station));

    double      t = 0.0;
    const float dt = 1.0f / 60.0f;
    Orbits::Place(f.World().entities, t);

    Orders::Order dock;
    dock.kind = Orders::Kind::Dock;
    dock.targetId = 44;
    REQUIRE(f.sim.GiveOrder(f.s, dock) > 0);

    auto where = [&]()
    {
        for (const auto& e : f.World().entities)
            if (e->GetId() == 44)
                return e->GetPosition();
        return Vector2{ 0.0f, 0.0f };
    };
    const Vector2 start = where();
    for (int i = 0; i < 60 * 180 && f.s.HasRunningOrder(); i++)  // three simulated minutes
    {
        t += dt;
        Orbits::Place(f.World().entities, t);
        f.sim.StepPlayerOrder(f.s, f.World(), dt);
    }

    CHECK(Vector2Distance(start, where()) > 100.0f);  // it really did move
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

TEST_CASE("an account remembers where the player was, not just what they own")
{
    // Until a session per connection existed (#3) the one player never went away, so
    // losing this on disconnect was invisible. It is the first thing a returning player
    // notices now (#49).
    Fixture f;
    f.s.ship->Teleport({ 1234.0f, -567.0f });
    f.s.ship->SetHeading(1.25f);
    REQUIRE(f.s.ship->AddCargo(AllResourceTypes()[0], 7));

    Mission m;
    m.type = MissionType::Bounty;
    m.giverStationId = 33;  // by id, the way a mission survives a jump
    m.targetCount = 5;
    m.progress = 2;
    m.rewardMoney = 900.0;
    f.s.missions.SetMirror({}, { m });

    // Somewhere other than where a fresh session starts, so "restored" cannot pass by
    // accident.
    const std::vector<std::string> nbrs = f.sim.Neighbors(f.s.systemId);
    REQUIRE_FALSE(nbrs.empty());
    f.s.systemId = nbrs[0];

    const std::string path = "account_place_tmp.json";
    f.sim.SaveAccount(f.s, path);

    ClientSession& back = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                              GetShipCatalog()[0].stats);
    REQUIRE(f.sim.LoadAccount(back, path) == Save::Result::Ok);
    std::remove(path.c_str());

    CHECK(back.systemId == nbrs[0]);
    CHECK(back.ship->GetPosition().x == doctest::Approx(1234.0f));
    CHECK(back.ship->GetPosition().y == doctest::Approx(-567.0f));
    CHECK(back.ship->GetHeading() == doctest::Approx(1.25f));
    CHECK(back.ship->GetCargoAmount(AllResourceTypes()[0]) == 7);

    REQUIRE(back.missions.Active().size() == 1);
    CHECK(back.missions.Active()[0].giverStationId == 33);
    CHECK(back.missions.Active()[0].progress == 2);  // progress, not just the mission
    CHECK(back.missions.Offers().empty());           // the board belongs to a station

    SUBCASE("a system the galaxy no longer has leaves the player somewhere real")
    {
        // An edited universe or an older save. Trusting the name would put a player in a
        // system that does not exist, which is a crash rather than a lost position.
        const std::string bad = "account_bad_tmp.json";
        {
            std::ofstream out(bad);
            out << R"({"money":100,"place":{"system":"nowhere","pos":[9.0,9.0]}})";
        }
        ClientSession& lost = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                  GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(lost, bad) == Save::Result::Ok);
        std::remove(bad.c_str());
        CHECK(lost.systemId == f.sim.Universe().startId);
        CHECK(f.sim.SystemOf(lost) != nullptr);
        CHECK(lost.account.GetMoney() == doctest::Approx(100.0));  // the rest still loaded
    }
}

TEST_CASE("players in the same system are in each other's snapshots")
{
    Fixture        f;
    ClientSession& other =
        f.sim.CreateSession(f.s.systemId, Vector2{ 400.0f, 0.0f }, GetShipCatalog()[0].stats);
    other.ship->SetPilotName("bob");

    auto FindPlayer = [](const Proto::Snapshot& snap, int id) -> const Proto::EntitySnapshot*
    {
        for (const Proto::EntitySnapshot& es : snap.entities)
            if (es.id == id && es.kind == EntityKind::PlayerShip)
                return &es;
        return nullptr;
    };

    Proto::Snapshot              mine = f.sim.BuildSnapshot(f.s, f.s.systemId);
    const Proto::EntitySnapshot* seen = FindPlayer(mine, other.ship->GetId());
    REQUIRE(seen != nullptr);
    CHECK(seen->name == "bob");
    CHECK(seen->pos.x == doctest::Approx(400.0f));

    // Not oneself: the client predicts its own ship, and a proxy of it would fight the
    // prediction for the wheel.
    CHECK(FindPlayer(mine, f.s.ship->GetId()) == nullptr);

    SUBCASE("ids do not collide with the world's")
    {
        // Both come from the same counter, which is what lets a player be selected like
        // any other object. Two objects sharing an id would be one object to the client.
        for (const auto& e : f.World().entities)
            CHECK(e->GetId() != other.ship->GetId());
    }

    SUBCASE("a docked player is not in the sky")
    {
        auto station = std::make_unique<Station>(Vector2{ 400.0f, 0.0f }, 60.0f, "Depot",
                                                 FactionId::TradersGuild, StationRole::TradeHub);
        station->SetId(77);
        f.World().entities.push_back(std::move(station));
        REQUIRE(f.sim.StepPlayerDock(other, f.World()) == 77);

        Proto::Snapshot after = f.sim.BuildSnapshot(f.s, f.s.systemId);
        CHECK(FindPlayer(after, other.ship->GetId()) == nullptr);
    }

    SUBCASE("a player in another system is not visible")
    {
        const std::vector<std::string> nbrs = f.sim.Neighbors(f.s.systemId);
        REQUIRE_FALSE(nbrs.empty());
        other.systemId = nbrs[0];
        Proto::Snapshot elsewhere = f.sim.BuildSnapshot(f.s, f.s.systemId);
        CHECK(FindPlayer(elsewhere, other.ship->GetId()) == nullptr);

        // And from over there, the first player is the one out of sight.
        Proto::Snapshot theirs = f.sim.BuildSnapshot(other, other.systemId);
        CHECK(FindPlayer(theirs, f.s.ship->GetId()) == nullptr);
    }
}

TEST_CASE("a save from a newer build is refused, not read leniently")
{
    // Every field is read with a default, which is right for a message from a peer and
    // exactly wrong for a save (#20): a file written by a build that stores something
    // differently would load "successfully" as a plausible wrong account, and the next
    // checkpoint would write that back over the real one.
    Fixture           f;
    const std::string path = "account_version_tmp.json";

    f.s.account.SetMoney(9999.0);
    f.sim.SaveAccount(f.s, path);

    ClientSession& fresh = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                               GetShipCatalog()[0].stats);
    CHECK(f.sim.LoadAccount(fresh, path) == Save::Result::Ok);
    CHECK(fresh.account.GetMoney() == doctest::Approx(9999.0));

    SUBCASE("a version this build has never heard of")
    {
        std::string text;
        {
            std::ifstream in(path);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string now = "\"version\": " + std::to_string(Save::ACCOUNT_VERSION);
        REQUIRE(text.find(now) != std::string::npos);
        const std::string bumped =
            text.replace(text.find(now), now.size(),
                         "\"version\": " + std::to_string(Save::ACCOUNT_VERSION + 1));
        {
            std::ofstream out(path);
            out << bumped;
        }

        ClientSession& other = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                   GetShipCatalog()[0].stats);
        CHECK(f.sim.LoadAccount(other, path) == Save::Result::TooNew);
        // Nothing was taken from it -- not even the fields this build does understand.
        CHECK(other.account.GetMoney() != doctest::Approx(9999.0));
    }

    SUBCASE("a file written before versions existed is still readable")
    {
        // Those files are a strict subset of version 1, so refusing them would throw away
        // real progress to no purpose.
        {
            std::ofstream out(path);
            out << R"({"money":1234.0})";
        }
        ClientSession& old = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                 GetShipCatalog()[0].stats);
        CHECK(f.sim.LoadAccount(old, path) == Save::Result::Ok);
        CHECK(old.account.GetMoney() == doctest::Approx(1234.0));
    }

    SUBCASE("a position from before the system grew is not trusted (#159)")
    {
        // Version 1 positions are in a system forty times smaller. Everything else in the
        // file still means what it did; the place in the system does not.
        const std::string sys = f.sim.Universe().startId;
        {
            std::ofstream out(path);
            out << R"({"version":1,"money":1234.0,"place":{"system":")" << sys
                << R"(","pos":[0.0,3000.0],"heading":0}})";
        }
        ClientSession& old =
            f.sim.CreateSession(sys, Vector2{ 1.0f, 1.0f }, GetShipCatalog()[0].stats);
        CHECK(f.sim.LoadAccount(old, path) == Save::Result::Ok);
        CHECK(old.account.GetMoney() == doctest::Approx(1234.0));
        CHECK(old.systemId == sys);
        CHECK(old.ship->GetPosition().x == doctest::Approx(f.sim.SafeArrival(sys).x));
        CHECK(old.ship->GetPosition().y == doctest::Approx(f.sim.SafeArrival(sys).y));

        // ...and a current one is.
        {
            std::ofstream out(path);
            out << R"({"version":)" << Save::ACCOUNT_VERSION << R"(,"place":{"system":")" << sys
                << R"(","pos":[123.0,-456.0],"heading":0}})";
        }
        ClientSession& now =
            f.sim.CreateSession(sys, Vector2{ 1.0f, 1.0f }, GetShipCatalog()[0].stats);
        CHECK(f.sim.LoadAccount(now, path) == Save::Result::Ok);
        CHECK(now.ship->GetPosition().x == doctest::Approx(123.0f));
        CHECK(now.ship->GetPosition().y == doctest::Approx(-456.0f));
    }

    SUBCASE("a missing file and a corrupt one are told apart")
    {
        ClientSession& none = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                  GetShipCatalog()[0].stats);
        CHECK(f.sim.LoadAccount(none, "no_such_account_file.json") == Save::Result::Missing);
        {
            std::ofstream out(path);
            out << "{ not json";
        }
        CHECK(f.sim.LoadAccount(none, path) == Save::Result::Corrupt);
    }

    std::remove(path.c_str());
}

TEST_CASE("a ship has to be bought before it can be flown")
{
    Fixture f;
    REQUIRE(GetShipCatalog().size() > 1);
    const int better = 1;

    CHECK(f.s.Owns(0));  // the starter, and only the starter
    CHECK_FALSE(f.s.Owns(better));

    SUBCASE("switching to a ship you do not own is refused")
    {
        // This is the hole the issue closes, not just the persistence: the server used to
        // take a catalog index from the client and refit to it, so any ship was free.
        const float before = f.s.ship->GetStats().maxSpeed;
        CHECK_FALSE(f.sim.SwitchShip(f.s, better));
        CHECK(f.s.currentShip == 0);
        CHECK(f.s.ship->GetStats().maxSpeed == doctest::Approx(before));
    }

    SUBCASE("buying one records it and flies it")
    {
        f.s.account.SetMoney(GetShipCatalog()[better].price * 2.0);
        REQUIRE(f.sim.BuyShip(f.s, better));
        CHECK(f.s.Owns(better));
        CHECK(f.s.currentShip == better);

        // And switching back to the starter still works: you keep what you paid for.
        CHECK(f.sim.SwitchShip(f.s, 0));
        CHECK(f.s.currentShip == 0);
        CHECK(f.s.Owns(better));
    }

    SUBCASE("a ship already owned is not sold a second time (#109)")
    {
        f.s.account.SetMoney(GetShipCatalog()[better].price * 3.0);
        REQUIRE(f.sim.BuyShip(f.s, better));
        REQUIRE(f.sim.SwitchShip(f.s, 0));
        const double money = f.s.account.GetMoney();

        // Buying it again used to charge the full price and hand back what was already
        // in the hangar. Going back to it is SwitchShip's job, and it is free.
        CHECK_FALSE(f.sim.BuyShip(f.s, better));
        CHECK(f.s.account.GetMoney() == doctest::Approx(money));
        CHECK(f.s.currentShip == 0);
        CHECK(f.s.ownedShips.size() == 2);
    }

    SUBCASE("a hangar survives a reconnect")
    {
        f.s.account.SetMoney(GetShipCatalog()[better].price * 2.0);
        REQUIRE(f.sim.BuyShip(f.s, better));

        const std::string path = "account_ships_tmp.json";
        f.sim.SaveAccount(f.s, path);

        ClientSession& back = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                  GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(back, path) == Save::Result::Ok);
        std::remove(path.c_str());

        CHECK(back.Owns(better));
        CHECK(back.currentShip == better);
        // Flying it, not merely owning it: the stats have to come back too.
        CHECK(back.ship->GetStats().maxSpeed ==
              doctest::Approx(GetShipCatalog()[better].stats.maxSpeed));
    }

    SUBCASE("an account from before this existed owns the starter, not nothing")
    {
        const std::string path = "account_noships_tmp.json";
        {
            std::ofstream out(path);
            out << R"({"version":1,"money":700.0})";
        }
        ClientSession& old = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                 GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(old, path) == Save::Result::Ok);
        std::remove(path.c_str());
        CHECK(old.Owns(0));
        CHECK(old.currentShip == 0);
    }
}

namespace
{
// The newest journal entry since `seq`, or nothing -- what a client would flash for the
// command it just sent.
std::string NoticeSince(const ClientSession& s, int seq)
{
    const std::vector<Ev::Event> fresh = s.EventsSince(seq);
    if (fresh.empty() || fresh.back().kind != Ev::Kind::Notice)
        return std::string();
    return fresh.back().text;
}

int CatalogIndex(const char* name)
{
    for (size_t i = 0; i < GetShipCatalog().size(); i++)
        if (GetShipCatalog()[i].name == name)
            return (int)i;
    return -1;
}
}  // namespace

TEST_CASE("a hull too small for the cargo is refused, not overloaded (#219)")
{
    Fixture   f;
    const int hauler = CatalogIndex("Hauler");
    const int courier = CatalogIndex("Courier");
    REQUIRE(hauler >= 0);
    REQUIRE(courier >= 0);
    const ShipType&    scout = GetShipCatalog()[0];
    const ResourceType ore = AllResourceTypes()[0];

    f.s.account.SetMoney(GetShipCatalog()[hauler].price + GetShipCatalog()[courier].price);
    REQUIRE(f.sim.BuyShip(f.s, hauler));
    const int load = scout.stats.cargoCapacity * 3;
    REQUIRE(f.s.ship->AddCargo(ore, load));

    SUBCASE("switching to a smaller hold says why and changes nothing")
    {
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.SwitchShip(f.s, 0));
        CHECK(f.s.currentShip == hauler);
        CHECK(f.s.ship->GetCargoCapacity() == GetShipCatalog()[hauler].stats.cargoCapacity);
        CHECK(f.s.ship->GetCargoAmount(ore) == load);
        const std::string why = NoticeSince(f.s, seq);
        CHECK(why.find("refused") != std::string::npos);
        CHECK(why.find(std::to_string(load)) != std::string::npos);
    }

    SUBCASE("buying a smaller hold is refused before anything is charged")
    {
        const double money = f.s.account.GetMoney();
        const int    seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.BuyShip(f.s, courier));
        CHECK(f.s.account.GetMoney() == doctest::Approx(money));
        CHECK_FALSE(f.s.Owns(courier));
        CHECK(f.s.currentShip == hauler);
        CHECK(NoticeSince(f.s, seq).find("refused") != std::string::npos);
    }

    SUBCASE("once the cargo fits, the switch goes through")
    {
        f.s.ship->RemoveCargo(ore, load - scout.stats.cargoCapacity);
        const int seq = f.s.LastEventSeq();
        CHECK(f.sim.SwitchShip(f.s, 0));
        CHECK(f.s.currentShip == 0);
        CHECK(f.s.ship->GetCargoUsed() <= f.s.ship->GetCargoCapacity());
        CHECK(NoticeSince(f.s, seq).find(scout.name) != std::string::npos);
    }
}

TEST_CASE("station business answers in the journal, yes or no (#219)")
{
    Fixture         f;
    const FactionId guild = FactionId::TradersGuild;

    SUBCASE("a bounty that is not owed is not paid, and the player is told")
    {
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.PayBounty(f.s, guild));
        CHECK(NoticeSince(f.s, seq).find("no bounty") != std::string::npos);
    }

    SUBCASE("a bounty the player cannot afford stays, with the reason")
    {
        f.s.account.SetBounty(guild, 500.0);
        f.s.account.SetMoney(100.0);
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.PayBounty(f.s, guild));
        CHECK(f.s.account.GetBounty(guild) == doctest::Approx(500.0));
        CHECK(f.s.account.GetMoney() == doctest::Approx(100.0));
        CHECK(NoticeSince(f.s, seq).find("not paid") != std::string::npos);
    }

    SUBCASE("a bounty paid says so -- from the server, not the button")
    {
        f.s.account.SetBounty(guild, 500.0);
        f.s.account.SetMoney(800.0);
        const int seq = f.s.LastEventSeq();
        CHECK(f.sim.PayBounty(f.s, guild));
        CHECK(f.s.account.GetBounty(guild) == doctest::Approx(0.0));
        CHECK(f.s.account.GetMoney() == doctest::Approx(300.0));
        CHECK(NoticeSince(f.s, seq).find("Bounty paid") != std::string::npos);
    }

    SUBCASE("a ship the player cannot afford is refused with its price")
    {
        f.s.account.SetMoney(0.0);
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.BuyShip(f.s, 1));
        CHECK(NoticeSince(f.s, seq).find("costs") != std::string::npos);
    }

    SUBCASE("a hand-in that does not exist is refused aloud")
    {
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.CompleteMission(f.s, 0));
        CHECK(NoticeSince(f.s, seq).find("refused") != std::string::npos);
    }

    SUBCASE("an offer that is not on the board is not taken, aloud")
    {
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.AcceptMission(f.s, 0));
        CHECK(NoticeSince(f.s, seq).find("not taken") != std::string::npos);
    }
}

TEST_CASE("one player cannot take the whole board (#219)")
{
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(91);
    f.World().entities.push_back(std::move(station));

    // The board is regenerated on every dock, so docking again is how a player would hoard;
    // the test does the same.
    int guard = 0;
    while ((int)f.s.missions.Active().size() < MissionSystem::MAX_ACTIVE && guard++ < 50)
    {
        if (f.s.missions.Offers().empty())
        {
            f.sim.StepPlayerUndock(f.s);
            REQUIRE(f.sim.StepPlayerDock(f.s, f.World()) == 91);
        }
        const int seq = f.s.LastEventSeq();
        REQUIRE(f.sim.AcceptMission(f.s, 0));
        CHECK(NoticeSince(f.s, seq).find("Mission taken") != std::string::npos);
    }
    REQUIRE((int)f.s.missions.Active().size() == MissionSystem::MAX_ACTIVE);

    if (f.s.missions.Offers().empty())
    {
        f.sim.StepPlayerUndock(f.s);
        REQUIRE(f.sim.StepPlayerDock(f.s, f.World()) == 91);
    }
    const size_t offers = f.s.missions.Offers().size();
    REQUIRE(offers > 0);
    const int seq = f.s.LastEventSeq();
    CHECK_FALSE(f.sim.AcceptMission(f.s, 0));
    CHECK((int)f.s.missions.Active().size() == MissionSystem::MAX_ACTIVE);
    CHECK(f.s.missions.Offers().size() == offers);  // still on the board for someone else
    CHECK(NoticeSince(f.s, seq).find(std::to_string(MissionSystem::MAX_ACTIVE)) !=
          std::string::npos);
}

TEST_CASE("a worse standing is never a better deal (#219)")
{
    // Hated once fell through to list price: cheaper ships than Hostile, better sale prices,
    // richer missions. Only the dock refusing Hated players hid it.
    const RepTier tiers[] = { RepTier::Hated, RepTier::Hostile, RepTier::Neutral, RepTier::Liked,
                              RepTier::Allied };
    for (size_t i = 1; i < sizeof(tiers) / sizeof(tiers[0]); i++)
    {
        CAPTURE(i);
        CHECK(ShipPriceMultiplier(tiers[i - 1]) > ShipPriceMultiplier(tiers[i]));
        CHECK(SellPriceMultiplier(tiers[i - 1]) < SellPriceMultiplier(tiers[i]));
        CHECK(MissionRewardMultiplier(tiers[i - 1]) < MissionRewardMultiplier(tiers[i]));
    }
    CHECK(ShipPriceMultiplier(RepTier::Neutral) == doctest::Approx(1.0f));
    CHECK(SellPriceMultiplier(RepTier::Neutral) == doctest::Approx(1.0f));
    CHECK(MissionRewardMultiplier(RepTier::Neutral) == doctest::Approx(1.0f));
}

TEST_CASE("how far a thing can be used is the thing's business, not the code's")
{
    // Every one of these reaches used to be a literal in the pass that enforced it -- 40
    // for mining, 120 for salvage, 200 for a gate. They are archetype data now (#34), so
    // these tests read the range from the same place the simulation does: what is pinned
    // is the wiring, not the number, and a player-built object with its own reach (#44)
    // gets the same treatment without touching any of this.
    Fixture f;

    SUBCASE("a wreck is looted from the distance it declares")
    {
        Derelict probe({ 0.0f, 0.0f }, 40.0f, "Wreck", 500.0);
        REQUIRE(probe.GetArchetype() != nullptr);
        const float reach = probe.GetSize() + probe.GetArchetype()->salvageRange;

        const float x = 5000.0f;  // the ship starts at the origin, so x is the gap
        auto wreck = std::make_unique<Derelict>(Vector2{ x, 0.0f }, 40.0f, "Old Hauler", 500.0);
        wreck->SetId(71);
        f.World().entities.push_back(std::move(wreck));

        f.s.ship->Teleport({ x - (reach + 50.0f), 0.0f });
        CHECK(f.sim.StepPlayerLoot(f.s, f.World(), 71) == doctest::Approx(0.0));  // too far

        f.s.ship->Teleport({ x - (reach - 5.0f), 0.0f });
        CHECK(f.sim.StepPlayerLoot(f.s, f.World(), 71) == doctest::Approx(500.0));
        CHECK(f.sim.StepPlayerLoot(f.s, f.World(), 71) == doctest::Approx(0.0));  // once only
    }

    SUBCASE("a gate answers only from the distance it declares")
    {
        JumpGate probe({ 0.0f, 0.0f }, 150.0f, "Gate", "reach");
        REQUIRE(probe.GetArchetype() != nullptr);
        const float reach = probe.GetSize() + probe.GetArchetype()->jumpRange;

        const float x = 5000.0f;
        auto        gate =
            std::make_unique<JumpGate>(Vector2{ x, 0.0f }, 150.0f, "Gate to Sigma Reach", "reach");
        gate->SetId(72);
        f.World().entities.push_back(std::move(gate));

        f.s.ship->Teleport({ x - (reach + 100.0f), 0.0f });
        CHECK(f.sim.JumpGateDestIfNear(f.s, f.World(), 72).empty());
        f.s.ship->Teleport({ x - (reach - 10.0f), 0.0f });
        CHECK(f.sim.JumpGateDestIfNear(f.s, f.World(), 72) == "reach");
    }

    SUBCASE("a belt is mined from the distance it declares")
    {
        AsteroidField probe({ 0.0f, 0.0f }, 30.0f, "Belt", AllResourceTypes()[0], 1000);
        REQUIRE(probe.GetArchetype() != nullptr);
        const float reach = probe.GetSize() + probe.GetArchetype()->extractRange;

        const float x = 5000.0f;
        auto        belt = std::make_unique<AsteroidField>(Vector2{ x, 0.0f }, 30.0f, "Belt",
                                                           AllResourceTypes()[0], 1000);
        belt->SetId(73);
        f.World().entities.push_back(std::move(belt));
        f.s.ship->SetMiningOn(true);

        const float dt = 1.0f / 60.0f;
        int         mined = 0;
        f.s.ship->Teleport({ x - (reach + 100.0f), 0.0f });
        for (int i = 0; i < 600; i++)
            mined += f.sim.StepPlayerMining(f.s, f.World(), 1.0f, dt).minedUnits;
        CHECK(mined == 0);  // out of reach

        f.s.ship->Teleport({ x - (reach - 5.0f), 0.0f });
        for (int i = 0; i < 600; i++)
            mined += f.sim.StepPlayerMining(f.s, f.World(), 1.0f, dt).minedUnits;
        CHECK(mined > 0);
    }

    SUBCASE("an object without the component is not a target for the verb at all")
    {
        // A station is not salvage and does not lead anywhere, and neither pass should
        // need to know what a station is to say so.
        auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                 FactionId::TradersGuild, StationRole::TradeHub);
        station->SetId(74);
        f.World().entities.push_back(std::move(station));

        CHECK(f.sim.StepPlayerLoot(f.s, f.World(), 74) == doctest::Approx(0.0));
        CHECK(f.sim.JumpGateDestIfNear(f.s, f.World(), 74).empty());
    }
}

TEST_CASE("an order whose destination is already reached finishes at once")
{
    // The agent selftest flies to a station and, on a second run of the same account,
    // starts where it left off -- inside the stop distance it is about to ask for. An
    // order that neither moves nor finishes would leave it waiting for an event that
    // never comes.
    Fixture f;
    auto    station = std::make_unique<Station>(Vector2{ 100.0f, 0.0f }, 60.0f, "Depot",
                                                FactionId::TradersGuild, StationRole::TradeHub);
    station->SetId(88);
    f.World().entities.push_back(std::move(station));

    Orders::Order o;
    o.kind = Orders::Kind::MoveTo;
    o.targetId = 88;
    o.stopDist = 400.0f;  // the ship is at the origin, 100 units away: already there
    REQUIRE(f.sim.GiveOrder(f.s, o) > 0);

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120 && f.s.HasRunningOrder(); i++)
        f.sim.StepPlayerOrder(f.s, f.World(), dt);

    CHECK_FALSE(f.s.HasRunningOrder());
    CHECK(f.s.orderStatus == Orders::Status::Done);
    // And it says so in the journal, which is what an agent sleeps on.
    bool told = false;
    for (const Ev::Event& e : f.s.EventsSince(0))
        if (e.kind == Ev::Kind::OrderDone)
            told = true;
    CHECK(told);
}

// #157: orbit and keep-at-range are the two verbs every fight is flown with, and neither
// existed. They are standing behaviours -- they do not finish -- and they follow something
// that moves, which is why the step is told where the target is rather than being handed a
// point once.
TEST_CASE("a ship can hold station on something, and keeps holding it")
{
    Ship s({ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.SetStabilizerOn(true);

    Proto::Command hold;
    hold.navMode = 4;  // keep at range
    hold.navHoldId = 9;
    hold.navRange = 400.0f;

    const Vector2 target{ 2000.0f, 0.0f };
    Sim::StepPlayerShip(s, hold, 1.0f, Sim::SIM_DT, &target);
    REQUIRE(s.GetHoldMode() == HoldMode::Keep);
    REQUIRE(s.GetHoldTargetId() == 9);

    // Fly it. A hold is a control loop, so it is judged by where the ship ends up rather
    // than by any one tick.
    for (int i = 0; i < 60 * 90; i++)
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);

    const float dx = s.GetPosition().x - target.x, dy = s.GetPosition().y - target.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    CHECK(dist == doctest::Approx(400.0f).epsilon(0.25));

    SUBCASE("and it is still holding a minute later, because it is standing and not one-shot")
    {
        for (int i = 0; i < 60 * 60; i++)
            Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);
        CHECK(s.GetHoldMode() == HoldMode::Keep);
        const float ax = s.GetPosition().x - target.x, ay = s.GetPosition().y - target.y;
        CHECK(std::sqrt(ax * ax + ay * ay) == doctest::Approx(400.0f).epsilon(0.3));
    }

    SUBCASE("touching the controls releases it -- nothing else ever would")
    {
        Proto::Command manual;
        manual.thrust = true;
        Sim::StepPlayerShip(s, manual, 1.0f, Sim::SIM_DT, &target);
        CHECK(s.GetHoldMode() == HoldMode::None);
    }

    SUBCASE("with no target position it stops steering rather than flying at a stale point")
    {
        const Vector2 before = s.GetPosition();
        for (int i = 0; i < 30; i++)
            Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, nullptr);
        // It coasts, but it is not being aimed anywhere; the hold is still set so that the
        // server can release it when it works out the target is gone.
        CHECK(s.GetHoldMode() == HoldMode::Keep);
        (void)before;
    }
}

TEST_CASE("an orbit goes round rather than parking on the ring")
{
    Ship s({ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.SetStabilizerOn(true);

    Proto::Command orbit;
    orbit.navMode = 3;
    orbit.navHoldId = 1;
    orbit.navRange = 500.0f;

    const Vector2 target{ 1500.0f, 0.0f };
    Sim::StepPlayerShip(s, orbit, 1.0f, Sim::SIM_DT, &target);
    for (int i = 0; i < 60 * 60; i++)
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);

    const Vector2 a = s.GetPosition();
    const float   ax = a.x - target.x, ay = a.y - target.y;
    CHECK(std::sqrt(ax * ax + ay * ay) == doctest::Approx(500.0f).epsilon(0.3));

    // Ten seconds later it is somewhere else on the same ring. Aiming at the ring rather
    // than ahead of the ship on it would have parked it.
    for (int i = 0; i < 60 * 10; i++)
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);
    const Vector2 b = s.GetPosition();
    const float   bx = b.x - target.x, by = b.y - target.y;
    CHECK(std::sqrt(bx * bx + by * by) == doctest::Approx(500.0f).epsilon(0.3));

    const float moved = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    CHECK(moved > 100.0f);
}

TEST_CASE("the region hangs off the start system by a wormhole, and its seed is saved (#140)")
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";

    Simulation sim;
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    const size_t handWritten = sim.Universe().systems.size();
    sim.AttachRegion(424242, systems);
    REQUIRE(sim.HasRegion());
    CHECK(sim.Universe().systems.size() > handWritten);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(systems);

    // The start system keeps everything it was written with, plus the wormhole.
    const std::string  start = sim.Universe().startId;
    const SystemState* home = sim.SystemById(start);
    REQUIRE(home != nullptr);
    std::string entry;
    for (const auto& e : home->entities)
        if (e->GetKind() == EntityKind::Gate && e->GetName() == "Wormhole")
            entry = static_cast<JumpGate*>(e.get())->GetDestination();
    REQUIRE_FALSE(entry.empty());

    // And the far side leads back.
    const SystemState* far = sim.SystemById(entry);
    REQUIRE(far != nullptr);
    bool back = false;
    for (const auto& e : far->entities)
        if (e->GetKind() == EntityKind::Gate &&
            static_cast<JumpGate*>(e.get())->GetDestination() == start)
            back = true;
    CHECK(back);

    // A system out there has no station to arrive beside, and the arrival must still not be
    // inside its star.
    for (const auto& info : sim.Universe().systems)
    {
        const Vector2 at = sim.SafeArrival(info.id);
        for (const auto& e : sim.SystemById(info.id)->entities)
            if (e->GetKind() == EntityKind::Star)
            {
                CAPTURE(info.id);
                CHECK(std::sqrt(at.x * at.x + at.y * at.y) > e->GetSize());
            }
    }

    // The region is not saved, only what remakes it.
    const std::string path = "world_seed_tmp.json";
    sim.SaveWorld(path);
    uint64_t seed = 0;
    int      rules = 0;
    REQUIRE(Simulation::ReadWorldSeed(path, seed, rules));
    CHECK(seed == 424242u);
    CHECK(rules == Gen::GENERATOR_VERSION);
    std::remove(path.c_str());
}

TEST_CASE("the region is nobody's to take until somebody has been there (#143)")
{
    // Seen with the first generated region: within seconds of a start, the macro model
    // handed most of the region to the pirates and the news opened with eight seizures --
    // a world that had run its course before any player arrived.
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";

    Simulation sim;
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    const size_t handWritten = sim.Universe().systems.size();
    sim.AttachRegion(7, systems);  // the seed it was seen with
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(systems);

    std::map<std::string, FactionId>   before;
    std::map<std::string, std::string> names;
    for (size_t i = handWritten; i < sim.Universe().systems.size(); i++)
    {
        const std::string& id = sim.Universe().systems[i].id;
        names[id] = sim.Universe().systems[i].name;
        CHECK_FALSE(sim.SystemById(id)->agg.visited);
        before[id] = sim.SystemById(id)->agg.controller;
    }
    REQUIRE_FALSE(before.empty());
    CHECK(sim.SystemById(sim.Universe().startId)->agg.visited);  // known space is known

    for (int i = 0; i < 60 * 300; i++)  // five simulated minutes of maintenance
        sim.MaintainWorld(1.0f / 60.0f);

    for (const auto& kv : before)
    {
        CAPTURE(kv.first);
        CHECK(sim.SystemById(kv.first)->agg.controller == kv.second);
    }
    // Known space is contested from the start -- Tau Verge, at 0.3, can fall -- so only
    // the region's names are checked here.
    for (const std::string& e : sim.Events())
        for (const auto& kv : before)
        {
            CAPTURE(e);
            CHECK(e.find("seized " + names[kv.first]) == std::string::npos);
        }

    // The first ship in makes it part of the contested world, and that is news.
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    const std::string entry = before.begin()->first;
    sim.ServerEnterSystem(s, entry, sim.Universe().startId);
    CHECK(sim.SystemById(entry)->agg.visited);
    REQUIRE_FALSE(sim.Events().empty());
    CHECK(sim.Events().back().find("First ship into") != std::string::npos);
}

TEST_CASE("players are told only what somebody has charted (#144)")
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";

    Simulation sim;
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    const size_t handWritten = sim.Universe().systems.size();
    sim.AttachRegion(7, systems);
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(systems);

    auto find = [](const WorldLoader::Universe& u, const std::string& id)
    {
        for (const auto& si : u.systems)
            if (si.id == id)
                return &si;
        return static_cast<const WorldLoader::SystemInfo*>(nullptr);
    };
    std::string entry;  // the far side of the wormhole
    for (const auto& l : sim.Universe().links)
        if (l.a == sim.Universe().startId && l.b.rfind("w1-", 0) == 0)
            entry = l.b;
        else if (l.b == sim.Universe().startId && l.a.rfind("w1-", 0) == 0)
            entry = l.a;
    REQUIRE_FALSE(entry.empty());

    // Before anyone goes through: known space, and one uncharted dot at the wormhole.
    WorldLoader::Universe known = sim.KnownUniverse();
    CHECK(known.systems.size() == handWritten + 1);
    REQUIRE(find(known, entry) != nullptr);
    CHECK_FALSE(find(known, entry)->charted);
    CHECK(find(known, entry)->security == 0.0f);  // nothing about it but where it is
    for (const auto& l : known.links)
        CHECK((find(known, l.a) != nullptr && find(known, l.b) != nullptr));
    CHECK_FALSE(sim.TakeChartsChanged());
    for (const auto& g : sim.BuildGalaxyState().systems)
        CHECK(g.id != entry);

    // The first ship through charts it, for everyone, and shows what its gates lead to.
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    sim.ServerEnterSystem(s, entry, sim.Universe().startId);
    CHECK(sim.TakeChartsChanged());
    CHECK_FALSE(sim.TakeChartsChanged());  // taken once
    known = sim.KnownUniverse();
    CHECK(find(known, entry)->charted);
    CHECK(known.systems.size() > handWritten + 1);
    for (const auto& l : sim.Universe().links)
        if (l.a == entry || l.b == entry)
        {
            const std::string other = l.a == entry ? l.b : l.a;
            CAPTURE(other);
            CHECK(find(known, other) != nullptr);
        }

    // And the wire carries the difference.
    WorldLoader::Universe decoded;
    REQUIRE(Proto::DecodeUniverse(Proto::EncodeUniverse(known), decoded));
    size_t uncharted = 0;
    for (const auto& si : decoded.systems)
        uncharted += si.charted ? 0 : 1;
    CHECK(uncharted > 0);
}

TEST_CASE("a richer deposit gives up its ore faster (#193)")
{
    // The ship sets the pace and the deposit how rich it is: the same ship on the same
    // skill takes half as much again from a motherlode as from an ordinary belt.
    auto mineFor = [](const char* archetype)
    {
        Fixture f;
        auto    belt = std::make_unique<AsteroidField>(Vector2{ 20.0f, 0.0f }, 30.0f, "Belt",
                                                       AllResourceTypes()[0], 1000);
        belt->SetArchetype(archetype);
        belt->SetId(56);
        f.World().entities.push_back(std::move(belt));
        f.s.ship->SetMiningOn(true);
        int mined = 0;
        for (int i = 0; i < 600; i++)  // ten simulated seconds, short of a full hold
            mined += f.sim.StepPlayerMining(f.s, f.World(), 1.0f, 1.0f / 60.0f).minedUnits;
        return mined;
    };

    const int fromBelt = mineFor("field.asteroid");
    const int fromLode = mineFor("field.motherlode");

    // Looked up after the runs: every Fixture reloads the registry, which would leave a
    // pointer taken before it dangling.
    const Archetype* ordinary = Archetypes::Find("field.asteroid");
    const Archetype* rich = Archetypes::Find("field.motherlode");
    REQUIRE(ordinary != nullptr);
    REQUIRE(rich != nullptr);
    REQUIRE(rich->extractRate > ordinary->extractRate);
    REQUIRE(fromBelt > 0);
    CHECK(fromLode > fromBelt);
    CHECK((float)fromLode / (float)fromBelt ==
          doctest::Approx(rich->extractRate / ordinary->extractRate).epsilon(0.1));
}

TEST_CASE("a defensive station fires on hostiles near it, and only on them (#193)")
{
    Fixture          f;
    const Archetype* mil = Archetypes::Find("station.military");
    REQUIRE(mil != nullptr);
    REQUIRE(mil->Has(Component::Defensive));
    REQUIRE(mil->weaponDamage > 0.0f);

    const Vector2 at{ 40000.0f, 0.0f };  // clear of anything the start system holds
    auto          station = std::make_unique<Station>(at, 600.0f, "Bastion", FactionId::Independent,
                                                      StationRole::Military);
    station->SetId(90);
    Station* bastion = station.get();
    f.World().entities.push_back(std::move(station));
    const float reach = bastion->GetSize() + mil->weaponRange;

    auto addNpc = [&](float x, FactionId faction, NpcRole role, int id)
    {
        auto npc = std::make_unique<NpcShip>(Vector2{ at.x + x, at.y }, faction, role,
                                             std::vector<Vector2>{});
        npc->SetId(id);
        NpcShip* raw = npc.get();
        f.World().entities.push_back(std::move(npc));
        return raw;
    };

    std::vector<FireEvent>                  fires;
    std::vector<Simulation::PlayerPresence> nobody;
    const float                             dt = 1.0f / 60.0f;

    SUBCASE("a pirate in range is hit, once a second")
    {
        NpcShip* pirate = addNpc(reach - 50.0f, FactionId::Pirates, NpcRole::Pirate, 91);
        for (int i = 0; i < 60; i++)  // one second: exactly one shot
            f.sim.StepStationDefence(f.World(), nobody, &fires, dt);
        CHECK(fires.size() == 1);
        CHECK(pirate->GetHull() == doctest::Approx(pirate->GetMaxHull() - mil->weaponDamage));
        REQUIRE_FALSE(fires.empty());
        CHECK(fires[0].shooterFaction == FactionId::Independent);
        CHECK(fires[0].targetSessionId == 0);
    }

    SUBCASE("a pirate out of range is not")
    {
        NpcShip* pirate = addNpc(reach + 50.0f, FactionId::Pirates, NpcRole::Pirate, 91);
        for (int i = 0; i < 120; i++)
            f.sim.StepStationDefence(f.World(), nobody, &fires, dt);
        CHECK(fires.empty());
        CHECK(pirate->GetHull() == doctest::Approx(pirate->GetMaxHull()));
    }

    SUBCASE("a friendly trader alongside is left alone")
    {
        NpcShip* trader = addNpc(100.0f, FactionId::TradersGuild, NpcRole::Trader, 92);
        for (int i = 0; i < 120; i++)
            f.sim.StepStationDefence(f.World(), nobody, &fires, dt);
        CHECK(fires.empty());
        CHECK(trader->GetHull() == doctest::Approx(trader->GetMaxHull()));
    }

    SUBCASE("a player is judged by their own account")
    {
        f.s.ship->Teleport({ at.x + reach - 50.0f, at.y });
        Simulation::PlayerPresence p;
        p.session = &f.s;
        p.ship = f.s.ship.get();
        std::vector<Simulation::PlayerPresence> players{ p };

        for (int i = 0; i < 120; i++)  // in good standing: not a target
            f.sim.StepStationDefence(f.World(), players, &fires, dt);
        CHECK(fires.empty());

        f.s.account.AddBounty(FactionId::Independent, 100.0);  // wanted by the owner
        for (int i = 0; i < 60; i++)
            f.sim.StepStationDefence(f.World(), players, &fires, dt);
        REQUIRE(fires.size() == 1);
        CHECK(fires[0].targetSessionId == f.s.id);
        // Shields take the first hits.
        CHECK(f.s.ship->GetShields() + f.s.ship->GetHull() ==
              doctest::Approx(f.s.ship->GetMaxShields() + f.s.ship->GetMaxHull() -
                              mil->weaponDamage));

        // Hidden in cover, the same wanted player is not seen.
        fires.clear();
        const float hull = f.s.ship->GetShields() + f.s.ship->GetHull();
        players[0].hidden = true;
        for (int i = 0; i < 120; i++)
            f.sim.StepStationDefence(f.World(), players, &fires, dt);
        CHECK(fires.empty());
        CHECK(f.s.ship->GetShields() + f.s.ship->GetHull() == doctest::Approx(hull));
    }

    SUBCASE("a station without the component never fires")
    {
        auto hub = std::make_unique<Station>(Vector2{ -40000.0f, 0.0f }, 600.0f, "Hub",
                                             FactionId::Independent, StationRole::TradeHub);
        hub->SetId(93);
        REQUIRE_FALSE(hub->Has(Component::Defensive));
        f.World().entities.push_back(std::move(hub));
        auto npc =
            std::make_unique<NpcShip>(Vector2{ -40000.0f + 700.0f, 0.0f }, FactionId::Pirates,
                                      NpcRole::Pirate, std::vector<Vector2>{});
        npc->SetId(94);
        NpcShip* pirate = npc.get();
        f.World().entities.push_back(std::move(npc));
        // The bastion is still there, eighty thousand units away, out of its own reach.
        for (int i = 0; i < 120; i++)
            f.sim.StepStationDefence(f.World(), nobody, &fires, dt);
        CHECK(fires.empty());
        CHECK(pirate->GetHull() == doctest::Approx(pirate->GetMaxHull()));
    }
}

TEST_CASE("a system changes hands only after the balance has held for a while (#225)")
{
    // Tau Verge, at security 0.3, fell to the pirates ten seconds into a fresh world.
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";
    Simulation        sim;
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(systems);
    REQUIRE(sim.SystemById("verge") != nullptr);
    const FactionId start = sim.SystemById("verge")->agg.controller;
    REQUIRE(start != FactionId::Pirates);

    int fell = -1;
    for (int s = 1; s <= 20 * 60 && fell < 0; s++)  // up to twenty simulated minutes
    {
        for (int i = 0; i < 60; i++)
            sim.MaintainWorld(1.0f / 60.0f);
        if (sim.SystemById("verge")->agg.controller == FactionId::Pirates)
            fell = s;
    }
    MESSAGE("Tau Verge fell after " << fell << " s");
    CHECK((fell < 0 || fell >= 180));  // not before three minutes of holding
}
