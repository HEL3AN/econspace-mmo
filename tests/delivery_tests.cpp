#include <doctest/doctest.h>

#include "factions_named.h"
#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Market.h"
#include "entities/Derelict.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include <cmath>
#include <memory>
#include <string>

// The NPC delivery service and what a destroyed structure leaves (#472, slice 6): a quote from
// the route alone, goods out of a hangar at a station that now hates the pilot brought to
// where they are at a surcharge paid to that station's faction, and half of what a destroyed
// structure kept dropping into its wreck.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

void StartServer(Simulation& sim)
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

struct World
{
    Simulation     sim;
    ClientSession* s = nullptr;
    SystemState*   st = nullptr;

    World()
    {
        LoadRegistries();
        WorldRules::SetHangarRules(WorldRules::Hangars());
        WorldRules::SetDeliveryRules(WorldRules::Deliveries());
        StartServer(sim);
        s = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
        s->accountName = "hunter";
        st = sim.SystemOf(*s);
        Add(990001, "Old Depot", Fac::TradersGuild());
        Add(990002, "Annex", Fac::Syndicate());
        s->account.SetMoney(10000.0);
    }
    void Add(int id, const char* name, FactionId f)
    {
        auto station =
            std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, name, f, StationRole::TradeHub);
        station->SetId(id);
        st->entities.push_back(std::move(station));
    }
};
}  // namespace

TEST_CASE("Delivery: a quote comes from the route, by the owner's formula (#472)")
{
    World                           w;
    const auto&                     r = WorldRules::DeliveryRules();
    const std::string               sys = w.st->id;
    const double                    sec = w.st->agg.security;
    const double                    risk = 1.0 + r.riskWeight * (1.0 - sec);
    const Simulation::DeliveryQuote q = w.sim.QuoteDelivery(sys, sys, 100, 1000.0);
    REQUIRE(q.ok);
    CHECK(q.jumps == 1);
    CHECK(q.risk == doctest::Approx(risk));
    CHECK(q.fee == doctest::Approx(std::ceil(std::max(r.minFee, r.rate * 100 * 1 * risk) +
                                             r.insure * 1000.0 * (risk - 1.0))));
    CHECK(q.hours == doctest::Approx(r.hoursPerJump));
    // The cheapest is the minimum fee, and the hostile surcharge multiplies it.
    CHECK(w.sim.QuoteDelivery(sys, sys, 1, 0.0).fee >= r.minFee);
    CHECK(w.sim.QuoteDelivery(sys, sys, 100, 1000.0, 1.5).fee ==
          doctest::Approx(std::ceil(q.fee * 1.5)).epsilon(0.01));
    // No route: said, and no fee.
    CHECK_FALSE(w.sim.QuoteDelivery(sys, "nowhere", 1, 0.0).ok);
}

TEST_CASE(
    "Delivery: goods out of a hangar at a station that refuses the pilot come by freight (#472)")
{
    World w;
    w.s->dockedStationId = 990001;
    REQUIRE(w.s->ship->AddCargo(Good::Iron(), 40));
    REQUIRE(w.sim.MoveGoods(*w.s, "hold", "hangar", Good::Iron().Index(), 40) == 40);
    // Docked elsewhere, the hangar is listed with the plain quote while the station lets the
    // pilot in: the service carries from any hangar (slice 11), at a surcharge only to a hater.
    w.s->dockedStationId = 990002;
    const Proto::PlayerView plain = w.sim.BuildSnapshot(*w.s, w.st->id).player;
    REQUIRE(plain.locked.size() == 1);
    CHECK_FALSE(plain.locked[0].hostile);
    const double plainFee = plain.locked[0].fee;
    int          seq = w.s->LastEventSeq();
    // ...and the one at the station they are docked at needs no freight.
    w.s->dockedStationId = 990001;
    CHECK(w.sim.BuildSnapshot(*w.s, w.st->id).player.locked.empty());
    CHECK_FALSE(w.sim.RequestDelivery(*w.s, w.sim.Hangars().begin()->first, -1, 0));
    w.s->dockedStationId = 990002;

    // The Traders Guild now hates the pilot: the hangar is listed, with its price.
    w.s->account.SetReputation(Fac::TradersGuild(), -100.0f);
    const Proto::PlayerView p = w.sim.BuildSnapshot(*w.s, w.st->id).player;
    REQUIRE(p.locked.size() == 1);
    CHECK(p.locked[0].name == "Old Depot");
    CHECK(p.locked[0].hostile);
    CHECK(p.locked[0].fee > plainFee);  // the hostility surcharge, on the same route
    const double fee = p.locked[0].fee;
    const double before = w.s->account.GetMoney();

    REQUIRE(w.sim.RequestDelivery(*w.s, p.locked[0].key, -1, 0));
    CHECK(w.s->account.GetMoney() == doctest::Approx(before - fee));
    const auto& hangars = w.sim.Hangars();
    CHECK(hangars.at(p.locked[0].key).at("hunter").goods.Empty());  // out of the hangar...
    CHECK(w.sim.BuildSnapshot(*w.s, w.st->id).player.shipments.size() == 1);  // ...in transit
    CHECK(w.sim.BuildSnapshot(*w.s, w.st->id).player.locked.empty());

    // Not there yet, then there when the clock says.
    w.sim.StepShipments();
    CHECK(w.sim.BuildSnapshot(*w.s, w.st->id).player.shipments.size() == 1);
    w.sim.SetTime(w.sim.Time() + p.locked[0].hours * 3600.0 + 1.0);
    w.sim.StepShipments();
    const Proto::PlayerView after = w.sim.BuildSnapshot(*w.s, w.st->id).player;
    CHECK(after.shipments.empty());
    CHECK(after.hangar.goods[(size_t)Good::Iron().Index()] == 40);
    (void)seq;
}

TEST_CASE("Delivery: a destroyed structure drops half of what it kept into its wreck (#472)")
{
    World            w;
    const Blueprint* bp = Blueprints::Find("outpost");
    REQUIRE(bp != nullptr);
    auto t = std::make_unique<Structure>(Vector2{ 120000.0f, -300000.0f }, 0.0f, "Hearth",
                                         bp->archetype);
    t->SetBlueprint(bp->id);
    t->AddStored(Good::Iron(), 40);
    t->HangarOf("hunter").Add(Good::Iron(), 10);
    const int id = w.sim.AddStatic(w.st->id, std::move(t), "hunter");
    REQUIRE(id != 0);
    CHECK(Blueprints::Rules().storageShare == doctest::Approx(0.5f));
    // Shot down in one hit.
    w.sim.DamageStructure(*w.st, id, 1e9f, nullptr);
    int found = 0;
    for (const auto& e : w.st->entities)
        if (e->GetKind() == EntityKind::Derelict)
            found += static_cast<const Derelict&>(*e).Contents().Amount(Good::Iron());
    CHECK(found == 25);  // half of 40 and half of 10
}
