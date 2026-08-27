// The galaxy itself: seeding it, materializing a system's NPCs from its aggregate,
// activating a system, planning a route across it, and persistence.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"

#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/Station.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

void Simulation::InitGalaxy()
{
    for (const auto& info : universe_.systems)
    {
        SystemState& st = systems_[info.id];  // creates a cold skeleton (no entities)
        st.id = info.id;
        if (!st.agg.seeded)
            SeedAggregate(st, info);
    }
}

// Starting population of a system from its base security (heuristics close to the
// former role-based spawn, but in aggregate numbers).
void Simulation::SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info)
{
    float            sec = info.security;
    SystemAggregate& a = st.agg;
    a.baseSecurity = sec;
    a.security = sec;
    a.prosperity = sec;
    a.traders = 2.0f + sec * 4.0f;
    a.miners = 2.0f;
    a.police = roundf(sec * 4.0f);
    a.pirates = std::max(0.0f, roundf((0.7f - sec) * 8.0f));
    a.controller = info.owner.empty() ? FactionId::Independent : FactionFromString(info.owner);
    a.seeded = true;
}

std::vector<std::string> Simulation::Neighbors(const std::string& id) const
{
    std::vector<std::string> out;
    for (const auto& l : universe_.links)
    {
        if (l.a == id)
            out.push_back(l.b);
        else if (l.b == id)
            out.push_back(l.a);
    }
    return out;
}

std::string Simulation::SystemName(const std::string& id) const
{
    for (const auto& s : universe_.systems)
        if (s.id == id)
            return s.name;
    return id;
}

// Materialize (hydrate) the NPCs of a system from its cold aggregate: as many ships of
// each role as the aggregate "accumulated" — that many we spawn in suitable places.
void Simulation::HydrateSystem(SystemState& st)
{
    // Stable ids for the static objects (stations/planets/gates/fields) from JSON —
    // for snapshots/selection over the network. NPCs get an id on spawn (SpawnNpcInto).
    for (auto& e : st.entities)
        if (e->GetId() == 0)
            e->SetId(NextAgentId());

    SpawnNodes             nd = GatherNodes(st);
    const SystemAggregate& agg = st.agg;

    // The controlling faction holds the law; if control is with pirates/no one — fall back
    // to the station's faction, otherwise the Guild.
    FactionId owner = agg.controller;
    if (!Factions::IsLawful(owner))
    {
        owner = FactionId::TradersGuild;
        for (auto& e : st.entities)
            if (Station* s =
                    e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
                if (Factions::IsLawful(s->GetFaction()))
                {
                    owner = s->GetFaction();
                    break;
                }
    }

    auto pick = [&](const std::vector<Vector2>& v) -> Vector2
    { return v[RandRange(0, (int)v.size() - 1)]; };

    // Traders cruise the lanes (stations + gates).
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    if (lanes.size() >= 2)
    {
        FactionId tradeFactions[] = { owner, FactionId::Independent, FactionId::TradersGuild };
        int       traders = (int)roundf(agg.traders);
        for (int i = 0; i < traders; i++)
            SpawnNpcInto(st, pick(lanes), tradeFactions[i % 3], NpcRole::Trader, lanes);
    }

    // Miners — at the fields.
    if (!nd.fields.empty())
    {
        int miners = (int)roundf(agg.miners);
        for (int i = 0; i < miners; i++)
        {
            Vector2              spot = pick(nd.fields);
            std::vector<Vector2> near = { spot };
            Vector2 start = { spot.x + RandRange(-200, 200), spot.y + RandRange(-200, 200) };
            SpawnNpcInto(st, start, FactionId::Independent, NpcRole::Miner, near);
        }
    }

    // The owner's police patrol the whole system (lanes + fields).
    std::vector<Vector2> patrolRoute = lanes;
    patrolRoute.insert(patrolRoute.end(), nd.fields.begin(), nd.fields.end());
    if (patrolRoute.size() >= 2)
    {
        int police = (int)roundf(agg.police);
        for (int i = 0; i < police; i++)
            SpawnNpcInto(st, pick(patrolRoute), owner, NpcRole::Police, patrolRoute);
    }

    // Pirates — on the dark periphery and at gates into dangerous systems (not at peaceful gates).
    std::vector<Vector2> hot = PirateSpots(nd);
    if (!hot.empty())
    {
        int pirates = (int)roundf(agg.pirates);
        for (int i = 0; i < pirates; i++)
        {
            Vector2              spot = PirateSpawnPos(hot, {});
            std::vector<Vector2> patrol = { spot };
            SpawnNpcInto(st, spot, FactionId::Pirates, NpcRole::Pirate, patrol);
        }
    }
}

void Simulation::MaterializeAllSystems(const std::string& systemsDir)
{
    for (const auto& info : universe_.systems)
    {
        SystemState& st = systems_[info.id];  // created by InitGalaxy (aggregate already exists)
        st.id = info.id;
        if (st.entities.empty())
            st.entities = WorldLoader::LoadSystem(systemsDir + info.file);
        if (!st.populated)
        {
            HydrateSystem(st);
            st.populated = true;
        }
    }
}

// Server world persistence: the galaxy (system aggregates) + time. The sup* "pressure"
// field is transient — not written (on load the world has "rested").
void Simulation::SaveWorld(const std::string& path) const
{
    using nlohmann::json;
    json j;
    j["simTime"] = time_;
    json galaxy = json::object();
    for (const auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        galaxy[kv.first] = { { "traders", a.traders },       { "miners", a.miners },
                             { "police", a.police },         { "pirates", a.pirates },
                             { "security", a.security },     { "baseSecurity", a.baseSecurity },
                             { "prosperity", a.prosperity }, { "controller", (int)a.controller } };
    }
    j["galaxy"] = galaxy;

    std::ofstream out(path);
    if (out.is_open())
        out << j.dump(2) << "\n";
}

bool Simulation::LoadWorld(const std::string& path)
{
    using nlohmann::json;
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("galaxy") || !j["galaxy"].is_object())
        return false;

    Reset();
    InitGalaxy();  // skeletons with default aggregates for all systems
    time_ = j.value("simTime", 0.0);
    for (auto it = j["galaxy"].begin(); it != j["galaxy"].end(); ++it)
    {
        const std::string& sid = it.key();
        if (!HasSystem(sid))
            continue;
        SystemAggregate& a = systems_[sid].agg;
        const json&      gj = it.value();
        a.traders = gj.value("traders", a.traders);
        a.miners = gj.value("miners", a.miners);
        a.police = gj.value("police", a.police);
        a.pirates = gj.value("pirates", a.pirates);
        a.security = gj.value("security", a.security);
        a.baseSecurity = gj.value("baseSecurity", a.baseSecurity);
        a.prosperity = gj.value("prosperity", a.prosperity);
        a.controller = (FactionId)gj.value("controller", (int)a.controller);
        a.seeded = true;
    }
    return true;
}

void Simulation::Reset()
{
    systems_.clear();
    agentIdCounter_ = 0;
}

SystemState* Simulation::SystemById(const std::string& id)
{
    auto it = systems_.find(id);
    return it == systems_.end() ? nullptr : &it->second;  // pointer in std::map is stable
}

const SystemState* Simulation::SystemById(const std::string& id) const
{
    auto it = systems_.find(id);
    return it == systems_.end() ? nullptr : &it->second;
}

const WorldLoader::SystemInfo* Simulation::SystemInfoById(const std::string& id) const
{
    for (const auto& si : universe_.systems)
        if (si.id == id)
            return &si;
    return nullptr;
}

// --- Route planning (#30) ----------------------------------------------------
//
// A jump today is a single hop: the client names one gate and the server checks it is
// close enough. "Fly to Verge" therefore meant planning the hops by hand, and for an
// agent every extra hop is another round trip to a language model. This turns the whole
// journey into one order.
std::vector<std::string> Simulation::PlanRoute(const std::string& from, const std::string& to,
                                               bool avoidDanger) const
{
    if (from.empty() || to.empty() || !HasSystem(from) || !HasSystem(to))
        return {};
    if (from == to)
        return { from };

    // What one hop into a system costs. Counting hops gives the short way; weighing danger
    // gives the way a loaded hauler survives. Both are Dijkstra over the same small graph,
    // so the difference is only this function.
    auto cost = [this, avoidDanger](const std::string& id) -> double
    {
        if (!avoidDanger)
            return 1.0;
        auto it = systems_.find(id);
        if (it == systems_.end())
            return 1.0;
        const SystemAggregate& a = it->second.agg;
        // Low security and a pirate presence both make a system expensive to cross. The
        // numbers only need to order routes sensibly, not to model risk exactly.
        double danger = (1.0 - (double)a.security) * 4.0 + (double)a.pirates * 0.5;
        return 1.0 + danger;
    };

    std::map<std::string, double>      best;
    std::map<std::string, std::string> cameFrom;
    // The galaxy is a handful of systems, so a linear scan for the next node is cheaper to
    // read than a priority queue and indistinguishable in cost.
    std::map<std::string, bool> settled;
    for (const auto& kv : systems_)
        best[kv.first] = 1e18;
    best[from] = 0.0;

    for (;;)
    {
        std::string cur;
        double      curCost = 1e18;
        for (const auto& kv : best)
            if (!settled[kv.first] && kv.second < curCost)
            {
                cur = kv.first;
                curCost = kv.second;
            }
        if (cur.empty())
            break;  // nothing reachable left
        if (cur == to)
            break;
        settled[cur] = true;

        for (const std::string& n : Neighbors(cur))
        {
            if (settled[n])
                continue;
            double via = curCost + cost(n);
            auto   it = best.find(n);
            if (it != best.end() && via < it->second)
            {
                it->second = via;
                cameFrom[n] = cur;
            }
        }
    }

    if (best[to] >= 1e18)
        return {};  // unreachable: say so rather than returning a partial path

    std::vector<std::string> path;
    for (std::string at = to;; at = cameFrom[at])
    {
        path.push_back(at);
        if (at == from)
            break;
        if (cameFrom.find(at) == cameFrom.end())
            return {};
    }
    std::reverse(path.begin(), path.end());
    return path;
}
