#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/Derelict.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "missions/Mission.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// Outposts over time (#295, slice 5): an outpost costs its faction something every period, one
// nobody supplies falls into ruin, a lost outpost takes its claim with it, and a faction may
// besiege an enemy's -- as arithmetic where nobody is, and as warships where somebody is.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

std::unique_ptr<Simulation> World(bool allHot = false)
{
    SetRandomSeed(0xC0FFEEu);
    LoadRegistries();
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->SetAllHot(allHot);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
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
        if (!kv.second.agg.claimed || kv.second.agg.controller != Fac::Pirates())
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
        sim.FindOutpostSpot(Fac::TradersGuild(), Outposts::Purpose::Open, s.den, s.frontier);
    REQUIRE(spot.found);
    const Blueprint* bp = Blueprints::Find(Outposts::BLUEPRINT);
    auto t = std::make_unique<Structure>(spot.at, 0.0f, "Traders Guild Outpost", bp->archetype);
    t->SetBlueprint(bp->id);
    s.outpost = sim.AddStatic(s.frontier, std::move(t), Outposts::OwnerOf(Fac::TradersGuild()));
    REQUIRE(s.outpost != 0);
    SystemAggregate& a = sim.SystemById(s.frontier)->agg;
    a.claimed = true;
    a.controller = Fac::TradersGuild();
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
    const float cap = Factions::TemperamentOf(Fac::Pirates()).capacity;
    for (int pass = 0; pass < 3 * PERIOD && SiegeBy(sim, Fac::Pirates()) == nullptr; pass++)
    {
        // Only the den has anything to spare, so the den is where the pirates act from, and
        // the frontier is all they know of beyond their holdings: nothing else to choose.
        for (auto& kv : sim.Systems())
        {
            const bool held = kv.second.agg.claimed && kv.second.agg.controller == Fac::Pirates();
            kv.second.agg.presence[Fac::Pirates()] = kv.first == s.den ? cap
                                                     : held            ? cap * 0.6f
                                                                       : 0.0f;
            if (!held && kv.first != s.frontier)
                sim.MindOf(Fac::Pirates()).intel.erase(kv.first);
        }
        SystemAggregate& a = sim.SystemById(s.frontier)->agg;
        a.presence[Fac::TradersGuild()] = 1.0f;
        a.presence[Fac::Pirates()] = 0.0f;
        sim.MindOf(Fac::Pirates()).stock = Simulation::OutpostCost() * 2.0f;
        sim.MindOf(Fac::Pirates()).intel[s.frontier] = sim.Observe(*sim.SystemById(s.frontier));
        Pass(sim);
    }
    const Plan* p = SiegeBy(sim, Fac::Pirates());
    REQUIRE(p != nullptr);
    // ...and a jump later they are at it.
    CHECK_FALSE(p->arrived);
    for (int pass = 0; pass < 2 * PERIOD && SiegeBy(sim, Fac::Pirates()) != nullptr &&
                       !SiegeBy(sim, Fac::Pirates())->arrived;
         pass++)
        Pass(sim);
    p = SiegeBy(sim, Fac::Pirates());
    REQUIRE(p != nullptr);
    REQUIRE(p->arrived);
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
    // Every outpost mends at its tier's rate (#431 D1), hull a second.
    for (const char* id : { "outpost", "outpost_market", "anchor" })
        CHECK(Blueprints::Find(id)->mend > 0.0f);
    const Blueprints::Siege& r = Blueprints::SiegeRules();
    CHECK(r.seconds > 0.0f);
    CHECK(Simulation::SiegeCost() > 0.0f);
    CHECK(Simulation::SiegeCost() < Simulation::OutpostCost());

    const TempDir     tmp;
    const std::string path = tmp.Path("siege_rules.json");
    auto              refused = [&](const std::string& extra)
    {
        {
            std::ofstream out(path);
            out << "{ \"blueprints\": [], " << extra << " }";
        }
        const bool ok = Blueprints::Load(path);
        return !ok;
    };
    CHECK(refused("\"upkeep\": { \"share\": 0.01, \"decay\": 2 }"));  // a share over 1
    CHECK(refused("\"upkeep\": { \"shares\": 0.01 }"));               // a misspelling
    CHECK(refused("\"siege\": { \"seconds\": -1 }"));                 // negative
    // A besieger's guns are its faction's warship design now: an old file that still names
    // a number for them is told so rather than quietly ignored.
    CHECK(refused("\"siege\": { \"dps\": 40 }"));
    CHECK(Blueprints::Error().find("warship") != std::string::npos);
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
    const float hurt = std::min(hull * 0.9f, t->MendRate() * 30.0f);  // half a period's mending
    t->SetDamage(hurt, sim->Time());
    const double from = sim->Time();

    // Paid for out of the stock, a period's mending at a time.
    for (int pass = 0; pass < PERIOD; pass++)
    {
        sim->MindOf(Fac::TradersGuild()).stock = 10.0f;
        Pass(*sim);
    }
    // ...at its tier's rate, on the clock (#431 D1): mended in full by now.
    const Structure* back = OutpostIn(*sim, s.frontier);
    CHECK(back->GetDamage(from + 15.0) ==
          doctest::Approx(hurt - back->MendRate() * 15.0f).epsilon(0.01));
    CHECK(back->GetDamage(sim->Time()) == 0.0f);
    CHECK(sim->MindOf(Fac::TradersGuild()).unsupplied.empty());
}

TEST_CASE("an outpost nobody pays for falls into ruin, cold, and takes its claim with it (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    REQUIRE_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));
    const float hull = OutpostIn(*sim, s.frontier)->MaxHull(sim->Time());

    auto broke = [&]() { sim->MindOf(Fac::TradersGuild()).stock = 0.0f; };
    for (int pass = 0; pass < PERIOD; pass++)
    {
        broke();
        Pass(*sim);
    }
    REQUIRE(OutpostIn(*sim, s.frontier) != nullptr);
    CHECK(OutpostIn(*sim, s.frontier)->GetDamage(sim->Time()) ==
          doctest::Approx(hull * Blueprints::UpkeepRules().decay));
    CHECK_FALSE(OutpostIn(*sim, s.frontier)->Mends());  // unsupplied, it mends nothing
    CHECK(Told(*sim, "upkeep", s.frontier, "cannot pay"));
    CHECK(sim->MindOf(Fac::TradersGuild()).unsupplied.size() == 1);
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
    CHECK(sim->MindOf(Fac::TradersGuild()).unsupplied.empty());
    CHECK_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));  // nobody had to be there
}

TEST_CASE(
    "an outpost in a system its faction does not hold is abandoned, whatever the stock (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    sim->SystemById(s.frontier)->agg.controller = Fac::Syndicate();
    for (int pass = 0; pass < PERIOD; pass++)
    {
        sim->MindOf(Fac::TradersGuild()).stock = Simulation::OutpostCost();
        Pass(*sim);
    }
    CHECK(OutpostIn(*sim, s.frontier)->GetDamage(sim->Time()) > 0.0f);
    CHECK(Told(*sim, "upkeep", s.frontier, "abandoned"));
}

TEST_CASE("a lost outpost undoes the claim it anchored, and only shakes one it did not (#295)")
{
    SUBCASE("the outpost was the claim")
    {
        auto         sim = World();
        const Scene  s = GuildOutpostNextToPirates(*sim);
        SystemState& st = *sim->SystemById(s.frontier);
        st.agg.presence[Fac::TradersGuild()] = 4.0f;
        CHECK(sim->DamageStructure(st, s.outpost, 1.0e9f, nullptr));
        CHECK_FALSE(st.agg.claimed);
        CHECK(st.agg.presence[Fac::TradersGuild()] ==
              doctest::Approx(4.0f * (1.0f - Blueprints::SiegeRules().shock)));
        CHECK(Told(*sim, "lost", s.frontier, "Traders Guild lost its outpost"));
        CHECK(Told(*sim, "claim", s.frontier, "Traders Guild no longer holds"));
        // ...and it remembers where it was beaten when it thinks of building there again.
        CHECK(sim->MindOf(Fac::TradersGuild()).lostAt.count(s.frontier) == 1);
        CHECK(Intelligence::LossRisk(sim->MindOf(Fac::TradersGuild()).lostAt, s.frontier,
                                     sim->Time()) == doctest::Approx(Intelligence::LOSS_RISK));
        CHECK(Intelligence::LossRisk(sim->MindOf(Fac::TradersGuild()).lostAt, s.frontier,
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
        REQUIRE(sim->AddStatic(s.frontier, std::move(t), Outposts::OwnerOf(Fac::TradersGuild())) !=
                0);
        SystemState& st = *sim->SystemById(s.frontier);
        CHECK(sim->DamageStructure(st, s.outpost, 1.0e9f, nullptr));
        CHECK(st.agg.claimed);
        CHECK(st.agg.controller == Fac::TradersGuild());
        CHECK(Told(*sim, "claim", s.frontier, "is shaken"));
    }
}

TEST_CASE("a held system with its holder's outpost standing is not taken by strength (#295)")
{
    auto        sim = World();
    const Scene s = GuildOutpostNextToPirates(*sim);
    const float cap = Factions::TemperamentOf(Fac::Pirates()).capacity;
    auto        swarm = [&](int passes)
    {
        for (int i = 0; i < passes; i++)
        {
            SystemAggregate& a = sim->SystemById(s.frontier)->agg;
            a.presence[Fac::Pirates()] = cap * 3.0f;
            a.presence[Fac::TradersGuild()] = 0.5f;
            for (FactionId fi : Factions::All())
                sim->MindOf(fi).stock = 0.0f;               // nobody besieges or builds
            sim->MindOf(Fac::TradersGuild()).stock = 1.0f;  // ...but the upkeep is paid
            Pass(*sim);
        }
    };
    swarm(Simulation::ContestPasses() + 10);
    CHECK(sim->SystemById(s.frontier)->agg.claimed);
    CHECK(sim->SystemById(s.frontier)->agg.controller == Fac::TradersGuild());
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
    CHECK(plan.against == Fac::TradersGuild());
    CHECK(plan.site == sim->StaticKey(s.frontier, s.outpost));
    CHECK(plan.force >= 1.5f);
    CHECK(plan.dueAt > plan.startedAt + Blueprints::SiegeRules().seconds - 1.0);
    CHECK(sim->MindOf(Fac::Pirates()).stock < stock);
    CHECK(sim->SystemById(s.frontier)->agg.presence[Fac::Pirates()] >= plan.force - 0.5f);
    CHECK(Told(*sim, "siege", s.frontier, "Pirates laid siege to the Traders Guild outpost"));
    CHECK(sim->SiegeForceIn(s.frontier, Fac::Pirates()) == doctest::Approx(plan.force));

    // Worn down where nobody is, before the besiegers' time is up.
    RunUntil(*sim, plan.dueAt - sim->Time(),
             [&]() { return OutpostIn(*sim, s.frontier) == nullptr; });
    CHECK(OutpostIn(*sim, s.frontier) == nullptr);
    CHECK(sim->Time() < plan.dueAt);
    CHECK(SiegeBy(*sim, Fac::Pirates()) == nullptr);
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
                 sim->SystemById(s.frontier)->agg.presence[Fac::TradersGuild()] =
                     plan.force / Blueprints::SiegeRules().defence + 1.0f;
                 return SiegeBy(*sim, Fac::Pirates()) == nullptr;
             });
    CHECK(SiegeBy(*sim, Fac::Pirates()) == nullptr);
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
    REQUIRE((int)fleet.size() == (int)std::lround(SiegeBy(*sim, Fac::Pirates())->force));
    for (const NpcShip* n : fleet)
    {
        CHECK(n->GetFaction() == Fac::Pirates());
        CHECK(n->GetDesign() == "cruiser");  // the doctrine's warship
    }

    // One shot down: the siege is a ship weaker, and so is the pirates' strength there.
    const float force = SiegeBy(*sim, Fac::Pirates())->force;
    const float there = sim->SystemById(s.frontier)->agg.presence[Fac::Pirates()];
    fleet[0]->TakeDamage(1.0e6f);
    Pass(*sim);
    REQUIRE(SiegeBy(*sim, Fac::Pirates()) != nullptr);
    CHECK(SiegeBy(*sim, Fac::Pirates())->force < force - 0.9f);
    CHECK(sim->SystemById(s.frontier)->agg.presence[Fac::Pirates()] < there - 0.9f);
    CHECK(warships().size() == fleet.size() - 1);

    // The player leaves; the system cools and the siege goes on as numbers -- and their ships
    // are back when somebody returns.
    sim->DestroySession(me.id);
    RunUntil(*sim, Simulation::COOL_AFTER + 10.0,
             [&]() { return !Simulation::IsHot(*sim->SystemById(s.frontier)); });
    REQUIRE_FALSE(Simulation::IsHot(*sim->SystemById(s.frontier)));
    if (SiegeBy(*sim, Fac::Pirates()) != nullptr)
    {
        sim->CreateSession(s.frontier, sim->SafeArrival(s.frontier), GetShipCatalog()[0].stats);
        Pass(*sim);
        if (SiegeBy(*sim, Fac::Pirates()) != nullptr)
            CHECK((int)warships().size() == (int)std::lround(SiegeBy(*sim, Fac::Pirates())->force));
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

// The owner's rule of 2026-10-22: no hand-tuned number stands in for the world. What a
// besieging ship takes off a wall is its faction's warship design's guns -- the same volley
// a player's ship of that hull and fit fires, at the same cooldown.
TEST_CASE("a besieger hits as hard as its faction's warship design does")
{
    auto                    sim = World();
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    for (FactionId f : { Fac::Pirates(), Fac::TradersGuild(), Fac::Syndicate() })
    {
        const std::vector<std::string>* list = c.DesignsFor(Factions::Id(f), "warship");
        REQUIRE(list != nullptr);
        REQUIRE_FALSE(list->empty());
        float sum = 0.0f;
        for (const std::string& id : *list)
        {
            Ships::Stats st;
            std::string  why;
            REQUIRE(Ships::Derive(c, *c.FindDesign(id), st, why));
            sum += Ships::Dps(st);
        }
        CHECK(sim->WarshipDps(f) == doctest::Approx(sum / (float)list->size()));
        CHECK(sim->WarshipDps(f) > 0.0f);
    }
    // Today every doctrine's warship is the cruiser: a railgun of 6 plus every ship's 16, twice
    // a second.
    Ships::Stats cruiser;
    std::string  why;
    REQUIRE(Ships::Derive(c, *c.FindDesign("cruiser"), cruiser, why));
    CHECK(Ships::Dps(cruiser) == doctest::Approx(44.0f));
    CHECK(sim->WarshipDps(Fac::Pirates()) == doctest::Approx(Ships::Dps(cruiser)));
}

TEST_CASE("a lighter gun on the warship design makes a slower siege")
{
    namespace fs = std::filesystem;
    // When the outpost falls with the doctrine's warship as it is in `ships`.
    auto fallsAt = [](const std::string& archetypes)
    {
        SetRandomSeed(0xC0FFEEu);
        REQUIRE(Factions::Load(DATA + "factions.json"));
        REQUIRE(Archetypes::Load(archetypes));
        REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
        auto sim = std::make_unique<Simulation>();
        sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
        sim->Seed(1234u);
        sim->InitGalaxy();
        sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        const Scene s = GuildOutpostNextToPirates(*sim);
        const Plan  plan = *PiratesBesiege(*sim, s);
        const float dps = sim->WarshipDps(Fac::Pirates());
        RunUntil(*sim, plan.dueAt - sim->Time() + 10.0,
                 [&]() { return OutpostIn(*sim, s.frontier) == nullptr; });
        const double at =
            OutpostIn(*sim, s.frontier) == nullptr ? sim->Time() - plan.startedAt : 1.0e9;
        return std::make_pair(dps, at);
    };
    const auto stock = fallsAt(DATA + "archetypes.json");

    // The same data, but the cruiser's railgun swapped for a laser.
    const TempDir  tmp;
    const fs::path dir = tmp.Path("data");
    fs::create_directories(dir);
    for (const char* f : { "archetypes.json", "modules.json" })
        fs::copy_file(fs::path(DATA) / f, dir / f);
    fs::copy(fs::path(DATA) / "modules", dir / "modules", fs::copy_options::recursive);
    std::string ships;
    {
        std::ifstream in(DATA + "ships.json");
        ships.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string railgun =
        "{ \"module\": \"weapons.railgun\", \"in\": \"bow\", \"on\": \"top\", \"count\": 1 }";
    const size_t at = ships.find(railgun);
    REQUIRE(at != std::string::npos);  // the cruiser's, the one railgun fitted without a scale
    ships.replace(
        at, railgun.size(),
        "{ \"module\": \"weapons.laser\", \"in\": \"bow\", \"on\": \"top\", \"count\": 1 }");
    {
        std::ofstream out(dir / "ships.json");
        out << ships;
    }
    const auto lighter = fallsAt((dir / "archetypes.json").string());
    LoadRegistries();  // the stock data again, for whatever runs next

    CHECK(lighter.first < stock.first);  // a laser takes off less than a railgun
    REQUIRE(stock.second < 1.0e9);
    CHECK(lighter.second > stock.second);
}

TEST_CASE("a siege and what a faction cannot supply survive a restart (#295)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("siege_world.json");
    auto              a = World();
    const Scene       s = GuildOutpostNextToPirates(*a);
    const Plan        plan = *PiratesBesiege(*a, s);
    a->MindOf(Fac::Syndicate()).unsupplied.insert("W-1.1/+7");
    a->MindOf(Fac::Syndicate()).lostAt[s.frontier] = 123.0;
    a->SaveWorld(path);

    Simulation b;
    b.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    b.AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");

    REQUIRE(b.Plans().count(plan.id) == 1);
    const Plan back = b.Plans().at(plan.id);
    CHECK(back.kind == Plan::Kind::Siege);
    CHECK(back.against == Fac::TradersGuild());
    CHECK(back.site == plan.site);
    CHECK(back.from == plan.from);
    CHECK(back.force == doctest::Approx(SiegeBy(*a, Fac::Pirates())->force));
    CHECK(back.dueAt == doctest::Approx(plan.dueAt));
    CHECK(b.MindOf(Fac::Syndicate()).unsupplied.count("W-1.1/+7") == 1);
    CHECK(b.MindOf(Fac::Syndicate()).lostAt.at(s.frontier) == doctest::Approx(123.0));
    REQUIRE(back.lent.size() == plan.lent.size());
    CHECK(back.lent[0].from == plan.lent[0].from);
    CHECK(back.lent[0].force == doctest::Approx(plan.lent[0].force));
    CHECK(back.lent[0].hops == plan.lent[0].hops);
    CHECK(back.arrived);
    CHECK(back.arrivesAt == doctest::Approx(plan.arrivesAt));
    CHECK(back.via == plan.via);
}

// --- Pooled sieges (#295, the owner's decision of 2026-10-11) ---------------------------------

namespace
{
// A pirate holding with a pirate base in it and a full garrison, ringed for two jumps by
// holdings of the Traders Guild and, past them, by systems of the Guild's friends: a scene in
// which besieging the base is the one thing the Guild could do.
struct Ring
{
    std::string              den;
    std::vector<std::string> holdings;  // the Guild's, within two jumps of the den
};

Ring PirateBaseRingedByTheGuild(Simulation& sim)
{
    Ring r;
    for (const auto& kv : sim.Systems())
        if (r.den.empty() && kv.second.agg.claimed && kv.second.agg.controller == Fac::Pirates() &&
            sim.Neighbors(kv.first).size() >= 2)
            r.den = kv.first;
    REQUIRE_FALSE(r.den.empty());
    std::set<std::string> near;
    for (const std::string& n : sim.Neighbors(r.den))
    {
        near.insert(n);
        for (const std::string& m : sim.Neighbors(n))
            near.insert(m);
    }
    near.erase(r.den);
    for (auto& kv : sim.Systems())
    {
        if (kv.first == r.den)
            continue;
        SystemAggregate& a = kv.second.agg;
        a.claimed = true;
        a.controller = near.count(kv.first) != 0 ? Fac::TradersGuild() : Fac::Independent();
        if (near.count(kv.first) != 0)
            r.holdings.push_back(kv.first);
    }
    const Simulation::OutpostSpot spot = sim.FindOutpostSpot(
        Fac::Pirates(), Outposts::Purpose::Hidden, sim.Neighbors(r.den)[0], r.den);
    REQUIRE(spot.found);
    const Blueprint* bp = Blueprints::Find("outpost_hidden");
    REQUIRE(bp != nullptr);
    auto t = std::make_unique<Structure>(spot.at, 0.0f, "Pirate Base", bp->archetype);
    t->SetBlueprint(bp->id);
    REQUIRE(sim.AddStatic(r.den, std::move(t), Outposts::OwnerOf(Fac::Pirates())) != 0);
    return r;
}

// Every Guild holding at full strength, the den's garrison full, and the Guild with the stock
// for a siege but not for an outpost and fresh word of everything -- until it musters.
const Plan* GuildMusters(Simulation& sim, const Ring& r)
{
    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    for (int pass = 0; pass < 3 * PERIOD && SiegeBy(sim, Fac::TradersGuild()) == nullptr; pass++)
    {
        for (const std::string& h : r.holdings)
            sim.SystemById(h)->agg.presence[Fac::TradersGuild()] = cap;
        sim.SystemById(r.den)->agg.presence[Fac::Pirates()] =
            Factions::TemperamentOf(Fac::Pirates()).capacity;
        sim.MindOf(Fac::TradersGuild()).stock = Simulation::SiegeCost() * 1.2f;
        for (const auto& kv : sim.Systems())
            sim.MindOf(Fac::TradersGuild()).intel[kv.first] = sim.Observe(kv.second);
        Pass(sim);
    }
    const Plan* p = SiegeBy(sim, Fac::TradersGuild());
    REQUIRE(p != nullptr);
    return p;
}

std::vector<const Plan*> ReturnsOf(const Simulation& sim, FactionId f)
{
    std::vector<const Plan*> out;
    for (const auto& kv : sim.Plans())
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Return)
            out.push_back(&kv.second);
    return out;
}
}  // namespace

TEST_CASE("a siege is lent by the holdings within reach, nearest first (#295)")
{
    auto                     sim = World();
    const Ring               r = PirateBaseRingedByTheGuild(*sim);
    const float              cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    const Blueprints::Siege& rules = Blueprints::SiegeRules();
    for (const std::string& h : r.holdings)
        sim->SystemById(h)->agg.presence[Fac::TradersGuild()] = cap;

    std::string                   via;
    const std::vector<Plan::Lent> lent =
        sim->PoolFor(Fac::TradersGuild(), r.den, rules.pool * cap, &via);
    REQUIRE(lent.size() >= 2);  // one holding's surplus is not what it asked for
    float sum = 0.0f;
    for (size_t i = 0; i < lent.size(); i++)
    {
        sum += lent[i].force;
        CHECK(lent[i].force <= cap * rules.commit + 1e-4f);
        CHECK(lent[i].hops >= 1);
        CHECK(lent[i].hops <= (int)rules.reach);
        CHECK(std::find(r.holdings.begin(), r.holdings.end(), lent[i].from) != r.holdings.end());
        if (i > 0)
            CHECK(lent[i - 1].hops <= lent[i].hops);
    }
    CHECK(sum <= rules.pool * cap + 1e-3f);
    CHECK(lent[0].hops == 1);
    CHECK(via == lent[0].from);  // the nearest is next door, and comes in by its own gate

    // A holding with nothing over its home guard lends nothing; another faction's, nothing.
    for (const std::string& h : r.holdings)
        sim->SystemById(h)->agg.presence[Fac::TradersGuild()] = cap * 0.6f;
    CHECK(sim->PoolFor(Fac::TradersGuild(), r.den, rules.pool * cap).empty());
    CHECK(sim->PoolFor(Fac::Syndicate(), r.den, rules.pool * cap).empty());
}

TEST_CASE("a lawful faction pools its holdings to bring down a pirate base, and the force comes "
          "home (#295)")
{
    auto        sim = World();
    const Ring  r = PirateBaseRingedByTheGuild(*sim);
    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    const Plan  plan = *GuildMusters(*sim, r);

    CHECK(plan.against == Fac::Pirates());
    CHECK(plan.target == r.den);
    REQUIRE(plan.lent.size() >= 2);
    CHECK_FALSE(plan.arrived);
    int   hops = 0;
    float lent = 0.0f;
    for (const Plan::Lent& l : plan.lent)
    {
        hops = std::max(hops, l.hops);
        lent += l.force;
        // Each holding is short of what it lent, and stays so while it is away.
        CHECK(sim->LentFrom(l.from, Fac::TradersGuild()) == doctest::Approx(l.force));
    }
    CHECK(plan.force == doctest::Approx(lent));
    CHECK(plan.force > cap * Blueprints::SiegeRules().commit);  // more than one holding sends
    CHECK(plan.arrivesAt ==
          doctest::Approx(plan.startedAt + hops * Blueprints::SiegeRules().hop).epsilon(1e-6));
    CHECK(Told(*sim, "siege", r.den, "Traders Guild sent"));
    CHECK(sim->SiegeForceIn(r.den, Fac::TradersGuild()) == 0.0f);  // still on the way

    // A period on, the lenders have not made good what they lent.
    for (int pass = 0; pass < PERIOD; pass++)
        Pass(*sim);
    for (const Plan::Lent& l : plan.lent)
        CHECK(sim->SystemById(l.from)->agg.presence[Fac::TradersGuild()] <= cap - l.force + 0.05f);

    // It arrives when the farthest has, and the base falls -- where nobody is.
    RunUntil(*sim, plan.dueAt - sim->Time() + 10.0,
             [&]() { return OutpostIn(*sim, r.den) == nullptr; });
    CHECK(OutpostIn(*sim, r.den) == nullptr);
    CHECK(Told(*sim, "siege", r.den, "Traders Guild laid siege to the Pirates hidden base"));
    CHECK(Told(*sim, "siege", r.den, "Traders Guild destroyed the Pirates"));
    CHECK(SiegeBy(*sim, Fac::TradersGuild()) == nullptr);
    const SystemAggregate& den = sim->SystemById(r.den)->agg;
    const bool             pirates = den.claimed && den.controller == Fac::Pirates();
    CHECK_FALSE(pirates);

    // ...and what is left of the force goes home, each holding its share, minus the losses.
    const std::vector<const Plan*> returns = ReturnsOf(*sim, Fac::TradersGuild());
    REQUIRE_FALSE(returns.empty());
    float                        back = 0.0f;
    std::map<std::string, float> before, owed;
    for (const Plan* p : returns)
    {
        back += p->force;
        CHECK(p->from == r.den);
        owed[p->target] += p->force;
        before[p->target] = sim->SystemById(p->target)->agg.presence[Fac::TradersGuild()];
    }
    CHECK(back <= lent + 1e-3f);
    CHECK(back > 0.5f * lent);
    RunUntil(*sim, hops * Blueprints::SiegeRules().hop + 10.0,
             [&]() { return ReturnsOf(*sim, Fac::TradersGuild()).empty(); });
    CHECK(ReturnsOf(*sim, Fac::TradersGuild()).empty());
    for (const auto& kv : owed)
    {
        CHECK(sim->LentFrom(kv.first, Fac::TradersGuild()) == 0.0f);
        CHECK(sim->SystemById(kv.first)->agg.presence[Fac::TradersGuild()] >
              before[kv.first] + 0.5f * kv.second);
    }
}

TEST_CASE("a pooled siege falls the same whether anybody watches or not, and where somebody does "
          "its ships come in through the gate (#295)")
{
    double fell[2] = { -1.0, -1.0 };
    for (int hot = 0; hot < 2; hot++)
    {
        auto       sim = World(hot == 1);
        const Ring r = PirateBaseRingedByTheGuild(*sim);
        const Plan plan = *GuildMusters(*sim, r);
        RunUntil(*sim, plan.arrivesAt - sim->Time() + 3.0,
                 [&]()
                 {
                     const Plan* p = SiegeBy(*sim, Fac::TradersGuild());
                     return p == nullptr || p->arrived;
                 });
        REQUIRE(SiegeBy(*sim, Fac::TradersGuild()) != nullptr);
        REQUIRE(SiegeBy(*sim, Fac::TradersGuild())->arrived);
        if (hot == 1)
        {
            int warships = 0;
            for (const auto& e : sim->SystemById(r.den)->entities)
                if (e->GetKind() == EntityKind::Npc &&
                    static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Warship &&
                    static_cast<const NpcShip&>(*e).GetFaction() == Fac::TradersGuild())
                    warships++;
            CHECK(warships == std::min(8, (int)std::lround(plan.force)));
        }
        RunUntil(*sim, plan.dueAt - sim->Time() + 10.0,
                 [&]() { return OutpostIn(*sim, r.den) == nullptr; });
        if (OutpostIn(*sim, r.den) == nullptr)
            fell[hot] = sim->Time();
    }
    REQUIRE(fell[0] > 0.0);
    REQUIRE(fell[1] > 0.0);
    CHECK(std::fabs(fell[0] - fell[1]) <= 60.0);
}

TEST_CASE("a siege still on its way finds the outpost gone and turns back (#295)")
{
    auto       sim = World();
    const Ring r = PirateBaseRingedByTheGuild(*sim);
    const Plan plan = *GuildMusters(*sim, r);
    REQUIRE_FALSE(plan.arrived);
    Structure* base = OutpostIn(*sim, r.den);
    REQUIRE(base != nullptr);
    CHECK(sim->DamageStructure(*sim->SystemById(r.den), base->GetId(), 1.0e9f, nullptr));
    RunUntil(*sim, plan.arrivesAt - sim->Time() + 3.0,
             [&]() { return SiegeBy(*sim, Fac::TradersGuild()) == nullptr; });
    CHECK(SiegeBy(*sim, Fac::TradersGuild()) == nullptr);
    CHECK(Told(*sim, "siege", r.den, "turned back"));
    float back = 0.0f;
    for (const Plan* p : ReturnsOf(*sim, Fac::TradersGuild()))
        back += p->force;
    CHECK(back == doctest::Approx(plan.force));  // nobody fought: everybody goes home
}

TEST_CASE("the besieging side posts join-the-siege jobs: defenders shot down pay, and are gone "
          "from the garrison the siege wears against (#407)")
{
    auto       sim = World();
    const Ring r = PirateBaseRingedByTheGuild(*sim);
    const int  plan = GuildMusters(*sim, r)->id;

    // A Guild depot next door to the den posts the job.
    const std::string next = sim->Neighbors(r.den)[0];
    REQUIRE(sim->SystemById(next)->agg.controller == Fac::TradersGuild());
    const Vector2 depot = sim->SafeArrival(next);
    const int     station = sim->AddStatic(
        next,
        std::make_unique<Station>(Vector2{ depot.x + 9000.0f, depot.y + 9000.0f }, 300.0f,
                                  "Guild Depot", Fac::TradersGuild(), StationRole::TradeHub),
        std::string());
    REQUIRE(station != 0);
    sim->MindOf(Fac::TradersGuild()).stock = 4.0f * Simulation::OutpostCost();
    ClientSession& s = sim->CreateSession(next, { depot.x + 9350.0f, depot.y + 9000.0f },
                                          GetShipCatalog()[0].stats);
    REQUIRE(sim->StepPlayerDock(s, *sim->SystemById(next)) == station);
    int offer = -1;
    for (size_t i = 0; i < s.missions.Offers().size(); i++)
        if (s.missions.Offers()[i].type == MissionType::Besiege &&
            s.missions.Offers()[i].planId == plan)
            offer = (int)i;
    REQUIRE(offer >= 0);
    const Mission job = s.missions.Offers()[(size_t)offer];
    // It says which side it is for -- the issuer's, the besieger -- and whom it is against.
    CHECK(job.faction == Fac::TradersGuild());
    CHECK(job.against == Fac::Pirates());
    CHECK(job.systemId == r.den);
    CHECK(job.targetCount >= 1);
    CHECK(job.title.find("Join the siege") != std::string::npos);
    // ...and the besieged's board, had it one, would carry the other side's: never this one.
    for (const Mission& m : s.missions.Offers())
        CHECK(m.type != MissionType::Siege);
    REQUIRE(sim->AcceptMission(s, s.missions.Offers()[(size_t)offer].offerId));

    // Into the den: its pirates are ships there now.
    sim->StepPlayerUndock(s);
    sim->ServerEnterSystem(s, r.den, next);
    Pass(*sim);
    const float before = sim->SystemById(r.den)->agg.presence[Fac::Pirates()];
    // A ship of the besieger's own counts for nothing; each defender shot down counts one.
    NpcShip own({ 0.0f, 0.0f }, Fac::TradersGuild(), NpcRole::Warship, {});
    sim->CreditJobKill(s, own);
    CHECK(s.missions.Active()[0].progress == 0);
    const int need = s.missions.Active()[0].targetCount;
    int       shot = 0;
    for (const auto& e : sim->SystemById(r.den)->entities)
    {
        if (shot >= need || e->GetKind() != EntityKind::Npc)
            continue;
        NpcShip& n = static_cast<NpcShip&>(*e);
        if (n.GetFaction() != Fac::Pirates() || n.GetRole() != NpcRole::Pirate || n.GetTask() != 0)
            continue;
        n.TakeDamage(1.0e9f);
        sim->CreditJobKill(s, n);
        shot++;
    }
    REQUIRE(shot == need);
    CHECK(s.missions.Active()[0].progress == need);
    // What the recount finds gone is gone from the pirates' strength there.
    Pass(*sim);
    CHECK(sim->SystemById(r.den)->agg.presence[Fac::Pirates()] < before - 0.5f * (float)need);

    REQUIRE(sim->MissionCompletableNow(s, s.missions.Active()[0]));
    const double money = s.account.GetMoney();
    const double reward = s.missions.Active()[0].rewardMoney;
    REQUIRE(sim->CompleteMission(s, 0));
    CHECK(s.account.GetMoney() == doctest::Approx(money + reward));
}

TEST_CASE("an outpost mends at its tier's rate, the same where nobody is as where somebody is "
          "(#431 slice 7)")
{
    float left[2] = { -1.0f, -1.0f };
    for (int hot = 0; hot < 2; hot++)
    {
        auto        sim = World(hot == 1);
        const Scene s = GuildOutpostNextToPirates(*sim);
        REQUIRE(Simulation::IsHot(*sim->SystemById(s.frontier)) == (hot == 1));
        Structure*  t = OutpostIn(*sim, s.frontier);
        const float hull = t->MaxHull(sim->Time());
        t->SetDamage(hull * 0.6f, sim->Time());
        const double from = sim->Time();
        for (int pass = 0; pass < 10; pass++)
        {
            sim->MindOf(Fac::TradersGuild()).stock = 10.0f;
            Pass(*sim);
        }
        t = OutpostIn(*sim, s.frontier);
        REQUIRE(t != nullptr);
        left[hot] = t->GetDamage(sim->Time());
        CHECK(left[hot] ==
              doctest::Approx(hull * 0.6f - t->MendRate() * (sim->Time() - from)).epsilon(0.001));
        CHECK(left[hot] < hull * 0.6f);
    }
    CHECK(left[0] == doctest::Approx(left[1]));
}

// --- The front line (#431 D3, slice 8) ------------------------------------------------------

TEST_CASE("a core with only its holder's neighbours is not besieged; once a border falls, it is "
          "(#431 D3)")
{
    auto sim = World();
    // A Guild system `core` with a Guild outpost, every system the Guild's but one: the
    // pirates' `den`, two jumps away through `border`.
    std::string core, border, den;
    for (const auto& kv : sim->Systems())
    {
        const auto& near = sim->Neighbors(kv.first);
        for (const std::string& b : near)
            for (const std::string& p : sim->Neighbors(b))
                if (den.empty() && p != kv.first &&
                    std::find(near.begin(), near.end(), p) == near.end())
                {
                    core = kv.first;
                    border = b;
                    den = p;
                }
        if (!den.empty())
            break;
    }
    REQUIRE_FALSE(den.empty());
    for (auto& kv : sim->Systems())
    {
        SystemAggregate& a = kv.second.agg;
        a.claimed = true;
        a.controller = kv.first == den ? Fac::Pirates() : Fac::TradersGuild();
    }
    const Simulation::OutpostSpot spot =
        sim->FindOutpostSpot(Fac::TradersGuild(), Outposts::Purpose::Open, border, core);
    REQUIRE(spot.found);
    const Blueprint* bp = Blueprints::Find(Outposts::BLUEPRINT);
    auto t = std::make_unique<Structure>(spot.at, 0.0f, "Traders Guild Outpost", bp->archetype);
    t->SetBlueprint(bp->id);
    REQUIRE(sim->AddStatic(core, std::move(t), Outposts::OwnerOf(Fac::TradersGuild())) != 0);

    const float cap = Factions::TemperamentOf(Fac::Pirates()).capacity;
    const float guild = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    // The pirates with strength and stock to spare, the core's garrison thin, and the pirates
    // knowing it -- for `passes`, or until they lay siege.
    auto tempt = [&](int passes)
    {
        for (int pass = 0; pass < passes && SiegeBy(*sim, Fac::Pirates()) == nullptr; pass++)
        {
            for (auto& kv : sim->Systems())
            {
                const bool theirs =
                    kv.second.agg.claimed && kv.second.agg.controller == Fac::Pirates();
                kv.second.agg.presence[Fac::Pirates()] = theirs ? cap : 0.0f;
                kv.second.agg.presence[Fac::TradersGuild()] = kv.first == core ? 1.0f
                                                              : theirs         ? 0.0f
                                                                               : guild;
                if (kv.first != core)
                    sim->MindOf(Fac::Pirates()).intel.erase(kv.first);
            }
            sim->MindOf(Fac::Pirates()).stock = Simulation::OutpostCost() * 2.0f;
            sim->MindOf(Fac::TradersGuild()).stock = 1.0f;  // its upkeep, not a siege
            sim->MindOf(Fac::Pirates()).intel[core] = sim->Observe(*sim->SystemById(core));
            Pass(*sim);
        }
    };

    // Behind the line: the core borders only the Guild's systems.
    CHECK_FALSE(sim->FrontOpen(core, Fac::Pirates()));
    tempt(3 * PERIOD);
    CHECK(SiegeBy(*sim, Fac::Pirates()) == nullptr);
    CHECK(sim->SystemById(core)->agg.controller == Fac::TradersGuild());

    // The border falls to the pirates, and the core is on the front line.
    sim->SystemById(border)->agg.controller = Fac::Pirates();
    CHECK(sim->FrontOpen(core, Fac::Pirates()));
    tempt(3 * PERIOD);
    const Plan* p = SiegeBy(*sim, Fac::Pirates());
    REQUIRE(p != nullptr);
    CHECK(p->target == core);
    CHECK(p->against == Fac::TradersGuild());
}
