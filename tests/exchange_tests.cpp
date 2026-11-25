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
#include "sim/Observation.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "sim/SaveSchema.h"

#include "factions_named.h"
#include "temp_dir.h"
#include "goods_named.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// --- Exchange contracts (#473, slice 2): goods, credits and charts held at a station ---------

namespace
{
struct Desk : World
{
    // A contract through the verb.
    void Board(ClientSession& s, const std::string& action, Exchange::Bundle giving = {},
               Exchange::Bundle asking = {}, int id = 0, const std::string& to = "",
               bool corp = false, int days = 0)
    {
        Proto::Act::Board b;
        b.action = action;
        b.id = id;
        b.giving = std::move(giving);
        b.asking = std::move(asking);
        b.to = to;
        b.corp = corp;
        b.days = days;
        std::vector<Proto::TradeAck> acks;
        sim.ApplyAction(s, b, acks);
    }
    int OpenId() const
    {
        int id = 0;
        for (const auto& kv : sim.Contracts())
            if (kv.second.IsExchange())
                id = std::max(id, kv.first);
        return id;
    }
    int Hangar(const ClientSession& s, ResourceType t) const
    {
        return sim.HangarViewFor(s).goods[(size_t)t.Index()];
    }
};
}  // namespace

TEST_CASE("a pilot posts goods for credits and another takes them, the server holding both "
          "between (#473)")
{
    Desk           w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 40));
    const WorldRules::Exchange& rules = WorldRules::ExchangeRules();
    const double annMoney = ann.account.GetMoney(), bobMoney = bob.account.GetMoney();
    const double treasury = w.sim.MindOf(Fac::TradersGuild()).treasury;

    w.Board(ann, "post", w.Of(Good::Iron(), 30), w.Credits(600));
    const int id = w.OpenId();
    REQUIRE(id != 0);
    // Held: out of the hold, the posting fee paid to the station's owner.
    CHECK(ann.ship->GetCargoAmount(Good::Iron()) == 10);
    CHECK(ann.account.GetMoney() == doctest::Approx(annMoney - rules.postingFee));
    CHECK(w.sim.Contracts().at(id).giveGoods.Amount(Good::Iron()) == 30);
    CHECK(w.sim.MindOf(Fac::TradersGuild()).treasury ==
          doctest::Approx(treasury + rules.postingFee));

    // Bob reads it, as the server describes it, and over the wire.
    Proto::Snapshot snap = w.sim.BuildSnapshot(bob, bob.systemId);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    REQUIRE(back.player.board.size() == 1);
    const Proto::DealView& d = back.player.board[0];
    CHECK(d.issuer == "ann");
    CHECK(d.askCredits == 600);
    REQUIRE(d.give.size() == 1);
    CHECK(d.give[0].amount == 30);
    CHECK(d.canAccept);
    CHECK(d.fee > 0.0);

    // Ann may not take her own; bob takes it.
    w.Board(ann, "accept", {}, {}, id);
    CHECK(Told(ann, "yours: cancel it"));
    w.Board(bob, "accept", {}, {}, id);
    CHECK(w.sim.Contracts().count(id) == 0);
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) + w.Hangar(bob, Good::Iron()) == 30);
    // Whatever the market took of it went to the holder, and nothing else was lost.
    const double feePaid = w.sim.MindOf(Fac::TradersGuild()).treasury - treasury;
    CHECK(ann.account.GetMoney() + bob.account.GetMoney() + feePaid ==
          doctest::Approx(annMoney + bobMoney));
    CHECK(ann.account.GetMoney() == doctest::Approx(annMoney - rules.postingFee + 600.0));
    CHECK(Told(ann, "was taken by bob"));
    CHECK(Told(bob, "Took contract"));
}

TEST_CASE("an offline issuer is paid: goods into their hangar (#473)")
{
    Desk           w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 30));
    REQUIRE(bob.ship->AddCargo(Good::Ice(), 5));
    // Ann gives Iron and asks for Ice; she then goes away.
    w.Board(ann, "post", w.Of(Good::Iron(), 30), w.Of(Good::Ice(), 5));
    const int id = w.OpenId();
    REQUIRE(id != 0);
    w.sim.DestroySession(ann.id);
    w.Board(bob, "accept", {}, {}, id);
    CHECK(w.sim.Contracts().count(id) == 0);
    int ice = 0;
    for (const auto& place : w.sim.Hangars())
    {
        const auto h = place.second.find("ann");
        if (h != place.second.end())
            ice += h->second.goods.Amount(Good::Ice());
    }
    CHECK(ice == 5);
    CHECK(bob.ship->GetCargoAmount(Good::Iron()) + w.Hangar(bob, Good::Iron()) == 30);
}

TEST_CASE("a contract can be cancelled or can run out, and what it held goes back; audiences "
          "and the cap hold (#473)")
{
    Desk           w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    ClientSession& cy = w.Pilot("cy");
    for (ClientSession* p : { &ann, &bob, &cy })
        w.Dock(*p);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 40));
    const double annMoney = ann.account.GetMoney();
    const auto&  rules = WorldRules::ExchangeRules();

    // Cancelled: the goods and credits come back, the posting fee does not.
    Exchange::Bundle give = w.Of(Good::Iron(), 20);
    give.credits = 100;
    w.Board(ann, "post", give, w.Credits(50));
    const int first = w.OpenId();
    REQUIRE(first != 0);
    w.Board(bob, "cancel", {}, {}, first);
    CHECK(w.sim.Contracts().count(first) == 1);  // not bob's
    w.Board(ann, "cancel", {}, {}, first);
    CHECK(w.sim.Contracts().count(first) == 0);
    CHECK(ann.ship->GetCargoAmount(Good::Iron()) + w.Hangar(ann, Good::Iron()) == 40);
    CHECK(ann.account.GetMoney() == doctest::Approx(annMoney - rules.postingFee));

    // To a pilot: only that pilot takes it.
    w.Board(ann, "post", w.Of(Good::Iron(), 10), w.Credits(10), 0, "bob");
    const int second = w.OpenId();
    REQUIRE(second != 0);
    CHECK(w.sim.BoardFor(cy).empty());
    w.Board(cy, "accept", {}, {}, second);
    CHECK(Told(cy, "not offered to you"));
    CHECK(w.sim.BoardFor(bob).size() == 1);

    // Out of time: back to the poster, who is told.
    w.sim.SetTime(w.sim.Time() + (rules.defaultDays + 1.0) * 86400.0);
    w.sim.StepBoard();
    CHECK(w.sim.Contracts().count(second) == 0);
    CHECK(Told(ann, "ran out of time"));
    CHECK(ann.ship->GetCargoAmount(Good::Iron()) + w.Hangar(ann, Good::Iron()) == 40);

    // The cap on what one pilot keeps open.
    for (int i = 0; i < rules.maxOpen + 2; ++i)
        w.Board(ann, "post", w.Of(Good::Iron(), 1), w.Credits(5));
    int open = 0;
    for (const auto& kv : w.sim.Contracts())
        open += kv.second.IsExchange() ? 1 : 0;
    CHECK(open == rules.maxOpen);
    CHECK(Told(ann, "the most there may be"));
}

TEST_CASE("a contract's chart is a copy frozen when it was posted, and it is saved (#473)")
{
    Desk           w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Visit(ann, w.deeper);
    w.Dock(ann);
    w.Dock(bob);
    Exchange::Bundle give;
    give.charts.push_back(w.deeper);
    give.goods.push_back({ Good::Iron().Index(), 1, "" });
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 1));
    w.Board(ann, "post", give, w.Credits(200));
    const int id = w.OpenId();
    REQUIRE(id != 0);
    // Bob is told what is new to him, not where.
    const auto board = w.sim.BoardFor(bob);
    REQUIRE(board.size() == 1);
    REQUIRE(board[0].giveCharts.size() == 1);
    CHECK(board[0].giveCharts[0].find("new to you") != std::string::npos);
    CHECK(board[0].giveCharts[0].find(w.sim.SystemName(w.deeper)) == std::string::npos);

    // A restart loses nothing: what is held and what it asks come back as they were.
    const TempDir     tmp;
    const std::string path = tmp.Path("world_exchange.json");
    REQUIRE(w.sim.SaveWorld(path));
    Simulation b;
    b.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    b.AttachRegion(7, SYSTEMS);
    b.Seed(1234u);
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(SYSTEMS);
    REQUIRE(b.Contracts().count(id) == 1);
    const Contract& c = b.Contracts().at(id);
    CHECK(c.IsExchange());
    CHECK(c.poster == "ann");
    CHECK(c.giveGoods.Amount(Good::Iron()) == 1);
    CHECK(c.asking.credits == 200);
    REQUIRE(c.giveCharts.size() == 1);
    CHECK(c.giveCharts[0].system == w.deeper);

    // Bob, on the restarted server, takes it.
    ClientSession& bob2 = b.CreateSession(w.home, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    bob2.accountName = "bob";
    bob2.account.SetMoney(1000.0);
    bob2.dockedStationId = w.GuildStation();
    Proto::Act::Board take;
    take.action = "accept";
    take.id = id;
    std::vector<Proto::TradeAck> acks;
    b.ApplyAction(bob2, take, acks);
    CHECK(b.Contracts().count(id) == 0);
    CHECK(b.Knows(bob2, w.deeper));
    CHECK(bob2.secondHand.count(w.deeper) == 1);
}

TEST_CASE("a corporation posts from its office and treasury, and what it is paid goes back into "
          "them; one may be offered to a corporation (#473 slice 3)")
{
    Desk           w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    ClientSession& cy = w.Pilot("cy");
    ClientSession& di = w.Pilot("di");
    for (ClientSession* p : { &ann, &bob, &cy, &di })
        w.Dock(*p);
    Corps::Registry& corps = w.sim.Corporations();
    REQUIRE(corps.Create("ann", "Ann Works", "ANN", 1000.0).Ok());
    const std::string id = corps.IdOf("ann");
    corps.Credit(id, 20000.0);
    Corps::Act open;
    open.verb = "office";
    REQUIRE(w.sim.ApplyOfficeAct("ann", open).Ok());
    REQUIRE(w.sim.OfficeHere(ann) != nullptr);
    // A member who is not an officer cannot post for it.
    corps.InviteTo("ann", "bob", { "bob" }, 1000.0);
    REQUIRE(corps.Join("bob", "ANN", 1001.0).Ok());

    // Its goods in the office, its credits in the treasury.
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 40));
    REQUIRE(w.sim.MoveGoods(ann, "hold", "storage", Good::Iron().Index(), 40) == 40);
    w.Board(bob, "post", w.Of(Good::Iron(), 1), {}, 0, "", true);
    CHECK(Told(bob, "only a corporation's CEO or a director"));

    const double     treasury = corps.Find(id)->treasury;
    Exchange::Bundle give;
    give.goods.push_back({ Good::Iron().Index(), 25, "storage" });
    w.Board(ann, "post", give, w.Credits(900), 0, "", true);
    const int deal = w.OpenId();
    REQUIRE(deal != 0);
    const Contract& c = w.sim.Contracts().at(deal);
    CHECK(c.issuer == Corps::OwnerOf(id));
    CHECK(c.poster == "ann");
    CHECK(corps.Find(id)->offices.begin()->second.goods.at("shared").Amount(Good::Iron()) == 15);
    CHECK(corps.Find(id)->treasury ==
          doctest::Approx(treasury - WorldRules::ExchangeRules().postingFee));
    // Anyone docked here reads it as the corporation's.
    REQUIRE(w.sim.BoardFor(cy).size() == 1);
    CHECK(w.sim.BoardFor(cy)[0].issuer == "Ann Works [ANN]");
    CHECK(w.sim.BoardFor(ann)[0].mine);

    // Taken by an outsider: the treasury is paid.
    w.Board(cy, "accept", {}, {}, deal);
    CHECK(w.sim.Contracts().count(deal) == 0);
    CHECK(cy.ship->GetCargoAmount(Good::Iron()) + w.Hangar(cy, Good::Iron()) == 25);
    CHECK(corps.Find(id)->treasury > treasury);
    CHECK(Told(ann, "was taken by cy"));

    // Offered to the corporation: its members only; paid by it, from its office.
    ClientSession& di2 = di;
    REQUIRE(di2.ship->AddCargo(Good::Ice(), 10));
    w.Board(di2, "post", w.Of(Good::Ice(), 10), w.Credits(300), 0, "corp:ANN");
    const int offered = w.OpenId();
    REQUIRE(offered != 0);
    CHECK(w.sim.BoardFor(cy).empty());
    w.Board(cy, "accept", {}, {}, offered);
    CHECK(Told(cy, "not offered to you"));
    // An officer takes it for the corporation: the treasury pays, the office receives.
    w.Board(ann, "accept", {}, {}, offered, "", true);
    CHECK(w.sim.Contracts().count(offered) == 0);
    CHECK(corps.Find(id)->offices.begin()->second.goods.at("shared").Amount(Good::Ice()) == 10);
}

TEST_CASE("nothing is made or lost across any run of contracts, escrow counted (#473)")
{
    Desk                        w;
    std::vector<ClientSession*> pilots = { &w.Pilot("ann", 10000.0), &w.Pilot("bob", 10000.0),
                                           &w.Pilot("cy", 10000.0) };
    for (ClientSession* p : pilots)
    {
        w.Dock(*p);
        REQUIRE(p->ship->AddCargo(Good::Iron(), 30));
        REQUIRE(p->ship->AddCargo(Good::Ice(), 10));
    }
    const FactionId guild = Fac::TradersGuild();
    auto            goods = [&](ResourceType t)
    {
        long long n = 0;
        for (ClientSession* p : pilots)
            n += p->ship->GetCargoAmount(t) + w.Hangar(*p, t);
        for (const auto& kv : w.sim.Contracts())
            n += kv.second.giveGoods.Amount(t);
        return n;
    };
    auto money = [&]()
    {
        double m = w.sim.MindOf(guild).treasury;
        for (ClientSession* p : pilots)
            m += p->account.GetMoney();
        for (const auto& kv : w.sim.Contracts())
            m += (double)kv.second.giveCredits;
        return m;
    };
    const long long iron = goods(Good::Iron()), ice = goods(Good::Ice());
    // The posting fee goes to the Guild, which the sum counts; the escrow is counted in place.
    const double cash = money();

    std::uint32_t rng = 777u;
    auto          next = [&](std::uint32_t n)
    {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) % n;
    };
    int taken = 0;
    for (int i = 0; i < 400; ++i)
    {
        ClientSession& who = *pilots[next(3)];
        switch (next(4))
        {
            case 0:
            {
                Exchange::Bundle give = next(2) ? w.Of(Good::Iron(), (int)next(20) + 1)
                                                : w.Of(Good::Ice(), (int)next(8) + 1);
                if (next(3) == 0)
                    give.credits = (long long)next(400) + 1;
                Exchange::Bundle ask = next(2) ? w.Credits((long long)next(500) + 1)
                                               : w.Of(Good::Ice(), (int)next(5) + 1);
                w.Board(who, "post", give, ask);
                break;
            }
            case 1:
            case 2:
            {
                const int before = (int)w.sim.Contracts().size();
                w.Board(who, "accept", {}, {}, w.OpenId());
                taken += (int)w.sim.Contracts().size() < before ? 1 : 0;
                break;
            }
            default: w.Board(who, "cancel", {}, {}, w.OpenId()); break;
        }
        if (i % 100 == 99)
        {
            w.sim.SetTime(w.sim.Time() + 20.0 * 86400.0);  // all that is left runs out
            w.sim.StepBoard();
        }
        CHECK(goods(Good::Iron()) == iron);
        CHECK(goods(Good::Ice()) == ice);
        CHECK(money() == doctest::Approx(cash));
    }
    CHECK(taken > 5);
}

TEST_CASE("what a pilot sees of a trade and of the board reaches an agent too (#473)")
{
    Desk           w;
    ClientSession& ann = w.Pilot("ann");
    ClientSession& bob = w.Pilot("bob");
    w.Dock(ann);
    w.Dock(bob);
    REQUIRE(ann.ship->AddCargo(Good::Iron(), 30));
    w.Open(ann, bob);
    w.Act(ann, "put", "", w.Of(Good::Iron(), 5));
    w.Board(ann, "post", w.Of(Good::Iron(), 10), w.Credits(100));

    auto read = [&](ClientSession& s)
    {
        Proto::Snapshot snap = w.sim.BuildSnapshot(s, s.systemId);
        Proto::Snapshot back;
        REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
        Obs::View v;
        v.snapshot = &back;
        return std::make_pair(Obs::Describe(v, Obs::Detail::Brief),
                              Obs::DescribeBoard(back.player.board, back.time));
    };
    const auto [seen, board] = read(bob);
    CHECK(seen.find("TRADE  with ann, revision") != std::string::npos);
    CHECK(seen.find("ann offers: 5 Iron") != std::string::npos);
    CHECK(seen.find("CONTRACTS  1 exchange contract") != std::string::npos);
    CHECK(board.find("gives 10 Iron, for 100 cr") != std::string::npos);
    CHECK(board.find("you can take it") != std::string::npos);
    CHECK(read(ann).second.find("yours") != std::string::npos);
}
