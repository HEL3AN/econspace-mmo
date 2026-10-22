#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/AsteroidField.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "factions_named.h"
#include "raylib.h"  // SetRandomSeed: a station's own offers are drawn

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// What security does and who raises it (#431 L5, slice 11): a corporation pays the world's
// police out of its treasury in a system it holds, which raises the security there, makes its
// law the system's and puts police in the air; and the lower a system's security, the more
// pirates fly there, the likelier a convoy is set upon, and the better the work pays.

namespace
{
constexpr double NOW = 1.8e9;  // seconds, as Unix time
constexpr double PASS = 2.0;   // a macro pass, MaintainWorld's MAINT_STEP

std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

// The rules as the data file has them, put back after a test changed them.
struct RulesGuard
{
    WorldRules::Security security = WorldRules::SecurityRules();
    ~RulesGuard() { WorldRules::SetSecurityRules(security); }
};

// Tau Verge, held by Deep Haul [DH] -- anna its CEO, boris a member -- by an outpost of
// anna's, with nobody else's ships, station or claim there: what happens to its security is
// what the corporation pays for.
struct Held
{
    Simulation        sim;
    const std::string sys = "verge";
    float             x = 100000.0f;

    // `load` false: the registries as they are -- a second world beside a first must not
    // load them again under the first one's entities.
    explicit Held(bool load = true)
    {
        if (load)
        {
            REQUIRE(Factions::Load(DataFile("factions.json")));
            REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
            REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
            REQUIRE(WorldRules::Load(DataFile("world_rules.json")));
        }
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
        REQUIRE(r.Join("boris", "DH", NOW).Ok());

        // Core nobody's -- and nobody's garrison -- opens verge's front (#431 D3); verge
        // nobody's, with no station of the world's to stand for it.
        for (const char* id : { "core", "verge" })
        {
            SystemAggregate& a = sim.SystemById(id)->agg;
            a.claimed = false;
            a.controller = Factions::Unaffiliated();
            for (FactionId g : Factions::All())
                a.presence[g] = 0.0f;
            a.police = a.pirates = 0.0f;
        }
        auto& e = St().entities;
        e.erase(std::remove_if(e.begin(), e.end(), [](const std::unique_ptr<Entity>& en)
                               { return en->GetKind() == EntityKind::Station; }),
                e.end());
        St().profile = Simulation::ProfileOf(St());
        Build("outpost", "anna");
        for (int i = 0; i < Simulation::ContestPasses() + 1; i++)
            sim.StepControl(false);
        REQUIRE(Agg().holderCorp == Corp());
        Agg().baseSecurity = Agg().security = 0.2f;
    }
    SystemState&     St() { return *sim.SystemById(sys); }
    SystemAggregate& Agg() { return St().agg; }
    std::string      Corp() { return sim.Corporations().IdOf("anna"); }
    double           Treasury() { return sim.Corporations().Find(Corp())->treasury; }

    int Build(const std::string& blueprint, const std::string& owner)
    {
        const Blueprint* bp = Blueprints::Find(blueprint);
        REQUIRE(bp != nullptr);
        x += 20000.0f;
        auto t = std::make_unique<Structure>(Vector2{ x, -300000.0f }, 0.0f,
                                             blueprint + " " + owner, bp->archetype);
        t->SetBlueprint(blueprint);
        const int id = sim.AddStatic(sys, std::move(t), owner);
        REQUIRE(id != 0);
        return id;
    }
    Corps::Outcome Police(const std::string& who, const std::string& value,
                          const std::string& where = "verge")
    {
        Corps::Act act;
        act.verb = "police";
        act.value = value;
        act.system = where;
        return sim.ApplyCorpAct(who, act, {}, NOW);
    }
    ClientSession& Pilot(const std::string& account, Vector2 at)
    {
        ClientSession& s = sim.CreateSession(sys, at, GetShipCatalog()[0].stats);
        s.accountName = account;
        return s;
    }
};

int PoliceIn(const SystemState& st)
{
    int n = 0;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police &&
            static_cast<const NpcShip&>(*e).IsAlive())
            n++;
    return n;
}
}  // namespace

TEST_CASE("a corporation's police budget: set by its officers, only where it holds (#431 slice "
          "11)")
{
    Held                        h;
    const WorldRules::Security& r = WorldRules::SecurityRules();
    CHECK_FALSE(h.Police("boris", "1000").Ok());         // a member does not set policies
    CHECK_FALSE(h.Police("anna", "1000", "core").Ok());  // it does not hold core
    CHECK_FALSE(h.Police("anna", "lots").Ok());
    CHECK_FALSE(h.Police("anna", std::to_string((int)r.budgetMax + 1)).Ok());  // over the cap
    REQUIRE(h.Police("anna", "1200").Ok());
    CHECK(h.sim.Corporations().Find(h.Corp())->police.at("verge") == doctest::Approx(1200.0));
    // By the name a player reads, too, and "none" stops it.
    std::string name;
    for (const auto& si : h.sim.Universe().systems)
        if (si.id == "verge")
            name = si.name;
    REQUIRE_FALSE(name.empty());
    REQUIRE(h.Police("anna", "none", name).Ok());
    CHECK(h.sim.Corporations().Find(h.Corp())->police.empty());

    SUBCASE("kept in corps.json")
    {
        REQUIRE(h.Police("anna", "900").Ok());
        Corps::Registry back;
        std::string     why;
        REQUIRE(back.FromJson(h.sim.Corporations().ToJson(), why));
        CHECK(back.Find(h.Corp())->police.at("verge") == doctest::Approx(900.0));
    }
    SUBCASE("on the wire, and in the view its members are sent")
    {
        Proto::CorpAct act;
        act.verb = "police";
        act.system = "verge";
        act.value = "900";
        Proto::CorpAct back;
        REQUIRE(Proto::DecodeCorpAct(Proto::EncodeCorpAct(act), back));
        CHECK(back.system == "verge");
        CHECK(back.value == "900");
        Proto::CorpView v;
        v.id = "c1";
        v.police["verge"] = 900.0;
        Proto::CorpView vb;
        REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(v), vb));
        CHECK(vb.police.at("verge") == doctest::Approx(900.0));
    }
}

TEST_CASE("paying police raises security over time, and it stops when the treasury runs dry "
          "(#431 slice 11)")
{
    Held                        h;
    const WorldRules::Security& r = WorldRules::SecurityRules();
    const double                budget = 3000.0;
    const double                perPass = budget * PASS / 3600.0;
    REQUIRE(h.sim.Corporations().Credit(h.Corp(), perPass * 300.0 + 0.5 * perPass));
    REQUIRE(h.Police("anna", "3000").Ok());
    FactionId law;
    CHECK_FALSE(h.sim.LawIn(h.St(), law));  // nothing is paid yet: no law here

    // Nobody pays: security stays at its base. Paying: it climbs toward base plus the swing.
    const float base = h.Agg().security;
    for (int i = 0; i < 150; i++)
        h.sim.StepWorldMacro();
    CHECK(h.Agg().paidPolice == doctest::Approx((float)budget));
    CHECK(h.Treasury() == doctest::Approx(perPass * 150.5).epsilon(1e-6));
    CHECK(h.Agg().security > base + 0.8f * (float)r.Swing(budget));
    CHECK(h.Agg().security <= base + (float)r.Swing(budget) + 1e-3f);
    CHECK(h.sim.LawIn(h.St(), law));
    CHECK(Factions::IsLawful(law));

    // The money runs out: the police stand down, once said, and security falls back.
    for (int i = 0; i < 300; i++)
        h.sim.StepWorldMacro();
    CHECK(h.Agg().paidPolice == 0.0f);
    CHECK(h.Treasury() < perPass);
    CHECK(h.Agg().security < base + 0.05f);
    CHECK_FALSE(h.sim.LawIn(h.St(), law));
    int told = 0;
    for (const Ev::Event& e : h.sim.Corporations().Find(h.Corp())->journal)
        told += e.text.find("can no longer pay for police in") != std::string::npos ? 1 : 0;
    CHECK(told == 1);
}

TEST_CASE("a corporation's paid police make its system lawful, fly there, and answer a crime "
          "(#431 slice 11)")
{
    Held h;
    h.sim.Corporations().Credit(h.Corp(), 1.0e6);
    ClientSession& anna = h.Pilot("anna", { 300000.0f, -300000.0f });
    ClientSession& dima = h.Pilot("dima", { 300400.0f, -300000.0f });
    FactionId      law;
    // Unpaid, a corporation's system is lawless: shooting a pilot there is no crime.
    CHECK_FALSE(h.sim.IsCrime(h.St(), dima, *anna.ship, law));

    REQUIRE(h.Police("anna", "3600").Ok());  // six ships' worth at the data's 600 an hour
    const int ships = WorldRules::SecurityRules().Ships(3600.0);
    CHECK(ships == 6);
    // The director flies them, a couple a pass, under the world's police's flag.
    for (int i = 0; i < 10; i++)
        h.sim.MaintainWorld((float)PASS);
    CHECK(h.sim.KeepOf(h.sys, NpcRole::Police) == ships);
    REQUIRE(PoliceIn(h.St()) == ships);
    h.Agg().security = 0.6f;  // the middle band: its local police, at once
    CHECK(h.sim.IsCrime(h.St(), dima, *anna.ship, law));
    CHECK(Factions::IsLawful(law));
    CHECK(law == h.Agg().policeFaction);
    h.sim.MarkCrime(dima, false);
    CHECK(h.sim.CallPolice(h.St(), dima, law));
    WorldRules::Law::Band band;
    REQUIRE(WorldRules::LawRules().BandFor(h.Agg().security, band));
    if (band.delay > 0.0)
    {
        h.sim.SetTime(h.sim.Time() + band.delay + 0.1);
        h.sim.StepPoliceCall(h.St());
    }
    int beside = 0;
    for (const auto& e : h.St().entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police)
        {
            const Vector2 a = e->GetPosition(), b = dima.ship->GetPosition();
            beside += std::hypot(a.x - b.x, a.y - b.y) <= 3500.0f ? 1 : 0;
        }
    CHECK(beside == std::min(ships, band.local));
}

TEST_CASE("the higher the security, the fewer of the pirates' strength fly (#431 slice 11)")
{
    Held h;
    // Somewhere dark enough for pirates: a system with a belt.
    std::string dark;
    for (auto& kv : h.sim.Systems())
        if (dark.empty() && kv.second.profile.belts > 0)
            dark = kv.first;
    REQUIRE_FALSE(dark.empty());
    SystemAggregate& a = h.sim.SystemById(dark)->agg;
    a.presence[Factions::Pirates()] = 10.0f;
    a.security = 0.0f;
    const int low = h.sim.KeepOf(dark, NpcRole::Pirate);
    a.security = 1.0f;
    const int high = h.sim.KeepOf(dark, NpcRole::Pirate);
    CHECK(low == 10);
    CHECK(high == (int)std::lround(10.0 * WorldRules::SecurityRules().PiratesFly(1.0)));
    CHECK(high < low);
}

TEST_CASE("a convoy is set upon less the higher the security (#431 slice 11)")
{
    Held  h;  // the rules from the file
    float last = 2.0f;
    for (float sec : { 0.0f, 0.3f, 0.6f, 1.0f })
    {
        const float chance = Simulation::AmbushChance(4.0f, sec);
        CHECK(chance < last);
        last = chance;
    }
    CHECK(Simulation::AmbushChance(4.0f, 1.0f) > 0.0f);  // never safe: police are not a wall
}

TEST_CASE("the lower the security, the better the work pays and the richer the ore (#431 slice "
          "11)")
{
    Held                        h;
    const WorldRules::Security& r = WorldRules::SecurityRules();
    CHECK(r.Pay(0.0) == doctest::Approx(r.payAtZero));
    CHECK(r.Pay(1.0) == doctest::Approx(1.0));
    CHECK(r.Pay(0.2) > r.Pay(0.8));

    SUBCASE("a station's offers")
    {
        SystemState& core = *h.sim.SystemById("core");
        int          station = 0;
        for (const auto& e : core.entities)
            if (station == 0 && e->GetKind() == EntityKind::Station)
                station = e->GetId();
        REQUIRE(station != 0);
        ClientSession& s =
            h.sim.CreateSession("core", Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = "erik";
        s.dockedStationId = station;
        auto offers = [&](float security)
        {
            core.agg.security = security;
            SetRandomSeed(77u);
            h.sim.GenerateDockOffers(s);
            double sum = 0.0;
            for (const Mission& m : s.missions.Offers())
                if (!IsPlanJob(m.type) && m.type != MissionType::Contract)
                    sum += m.rewardMoney;
            return sum;
        };
        const double safe = offers(1.0f), risky = offers(0.1f);
        REQUIRE(safe > 0.0);
        CHECK(risky == doctest::Approx(safe * r.Pay(0.1)));
    }
    SUBCASE("ore out of a belt")
    {
        ClientSession& s = h.Pilot("erik", { 0.0f, 0.0f });
        auto field = std::make_unique<AsteroidField>(Vector2{ 20.0f, 0.0f }, 30.0f, "Belt",
                                                     AllResourceTypes()[0], 100000);
        field->SetId(9055);
        h.St().entities.push_back(std::move(field));
        s.ship->SetMiningOn(true);
        auto mine = [&](float security)
        {
            h.Agg().security = security;
            s.miningProgress = 0.0f;
            int mined = 0;
            for (int i = 0; i < 300; i++)  // five seconds: short of a full hold
                mined += h.sim.StepPlayerMining(s, h.St(), 1.0f, 1.0f / 60.0f).minedUnits;
            s.ship->RemoveCargo(AllResourceTypes()[0], mined);
            return mined;
        };
        const int safe = mine(1.0f), risky = mine(0.0f);
        REQUIRE(safe > 0);
        CHECK(risky > safe);
    }
}

TEST_CASE("bought security is the same in a cold system as in a hot one (#431 slice 11)")
{
    // Two worlds alike but for one thing: in one, somebody is always in verge.
    Held cold, hot(false);
    for (Held* h : { &cold, &hot })
    {
        h->sim.Corporations().Credit(h->Corp(), 500.0);
        REQUIRE(h->Police("anna", "1800").Ok());
    }
    hot.sim.SetAllHot(true);
    hot.sim.Warm(hot.St());
    for (int i = 0; i < 400; i++)
    {
        cold.sim.MaintainWorld((float)PASS);
        hot.sim.MaintainWorld((float)PASS);
    }
    REQUIRE(cold.Agg().holderCorp == cold.Corp());
    REQUIRE(hot.Agg().holderCorp == hot.Corp());
    CHECK(cold.Treasury() == doctest::Approx(hot.Treasury()));
    CHECK(cold.Agg().paidPolice == doctest::Approx(hot.Agg().paidPolice));
    CHECK(cold.Agg().security == doctest::Approx(hot.Agg().security).epsilon(0.03));
    CHECK(cold.Agg().security > 0.2f);
}
