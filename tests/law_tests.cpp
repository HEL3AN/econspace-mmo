#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"
#include "factions_named.h"

// The police answer a crime from real forces (#431 slice 9): every free police ship in the
// system, patrols a station launches out of what the faction can spare, and police lent by
// systems nearby -- more and sooner the higher the security, nobody below the lowest band --
// and station guns join in only from security 0.5.

namespace
{
const std::string DATA = TEST_DATA_DIR;
constexpr float   IN_SIGHT = 3500.0f;  // a patrol's NPC_AGGRO_RANGE

// The rules as the data file has them, put back after a test changed them.
struct RulesGuard
{
    WorldRules::Law saved = WorldRules::LawRules();
    ~RulesGuard() { WorldRules::SetLawRules(saved); }
};

// The fixture world -- Helios Core (the Traders Guild's) between Tau Verge and Sigma Reach --
// with nothing in it but the police a test puts there: `here` in the core, `nextDoor` in each
// neighbour, all the Guild's, each backed by as much presence.
struct Scene
{
    std::unique_ptr<Simulation> sim;
    std::string                 sys;
    FactionId                   law = Fac::TradersGuild();
    ClientSession*              pilot = nullptr;
    SystemState&                St() { return *sim->SystemById(sys); }
};

Scene CrimeScene(float security, int here, float nextDoor, bool warm = true)
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    Scene s;
    s.sim = std::make_unique<Simulation>();
    Simulation& sim = *s.sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    s.sys = sim.Universe().startId;
    for (auto& kv : sim.Systems())
    {
        SystemAggregate& a = kv.second.agg;
        for (FactionId f : Factions::All())
            a.presence[f] = 0.0f;
        a.police = a.pirates = 0.0f;
        a.policeFaction = s.law;
    }
    auto hold = [&](const std::string& id, float police)
    {
        SystemAggregate& a = sim.SystemById(id)->agg;
        a.claimed = true;
        a.controller = s.law;
        a.presence[s.law] = police;
        a.police = police;
    };
    hold(s.sys, (float)here);
    for (const std::string& n : sim.Neighbors(s.sys))
        hold(n, nextDoor);
    s.St().agg.security = s.St().agg.baseSecurity = security;
    sim.MindOf(s.law).stock = 0.0f;  // nothing to buy patrols with, unless a test says so

    s.pilot =
        &sim.CreateSession(s.sys, Vector2{ 300000.0f, -300000.0f }, GetShipCatalog()[0].stats);
    s.pilot->accountName = "outlaw";
    s.pilot->account.AddBounty(s.law, 5.0);  // wanted: the Guild's police act on them
    if (warm)
        sim.Warm(s.St());
    return s;
}

int PoliceIn(const SystemState& st)
{
    int n = 0;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police)
            n++;
    return n;
}

// Police in sight of the pilot: those answering, whichever way they came.
int Beside(Scene& s)
{
    int n = 0;
    for (const auto& e : s.St().entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police &&
            static_cast<const NpcShip&>(*e).IsAlive())
        {
            const Vector2 a = e->GetPosition(), b = s.pilot->ship->GetPosition();
            if (std::hypot(a.x - b.x, a.y - b.y) <= IN_SIGHT)
                n++;
        }
    return n;
}

const Plan* ResponseTo(const Simulation& sim, const std::string& sys)
{
    for (const auto& kv : sim.Plans())
        if (kv.second.kind == Plan::Kind::Response && kv.second.target == sys)
            return &kv.second;
    return nullptr;
}

float Called(const Simulation& sim, const std::string& sys)
{
    const Plan* p = ResponseTo(sim, sys);
    return p != nullptr ? p->force : 0.0f;
}

// What answers a crime at `security`: the police here once their time has come, and those
// coming from next door.
struct Answer
{
    bool  called = false;
    int   here = 0;
    float nextDoor = 0.0f;
    int   hops = 0;
};

Answer AnswerAt(float security)
{
    Scene s = CrimeScene(security, 10, 5.0f);
    REQUIRE(PoliceIn(s.St()) == 10);
    REQUIRE(Beside(s) == 0);
    Answer a;
    a.called = s.sim->CallPolice(s.St(), *s.pilot, s.law);
    WorldRules::Law::Band band;
    if (WorldRules::LawRules().BandFor(security, band) && band.delay > 0.0)
    {
        CHECK(Beside(s) == 0);  // not yet: the lower the security, the longer it takes
        s.sim->SetTime(s.sim->Time() + band.delay + 0.1);
        s.sim->StepPoliceCall(s.St());
    }
    a.here = Beside(s);
    a.nextDoor = Called(*s.sim, s.sys);
    if (const Plan* p = ResponseTo(*s.sim, s.sys))
        for (const Plan::Lent& l : p->lent)
            a.hops = std::max(a.hops, l.hops);
    return a;
}
}  // namespace

TEST_CASE("the police answer grows with security, and below the lowest band nobody comes")
{
    const WorldRules::Law& r = WorldRules::LawRules();
    const Answer           high = AnswerAt(0.9f), mid = AnswerAt(0.6f), low = AnswerAt(0.3f),
                           none = AnswerAt(0.05f);
    CHECK(high.called);
    CHECK(high.here == 10);  // every free police ship in the system, at once
    CHECK(high.nextDoor >= 1.0f);
    CHECK(high.nextDoor <= (float)r.highCalled);
    CHECK(mid.called);
    CHECK(mid.here == r.midLocal);
    CHECK(mid.nextDoor <= (float)r.midCalled);
    CHECK(mid.hops <= r.midReach);
    CHECK(low.called);
    CHECK(low.here == r.lowLocal);  // the local patrols only
    CHECK(low.nextDoor == 0.0f);
    CHECK_FALSE(none.called);
    CHECK(none.here == 0);
    CHECK(none.nextDoor == 0.0f);
    CHECK(high.here + high.nextDoor > mid.here + mid.nextDoor);
    CHECK(mid.here + mid.nextDoor > low.here + low.nextDoor);
}

TEST_CASE("a system with no police sends nobody, and one call holds")
{
    Scene s = CrimeScene(0.9f, 0, 0.0f);
    CHECK_FALSE(s.sim->CallPolice(s.St(), *s.pilot, s.law));
    CHECK(ResponseTo(*s.sim, s.sys) == nullptr);

    Scene t = CrimeScene(0.9f, 3, 0.0f);
    CHECK(t.sim->CallPolice(t.St(), *t.pilot, t.law));
    CHECK(Beside(t) == 3);
    t.sim->SetTime(t.sim->Time() + 1.0);
    CHECK_FALSE(t.sim->CallPolice(t.St(), *t.pilot, t.law));  // the next shot brings no more
}

TEST_CASE("a station launches patrols out of the presence it has spare, then out of stock")
{
    SUBCASE("presence not yet flying")
    {
        Scene s = CrimeScene(0.9f, 2, 0.0f);
        s.St().agg.presence[s.law] = 5.0f;  // three more than fly
        REQUIRE(s.sim->CallPolice(s.St(), *s.pilot, s.law));
        CHECK(Beside(s) == 5);
        CHECK(s.St().agg.presence[s.law] == doctest::Approx(5.0f));  // nothing made from nothing
    }
    SUBCASE("bought, while the stock lasts")
    {
        Scene       s = CrimeScene(0.9f, 2, 0.0f);
        const float price = (float)WorldRules::LawRules().launchCost * Simulation::OutpostCost();
        REQUIRE(price > 0.0f);
        s.sim->MindOf(s.law).stock = 2.5f * price;  // two patrols' worth
        REQUIRE(s.sim->CallPolice(s.St(), *s.pilot, s.law));
        CHECK(Beside(s) == 4);
        CHECK(s.sim->MindOf(s.law).stock == doctest::Approx(0.5f * price));
        CHECK(s.St().agg.presence[s.law] == doctest::Approx(4.0f));
    }
}

TEST_CASE("police from next door are lent: missing from home until they are back")
{
    RulesGuard      guard;
    WorldRules::Law quick = WorldRules::LawRules();
    quick.hop = 1.0;
    quick.stay = 6.0;
    WorldRules::SetLawRules(quick);

    // At the Guild's capacity next door, so a holding neither grows nor fades on its own.
    const float                  cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    Scene                        s = CrimeScene(0.9f, 0, cap);
    Simulation&                  sim = *s.sim;
    std::map<std::string, float> before;
    for (const std::string& n : sim.Neighbors(s.sys))
        before[n] = sim.SystemById(n)->agg.presence[s.law];

    REQUIRE(sim.CallPolice(s.St(), *s.pilot, s.law));
    const Plan* p = ResponseTo(sim, s.sys);
    REQUIRE(p != nullptr);
    const int   id = p->id;
    const float force = p->force;
    float       lent = 0.0f;
    for (const Plan::Lent& l : p->lent)
    {
        const SystemState& home = *sim.SystemById(l.from);
        CHECK(home.agg.presence[s.law] == doctest::Approx(before[l.from] - l.force));
        CHECK(home.agg.police == doctest::Approx(before[l.from] - l.force));
        CHECK(sim.LentFrom(l.from, s.law) == doctest::Approx(l.force));
        lent += l.force;
    }
    CHECK(lent == doctest::Approx(force));

    // They come, as ships beside the pilot, counted by the Response and not by the system.
    auto pass = [&]()
    {
        for (int i = 0; i < 120; i++)
        {
            sim.StepSystemAgents(s.St(), {}, nullptr, 1.0f / 60.0f);
            sim.MaintainWorld(1.0f / 60.0f);
        }
    };
    for (int i = 0; i < 3 && !sim.Plans().at(id).arrived; i++)
        pass();
    REQUIRE(sim.Plans().at(id).arrived);
    CHECK(sim.Plans().at(id).fielded == (int)std::lround(force));
    CHECK(Beside(s) == (int)std::lround(force));

    // One shot down is one fewer to go home.
    for (const auto& e : s.St().entities)
        if (e->GetKind() == EntityKind::Npc && static_cast<const NpcShip&>(*e).GetTask() == id)
        {
            static_cast<NpcShip&>(*e).SetHull(0.0f);
            break;
        }
    pass();
    REQUIRE(sim.Plans().count(id) == 1);
    CHECK(sim.Plans().at(id).force == doctest::Approx(force - 1.0f));

    // Their time up, the rest go home and are that holding's again.
    for (int i = 0; i < 20 && sim.Plans().count(id) != 0; i++)
        pass();
    CHECK(sim.Plans().count(id) == 0);
    CHECK(Beside(s) == 0);
    float coming = 0.0f;
    for (const auto& kv : sim.Plans())
        if (kv.second.kind == Plan::Kind::Return && kv.second.faction == s.law)
            coming += kv.second.force;
    CHECK(coming == doctest::Approx(force - 1.0f));
    for (int i = 0; i < 20 && coming > 0.0f; i++)
    {
        pass();
        coming = 0.0f;
        for (const auto& kv : sim.Plans())
            if (kv.second.kind == Plan::Kind::Return && kv.second.faction == s.law)
                coming += kv.second.force;
    }
    CHECK(coming == 0.0f);
    float home = 0.0f, was = 0.0f;
    for (const auto& kv : before)
    {
        home += sim.SystemById(kv.first)->agg.presence[s.law];
        was += kv.second;
    }
    CHECK(home == doctest::Approx(was - 1.0f).epsilon(0.02));
}

TEST_CASE("a faction whose police are all out answers the next crime with fewer")
{
    Scene       s = CrimeScene(0.9f, 0, 3.0f);
    Simulation& sim = *s.sim;
    REQUIRE(sim.CallPolice(s.St(), *s.pilot, s.law));
    const float first = Called(sim, s.sys);
    REQUIRE(first >= 1.0f);

    sim.SetTime(sim.Time() + WorldRules::LawRules().callSeconds + 1.0);
    sim.CallPolice(s.St(), *s.pilot, s.law);
    float second = 0.0f;
    for (const auto& kv : sim.Plans())
        if (kv.second.kind == Plan::Kind::Response && kv.second.force != first)
            second += kv.second.force;
    CHECK(second < first);
}

TEST_CASE("station guns join in against a criminal only from security 0.5")
{
    auto fired = [](float security)
    {
        // Any armed station in the world, its faction the law the pilot is wanted by.
        Scene          s = CrimeScene(security, 0, 0.0f);
        SystemState*   where = nullptr;
        const Station* station = nullptr;
        for (auto& kv : s.sim->Systems())
            for (const auto& e : kv.second.entities)
                if (station == nullptr && e->GetKind() == EntityKind::Station &&
                    e->Has(Component::Defensive) && e->GetArchetype()->weaponDamage > 0.0f &&
                    Factions::IsLawful(static_cast<const Station&>(*e).GetFaction()))
                {
                    station = static_cast<const Station*>(e.get());
                    where = &kv.second;
                }
        REQUIRE(station != nullptr);
        const FactionId owner = station->GetFaction();
        s.pilot->account.AddBounty(owner, 5.0);
        where->agg.security = security;
        s.pilot->ship->Teleport(
            { station->GetPosition().x + station->GetSize() + 50.0f, station->GetPosition().y });
        Simulation::PlayerPresence p;
        p.session = s.pilot;
        p.ship = s.pilot->ship.get();
        std::vector<FireEvent> fires;
        for (int i = 0; i < 120; i++)
            s.sim->StepStationDefence(*where, { p }, &fires, 1.0f / 60.0f);
        for (const FireEvent& f : fires)
            if (f.shooterEntityId == station->GetId())
                return true;
        return false;
    };
    CHECK(fired(0.6f));
    CHECK_FALSE(fired(0.4f));
}

TEST_CASE("police are taken from a hot neighbour as from a cold one")
{
    struct Took
    {
        std::vector<std::pair<std::string, float>> lent;
        std::map<std::string, float>               police, presence;
    };
    auto run = [](bool hotNextDoor)
    {
        Scene s = CrimeScene(0.9f, 0, 3.0f);
        if (hotNextDoor)
            for (const std::string& n : s.sim->Neighbors(s.sys))
            {
                s.sim->Warm(*s.sim->SystemById(n));
                REQUIRE(PoliceIn(*s.sim->SystemById(n)) == 3);
            }
        REQUIRE(s.sim->CallPolice(s.St(), *s.pilot, s.law));
        Took        t;
        const Plan* p = ResponseTo(*s.sim, s.sys);
        REQUIRE(p != nullptr);
        for (const Plan::Lent& l : p->lent)
            t.lent.push_back({ l.from, l.force });
        for (const std::string& n : s.sim->Neighbors(s.sys))
        {
            const SystemState& st = *s.sim->SystemById(n);
            t.police[n] = st.agg.police;
            t.presence[n] = st.agg.presence[s.law];
            if (hotNextDoor)
                CHECK(PoliceIn(st) == (int)std::lround(st.agg.police));  // the ships went
        }
        return t;
    };
    const Took cold = run(false), hot = run(true);
    CHECK(cold.lent == hot.lent);
    CHECK(cold.police == hot.police);
    CHECK(cold.presence == hot.presence);
}

TEST_CASE("a save sends the police from next door home")
{
    Scene s = CrimeScene(0.9f, 0, 3.0f);
    REQUIRE(s.sim->CallPolice(s.St(), *s.pilot, s.law));
    const float force = Called(*s.sim, s.sys);
    REQUIRE(force > 0.0f);
    const TempDir     tmp;
    const std::string path = tmp.Path("world_law.json");
    REQUIRE(s.sim->SaveWorld(path));
    Simulation back;
    back.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    back.Seed(1234u);
    REQUIRE(back.LoadWorld(path) == Save::Result::Ok);
    back.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    float coming = 0.0f;
    for (const auto& kv : back.Plans())
    {
        CHECK(kv.second.kind != Plan::Kind::Response);
        if (kv.second.kind == Plan::Kind::Return && kv.second.faction == s.law)
            coming += kv.second.force;
    }
    CHECK(coming == doctest::Approx(force));
}
