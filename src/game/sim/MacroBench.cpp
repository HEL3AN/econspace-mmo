// `econserver macrobench`: what the galaxy costs to run, by its size (#295).
//
// Builds a world whose region beyond the wormhole has N systems, runs the host's world
// step headless for a fixed stretch of simulated time, and says where the time went. Nothing
// here changes a rule; it calls what the host calls, in the order the host calls it
// (HostStepWorld), with a stopwatch around each part. Nobody is connected unless asked, and
// then each player stands in a system of their own; --hot keeps every system hot, which is
// what the world cost before cold systems (#295).

#include "sim/MacroBench.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Combatant.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ProcessMemory.h"
#include "sim/Simulation.h"
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
    double marketMs = 0.0;      // market recovery in every system, per tick
    double structuresMs = 0.0;  // StepStructures, per tick
    double recountMs = 0.0;     // per coarse pass
    double macroMs = 0.0;       // StepWorldMacro, per coarse pass
    double topUpMs = 0.0;       // the spawn director, per coarse pass
    double tickMs = 0.0, worstTickMs = 0.0;
    double saveMs = 0.0;
    long   saveBytes = 0;
    double memoryMb = -1.0;  // resident growth from before the world was built
};

BenchRow RunOne(int regionSystems, double seconds, uint64_t seed, bool allHot, int players)
{
    const std::string dataDir = SIM_DATA_DIR;
    BenchRow          row;
    row.region = regionSystems;
    const double memBefore = ResidentMegabytes();

    // As SetupHostSim does it, with the region's size the one thing chosen here.
    SetRandomSeed(0xC0FFEEu);
    Simulation sim;
    sim.LoadUniverse(dataDir + "universe.json");
    Clock::time_point t = Clock::now();
    sim.AttachRegion(seed, dataDir + "systems/", regionSystems);
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

    // HostStepWorld: the agents and the market of every hot system, then maintenance.
    const float              dt = 1.0f / 60.0f;
    Simulation::MaintainCost cost;
    double                   agents = 0.0, market = 0.0, total = 0.0;
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
        t = Clock::now();
        for (auto& kv : sim.Systems())
            if (Simulation::IsHot(kv.second))
                kv.second.market.Update(dt);
        market += Since(t);
        sim.MaintainWorld(dt, &cost);
        sim.TakeLayoutDeltas();  // the host sends these every frame; nobody is here to read them
        const double ms = Since(tick);
        total += ms;
        row.worstTickMs = std::max(row.worstTickMs, ms);
    }
    row.passes = cost.passes;
    row.agentsMs = agents / row.ticks;
    row.marketMs = market / row.ticks;
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
    const std::string dataDir = SIM_DATA_DIR;
    Factions::Load(dataDir + "factions.json");
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
    printf("| region | systems | links | players | hot | static | NPCs | generate ms | "
           "materialise ms | "
           "agents ms/tick | market ms/tick | structures ms/tick | recount ms/pass | "
           "macro ms/pass | top-up ms/pass | tick ms (mean / worst) | save ms | save KB | "
           "memory MB |\n");
    printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
           "---:|---:|\n");
    for (const BenchRow& r : rows)
    {
        char mem[32];
        if (r.memoryMb >= 0.0)
            snprintf(mem, sizeof(mem), "%.0f", r.memoryMb);
        else
            snprintf(mem, sizeof(mem), "n/a");
        printf("| %d | %d | %d | %d | %d | %ld | %ld | %.1f | %.1f | %.3f | %.3f | %.4f | %.3f | "
               "%.3f | %.3f | %.2f / %.1f | %.2f | %.0f | %s |\n",
               r.region, r.systems, r.links, r.players, r.hot, r.statics, r.npcs, r.generateMs,
               r.materializeMs, r.agentsMs, r.marketMs, r.structuresMs, r.recountMs, r.macroMs,
               r.topUpMs, r.tickMs, r.worstTickMs, r.saveMs, r.saveBytes / 1024.0, mem);
    }
    return 0;
}
