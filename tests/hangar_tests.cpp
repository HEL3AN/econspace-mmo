#include <doctest/doctest.h>

#include "factions_named.h"
#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/InventoryList.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

#include <memory>
#include <string>

// Personal hangars at NPC stations (#472): every pilot has the free units at a station that
// keeps storage, rents more as they choose, pays it every period of world time to the
// station's faction, and an unpaid rent closes the hangar until it is paid -- nothing lost.

namespace
{
const std::string DATA = TEST_DATA_DIR;
constexpr double  DAY = 86400.0;

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

bool Told(const ClientSession& s, int seq, const std::string& text)
{
    for (const Ev::Event& e : s.EventsSince(seq))
        if (e.text.find(text) != std::string::npos)
            return true;
    return false;
}

// A pilot docked at a trade hub of the Traders Guild, set down for the test.
struct Docked
{
    Simulation     sim;
    ClientSession* s = nullptr;
    SystemState*   st = nullptr;

    Docked()
    {
        LoadRegistries();
        WorldRules::SetHangarRules(WorldRules::Hangars());
        StartServer(sim);
        s = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
        s->accountName = "hunter";
        st = sim.SystemOf(*s);
        auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                 Fac::TradersGuild(), StationRole::TradeHub);
        station->SetId(990001);
        st->entities.push_back(std::move(station));
        REQUIRE(sim.StepPlayerDock(*s, *st) == 990001);
        s->account.SetMoney(10000.0);
    }
    const Proto::HangarView View() const { return sim.HangarViewFor(*s); }
};
}  // namespace

TEST_CASE(
    "Hangars: a station keeps one for every pilot, the free units in it, moved into by one verb")
{
    Docked                     d;
    const WorldRules::Hangars& r = WorldRules::HangarRules();
    Proto::HangarView          v = d.View();
    REQUIRE(v.capacity == r.freeUnits);
    CHECK(v.free == r.freeUnits);
    CHECK(v.rented == 0);
    CHECK(v.most > r.freeUnits);  // a trade hub keeps far more than the free units

    REQUIRE(d.s->ship->AddCargo(Good::Iron(), 40));
    CHECK(d.sim.MoveGoods(*d.s, "hold", "hangar", Good::Iron().Index(), 40) == 40);
    CHECK(d.View().goods[(size_t)Good::Iron().Index()] == 40);
    CHECK(d.s->ship->GetCargoAmount(Good::Iron()) == 0);

    // The client lists it, from the snapshot, over the wire.
    Proto::Snapshot snap = d.sim.BuildSnapshot(*d.s, d.s->systemId);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    CHECK(back.player.hangar.capacity == r.freeUnits);
    const auto                      all = InventoryList::Reachable(back.player);
    const InventoryList::Container* h = InventoryList::Find(all, "hangar");
    REQUIRE(h != nullptr);
    CHECK(h->Amount(Good::Iron()) == 40);

    // Undocked, it is out of reach.
    d.sim.StepPlayerUndock(*d.s);
    const int seq = d.s->LastEventSeq();
    CHECK(d.sim.MoveGoods(*d.s, "hangar", "hold", Good::Iron().Index(), 1) == 0);
    CHECK(Told(*d.s, seq, "dock at a station"));
    CHECK(d.View().capacity == 0);
}

TEST_CASE("Hangars: a purchase goes into the hangar, and a sale comes out of it")
{
    Docked             d;
    const ResourceType ice = Good::Ice();
    const auto         r = d.sim.StepPlayerBuy(*d.s, *d.st, ice.Index(), 60, "hangar");
    CHECK(r.bought == 60);
    CHECK(d.s->ship->GetCargoAmount(ice) == 0);
    CHECK(d.View().goods[(size_t)ice.Index()] == 60);
    // More than the hold could take in one: the free units are the limit.
    const auto more = d.sim.StepPlayerBuy(*d.s, *d.st, ice.Index(), 1000, "hangar");
    CHECK(more.bought == WorldRules::HangarRules().freeUnits - 60);
    const auto sold = d.sim.StepPlayerSell(*d.s, *d.st, ice.Index(), 30, "hangar");
    CHECK(sold.sold == 30);
    CHECK(d.View().goods[(size_t)ice.Index()] == WorldRules::HangarRules().freeUnits - 30);
}

TEST_CASE(
    "Hangars: rent is units chosen, paid ahead, pro rata when raised, and never below what is held")
{
    Docked                     d;
    const WorldRules::Hangars& r = WorldRules::HangarRules();
    const double               period = r.periodDays * DAY;
    const double               money = d.s->account.GetMoney();

    CHECK(d.sim.RentHangar(*d.s, 200));
    CHECK(d.s->account.GetMoney() == doctest::Approx(money - 200 * r.rentPerUnit));
    CHECK(d.View().capacity == r.freeUnits + 200);
    CHECK(d.View().paidUntil == doctest::Approx(d.sim.Time() + period));

    // Half a period on, 100 more pay for the half that is left.
    d.sim.SetTime(d.sim.Time() + period / 2);
    const double before = d.s->account.GetMoney();
    CHECK(d.sim.RentHangar(*d.s, 300));
    CHECK(d.s->account.GetMoney() == doctest::Approx(before - 100 * r.rentPerUnit * 0.5));

    // Filled past the free units, it cannot be cut below what it holds above them.
    REQUIRE(d.s->ship->AddCargo(Good::Iron(), 40));
    d.sim.StepPlayerBuy(*d.s, *d.st, Good::Ice().Index(), 200, "hangar");
    const int held = d.View().goods[(size_t)Good::Ice().Index()];
    REQUIRE(held > r.freeUnits);
    int seq = d.s->LastEventSeq();
    CHECK_FALSE(d.sim.RentHangar(*d.s, 0));
    CHECK(Told(*d.s, seq, "move them out first"));
    // Lowering returns nothing.
    const double now = d.s->account.GetMoney();
    CHECK(d.sim.RentHangar(*d.s, held - r.freeUnits));
    CHECK(d.s->account.GetMoney() == doctest::Approx(now));

    // Beyond what the station lets one pilot keep, refused.
    seq = d.s->LastEventSeq();
    CHECK_FALSE(d.sim.RentHangar(*d.s, d.View().most));
    CHECK(Told(*d.s, seq, "at most"));
}

TEST_CASE(
    "Hangars: rent falls due each period, to the station's faction; unpaid, it closes the hangar")
{
    Docked                     d;
    const WorldRules::Hangars& r = WorldRules::HangarRules();
    const double               period = r.periodDays * DAY;
    REQUIRE(d.sim.RentHangar(*d.s, 100));
    d.sim.StepPlayerBuy(*d.s, *d.st, Good::Ice().Index(), 150, "hangar");
    REQUIRE(d.View().goods[(size_t)Good::Ice().Index()] == 150);

    // Due, and paid.
    double money = d.s->account.GetMoney();
    d.sim.SetTime(d.View().paidUntil + 1.0);
    d.sim.StepHangars();
    CHECK(d.s->account.GetMoney() == doctest::Approx(money - 100 * r.rentPerUnit));
    CHECK(d.View().paidUntil > d.sim.Time() + period * 0.9);

    // Due, and not paid: the rent shrinks to what is held above the free units, that is owed,
    // and nothing goes in or out.
    d.s->account.SetMoney(0.0);
    d.sim.SetTime(d.View().paidUntil + 1.0);
    d.sim.StepHangars();
    Proto::HangarView v = d.View();
    CHECK(v.rented == 50);
    CHECK(v.owed == doctest::Approx(50 * r.rentPerUnit));
    CHECK(v.goods[(size_t)Good::Ice().Index()] == 150);  // nothing lost
    int seq = d.s->LastEventSeq();
    CHECK(d.sim.MoveGoods(*d.s, "hangar", "hold", Good::Ice().Index(), 1) == 0);
    CHECK(Told(*d.s, seq, "closed"));
    CHECK(InventoryList::Find(
              InventoryList::Reachable(d.sim.BuildSnapshot(*d.s, d.s->systemId).player), "hangar")
              ->closed);

    // Docking with the money pays it, and it is open again.
    d.s->account.SetMoney(1000.0);
    d.sim.StepPlayerUndock(*d.s);
    REQUIRE(d.sim.StepPlayerDock(*d.s, *d.st) == 990001);
    CHECK(d.View().owed == 0.0);
    CHECK(d.s->account.GetMoney() == doctest::Approx(1000.0 - 50 * r.rentPerUnit));
    CHECK(d.sim.MoveGoods(*d.s, "hangar", "hold", Good::Ice().Index(), 1) == 1);
}

TEST_CASE("Hangars: a hangar and its rent are saved with the world")
{
    Docked d;
    REQUIRE(d.sim.RentHangar(*d.s, 50));
    REQUIRE(d.s->ship->AddCargo(Good::Iron(), 30));
    REQUIRE(d.sim.MoveGoods(*d.s, "hold", "hangar", Good::Iron().Index(), 30) == 30);
    const Proto::HangarView v = d.View();

    const TempDir     tmp;
    const std::string path = tmp.Path("world_hangars.json");
    REQUIRE(d.sim.SaveWorld(path));
    Simulation b;
    StartServer(b, path);
    REQUIRE(b.Hangars().size() == 1);
    const auto& byAccount = b.Hangars().begin()->second;
    REQUIRE(byAccount.count("hunter") == 1);
    const Simulation::Hangar& h = byAccount.at("hunter");
    CHECK(h.goods.Amount(Good::Iron()) == 30);
    CHECK(h.rented == 50);
    CHECK(h.paidUntil == doctest::Approx(v.paidUntil));
    CHECK(h.holder == "f:" + Factions::Id(Fac::TradersGuild()));
}
