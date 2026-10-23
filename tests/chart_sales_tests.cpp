#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "factions_named.h"
#include "temp_dir.h"

#include <memory>
#include <string>

// Charts sold to factions (#346, #431 slice 16): a faction docked at buys what a pilot has of a
// system by how much its own picture of it changes, and the sale writes into that picture --
// so what it knows is worth nothing, an update pays for what is new, and it then decides on
// what it bought.

namespace
{

const std::string SYSTEMS = std::string(TEST_WORLD_DIR) + "systems/";

// The design's chart rules, whatever the last test set; restored when it ends.
struct Rules
{
    WorldRules::Charts charts = WorldRules::ChartRules();
    Rules() { WorldRules::SetChartRules(WorldRules::Charts{}); }
    ~Rules() { WorldRules::SetChartRules(charts); }
};

// Known space and the region of seed 7 beyond its wormhole, as the #431 tests have it: home
// (the Guild's Helios Core), the system the wormhole leads to, and one a gate further in.
struct World
{
    Rules       rules;
    Simulation  sim;
    std::string home, entry, deeper;

    World()
    {
        REQUIRE(Factions::Load(std::string(TEST_DATA_DIR) + "factions.json"));
        REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
        REQUIRE(Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json"));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.AttachRegion(7, SYSTEMS);
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(SYSTEMS);
        home = sim.Universe().startId;
        for (const auto& l : sim.Universe().links)
            if (l.a == home && Gen::DepthOf(l.b) == 1)
                entry = l.b;
            else if (l.b == home && Gen::DepthOf(l.a) == 1)
                entry = l.a;
        for (const auto& l : sim.Universe().links)
            if ((l.a == entry || l.b == entry) && Gen::DepthOf(l.a == entry ? l.b : l.a) == 2)
                deeper = l.a == entry ? l.b : l.a;
        REQUIRE_FALSE(entry.empty());
        REQUIRE_FALSE(deeper.empty());
        // Nobody has a picture of the far system yet, and the Guild has money to buy one.
        Forget(deeper);
        sim.MindOf(Fac::TradersGuild()).treasury = 100000.0;
        sim.MindOf(Fac::Syndicate()).treasury = 100000.0;
    }

    void Forget(const std::string& system)
    {
        for (FactionId f : Factions::All())
            sim.MindOf(f).intel.erase(system);
    }

    ClientSession& Pilot(const std::string& name)
    {
        ClientSession& s =
            sim.CreateSession(home, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = name;
        s.ship->SetPilotName(name);
        return s;
    }

    // A coarse pass: what a pilot is looking at goes onto their record.
    void Pass() { sim.MaintainWorld(2.0f); }

    // `s` flies out to `system` and looks, then comes home.
    void Visit(ClientSession& s, const std::string& system)
    {
        s.dockedStationId = 0;
        sim.ServerEnterSystem(s, system, s.systemId);
        Pass();
        sim.ServerEnterSystem(s, home, system);
        Pass();
    }

    int StationOf(const std::string& system, FactionId f)
    {
        for (const auto& e : sim.SystemById(system)->entities)
            if (e->GetKind() == EntityKind::Station &&
                static_cast<const Station&>(*e).GetFaction() == f)
                return e->GetId();
        return 0;
    }

    // Docked at the Guild's station at home.
    void DockAtGuild(ClientSession& s)
    {
        REQUIRE(s.systemId == home);
        s.dockedStationId = StationOf(home, Fac::TradersGuild());
        REQUIRE(s.dockedStationId != 0);
    }

    int Build(const std::string& system, const std::string& name, const std::string& archetype,
              Vector2 at)
    {
        const int id =
            sim.AddStatic(system, std::make_unique<Structure>(at, 0.0f, name, archetype), "zed");
        REQUIRE(id != 0);
        return id;
    }

    int Offer(const ClientSession& s, FactionId f, const std::string& system)
    {
        return sim.ChartOfferFor(s, f, system).price;
    }
};

}  // namespace

TEST_CASE("a faction buys a system it never saw, once; the sale is in its picture (#431 slice 16)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    w.Build(w.deeper, "Zed's Beacon", "structure.beacon", { 5000.0f, 5000.0f });
    w.Visit(ann, w.deeper);
    w.Forget(w.deeper);
    const FactionId guild = Fac::TradersGuild();

    // In flight, nobody buys.
    CHECK_FALSE(w.sim.SellCharts(ann, w.deeper));
    w.DockAtGuild(ann);
    FactionId buyer;
    REQUIRE(w.sim.ChartBuyer(ann, buyer));
    CHECK(buyer == guild);

    // News, and what stood there: the system, and the beacon.
    const Simulation::ChartOffer o = w.sim.ChartOfferFor(ann, guild, w.deeper);
    CHECK(o.news);
    CHECK(o.changed == 1);
    CHECK(o.finds == 0);
    const WorldRules::Charts r;
    CHECK(o.price == doctest::Approx(r.system + r.structure).epsilon(0.02));
    CHECK(o.What() == "new system, 1 change");
    // The map is told, docked here.
    const Proto::GalaxyState g = w.sim.BuildGalaxyState(ann);
    CHECK(g.chartBuyer);
    CHECK(g.chartFaction == guild);
    bool listed = false;
    for (const auto& s : g.systems)
        listed = listed || (s.id == w.deeper && s.chartPrice == o.price);
    CHECK(listed);
    CHECK(Proto::EncodeGalaxy(g).find("\"chart\"") != std::string::npos);

    const double money = ann.account.GetMoney();
    const double treasury = w.sim.MindOf(guild).treasury;
    REQUIRE(w.sim.SellCharts(ann, w.deeper));
    CHECK(ann.account.GetMoney() == doctest::Approx(money + o.price));
    CHECK(w.sim.MindOf(guild).treasury == doctest::Approx(treasury - o.price));

    // It is in the Guild's picture now, as of when she looked -- and its plans read that:
    // a fresh picture is not worth surveying.
    const auto known = w.sim.MindOf(guild).intel.find(w.deeper);
    REQUIRE(known != w.sim.MindOf(guild).intel.end());
    CHECK(known->second.structures == 1);
    CHECK(known->second.seenAt == doctest::Approx(ann.sightings.at(w.deeper).at));
    CHECK(Intelligence::SurveyValue(1.0f, &known->second, w.sim.Time()) == 0.0f);
    // ...and it charts it: her find is public from now on.
    CHECK(w.sim.OnPublicCharts(w.deeper));

    // Again: worth nothing, and refused.
    CHECK(w.Offer(ann, guild, w.deeper) == 0);
    CHECK_FALSE(w.sim.SellCharts(ann, w.deeper));
    CHECK(ann.journal.back().text.find("knows all you have") != std::string::npos);
    CHECK(ann.account.GetMoney() == doctest::Approx(money + o.price));

    // Another faction's picture is its own: the Syndicate still pays, a little less for the
    // Guild knowing it too.
    const int syndicate = w.Offer(ann, Fac::Syndicate(), w.deeper);
    CHECK(syndicate > 0);
    CHECK(syndicate < o.price);
}

TEST_CASE("an update sells for what is new only (#431 slice 16)")
{
    World           w;
    ClientSession&  ann = w.Pilot("ann");
    const FactionId guild = Fac::TradersGuild();
    w.Visit(ann, w.deeper);
    w.Forget(w.deeper);
    w.DockAtGuild(ann);
    REQUIRE(w.sim.SellCharts(ann, w.deeper));
    CHECK(w.Offer(ann, guild, w.deeper) == 0);

    // Somebody builds out there while she is away; she goes to look.
    w.Build(w.deeper, "Zed's Beacon", "structure.beacon", { 5000.0f, 5000.0f });
    w.sim.SetTime(w.sim.Time() + 60.0);
    w.Visit(ann, w.deeper);
    w.DockAtGuild(ann);

    const Simulation::ChartOffer o = w.sim.ChartOfferFor(ann, guild, w.deeper);
    CHECK_FALSE(o.news);
    CHECK(o.changed == 1);
    const WorldRules::Charts r;
    CHECK(o.price > 0);
    CHECK(o.price <= (int)r.structure);
    REQUIRE(w.sim.SellCharts(ann, w.deeper));
    CHECK(w.sim.MindOf(guild).intel.at(w.deeper).structures == 1);
    CHECK(w.Offer(ann, guild, w.deeper) == 0);

    // An old look is worth less than a fresh one.
    WorldRules::Charts halving;
    halving.halfLifeHours = 1.0;
    WorldRules::SetChartRules(halving);
    w.Forget(w.deeper);
    const int fresh = w.Offer(ann, guild, w.deeper);
    w.sim.SetTime(w.sim.Time() + 3600.0);
    CHECK(w.Offer(ann, guild, w.deeper) < fresh);
}

TEST_CASE("what a pilot's scans found raises the price; the faction is told of it once "
          "(#346, #431 slice 16)")
{
    World           w;
    ClientSession&  ann = w.Pilot("ann");
    const FactionId guild = Fac::TradersGuild();
    const int       base =
        w.Build(w.deeper, "Den", "structure.outpost_hidden", Vector2{ 300000.0f, 0.0f });
    w.Visit(ann, w.deeper);
    w.Forget(w.deeper);
    w.DockAtGuild(ann);
    const int plain = w.Offer(ann, guild, w.deeper);

    // Her scan resolved the base: a find, not one more structure.
    const SystemState& st = *w.sim.SystemById(w.deeper);
    std::string        key;
    for (const auto& e : st.entities)
        if (e->GetId() == base)
            key = w.sim.SignatureKey(st, *e);
    REQUIRE_FALSE(key.empty());
    ann.known.insert(key);
    const Simulation::ChartOffer o = w.sim.ChartOfferFor(ann, guild, w.deeper);
    CHECK(o.finds == 1);
    CHECK(o.changed == 0);
    const WorldRules::Charts r;
    CHECK(o.price == doctest::Approx(plain + r.find).epsilon(0.02));
    CHECK(o.What() == "new system, 1 hidden find");

    REQUIRE(w.sim.SellCharts(ann, "*"));
    CHECK(w.sim.MindOf(guild).finds.count(key) == 1);
    CHECK(w.Offer(ann, guild, w.deeper) == 0);
    // A save keeps what it was told.
    TempDir           dir;
    const std::string path = dir.Path("world.json");
    REQUIRE(w.sim.SaveWorld(path));
    Simulation again;
    again.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    again.AttachRegion(7, SYSTEMS);
    again.Seed(1234u);
    REQUIRE(again.LoadWorld(path) == Save::Result::Ok);
    CHECK(again.MindOf(guild).finds.count(key) == 1);
}

TEST_CASE("only the pilot's own charts sell, and a faction's treasury bounds what it buys "
          "(#431 slice 16)")
{
    World            w;
    ClientSession&   ann = w.Pilot("ann");
    ClientSession&   bob = w.Pilot("bob");
    Corps::Registry& reg = w.sim.Corporations();
    REQUIRE(reg.Create("ann", "Far Reach", "FAR", 1000).Ok());
    REQUIRE(reg.InviteTo("ann", "bob", { "ann", "bob" }, 1000).Ok());
    REQUIRE(reg.Join("bob", "FAR", 1001).Ok());
    w.Visit(ann, w.deeper);
    w.Forget(w.deeper);
    const FactionId guild = Fac::TradersGuild();

    // Bob has it on his map, through the corporation; it is not his to sell.
    CHECK(w.sim.Knows(bob, w.deeper));
    w.DockAtGuild(bob);
    CHECK(w.Offer(bob, guild, w.deeper) == 0);
    CHECK_FALSE(w.sim.SellCharts(bob, w.deeper));

    // A faction with no money buys nothing, and says so.
    w.DockAtGuild(ann);
    w.sim.MindOf(guild).treasury = 10.0;
    CHECK_FALSE(w.sim.SellCharts(ann, w.deeper));
    CHECK(ann.journal.back().text.find("treasury") != std::string::npos);
    CHECK(w.sim.MindOf(guild).intel.count(w.deeper) == 0);

    // Where it holds, it sees for itself.
    CHECK(w.Offer(ann, guild, w.home) == 0);
}

TEST_CASE("the chart rules read, and a misspelt one is an error (#431 slice 16)")
{
    Rules r;
    REQUIRE(WorldRules::Load(std::string(TEST_DATA_DIR) + "world_rules.json"));
    CHECK(WorldRules::ChartRules().find > WorldRules::ChartRules().structure);
    TempDir           dir;
    const std::string path = dir.Path("rules.json");
    {
        nlohmann::json j = { { "tide", nlohmann::json::object() },
                             { "charts", { { "sytem", 1 } } } };
        std::FILE*     f = std::fopen(path.c_str(), "w");
        REQUIRE(f != nullptr);
        const std::string text = j.dump();
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    CHECK_FALSE(WorldRules::Load(path));
    CHECK(WorldRules::Error().find("sytem") != std::string::npos);
}

TEST_CASE("a chart sale and the faction's prices survive the wire (#431 slice 16)")
{
    Proto::Command c;
    c.sellChart = "r7-1-2";
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.sellChart == "r7-1-2");

    Proto::GalaxyState g;
    g.chartBuyer = true;
    g.chartFaction = Fac::Syndicate();
    g.chartTreasury = 4200.0;
    Proto::GalaxySystemStat s;
    s.id = "r7-1-2";
    s.chartPrice = 360;
    s.chartWhat = "new system, 1 change";
    g.systems.push_back(s);
    Proto::GalaxyState got;
    REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(g), got));
    CHECK(got.chartBuyer);
    CHECK(got.chartFaction == Fac::Syndicate());
    CHECK(got.chartTreasury == doctest::Approx(4200.0));
    REQUIRE(got.systems.size() == 1);
    CHECK(got.systems[0].chartPrice == 360);
    CHECK(got.systems[0].chartWhat == "new system, 1 change");

    Proto::GalaxyState none;
    REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(Proto::GalaxyState{}), none));
    CHECK_FALSE(none.chartBuyer);
}
