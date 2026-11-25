#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Lock.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// Lock before fire (#619, documents/combat_and_flight.md 3.1): a gun fires at a lock, a lock
// takes time by the target's signature and holds a slot, and an NPC locks by the same timer.
namespace
{
const std::string DATA = TEST_DATA_DIR;
constexpr float   DT = 1.0f / 60.0f;

struct Scene
{
    Simulation                   sim;
    ClientSession*               s = nullptr;
    std::vector<Proto::TradeAck> acks;
    Simulation::FireMap          fires;
    int                          nextId = 4000;

    Scene()
    {
        REQUIRE(Factions::Load(DATA + "factions.json"));
        Archetypes::Load(DATA + "archetypes.json");
        Blueprints::Load(DATA + "blueprints.json");
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        s = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
    }
    SystemState& World() { return *sim.SystemOf(*s); }
    NpcShip*     Pirate(Vector2 at)
    {
        auto n =
            std::make_unique<NpcShip>(at, Fac::Pirates(), NpcRole::Pirate, std::vector<Vector2>{});
        n->SetId(++nextId);
        NpcShip* out = n.get();
        World().entities.push_back(std::move(n));
        return out;
    }
    void Act(const Proto::Action& a) { sim.ApplyAction(*s, a, acks); }
    // `seconds` of world ticks for the pilot's locks.
    void Tick(float seconds)
    {
        for (float t = 0.0f; t < seconds; t += DT)
            sim.StepPlayerLocks(*s, World(), DT);
    }
};

std::string LastNotice(const ClientSession& s)
{
    std::string said;
    for (const Ev::Event& e : s.EventsSince(0))
        if (e.kind == Ev::Kind::Notice)
            said = e.text;
    return said;
}
}  // namespace

TEST_CASE("a lock takes the time the target's signature says, and a big target is quick")
{
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    const Ships::Rules& r = Archetypes::ShipCatalogue().rules;
    // The reference signature is a frigate's: it takes lockBase seconds to lock another.
    CHECK(Ships::LockTime(r, r.lockRef, r.scanRes) == doctest::Approx(r.lockBase));
    // A smaller ship is slower to find, a larger one quicker, and the extremes are clamped.
    CHECK(Ships::LockTime(r, r.lockRef / 4.0f, r.scanRes) > r.lockBase);
    CHECK(Ships::LockTime(r, r.lockRef * 4.0f, r.scanRes) < r.lockBase);
    CHECK(Ships::LockTime(r, 5000.0f, r.scanRes) == doctest::Approx(r.lockMin));
    CHECK(Ships::LockTime(r, 0.5f, 0.01f) == doctest::Approx(r.lockMax));
    // Better sensors lock sooner.
    CHECK(Ships::LockTime(r, r.lockRef, 2.0f) < Ships::LockTime(r, r.lockRef, 1.0f));
}

TEST_CASE("a lock forms, is held, and its slot is a limit")
{
    Scene    f;
    NpcShip* a = f.Pirate({ 100.0f, 0.0f });
    f.Act(Proto::Act::Lock{ a->GetId() });
    REQUIRE(f.s->locks.size() == 1);
    CHECK(f.s->activeLock == a->GetId());  // the first lock is the active one
    CHECK_FALSE(f.s->locks[0].held);
    CHECK(f.s->HeldTarget() == 0);  // not yet: nothing may be fired at a lock still forming
    f.Tick(f.s->locks[0].need * 0.5f);
    CHECK_FALSE(f.s->locks[0].held);
    f.Tick(f.s->locks[0].need);
    REQUIRE(f.s->locks.size() == 1);
    CHECK(f.s->locks[0].held);
    CHECK(f.s->HeldTarget() == a->GetId());

    // The starter frigate holds three; the fourth is refused, and says why.
    const int max = f.s->ship->GetStats().locks;
    REQUIRE(max == 3);
    for (int i = 1; i < max; i++)
        f.Act(Proto::Act::Lock{ f.Pirate({ 100.0f, 50.0f * (float)i })->GetId() });
    CHECK((int)f.s->locks.size() == max);
    f.Act(Proto::Act::Lock{ f.Pirate({ 100.0f, -80.0f })->GetId() });
    CHECK((int)f.s->locks.size() == max);
    CHECK(LastNotice(*f.s).find("every lock is in use") != std::string::npos);

    // Letting go frees the slot, and the guns follow another held lock.
    f.Tick(30.0f);
    f.Act(Proto::Act::Unlock{ a->GetId() });
    CHECK((int)f.s->locks.size() == max - 1);
    CHECK(f.s->activeLock != a->GetId());
    CHECK(f.s->activeLock != 0);
    f.Act(Proto::Act::Unlock{ 0 });
    CHECK(f.s->locks.empty());
    CHECK(f.s->activeLock == 0);
}

TEST_CASE("only ships and others' structures can be locked, and only within the sensors' reach")
{
    Scene f;
    // A station is looked at, never locked.
    auto st = std::make_unique<Station>(Vector2{ 300.0f, 0.0f }, 60.0f, "Depot",
                                        Fac::TradersGuild(), StationRole::TradeHub);
    st->SetId(4999);
    f.World().entities.push_back(std::move(st));
    f.Act(Proto::Act::Lock{ 4999 });
    CHECK(f.s->locks.empty());
    // Nothing there, and oneself.
    f.Act(Proto::Act::Lock{ 123456 });
    f.Act(Proto::Act::Lock{ f.s->ship->GetId() });
    CHECK(f.s->locks.empty());

    // Beyond the lock range: refused; inside it, taken.
    const float reach = f.s->ship->GetStats().lockRange;
    NpcShip*    far = f.Pirate({ reach + 500.0f, 0.0f });
    f.Act(Proto::Act::Lock{ far->GetId() });
    CHECK(f.s->locks.empty());
    CHECK(LastNotice(*f.s).find("out of lock range") != std::string::npos);
    far->SetPosition({ reach - 500.0f, 0.0f });
    f.Act(Proto::Act::Lock{ far->GetId() });
    CHECK(f.s->locks.size() == 1);
}

TEST_CASE(
    "a held lock survives a short absence, a forming one does not, and a gone target frees it")
{
    Scene    f;
    NpcShip* a = f.Pirate({ 100.0f, 0.0f });
    f.Act(Proto::Act::Lock{ a->GetId() });
    f.Tick(f.s->locks[0].need * 0.5f);
    a->SetPosition({ f.s->ship->GetStats().lockRange * 2.0f, 0.0f });
    f.Tick(0.1f);
    CHECK(f.s->locks.empty());  // forming, and out of reach: lost at once

    a->SetPosition({ 100.0f, 0.0f });
    f.Act(Proto::Act::Lock{ a->GetId() });
    f.Tick(30.0f);
    REQUIRE(f.s->locks.size() == 1);
    REQUIRE(f.s->locks[0].held);
    const float grace = Archetypes::ShipCatalogue().rules.lockGrace;
    a->SetPosition({ f.s->ship->GetStats().lockRange * 2.0f, 0.0f });
    f.Tick(grace * 0.5f);
    CHECK(f.s->locks.size() == 1);  // held through a short absence
    a->SetPosition({ 100.0f, 0.0f });
    f.Tick(1.0f);
    CHECK(f.s->locks[0].held);  // and back
    a->SetPosition({ f.s->ship->GetStats().lockRange * 2.0f, 0.0f });
    f.Tick(grace + 0.5f);
    CHECK(f.s->locks.empty());  // out for longer than the grace

    a->SetPosition({ 100.0f, 0.0f });
    f.Act(Proto::Act::Lock{ a->GetId() });
    REQUIRE(f.s->locks.size() == 1);
    f.World().entities.pop_back();  // the target is gone from the system
    f.Tick(DT * 2.0f);
    CHECK(f.s->locks.empty());
}

TEST_CASE("a gun fires at a held active lock and at nothing selected")
{
    Scene    f;
    NpcShip* a = f.Pirate({ 100.0f, 0.0f });
    f.Act(Proto::Act::Fire{ true });
    Proto::Command c;
    c.targetId = a->GetId();  // selected, and nothing more
    const float hull = a->GetHull() + a->GetArmour();

    for (int i = 0; i < 120; i++)
        f.sim.ApplyCommand(*f.s, c, DT, f.fires);
    CHECK(a->GetHull() + a->GetArmour() == doctest::Approx(hull));  // armed, selected, not locked

    f.Act(Proto::Act::Lock{ a->GetId() });
    for (int i = 0; i < 10; i++)  // forming: still no shot
    {
        f.sim.StepPlayerLocks(*f.s, f.World(), DT);
        f.sim.ApplyCommand(*f.s, c, DT, f.fires);
    }
    CHECK(a->GetHull() + a->GetArmour() == doctest::Approx(hull));

    f.Tick(f.s->locks[0].need);
    f.sim.ApplyCommand(*f.s, c, DT, f.fires);
    CHECK(a->GetHull() + a->GetArmour() < hull);  // held: it fires
}

TEST_CASE("an NPC takes the same lock time before it shoots, and the pilot is told")
{
    Scene                      f;
    NpcShip*                   pirate = f.Pirate({ 120.0f, 0.0f });
    Simulation::PlayerPresence pr;
    pr.session = f.s;
    pr.ship = f.s->ship.get();
    f.sim.Warm(f.World());
    std::vector<FireEvent> fires;

    const float need = Ships::LockTime(Archetypes::ShipCatalogue().rules, f.s->ship->GetSize(),
                                       Archetypes::ShipCatalogue().rules.scanRes);
    const float hull = f.s->ship->GetHull() + f.s->ship->GetShields() + f.s->ship->GetArmour();
    float       t = 0.0f;
    bool        told = false;
    for (; t < need * 0.8f; t += DT)
    {
        f.sim.StepNpcCombat(f.World(), { pr }, &fires, DT);
        for (const Proto::LockedByView& by : f.sim.LockedByOf(*f.s, f.World()))
            told = told || (by.id == pirate->GetId() && !by.held);
    }
    CHECK(fires.empty());  // locking, not shooting
    CHECK(told);
    for (; t < need + 0.5f; t += DT)
        f.sim.StepNpcCombat(f.World(), { pr }, &fires, DT);
    CHECK_FALSE(fires.empty());
    CHECK(f.s->ship->GetHull() + f.s->ship->GetShields() + f.s->ship->GetArmour() < hull);
    bool held = false;
    for (const Proto::LockedByView& by : f.sim.LockedByOf(*f.s, f.World()))
        held = held || (by.id == pirate->GetId() && by.held);
    CHECK(held);
}

TEST_CASE("two equal starter frigates fight: both lock, both fire, one is lost")
{
    Scene          f;
    ClientSession& a = *f.s;
    ClientSession& b =
        f.sim.CreateSession(a.systemId, Vector2{ 150.0f, 0.0f }, GetShipCatalog()[0].stats);
    a.accountName = "alpha";
    b.accountName = "bravo";
    f.sim.SetAllHot(true);
    Orders::Order oa, ob;
    oa.kind = ob.kind = Orders::Kind::Attack;
    oa.targetId = b.ship->GetId();
    ob.targetId = a.ship->GetId();
    f.sim.GiveOrder(a, oa);
    f.sim.GiveOrder(b, ob);

    float seconds = 0.0f, firstShot = -1.0f;
    auto  total = [](const ClientSession& s)
    { return s.ship->GetHull() + s.ship->GetShields() + s.ship->GetArmour(); };
    const float whole = total(a);
    for (; seconds < 600.0f && a.ship->IsAlive() && b.ship->IsAlive(); seconds += DT)
    {
        f.sim.StepPlayerOrder(a, f.World(), DT);
        f.sim.StepPlayerOrder(b, f.World(), DT);
        f.sim.StepPlayerLocks(a, f.World(), DT);
        f.sim.StepPlayerLocks(b, f.World(), DT);
        if (firstShot < 0.0f && (total(a) < whole || total(b) < whole))
            firstShot = seconds;
    }
    MESSAGE("a duel of two starter frigates: first blood at " << firstShot << " s, over at "
                                                              << seconds << " s");
    CHECK_FALSE((a.ship->IsAlive() && b.ship->IsAlive()));
    // Nobody shoots before the lock has formed.
    CHECK(firstShot >= Ships::LockTime(Archetypes::ShipCatalogue().rules, a.ship->GetSize(),
                                       Archetypes::ShipCatalogue().rules.scanRes) -
                           0.1f);
}
