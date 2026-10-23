#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Market.h"
#include "economy/Resource.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "missions/Mission.h"
#include "sim/ClientSession.h"
#include "sim/Observation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "goods_named.h"
#include "temp_dir.h"

#include <fstream>
#include <string>
#include <vector>

// Delivery contracts a short market posts (#343, #407): priced from the shortage and from what
// the goods cost nearby, paid out of the owner's treasury, taken by one pilot, handed in docked
// at the market's station, and in the save, on the wire and in what an agent reads.

namespace
{
const std::string DATA = TEST_DATA_DIR;

struct GoodsFile
{
    GoodsFile() { REQUIRE(Goods::Load(DATA + "goods.json")); }
    ~GoodsFile() { Goods::Load(DATA + "goods.json"); }
};

// The design's contract rules for every test, small enough for a starter's hold.
struct Rules
{
    WorldRules::Contracts contracts = WorldRules::ContractRules();
    WorldRules::Jobs      jobs = WorldRules::JobRules();
    Rules()
    {
        WorldRules::Contracts c;
        c.minAmount = 5;
        c.maxAmount = 10;
        WorldRules::SetContractRules(c);
        WorldRules::SetJobRules(WorldRules::Jobs{});
    }
    ~Rules()
    {
        WorldRules::SetContractRules(contracts);
        WorldRules::SetJobRules(jobs);
    }
};

void StartServer(Simulation& sim, const std::string& worldPath = std::string())
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    // Nothing made or used anywhere: whatever moves a stock now is the test.
    for (const auto& kv : sim.Systems())
        sim.SystemById(kv.first)->profile.rates = MarketRates{};
}

// A system whose market a faction's station keeps, with a neighbour that trades too.
struct Scene
{
    std::string system, next;
    int         station = 0;
    FactionId   owner = Factions::Unaffiliated();
};
Scene FindScene(const Simulation& sim)
{
    for (const auto& kv : sim.Systems())
    {
        const Simulation::MarketHolder h = sim.MarketStation(kv.second);
        if (h.stationId == 0)
            continue;
        for (const std::string& n : sim.Neighbors(kv.first))
            if (sim.SystemById(n)->profile.markets > 0)
                return { kv.first, n, h.stationId, h.faction };
    }
    FAIL("no market with a market next door");
    return {};
}

// The open contract for `good` at `system`, or null.
const Contract* ContractFor(const Simulation& sim, const std::string& system, ResourceType good)
{
    for (const auto& kv : sim.Contracts())
        if (kv.second.system == system && kv.second.good == good)
            return &kv.second;
    return nullptr;
}

ClientSession& DockedAt(Simulation& sim, const Scene& sc)
{
    Vector2 at{ 0.0f, 0.0f };
    for (const auto& e : sim.SystemById(sc.system)->entities)
        if (e->GetId() == sc.station)
            at = e->GetPosition();
    ClientSession& s = sim.CreateSession(sc.system, at, GetShipCatalog()[0].stats);
    REQUIRE(sim.StepPlayerDock(s, *sim.SystemById(sc.system)) == sc.station);
    return s;
}

int OfferOf(const ClientSession& s, int contractId)
{
    const std::vector<Mission>& offers = s.missions.Offers();
    for (size_t i = 0; i < offers.size(); i++)
        if (offers[i].type == MissionType::Contract && offers[i].contractId == contractId)
            return (int)i;
    return -1;
}

// The scene with food short at its market, plenty next door, and the owner with money.
Scene ShortOfFood(Simulation& sim, double treasury)
{
    const Scene        sc = FindScene(sim);
    const ResourceType food = Good::Named("Food");
    Market&            m = sim.SystemById(sc.system)->market;
    m.SetStock(food, 0.2 * m.Target(food));
    sim.MindOf(sc.owner).treasury = treasury;
    return sc;
}
}  // namespace

TEST_CASE("the contract rules read, and an impossible one is an error (#343)")
{
    Rules r;
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    CHECK(WorldRules::ContractRules().every > 0.0);
    CHECK(WorldRules::ContractRules().premium + WorldRules::ContractRules().premiumShort < 1.0);

    TempDir     dir;
    std::string text;
    {
        std::ifstream in(DATA + "world_rules.json");
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto loads = [&](const std::string& from, const std::string& to)
    {
        std::string  changed = text;
        const size_t at = changed.find(from);
        REQUIRE(at != std::string::npos);
        changed.replace(at, from.size(), to);
        const std::string path = dir.Path("rules.json");
        std::ofstream(path) << changed;
        return WorldRules::Load(path);
    };
    CHECK_FALSE(loads("\"shortAt\"", "\"shortAtt\""));
    CHECK(WorldRules::Error().find("shortAtt") != std::string::npos);
    // A contract paying what buying the goods at its own market costs would print money.
    CHECK_FALSE(loads("\"premiumShort\": 0.4", "\"premiumShort\": 0.7"));
    CHECK_FALSE(loads("\"maxAmount\": 40", "\"maxAmount\": 5"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
}

TEST_CASE("a short market posts a contract between selling and buying there, if near is cheaper "
          "(#343)")
{
    GoodsFile  file;
    Rules      r;
    Simulation sim;
    StartServer(sim);
    const ResourceType food = Good::Named("Food");
    const Scene        sc = ShortOfFood(sim, 0.0);

    // Nothing in the treasury: nothing posted.
    sim.PostContracts();
    CHECK(ContractFor(sim, sc.system, food) == nullptr);

    sim.MindOf(sc.owner).treasury = 1.0e6;
    sim.PostContracts();
    const Contract* c = ContractFor(sim, sc.system, food);
    REQUIRE(c != nullptr);
    CHECK(c->faction == sc.owner);
    CHECK(c->amount >= 5);
    CHECK(c->amount <= 10);
    CHECK(c->deadline > sim.Time());
    // The price is set aside.
    CHECK(sim.MindOf(sc.owner).treasury == doctest::Approx(1.0e6 - c->reward));

    const double  fee = Goods::Rules().fee;
    const Market& here = sim.SystemById(sc.system)->market;
    // Better than selling the goods there, and less than buying them there to hand back...
    CHECK(c->reward > here.QuoteSell(food, c->amount) * (1.0 - fee));
    CHECK(c->reward < here.QuoteBuy(food, c->amount) * (1.0 + fee));
    // ...and more than bringing them from next door costs.
    const Market& next = sim.SystemById(sc.next)->market;
    CHECK(c->reward > next.QuoteBuy(food, c->amount) * (1.0 + fee));

    // One a market, and none again for what is already asked for.
    const size_t open = sim.Contracts().size();
    sim.PostContracts();
    CHECK(sim.Contracts().size() == open);
}

TEST_CASE("a shortage the whole neighbourhood shares posts nothing (#343)")
{
    GoodsFile  file;
    Rules      r;
    Simulation sim;
    StartServer(sim);
    const ResourceType food = Good::Named("Food");
    const Scene        sc = ShortOfFood(sim, 1.0e6);
    for (const auto& kv : sim.Systems())
    {
        Market& m = sim.SystemById(kv.first)->market;
        m.SetStock(food, 0.2 * m.Target(food));
    }
    // Every market short alike: buying next door costs what the contract would pay.
    sim.PostContracts();
    CHECK(ContractFor(sim, sc.system, food) == nullptr);
    CHECK(sim.MindOf(sc.owner).treasury == doctest::Approx(1.0e6));
}

TEST_CASE("one pilot takes a contract and hands it in docked there: goods in, price out (#343)")
{
    GoodsFile  file;
    Rules      r;
    Simulation sim;
    StartServer(sim);
    const ResourceType food = Good::Named("Food");
    const Scene        sc = ShortOfFood(sim, 1.0e6);
    sim.PostContracts();
    const Contract* c = ContractFor(sim, sc.system, food);
    REQUIRE(c != nullptr);
    const int    id = c->id;
    const int    amount = c->amount;
    const double reward = c->reward;

    ClientSession& s = DockedAt(sim, sc);
    const int      offer = OfferOf(s, id);
    REQUIRE(offer >= 0);
    CHECK(s.missions.Offers()[(size_t)offer].rewardMoney == doctest::Approx(reward));
    ClientSession& late = DockedAt(sim, sc);
    const int      lateOffer = OfferOf(late, id);
    REQUIRE(lateOffer >= 0);
    REQUIRE(sim.AcceptMission(s, s.missions.Offers()[(size_t)offer].offerId));
    CHECK(sim.Contracts().at(id).taken);
    // Off the board, and a stale offer of it is refused.
    CHECK_FALSE(sim.AcceptMission(late, late.missions.Offers()[(size_t)lateOffer].offerId));
    sim.GenerateDockOffers(late);
    CHECK(OfferOf(late, id) < 0);

    // Not without the goods.
    REQUIRE(s.missions.Active().size() == 1);
    CHECK_FALSE(sim.MissionCompletableNow(s, s.missions.Active()[0]));
    CHECK_FALSE(sim.CompleteMission(s, 0));

    s.ship->AddCargo(food, amount);
    const double money = s.account.GetMoney();
    const double stock = sim.SystemById(sc.system)->market.Stock(food);
    CHECK(sim.MissionCompletableNow(s, s.missions.Active()[0]));
    REQUIRE(sim.CompleteMission(s, 0));
    CHECK(s.account.GetMoney() == doctest::Approx(money + reward));
    CHECK(s.ship->GetCargoAmount(food) == 0);
    CHECK(sim.SystemById(sc.system)->market.Stock(food) == doctest::Approx(stock + amount));
    CHECK(sim.Contracts().count(id) == 0);
    CHECK(s.missions.Active().empty());
    // Paid out of what was set aside: the treasury does not pay it twice.
    CHECK(sim.MindOf(sc.owner).treasury == doctest::Approx(1.0e6 - reward));
}

TEST_CASE("a contract that ends gives its price back, and a pilot out of time fails it (#343)")
{
    GoodsFile  file;
    Rules      r;
    Simulation sim;
    StartServer(sim);
    const ResourceType food = Good::Named("Food");
    const Scene        sc = ShortOfFood(sim, 1.0e6);
    sim.PostContracts();
    const Contract* c = ContractFor(sim, sc.system, food);
    REQUIRE(c != nullptr);
    const int    id = c->id;
    const double deadline = c->deadline;

    // Untaken, and the market no longer short: withdrawn, the price back.
    {
        Market&      m = sim.SystemById(sc.system)->market;
        const double was = m.Stock(food);
        m.SetStock(food, m.Target(food));
        sim.StepContracts();
        CHECK(sim.Contracts().count(id) == 0);
        CHECK(sim.MindOf(sc.owner).treasury == doctest::Approx(1.0e6));
        m.SetStock(food, was);
    }

    sim.PostContracts();
    c = ContractFor(sim, sc.system, food);
    REQUIRE(c != nullptr);
    const int      taken = c->id;
    ClientSession& s = DockedAt(sim, sc);
    const int      offer = OfferOf(s, taken);
    REQUIRE(offer >= 0);
    REQUIRE(sim.AcceptMission(s, s.missions.Offers()[(size_t)offer].offerId));
    const float standing = s.account.GetReputation(sc.owner);
    sim.SetTime(deadline + 10.0);
    sim.StepContracts();
    CHECK(s.missions.Active().empty());
    CHECK(s.account.GetReputation(sc.owner) < standing);
    CHECK(sim.Contracts().count(taken) == 0);
    // Its price is the owner's again -- less what the market, still short, has set aside since.
    double aside = 0.0;
    for (const auto& kv : sim.Contracts())
        aside += kv.second.faction == sc.owner ? kv.second.reward : 0.0;
    CHECK(sim.MindOf(sc.owner).treasury + aside == doctest::Approx(1.0e6));
}

TEST_CASE("contracts survive the save, and a pilot's with them (#343)")
{
    GoodsFile          file;
    Rules              r;
    const ResourceType food = Good::Named("Food");
    TempDir            dir;
    const std::string  world = dir.Path("world.json");
    const std::string  pilot = dir.Path("pilot.json");
    Contract           was;
    double             treasury = 0.0;
    Scene              sc;
    {
        Simulation sim;
        StartServer(sim);
        sc = ShortOfFood(sim, 1.0e6);
        sim.PostContracts();
        const Contract* c = ContractFor(sim, sc.system, food);
        REQUIRE(c != nullptr);
        ClientSession& s = DockedAt(sim, sc);
        REQUIRE(sim.AcceptMission(s, s.missions.Offers()[(size_t)OfferOf(s, c->id)].offerId));
        was = *c;
        treasury = sim.MindOf(sc.owner).treasury;
        REQUIRE(sim.SaveWorld(world));
        REQUIRE(sim.SaveAccount(s, pilot));
    }
    Simulation sim;
    StartServer(sim, world);
    REQUIRE(sim.Contracts().count(was.id) == 1);
    const Contract& c = sim.Contracts().at(was.id);
    CHECK(c.faction == was.faction);
    CHECK(c.system == was.system);
    CHECK(c.station == was.station);
    CHECK(c.good == was.good);
    CHECK(c.amount == was.amount);
    CHECK(c.reward == doctest::Approx(was.reward));
    CHECK(c.deadline == doctest::Approx(was.deadline));
    CHECK(c.taken);
    CHECK(sim.MindOf(sc.owner).treasury == doctest::Approx(treasury).epsilon(1e-3));
    REQUIRE(sim.ContractStation(c) != nullptr);

    ClientSession& back = sim.CreateSession(sc.system, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    REQUIRE(sim.LoadAccount(back, pilot) == Save::Result::Ok);
    REQUIRE(back.missions.Active().size() == 1);
    const Mission& m = back.missions.Active()[0];
    CHECK(m.type == MissionType::Contract);
    CHECK(m.contractId == was.id);
    CHECK(m.destStationId == sim.ContractStation(c)->GetId());
    CHECK(m.deadline == doctest::Approx(was.deadline));

    // A world where the contract is over: the pilot's goes, and they are told.
    {
        std::ifstream  in(pilot);
        nlohmann::json j = nlohmann::json::parse(in);
        j["missions"][0]["contract"] = 999999;
        std::ofstream(pilot) << j.dump();
    }
    ClientSession& gone = sim.CreateSession(sc.system, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    REQUIRE(sim.LoadAccount(gone, pilot) == Save::Result::Ok);
    CHECK(gone.missions.Active().empty());
}

TEST_CASE("a contract crosses the wire, and an agent reads where, when and what it needs (#343)")
{
    GoodsFile       file;
    Proto::Snapshot snap;
    snap.time = 1000.0;
    Proto::MissionView m;
    m.type = (int)MissionType::Contract;
    m.faction = Factions::Unaffiliated();
    m.title = "Contract: 10 Food to Vela Hub";
    m.resource = Good::Named("Food").Index();
    m.targetCount = 10;
    m.rewardMoney = 420.0;
    m.destStationId = 77;
    m.systemId = "r1-2-3";
    m.systemName = "Vela";
    m.deadline = 1600.0;
    snap.missionActive.push_back(m);
    snap.player.cargoByType.assign((size_t)Goods::Count(), 0);
    snap.player.cargoByType[(size_t)m.resource] = 4;

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    REQUIRE(back.missionActive.size() == 1);
    const Proto::MissionView& got = back.missionActive[0];
    CHECK(got.type == (int)MissionType::Contract);
    CHECK(got.systemId == "r1-2-3");
    CHECK(got.systemName == "Vela");
    CHECK(got.destStationId == 77);
    CHECK(got.deadline == doctest::Approx(1600.0));

    Obs::View view;
    view.snapshot = &back;
    CHECK(Obs::MissionNeeds(view, got).find("carry 10 Food (the hold has 4)") == 0);
    const std::string text = Obs::DescribeMissions(view);
    CHECK(text.find("contract: Contract: 10 Food to Vela Hub") != std::string::npos);
    CHECK(text.find("due in 10 min") != std::string::npos);
    CHECK(text.find("hand in docked at") != std::string::npos);
}
