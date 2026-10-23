#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Market.h"
#include "economy/Resource.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// The trade loop (#343): goods are data in a chain, a system's market keeps a stock its
// stations make and use, prices follow the stock, and a player buys as well as sells --
// paying the fee everybody pays (#348).

namespace
{
const std::string DATA = TEST_DATA_DIR;

// The goods of data/goods.json for one test, and the three ores again after it: the rest of
// the suite was written against the registry a process starts with.
struct GoodsFile
{
    GoodsFile() { REQUIRE(Goods::Load(DATA + "goods.json")); }
    ~GoodsFile() { Goods::Load(DATA + "goods.json"); }
};

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

void StartServer(Simulation& sim, const std::string& worldPath = std::string())
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

// The system of the fixture world whose stations refine ore (core has two mining stations).
std::string RefiningSystem(const Simulation& sim)
{
    for (const auto& kv : sim.Systems())
        for (const Recipe& r : kv.second.profile.rates.recipes)
            for (const auto& out : r.out)
                if (out.first == Good::Named("Steel"))
                    return kv.first;
    return std::string();
}

bool Writes(const std::string& path, const std::string& text)
{
    std::ofstream f(path);
    f << text;
    return (bool)f;
}
}  // namespace

TEST_CASE("goods are data: a chain of tiers, and a mistake in the file changes nothing (#343)")
{
    const int n0 = Goods::Count();
    Goods::ResetToBuiltin();
    CHECK(Goods::Count() ==
          3);  // the registry before any load: the three ores of the original game
    {
        GoodsFile file;
        const int n = Goods::Count();
        CHECK(n >= 6);
        CHECK(n <= 10);
        // Every good above the raw tier is made by some station from goods of a lower tier,
        // and every raw one comes from a belt or a planet: the chain has no loose end.
        for (ResourceType g : AllResourceTypes())
        {
            CAPTURE(Goods::Id(g));
            const GoodDef& d = Goods::Def(g);
            CHECK(d.price > 0.0);
            CHECK(Goods::Id(g) == d.id);
            if (d.tier == 0)
            {
                bool grown = d.mined;
                for (const char* planet : { "planet.oceanic", "planet.rocky", "planet.gas" })
                    if (const auto* y = Goods::PlanetYield(planet))
                        for (const auto& a : *y)
                            grown = grown || a.first == g;
                CHECK(grown);
                continue;
            }
            bool made = false;
            for (const Archetype& a : Archetypes::All())
                if (const Industry* ind = Goods::IndustryOf(a.id))
                    for (const Recipe& r : ind->recipes)
                        for (const auto& out : r.out)
                            if (out.first == g)
                            {
                                made = true;
                                for (const auto& in : r.in)
                                    CHECK(Goods::Def(in.first).tier < d.tier);
                            }
            CHECK(made);
        }
        CHECK(Goods::Mined().size() == 3);
        CHECK(Goods::Rules().fee > 0.0);  // #348: a fee for everybody

        // A name reads as the good, and so does an id.
        ResourceType paste;
        REQUIRE(Goods::Find("Repair Paste", paste));
        CHECK(Goods::Id(paste) == "RepairPaste");
        CHECK(ResourceFromName("RepairPaste") == paste);

        // Strict (#191): each of these is refused, and the registry stays the file's.
        const TempDir tmp;
        const char*   bad[] = {
            R"({"goods": [{"id": "Iron", "prise": 10}]})",
            R"({"goods": [{"id": "Iron"}, {"id": "Iron"}]})",
            R"({"goods": [{"id": "Iron"}], "industries": {"x": {"uses": {"Gold": 1}}}})",
            R"({"goods": [{"id": "Iron"}], "industries": {"x": {"recipes": [{"in": {"Iron": 1}}]}}})",
            R"({"goods": [{"id": "Iron", "supply": "far"}]})",
            R"({"goods": []})",
        };
        for (const char* text : bad)
        {
            CAPTURE(text);
            const std::string path = tmp.Path("goods.json");
            REQUIRE(Writes(path, text));
            CHECK_FALSE(Goods::Load(path));
            CHECK_FALSE(Goods::Error().empty());
            CHECK(Goods::Count() == n);
        }
    }
    CHECK(Goods::Count() == n0);  // and the file again once it is done: what every test runs on
}

TEST_CASE("a market's price follows its stock, and its recipes run on what is in stock (#343)")
{
    GoodsFile          file;
    const ResourceType iron = Good::Iron();
    const ResourceType steel = Good::Named("Steel");
    const ResourceType food = Good::Named("Food");

    Market m;
    CHECK(m.Stock(iron) == doctest::Approx(m.Target(iron)));
    CHECK(m.Mid(iron) == doctest::Approx(m.GetBasePrice(iron)));
    CHECK(m.Bid(iron) < m.Ask(iron));  // the spread

    // Buying takes stock and raises the price; the next unit costs more than the first.
    const double firstAsk = m.Ask(iron);
    int          bought = 0;
    const double cost = m.Buy(iron, 100, bought);
    CHECK(bought == 100);
    CHECK(cost > firstAsk * 100.0);
    CHECK(m.Ask(iron) > firstAsk);
    CHECK(m.Stock(iron) == doctest::Approx(m.Target(iron) - 100.0));
    // ...and nobody buys what is not there.
    m.Buy(iron, 1000000, bought);
    CHECK(bought == (int)m.Target(iron) - 100);
    CHECK(m.Stock(iron) < 1.0);
    CHECK(m.Ask(iron) <= m.GetBasePrice(iron) * Goods::Rules().ceiling * 1.06);

    // A refinery turns ore into steel only as far as there is ore, and people eat.
    Market      works;
    MarketRates rates;
    rates.recipes.push_back(Goods::IndustryOf("station.mining_outpost")->recipes[0]);
    rates.use.assign((size_t)Goods::Count(), 0.0);
    rates.use[(size_t)food.Index()] = 8.0;
    works.SetStock(iron, 30.0);
    const double steelBefore = works.Stock(steel);
    const double foodBefore = works.Stock(food);
    works.Step(rates, 1.0);  // an hour
    CHECK(works.Stock(iron) == doctest::Approx(0.0));
    CHECK(works.Stock(steel) == doctest::Approx(steelBefore + 10.0));  // 30 ore, 3 a bar
    CHECK(works.Stock(food) == doctest::Approx(foodBefore - 8.0));
    // With no ore, nothing more is made -- and nothing brings ore but a hauler (#343).
    works.Step(rates, 1.0);
    CHECK(works.Stock(steel) == doctest::Approx(steelBefore + 10.0));
    CHECK(works.Stock(iron) == doctest::Approx(0.0));

    // A hauler carries ore from a market that has plenty to this one, which has none: as much
    // as still pays, and never so much that the gap closes.
    Market plenty;
    plenty.SetStock(iron, plenty.Target(iron) * 4.0);
    const int cap = (int)Goods::Rules().haulCargo;
    REQUIRE(cap > 0);
    const int n = HaulAmount(plenty, works, iron, cap);
    CHECK(n > 0);
    CHECK(n <= cap);
    bought = 0;
    plenty.Buy(iron, n, bought);
    works.Sell(iron, n);
    CHECK(works.Bid(iron) * (1.0 - Goods::Rules().fee) >
          plenty.Ask(iron) * (1.0 + Goods::Rules().fee));
    // Nothing is carried the wrong way, nor between two markets alike.
    CHECK(HaulAmount(works, plenty, iron, cap) == 0);
    CHECK(HaulAmount(Market(), Market(), iron, cap) == 0);
}

TEST_CASE("buying at a station: the stock, the hold and the money decide, and the fee is paid "
          "(#343, #348)")
{
    GoodsFile file;
    LoadRegistries();
    Simulation sim;
    StartServer(sim);
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    SystemState& st = *sim.SystemOf(s);
    auto         station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                             Fac::TradersGuild(), StationRole::TradeHub);
    station->SetId(990001);
    st.entities.push_back(std::move(station));
    REQUIRE(sim.StepPlayerDock(s, st) == 990001);

    const ResourceType food = Good::Named("Food");
    s.account.SetMoney(10000.0);
    const double                      quote = st.market.QuoteBuy(food, 10);
    const double                      stock = st.market.Stock(food);
    const Simulation::PlayerBuyResult r = sim.StepPlayerBuy(s, st, food.Index(), 10);
    CHECK(r.bought == 10);
    CHECK(r.fee == doctest::Approx(quote * Goods::Rules().fee));
    CHECK(r.cost == doctest::Approx(quote * (1.0 + Goods::Rules().fee)));
    CHECK(s.account.GetMoney() == doctest::Approx(10000.0 - r.cost));
    CHECK(s.ship->GetCargoAmount(food) == 10);
    CHECK(st.market.Stock(food) == doctest::Approx(stock - 10.0));

    // Sold straight back, it fetches less than it cost: the spread and two fees.
    const Simulation::PlayerSellResult back = sim.StepPlayerSell(s, st, food.Index(), 10);
    CHECK(back.sold == 10);
    CHECK(back.fee > 0.0);
    CHECK(back.revenue < r.cost);
    CHECK(s.ship->GetCargoAmount(food) == 0);

    SUBCASE("as many as the money pays for")
    {
        s.account.SetMoney(st.market.Ask(food) * 3.5);
        const auto some = sim.StepPlayerBuy(s, st, food.Index(), 100);
        CHECK(some.bought >= 2);
        CHECK(some.bought <= 3);
        CHECK(s.account.GetMoney() >= 0.0);
    }
    SUBCASE("as many as the hold takes")
    {
        s.ship->AddCargo(Good::Iron(), s.ship->GetCargoCapacity() - 4);
        CHECK(sim.StepPlayerBuy(s, st, food.Index(), 100).bought == 4);
        const int seq = s.LastEventSeq();
        CHECK(sim.StepPlayerBuy(s, st, food.Index(), 1).bought == 0);
        bool said = false;
        for (const Ev::Event& e : s.EventsSince(seq))
            said = said || e.text.find("hold is full") != std::string::npos;
        CHECK(said);
    }
    SUBCASE("nothing in open space")
    {
        sim.StepPlayerUndock(s);
        CHECK(sim.StepPlayerBuy(s, st, food.Index(), 5).bought == 0);
        CHECK(s.ship->GetCargoAmount(food) == 0);
    }
}

TEST_CASE("stations make and use goods in the coarse pass, and the stock outlives a restart "
          "(#343)")
{
    GoodsFile file;
    LoadRegistries();
    const TempDir      tmp;
    const std::string  path = tmp.Path("world.json");
    const ResourceType steel = Good::Named("Steel");
    std::string        refinery;
    double             stock = 0.0;
    {
        Simulation sim;
        StartServer(sim);
        refinery = RefiningSystem(sim);
        REQUIRE_FALSE(refinery.empty());
        const SystemState& st = *sim.SystemById(refinery);
        CHECK(st.profile.belts > 0);  // its belts, which the coarse pass asks itself (#348)
        // Half an hour of the world with nobody in it: the refineries run on the belts' ore,
        // and the steel piles up past what the market keeps, cheaper for it.
        for (int i = 0; i < 900; i++)
            sim.MaintainWorld(2.0f);
        CHECK(st.market.Stock(steel) > st.market.Target(steel));
        CHECK(st.market.Mid(steel) < st.market.GetBasePrice(steel));
        stock = st.market.Stock(steel);
        REQUIRE(sim.SaveWorld(path));
    }
    Simulation again;
    StartServer(again, path);
    CHECK(again.SystemById(refinery)->market.Stock(steel) == doctest::Approx(stock).epsilon(1e-3));
}

TEST_CASE("an account's hold keeps any good by its id (#343)")
{
    GoodsFile file;
    LoadRegistries();
    Simulation sim;
    StartServer(sim);
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    const ResourceType medicine = Good::Named("Medicine");
    REQUIRE(s.ship->AddCargo(medicine, 6));
    const TempDir     tmp;
    const std::string path = tmp.Path("account.json");
    REQUIRE(sim.SaveAccount(s, path));
    {
        std::ifstream     f(path);
        const std::string text((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        CHECK(text.find("\"Medicine\"") != std::string::npos);
    }
    ClientSession& back =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    REQUIRE(sim.LoadAccount(back, path) == Save::Result::Ok);
    CHECK(back.ship->GetCargoAmount(medicine) == 6);
    CHECK(back.ship->GetCargoAmount(Good::Iron()) == 0);  // not read as ore, as version 5 would
    CHECK(Save::ACCOUNT_VERSION >= 6);
}

TEST_CASE("the wire carries a purchase and the market's bid, ask and stock (#343)")
{
    GoodsFile      file;
    Proto::Command c;
    c.buyType = Good::Named("Ammunition").Index();
    c.buyAmount = 12;
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.buyType == c.buyType);
    CHECK(back.buyAmount == 12);
    // A good the registry does not have is no purchase, not a purchase of another one.
    c.buyType = Goods::Count();
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.buyType == -1);

    Proto::Snapshot s;
    s.systemId = "core";
    s.marketPrices = { 9.5f, 14.25f };
    s.marketAsk = { 10.5f, 15.75f };
    s.marketStock = { 400, 0 };
    s.tradeAcks.push_back(Proto::TradeAck{ 1, 0, 0.0, 0.0, 5, 80.0, 1.6 });
    Proto::Snapshot r;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), r));
    REQUIRE(r.marketPrices.size() == 2);
    CHECK(r.marketAsk[1] == doctest::Approx(15.75f));
    CHECK(r.marketStock[0] == 400);
    CHECK(r.marketStock[1] == 0);
    REQUIRE(r.tradeAcks.size() == 1);
    CHECK(r.tradeAcks[0].bought == 5);
    CHECK(r.tradeAcks[0].cost == doctest::Approx(80.0));
    CHECK(r.tradeAcks[0].fee == doctest::Approx(1.6));
}
