#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Defence.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

#include "factions_named.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>

// Influence and claims (#431 slice 5): what a party weighs in a system, how a corporation
// comes to hold one and loses it, and what holding it brings -- the tax at its markets, which
// goes to its treasury, and who docks at its structures. A player's market pays its owner
// (#438). The contest is the coarse pass's, read from the profile and the aggregate alone.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

constexpr double NOW = 1.8e9;  // seconds, as Unix time

void LoadRegistries()
{
    REQUIRE(Factions::Load(DataFile("factions.json")));
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
    WorldRules::SetInfluenceRules(WorldRules::Influence{});  // the design's numbers
}

void StartServer(Simulation& sim, const std::string& worldPath = std::string())
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

// The fringe system, with its Independent station and nobody's ships: what is weighed there
// is only what a test puts there.
struct Fringe
{
    Simulation        sim;
    const std::string sys = "verge";
    float             x = 100000.0f;

    Fringe()
    {
        LoadRegistries();
        StartServer(sim);
        for (FactionId f : Factions::All())
            Agg().presence[f] = 0.0f;
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
        REQUIRE(r.Join("boris", "DH", NOW).Ok());
        REQUIRE(r.Create("dima", "Kestrel Union", "KX", NOW).Ok());
    }
    SystemState&     St() { return *sim.SystemById(sys); }
    SystemAggregate& Agg() { return St().agg; }
    std::string      Id(const std::string& account) { return sim.Corporations().IdOf(account); }

    // A finished structure of `blueprint`, owned by `owner`, somewhere free.
    int Build(const std::string& blueprint, const std::string& owner)
    {
        const Blueprint* bp = Blueprints::Find(blueprint);
        REQUIRE(bp != nullptr);
        x += 20000.0f;
        auto t = std::make_unique<Structure>(Vector2{ x, -300000.0f }, 0.0f,
                                             blueprint + " " + owner, bp->archetype);
        t->SetBlueprint(blueprint);
        const int id = sim.AddStatic(sys, std::move(t), owner);
        REQUIRE(id != 0);
        return id;
    }
    // The macro passes that settle a contest, and one more.
    void Contest(int passes = Simulation::ContestPasses() + 1)
    {
        for (int i = 0; i < passes; i++)
            sim.StepControl(false);
    }
    const Entity* ById(int id)
    {
        for (const auto& e : St().entities)
            if (e->GetId() == id)
                return e.get();
        return nullptr;
    }
    ClientSession& Pilot(const std::string& account)
    {
        ClientSession& s = sim.CreateSession(sys, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = account;
        return s;
    }
};
}  // namespace

TEST_CASE("influence: structures by tier, a faction's presence, a pilot's for their corporation "
          "(#431 T1)")
{
    Fringe                       f;
    const WorldRules::Influence& w = WorldRules::InfluenceRules();
    const InfluenceMap           before = f.sim.InfluenceIn(f.St());
    // The Independents' station weighs for them; nobody else has anything here.
    CHECK(before.faction[Fac::Independent()] == doctest::Approx(w.station));
    CHECK(before.corp.empty());

    f.Build("outpost", "anna");
    f.Build("outpost_market", "boris");
    f.Build("beacon", "boris");  // not of a claim's tier: it weighs nothing
    f.Build("outpost", "cara");  // in no corporation: a corporation of one
    f.Agg().presence[Fac::Syndicate()] = 4.0f;
    const InfluenceMap m = f.sim.InfluenceIn(f.St());
    CHECK(m.corp.at(f.Id("anna")) == doctest::Approx(w.outpost + w.market));
    CHECK(m.corpClaims.count(f.Id("anna")) == 1);
    CHECK(m.corp.at(std::string(InfluenceMap::PILOT) + "cara") == doctest::Approx(w.outpost));
    CHECK(m.corpClaims.size() == 1);  // a pilot in none weighs, but holds nothing
    CHECK(m.faction[Fac::Syndicate()] == doctest::Approx(4.0f * w.perShip));

    SUBCASE("a site going up is nothing yet")
    {
        const Blueprint* bp = Blueprints::Find("outpost");
        auto t = std::make_unique<Structure>(Vector2{ -300000.0f, 300000.0f }, 0.0f, "Site",
                                             bp->archetype);
        t->SetBlueprint("outpost");
        t->StartBuilding(f.sim.Time(), f.sim.Time() + 1.0e6);
        REQUIRE(f.sim.AddStatic(f.sys, std::move(t), "dima") != 0);
        CHECK(f.sim.InfluenceIn(f.St()).corp.count(f.Id("dima")) == 0);
    }
    SUBCASE("...and leaving the corporation takes a pilot's weight with them")
    {
        REQUIRE(f.sim.Corporations().Leave("boris").Ok());
        const InfluenceMap after = f.sim.InfluenceIn(f.St());
        CHECK(after.corp.at(f.Id("anna")) == doctest::Approx(w.outpost));
        CHECK(after.corp.at(std::string(InfluenceMap::PILOT) + "boris") ==
              doctest::Approx(w.market));
    }
}

TEST_CASE("a claim needs the most influence, held for the contest (#431 T2)")
{
    // The contest is the coarse pass's, reading the profile and the aggregate only: the same
    // for a system nobody is in as for one somebody is.
    Fringe f;
    f.Build("outpost", "anna");
    f.Contest();
    // An outpost against the Independents' station: the claim is there, the influence is not.
    CHECK(f.Agg().holderCorp.empty());
    CHECK(f.Agg().controller == Fac::Independent());

    // A market and a second outpost: 1 + 2 + 1 is the station's 4 -- a tie, which the holder
    // keeps.
    f.Build("outpost_market", "boris");
    f.Build("outpost", "boris");
    f.Contest();
    CHECK(f.Agg().holderCorp.empty());

    // One more and it outweighs them -- but only once it has for the whole contest.
    f.Build("outpost", "anna");
    f.Contest(Simulation::ContestPasses() - 1);
    CHECK(f.Agg().holderCorp.empty());
    f.Contest(1);
    CHECK(f.Agg().holderCorp == f.Id("anna"));
    CHECK_FALSE(f.Agg().claimed);  // no faction holds it now
    CHECK(f.sim.HeldBy(f.Id("anna")) == std::vector<std::string>{ f.sys });
    const Proto::GalaxyState g = f.sim.GalaxyStats();
    for (const Proto::GalaxySystemStat& s : g.systems)
        if (s.id == f.sys)
        {
            CHECK(s.holder == f.Id("anna"));
            CHECK(s.holderTicker == "DH");
        }
}

TEST_CASE("a contest flips the holder; destroying its claim drops it; a faction takes it back "
          "(#431 T2)")
{
    Fringe f;
    f.Agg().claimed = false;  // nobody's: the station is the world's to stand, not a claim
    f.Agg().controller = Factions::Unaffiliated();
    f.St().entities.erase(std::remove_if(f.St().entities.begin(), f.St().entities.end(),
                                         [](const std::unique_ptr<Entity>& e)
                                         { return e->GetKind() == EntityKind::Station; }),
                          f.St().entities.end());
    f.St().profile = Simulation::ProfileOf(f.St());
    const int claim = f.Build("outpost", "anna");
    f.Contest();
    REQUIRE(f.Agg().holderCorp == f.Id("anna"));

    SUBCASE("a rival with the same weight does not take it: a tie keeps the holder")
    {
        f.Build("outpost", "dima");
        f.Contest();
        CHECK(f.Agg().holderCorp == f.Id("anna"));
        f.Build("outpost", "dima");
        f.Contest();
        CHECK(f.Agg().holderCorp == f.Id("dima"));
        bool told = false;  // in the news: "Kestrel Union [KX] took <system> from Deep Haul [DH]"
        for (const auto& e : f.sim.Chronicle())
            told = told || (e.text.rfind("Kestrel Union [KX] took ", 0) == 0 &&
                            e.text.find(" from Deep Haul [DH]") != std::string::npos);
        CHECK(told);
    }
    SUBCASE("the claim brought down, nobody holds it")
    {
        REQUIRE(f.sim.RemoveStatic(f.sys, claim));
        f.Contest(1);
        CHECK(f.Agg().holderCorp.empty());
        CHECK_FALSE(f.Agg().claimed);
    }
    SUBCASE("a corporation disbanded holds nothing")
    {
        REQUIRE(f.sim.Corporations().Leave("boris").Ok());
        REQUIRE(f.sim.Corporations().Leave("anna").Ok());
        f.Contest(1);
        CHECK(f.Agg().holderCorp.empty());
    }
    SUBCASE("a faction with an outpost and more influence takes it")
    {
        f.Build("outpost", Outposts::OwnerOf(Fac::Syndicate()));
        f.Agg().presence[Fac::Syndicate()] = 1.0f;  // 1 + a quarter outweighs the outpost
        f.Contest();
        CHECK(f.Agg().holderCorp.empty());
        CHECK(f.Agg().claimed);
        CHECK(f.Agg().controller == Fac::Syndicate());
    }
}

TEST_CASE("the holder's tax is charged at its markets and goes to its treasury (#431 T3)")
{
    Fringe    f;
    const int market = f.Build("outpost_market", "boris");
    f.Build("outpost_market", "anna");
    f.Build("outpost", "anna");
    f.Contest();
    REQUIRE(f.Agg().holderCorp == f.Id("anna"));

    // A member sets it; a director or the CEO may, a member may not.
    Corps::Registry& r = f.sim.Corporations();
    CHECK_FALSE(r.Apply("boris", { "tax", "", "", "", "", "5" }, {}, NOW).Ok());
    CHECK_FALSE(r.Apply("anna", { "tax", "", "", "", "", "50" }, {}, NOW).Ok());  // over the cap
    REQUIRE(r.Apply("anna", { "tax", "", "", "", "", "5%" }, {}, NOW).Ok());
    CHECK(f.sim.FeeRateIn(f.St()) == doctest::Approx(0.05));

    ClientSession& dima = f.Pilot("dima");
    dima.dockedStationId = market;
    REQUIRE(dima.ship->AddCargo(Good::Iron(), 10));
    const double                       before = r.Find(f.Id("anna"))->treasury;
    const int                          money = r.MoneyRev();
    const Simulation::PlayerSellResult sold =
        f.sim.StepPlayerSell(dima, f.St(), Good::Iron().Index(), 10);
    REQUIRE(sold.sold == 10);
    CHECK(sold.fee == doctest::Approx((sold.revenue + sold.fee) * 0.05));
    CHECK(r.Find(f.Id("anna"))->treasury == doctest::Approx(before + sold.fee));
    CHECK(r.MoneyRev() != money);

    // Bought, too, at the holder's rate.
    dima.account.SetMoney(100000.0);
    const Simulation::PlayerBuyResult bought =
        f.sim.StepPlayerBuy(dima, f.St(), Good::Iron().Index(), 5);
    REQUIRE(bought.bought == 5);
    CHECK(bought.fee == doctest::Approx((bought.cost - bought.fee) * 0.05));
    CHECK(r.Find(f.Id("anna"))->treasury == doctest::Approx(before + sold.fee + bought.fee));

    SUBCASE("'standard' puts the server's fee back")
    {
        REQUIRE(r.Apply("anna", { "tax", "", "", "", "", "standard" }, {}, NOW).Ok());
        CHECK(f.sim.FeeRateIn(f.St()) == doctest::Approx(Goods::Rules().fee));
    }
}

TEST_CASE("a player's market pays its owner where no corporation holds the system (#438)")
{
    Fringe    f;
    const int corpMarket = f.Build("outpost_market", "boris");
    const int ownMarket = f.Build("outpost_market", "cara");  // in no corporation
    REQUIRE(f.Agg().holderCorp.empty());
    ClientSession& dima = f.Pilot("dima");
    REQUIRE(dima.ship->AddCargo(Good::Iron(), 20));

    // A member's: the corporation's treasury.
    dima.dockedStationId = corpMarket;
    const auto a = f.sim.StepPlayerSell(dima, f.St(), Good::Iron().Index(), 10);
    REQUIRE(a.fee > 0.0);
    CHECK(a.fee == doctest::Approx((a.revenue + a.fee) * Goods::Rules().fee));
    CHECK(f.sim.Corporations().Find(f.Id("anna"))->treasury == doctest::Approx(a.fee));

    // A pilot's in none, who is away: kept for them, and paid when they are next on.
    dima.dockedStationId = ownMarket;
    const auto b = f.sim.StepPlayerSell(dima, f.St(), Good::Iron().Index(), 10);
    REQUIRE(b.fee > 0.0);
    ClientSession& cara = f.Pilot("cara");
    const double   had = cara.account.GetMoney();
    f.sim.StepOwners();
    CHECK(cara.account.GetMoney() == doctest::Approx(had + b.fee));

    SUBCASE("...and kept over a restart")
    {
        TempDir dir;
        dima.ship->AddCargo(Good::Iron(), 10);
        f.sim.DestroySession(cara.id);
        const auto c = f.sim.StepPlayerSell(dima, f.St(), Good::Iron().Index(), 10);
        f.sim.SaveWorld(dir.Path("world.json"));
        Simulation back;
        StartServer(back, dir.Path("world.json"));
        ClientSession& again =
            back.CreateSession(f.sys, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        again.accountName = "cara";
        const double start = again.account.GetMoney();
        back.StepOwners();
        CHECK(again.account.GetMoney() == doctest::Approx(start + c.fee));
    }
}

TEST_CASE("the holder's docking policy at its structures (#431 T3)")
{
    Fringe    f;
    const int market = f.Build("outpost_market", "anna");
    f.Build("outpost_market", "boris");
    f.Build("outpost", "anna");
    const Entity* dock = f.ById(market);
    REQUIRE(dock != nullptr);
    ClientSession&   boris = f.Pilot("boris");
    ClientSession&   dima = f.Pilot("dima");
    ClientSession&   cara = f.Pilot("cara");
    Corps::Registry& r = f.sim.Corporations();
    REQUIRE(r.Apply("anna", { "docking", "", "", "", "", "own" }, {}, NOW).Ok());

    // Not held yet: the policy is a holder's, and anyone docks at a market.
    CHECK(f.sim.DockRefusal(dima, *dock).empty());
    f.Contest();
    REQUIRE(f.Agg().holderCorp == f.Id("anna"));

    CHECK(f.sim.DockRefusal(boris, *dock).empty());
    CHECK(f.sim.DockRefusal(dima, *dock).find("docks only members of Deep Haul") !=
          std::string::npos);

    // Allies: whoever its rules of engagement hold fire for.
    REQUIRE(r.Apply("boris", { "docking", "", "", "", "", "all" }, {}, NOW).Ok() == false);
    REQUIRE(r.Apply("anna", { "docking", "", "", "", "", "allies" }, {}, NOW).Ok());
    Defence d;
    d.players = { "cara" };
    for (auto& e : f.St().entities)
        if (e->GetId() == market)
            static_cast<Structure&>(*e).SetDefence(d);
    CHECK(f.sim.DockRefusal(cara, *dock).empty());
    CHECK_FALSE(f.sim.DockRefusal(dima, *dock).empty());

    REQUIRE(r.Apply("anna", { "docking", "", "", "", "", "all" }, {}, NOW).Ok());
    CHECK(f.sim.DockRefusal(dima, *dock).empty());
}

TEST_CASE("the holder, the treasury and the policies outlive a restart (#431 slice 5)")
{
    TempDir     dir;
    std::string id;
    {
        Fringe f;
        f.Build("outpost_market", "anna");
        f.Build("outpost_market", "boris");
        f.Build("outpost", "anna");
        f.Contest();
        REQUIRE(f.Agg().holderCorp == f.Id("anna"));
        id = f.Id("anna");
        Corps::Registry& r = f.sim.Corporations();
        REQUIRE(r.Apply("anna", { "tax", "", "", "", "", "7.5" }, {}, NOW).Ok());
        REQUIRE(r.Apply("anna", { "docking", "", "", "", "", "allies" }, {}, NOW).Ok());
        REQUIRE(r.Credit(id, 1234.5));
        REQUIRE(r.Save(dir.Path("corps.json")));
        f.sim.SaveWorld(dir.Path("world.json"));
    }
    Simulation back;
    StartServer(back, dir.Path("world.json"));
    REQUIRE(back.Corporations().Load(dir.Path("corps.json")) == Save::Result::Ok);
    CHECK(back.SystemById("verge")->agg.holderCorp == id);
    const Corps::Corp* c = back.Corporations().Find(id);
    REQUIRE(c != nullptr);
    CHECK(c->treasury == doctest::Approx(1234.5));
    CHECK(c->tax == doctest::Approx(0.075));
    CHECK(c->docking == Corps::Docking::Allies);
    CHECK(back.FeeRateIn(*back.SystemById("verge")) == doctest::Approx(0.075));
    // It still holds it on the next pass: the claim came back with the structures.
    back.StepControl(false);
    CHECK(back.SystemById("verge")->agg.holderCorp == id);

    SUBCASE("a damaged policy is a damaged file")
    {
        nlohmann::json j = back.Corporations().ToJson();
        j["corps"][0]["docking"] = "everyone";
        Corps::Registry r;
        std::string     why;
        CHECK_FALSE(r.FromJson(j, why));
    }
}

TEST_CASE("a policy on the wire, and a holder in the galaxy (#431 slice 5)")
{
    Proto::CorpAct a;
    a.verb = "tax";
    a.value = "5";
    Proto::CorpAct back;
    REQUIRE(Proto::DecodeCorpAct(Proto::EncodeCorpAct(a), back));
    CHECK(back.value == "5");

    Proto::CorpView v;
    v.id = "c1";
    v.name = "Deep Haul";
    v.ticker = "DH";
    v.treasury = 250.0;
    v.tax = 0.05f;
    v.docking = "own";
    v.holds = { "verge" };
    Proto::CorpView vb;
    REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(v), vb));
    CHECK(vb.treasury == doctest::Approx(250.0));
    CHECK(vb.tax == doctest::Approx(0.05f));
    CHECK(vb.docking == "own");
    CHECK(vb.holds == std::vector<std::string>{ "verge" });

    Proto::GalaxyState      g;
    Proto::GalaxySystemStat s;
    s.id = "verge";
    s.holder = "c1";
    s.holderTicker = "DH";
    s.holderName = "Deep Haul";
    s.fee = 0.05f;
    g.systems.push_back(s);
    g.systems.push_back(Proto::GalaxySystemStat{});
    Proto::GalaxyState gb;
    REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(g), gb));
    REQUIRE(gb.systems.size() == 2);
    CHECK(gb.systems[0].holderName == "Deep Haul");
    CHECK(gb.systems[0].FeeOr(0.02f) == doctest::Approx(0.05f));
    CHECK(gb.systems[1].holder.empty());
    CHECK(gb.systems[1].FeeOr(0.02f) == doctest::Approx(0.02f));
}
