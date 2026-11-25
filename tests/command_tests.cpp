#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "goods_named.h"
#include "factions_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/JumpGate.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Events.h"
#include "sim/Orders.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

// The server's verb table (#354), through Simulation::ApplyAction and ApplyCommand: an Action in,
// the world's answer out, and a Command in, one tick of it. Before it moved out of the host loop
// none of this could be tested without a socket; here a Command is built the way a client builds it
// and the session is read afterwards.
namespace
{
const std::string DATA = TEST_DATA_DIR;

struct Fixture
{
    Simulation                   sim;
    ClientSession&               s;
    std::vector<Proto::TradeAck> acks;
    Simulation::FireMap          fires;
    int                          nextId = 700;

    Fixture() : s(Make()) {}

    ClientSession& Make()
    {
        REQUIRE(Factions::Load(DATA + "factions.json"));
        REQUIRE(Goods::Load(DATA + "goods.json"));
        Archetypes::Load(DATA + "archetypes.json");
        Blueprints::Load(DATA + "blueprints.json");
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        return sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                 GetShipCatalog()[0].stats);
    }

    SystemState& World() { return *sim.SystemOf(s); }

    void Apply(const Proto::Command& c, float dt = 1.0f / 60.0f)
    {
        sim.ApplyCommand(s, c, dt, fires);
    }

    // True if the action moved the session to another system.
    bool Act(const Proto::Action& a) { return sim.ApplyAction(s, a, acks); }

    // A trading station, under the ship unless told.
    int AddStation(Vector2 at = { 0.0f, 0.0f })
    {
        auto st = std::make_unique<Station>(at, 60.0f, "Depot", Fac::TradersGuild(),
                                            StationRole::TradeHub);
        st->SetId(++nextId);
        World().entities.push_back(std::move(st));
        return nextId;
    }

    void Dock()
    {
        AddStation();
        Act(Proto::Act::Dock{});
        REQUIRE(s.IsDocked());
    }

    // Whether the journal has an entry of this kind since `seq`.
    bool Said(int sinceSeq, Ev::Kind kind) const
    {
        for (const Ev::Event& e : s.EventsSince(sinceSeq))
            if (e.kind == kind)
                return true;
        return false;
    }
};
}  // namespace

TEST_CASE("a dock action docks within reach and an undock action leaves")
{
    Fixture f;
    f.AddStation({ 5000.0f, 0.0f });

    f.Act(Proto::Act::Dock{});
    CHECK_FALSE(f.s.IsDocked());  // too far away

    f.s.ship->Teleport({ 5000.0f, 0.0f });
    f.Act(Proto::Act::Dock{});
    CHECK(f.s.IsDocked());

    f.Act(Proto::Act::Undock{});
    CHECK_FALSE(f.s.IsDocked());
}

TEST_CASE("a docked ship does not fly on a thrust command; one in space does")
{
    Fixture        f;
    Proto::Command c;
    c.thrust = true;

    const Vector2 free = f.s.ship->GetPosition();
    for (int i = 0; i < 30; i++)
        f.Apply(c);
    CHECK(f.s.ship->GetPosition().x != doctest::Approx(free.x));

    Fixture g;
    g.Dock();
    const Vector2 held = g.s.ship->GetPosition();
    for (int i = 0; i < 30; i++)
        g.Apply(c);
    CHECK(g.s.ship->GetPosition().x == doctest::Approx(held.x));
    CHECK(g.s.ship->GetPosition().y == doctest::Approx(held.y));
}

TEST_CASE("an action is not a tick: the ship does not move by it")
{
    Fixture       f;
    const Vector2 before = f.s.ship->GetPosition();
    f.Act(Proto::Act::Scan{});
    f.Act(Proto::Act::RequestAssets{});
    f.Act(Proto::Act::AbortOrder{});
    CHECK(f.s.ship->GetPosition().x == doctest::Approx(before.x));
    CHECK(f.s.ship->GetPosition().y == doctest::Approx(before.y));
}

TEST_CASE("selling by action pays the account and acknowledges the sale; in space it does neither")
{
    Fixture            f;
    const ResourceType ore = AllResourceTypes()[0];
    f.s.ship->AddCargo(ore, 10);

    Proto::Act::Sell sell;
    sell.type = ore.Index();
    sell.amount = 10;

    f.Act(sell);  // in space: nobody to sell to
    CHECK(f.acks.empty());
    CHECK(f.s.ship->GetCargoAmount(ore) == 10);

    f.Dock();
    const double before = f.s.account.GetMoney();
    f.Act(sell);
    REQUIRE(f.acks.size() == 1);
    CHECK(f.acks[0].type == ore.Index());
    CHECK(f.acks[0].sold == 10);
    CHECK(f.s.ship->GetCargoAmount(ore) == 0);
    CHECK(f.s.account.GetMoney() > before);
}

TEST_CASE("buying by action takes money, fills the hold and acknowledges the purchase")
{
    Fixture f;
    f.Dock();
    const ResourceType food = Good::Named("Food");
    f.s.account.SetMoney(10000.0);

    Proto::Act::Buy buy;
    buy.type = food.Index();
    buy.amount = 5;
    f.Act(buy);

    REQUIRE(f.acks.size() == 1);
    CHECK(f.acks[0].bought == 5);
    CHECK(f.acks[0].cost > 0.0);
    CHECK(f.s.ship->GetCargoAmount(food) == 5);
    CHECK(f.s.account.GetMoney() < 10000.0);
}

TEST_CASE("debug money is granted only on a server in dev mode, and a refusal is said")
{
    Fixture      f;
    const double before = f.s.account.GetMoney();

    const int seq = f.s.LastEventSeq();
    f.Act(Proto::Act::DebugMoney{});
    CHECK(f.s.account.GetMoney() == doctest::Approx(before));
    CHECK(f.Said(seq, Ev::Kind::Notice));

    f.sim.SetDevCommands(true);
    f.Act(Proto::Act::DebugMoney{});
    CHECK(f.s.account.GetMoney() > before);
}

TEST_CASE("a jump action through a gate in reach changes the system and says so")
{
    Fixture           f;
    const std::string from = f.s.systemId;

    JumpGate probe({ 0.0f, 0.0f }, 150.0f, "Gate", "reach");
    REQUIRE(probe.GetArchetype() != nullptr);
    const float reach = probe.GetSize() + probe.GetArchetype()->jumpRange;
    const float x = 5000.0f;
    auto        gate =
        std::make_unique<JumpGate>(Vector2{ x, 0.0f }, 150.0f, "Gate to Sigma Reach", "reach");
    gate->SetId(72);
    f.World().entities.push_back(std::move(gate));

    Proto::Act::Jump jump;
    jump.gateId = 72;

    f.s.ship->Teleport({ x - (reach + 100.0f), 0.0f });
    CHECK_FALSE(f.Act(jump));  // out of reach
    CHECK(f.s.systemId == from);

    f.s.ship->Teleport({ x - (reach - 10.0f), 0.0f });
    CHECK(f.Act(jump));  // the layout must be sent again
    CHECK(f.s.systemId == "reach");
}

TEST_CASE("an order action starts an order, an abort drops it, and manual control takes over")
{
    Fixture               f;
    Proto::Act::GiveOrder order;
    order.order.kind = Orders::Kind::MoveTo;
    order.order.point = { 3000.0f, 0.0f };

    f.Act(order);
    CHECK(f.s.HasRunningOrder());

    f.Act(Proto::Act::AbortOrder{});
    CHECK_FALSE(f.s.HasRunningOrder());

    f.Act(order);
    REQUIRE(f.s.HasRunningOrder());
    Proto::Command stick;
    stick.thrust = true;
    f.Apply(stick);
    CHECK_FALSE(f.s.HasRunningOrder());  // the same rule the autopilot follows
}

TEST_CASE("docked, only an undock order can run; any other is refused")
{
    Fixture f;
    f.Dock();

    Proto::Act::GiveOrder move;
    move.order.kind = Orders::Kind::MoveTo;
    move.order.point = { 3000.0f, 0.0f };
    f.Act(move);
    CHECK_FALSE(f.s.HasRunningOrder());

    Proto::Act::GiveOrder leave;
    leave.order.kind = Orders::Kind::Undock;
    f.Act(leave);
    CHECK(f.s.HasRunningOrder());
}

TEST_CASE("a toggle command arms and disarms the weapon")
{
    Fixture        f;
    Proto::Command c;
    c.toggleWeapon = true;
    CHECK_FALSE(f.s.weaponOn);
    f.Apply(c);
    CHECK(f.s.weaponOn);
    f.Apply(c);
    CHECK_FALSE(f.s.weaponOn);
}

TEST_CASE("a refused verb is said in the journal, not dropped without a word")
{
    Fixture   f;
    const int seq = f.s.LastEventSeq();

    f.Act(Proto::Act::NameSystem{ "7", "" });  // not a name: a name starts with a letter
    CHECK(f.Said(seq, Ev::Kind::Notice));

    const int seq2 = f.s.LastEventSeq();
    f.Act(Proto::Act::Refit{});  // no shipyard in open space
    CHECK(f.Said(seq2, Ev::Kind::Notice));
}

TEST_CASE("verbs for the other side of the counter are not asked of the ship")
{
    Fixture            f;
    const ResourceType ore = AllResourceTypes()[0];
    f.s.ship->AddCargo(ore, 4);

    // In flight a sale is ignored; docked, a jump or a scan is.
    f.Act(Proto::Act::Sell{ ore.Index(), 4, "" });
    CHECK(f.s.ship->GetCargoAmount(ore) == 4);

    f.Dock();
    const int seq = f.s.LastEventSeq();
    f.Act(Proto::Act::Scan{});
    CHECK_FALSE(f.Said(seq, Ev::Kind::Scanned));
    CHECK(f.s.IsDocked());
}
