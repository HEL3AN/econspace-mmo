#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Market.h"
#include "economy/Resource.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// The trade loop, slice 2 (#343): NPC haulers carry goods between markets by price, conserved
// as ships are (#295); a hauler shot down leaves its cargo in a wreck; belts run out and grow
// back (#348); and the fee is the market owner's.

namespace
{
const std::string DATA = TEST_DATA_DIR;

struct GoodsFile
{
    GoodsFile() { REQUIRE(Goods::Load(DATA + "goods.json")); }
    ~GoodsFile() { Goods::ResetToBuiltin(); }
};

void StartServer(Simulation& sim, const std::string& worldPath = std::string(), bool hot = false)
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.SetAllHot(hot);
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

// Of one good: what every market holds, and what is on its way between them.
double Everywhere(const Simulation& sim, ResourceType g)
{
    double total = sim.GoodsInTransit()[(size_t)g.Index()];
    for (const auto& kv : sim.Systems())
        if (kv.second.profile.markets > 0)
            total += kv.second.market.Stock(g);
    return total;
}
}  // namespace

TEST_CASE("haulers carry a surplus next door, and no unit is made or lost on the way (#343)")
{
    GoodsFile  file;
    Simulation sim;
    StartServer(sim);
    const ResourceType medicine = Good::Named("Medicine");

    // Nothing made or used anywhere: whatever moves a stock now is a hauler.
    for (const auto& kv : sim.Systems())
        sim.SystemById(kv.first)->profile.rates = MarketRates{};
    // A market with a glut, and a neighbour with a market of its own.
    std::string from, to;
    for (const auto& kv : sim.Systems())
    {
        if (kv.second.profile.markets <= 0 || !from.empty())
            continue;
        for (const std::string& n : sim.Neighbors(kv.first))
            if (sim.SystemById(n)->profile.markets > 0)
            {
                from = kv.first;
                to = n;
                break;
            }
    }
    REQUIRE_FALSE(from.empty());
    SystemState& glut = *sim.SystemById(from);
    glut.market.SetMarkets(glut.profile.markets);
    glut.market.SetStock(medicine, glut.market.Target(medicine) * 6.0);
    const double before = Everywhere(sim, medicine);
    const double there = sim.SystemById(to)->market.Stock(medicine);

    bool carried = false;
    for (int i = 0; i < 1800; i++)  // an hour, nobody anywhere
    {
        sim.MaintainWorld(2.0f);
        for (const auto& m : sim.Migrants())
            carried = carried || (m.second.cargo > 0 && m.second.good == medicine);
        // Conserved at every pass, the goods on the trip included.
        if (i % 100 == 0)
            CHECK(Everywhere(sim, medicine) == doctest::Approx(before).epsilon(1e-6));
    }
    CHECK(carried);
    CHECK(Everywhere(sim, medicine) == doctest::Approx(before).epsilon(1e-6));
    CHECK(glut.market.Stock(medicine) < glut.market.Target(medicine) * 6.0);
    CHECK(sim.SystemById(to)->market.Stock(medicine) > there);
    // ...and a gap is narrowed, never closed: next door still pays more than the glut asks.
    CHECK(sim.SystemById(to)->market.Mid(medicine) > glut.market.Mid(medicine));
}

TEST_CASE("a hauler shot down leaves its cargo in its wreck, for whoever searches it (#343)")
{
    GoodsFile  file;
    Simulation sim;
    StartServer(sim, std::string(), true);
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    SystemState&       st = *sim.SystemOf(s);
    const ResourceType steel = Good::Named("Steel");
    const int          room = s.ship->GetCargoCapacity() - s.ship->GetCargoUsed();
    REQUIRE(room > 0);

    auto hauler = std::make_unique<NpcShip>(Vector2{ 20.0f, 0.0f }, Fac::TradersGuild(),
                                            NpcRole::Trader, std::vector<Vector2>{});
    hauler->SetId(990010);
    hauler->SetCargo(steel, room + 7);
    hauler->TakeDamage(1.0e6f);
    st.entities.push_back(std::move(hauler));
    sim.StepSystemAgents(st, {}, nullptr, 1.0f / 60.0f);

    const Derelict* wreck = nullptr;
    for (const auto& e : st.entities)
    {
        CHECK(e->GetId() != 990010);  // the ship is gone...
        if (e->GetKind() == EntityKind::Derelict &&
            static_cast<const Derelict&>(*e).GetCargoAmount() > 0)
            wreck = static_cast<const Derelict*>(e.get());
    }
    REQUIRE(wreck != nullptr);  // ...and what it carried is where it was
    CHECK(wreck->GetCargo() == steel);
    CHECK(wreck->GetCargoAmount() == room + 7);
    // The layout says so, for a client and an agent.
    bool told = false;
    for (const Proto::EntityLayout& l : sim.BuildLayout(st.id).entities)
        told = told ||
               (l.id == wreck->GetId() && l.cargo == steel.Index() && l.cargoAmount == room + 7);
    CHECK(told);

    // The hold takes what it has room for; the rest waits for the next search.
    s.ship->SetPosition(wreck->GetPosition());
    sim.StepPlayerLoot(s, st, wreck->GetId());
    CHECK(s.ship->GetCargoAmount(steel) == room);
    CHECK(wreck->GetCargoAmount() == 7);
    CHECK_FALSE(wreck->IsLooted());
    s.ship->RemoveCargo(steel, room);
    sim.StepPlayerLoot(s, st, wreck->GetId());
    CHECK(s.ship->GetCargoAmount(steel) == 7);
    CHECK(wreck->IsLooted());
}

TEST_CASE("a belt runs out as it is mined, grows back, and is kept by the save (#348)")
{
    GoodsFile file;
    SUBCASE("the field")
    {
        AsteroidField belt({ 0.0f, 0.0f }, 500.0f, "Belt", Good::Iron(), 200);
        CHECK(belt.Take(50.5) == doctest::Approx(50.5));
        CHECK(belt.Extract(200) == 149);  // whole units only
        CHECK_FALSE(belt.HasOre());
        CHECK(belt.Take(10.0) == doctest::Approx(0.5));
        belt.Regrow(0.1, 2.0);  // two hours at a tenth of the belt an hour
        CHECK(belt.GetOre() == doctest::Approx(40.0));
        belt.Regrow(0.1, 100.0);
        CHECK(belt.GetOre() == doctest::Approx(200.0));  // never past full
    }
    SUBCASE("the world")
    {
        const TempDir     tmp;
        const std::string path = tmp.Path("world.json");
        std::string       system;
        int               beltId = 0;
        double            left = 0.0;
        {
            Simulation sim;
            StartServer(sim);
            for (auto& kv : sim.Systems())
                if (kv.second.profile.belts > 0 && system.empty())
                    system = kv.first;
            REQUIRE_FALSE(system.empty());
            SystemState&   st = *sim.SystemById(system);
            AsteroidField* belt = nullptr;
            for (auto& e : st.entities)
                if (e->GetKind() == EntityKind::Field && belt == nullptr)
                    belt = static_cast<AsteroidField*>(e.get());
            REQUIRE(belt != nullptr);
            beltId = belt->GetId();
            belt->SetOre(0.0);             // worked out by somebody
            for (int i = 0; i < 900; i++)  // half an hour
                sim.MaintainWorld(2.0f);
            CHECK(belt->GetOre() > 0.0);  // growing back
            CHECK(belt->GetOre() < belt->GetOreMax());
            left = belt->GetOre();
            // The snapshot carries what is left (#348).
            ClientSession& s =
                sim.CreateSession(system, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
            bool shown = false;
            for (const auto& b : sim.BuildSnapshot(s, system).belts)
                shown =
                    shown || (b.id == beltId && b.ore == (int)left && b.max == belt->GetOreMax());
            CHECK(shown);
            REQUIRE(sim.SaveWorld(path));
        }
        Simulation again;
        StartServer(again, path);
        double back = -1.0;
        for (const auto& e : again.SystemById(system)->entities)
            if (e->GetKind() == EntityKind::Field && back < 0.0)
                back = static_cast<const AsteroidField&>(*e).GetOre();
        CHECK(back == doctest::Approx(left).epsilon(1e-3));
    }
}

TEST_CASE("the fee is the market owner's, and its treasury is kept (#343)")
{
    GoodsFile         file;
    const TempDir     tmp;
    const std::string path = tmp.Path("world.json");
    double            fees = 0.0;
    {
        Simulation sim;
        StartServer(sim);
        ClientSession& s = sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                             GetShipCatalog()[0].stats);
        SystemState&   st = *sim.SystemOf(s);
        auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                 Fac::TradersGuild(), StationRole::TradeHub);
        station->SetId(990001);
        st.entities.push_back(std::move(station));
        REQUIRE(sim.StepPlayerDock(s, st) == 990001);
        const double was = sim.MindOf(Fac::TradersGuild()).treasury;
        s.account.SetMoney(10000.0);
        const auto bought = sim.StepPlayerBuy(s, st, Good::Named("Food").Index(), 10);
        REQUIRE(bought.bought > 0);
        const auto sold = sim.StepPlayerSell(s, st, Good::Named("Food").Index(), 10);
        REQUIRE(sold.sold > 0);
        fees = bought.fee + sold.fee;
        CHECK(sim.MindOf(Fac::TradersGuild()).treasury == doctest::Approx(was + fees));
        fees = sim.MindOf(Fac::TradersGuild()).treasury;
        REQUIRE(sim.SaveWorld(path));
    }
    Simulation again;
    StartServer(again, path);
    CHECK(again.MindOf(Fac::TradersGuild()).treasury == doctest::Approx(fees).epsilon(1e-3));
}

TEST_CASE("the wire carries a hauler's cargo, a wreck's and what is left of a belt (#343, #348)")
{
    GoodsFile       file;
    Proto::Snapshot s;
    s.systemId = "core";
    Proto::EntitySnapshot npc;
    npc.id = 5;
    npc.kind = Proto::EntityKind::Npc;
    npc.cargo = Good::Named("Fuel").Index();
    npc.cargoAmount = 30;
    s.entities.push_back(npc);
    s.belts.push_back({ 7, 120, 300 });
    Proto::Snapshot r;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), r));
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].cargo == npc.cargo);
    CHECK(r.entities[0].cargoAmount == 30);
    REQUIRE(r.belts.size() == 1);
    CHECK(r.belts[0].ore == 120);
    CHECK(r.belts[0].max == 300);

    Proto::SystemLayout l;
    Proto::EntityLayout belt, wreck;
    belt.id = 7;
    belt.kind = Proto::EntityKind::Field;
    belt.resource = Good::Iron().Index();
    wreck.id = 8;
    wreck.kind = Proto::EntityKind::Derelict;
    wreck.cargo = npc.cargo;
    wreck.cargoAmount = 12;
    l.entities = { belt, wreck };
    Proto::SystemLayout back;
    REQUIRE(Proto::DecodeLayout(Proto::EncodeLayout(l), back));
    REQUIRE(back.entities.size() == 2);
    CHECK(back.entities[1].cargoAmount == 12);

    // A client completes them: the belt's ore from the snapshot, the wreck's hold from the
    // layout.
    std::map<int, Proto::EntityLayout> byId;
    for (const Proto::EntityLayout& e : back.entities)
        byId[e.id] = e;
    Proto::CompleteFromLayout(r, byId);
    for (const Proto::EntitySnapshot& e : r.entities)
    {
        if (e.id == 7)
        {
            CHECK(e.oreLeft == 120);
            CHECK(e.oreMax == 300);
        }
        if (e.id == 8)
            CHECK(e.cargoAmount == 12);
    }
}
