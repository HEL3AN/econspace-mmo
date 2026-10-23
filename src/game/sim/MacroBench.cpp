// `econserver macrobench`: what the galaxy costs to run, by its size (#295).
//
// Builds a world of one centre and its frontier, plus a region of N systems through a
// wormhole from the centre, runs the host's world
// step headless for a fixed stretch of simulated time, and says where the time went. Nothing
// here changes a rule; it calls what the host calls, in the order the host calls it
// (HostStepWorld), with a stopwatch around each part. Nobody is connected unless asked, and
// then each player stands in a system of their own; --hot keeps every system hot, which is
// what the world cost before cold systems (#295).

#include "sim/MacroBench.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/DataDir.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "economy/Resource.h"
#include "entities/Combatant.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ProcessMemory.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

double Since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct BenchRow
{
    int    region = 0, systems = 0, links = 0;
    int    players = 0, hot = 0;   // sessions, and systems hot at the end of the run
    long   statics = 0, npcs = 0;  // entities at the end of the run
    double generateMs = 0.0;       // AttachRegion: the generator and the pins
    double materializeMs = 0.0;    // InitGalaxy + MaterializeAllSystems
    int    ticks = 0, passes = 0;
    double agentsMs = 0.0;      // StepSystemAgents in every system, per tick
    double structuresMs = 0.0;  // StepStructures, per tick
    double recountMs = 0.0;     // per coarse pass
    double macroMs = 0.0;       // StepWorldMacro, per coarse pass
    double topUpMs = 0.0;       // the spawn director, per coarse pass
    double tickMs = 0.0, worstTickMs = 0.0;
    double routeKnownMs = 0.0;  // one route order's plan: the pilot's index and the search (#432)
    double routeAllMs = 0.0;    // the search alone, over every system: a galaxy fully charted
    double saveMs = 0.0;
    long   saveBytes = 0;
    double memoryMb = -1.0;  // resident growth from before the world was built
};

BenchRow RunOne(int regionSystems, double seconds, uint64_t seed, bool allHot, int players)
{
    const std::string dataDir = DataDir::Path();
    BenchRow          row;
    row.region = regionSystems;
    const double memBefore = ResidentMegabytes();

    // As SetupHostSim does it, with the region's size the one thing chosen here.
    SetRandomSeed(0xC0FFEEu);
    Simulation        sim;
    Clock::time_point t = Clock::now();
    // One civilisation centre and the frontier beside it (#342 S2), then a region of the size
    // asked for, opened from the centre's heart into the next free slot beside it.
    Gen::WorldParams world;
    world.seed = seed;
    world.centres = 1;
    world.owners = Factions::MajorIds();
    world.pirates = Factions::Id(Factions::Pirates());
    world.unaffiliated = Factions::Id(Factions::Unaffiliated());
    sim.OpenGalaxy(world, dataDir);
    const int centre = sim.Skeleton().centres.front();
    for (int n : sim.Skeleton().slots[(size_t)centre].neighbours)
        if (sim.OpenSlots().count(n) == 0)
        {
            sim.AttachRegion(seed, n, sim.Starts().front().systemId, dataDir + "systems/",
                             regionSystems);
            break;
        }
    row.generateMs = Since(t);
    sim.Seed(0xC0FFEEu);
    sim.SetAllHot(allHot);
    t = Clock::now();
    sim.InitGalaxy();
    sim.MaterializeAllSystems(dataDir + "systems/");
    row.materializeMs = Since(t);
    row.systems = (int)sim.Systems().size();
    row.links = (int)sim.Universe().links.size();

    // The players, if any: one to a system, spread evenly through the galaxy, each where a
    // ship arrives without a gate.
    std::vector<std::string> ids;
    for (const auto& kv : sim.Systems())
        ids.push_back(kv.first);
    row.players = std::min(players, (int)ids.size());
    for (int i = 0; i < row.players; i++)
    {
        const std::string& at = ids[(size_t)i * ids.size() / (size_t)row.players];
        sim.CreateSession(at, sim.SafeArrival(at), GetShipCatalog()[0].stats);
    }

    // HostStepWorld: the agents of every hot system, then maintenance (markets are in its pass).
    const float              dt = 1.0f / 60.0f;
    Simulation::MaintainCost cost;
    double                   agents = 0.0, total = 0.0;
    row.ticks = (int)(seconds * 60.0 + 0.5);
    for (int i = 0; i < row.ticks; i++)
    {
        const Clock::time_point tick = Clock::now();
        t = tick;
        std::map<std::string, std::vector<Simulation::PlayerPresence>> present;
        for (auto& kv : sim.Sessions())
            present[kv.second.systemId].push_back(
                { &kv.second, (Combatant*)kv.second.ship.get(), false });
        for (auto& kv : sim.Systems())
            if (Simulation::IsHot(kv.second))
                sim.StepSystemAgents(kv.second, present[kv.first], nullptr, dt);
        agents += Since(t);
        sim.MaintainWorld(dt, &cost);
        sim.TakeLayoutDeltas();  // the host sends these every frame; nobody is here to read them
        const double ms = Since(tick);
        total += ms;
        row.worstTickMs = std::max(row.worstTickMs, ms);
    }
    row.passes = cost.passes;
    row.agentsMs = agents / row.ticks;
    row.structuresMs = cost.structures * 1000.0 / row.ticks;
    const int passes = std::max(1, cost.passes);
    row.recountMs = cost.recount * 1000.0 / passes;
    row.macroMs = cost.macro * 1000.0 / passes;
    row.topUpMs = cost.topUp * 1000.0 / passes;
    row.tickMs = total / row.ticks;

    for (const auto& kv : sim.Systems())
    {
        row.hot += Simulation::IsHot(kv.second) ? 1 : 0;
        for (const auto& e : kv.second.entities)
            (e->GetKind() == EntityKind::Npc ? row.npcs : row.statics)++;
    }

    // A route order's plan (#432), which it makes on arrival in a system and once a second on
    // the way: the pilot's index, then the search through it -- and the search over the whole
    // graph, which is what it costs once everything is charted. End to end of the galaxy.
    {
        const int          reps = 20;
        const std::string& from = ids.front();
        const std::string& to = ids.back();
        ClientSession&     pilot =
            sim.CreateSession(from, sim.SafeArrival(from), GetShipCatalog()[0].stats);
        Orders::Order order;
        order.kind = Orders::Kind::Route;
        order.destSystem = to;
        sim.GiveOrder(pilot, order);
        t = Clock::now();
        for (int i = 0; i < reps; i++)
            (void)sim.RouteFor(pilot);
        row.routeKnownMs = Since(t) / reps;
        t = Clock::now();
        for (int i = 0; i < reps; i++)
            (void)sim.PlanRoute(from, to);
        row.routeAllMs = Since(t) / reps;
    }

    // The checkpoint the host writes once a minute, into a file of the benchmark's own.
    const std::string path = std::string(GetApplicationDirectory()) + "macrobench_world.json";
    const int         saves = 3;
    t = Clock::now();
    for (int i = 0; i < saves; i++)
        sim.SaveWorld(path);
    row.saveMs = Since(t) / saves;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        row.saveBytes = in.is_open() ? (long)in.tellg() : 0;
    }
    std::remove(path.c_str());

    const double memAfter = ResidentMegabytes();
    if (memBefore >= 0.0 && memAfter >= 0.0)
        row.memoryMb = memAfter - memBefore;
    return row;
}
}  // namespace

int MacroBench(const std::vector<int>& regionSizes, double seconds, uint64_t seed, bool allHot,
               int players)
{
    SetTraceLogLevel(LOG_WARNING);
    const std::string dataDir = DataDir::Path();
    if (!Factions::Load(dataDir + "factions.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Factions::Error().c_str());
        return 1;
    }
    if (!Goods::Load(dataDir + "goods.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Goods::Error().c_str());
        return 1;
    }
    if (!Archetypes::Load(dataDir + "archetypes.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Archetypes::Error().c_str());
        return 1;
    }
    if (!Blueprints::Load(dataDir + "blueprints.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Blueprints::Error().c_str());
        return 1;
    }

    printf("macrobench: seed %llu, %.0f s simulated per run, %d player(s) connected, %s\n",
           (unsigned long long)seed, seconds, players,
           allHot ? "every system hot" : "systems hot only where a player is");
    std::vector<BenchRow> rows;
    for (int n : regionSizes)
    {
        printf("  region of %d ...", n);
        rows.push_back(RunOne(n, seconds, seed, allHot, players));
        const BenchRow& r = rows.back();
        printf(" %d systems, %.2f ms/tick (worst %.1f)\n", r.systems, r.tickMs, r.worstTickMs);
    }

    // Markdown, so the table can be pasted where it is discussed.
    printf("\nPer tick (60 Hz, budget 16.7 ms) and per coarse pass (every 2 s):\n\n");
    printf(
        "| region | systems | links | players | hot | static | NPCs | generate ms | "
        "materialise ms | "
        "agents ms/tick | structures ms/tick | recount ms/pass | "
        "macro ms/pass | top-up ms/pass | tick ms (mean / worst) | route plan ms (known / all) | "
        "save ms | save KB | memory MB |\n");
    printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
           "---:|---:|---:|\n");
    for (const BenchRow& r : rows)
    {
        char mem[32];
        if (r.memoryMb >= 0.0)
            snprintf(mem, sizeof(mem), "%.0f", r.memoryMb);
        else
            snprintf(mem, sizeof(mem), "n/a");
        printf("| %d | %d | %d | %d | %d | %ld | %ld | %.1f | %.1f | %.3f | %.4f | %.3f | "
               "%.3f | %.3f | %.2f / %.1f | %.2f / %.2f | %.2f | %.0f | %s |\n",
               r.region, r.systems, r.links, r.players, r.hot, r.statics, r.npcs, r.generateMs,
               r.materializeMs, r.agentsMs, r.structuresMs, r.recountMs, r.macroMs, r.topUpMs,
               r.tickMs, r.worstTickMs, r.routeKnownMs, r.routeAllMs, r.saveMs,
               r.saveBytes / 1024.0, mem);
    }
    return 0;
}

int FleetBench(const std::vector<uint64_t>& seeds, double days, int regionSystems,
               double everyHours, double shockHours)
{
    SetTraceLogLevel(LOG_WARNING);
    const std::string dataDir = DataDir::Path();
    if (!Factions::Load(dataDir + "factions.json") || !Goods::Load(dataDir + "goods.json") ||
        !Archetypes::Load(dataDir + "archetypes.json") ||
        !Blueprints::Load(dataDir + "blueprints.json") ||
        !WorldRules::Load(dataDir + "world_rules.json"))
    {
        fprintf(stderr, "FATAL: the data did not load\n");
        return 1;
    }
    bool collapsed = false;
    for (uint64_t seed : seeds)
    {
        SetRandomSeed(0xC0FFEEu);
        Simulation       sim;
        Gen::WorldParams world;
        world.seed = seed;
        world.centres = 1;
        world.owners = Factions::MajorIds();
        world.pirates = Factions::Id(Factions::Pirates());
        world.unaffiliated = Factions::Id(Factions::Unaffiliated());
        sim.OpenGalaxy(world, dataDir);
        const int centre = sim.Skeleton().centres.front();
        for (int n : sim.Skeleton().slots[(size_t)centre].neighbours)
            if (sim.OpenSlots().count(n) == 0)
            {
                sim.AttachRegion(seed, n, sim.Starts().front().systemId, dataDir + "systems/",
                                 regionSystems);
                break;
            }
        sim.Seed(0xC0FFEEu);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(dataDir + "systems/");

        printf("\nfleetbench: seed %llu, %d systems, %.1f days\n\n", (unsigned long long)seed,
               (int)sim.Systems().size(), days);
        printf("| hour | faction | ships | holdings | no market near | keep | treasury |\n"
               "|---:|---|---:|---:|---:|---:|---:|\n");
        const int passes = (int)(days * 86400.0 / 2.0);
        const int every = std::max(1, (int)(everyHours * 1800.0));
        const int shock = shockHours > 0.0 ? (int)(shockHours * 1800.0) : -1;
        for (int pass = 0; pass <= passes; pass++)
        {
            if (pass > 0)
                sim.MaintainWorld(2.0f);
            // A war everywhere at once: every holding left with a third of its ships.
            if (pass == shock)
            {
                for (auto& kv : sim.Systems())
                    if (kv.second.agg.claimed)
                        kv.second.agg.presence[kv.second.agg.controller] *= 0.3f;
                printf("| %.0f | (every holding loses 70%% of its ships) | | | | | |\n",
                       pass / 1800.0);
            }
            if (pass % every != 0)
                continue;
            for (FactionId f : Factions::All())
            {
                float ships = 0.0f;
                int   held = 0, far = 0;
                for (const auto& kv : sim.Systems())
                {
                    ships += kv.second.agg.presence[f];
                    if (kv.second.agg.claimed && kv.second.agg.controller == f)
                    {
                        held++;
                        far += sim.BuyingMarket(kv.first) == nullptr ? 1 : 0;
                    }
                }
                if (held == 0)
                    continue;
                const float keep = Factions::TemperamentOf(f).capacity * (float)held;
                printf("| %.0f | %s | %.1f | %d | %d | %.0f | %.0f |\n", pass / 1800.0,
                       Factions::Id(f).c_str(), ships, held, far, keep, sim.MindOf(f).treasury);
                // Holding systems with no ships in them: emptied, not beaten.
                if (pass > 0 && ships < 0.05f * keep)
                {
                    printf("  COLLAPSE: %s holds %d systems with %.1f ships\n",
                           Factions::Id(f).c_str(), held, ships);
                    collapsed = true;
                }
            }
        }
        // What each side ended up flying, by weapon family and hull line, against the style of
        // the regions it holds: the regional difference the slice is for.
        printf("\nAt the end, what each faction's warships are, and each region's style:\n\n");
        const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
        for (FactionId f : Factions::All())
        {
            std::map<std::string, float> by;
            float                        all = 0.0f;
            for (const auto& kv : sim.Systems())
            {
                const auto it =
                    kv.second.agg.fleets.find(Simulation::FleetKey(f, NpcRole::Warship));
                if (it == kv.second.agg.fleets.end())
                    continue;
                for (const auto& [design, n] : it->second)
                {
                    const Ships::Design*  d = cat.FindDesign(design);
                    const Ships::Variant* v = cat.FindVariant(design);
                    const Ships::Family*  fam = d != nullptr ? cat.FamilyOf(*d) : nullptr;
                    by[(fam != nullptr ? fam->id : std::string("?")) + "/" +
                       (v != nullptr && !v->line.empty() ? v->line : std::string("stock"))] += n;
                    all += n;
                }
            }
            if (all <= 0.0f)
                continue;
            printf("- %s:", Factions::Id(f).c_str());
            for (const auto& [k, n] : by)
                printf(" %s %.0f%%", k.c_str(), 100.0 * n / all);
            printf("\n");
        }
        for (const auto& [slot, s] : sim.Styles())
        {
            printf("- region %d (%s):", slot, sim.RegionCulture(slot).c_str());
            for (const auto& [id, share] : s.families)
                printf(" %s %.0f%%", id.c_str(), 100.0 * share);
            for (const auto& [id, share] : s.lines)
                printf(" %s %.0f%%", id.c_str(), 100.0 * share);
            printf("\n");
        }
    }
    printf("\nfleetbench: %s\n", collapsed ? "a faction collapsed" : "no faction collapsed");
    return collapsed ? 1 : 0;
}
