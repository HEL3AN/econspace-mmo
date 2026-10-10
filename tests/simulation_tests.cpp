#include <doctest/doctest.h>
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Orbits.h"
#include "entities/Planet.h"
#include "raymath.h"
#include "core/Faction.h"
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
#include "net/Transport.h"
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
        Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json");
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

    // A mission is handed in at a station the galaxy has, or it is dropped on load (#227).
    auto hub = std::make_unique<Station>(Vector2{ 9000.0f, 0.0f }, 700.0f, "Aurora Hub",
                                         FactionId::TradersGuild, StationRole::TradeHub);
    hub->SetId(33);
    f.World().entities.push_back(std::move(hub));

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

    const Sim::HoldTarget target{ { 2000.0f, 0.0f } };
    Sim::StepPlayerShip(s, hold, 1.0f, Sim::SIM_DT, &target);
    REQUIRE(s.GetHoldMode() == HoldMode::Keep);
    REQUIRE(s.GetHoldTargetId() == 9);

    // Fly it. A hold is a control loop, so it is judged by where the ship ends up rather
    // than by any one tick.
    for (int i = 0; i < 60 * 90; i++)
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);

    const float dx = s.GetPosition().x - target.pos.x, dy = s.GetPosition().y - target.pos.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    CHECK(dist == doctest::Approx(400.0f).epsilon(0.25));

    SUBCASE("and it is still holding a minute later, because it is standing and not one-shot")
    {
        for (int i = 0; i < 60 * 60; i++)
            Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);
        CHECK(s.GetHoldMode() == HoldMode::Keep);
        const float ax = s.GetPosition().x - target.pos.x, ay = s.GetPosition().y - target.pos.y;
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

    const Sim::HoldTarget target{ { 1500.0f, 0.0f } };
    Sim::StepPlayerShip(s, orbit, 1.0f, Sim::SIM_DT, &target);
    for (int i = 0; i < 60 * 60; i++)
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);

    const Vector2 a = s.GetPosition();
    const float   ax = a.x - target.pos.x, ay = a.y - target.pos.y;
    CHECK(std::sqrt(ax * ax + ay * ay) == doctest::Approx(500.0f).epsilon(0.05));

    // Ten seconds later it is somewhere else on the same ring. Aiming at the ring rather
    // than ahead of the ship on it would have parked it.
    for (int i = 0; i < 60 * 10; i++)
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);
    const Vector2 b = s.GetPosition();
    const float   bx = b.x - target.pos.x, by = b.y - target.pos.y;
    CHECK(std::sqrt(bx * bx + by * by) == doctest::Approx(500.0f).epsilon(0.05));

    const float moved = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    CHECK(moved > 100.0f);
}

// --- Flight near moving stations (#298) ---

TEST_CASE("orbit and keep-at-range hold the distance asked for, not only the presets (#298)")
{
    // The context menu offers a few multiples of the target's size and a slider beside them;
    // an agent passes any number. Whatever the number, the ring is that far out.
    for (const float range : { 777.0f, 1234.0f })
    {
        for (const int mode : { 3, 4 })
        {
            Ship s({ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
            s.SetStabilizerOn(true);
            Proto::Command hold;
            hold.navMode = mode;
            hold.navHoldId = 1;
            hold.navRange = range;
            const Sim::HoldTarget target{ { 3000.0f, 500.0f } };
            Sim::StepPlayerShip(s, hold, 1.0f, Sim::SIM_DT, &target);
            for (int i = 0; i < 60 * 60; i++)
                Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);
            CAPTURE(range);
            CAPTURE(mode);
            CHECK(s.GetHoldRange() == range);
            CHECK(Vector2Distance(s.GetPosition(), target.pos) ==
                  doctest::Approx(range).epsilon(mode == 3 ? 0.05 : 0.1));
        }
    }
}

namespace
{
// A station going round a planet that itself goes round the star -- the case #298 is
// about -- with its position and velocity at a time, the way the server places it.
struct MovingStation
{
    float   radius = 30000.0f, speed = 50.0f;
    Vector2 At(double t) const
    {
        const double a = (double)speed / radius * t;
        return { 200000.0f + (float)std::cos(a) * radius, (float)std::sin(a) * radius };
    }
    Vector2 VelAt(double t) const
    {
        const double a = (double)speed / radius * t;
        return { -(float)std::sin(a) * speed, (float)std::cos(a) * speed };
    }
};
}  // namespace

TEST_CASE("a follow stays with a target that moves, at the range and at its speed (#298)")
{
    const MovingStation st;
    Ship                s({ st.At(0).x - 900.0f, st.At(0).y + 300.0f }, GetShipCatalog()[0].stats);
    s.SetStabilizerOn(true);

    Proto::Command follow;
    follow.navMode = 5;
    follow.navHoldId = 7;
    follow.navRange = 600.0f;

    double          t = 0.0;
    Sim::HoldTarget target{ st.At(t), st.VelAt(t) };
    Sim::StepPlayerShip(s, follow, 1.0f, Sim::SIM_DT, &target);
    REQUIRE(s.GetHoldMode() == HoldMode::Follow);

    // Settle, then watch for a minute: a tether is judged by how it holds, not by one tick.
    float worst = 0.0f, worstSpeed = 0.0f;
    for (int i = 0; i < 60 * 120; i++)
    {
        t += Sim::SIM_DT;
        target = { st.At(t), st.VelAt(t) };
        Sim::StepPlayerShip(s, Proto::Command{}, 1.0f, Sim::SIM_DT, &target);
        if (i >= 60 * 60)
        {
            worst =
                std::max(worst, std::fabs(Vector2Distance(s.GetPosition(), target.pos) - 600.0f));
            worstSpeed = std::max(worstSpeed, Vector2Distance(s.GetVelocity(), target.vel));
        }
    }
    CHECK(worst < 30.0f);                        // on the ring...
    CHECK(worstSpeed < 5.0f);                    // ...and moving with it, not chasing it
    CHECK(s.GetHoldMode() == HoldMode::Follow);  // standing: still running
}

namespace
{
// A planet far out and a station going round it, as the generator makes them (#210). The
// station's id is 44; the fixture's clock starts at zero.
Entity* AddOrbitingStation(Fixture& f)
{
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
    Entity* e = station.get();
    f.World().entities.push_back(std::move(station));
    Orbits::Place(f.World().entities, 0.0);
    return e;
}
}  // namespace

TEST_CASE("a docked ship goes round with its station, and undocks beside where it is now (#298)")
{
    Fixture       f;
    Entity* const station = AddOrbitingStation(f);
    const Vector2 at = station->GetPosition();
    f.s.ship->Teleport({ at.x + 700.0f, at.y });  // inside the door, off to one side
    REQUIRE(f.sim.StepPlayerDock(f.s, f.World()) == 44);

    // Berthed just clear of the hull on the side it came in from.
    const float berth = station->GetSize() + Sim::DOCK_BERTH_CLEARANCE;
    CHECK(Vector2Distance(f.s.ship->GetPosition(), at) == doctest::Approx(berth));

    // Two minutes of the world turning, the way the host steps it: the world moves, then
    // whatever the ship is attached to is applied.
    double      t = 0.0;
    const float dt = Sim::SIM_DT;
    for (int i = 0; i < 60 * 120; i++)
    {
        t += dt;
        Orbits::Place(f.World().entities, t);
        f.sim.StepPlayerAttachment(f.s, dt);
        if (i % 600 == 0)
        {
            CAPTURE(i);
            CHECK(Vector2Distance(f.s.ship->GetPosition(), station->GetPosition()) ==
                  doctest::Approx(berth));
        }
    }
    const Vector2 now = station->GetPosition();
    REQUIRE(Vector2Distance(at, now) > 5000.0f);  // it really did go a long way
    CHECK(Vector2Distance(f.s.ship->GetPosition(),
                          Sim::DockBerth(now, station->GetSize(), f.s.dockBearing)) < 0.01f);
    // Carried, so it reports the station's speed rather than standing still.
    CHECK(f.s.ship->GetSpeed() > 50.0f);

    // What a client is told is where the ship is: the snapshot carries the berth. A client
    // that docked does not predict -- it shows this -- so there is nothing to disagree with.
    const Proto::Snapshot snap = f.sim.BuildSnapshot(f.s, f.s.systemId);
    CHECK(snap.player.docked);
    CHECK(Vector2Distance(snap.player.pos, f.s.ship->GetPosition()) < 0.1f);

    // Undocking puts it beside where the station is now, at rest, close enough to dock
    // again -- not where it docked, which the station left minutes ago.
    f.sim.StepPlayerUndock(f.s);
    CHECK_FALSE(f.s.IsDocked());
    CHECK(Vector2Distance(f.s.ship->GetPosition(), now) == doctest::Approx(berth));
    CHECK(Vector2Distance(f.s.ship->GetPosition(), at) > 5000.0f);
    CHECK(f.s.ship->GetSpeed() == 0.0f);
    CHECK(Vector2Distance(f.s.ship->GetPosition(), now) <=
          station->GetSize() + station->GetArchetype()->dockRange);

    // And from there, the server's step and the client's prediction are one function of
    // the same input: they fly off identically.
    Ship client = *f.s.ship;
    for (int i = 0; i < 120; i++)
    {
        Proto::Command c;
        c.thrust = true;
        c.turn = i < 60 ? 1.0f : 0.0f;
        f.sim.StepPlayerShip(f.s, c, 1.0f, dt);
        Sim::StepPlayerShip(client, c, 1.0f, dt);
    }
    CHECK(client.GetPosition().x == f.s.ship->GetPosition().x);
    CHECK(client.GetPosition().y == f.s.ship->GetPosition().y);
}

TEST_CASE("a follow order stays with an orbiting station, and prediction keeps up (#298)")
{
    Fixture       f;
    Entity* const station = AddOrbitingStation(f);
    const Vector2 at = station->GetPosition();
    f.s.ship->Teleport({ at.x - 2500.0f, at.y + 800.0f });

    // What an agent's hold_station sends.
    Orders::Order o;
    o.kind = Orders::Kind::Follow;
    o.targetId = 44;
    o.stopDist = 1500.0f;
    REQUIRE(f.sim.GiveOrder(f.s, o) > 0);

    // The client's prediction of the same ship: the same step, told where the station was
    // a render delay ago (six ticks) and how fast the snapshots say it goes.
    Ship           client = *f.s.ship;
    Proto::Command nav;
    nav.navMode = 5;
    nav.navHoldId = 44;
    nav.navRange = 1500.0f;
    bool                first = true;
    std::deque<Vector2> seen;  // the station as each tick left it, newest at the back
    double              t = 0.0;
    const float         dt = Sim::SIM_DT;
    float               worstServer = 0.0f, worstApart = 0.0f;
    for (int i = 0; i < 60 * 150; i++)
    {
        t += dt;
        Orbits::Place(f.World().entities, t);
        f.sim.StepPlayerAttachment(f.s, dt);
        f.sim.StepPlayerOrder(f.s, f.World(), dt);

        seen.push_back(station->GetPosition());
        if (seen.size() > 7)
            seen.pop_front();
        const Vector2   late = seen.front();
        const Vector2   later = seen.size() > 1 ? seen[1] : late;
        Sim::HoldTarget proxy{ late, { (later.x - late.x) / dt, (later.y - late.y) / dt } };
        Sim::StepPlayerShip(client, first ? nav : Proto::Command{}, 1.0f, dt, &proxy);
        first = false;

        if (i >= 60 * 90)
        {
            worstServer = std::max(worstServer, std::fabs(Vector2Distance(f.s.ship->GetPosition(),
                                                                          station->GetPosition()) -
                                                          1500.0f));
            worstApart = std::max(worstApart,
                                  Vector2Distance(f.s.ship->GetPosition(), client.GetPosition()));
        }
    }
    CHECK(Vector2Distance(at, station->GetPosition()) > 5000.0f);  // it moved, a long way
    CHECK(f.s.orderStatus == Orders::Status::Running);             // a hold does not finish
    CHECK(f.s.ship->GetHoldMode() == HoldMode::Follow);
    CHECK(worstServer < 60.0f);
    // The two differ by roughly the render delay times the station's speed, and no more:
    // the one difference CLAUDE.md allows a standing hold.
    CHECK(worstApart < 40.0f);

    // Aborting the order lets go of the station, rather than leaving a hold nobody runs.
    f.sim.AbortOrder(f.s, "test");
    CHECK(f.s.ship->GetHoldMode() == HoldMode::None);
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

TEST_CASE("the region is not decided in its first minutes, and a first ship is news (#143, #231)")
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

TEST_CASE("factions reach a step at a time, where the prize is worth the risk (#231)")
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";
    Simulation        sim;
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    sim.AttachRegion(7, systems);
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(systems);

    std::map<std::string, FactionId> held;
    for (auto& kv : sim.Systems())
        held[kv.first] = kv.second.agg.controller;

    int changes = 0, early = 0;
    for (int pass = 1; pass <= 30 * 120; pass++)  // two simulated hours, a pass every 2 s
    {
        sim.MaintainWorld(2.0f);
        for (auto& kv : sim.Systems())
        {
            const FactionId now = kv.second.agg.controller;
            if (now == held[kv.first])
                continue;
            CAPTURE(kv.first);
            CAPTURE(pass);
            changes++;
            if (pass <= 30 * 10)
                early++;
            // A step, never a leap: whoever took it held a system next door.
            bool adjacent = false;
            for (const std::string& n : sim.Neighbors(kv.first))
                adjacent = adjacent || held[n] == now;
            CHECK(adjacent);
            // The Independents hold what they have and reach for nothing.
            CHECK(now != FactionId::Independent);
        }
        for (auto& kv : sim.Systems())
            held[kv.first] = kv.second.agg.controller;
    }
    MESSAGE(changes << " systems changed hands in two hours, " << early
                    << " in the first ten minutes");
    CHECK(changes > 0);  // the world does go on without players
    CHECK(early <= 1);   // ...but nothing is decided at once
    CHECK(sim.SystemById("verge")->agg.controller == FactionId::Independent);
}

TEST_CASE("a player is not respawned beside a station that would shoot them (#224)")
{
    Fixture f;
    // A system whose only station is an armed one, as Tau Verge's is.
    auto& ents = f.World().entities;
    ents.erase(std::remove_if(ents.begin(), ents.end(), [](const std::unique_ptr<Entity>& e)
                              { return e->GetKind() == EntityKind::Station; }),
               ents.end());
    auto fort = std::make_unique<Station>(Vector2{ 300000.0f, 0.0f }, 600.0f, "Outpost",
                                          FactionId::Independent, StationRole::Military);
    REQUIRE(fort->Has(Component::Defensive));
    const Vector2 at = fort->GetPosition();
    const float   reach = fort->GetSize() + fort->GetArchetype()->weaponRange;
    fort->SetId(77);
    ents.push_back(std::move(fort));

    // Welcome: beside it, as before.
    CHECK(Vector2Distance(f.sim.SafeArrival(f.s.systemId, &f.s), at) < reach);

    // Hated by its owner: anywhere but in its sights.
    f.s.account.SetReputation(FactionId::Independent, -80.0f);
    REQUIRE(f.sim.AccountHostileToFaction(f.s, FactionId::Independent));
    CHECK(Vector2Distance(f.sim.SafeArrival(f.s.systemId, &f.s), at) > reach + 10000.0f);

    // And a respawn uses it.
    f.sim.ServerRespawnPlayer(f.s);
    CHECK(Vector2Distance(f.s.ship->GetPosition(), at) > reach + 10000.0f);
}

TEST_CASE("whoever gets there first names it, once, for everyone (#145)")
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";
    Simulation        sim;
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    sim.AttachRegion(7, systems);
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(systems);
    const std::string home = sim.Universe().startId;
    std::string       entry;
    for (const auto& l : sim.Universe().links)
        if (l.a == home && l.b.rfind("w1-", 0) == 0)
            entry = l.b;
        else if (l.b == home && l.a.rfind("w1-", 0) == 0)
            entry = l.a;
    REQUIRE_FALSE(entry.empty());

    auto pilot = [&](const char* name) -> ClientSession&
    {
        ClientSession& s =
            sim.CreateSession(home, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.ship->SetPilotName(name);
        return s;
    };
    auto lastNotice = [](ClientSession& s)
    { return s.EventsSince(0).empty() ? std::string() : s.EventsSince(0).back().text; };

    ClientSession& ann = pilot("ann");
    ClientSession& bo = pilot("bo");
    CHECK_FALSE(sim.NameSystem(ann, "Haven"));  // known space has its names
    sim.ServerEnterSystem(ann, entry, home);    // first in
    sim.ServerEnterSystem(bo, entry, home);

    CHECK_FALSE(sim.NameSystem(bo, "Bo's Rest"));  // not the discoverer
    CHECK(lastNotice(bo).find("ann") != std::string::npos);
    CHECK_FALSE(sim.NameSystem(ann, "x"));              // too short
    CHECK_FALSE(sim.NameSystem(ann, "9 Lives"));        // not a letter first
    CHECK_FALSE(sim.NameSystem(ann, "Haven<script>"));  // not a name
    CHECK_FALSE(sim.NameSystem(ann, "helios core"));    // taken, whatever the case
    CHECK(sim.NameSystem(ann, "Haven"));
    CHECK_FALSE(sim.NameSystem(ann, "Other Haven"));  // once

    // Everyone sees it, with its designation beside it, and the news says who.
    CHECK(sim.TakeChartsChanged());
    const WorldLoader::Universe known = sim.KnownUniverse();
    bool                        seen = false;
    for (const auto& si : known.systems)
        if (si.id == entry)
        {
            seen = true;
            CHECK(si.name == "Haven");
            CHECK(si.designation.rfind("W-1.", 0) == 0);
            CHECK(si.discoverer == "ann");
        }
    CHECK(seen);
    CHECK(sim.Events().back().find("Haven, named by ann") != std::string::npos);

    // And it is kept: world state, in the world save.
    const std::string path = "names_test_world.json";
    sim.SaveWorld(path);
    Simulation again;
    again.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    again.AttachRegion(7, systems);
    REQUIRE(again.LoadWorld(path) == Save::Result::Ok);
    bool kept = false;
    for (const auto& si : again.KnownUniverse().systems)
        kept = kept || (si.id == entry && si.name == "Haven");
    CHECK(kept);
    std::remove(path.c_str());
}

TEST_CASE("a ship left beside a moving body is found beside it again (#258)")
{
    // A station that orbits a planet (#210) goes on round while its visitor is logged out.
    // Saved as a point in space, the ship came back further from it every time.
    Fixture f;
    auto    hub = std::make_unique<Station>(Vector2{ 400000.0f, 0.0f }, 700.0f, "Aurora Hub",
                                            FactionId::TradersGuild, StationRole::TradeHub);
    Entity* station = hub.get();
    f.World().entities.push_back(std::move(hub));
    const Vector2 was = station->GetPosition();
    f.s.ship->Teleport({ was.x + 1000.0f, was.y - 120.0f });

    const std::string path = "account_near_tmp.json";
    f.sim.SaveAccount(f.s, path);

    // Meanwhile the station moves on, a long way.
    station->SetPosition({ was.x + 15000.0f, was.y + 9000.0f });

    ClientSession& back = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                              GetShipCatalog()[0].stats);
    REQUIRE(f.sim.LoadAccount(back, path) == Save::Result::Ok);
    std::remove(path.c_str());

    CHECK(back.ship->GetPosition().x == doctest::Approx(was.x + 16000.0f));
    CHECK(back.ship->GetPosition().y == doctest::Approx(was.y + 8880.0f));
}

// --- Authoritative world mutation (#38) ---

namespace
{
std::unique_ptr<Station> MakeDepot(Vector2 at)
{
    return std::make_unique<Station>(at, 60.0f, "Depot", FactionId::TradersGuild,
                                     StationRole::TradeHub);
}

bool LayoutHas(const Proto::SystemLayout& l, int id)
{
    for (const Proto::EntityLayout& e : l.entities)
        if (e.id == id)
            return true;
    return false;
}
}  // namespace

TEST_CASE("the static layer changes on the server, and says what changed (#38)")
{
    Fixture           f;
    const std::string sys = f.s.systemId;
    const int         rev0 = f.sim.BuildLayout(sys).rev;

    const int id = f.sim.AddStatic(sys, MakeDepot({ 3000.0f, 0.0f }), "hunter");
    REQUIRE(id != 0);
    const Proto::SystemLayout after = f.sim.BuildLayout(sys);
    CHECK(after.rev > rev0);
    REQUIRE(LayoutHas(after, id));
    for (const Proto::EntityLayout& e : after.entities)
        if (e.id == id)
            CHECK(e.owner == "hunter");

    std::vector<Proto::LayoutDelta> ds = f.sim.TakeLayoutDeltas();
    REQUIRE(ds.size() == 1);
    CHECK(ds[0].systemId == sys);
    CHECK(ds[0].rev == after.rev);
    REQUIRE(ds[0].added.size() == 1);
    CHECK(ds[0].added[0].id == id);
    CHECK(ds[0].added[0].name == "Depot");
    CHECK(f.sim.TakeLayoutDeltas().empty());  // said once

    SUBCASE("taking it away is a removal, and it is gone from the layout")
    {
        REQUIRE(f.sim.RemoveStatic(sys, id));
        ds = f.sim.TakeLayoutDeltas();
        REQUIRE(ds.size() == 1);
        CHECK(ds[0].removed == std::vector<int>{ id });
        CHECK_FALSE(LayoutHas(f.sim.BuildLayout(sys), id));
        CHECK_FALSE(f.sim.RemoveStatic(sys, id));  // and only once
    }

    SUBCASE("an object built and gone before anyone was told is only a removal")
    {
        const int brief = f.sim.AddStatic(sys, MakeDepot({ 0.0f, 3000.0f }), "hunter");
        REQUIRE(f.sim.RemoveStatic(sys, brief));
        ds = f.sim.TakeLayoutDeltas();
        REQUIRE(ds.size() == 1);
        CHECK(ds[0].added.empty());
        CHECK(ds[0].removed == std::vector<int>{ brief });
    }

    SUBCASE("ids are never handed out twice")
    {
        const int next = f.sim.AddStatic(sys, MakeDepot({ 0.0f, -3000.0f }), "");
        CHECK(next != id);
        CHECK(next > id);
    }
}

TEST_CASE("what may not change is refused, not half-changed (#38)")
{
    Fixture           f;
    const std::string sys = f.s.systemId;

    // A planet: satellites find their planet by its place in the file (#210).
    auto planet = std::make_unique<Planet>(1500.0f, 0.0f, 0.0f, 40.0f, Color{ 70, 130, 200, 255 },
                                           ResourceType::Iron, PlanetType::Rocky);
    CHECK(f.sim.AddStatic(sys, std::move(planet), "hunter") == 0);
    // A ship is not part of the static layer at all.
    CHECK(f.sim.AddStatic(sys,
                          std::make_unique<NpcShip>(Vector2{ 0.0f, 0.0f }, FactionId::Pirates,
                                                    NpcRole::Pirate, std::vector<Vector2>{}),
                          "") == 0);
    CHECK(f.sim.AddStatic("no-such-system", MakeDepot({ 0.0f, 0.0f }), "") == 0);

    // A gate already in the world stays: it is an edge of the route graph.
    auto gate = std::make_unique<JumpGate>(Vector2{ 2000.0f, 0.0f }, 50.0f, "Gate", "elsewhere");
    gate->SetId(91);
    f.World().entities.push_back(std::move(gate));
    CHECK_FALSE(f.sim.RemoveStatic(sys, 91));
    CHECK(f.sim.TakeLayoutDeltas().empty());  // nothing was changed, so nothing is said
}

TEST_CASE("nobody is left docked inside a station that is gone (#38)")
{
    Fixture           f;
    const std::string sys = f.s.systemId;
    const int         id = f.sim.AddStatic(sys, MakeDepot({ 5000.0f, 0.0f }), "");
    f.s.ship->Teleport({ 5000.0f, 0.0f });
    REQUIRE(f.sim.StepPlayerDock(f.s, f.World()) == id);

    const int before = f.s.LastEventSeq();
    REQUIRE(f.sim.RemoveStatic(sys, id));
    CHECK_FALSE(f.s.IsDocked());
    const std::vector<Ev::Event> evs = f.s.EventsSince(before);
    REQUIRE(evs.size() == 1);
    CHECK(evs[0].kind == Ev::Kind::Undocked);
    // The snapshot built after it agrees, so a client's station screen closes with it.
    CHECK_FALSE(f.sim.BuildSnapshot(f.s, sys).player.docked);
}

TEST_CASE("a searched wreck is searched for everyone, at once (#38)")
{
    Fixture f;
    auto wreck = std::make_unique<Derelict>(Vector2{ 100.0f, 0.0f }, 20.0f, "Hauler Wreck", 250.0);
    wreck->SetId(61);
    f.World().entities.push_back(std::move(wreck));
    f.sim.TakeLayoutDeltas();

    REQUIRE(f.sim.StepPlayerLoot(f.s, f.World(), 61) > 0.0);
    std::vector<Proto::LayoutDelta> ds = f.sim.TakeLayoutDeltas();
    REQUIRE(ds.size() == 1);
    REQUIRE(ds[0].changed.size() == 1);
    CHECK(ds[0].changed[0].id == 61);
    CHECK(ds[0].changed[0].looted);
    CHECK(ds[0].changed[0].name == "Hauler Wreck");  // the state is apart from the name
}

// The ordering guarantee, end to end through the test transport: the server sends what
// changed before every snapshot built after it, and a client that applies messages in the
// order they arrive never holds a snapshot that disagrees with the server's world -- no
// ghost of something removed, no gap where something was built.
TEST_CASE("a client never sees a snapshot ahead of the change it reflects (#38)")
{
    Fixture           f;
    const std::string sys = f.s.systemId;
    LocalTransport    link;

    Proto::LayoutMirror mirror;
    int                 depot = 0, wreck = 0;
    // What the server held when it built each snapshot, in the order they were sent.
    std::deque<std::set<int>> truths;
    auto                      drain = [&]()
    {
        std::string msg;
        while (link.Client().Poll(msg))
        {
            const std::string   t = Proto::MessageType(msg);
            Proto::SystemLayout lay;
            Proto::LayoutDelta  d;
            if (t == "layout" && Proto::DecodeLayout(msg, lay))
                mirror.Reset(lay);
            else if (t == "ldelta" && Proto::DecodeLayoutDelta(msg, d))
                mirror.Apply(d);
            else if (t == "snap")
            {
                Proto::Snapshot s;
                REQUIRE(Proto::DecodeSnapshot(msg, s));
                Proto::CompleteFromLayout(s, mirror.byId);
                std::set<int> seen;
                for (const Proto::EntitySnapshot& e : s.entities)
                    seen.insert(e.id);
                REQUIRE_FALSE(truths.empty());
                CHECK(seen == truths.front());
                truths.pop_front();
            }
        }
    };
    // One host frame, in RunHost's order: whatever changed, then the deltas, then the
    // snapshot.
    auto frame = [&](const std::function<void()>& change)
    {
        if (change)
            change();
        for (const Proto::LayoutDelta& d : f.sim.TakeLayoutDeltas())
            link.Server().Send(Proto::EncodeLayoutDelta(d));
        link.Server().Send(Proto::EncodeSnapshot(f.sim.BuildSnapshot(f.s, sys)));
        std::set<int> truth;
        for (const auto& e : f.World().entities)
            if (e->GetKind() != EntityKind::Npc)
                truth.insert(e->GetId());
        truths.push_back(truth);
    };

    link.Server().Send(Proto::EncodeLayout(f.sim.BuildLayout(sys)));
    frame(nullptr);
    frame([&]() { depot = f.sim.AddStatic(sys, MakeDepot({ 4000.0f, 0.0f }), "hunter"); });
    frame(
        [&]()
        {
            wreck = f.sim.AddStatic(
                sys, std::make_unique<Derelict>(Vector2{ 50.0f, 0.0f }, 20.0f, "Wreck", 100.0), "");
        });
    drain();
    REQUIRE(mirror.byId.count(depot) == 1);

    SUBCASE("a layout sent in between does not make the delta after it take anything back")
    {
        // A client arriving mid-frame is sent the whole layout, built after a change but
        // before the delta about it goes out. Applying that delta again is a no-op.
        frame(
            [&]()
            {
                f.sim.RemoveStatic(sys, depot);
                link.Server().Send(Proto::EncodeLayout(f.sim.BuildLayout(sys)));
            });
        drain();
        CHECK(mirror.byId.count(depot) == 0);
    }

    SUBCASE("removals and changes arrive before the snapshot that reflects them")
    {
        frame(
            [&]()
            {
                f.sim.RemoveStatic(sys, depot);
                f.s.ship->Teleport({ 50.0f, 0.0f });
                f.sim.StepPlayerLoot(f.s, f.World(), wreck);
            });
        drain();
        CHECK(mirror.byId.count(depot) == 0);
        REQUIRE(mirror.byId.count(wreck) == 1);
        CHECK(mirror.byId.at(wreck).looted);
        CHECK(mirror.rev == f.sim.BuildLayout(sys).rev);
    }
}

TEST_CASE("a saved mission finds its stations again in a world numbered differently (#227)")
{
    // Entity ids come from one counter that runs through every system's stations and the
    // NPCs hydrated beside them, so another run can number the same station differently.
    // A mission saved by id would then point at the wrong station, or at none.
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::string systems = std::string(TEST_DATA_DIR) + "systems/";
    auto              build = [&](Simulation& sim, int shift)
    {
        sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        for (int i = 0; i < shift; i++)  // what a different count of NPCs before it would do
            sim.NextAgentId();
        sim.MaterializeAllSystems(systems);
    };
    struct Place
    {
        std::string system, name;
        int         id = 0;
    };
    auto stations = [](const Simulation& sim)
    {
        std::vector<Place> out;
        for (const auto& kv : sim.Systems())
            for (const auto& e : kv.second.entities)
                if (e->GetKind() == EntityKind::Station)
                    out.push_back({ kv.first, e->GetName(), e->GetId() });
        return out;
    };

    Simulation a;
    build(a, 0);
    const std::vector<Place> before = stations(a);
    REQUIRE(before.size() >= 2);
    const Place giver = before.front();
    const Place dest = before.back();
    REQUIRE(giver.system != dest.system);  // a delivery into another system, the usual case

    ClientSession& s =
        a.CreateSession(a.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    Mission delivery;
    delivery.type = MissionType::Delivery;
    delivery.title = "Haul";
    delivery.giverStationId = giver.id;
    delivery.destStationId = dest.id;
    Mission bounty;
    bounty.type = MissionType::Bounty;
    bounty.title = "Hunt";
    bounty.giverStationId = dest.id;
    bounty.targetCount = 3;
    bounty.progress = 1;
    s.missions.SetMirror({}, { delivery, bounty });

    const std::string path = "account_missions_tmp.json";
    a.SaveAccount(s, path);

    Simulation b;
    build(b, 37);
    const std::vector<Place> after = stations(b);
    REQUIRE(after.size() == before.size());
    auto idIn = [&](const Place& p)
    {
        for (const Place& q : after)
            if (q.system == p.system && q.name == p.name)
                return q.id;
        return 0;
    };
    REQUIRE(idIn(dest) != dest.id);  // the numbering really did move

    SUBCASE("each mission is handed in where it was taken to be")
    {
        ClientSession& t =
            b.CreateSession(b.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        REQUIRE(b.LoadAccount(t, path) == Save::Result::Ok);
        const std::vector<Mission>& got = t.missions.Active();
        REQUIRE(got.size() == 2);
        CHECK(got[0].giverStationId == idIn(giver));
        CHECK(got[0].destStationId == idIn(dest));
        CHECK(got[1].giverStationId == idIn(dest));
        CHECK(got[1].progress == 1);
    }

    SUBCASE("a mission whose station is gone is dropped, and the player told")
    {
        nlohmann::json j;
        {
            std::ifstream in(path);
            j = nlohmann::json::parse(in);
        }
        j["missions"][0]["dest"]["station"] = "Nowhere At All";
        {
            std::ofstream out(path);
            out << j.dump();
        }
        ClientSession& t =
            b.CreateSession(b.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        const int seq = t.LastEventSeq();
        REQUIRE(b.LoadAccount(t, path) == Save::Result::Ok);
        REQUIRE(t.missions.Active().size() == 1);
        CHECK(t.missions.Active()[0].title == "Hunt");
        CHECK(NoticeSince(t, seq).find("Haul") != std::string::npos);
    }

    SUBCASE("a save from before names were written keeps its ids")
    {
        {
            std::ofstream out(path);
            out << R"({"version":2,"missions":[{"type":2,"title":"Old","giver":5,"dest":9}]})";
        }
        ClientSession& t =
            b.CreateSession(b.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        REQUIRE(b.LoadAccount(t, path) == Save::Result::Ok);
        REQUIRE(t.missions.Active().size() == 1);
        CHECK(t.missions.Active()[0].giverStationId == 5);
        CHECK(t.missions.Active()[0].destStationId == 9);
    }
}

// --- What players changed outlives the server (#38) ---

namespace
{
// The registries once per test, before any server: a second Archetypes::Load leaves every
// entity already built pointing into the registry it replaced.
void LoadRegistries()
{
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
}

// A server's start, as econserver makes one: the data, then the save if there is one, then
// every system built from data with the save's changes replayed on top.
void StartServer(Simulation& sim, const std::string& worldPath)
{
    sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_DATA_DIR) + "systems/");
}

// A system's static layer as somebody standing in it would describe it, in its order: what
// each object is called in a save, and everything a player could tell apart. Ids are left
// out on purpose -- they are good for one run.
std::vector<std::string> StaticLayer(const Simulation& sim, const std::string& sys)
{
    std::vector<std::string> out;
    for (const Proto::EntityLayout& e : sim.BuildLayout(sys).entities)
        out.push_back(sim.StaticKey(sys, e.id) + " | " + std::to_string((int)e.kind) + " " +
                      e.name + " owner=" + e.owner + (e.looted ? " searched" : "") + " @" +
                      std::to_string((long)std::lround(e.pos.x)) + "," +
                      std::to_string((long)std::lround(e.pos.y)));
    return out;
}

template <typename T> T* FirstOf(SystemState& st, EntityKind kind)
{
    for (auto& e : st.entities)
        if (e->GetKind() == kind)
            return static_cast<T*>(e.get());
    return nullptr;
}
}  // namespace

TEST_CASE("the world's own objects are called the same on every load (#38)")
{
    LoadRegistries();
    Simulation a, b;
    StartServer(a, "");
    StartServer(b, "");
    const std::string sys = a.Universe().startId;
    CHECK(StaticLayer(a, sys) == StaticLayer(b, sys));

    // By what they are: the array they came from, their name, and which of that name.
    Station* hub = FirstOf<Station>(*a.SystemById(sys), EntityKind::Station);
    REQUIRE(hub != nullptr);
    CHECK(a.StaticKey(sys, hub->GetId()) == "stations/" + hub->GetName() + "#0");
    // A body or a gate is not something a save names: it does not change.
    const Entity* star = FirstOf<Entity>(*a.SystemById(sys), EntityKind::Star);
    REQUIRE(star != nullptr);
    CHECK(a.StaticKey(sys, star->GetId()).empty());
}

TEST_CASE("what players changed in the static layer survives a restart (#38)")
{
    LoadRegistries();
    const std::string path = "world_changes_tmp.json";
    Simulation        a;
    StartServer(a, "");
    const std::string sys = a.Universe().startId;
    SystemState&      st = *a.SystemById(sys);

    // One of the world's own stations taken away, a wreck searched...
    Station* hub = FirstOf<Station>(st, EntityKind::Station);
    REQUIRE(hub != nullptr);
    const std::string hubName = hub->GetName();
    REQUIRE(a.RemoveStatic(sys, hub->GetId()));
    Derelict* wreck = FirstOf<Derelict>(st, EntityKind::Derelict);
    REQUIRE(wreck != nullptr);
    wreck->SetLooted();
    a.MarkStaticChanged(st, wreck->GetId());
    // ...a depot built, one built and pulled down again, and a wreck left by a player.
    const int depot = a.AddStatic(sys, MakeDepot({ 3000.0f, 0.0f }), "hunter");
    const int brief = a.AddStatic(sys, MakeDepot({ 0.0f, 3000.0f }), "hunter");
    REQUIRE(a.RemoveStatic(sys, brief));
    auto hulk = std::make_unique<Derelict>(Vector2{ -4000.0f, 0.0f }, 50.0f, "Hulk", 250.0);
    hulk->SetLooted();
    REQUIRE(a.AddStatic(sys, std::move(hulk), "ann") != 0);
    CHECK(a.StaticKey(sys, depot) == "+1");

    const std::vector<std::string> before = StaticLayer(a, sys);
    a.SaveWorld(path);

    Simulation b;
    StartServer(b, path);
    std::remove(path.c_str());
    CHECK(StaticLayer(b, sys) == before);  // the same objects, in the same order, the same state

    bool hubBack = false;
    for (const auto& e : b.SystemById(sys)->entities)
        hubBack = hubBack || (e->GetKind() == EntityKind::Station && e->GetName() == hubName &&
                              e->GetOwner().empty());
    CHECK_FALSE(hubBack);

    // A key is never handed out twice, not even across a restart.
    const int next = b.AddStatic(sys, MakeDepot({ 6000.0f, 0.0f }), "hunter");
    CHECK(b.StaticKey(sys, next) == "+4");
    // The replay itself is no news: a client is sent the whole layout on arrival anyway.
    CHECK(b.TakeLayoutDeltas().size() == 1);  // only the depot just built

    SUBCASE("and survives a second restart, with nothing more changed")
    {
        b.SaveWorld(path);
        Simulation c;
        StartServer(c, path);
        std::remove(path.c_str());
        CHECK(StaticLayer(c, sys) == StaticLayer(b, sys));
    }
}

TEST_CASE("a world saved before changes were kept loads as the data describes it (#38, #20)")
{
    LoadRegistries();
    Simulation fresh;
    StartServer(fresh, "");
    const std::string sys = fresh.Universe().startId;
    Station*          hub = FirstOf<Station>(*fresh.SystemById(sys), EntityKind::Station);
    REQUIRE(hub != nullptr);

    // Version 2 knew nothing of changes; anything under that name is not this build's to read.
    const std::string path = "world_v2_tmp.json";
    {
        std::ofstream out(path);
        out << R"({ "version": 2, "simTime": 5.0, "galaxy": { ")" << sys
            << R"(": { "traders": 3 } }, "changes": { ")" << sys
            << R"(": { "removed": [ "stations/)" << hub->GetName() << R"(#0" ] } } })";
    }
    Simulation old;
    StartServer(old, path);
    std::remove(path.c_str());
    CHECK(old.Time() == doctest::Approx(5.0));
    CHECK(StaticLayer(old, sys) == StaticLayer(fresh, sys));

    // And one from a later build is refused, as ever.
    {
        std::ofstream out(path);
        out << R"({ "version": )" << Save::WORLD_VERSION + 1 << R"(, "galaxy": {} })";
    }
    Simulation later;
    later.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
    CHECK(later.LoadWorld(path) == Save::Result::TooNew);
    std::remove(path.c_str());
}
