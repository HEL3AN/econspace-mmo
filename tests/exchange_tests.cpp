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
#include "sim/Corporation.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "sim/SaveSchema.h"

#include "factions_named.h"
#include "temp_dir.h"
#include "goods_named.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Exchange between pilots (#473, documents/player_exchange.md), slice 1: a direct trade of goods,
// credits and charts between two pilots docked at one station. The server is the escrow: what a
// side confirms is what it saw, the swap is all or nothing, nothing is made or lost, and a chart
// is a copy that says where it came from.

namespace
{

const std::string SYSTEMS = std::string(TEST_WORLD_DIR) + "systems/";

struct World
{
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
        for (FactionId f : Factions::All())
            sim.MindOf(f).intel.erase(deeper);
    }

    ClientSession& Pilot(const std::string& name, double money = 5000.0)
    {
        ClientSession& s =
            sim.CreateSession(home, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = name;
        s.ship->SetPilotName(name);
        s.account.SetMoney(money);
        return s;
    }

    int GuildStation()
    {
        for (const auto& e : sim.SystemById(home)->entities)
            if (e->GetKind() == EntityKind::Station &&
                static_cast<const Station&>(*e).GetFaction() == Fac::TradersGuild())
                return e->GetId();
        return 0;
    }

    void Dock(ClientSession& s)
    {
        s.dockedStationId = GuildStation();
        REQUIRE(s.dockedStationId != 0);
    }

    void Visit(ClientSession& s, const std::string& system)
    {
        s.dockedStationId = 0;
        sim.ServerEnterSystem(s, system, s.systemId);
        sim.MaintainWorld(2.0f);
        sim.ServerEnterSystem(s, home, system);
        sim.MaintainWorld(2.0f);
    }

    void Act(ClientSession& s, const std::string& action, const std::string& with = "",
             Exchange::Bundle bundle = {}, int rev = 0)
    {
        Proto::Act::Deal d;
        d.action = action;
        d.with = with;
        d.bundle = std::move(bundle);
        d.rev = rev;
        std::vector<Proto::TradeAck> acks;
        sim.ApplyAction(s, d, acks);
    }

    // Both ask for each other: an open trade.
    void Open(ClientSession& a, ClientSession& b)
    {
        Act(a, "open", b.accountName);
        Act(b, "open", a.accountName);
        REQUIRE(sim.TradeViewFor(a).with == b.accountName);
    }

    int Rev(const ClientSession& s) { return sim.TradeViewFor(s).rev; }

    void Confirm(ClientSession& s) { Act(s, "confirm", "", {}, Rev(s)); }

    static Exchange::Bundle Of(ResourceType t, int n)
    {
        Exchange::Bundle b;
        b.goods.push_back({ t.Index(), n, "" });
        return b;
    }
    static Exchange::Bundle Credits(long long c)
    {
        Exchange::Bundle b;
        b.credits = c;
        return b;
    }
};

bool Told(const ClientSession& s, const std::string& text)
{
    for (const Ev::Event& e : s.journal)
        if (e.text.find(text) != std::string::npos)
            return true;
    return false;
}

}  // namespace

TEST_CASE("a direct trade moves goods and credits at once, when both have confirmed (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 40));

    // A trade opens only when both ask.
    w.Act(ann, "open", "bob");
    CHECK(w.sim.TradeViewFor(ann).asking == "bob");
    CHECK(w.sim.TradeViewFor(bob).invites == std::vector<std::string>{ "ann" });
    CHECK(w.sim.TradeViewFor(ann).with.empty());
    w.Act(bob, "open", "ann");
    CHECK(w.sim.TradeViewFor(ann).with == "bob");
    CHECK(w.sim.TradeViewFor(bob).with == "ann");

    w.Act(ann, "put", "", w.Of(Good::Iron(), 25));
    w.Act(bob, "put", "", w.Credits(500));
    // The server describes both sides to both.
    const Proto::TradeView v = w.sim.TradeViewFor(bob);
    REQUIRE(v.theirs.goods.size() == 1);
    CHECK(v.theirs.goods[0].amount == 25);
    CHECK(v.mine.credits == 500);

    // ...and over the wire, in the snapshot.
    Proto::Snapshot snap = w.sim.BuildSnapshot(bob, bob.systemId);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    CHECK(back.player.trade.with == "ann");
    CHECK(back.player.trade.rev == v.rev);
    CHECK(back.player.trade.mine.credits == 500);
    REQUIRE(back.player.trade.theirs.goods.size() == 1);
    CHECK(back.player.trade.theirs.goods[0].amount == 25);

    const double annMoney = ann.account.GetMoney(), bobMoney = bob.account.GetMoney();
    const double treasury = w.sim.MindOf(Fac::TradersGuild()).treasury;
    w.Confirm(ann);
    // Nothing has moved while one side has not confirmed.
    CHECK(ann.ship->GetCargoAmount(Good::Iron()) == 40);
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) == 0);
    w.Confirm(bob);

    CHECK(ann.ship->GetCargoAmount(Good::Iron()) == 15);
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) == 25);
    const double fee = w.sim.TradeViewFor(ann).fee;  // each side's half, as it was shown
    (void)fee;
    // The fee went to the station's owner, and what the pilots paid is what it received.
    const double feePaid = w.sim.MindOf(Fac::TradersGuild()).treasury - treasury;
    CHECK(feePaid > 0.0);
    CHECK(ann.account.GetMoney() + bob.account.GetMoney() + feePaid ==
          doctest::Approx(annMoney + bobMoney));
    CHECK(ann.account.GetMoney() == doctest::Approx(annMoney + 500.0 - feePaid / 2.0));
    CHECK(bob.account.GetMoney() == doctest::Approx(bobMoney - 500.0 - feePaid / 2.0));
    CHECK(Told(ann, "Trade with bob done"));
    CHECK(Told(bob, "Trade with ann done"));
    CHECK_FALSE(w.sim.TakeTradeLog().empty());
    // The trade stays open and empty: the revision has moved on.
    CHECK(w.sim.TradeViewFor(ann).mine.goods.empty());
}

TEST_CASE("what is confirmed is what was seen: a change un-confirms both, an old revision is "
          "refused (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 40));
    w.Open(ann, bob);
    w.Act(ann, "put", "", w.Of(Good::Iron(), 10));
    const int seen = w.Rev(bob);

    w.Confirm(ann);
    CHECK(w.sim.TradeViewFor(bob).theirs.confirmed);
    // Ann swaps the offer after Bob looked: both are un-confirmed, the revision moves.
    w.Act(ann, "withdraw", "", w.Of(Good::Iron(), 10));
    w.Act(ann, "put", "", w.Of(Good::Iron(), 1));
    CHECK_FALSE(w.sim.TradeViewFor(bob).theirs.confirmed);
    CHECK(w.Rev(bob) != seen);

    // Bob's confirm of what he saw is refused -- even from a client that never looked again.
    w.Act(bob, "confirm", "", {}, seen);
    CHECK(Told(bob, "the offer changed"));
    CHECK_FALSE(w.sim.TradeViewFor(bob).mine.confirmed);
    CHECK(ann.ship->GetCargoAmount(Good::Iron()) == 40);
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) == 0);
}

TEST_CASE("a trade that fails a check changes nothing, and says why (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob", 100.0);
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 20));
    w.Open(ann, bob);

    // More than a pilot has is refused at the offer.
    w.Act(ann, "put", "", w.Of(Good::Iron(), 21));
    CHECK(w.sim.TradeViewFor(ann).mine.goods.empty());
    w.Act(bob, "put", "", w.Credits(101));
    CHECK(w.sim.TradeViewFor(bob).mine.credits == 0);

    // What was offered is gone by the time it is confirmed: the whole trade is off.
    w.Act(ann, "put", "", w.Of(Good::Iron(), 20));
    w.Act(bob, "put", "", w.Credits(50));
    REQUIRE(w.sim.MoveGoods(ann, "hold", "hangar", Good::Iron().Index(), 20) == 20);
    w.Confirm(bob);
    CHECK(Told(bob, "no longer has 20 Iron"));
    CHECK_FALSE(w.sim.TradeViewFor(bob).mine.confirmed);
    CHECK(bob.account.GetMoney() == doctest::Approx(100.0));
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) == 0);
}

TEST_CASE("what a full hold cannot take goes to the hangar here, and a trade with no room anywhere "
          "is refused whole (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 20));
    while (bob.ship->AddCargo(Good::Ice(), 1))  // a full hold
    {
    }
    w.Open(ann, bob);
    w.Act(ann, "put", "", w.Of(Good::Iron(), 20));
    w.Confirm(ann);
    w.Confirm(bob);
    CHECK(ann.ship->GetCargoAmount(Good::Iron()) == 0);
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) == 0);
    CHECK(w.sim.HangarViewFor(bob).goods[(size_t)Good::Iron().Index()] == 20);
    CHECK(Told(bob, "Trade with ann done"));
}

TEST_CASE("a trade is between two docked pilots, and ends when they part (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    ClientSession& cy = w.Pilot("cy");
    // Not docked: refused.
    w.Act(ann, "open", "bob");
    CHECK(Told(ann, "dock at a station first"));
    w.Dock(ann);
    w.Dock(bob);
    w.Act(ann, "open", "nobody");
    CHECK(Told(ann, "no pilot of that name"));
    w.Act(ann, "open", "cy");
    CHECK(Told(ann, "cy is not docked"));
    w.Dock(cy);
    w.Open(ann, bob);
    // A pilot already trading is not asked.
    w.Act(cy, "open", "bob");
    CHECK(Told(cy, "already trading with ann"));

    // Undocking ends it, for both.
    w.sim.StepPlayerUndock(ann);
    CHECK(w.sim.TradeViewFor(bob).with.empty());
    CHECK(Told(bob, "ended"));

    // A pilot who leaves the server ends it too.
    w.Dock(ann);
    w.Open(ann, bob);
    w.sim.DestroySession(ann.id);
    CHECK(w.sim.TradeViewFor(bob).with.empty());
}

TEST_CASE("a chart is a copy: the seller keeps theirs, the buyer is told what is new, and it says "
          "where it came from (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Visit(ann, w.deeper);
    REQUIRE(ann.known.count(w.sim.ChartKey(w.deeper)) == 1);
    REQUIRE_FALSE(w.sim.Knows(bob, w.deeper));
    w.Dock(ann);
    w.Dock(bob);
    w.Open(ann, bob);

    Exchange::Bundle chart;
    chart.charts.push_back(w.deeper);
    w.Act(ann, "put", "", chart);
    w.Act(bob, "put", "", w.Credits(300));
    // Bob is told what is new to him, and not where it is.
    const Proto::TradeView v = w.sim.TradeViewFor(bob);
    REQUIRE(v.theirs.charts.size() == 1);
    CHECK(v.theirs.charts[0].system.empty());
    CHECK(v.theirs.charts[0].text.find("new to you: 1 system") != std::string::npos);
    CHECK(v.theirs.charts[0].text.find(w.sim.SystemName(w.deeper)) == std::string::npos);
    // Ann sees her own, named, with how common it is.
    CHECK(w.sim.TradeViewFor(ann).mine.charts[0].system == w.deeper);

    w.Confirm(ann);
    w.Confirm(bob);
    CHECK(w.sim.Knows(bob, w.deeper));
    CHECK(ann.known.count(w.sim.ChartKey(w.deeper)) == 1);  // the seller keeps theirs
    REQUIRE(bob.secondHand.count(w.deeper) == 1);
    CHECK(bob.secondHand.at(w.deeper).from == "ann");
    CHECK(bob.secondHand.at(w.deeper).hops == 1);
    CHECK(ann.chartSales.at(w.deeper) == 1);
    CHECK(ann.secondHand.count(w.deeper) == 0);
    // A copy is no find: no faction pays for it, and a sale of it is no first contact.
    const Simulation::ChartOffer o = w.sim.ChartOfferFor(bob, Fac::TradersGuild(), w.deeper);
    CHECK(o.price == 0);
    CHECK(o.why.find("you bought this chart") != std::string::npos);

    // Offered again, it is known to the buyer: nothing new, and the listing says so; it can
    // be resold to a pilot as a second-hand copy, one hop further.
    ClientSession& cy = w.Pilot("cy");
    w.Dock(cy);
    w.Act(bob, "cancel");
    w.Open(bob, cy);
    w.Act(bob, "put", "", chart);
    const Proto::TradeView resale = w.sim.TradeViewFor(cy);
    CHECK(resale.theirs.charts[0].text.find("second-hand, copied once") != std::string::npos);
    w.Confirm(bob);
    w.Confirm(cy);
    CHECK(cy.secondHand.at(w.deeper).hops == 2);
    CHECK(cy.secondHand.at(w.deeper).from == "bob");

    // A visit of their own makes it theirs.
    w.Visit(bob, w.deeper);
    CHECK(bob.secondHand.count(w.deeper) == 0);

    // The mark is kept with the account: a copy read back is still a copy.
    const TempDir     tmp;
    const std::string path = tmp.Path("cy.json");
    REQUIRE(w.sim.SaveAccount(cy, path));
    ClientSession& back = w.Pilot("cy2");
    REQUIRE(w.sim.LoadAccount(back, path) == Save::Result::Ok);
    REQUIRE(back.secondHand.count(w.deeper) == 1);
    CHECK(back.secondHand.at(w.deeper).from == "bob");
    CHECK(back.secondHand.at(w.deeper).hops == 2);
}

TEST_CASE("a pilot trades only what is on their own charts (#473)")
{
    World          w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    w.Open(ann, bob);
    Exchange::Bundle chart;
    chart.charts.push_back(w.deeper);
    w.Act(ann, "put", "", chart);  // never been there
    CHECK(w.sim.TradeViewFor(ann).mine.charts.empty());
    CHECK(Told(ann, "is not on your own charts"));
    chart.charts = { w.home };  // public space has nothing to sell
    w.Act(ann, "put", "", chart);
    CHECK(w.sim.TradeViewFor(ann).mine.charts.empty());
}

TEST_CASE("nothing is made or lost across any run of trades (#473)")
{
    World                       w;
    std::vector<ClientSession*> pilots = { &w.Pilot("ann", 10000.0), &w.Pilot("bob", 10000.0),
                                           &w.Pilot("cy", 10000.0) };
    for (ClientSession* p : pilots)
    {
        w.Dock(*p);
        REQUIRE(p->ship->AddCargo(Good::Iron(), 30));
        REQUIRE(p->ship->AddCargo(Good::Ice(), 10));
    }
    const FactionId guild = Fac::TradersGuild();
    auto            total = [&](ResourceType t)
    {
        long long n = 0;
        for (ClientSession* p : pilots)  // the hold, and the hangar what spilled into
            n += p->ship->GetCargoAmount(t) + w.sim.HangarViewFor(*p).goods[(size_t)t.Index()];
        return n;
    };
    auto money = [&]()
    {
        double m = w.sim.MindOf(guild).treasury;
        for (ClientSession* p : pilots)
            m += p->account.GetMoney();
        return m;
    };
    const long long iron = total(Good::Iron()), ice = total(Good::Ice());
    const double    cash = money();

    // A fixed pseudo-random walk: who trades with whom, and what is put in.
    std::uint32_t rng = 12345u;
    auto          next = [&](std::uint32_t n)
    {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    int done = 0;
    for (int i = 0; i < 300; ++i)
    {
        ClientSession& a = *pilots[next(3)];
        ClientSession& b = *pilots[next(3)];
        if (&a == &b)
            continue;
        w.Act(a, "open", b.accountName);
        w.Act(b, "open", a.accountName);
        for (int k = 0; k < 3; ++k)
        {
            ClientSession& who = next(2) ? a : b;
            const auto     what = next(3);
            if (what == 0)
                w.Act(who, "put", "", w.Of(Good::Iron(), (int)next(25) + 1));
            else if (what == 1)
                w.Act(who, "put", "", w.Of(Good::Ice(), (int)next(12) + 1));
            else
                w.Act(who, "put", "", w.Credits((long long)next(1500) + 1));
        }
        w.Confirm(a);
        w.Confirm(b);
        if (Told(a, "done"))
            done++;
        w.Act(a, "cancel");
        for (ClientSession* p : pilots)
            p->journal.clear();
        CHECK(total(Good::Iron()) == iron);
        CHECK(total(Good::Ice()) == ice);
        CHECK(money() == doctest::Approx(cash));
    }
    CHECK(done > 20);  // the walk is not all refusals
}
