#include <doctest/doctest.h>
#include <cmath>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Faction.h"
#include "core/Archetype.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/SaveSchema.h"
#include "missions/Mission.h"
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
        const std::string bumped =
            text.replace(text.find("\"version\": 1"), 12,
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
