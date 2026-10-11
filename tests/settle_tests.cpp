#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// Settling (#295, slice 3): a faction's holdings yield a stock, the stock pays for an outpost
// in a system nobody holds, the outpost is a structure everyone there can see while it goes
// up, and the finished outpost -- not strength alone -- is the claim.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

// A galaxy as the host makes one: the hand-written systems and a region from `seed`.
std::unique_ptr<Simulation> World(uint64_t seed = 7)
{
    SetRandomSeed(0xC0FFEEu);
    LoadRegistries();
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(DATA + "universe.json");
    sim->AttachRegion(seed, DATA + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(DATA + "systems/");
    return sim;
}

void Pass(Simulation& sim, int passes = 1)
{
    for (int i = 0; i < passes; i++)
        sim.MaintainWorld(2.0f);
}

bool Holds(const Simulation& sim, FactionId f, const std::string& id)
{
    const SystemState* st = sim.SystemById(id);
    return st != nullptr && st->agg.claimed && st->agg.controller == f;
}

// A system nobody holds that a gate leads to from one the Traders Guild holds.
std::string GuildFrontier(const Simulation& sim)
{
    for (const auto& kv : sim.Systems())
        if (Holds(sim, Fac::TradersGuild(), kv.first))
            for (const std::string& n : sim.Neighbors(kv.first))
                if (!sim.SystemById(n)->agg.claimed)
                    return n;
    return std::string();
}

const Structure* OutpostIn(const Simulation& sim, const std::string& system)
{
    for (const auto& e : sim.SystemById(system)->entities)
        if (e->GetKind() == EntityKind::Structure &&
            Outposts::IsOutpost(static_cast<const Structure&>(*e).GetBuilds()))
            return static_cast<const Structure*>(e.get());
    return nullptr;
}

// "Traders Guild began a watch post in W-3.2": whoever, then whatever it is for, then where.
bool Says(const std::string& text, const std::string& who, const std::string& verb)
{
    return text.rfind(who + " " + verb + " ", 0) == 0 && text.find(" in ") != std::string::npos;
}

const Plan* SettlementOf(const Simulation& sim, FactionId f)
{
    for (const auto& kv : sim.Plans())
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Settle)
            return &kv.second;
    return nullptr;
}

// The Guild has come to its frontier in strength and has the stock for an outpost; within a
// period it lays one down. Returns the frontier.
std::string GuildSettles(Simulation& sim)
{
    const std::string frontier = GuildFrontier(sim);
    REQUIRE_FALSE(frontier.empty());
    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    for (int pass = 0; pass < 30 && SettlementOf(sim, Fac::TradersGuild()) == nullptr; pass++)
    {
        sim.SystemById(frontier)->agg.presence[Fac::TradersGuild()] = cap;
        sim.MindOf(Fac::TradersGuild()).stock = Simulation::OutpostCost();
        Pass(sim);
    }
    const Plan* p = SettlementOf(sim, Fac::TradersGuild());
    REQUIRE(p != nullptr);
    REQUIRE(p->target == frontier);
    return frontier;
}
}  // namespace

TEST_CASE("an outpost is built by factions; no player is offered one (#295)")
{
    LoadRegistries();
    const Blueprint* bp = Blueprints::Find(Outposts::BLUEPRINT);
    REQUIRE(bp != nullptr);
    CHECK(bp->byFactions);
    CHECK_FALSE(bp->byPlayers);
    CHECK(Simulation::OutpostCost() > 0.0f);
    for (const Blueprint& b : Blueprints::All())
        CHECK(b.id != Outposts::BLUEPRINT);
    CHECK(Blueprints::Find("beacon") != nullptr);
    CHECK(Blueprints::Find("beacon")->byPlayers);

    // Asked anyway, the server says no, and why.
    Simulation sim;
    sim.LoadUniverse(DATA + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
    ClientSession& s = sim.CreateSession(sim.Universe().startId, Vector2{ 300000.0f, 300000.0f },
                                         GetShipCatalog()[0].stats);
    s.accountName = "hunter";
    const int seq = s.LastEventSeq();
    CHECK(sim.Deploy(s, Outposts::BLUEPRINT, s.ship->GetPosition(), "") == 0);
    bool told = false;
    for (const Ev::Event& e : s.EventsSince(seq))
        told = told || e.text.find("only a faction") != std::string::npos;
    CHECK(told);

    // The owner a faction's structure carries reads back as the faction, and an account's
    // does not.
    FactionId f = Fac::Independent();
    CHECK(Outposts::FactionOf(Outposts::OwnerOf(Fac::Syndicate()), f));
    CHECK(f == Fac::Syndicate());
    CHECK_FALSE(Outposts::FactionOf("hunter", f));
    CHECK_FALSE(Outposts::FactionOf("faction:Nobody", f));
}

TEST_CASE("holdings yield a stock, and what cannot be stored is not income (#295)")
{
    auto sim = World();
    Pass(*sim, 30 * 3);  // three periods
    const float cost = Simulation::OutpostCost();
    CHECK(sim->MindOf(Fac::TradersGuild()).stock > 0.0f);
    CHECK(sim->MindOf(Fac::Pirates()).stock > 0.0f);
    for (FactionId fi : Factions::All())
        CHECK(sim->MindOf(fi).stock <= 2.0f * cost + 1e-3f);
}

TEST_CASE("strength alone does not claim a system nobody held; a finished outpost does (#295)")
{
    auto              sim = World();
    const std::string frontier = GuildFrontier(*sim);
    REQUIRE_FALSE(frontier.empty());
    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    auto        hold = [&](int passes)
    {
        for (int i = 0; i < passes; i++)
        {
            sim->SystemById(frontier)->agg.presence[Fac::TradersGuild()] = cap * 2.0f;
            for (FactionId fi : Factions::All())
                sim->MindOf(fi).stock = 0.0f;  // nobody has anything to build with
            Pass(*sim);
        }
    };
    hold(Simulation::ContestPasses() * 2);
    CHECK_FALSE(sim->SystemById(frontier)->agg.claimed);

    // A site is not an outpost yet.
    auto site = std::make_unique<Structure>(Vector2{ 500000.0f, 500000.0f }, 0.0f, "Site",
                                            Blueprints::Find(Outposts::BLUEPRINT)->archetype);
    site->StartBuilding(sim->Time(), sim->Time() + 1.0e6);
    const int siteId =
        sim->AddStatic(frontier, std::move(site), Outposts::OwnerOf(Fac::TradersGuild()));
    REQUIRE(siteId != 0);
    hold(Simulation::ContestPasses() + 5);
    CHECK_FALSE(sim->SystemById(frontier)->agg.claimed);

    // A finished one, with the strength to hold it, is a claim.
    REQUIRE(sim->RemoveStatic(frontier, siteId));
    REQUIRE(sim->AddStatic(
                frontier,
                std::make_unique<Structure>(Vector2{ 500000.0f, 500000.0f }, 0.0f, "Outpost",
                                            Blueprints::Find(Outposts::BLUEPRINT)->archetype),
                Outposts::OwnerOf(Fac::TradersGuild())) != 0);
    hold(Simulation::ContestPasses() + 5);
    CHECK(Holds(*sim, Fac::TradersGuild(), frontier));
}

TEST_CASE("a faction settles where it has come, everyone sees the site, and finished it is a "
          "claim (#295)")
{
    auto              sim = World();
    const float       cost = Simulation::OutpostCost();
    const std::string frontier = GuildSettles(*sim);
    const Plan        plan = *SettlementOf(*sim, Fac::TradersGuild());

    // Paid for, and standing there as a site anyone entering the system is sent.
    CHECK(sim->MindOf(Fac::TradersGuild()).stock < cost);
    const Structure* site = OutpostIn(*sim, frontier);
    REQUIRE(site != nullptr);
    CHECK(site->IsBuilding());
    CHECK(site->GetOwner() == Outposts::OwnerOf(Fac::TradersGuild()));
    CHECK(site->GetCompletesAt() == doctest::Approx(plan.dueAt));
    CHECK(sim->StaticKey(frontier, site->GetId()) == plan.site);
    bool sent = false;
    for (const Proto::EntityLayout& e : sim->BuildLayout(frontier).entities)
        if (e.id == site->GetId())
        {
            sent = true;
            CHECK(e.kind == EntityKind::Structure);
            CHECK(e.owner == Outposts::OwnerOf(Fac::TradersGuild()));
            CHECK(e.completesAt == doctest::Approx(plan.dueAt));
        }
    CHECK(sent);
    CHECK_FALSE(sim->SystemById(frontier)->agg.claimed);
    CHECK(Says(sim->Chronicle().back().text, "Traders Guild", "began"));

    // One at a time: no second settlement while the first goes up, whatever the stock.
    for (int pass = 0; pass < 60; pass++)
    {
        sim->MindOf(Fac::TradersGuild()).stock = cost * 2.0f;
        Pass(*sim);
        int settling = 0;
        for (const auto& kv : sim->Plans())
            settling +=
                kv.second.kind == Plan::Kind::Settle && kv.second.faction == Fac::TradersGuild()
                    ? 1
                    : 0;
        CHECK(settling <= 1);
    }

    // Finished on its tick, and the claim on the next pass.
    while (sim->Time() < plan.dueAt + 4.0)
        Pass(*sim);
    REQUIRE(OutpostIn(*sim, frontier) != nullptr);
    CHECK_FALSE(OutpostIn(*sim, frontier)->IsBuilding());
    CHECK(Holds(*sim, Fac::TradersGuild(), frontier));
    bool founded = false, news = false;
    for (const ChronicleEntry& e : sim->Chronicle())
        if (e.kind == "settle" && e.system == frontier && Says(e.text, "Traders Guild", "founded"))
        {
            founded = true;
            CHECK(e.faction == std::optional<FactionId>(Fac::TradersGuild()));
            CHECK(e.time >= plan.dueAt);
        }
    for (const std::string& line : sim->Events())
        news = news || Says(line, "Traders Guild", "founded");
    CHECK(founded);
    CHECK(news);  // in the feed every client shows
}

TEST_CASE("a settlement whose site is lost claims nothing (#295)")
{
    auto              sim = World();
    const std::string frontier = GuildSettles(*sim);
    const Plan        plan = *SettlementOf(*sim, Fac::TradersGuild());
    REQUIRE(sim->RemoveStatic(frontier, OutpostIn(*sim, frontier)->GetId()));
    while (sim->Time() < plan.dueAt + 4.0)
        Pass(*sim);
    CHECK(SettlementOf(*sim, Fac::TradersGuild()) == nullptr);
    CHECK_FALSE(sim->SystemById(frontier)->agg.claimed);
    bool lost = false;
    for (const ChronicleEntry& e : sim->Chronicle())
        lost =
            lost || (e.system == frontier && e.text.find("lost the outpost") != std::string::npos);
    CHECK(lost);
}

TEST_CASE("a settlement and the stock survive a restart, and the outpost finishes on time (#295)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("settle_world.json");
    auto              a = World();
    const std::string frontier = GuildSettles(*a);
    const Plan        plan = *SettlementOf(*a, Fac::TradersGuild());
    const float       stock = a->MindOf(Fac::TradersGuild()).stock;
    a->SaveWorld(path);

    Simulation b;
    b.LoadUniverse(DATA + "universe.json");
    b.AttachRegion(7, DATA + "systems/");
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");

    REQUIRE(b.Plans().count(plan.id) == 1);
    const Plan back = b.Plans().at(plan.id);
    CHECK(back.kind == Plan::Kind::Settle);
    CHECK(back.site == plan.site);
    CHECK(back.dueAt == doctest::Approx(plan.dueAt));
    CHECK(b.MindOf(Fac::TradersGuild()).stock == doctest::Approx(stock));
    const Structure* site = OutpostIn(b, frontier);
    REQUIRE(site != nullptr);
    CHECK(site->IsBuilding());
    CHECK(site->GetOwner() == Outposts::OwnerOf(Fac::TradersGuild()));
    CHECK(b.StaticKey(frontier, site->GetId()) == plan.site);

    // The structure step's queue was remade from the site's time line.
    b.SetTime(plan.dueAt - 0.5);
    b.StepStructures();
    CHECK(OutpostIn(b, frontier)->IsBuilding());
    b.SetTime(plan.dueAt);
    b.StepStructures();
    CHECK_FALSE(OutpostIn(b, frontier)->IsBuilding());
    Pass(b, 10);  // a few plans resolve per pass, and the surveys came due as well
    CHECK(Holds(b, Fac::TradersGuild(), frontier));
}

TEST_CASE("a world saved before factions had a stock loads, and they start from nothing (#295)")
{
    LoadRegistries();
    const TempDir     tmp;
    const std::string path = tmp.Path("settle_old_world.json");
    {
        Simulation probe;
        probe.LoadUniverse(DATA + "universe.json");
        const std::string start = probe.Universe().startId;
        std::ofstream     out(path);
        out << R"({ "version": 5, "simTime": 600.0, "galaxy": {},
                    "factions": { "Syndicate": { "intel": {} } },
                    "plans": [ { "id": 3, "kind": "survey", "faction": "Syndicate",
                                 "from": ")"
            << start << R"(", "target": ")" << start
            << R"(", "startedAt": 590.0, "dueAt": 700.0 } ],
                    "nextPlan": 4 })";
    }
    Simulation sim;
    sim.LoadUniverse(DATA + "universe.json");
    REQUIRE(sim.LoadWorld(path) == Save::Result::Ok);
    sim.MaterializeAllSystems(DATA + "systems/");
    for (FactionId fi : Factions::All())
        CHECK(sim.MindOf(fi).stock == 0.0f);
    REQUIRE(sim.Plans().count(3) == 1);
    CHECK(sim.Plans().at(3).kind == Plan::Kind::Survey);

    sim.SaveWorld(path);
    std::string text;
    {
        std::ifstream in(path);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(Save::WORLD_VERSION >= 6);
    CHECK(text.find("\"version\": " + std::to_string(Save::WORLD_VERSION)) != std::string::npos);
    CHECK(text.find("\"stock\"") != std::string::npos);
}
