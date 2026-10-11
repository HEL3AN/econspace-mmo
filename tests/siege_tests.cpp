#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Derelict.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"

// Outposts over time (#295, slice 5): an outpost costs its faction something every period, one
// nobody supplies falls into ruin, a lost outpost takes its claim with it, and a faction may
// besiege an enemy's -- as arithmetic where nobody is, and as warships where somebody is.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    Factions::Load(DATA + "factions.json");
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

std::unique_ptr<Simulation> World(bool allHot = false)
{
    SetRandomSeed(0xC0FFEEu);
    LoadRegistries();
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(DATA + "universe.json");
    sim->AttachRegion(7, DATA + "systems/");
    sim->Seed(1234u);
    sim->SetAllHot(allHot);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(DATA + "systems/");
    return sim;
}

// The host's tick with nobody's input, for a whole coarse pass.
void Pass(Simulation& sim, int passes = 1)
{
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < passes * 120; i++)
    {
        for (auto& kv : sim.Systems())
            sim.StepSystemAgents(kv.second, {}, nullptr, dt);
        sim.MaintainWorld(dt);
    }
}

constexpr int PERIOD = 30;  // passes in a faction period

Structure* OutpostIn(Simulation& sim, const std::string& system)
{
    for (const auto& e : sim.SystemById(system)->entities)
        if (e->GetKind() == EntityKind::Structure &&
            Outposts::IsOutpost(static_cast<const Structure&>(*e).GetBuilds()))
            return static_cast<Structure*>(e.get());
    return nullptr;
}

bool Wrecked(const Simulation& sim, const std::string& system)
{
    for (const auto& e : sim.SystemById(system)->entities)
        if (e->GetKind() == EntityKind::Derelict && e->GetArchetype() != nullptr &&
            e->GetArchetype()->id == "derelict.outpost_ruin")
            return true;
    return false;
}

bool Told(const Simulation& sim, const std::string& kind, const std::string& system,
          const std::string& words)
{
    for (const ChronicleEntry& e : sim.Chronicle())
        if (e.kind == kind && e.system == system && e.text.find(words) != std::string::npos)
            return true;
    return false;
}

// A pirate holding and a system next door that we hand to the Traders Guild, with a finished
// outpost of the Guild's standing in it: the scene every test here plays on.
struct Scene
{
    std::string den;       // the pirates'
    std::string frontier;  // the Guild's, by its outpost
    int         outpost = 0;
};

Scene GuildOutpostNextToPirates(Simulation& sim)
{
    Scene s;
    for (const auto& kv : sim.Systems())
    {
        if (!s.den.empty())
            break;
        if (!kv.second.agg.claimed || kv.second.agg.controller != FactionId::Pirates)
            continue;
        for (const std::string& n : sim.Neighbors(kv.first))
            if (n != sim.Universe().startId && !sim.SystemById(n)->agg.claimed)
            {
                s.den = kv.first;
                s.frontier = n;
                break;
            }
    }
    REQUIRE_FALSE(s.den.empty());
    const Simulation::OutpostSpot spot =
        sim.FindOutpostSpot(FactionId::TradersGuild, Outposts::Purpose::Open, s.den, s.frontier);
    REQUIRE(spot.found);
    const Blueprint* bp = Blueprints::Find(Outposts::BLUEPRINT);
    auto t = std::make_unique<Structure>(spot.at, 0.0f, "Traders Guild Outpost", bp->archetype);
    t->SetBlueprint(bp->id);
    s.outpost = sim.AddStatic(s.frontier, std::move(t), Outposts::OwnerOf(FactionId::TradersGuild));
    REQUIRE(s.outpost != 0);
    SystemAggregate& a = sim.SystemById(s.frontier)->agg;
    a.claimed = true;
    a.controller = FactionId::TradersGuild;
    return s;
}

const Plan* SiegeBy(const Simulation& sim, FactionId f)
{
    for (const auto& kv : sim.Plans())
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Siege)
            return &kv.second;
    return nullptr;
}

// The pirates next door with strength to spare and the stock for it, the Guild's garrison
// thin, and the pirates knowing it: within a period or two they lay siege.
const Plan* PiratesBesiege(Simulation& sim, const Scene& s)
{
    const float cap = Factions::TemperamentOf(FactionId::Pirates).capacity;
    for (int pass = 0; pass < 3 * PERIOD && SiegeBy(sim, FactionId::Pirates) == nullptr; pass++)
    {
        // Only the den has anything to spare, so the den is where the pirates act from, and
        // the frontier is all they know of beyond their holdings: nothing else to choose.
        for (auto& kv : sim.Systems())
        {
            const bool held =
                kv.second.agg.claimed && kv.second.agg.controller == FactionId::Pirates;
            kv.second.agg.presence[(int)FactionId::Pirates] = kv.first == s.den ? cap
                                                              : held            ? cap * 0.6f
                                                                                : 0.0f;
            if (!held && kv.first != s.frontier)
                sim.MindOf(FactionId::Pirates).intel.erase(kv.first);
        }
        SystemAggregate& a = sim.SystemById(s.frontier)->agg;
        a.presence[(int)FactionId::TradersGuild] = 1.0f;
        a.presence[(int)FactionId::Pirates] = 0.0f;
        sim.MindOf(FactionId::Pirates).stock = Simulation::OutpostCost() * 2.0f;
        sim.MindOf(FactionId::Pirates).intel[s.frontier] = sim.Observe(*sim.SystemById(s.frontier));
        Pass(sim);
    }
    const Plan* p = SiegeBy(sim, FactionId::Pirates);
    REQUIRE(p != nullptr);
    return p;
}

// Runs passes until `done` or `seconds` of world time have gone.
template <typename F> void RunUntil(Simulation& sim, double seconds, F done)
{
    const double until = sim.Time() + seconds;
    while (sim.Time() < until && !done())
        Pass(sim);
}
}  // namespace

TEST_CASE("upkeep and sieges are rules in the data, and a wrong one does not load (#295)")
{
    LoadRegistries();
    const Blueprints::Upkeep& u = Blueprints::UpkeepRules();
    CHECK(u.share > 0.0f);
    CHECK(u.decay > 0.0f);
    CHECK(u.repair > 0.0f);
    CHECK(u.repair < u.decay);  // left alone, an unsupplied outpost falls faster than one mends
    const Blueprints::Siege& r = Blueprints::SiegeRules();
    CHECK(r.seconds > 0.0f);
    CHECK(r.dps > 0.0f);
    CHECK(Simulation::SiegeCost() > 0.0f);
    CHECK(Simulation::SiegeCost() < Simulation::OutpostCost());

    const std::string path = "siege_rules_tmp.json";
    auto              refused = [&](const std::string& extra)
    {
        {
            std::ofstream out(path);
            out << "{ \"blueprints\": [], " << extra << " }";
        }
        const bool ok = Blueprints::Load(path);
        std::remove(path.c_str());
        return !ok;
    };
    CHECK(refused("\"upkeep\": { \"share\": 0.01, \"decay\": 2 }"));         // a share over 1
    CHECK(refused("\"upkeep\": { \"shares\": 0.01 }"));                      // a misspelling
    CHECK(refused("\"siege\": { \"dps\": -1 }"));                            // negative
    CHECK(refused("\"siege\": { \"commit\": 0.5, \"seconds\": \"ten\" }"));  // not a number
    LoadRegistries();
    CHECK(Blueprints::UpkeepRules().decay == doctest::Approx(u.decay));
}

TEST_CASE("a held outpost is paid for every period, and mends what it was hit for (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    Structure*  t = OutpostIn(*sim, s.frontier);
    REQUIRE(t != nullptr);
    const float hull = t->MaxHull(sim->Time());
    t->SetDamage(hull * 0.3f);

    // Paid for out of the stock, a period's mending at a time.
    for (int pass = 0; pass < PERIOD; pass++)
    {
        sim->MindOf(FactionId::TradersGuild).stock = 10.0f;
        Pass(*sim);
    }
    CHECK(OutpostIn(*sim, s.frontier)->GetDamage() < hull * 0.3f);
    CHECK(OutpostIn(*sim, s.frontier)->GetDamage() ==
          doctest::Approx(hull * (0.3f - Blueprints::UpkeepRules().repair)));
    CHECK(sim->MindOf(FactionId::TradersGuild).unsupplied.empty());
}

TEST_CASE("an outpost nobody pays for falls into ruin, cold, and takes its claim with it (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    REQUIRE_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));
    const float hull = OutpostIn(*sim, s.frontier)->MaxHull(sim->Time());

    auto broke = [&]() { sim->MindOf(FactionId::TradersGuild).stock = 0.0f; };
    for (int pass = 0; pass < PERIOD; pass++)
    {
        broke();
        Pass(*sim);
    }
    REQUIRE(OutpostIn(*sim, s.frontier) != nullptr);
    CHECK(OutpostIn(*sim, s.frontier)->GetDamage() ==
          doctest::Approx(hull * Blueprints::UpkeepRules().decay));
    CHECK(Told(*sim, "upkeep", s.frontier, "cannot pay"));
    CHECK(sim->MindOf(FactionId::TradersGuild).unsupplied.size() == 1);
    // Said once, not every period.
    for (int pass = 0; pass < PERIOD; pass++)
    {
        broke();
        Pass(*sim);
    }
    int said = 0;
    for (const ChronicleEntry& e : sim->Chronicle())
        said += e.kind == "upkeep" && e.text.find("cannot pay") != std::string::npos ? 1 : 0;
    CHECK(said == 1);

    // Left so, it is a ruin, and the system is nobody's.
    const int periods = (int)std::ceil(1.0f / Blueprints::UpkeepRules().decay) + 1;
    for (int pass = 0; pass < periods * PERIOD && OutpostIn(*sim, s.frontier) != nullptr; pass++)
    {
        broke();
        Pass(*sim);
    }
    CHECK(OutpostIn(*sim, s.frontier) == nullptr);
    CHECK(Wrecked(*sim, s.frontier));
    CHECK(Told(*sim, "upkeep", s.frontier, "fell into ruin"));
    CHECK_FALSE(sim->SystemById(s.frontier)->agg.claimed);
    CHECK(Told(*sim, "claim", s.frontier, "no longer holds"));
    CHECK(sim->MindOf(FactionId::TradersGuild).unsupplied.empty());
    CHECK_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));  // nobody had to be there
}

TEST_CASE(
    "an outpost in a system its faction does not hold is abandoned, whatever the stock (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    sim->SystemById(s.frontier)->agg.controller = FactionId::Syndicate;
    for (int pass = 0; pass < PERIOD; pass++)
    {
        sim->MindOf(FactionId::TradersGuild).stock = Simulation::OutpostCost();
        Pass(*sim);
    }
    CHECK(OutpostIn(*sim, s.frontier)->GetDamage() > 0.0f);
    CHECK(Told(*sim, "upkeep", s.frontier, "abandoned"));
}

TEST_CASE("a lost outpost undoes the claim it anchored, and only shakes one it did not (#295)")
{
    SUBCASE("the outpost was the claim")
    {
        auto         sim = World();
        const Scene  s = GuildOutpostNextToPirates(*sim);
        SystemState& st = *sim->SystemById(s.frontier);
        st.agg.presence[(int)FactionId::TradersGuild] = 4.0f;
        CHECK(sim->DamageStructure(st, s.outpost, 1.0e9f, nullptr));
        CHECK_FALSE(st.agg.claimed);
        CHECK(st.agg.presence[(int)FactionId::TradersGuild] ==
              doctest::Approx(4.0f * (1.0f - Blueprints::SiegeRules().shock)));
        CHECK(Told(*sim, "lost", s.frontier, "Traders Guild lost its outpost"));
        CHECK(Told(*sim, "claim", s.frontier, "Traders Guild no longer holds"));
        // ...and it remembers where it was beaten when it thinks of building there again.
        CHECK(sim->MindOf(FactionId::TradersGuild).lostAt.count(s.frontier) == 1);
        CHECK(Intelligence::LossRisk(sim->MindOf(FactionId::TradersGuild).lostAt, s.frontier,
                                     sim->Time()) == doctest::Approx(Intelligence::LOSS_RISK));
        CHECK(Intelligence::LossRisk(sim->MindOf(FactionId::TradersGuild).lostAt, s.frontier,
                                     sim->Time() + Intelligence::LOSS_MEMORY) == 0.0f);
        bool news = false;
        for (const std::string& line : sim->Events())
            news = news || line.find("no longer holds") != std::string::npos;
        CHECK(news);
    }
    SUBCASE("a second outpost still anchors it")
    {
        auto             sim = World();
        const Scene      s = GuildOutpostNextToPirates(*sim);
        const Blueprint* bp = Blueprints::Find("outpost_watch");
        REQUIRE(bp != nullptr);
        auto t =
            std::make_unique<Structure>(Vector2{ -400000.0f, 0.0f }, 0.0f, "Watch", bp->archetype);
        REQUIRE(sim->AddStatic(s.frontier, std::move(t),
                               Outposts::OwnerOf(FactionId::TradersGuild)) != 0);
        SystemState& st = *sim->SystemById(s.frontier);
        CHECK(sim->DamageStructure(st, s.outpost, 1.0e9f, nullptr));
        CHECK(st.agg.claimed);
        CHECK(st.agg.controller == FactionId::TradersGuild);
        CHECK(Told(*sim, "claim", s.frontier, "is shaken"));
    }
}

TEST_CASE("a held system with its holder's outpost standing is not taken by strength (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    const float cap = Factions::TemperamentOf(FactionId::Pirates).capacity;
    auto        swarm = [&](int passes)
    {
        for (int i = 0; i < passes; i++)
        {
            SystemAggregate& a = sim->SystemById(s.frontier)->agg;
            a.presence[(int)FactionId::Pirates] = cap * 3.0f;
            a.presence[(int)FactionId::TradersGuild] = 0.5f;
            for (int fi = 0; fi < FACTION_COUNT; fi++)
                sim->MindOf((FactionId)fi).stock = 0.0f;        // nobody besieges or builds
            sim->MindOf(FactionId::TradersGuild).stock = 1.0f;  // ...but the upkeep is paid
            Pass(*sim);
        }
    };
    swarm(Simulation::ContestPasses() + 10);
    CHECK(sim->SystemById(s.frontier)->agg.claimed);
    CHECK(sim->SystemById(s.frontier)->agg.controller == FactionId::TradersGuild);
}

TEST_CASE("pirates besiege a thin garrison's outpost, cold, and bring it and the claim down "
          "(#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    const float stock = Simulation::OutpostCost() * 2.0f;
    const Plan  plan = *PiratesBesiege(*sim, s);

    CHECK(plan.target == s.frontier);
    CHECK(plan.from == s.den);
    CHECK(plan.against == FactionId::TradersGuild);
    CHECK(plan.site == sim->StaticKey(s.frontier, s.outpost));
    CHECK(plan.force >= 1.5f);
    CHECK(plan.dueAt > plan.startedAt + Blueprints::SiegeRules().seconds - 1.0);
    CHECK(sim->MindOf(FactionId::Pirates).stock < stock);
    CHECK(sim->SystemById(s.frontier)->agg.presence[(int)FactionId::Pirates] >= plan.force - 0.5f);
    CHECK(Told(*sim, "siege", s.frontier, "Pirates laid siege to the Traders Guild outpost"));
    CHECK(sim->SiegeForceIn(s.frontier, FactionId::Pirates) == doctest::Approx(plan.force));

    // Worn down where nobody is, before the besiegers' time is up.
    RunUntil(*sim, plan.dueAt - sim->Time(),
             [&]() { return OutpostIn(*sim, s.frontier) == nullptr; });
    CHECK(OutpostIn(*sim, s.frontier) == nullptr);
    CHECK(sim->Time() < plan.dueAt);
    CHECK(SiegeBy(*sim, FactionId::Pirates) == nullptr);
    CHECK(Told(*sim, "siege", s.frontier, "Pirates destroyed the Traders Guild outpost"));
    CHECK_FALSE(sim->SystemById(s.frontier)->agg.claimed);
    CHECK(Wrecked(*sim, s.frontier));
    CHECK_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));
}

TEST_CASE("a siege that runs out of time is given up, and the outpost mends (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    const Plan  plan = *PiratesBesiege(*sim, s);
    // A garrison that ties every besieger up: nothing reaches the outpost.
    RunUntil(*sim, plan.dueAt - sim->Time() + 10.0,
             [&]()
             {
                 sim->SystemById(s.frontier)->agg.presence[(int)FactionId::TradersGuild] =
                     plan.force / Blueprints::SiegeRules().defence + 1.0f;
                 return SiegeBy(*sim, FactionId::Pirates) == nullptr;
             });
    CHECK(SiegeBy(*sim, FactionId::Pirates) == nullptr);
    REQUIRE(OutpostIn(*sim, s.frontier) != nullptr);
    CHECK((Told(*sim, "siege", s.frontier, "gave up the siege") ||
           Told(*sim, "siege", s.frontier, "drove the Pirates besiegers")));
    CHECK(sim->SystemById(s.frontier)->agg.claimed);
}

TEST_CASE("where somebody is, a siege is warships a player can watch and shoot down (#295)")
{
    auto           sim = World();
    const Scene    s = GuildOutpostNextToPirates(*sim);
    ClientSession& me =
        sim->CreateSession(s.frontier, sim->SafeArrival(s.frontier), GetShipCatalog()[0].stats);
    Pass(*sim);
    REQUIRE(Simulation::IsHot(*sim->SystemById(s.frontier)));
    const Plan plan = *PiratesBesiege(*sim, s);
    (void)me;

    auto warships = [&]()
    {
        std::vector<NpcShip*> out;
        for (const auto& e : sim->SystemById(s.frontier)->entities)
            if (e->GetKind() == EntityKind::Npc &&
                static_cast<NpcShip*>(e.get())->GetRole() == NpcRole::Warship)
                out.push_back(static_cast<NpcShip*>(e.get()));
        return out;
    };
    std::vector<NpcShip*> fleet = warships();
    REQUIRE((int)fleet.size() == (int)std::lround(SiegeBy(*sim, FactionId::Pirates)->force));
    for (const NpcShip* n : fleet)
    {
        CHECK(n->GetFaction() == FactionId::Pirates);
        CHECK(n->GetDesign() == "cruiser");  // the doctrine's warship
    }

    // One shot down: the siege is a ship weaker, and so is the pirates' strength there.
    const float force = SiegeBy(*sim, FactionId::Pirates)->force;
    const float there = sim->SystemById(s.frontier)->agg.presence[(int)FactionId::Pirates];
    fleet[0]->TakeDamage(1.0e6f);
    Pass(*sim);
    REQUIRE(SiegeBy(*sim, FactionId::Pirates) != nullptr);
    CHECK(SiegeBy(*sim, FactionId::Pirates)->force < force - 0.9f);
    CHECK(sim->SystemById(s.frontier)->agg.presence[(int)FactionId::Pirates] < there - 0.9f);
    CHECK(warships().size() == fleet.size() - 1);

    // The player leaves; the system cools and the siege goes on as numbers -- and their ships
    // are back when somebody returns.
    sim->DestroySession(me.id);
    RunUntil(*sim, Simulation::COOL_AFTER + 10.0,
             [&]() { return !Simulation::IsHot(*sim->SystemById(s.frontier)); });
    REQUIRE_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));
    if (SiegeBy(*sim, FactionId::Pirates) != nullptr)
    {
        sim->CreateSession(s.frontier, sim->SafeArrival(s.frontier), GetShipCatalog()[0].stats);
        Pass(*sim);
        if (SiegeBy(*sim, FactionId::Pirates) != nullptr)
            CHECK((int)warships().size() ==
                  (int)std::lround(SiegeBy(*sim, FactionId::Pirates)->force));
    }
    (void)plan;
}

TEST_CASE("a siege goes the same way whether anybody watches or not (#295)")
{
    double fell[2] = { -1.0, -1.0 };
    for (int hot = 0; hot < 2; hot++)
    {
        auto        sim = World(hot == 1);
        const Scene s = GuildOutpostNextToPirates(*sim);
        const Plan  plan = *PiratesBesiege(*sim, s);
        RunUntil(*sim, plan.dueAt - sim->Time() + 10.0,
                 [&]() { return OutpostIn(*sim, s.frontier) == nullptr; });
        if (OutpostIn(*sim, s.frontier) == nullptr)
            fell[hot] = sim->Time();
    }
    REQUIRE(fell[0] > 0.0);
    REQUIRE(fell[1] > 0.0);
    CHECK(std::fabs(fell[0] - fell[1]) <= 60.0);
}

TEST_CASE("a siege and what a faction cannot supply survive a restart (#295)")
{
    const std::string path = "siege_world_tmp.json";
    auto              a = World();
    const Scene       s = GuildOutpostNextToPirates(*a);
    const Plan        plan = *PiratesBesiege(*a, s);
    a->MindOf(FactionId::Syndicate).unsupplied.insert("W-1.1/+7");
    a->MindOf(FactionId::Syndicate).lostAt[s.frontier] = 123.0;
    a->SaveWorld(path);

    Simulation b;
    b.LoadUniverse(DATA + "universe.json");
    b.AttachRegion(7, DATA + "systems/");
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    std::remove(path.c_str());

    REQUIRE(b.Plans().count(plan.id) == 1);
    const Plan back = b.Plans().at(plan.id);
    CHECK(back.kind == Plan::Kind::Siege);
    CHECK(back.against == FactionId::TradersGuild);
    CHECK(back.site == plan.site);
    CHECK(back.from == plan.from);
    CHECK(back.force == doctest::Approx(SiegeBy(*a, FactionId::Pirates)->force));
    CHECK(back.dueAt == doctest::Approx(plan.dueAt));
    CHECK(b.MindOf(FactionId::Syndicate).unsupplied.count("W-1.1/+7") == 1);
    CHECK(b.MindOf(FactionId::Syndicate).lostAt.at(s.frontier) == doctest::Approx(123.0));
}
