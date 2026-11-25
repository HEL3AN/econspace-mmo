#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/Gunnery.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// Range and tracking (#619 slice 2, documents/combat_and_flight.md 3.2): one hit rule, in data
// per gun, read by a pilot's shot and an NPC's alike. Quality scales damage; there are no dice.
namespace
{
const std::string DATA = TEST_DATA_DIR;
constexpr float   DT = 1.0f / 60.0f;

Ships::GunSpec Spec(float optimal, float falloff, float tracking, float signatureRes)
{
    Ships::GunSpec g;
    g.optimal = optimal;
    g.falloff = falloff;
    g.tracking = tracking;
    g.signatureRes = signatureRes;
    return g;
}
}  // namespace

TEST_CASE("the hit rule: full at optimal, half a falloff on, tracking against how fast it crosses")
{
    const Ships::GunSpec g = Spec(400.0f, 300.0f, 0.5f, 10.0f);
    // At rest in the optimal, it hits whole; past it the effect halves with every falloff.
    CHECK(Ships::HitQuality(g, 100.0f, 0.0f, 10.0f) == doctest::Approx(1.0f));
    CHECK(Ships::HitQuality(g, 400.0f, 0.0f, 10.0f) == doctest::Approx(1.0f));
    CHECK(Ships::HitQuality(g, 700.0f, 0.0f, 10.0f) == doctest::Approx(0.5f));
    CHECK(Ships::HitQuality(g, 1000.0f, 0.0f, 10.0f) == doctest::Approx(0.25f));
    // ...and not at all past its reach.
    CHECK(Ships::HitQuality(g, Ships::ReachOf(g) + 1.0f, 0.0f, 10.0f) == 0.0f);
    // Crossing at exactly what it tracks, against a target as big as it resolves, halves it.
    CHECK(Ships::HitQuality(g, 400.0f, 0.5f, 10.0f) == doctest::Approx(0.5f));
    // A faster crossing is worse, a bigger target is better, a smaller one worse again.
    CHECK(Ships::HitQuality(g, 400.0f, 1.0f, 10.0f) < Ships::HitQuality(g, 400.0f, 0.5f, 10.0f));
    CHECK(Ships::HitQuality(g, 400.0f, 0.5f, 40.0f) > Ships::HitQuality(g, 400.0f, 0.5f, 10.0f));
    CHECK(Ships::HitQuality(g, 400.0f, 0.5f, 5.0f) < Ships::HitQuality(g, 400.0f, 0.5f, 10.0f));
    // The two terms are independent: their product.
    CHECK(Ships::HitQuality(g, 700.0f, 0.5f, 10.0f) == doctest::Approx(0.25f));
}

TEST_CASE("angular velocity is the transversal part of the relative velocity over the distance")
{
    // 100 units out, crossing at 100 u/s: one radian a second, however it is signed.
    CHECK(Ships::AngularVelocity(100.0f, 0.0f, 0.0f, 100.0f) == doctest::Approx(1.0f));
    CHECK(Ships::AngularVelocity(100.0f, 0.0f, 0.0f, -100.0f) == doctest::Approx(1.0f));
    // Straight at it or away from it is not crossing at all.
    CHECK(Ships::AngularVelocity(100.0f, 0.0f, 50.0f, 0.0f) == doctest::Approx(0.0f));
    // It is scale-free: ten times the distance at ten times the speed is the same rate; at the
    // same speed, a tenth of it.
    CHECK(Ships::AngularVelocity(1000.0f, 0.0f, 0.0f, 1000.0f) == doctest::Approx(1.0f));
    CHECK(Ships::AngularVelocity(1000.0f, 0.0f, 0.0f, 100.0f) == doctest::Approx(0.1f));
    // On top of the shooter: nothing to measure.
    CHECK(Ships::AngularVelocity(0.0f, 0.0f, 5.0f, 5.0f) == 0.0f);
}

TEST_CASE("a design's guns are its base gun and each fitted one, and they add up to its volley")
{
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    for (const char* id : { "scout", "cutter", "raider", "cruiser" })
    {
        INFO(id);
        const Ships::Design* d = cat.FindDesign(id);
        REQUIRE(d != nullptr);
        Ships::Stats s;
        std::string  why;
        REQUIRE(Ships::Derive(cat, *d, s, why));
        REQUIRE_FALSE(s.guns.empty());
        float sum = 0.0f;
        for (const Ships::Gun& g : s.guns)
            sum += g.damage;
        CHECK(sum == doctest::Approx(s.volley));
        // The base gun first, aimed as the rules say.
        CHECK(s.guns[0].damage == doctest::Approx(cat.rules.baseGun));
        CHECK(s.guns[0].spec.optimal == doctest::Approx(cat.rules.gun.optimal));
        // A shot at its reference -- at rest in the optimal of everything it fires -- is the
        // whole volley, with the mix the design has: what a siege and a cold fight reckon.
        const Ships::Profile pf = Ships::ProfileOf(s.guns);
        float                nearest = 1e9f;
        for (const Ships::Gun& g : s.guns)
            nearest = std::min(nearest, g.spec.optimal);
        const Ships::Shot ref = Ships::Gunnery(s.guns, nearest, 0.0f, 1000.0f);
        CHECK(ref.quality == doctest::Approx(1.0f));
        CHECK(ref.damage == doctest::Approx(s.volley));
        for (size_t t = 0; t < ref.mix.size(); t++)
            CHECK(ref.mix[t] == doctest::Approx(s.mix[t]));
        CHECK(Ships::Dps(s) == doctest::Approx(s.volley / Ships::PLAYER_FIRE_INTERVAL));
        CHECK(pf.reach > pf.optimal);
    }
    // A railgun is a long, slow gun and a laser a short, quick one; both are in data.
    Ships::Stats raider, cutter;
    std::string  why;
    REQUIRE(Ships::Derive(cat, *cat.FindDesign("raider"), raider, why));
    REQUIRE(Ships::Derive(cat, *cat.FindDesign("cutter"), cutter, why));
    REQUIRE(raider.guns.size() == 2);
    REQUIRE(cutter.guns.size() == 2);
    CHECK(raider.guns[1].spec.optimal > cutter.guns[1].spec.optimal);
    CHECK(raider.guns[1].spec.tracking < cutter.guns[1].spec.tracking);
}

TEST_CASE("a gun's aim is read in full, and an unknown or missing number is a load error")
{
    nlohmann::json base;
    {
        std::ifstream in(DATA + "ships.json");
        REQUIRE(in.good());
        in >> base;
    }
    auto loads = [](const nlohmann::json& j)
    {
        Ships::Catalogue c;
        std::string      why;
        return Ships::Parse(j, c, why);
    };
    REQUIRE(loads(base));
    for (auto& m : base["modules"])
        if (m["id"] == "weapons.laser")
        {
            nlohmann::json missing = base;
            for (auto& mm : missing["modules"])
                if (mm["id"] == "weapons.laser")
                    mm["gun"].erase("tracking");
            CHECK_FALSE(loads(missing));
            nlohmann::json misspelt = base;
            for (auto& mm : misspelt["modules"])
                if (mm["id"] == "weapons.laser")
                    mm["gun"]["trackin"] = 1;
            CHECK_FALSE(loads(misspelt));
            nlohmann::json zero = base;
            for (auto& mm : zero["modules"])
                if (mm["id"] == "weapons.laser")
                    mm["gun"]["falloff"] = 0;
            CHECK_FALSE(loads(zero));
        }
    // A gun on a module that fires nothing is a mistake, not a decoration.
    nlohmann::json idle = base;
    for (auto& m : idle["modules"])
        if (m["id"] == "hull.cargo")
            m["gun"] = {
                { "optimal", 1 }, { "falloff", 1 }, { "tracking", 1 }, { "signatureRes", 1 }
            };
    CHECK_FALSE(loads(idle));
}

namespace
{
struct Duelists
{
    Simulation                   sim;
    ClientSession*               a = nullptr;
    ClientSession*               b = nullptr;
    std::vector<Proto::TradeAck> acks;
    Simulation::FireMap          fires;

    Duelists()
    {
        REQUIRE(Factions::Load(DATA + "factions.json"));
        Archetypes::Load(DATA + "archetypes.json");
        Blueprints::Load(DATA + "blueprints.json");
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        a = &sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                               GetShipCatalog()[0].stats);
        b = &sim.CreateSession(a->systemId, Vector2{ 200.0f, 0.0f }, GetShipCatalog()[0].stats);
        a->accountName = "alpha";
        b->accountName = "bravo";
        sim.SetAllHot(true);
    }
    SystemState& World() { return *sim.SystemOf(*a); }
};
}  // namespace

TEST_CASE("a shot's damage is the guns' damage times the quality, and the beam carries it")
{
    Duelists f;
    REQUIRE_FALSE(f.a->ship->GetGuns().empty());
    const Ships::Profile pf = Ships::ProfileOf(f.a->ship->GetGuns());
    f.sim.LockTarget(*f.a, f.b->ship->GetId());
    for (int i = 0; i < 600; i++)
        f.sim.StepPlayerLocks(*f.a, f.World(), DT);
    REQUIRE(f.a->HeldTarget() == f.b->ship->GetId());
    f.a->weaponOn = true;

    auto lost = [&](ClientSession& s)
    {
        return s.ship->GetMaxHull() + s.ship->GetMaxShields() + s.ship->GetMaxArmour() -
               s.ship->GetHull() - s.ship->GetShields() - s.ship->GetArmour();
    };
    Simulation::PlayerCombatEvents ev;

    SUBCASE("at rest in the optimal it takes the whole volley")
    {
        f.b->ship->Teleport({ pf.optimal * 0.5f, 0.0f });
        f.a->ship->Teleport({ 0.0f, 0.0f });
        f.a->fireTimer = 0.0f;
        REQUIRE(f.sim.StepPlayerFire(*f.a, f.World(), f.b->ship->GetId(), DT, &ev));
        CHECK(ev.quality == doctest::Approx(1.0f));
        CHECK(lost(*f.b) == doctest::Approx(f.a->ship->GetDamage()));
    }
    SUBCASE("past the optimal it takes less, and says so")
    {
        f.b->ship->Teleport({ pf.optimal + pf.falloff, 0.0f });
        f.a->ship->Teleport({ 0.0f, 0.0f });
        f.a->fireTimer = 0.0f;
        REQUIRE(f.sim.StepPlayerFire(*f.a, f.World(), f.b->ship->GetId(), DT, &ev));
        CHECK(ev.quality == doctest::Approx(0.5f).epsilon(0.05));
        CHECK(lost(*f.b) < f.a->ship->GetDamage() * 0.6f);
        CHECK(lost(*f.b) > f.a->ship->GetDamage() * 0.4f);
    }
    SUBCASE("beyond every gun's reach there is no shot")
    {
        f.b->ship->Teleport({ pf.reach + 50.0f, 0.0f });
        f.a->fireTimer = 0.0f;
        CHECK_FALSE(f.sim.StepPlayerFire(*f.a, f.World(), f.b->ship->GetId(), DT, &ev));
        CHECK(lost(*f.b) == doctest::Approx(0.0f));
    }
    SUBCASE("a target crossing fast is hit worse than one at rest, at the same range")
    {
        f.b->ship->Teleport({ pf.optimal * 0.5f, 0.0f });
        f.a->ship->Teleport({ 0.0f, 0.0f });
        f.b->ship->ApplyView(f.b->ship->GetPosition(), 0.0f, { 0.0f, 600.0f }, 1000.0f, 0.0f);
        f.a->fireTimer = 0.0f;
        REQUIRE(f.sim.StepPlayerFire(*f.a, f.World(), f.b->ship->GetId(), DT, &ev));
        CHECK(ev.quality < 0.5f);
    }
}

TEST_CASE("an NPC's shots meet the same rule: a design's guns, from where both are")
{
    Duelists f;
    auto     raider = std::make_unique<NpcShip>(Vector2{ 100.0f, 0.0f }, Fac::Pirates(),
                                                NpcRole::Pirate, std::vector<Vector2>{});
    raider->SetId(5000);
    NpcShip* pirate = raider.get();
    f.World().entities.push_back(std::move(raider));
    REQUIRE_FALSE(pirate->GetGuns().empty());
    f.b->ship->Teleport({ 4000.0f, 4000.0f });  // out of the way

    Simulation::PlayerPresence pr;
    pr.session = f.a;
    pr.ship = f.a->ship.get();
    pirate->TargetLock().Begin(f.a->ship->GetId(), 0.0f);

    // Beyond what its guns reach, nothing is fired; inside it, a shot of less than a whole volley
    // lands when the pilot is past the optimal.
    const Ships::Profile   pf = Ships::ProfileOf(pirate->GetGuns());
    std::vector<FireEvent> shots;
    pirate->SetPosition({ pf.reach + 100.0f, 0.0f });
    f.sim.StepNpcCombat(f.World(), { pr }, &shots, DT);
    CHECK(shots.empty());

    pirate->SetPosition({ pf.optimal + pf.falloff, 0.0f });
    pirate->TargetLock().Begin(f.a->ship->GetId(), 0.0f);  // let go of it out of reach, again
    f.sim.StepNpcCombat(f.World(), { pr }, &shots, DT);
    REQUIRE_FALSE(shots.empty());
    CHECK(shots.back().quality < 1.0f);
    CHECK(shots.back().quality > 0.0f);
}

TEST_CASE("quality and velocity cross the wire, and a whole shot costs no field")
{
    Proto::Snapshot snap;
    FireEvent       whole;
    whole.from = { 1.0f, 2.0f };
    whole.to = { 3.0f, 4.0f };
    FireEvent half = whole;
    half.quality = 0.5f;
    snap.fires = { whole, half };
    Proto::EntitySnapshot e;
    e.id = 7;
    e.kind = EntityKind::Npc;
    e.vel = { 12.5f, -40.0f };
    snap.entities.push_back(e);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    REQUIRE(back.fires.size() == 2);
    CHECK(back.fires[0].quality == doctest::Approx(1.0f));
    CHECK(back.fires[1].quality == doctest::Approx(0.5f));
    REQUIRE(back.entities.size() == 1);
    CHECK(back.entities[0].vel.x == doctest::Approx(12.5f));
    CHECK(back.entities[0].vel.y == doctest::Approx(-40.0f));
}

namespace
{
// Two equal pilots on orbit holds round each other at `range`, each shooting the other: how long
// until one is lost, and how well the shots hit on average. `still`: both hold still instead.
struct DuelResult
{
    float seconds = 0.0f, firstBlood = -1.0f, meanQuality = 0.0f;
    int   shots = 0;
};
DuelResult Duel(float range, bool still)
{
    Duelists       f;
    ClientSession& a = *f.a;
    ClientSession& b = *f.b;
    a.ship->Teleport({ 0.0f, 0.0f });
    b.ship->Teleport({ range, 0.0f });
    f.sim.LockTarget(a, b.ship->GetId());
    f.sim.LockTarget(b, a.ship->GetId());
    a.weaponOn = b.weaponOn = true;
    DuelResult r;
    float      qsum = 0.0f;
    auto       whole = [](const ClientSession& s)
    { return s.ship->GetHull() + s.ship->GetShields() + s.ship->GetArmour(); };
    const float start = whole(a);
    for (; r.seconds < 300.0f && a.ship->IsAlive() && b.ship->IsAlive(); r.seconds += DT)
    {
        for (ClientSession* s : { &a, &b })
        {
            ClientSession& other = s == &a ? b : a;
            Proto::Command c;
            if (!still)
            {
                c.navMode = 3;
                c.navHoldId = other.ship->GetId();
                c.navRange = range;
            }
            f.sim.StepPlayerShip(*s, c, 1.0f, DT);
            f.sim.StepPlayerAttachment(*s, DT);
            f.sim.StepPlayerLocks(*s, f.World(), DT);
            Simulation::PlayerCombatEvents ev;
            if (f.sim.StepPlayerFire(*s, f.World(), s->HeldTarget(), DT, &ev))
            {
                qsum += ev.quality;
                r.shots++;
            }
        }
        if (r.firstBlood < 0.0f && (whole(a) < start || whole(b) < start))
            r.firstBlood = r.seconds;
    }
    r.meanQuality = r.shots > 0 ? qsum / (float)r.shots : 0.0f;
    return r;
}
}  // namespace

TEST_CASE("a duel of two equal starter frigates lasts 20 to 60 seconds when they fly it")
{
    // The target tempo (owner, #619 D6): long enough to lock, position and decide to leave. Two
    // pilots on orbit holds round each other at the guns' optimal, each shooting the other.
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    const Ships::Profile pf =
        Ships::ProfileOf(GetShipCatalog()[0].stats.guns);  // the starter's: its base gun
    const DuelResult flown = Duel(pf.optimal, false);
    const DuelResult brawl = Duel(pf.optimal, true);
    const DuelResult wide = Duel(pf.optimal + pf.falloff, false);
    MESSAGE("starter duel: flown at the optimal "
            << flown.seconds << " s (mean quality " << flown.meanQuality << "), standing still "
            << brawl.seconds << " s, flown one falloff out " << wide.seconds << " s");
    CHECK(flown.seconds >= 20.0f);
    CHECK(flown.seconds <= 60.0f);
    // Standing still at the optimal is the reference engagement: the quickest a duel goes. A
    // live fight is worse than the reference, never better.
    CHECK(brawl.seconds < flown.seconds);
    CHECK(brawl.meanQuality == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(flown.meanQuality < 0.7f);
    // Nobody shoots before the lock has formed.
    CHECK(flown.firstBlood >= 1.5f);
    CHECK(wide.seconds > flown.seconds);  // staying out costs the other side its damage too
}
