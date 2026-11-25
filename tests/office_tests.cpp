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
#include "sim/Corporation.h"
#include "sim/InventoryList.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include <memory>
#include <string>

// A corporation's office at an NPC station (#572, slice 12): rented from the treasury, a store
// of the three divisions, locked when the rent is unpaid and never emptied.

namespace
{
const std::string DATA = TEST_DATA_DIR;
constexpr double  DAY = 86400.0;

struct Office
{
    Simulation     sim;
    ClientSession* s = nullptr;
    SystemState*   st = nullptr;
    std::string    corp;

    Office()
    {
        REQUIRE(Factions::Load(DATA + "factions.json"));
        REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
        REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        s = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
        s->accountName = "boss";
        st = sim.SystemOf(*s);
        auto station = std::make_unique<Station>(Vector2{ 0.0f, 0.0f }, 60.0f, "Depot",
                                                 Fac::TradersGuild(), StationRole::TradeHub);
        station->SetId(990001);
        st->entities.push_back(std::move(station));
        REQUIRE(sim.StepPlayerDock(*s, *st) == 990001);
        REQUIRE(sim.Corporations().Create("boss", "Office Holders", "OFC", 1000.0).Ok());
        corp = sim.Corporations().IdOf("boss");
        sim.Corporations().Credit(corp, 20000.0);
    }
    Corps::Outcome Act(const std::string& value = std::string())
    {
        Corps::Act a;
        a.verb = "office";
        a.value = value;
        return sim.ApplyOfficeAct("boss", a);
    }
    const Corps::Corp& Corp() const { return *sim.Corporations().Find(corp); }
};
}  // namespace

TEST_CASE(
    "Offices: a CEO rents one, the treasury pays the faction, and it holds goods in divisions")
{
    Office       o;
    const auto&  rules = WorldRules::OfficeRules();
    const double before = o.Corp().treasury;
    REQUIRE(o.Act().Ok());
    CHECK(o.Corp().treasury == doctest::Approx(before - rules.rent));
    REQUIRE(o.Corp().offices.size() == 1);
    CHECK_FALSE(o.Act().Ok());  // one office a station

    // Docked there, its divisions are containers like a structure's.
    REQUIRE(o.s->ship->AddCargo(Good::Iron(), 20));
    CHECK(o.sim.MoveGoods(*o.s, "hold", "storage", Good::Iron().Index(), 20) == 20);
    CHECK(o.s->ship->GetCargoAmount(Good::Iron()) == 0);
    Proto::Snapshot snap = o.sim.BuildSnapshot(*o.s, o.s->systemId);
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    const auto                      all = InventoryList::Reachable(back.player);
    const InventoryList::Container* shared = InventoryList::Find(all, "storage");
    REQUIRE(shared != nullptr);
    CHECK(shared->Amount(Good::Iron()) == 20);
    CHECK(InventoryList::Find(all, "storage:command") != nullptr);

    // What it holds is written with the world's corporations and read back.
    Corps::Registry again;
    std::string     why;
    REQUIRE(again.FromJson(o.sim.Corporations().ToJson(), why));
    const Corps::Corp* c = again.Find(o.corp);
    REQUIRE(c != nullptr);
    REQUIRE(c->offices.size() == 1);
    CHECK(c->offices.begin()->second.goods.at("shared").Amount(Good::Iron()) == 20);
}

TEST_CASE(
    "Offices: closing one with goods in it is refused, and unpaid it is locked, never emptied")
{
    Office      o;
    const auto& rules = WorldRules::OfficeRules();
    REQUIRE(o.Act().Ok());
    REQUIRE(o.s->ship->AddCargo(Good::Iron(), 10));
    REQUIRE(o.sim.MoveGoods(*o.s, "hold", "storage", Good::Iron().Index(), 10) == 10);
    CHECK_FALSE(o.Act("close").Ok());
    CHECK(o.Corp().offices.size() == 1);

    // The treasury cannot pay: after the grace day the office is locked.
    o.sim.Corporations().Debit(o.corp, o.Corp().treasury);
    o.sim.SetTime(o.sim.Time() + rules.periodDays * DAY + rules.graceHours * 3600.0 + 1.0);
    o.sim.StepOffices();
    REQUIRE(o.sim.OfficeHere(*o.s) != nullptr);
    CHECK(o.sim.OfficeLocked(*o.sim.OfficeHere(*o.s)));
    REQUIRE(o.s->ship->AddCargo(Good::Iron(), 5));
    CHECK(o.sim.MoveGoods(*o.s, "hold", "storage", Good::Iron().Index(), 5) == 0);    // none in
    CHECK(o.sim.MoveGoods(*o.s, "storage", "hold", Good::Iron().Index(), 10) == 10);  // out
    Proto::Snapshot snap = o.sim.BuildSnapshot(*o.s, o.s->systemId);
    REQUIRE(!snap.player.divisions.empty());
    CHECK_FALSE(snap.player.divisions[0].canPut);
    CHECK(snap.player.divisions[0].canTake);

    // Paid, it opens again; empty, it can be closed.
    o.sim.Corporations().Credit(o.corp, 10000.0);
    o.sim.StepOffices();
    CHECK_FALSE(o.sim.OfficeLocked(*o.sim.OfficeHere(*o.s)));
    CHECK(o.sim.MoveGoods(*o.s, "hold", "storage", Good::Iron().Index(), 5) == 5);
    REQUIRE(o.sim.MoveGoods(*o.s, "storage", "hold", Good::Iron().Index(), 5) == 5);
    CHECK(o.Act("close").Ok());
    CHECK(o.Corp().offices.empty());
}

TEST_CASE("Offices: only a CEO or a director rents one, and only at a station with a market")
{
    Office     o;
    Corps::Act a;
    a.verb = "office";
    CHECK_FALSE(o.sim.ApplyOfficeAct("nobody", a).Ok());  // in no corporation
    a.name = "No Such Station";
    CHECK_FALSE(o.sim.ApplyOfficeAct("boss", a).Ok());
}
