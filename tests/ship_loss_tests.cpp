// Losing a ship (#344): the ship flown is lost with its fit and its cargo, a wreck lies where
// it fell, insurance pays what it covers, and the pilot wakes docked at home in the starter,
// which is free and always theirs.
#include "doctest/doctest.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/Derelict.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "gen/Galaxy.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "goods_named.h"

#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace
{
// A generated world with two civilisations to start in, and nothing flying.
struct World
{
    Simulation sim;

    World()
    {
        const std::string data = TEST_DATA_DIR;
        REQUIRE(Factions::Load(data + "factions.json"));
        REQUIRE(Archetypes::Load(data + "archetypes.json"));
        REQUIRE(Blueprints::Load(data + "blueprints.json"));
        Gen::WorldParams p;
        p.seed = 3;
        p.centres = 2;
        p.owners = Factions::MajorIds();
        REQUIRE(sim.OpenGalaxy(p, data));
        sim.InitGalaxy();
        sim.MaterializeAllSystems(data + "systems/");
        REQUIRE(sim.Starts().size() == 2);
    }

    // The trade hub a start arrives at.
    const Entity* Hub(const Simulation::StartPoint& sp) const
    {
        for (const auto& e : sim.Systems().at(sp.systemId).entities)
            if (e->GetKind() == EntityKind::Station && e->GetName() == sp.station)
                return e.get();
        return nullptr;
    }

    // A station with a shipyard, and its system.
    const Entity* Shipyard(std::string& systemId) const
    {
        for (const auto& kv : sim.Systems())
            for (const auto& e : kv.second.entities)
                if (e->GetKind() == EntityKind::Station && e->GetArchetype() != nullptr &&
                    e->GetArchetype()->Has(Component::Shipyard))
                {
                    systemId = kv.first;
                    return e.get();
                }
        return nullptr;
    }

    ClientSession& Pilot(const Simulation::StartPoint& sp)
    {
        ClientSession& s =
            sim.CreateSession(sp.systemId, sim.SafeArrival(sp.systemId), GetShipCatalog()[0].stats);
        s.accountName = "pilot";
        s.ship->SetPilotName("pilot");
        return s;
    }
};

const Derelict* WreckAt(const Simulation& sim, const std::string& systemId, Vector2 at)
{
    for (const auto& e : sim.Systems().at(systemId).entities)
        if (e->GetKind() == EntityKind::Derelict && e->GetPosition().x == at.x &&
            e->GetPosition().y == at.y)
            return static_cast<const Derelict*>(e.get());
    return nullptr;
}

constexpr int HAULER = 2;  // data/ships.json's hangar: scout, courier, hauler, miner
}  // namespace

TEST_CASE("a lost ship is gone with its fit and cargo; insurance pays; you wake at home (#344)")
{
    World                         w;
    const Simulation::StartPoint& start = w.sim.Starts()[1];
    const Entity*                 hub = w.Hub(start);
    REQUIRE(hub != nullptr);
    ClientSession& s = w.Pilot(start);
    // Home, until a shipyard is docked at, is the start's hub -- the second start's here.
    CHECK(s.homeStationId == hub->GetId());

    // A Hauler, bought and insured at the hub.
    s.account.SetMoney(10000.0);
    s.dockedStationId = hub->GetId();
    REQUIRE(w.sim.BuyShip(s, HAULER));
    const double value = GetShipCatalog()[HAULER].price;  // the stock fit adds nothing
    CHECK(w.sim.ValueOf(s, HAULER, *w.sim.SystemOf(s)) == doctest::Approx(value));
    const double before = s.account.GetMoney();
    REQUIRE(w.sim.InsureShip(s, HAULER));
    CHECK(s.CoverOf(HAULER) == doctest::Approx(value));
    CHECK(before - s.account.GetMoney() == doctest::Approx(std::ceil(value * INSURANCE_PREMIUM)));
    // Covered for what it is worth: nothing more to buy.
    CHECK_FALSE(w.sim.InsureShip(s, HAULER));
    // The starter as it comes is free, and needs none.
    CHECK_FALSE(w.sim.InsureShip(s, 0));

    // Out in another civilisation's space, with a hold of ore.
    s.dockedStationId = 0;
    const std::string there = w.sim.Starts()[0].systemId;
    s.systemId = there;
    w.sim.Warm(*w.sim.SystemById(there));
    const Vector2 fell = { 123400.0f, -56700.0f };
    s.ship->Teleport(fell);
    REQUIRE(s.ship->AddCargo(Good::Iron(), 20));
    REQUIRE(s.ship->AddCargo(Good::Ice(), 8));
    const double money = s.account.GetMoney();

    w.sim.ServerRespawnPlayer(s);

    // The Hauler is gone; the starter is what is left, stock, and empty.
    CHECK_FALSE(s.Owns(HAULER));
    CHECK(s.Owns(0));
    CHECK(s.currentShip == 0);
    CHECK(s.DesignOf(0) == GetShipCatalog()[0].stats.design);
    CHECK(s.ship->GetCargoCapacity() == GetShipCatalog()[0].stats.cargoCapacity);
    CHECK(s.ship->GetCargoUsed() == 0);
    // Insurance paid its share and is spent.
    CHECK(s.account.GetMoney() - money == doctest::Approx(std::floor(value * INSURANCE_PAYOUT)));
    CHECK(s.CoverOf(HAULER) == 0.0);
    // Awake at home: docked, in the home system.
    CHECK(s.systemId == start.systemId);
    CHECK(s.dockedStationId == hub->GetId());
    // A wreck where it fell, holding a share of the ship as credits and of the hold as the
    // goods themselves (#343), for a while: one wreck, holding every good (#472).
    const double    share = Blueprints::Rules().wreckShare;
    const Derelict* wreck = WreckAt(w.sim, there, fell);
    REQUIRE(wreck != nullptr);
    CHECK(wreck->GetReward() == doctest::Approx(std::floor(value * share)));
    CHECK(wreck->Contents().Amount(Good::Iron()) == (int)std::floor(20 * share));
    CHECK(wreck->Contents().Amount(Good::Ice()) == (int)std::floor(8 * share));
    int wrecks = 0;
    for (const auto& e : w.sim.SystemById(there)->entities)
        wrecks += e->GetKind() == EntityKind::Derelict &&
                  static_cast<const Derelict&>(*e).GetCargoAmount() > 0;
    CHECK(wrecks == 1);  // no pods beside it
    CHECK(wreck->GetExpiresAt() > 0.0);
    CHECK(wreck->GetBaseName() == "Wreck of pilot's Hauler");
    // And it is said: the journal, and the loss the snapshot carries.
    REQUIRE_FALSE(s.journal.empty());
    CHECK(s.journal.back().kind == Ev::Kind::ShipDestroyed);
    CHECK(s.journal.back().text.find("insurance paid") != std::string::npos);
    CHECK(s.lastLoss.seq == 1);
    CHECK(s.lastLoss.hull == "Hauler");
    CHECK(s.lastLoss.cargo == 28);
    CHECK(s.lastLoss.paid == doctest::Approx(std::floor(value * INSURANCE_PAYOUT)));
    CHECK(s.lastLoss.wokeAt.find(hub->GetName()) == 0);
    const Proto::Snapshot snap = w.sim.BuildSnapshot(s, s.systemId);
    CHECK(snap.player.loss.seq == 1);
    CHECK(snap.player.docked);
}

TEST_CASE("home is the last shipyard docked at; the starter is lost only as far as its fit (#344)")
{
    World          w;
    ClientSession& s = w.Pilot(w.sim.Starts()[0]);
    std::string    yardSystem;
    const Entity*  yard = w.Shipyard(yardSystem);
    REQUIRE(yard != nullptr);

    // Docking at a shipyard makes it home, and says so.
    s.systemId = yardSystem;
    w.sim.Warm(*w.sim.SystemById(yardSystem));
    s.ship->Teleport(yard->GetPosition());
    REQUIRE(w.sim.StepPlayerDock(s, *w.sim.SystemById(yardSystem)) == yard->GetId());
    CHECK(s.homeStationId == yard->GetId());
    w.sim.StepPlayerUndock(s);

    // A stock starter with an empty hold is worth nothing: no wreck, nothing to pay.
    const Vector2 fell = s.ship->GetPosition();
    const double  money = s.account.GetMoney();
    w.sim.ServerRespawnPlayer(s);
    CHECK(WreckAt(w.sim, yardSystem, fell) == nullptr);
    CHECK(s.account.GetMoney() == money);
    CHECK(s.Owns(0));
    CHECK(s.systemId == yardSystem);
    CHECK(s.dockedStationId == yard->GetId());

    // A home whose owner now hates the pilot will not take them: the start does.
    const FactionId owner = static_cast<const Station*>(yard)->GetFaction();
    s.account.SetReputation(owner, -100.0f);
    std::string sys;
    const int   home = w.sim.HomeOf(s, sys);
    CHECK(home != yard->GetId());
    if (home != 0)
    {
        const Entity* hub = w.Hub(w.sim.Starts()[0]);
        const Entity* hub2 = w.Hub(w.sim.Starts()[1]);
        CHECK(((hub != nullptr && home == hub->GetId()) ||
               (hub2 != nullptr && home == hub2->GetId())));
    }
}

TEST_CASE("insurance and home outlive the session; a policy pays no more than the ship was worth")
{
    // What a policy pays: its share of the cover, and never of more than the loss.
    CHECK(InsurancePayout(1000.0, 1000.0) == doctest::Approx(1000.0 * INSURANCE_PAYOUT));
    CHECK(InsurancePayout(1000.0, 400.0) == doctest::Approx(400.0 * INSURANCE_PAYOUT));
    CHECK(InsurancePayout(0.0, 400.0) == 0.0);
    // Insuring again charges for the difference only.
    CHECK(InsurancePremium(1500.0, 1000.0) == doctest::Approx(500.0 * INSURANCE_PREMIUM));
    CHECK(InsurancePremium(1000.0, 1000.0) == 0.0);

    World                         w;
    const Simulation::StartPoint& start = w.sim.Starts()[0];
    ClientSession&                s = w.Pilot(start);
    std::string                   yardSystem;
    const Entity*                 yard = w.Shipyard(yardSystem);
    REQUIRE(yard != nullptr);
    s.account.SetMoney(10000.0);
    s.dockedStationId = w.Hub(start)->GetId();
    REQUIRE(w.sim.BuyShip(s, HAULER));
    REQUIRE(w.sim.InsureShip(s, HAULER));
    s.homeStationId = yard->GetId();

    TempDir           dir;
    const std::string path = dir.Path("account_pilot.json");
    REQUIRE(w.sim.SaveAccount(s, path));
    ClientSession& back = w.Pilot(start);
    REQUIRE(w.sim.LoadAccount(back, path) == Save::Result::Ok);
    CHECK(back.CoverOf(HAULER) == doctest::Approx(s.CoverOf(HAULER)));
    CHECK(back.homeStationId == yard->GetId());
}

TEST_CASE("the wire carries the cover and the last loss, and a command insures (#344)")
{
    Proto::Command c;
    c.insureShip = 2;
    Proto::Command got;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), got));
    CHECK(got.insureShip == 2);
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(Proto::Command{}), got));
    CHECK(got.insureShip == -1);

    Proto::Snapshot s;
    s.systemId = "core";
    s.player.ownedShips = { 0, 2 };
    s.player.shipDesigns = { "scout", "hauler" };
    s.player.insured = { 0.0, 3000.0 };
    s.player.loss.seq = 3;
    s.player.loss.hull = "Miner";
    s.player.loss.system = "Vale";
    s.player.loss.value = 4200.0;
    s.player.loss.paid = 2940.0;
    s.player.loss.cargo = 12;
    s.player.loss.wokeAt = "Hub, Core";
    Proto::SnapshotSender   out;
    Proto::SnapshotReceiver in;
    Proto::Snapshot         back;
    REQUIRE(in.Decode(out.Encode(s, {}), back));
    CHECK(back.player.insured == s.player.insured);
    CHECK(back.player.loss.seq == 3);
    CHECK(back.player.loss.hull == "Miner");
    CHECK(back.player.loss.system == "Vale");
    CHECK(back.player.loss.value == doctest::Approx(4200.0));
    CHECK(back.player.loss.paid == doctest::Approx(2940.0));
    CHECK(back.player.loss.cargo == 12);
    CHECK(back.player.loss.wokeAt == "Hub, Core");
}
