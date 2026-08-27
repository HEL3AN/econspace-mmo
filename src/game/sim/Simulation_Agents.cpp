// The living galaxy: NPC behaviour, NPC combat, the spawn director and the macro step.
//
// One translation unit of Simulation (#17), not a separate class: these are the rules
// that run in every system whether or not anyone is watching, and they share the
// population aggregate the spawn director reads and writes.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"

#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/Combatant.h"
#include "entities/JumpGate.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/Station.h"

#include <algorithm>
#include <cmath>

namespace
{
// NPC combat parameters (server combat/AI simulation).
constexpr float PIRATE_WEAPON_RANGE = 230.0f;  // NPC fire range
constexpr float PIRATE_WEAPON_DAMAGE = 7.0f;   // damage to the player
constexpr float NPC_WEAPON_DAMAGE = 6.0f;      // NPC-vs-NPC damage
constexpr float NPC_AGGRO_RANGE = 3500.0f;     // AI target detection radius
constexpr float NPC_THREAT_RANGE = 2000.0f;    // distance at which peaceful ones flee

// Spawn director. "Pressure": losses in a role raise the suppression of its spawn,
// which then slowly recovers — the player/battles really change the population.
constexpr float SUPPRESS_PER_KILL = 0.30f;        // suppression gain per 1 loss
constexpr float SUPPRESS_DECAY = 0.97f;           // suppression falloff per coarse step (~2 s)
constexpr float DANGER_SEC = 0.35f;               // below this the system is "unsafe" (ambushes)
constexpr float SPAWN_MIN_PLAYER_DIST = 2600.0f;  // do not spawn closer to the player
constexpr float MAINT_STEP = 2.0f;                // coarse world maintenance period

float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Accumulate spawn suppression from losses (in [0,1]).
void AccumSuppress(float& sup, float losses)
{
    if (losses > 0.0f)
        sup = std::min(1.0f, sup + losses * SUPPRESS_PER_KILL);
}

float Dist(Vector2 a, Vector2 b)
{
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// Live NPCs of a system.
std::vector<NpcShip*> AliveShips(SystemState& st)
{
    std::vector<NpcShip*> ships;
    for (auto& e : st.entities)
        if (NpcShip* n = e->GetKind() == EntityKind::Npc ? static_cast<NpcShip*>(e.get()) : nullptr)
            if (n->IsAlive())
                ships.push_back(n);
    return ships;
}
}  // namespace

bool Simulation::NpcHostileToNpc(const NpcShip* a, const NpcShip* b)
{
    Stance s = Factions::Relation(a->GetFaction(), b->GetFaction());
    return s == Stance::Hostile || s == Stance::War;
}

void Simulation::StepNpcAi(SystemState& st, Combatant* player,
                           const std::function<bool(const NpcShip*)>& hostileToPlayer)
{
    std::vector<NpcShip*> ships = AliveShips(st);
    for (NpcShip* n : ships)
    {
        Vector2 npos = n->GetPosition();
        if (n->IsCombatant())
        {
            // Nearest hostile target within the detection radius.
            float   bestD = NPC_AGGRO_RANGE;
            Vector2 bestPos{};
            bool    found = false;
            if (player != nullptr && hostileToPlayer && hostileToPlayer(n))
            {
                float d = Dist(player->GetPosition(), npos);
                if (d < bestD)
                {
                    bestD = d;
                    bestPos = player->GetPosition();
                    found = true;
                }
            }
            for (NpcShip* o : ships)
            {
                if (o == n || !NpcHostileToNpc(n, o))
                    continue;
                float d = Dist(o->GetPosition(), npos);
                if (d < bestD)
                {
                    bestD = d;
                    bestPos = o->GetPosition();
                    found = true;
                }
            }

            if (!found)
                n->StandDown();
            else if (n->GetHull() < n->GetMaxHull() * 0.25f)
                n->FleeFrom(bestPos);  // heavily damaged — retreats
            else
                n->Engage(bestPos);
        }
        else
        {
            // Peaceful: flees from the nearest combat enemy within the threat zone.
            float   td = NPC_THREAT_RANGE;
            Vector2 threat{};
            bool    threatened = false;
            for (NpcShip* o : ships)
            {
                if (o == n || !o->IsCombatant() || !NpcHostileToNpc(o, n))
                    continue;
                float d = Dist(o->GetPosition(), npos);
                if (d < td)
                {
                    td = d;
                    threat = o->GetPosition();
                    threatened = true;
                }
            }
            if (threatened)
                n->FleeFrom(threat);
            else
                n->StandDown();
        }
    }
}

void Simulation::StepNpcCombat(SystemState& st, Combatant* player, bool playerHidden,
                               const std::function<bool(const NpcShip*)>& hostileToPlayer,
                               std::vector<FireEvent>*                    fires)
{
    std::vector<NpcShip*> ships = AliveShips(st);
    for (NpcShip* npc : ships)
    {
        if (!npc->IsAlive() || !npc->IsCombatant() || !npc->ReadyToFire())
            continue;

        Vector2    npos = npc->GetPosition();
        float      bestD = PIRATE_WEAPON_RANGE;
        Combatant* target = nullptr;
        bool       targetIsPlayer = false;

        auto consider = [&](Combatant* c, bool isPlayer)
        {
            float d = Dist(c->GetPosition(), npos);
            if (d <= bestD)
            {
                bestD = d;
                target = c;
                targetIsPlayer = isPlayer;
            }
        };

        if (player != nullptr && !playerHidden && hostileToPlayer && hostileToPlayer(npc))
            consider(player, true);
        for (NpcShip* other : ships)
            if (other != npc && other->IsAlive() && NpcHostileToNpc(npc, other))
                consider(other, false);

        if (target == nullptr)
            continue;

        target->TakeDamage(targetIsPlayer ? PIRATE_WEAPON_DAMAGE : NPC_WEAPON_DAMAGE);
        npc->ResetFireTimer();
        if (fires != nullptr)
            fires->push_back({ npos, target->GetPosition(), npc->GetFaction(), targetIsPlayer });
    }
}

void Simulation::StepSystemAgents(SystemState& st, float dt)
{
    StepNpcAi(st, nullptr, {});  // AI without the player

    for (auto& e : st.entities)  // movement
        e->Update(dt);

    StepNpcCombat(st, nullptr, false, {}, nullptr);  // NPC-vs-NPC combat, no events

    // Cleanup of the fallen.
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     {
                                         NpcShip* n = e->GetKind() == EntityKind::Npc
                                                          ? static_cast<NpcShip*>(e.get())
                                                          : nullptr;
                                         return n != nullptr && !n->IsAlive();
                                     }),
                      st.entities.end());
}

void Simulation::StepActiveSystemAgents(SystemState& st, Combatant* player, bool playerHidden,
                                        const std::function<bool(const NpcShip*)>& hostileToPlayer,
                                        std::vector<FireEvent>* fires, float dt)
{
    StepNpcAi(st, player, hostileToPlayer);  // AI sees the player (pursuit/flight)

    for (auto& e : st.entities)  // movement
        e->Update(dt);

    StepNpcCombat(st, player, playerHidden, hostileToPlayer,
                  fires);  // combat with the player, beams

    // Cleanup of the fallen.
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     {
                                         NpcShip* n = e->GetKind() == EntityKind::Npc
                                                          ? static_cast<NpcShip*>(e.get())
                                                          : nullptr;
                                         return n != nullptr && !n->IsAlive();
                                     }),
                      st.entities.end());
}

// M0 macro on REAL numbers (population in the aggregate is filled by Game::RecountAgg).
// Security/economy drift, economy diffusion, controller changes. The population is not
// changed — that is done by real battles and the Game spawn director.
void Simulation::StepWorldMacro()
{
    // Security and economy drift from the real populations.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        a.security = ClampF(a.security + (a.baseSecurity - a.security) * 0.05f +
                                (a.police - a.pirates) * 0.02f,
                            0.0f, 1.0f);
        a.prosperity = ClampF(a.prosperity + (a.security - 0.5f) * 0.02f +
                                  (a.traders - 3.0f) * 0.008f - a.pirates * 0.01f,
                              0.0f, 1.0f);
    }

    // Economic diffusion along gate lines (trade links neighbors).
    for (const auto& l : universe_.links)
    {
        auto a = systems_.find(l.a);
        auto b = systems_.find(l.b);
        if (a == systems_.end() || b == systems_.end())
            continue;
        float dp = b->second.agg.prosperity - a->second.agg.prosperity;
        a->second.agg.prosperity = ClampF(a->second.agg.prosperity + dp * 0.02f, 0.0f, 1.0f);
        b->second.agg.prosperity = ClampF(b->second.agg.prosperity - dp * 0.02f, 0.0f, 1.0f);
    }

    // Controller changes (on real numbers; reinforcements are provided by the director).
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        if (a.controller != FactionId::Pirates && a.pirates > a.police * 2.0f + 2.0f &&
            a.security < 0.25f)
        {
            a.controller = FactionId::Pirates;
            a.baseSecurity = std::min(a.baseSecurity, 0.15f);
            PushEvent("Pirates seized " + SystemName(kv.first));
        }
        else if (a.controller == FactionId::Pirates && a.police > a.pirates && a.security > 0.4f)
        {
            // Law restored (the director brought police from a strong neighbor).
            for (const std::string& nid : Neighbors(kv.first))
            {
                auto n = systems_.find(nid);
                if (n != systems_.end() && Factions::IsLawful(n->second.agg.controller) &&
                    n->second.agg.security > 0.6f)
                {
                    a.controller = n->second.agg.controller;
                    a.baseSecurity = std::max(a.baseSecurity, 0.5f);
                    PushEvent(SystemName(kv.first) + " liberated by " +
                              FactionName(n->second.agg.controller));
                    break;
                }
            }
        }
    }
}

void Simulation::RecountAgg(SystemState& st)
{
    int tr = 0, mi = 0, po = 0, pi = 0;
    for (auto& e : st.entities)
        if (NpcShip* n = e->GetKind() == EntityKind::Npc ? static_cast<NpcShip*>(e.get()) : nullptr)
            switch (n->GetRole())
            {
                case NpcRole::Trader: tr++; break;
                case NpcRole::Miner: mi++; break;
                case NpcRole::Police: po++; break;
                case NpcRole::Pirate: pi++; break;
                case NpcRole::Warship: po++; break;  // combat factions — count as "police"
            }
    st.agg.traders = (float)tr;
    st.agg.miners = (float)mi;
    st.agg.police = (float)po;
    st.agg.pirates = (float)pi;
}

// System nodes for spawning. Gates are marked "dangerous" if they lead into an unsafe/
// pirate system — only there do pirate ambushes belong.
Simulation::SpawnNodes Simulation::GatherNodes(const SystemState& st) const
{
    SpawnNodes nd;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() == EntityKind::Station)
            nd.stations.push_back(e->GetPosition());
        else if (JumpGate* g =
                     e->GetKind() == EntityKind::Gate ? static_cast<JumpGate*>(e.get()) : nullptr)
        {
            Vector2 p = g->GetPosition();
            nd.gates.push_back(p);
            auto dest = systems_.find(g->GetDestination());
            if (dest != systems_.end())
            {
                const SystemAggregate& da = dest->second.agg;
                if (da.controller == FactionId::Pirates || da.security < DANGER_SEC)
                    nd.dangerGates.push_back(p);  // border with low-sec — ambush spot
            }
        }
        else if (e->GetKind() == EntityKind::Field)
        {
            Vector2 p = e->GetPosition();
            nd.fields.push_back(p);
            if (sqrtf(p.x * p.x + p.y * p.y) > World::MID_RADIUS)
                nd.outerSpots.push_back(p);  // far periphery
        }
    }
    return nd;
}

std::vector<Vector2> Simulation::PirateSpots(const SpawnNodes& nd)
{
    std::vector<Vector2> hot = nd.outerSpots;
    hot.insert(hot.end(), nd.dangerGates.begin(), nd.dangerGates.end());
    if (hot.empty())
        hot = nd.fields;  // no periphery/danger gates — at the fields, but not at peaceful gates
    return hot;
}

Vector2 Simulation::PirateSpawnPos(const std::vector<Vector2>& pool,
                                   const std::vector<Vector2>& avoid)
{
    Vector2 base = pool[RandRange(0, (int)pool.size() - 1)];
    Vector2 pos = base;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        pos = { base.x + RandRange(-450, 450), base.y + RandRange(-450, 450) };
        if (avoid.empty())
            break;  // no one to avoid (background/hydrate)
        // Far enough from EVERY player in the system: clearing one player's space by
        // dropping the ambush into another's is not an improvement.
        bool clear = true;
        for (Vector2 a : avoid)
        {
            float dx = pos.x - a.x, dy = pos.y - a.y;
            if (dx * dx + dy * dy < SPAWN_MIN_PLAYER_DIST * SPAWN_MIN_PLAYER_DIST)
            {
                clear = false;
                break;
            }
        }
        if (clear)
            break;
    }
    return pos;
}

void Simulation::SpawnNpcInto(SystemState& st, Vector2 pos, FactionId faction, NpcRole role,
                              std::vector<Vector2> waypoints)
{
    auto npc = std::make_unique<NpcShip>(pos, faction, role, std::move(waypoints));
    npc->SetId(NextAgentId());
    st.entities.push_back(std::move(npc));
}

// Spawn director: tops up a system's population to targets by security/controller,
// accounting for the "pressure" from losses. A pirate system loses trade/police; a strong
// lawful neighbor sends reinforcements (police), which leads to reconquest.
void Simulation::TopUpSystem(SystemState& st, const std::vector<Vector2>& avoid)
{
    SpawnNodes           nd = GatherNodes(st);
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    std::vector<Vector2> patrolRoute = lanes;
    patrolRoute.insert(patrolRoute.end(), nd.fields.begin(), nd.fields.end());
    std::vector<Vector2> hot = PirateSpots(nd);

    const SystemAggregate& agg = st.agg;
    float                  sec = agg.security;
    FactionId              ctrl = agg.controller;
    bool                   lawful = Factions::IsLawful(ctrl);

    // Police faction and its target. A pirate system has no police UNLESS there is a
    // strong lawful neighbor — then it sends reinforcements.
    FactionId policeFac = lawful ? ctrl : FactionId::TradersGuild;
    int       policeTarget = lawful ? (int)roundf(sec * 4.0f) : 0;
    if (ctrl == FactionId::Pirates)
        for (const std::string& nid : Neighbors(st.id))
        {
            auto n = systems_.find(nid);
            if (n != systems_.end() && Factions::IsLawful(n->second.agg.controller) &&
                n->second.agg.security > 0.6f && n->second.agg.police >= 3.0f)
            {
                policeFac = n->second.agg.controller;
                policeTarget = 4;  // neighbor's reinforcements
                break;
            }
        }

    int piratesTarget =
        (ctrl == FactionId::Pirates) ? 6 : std::max(0, (int)roundf((0.7f - sec) * 8.0f));
    int       tradersTarget = (ctrl == FactionId::Pirates) ? 1 : (int)roundf(2.0f + sec * 4.0f);
    int       minersTarget = 2;
    FactionId tradeFac = lawful ? ctrl : FactionId::Independent;

    // "Pressure": recent losses temporarily cut the target (recovers in MaintainWorld).
    tradersTarget = (int)roundf(tradersTarget * (1.0f - agg.supTraders));
    minersTarget = (int)roundf(minersTarget * (1.0f - agg.supMiners));
    policeTarget = (int)roundf(policeTarget * (1.0f - agg.supPolice));
    piratesTarget = (int)roundf(piratesTarget * (1.0f - agg.supPirates));

    // Current population by role.
    int tr = 0, mi = 0, po = 0, pi = 0;
    for (auto& e : st.entities)
        if (NpcShip* n = e->GetKind() == EntityKind::Npc ? static_cast<NpcShip*>(e.get()) : nullptr)
            switch (n->GetRole())
            {
                case NpcRole::Trader: tr++; break;
                case NpcRole::Miner: mi++; break;
                case NpcRole::Police: po++; break;
                case NpcRole::Pirate: pi++; break;
                case NpcRole::Warship: po++; break;
            }

    auto pick = [&](const std::vector<Vector2>& v) -> Vector2
    { return v[RandRange(0, (int)v.size() - 1)]; };

    // Top up by no more than a couple of units per step (smoothly, no bursts).
    int  budget = 2;
    auto canSpawn = [&]() { return budget > 0; };

    if (lanes.size() >= 2)
        while (tr < tradersTarget && canSpawn())
        {
            SpawnNpcInto(st, pick(lanes), tradeFac, NpcRole::Trader, lanes);
            tr++;
            budget--;
        }
    if (!nd.fields.empty())
        while (mi < minersTarget && canSpawn())
        {
            Vector2              spot = pick(nd.fields);
            std::vector<Vector2> near = { spot };
            SpawnNpcInto(st, spot, FactionId::Independent, NpcRole::Miner, near);
            mi++;
            budget--;
        }
    if (patrolRoute.size() >= 2)
        while (po < policeTarget && canSpawn())
        {
            SpawnNpcInto(st, pick(patrolRoute), policeFac, NpcRole::Police, patrolRoute);
            po++;
            budget--;
        }
    if (!hot.empty())
        while (pi < piratesTarget && canSpawn())
        {
            Vector2              spot = PirateSpawnPos(hot, avoid);
            std::vector<Vector2> patrol = { spot };
            SpawnNpcInto(st, spot, FactionId::Pirates, NpcRole::Pirate, patrol);
            pi++;
            budget--;
        }
}

void Simulation::MaintainWorld(float dt)
{
    // The simulation clock lives here because MaintainWorld is called once per tick by
    // every driver of the world -- the host loop and the batch mode -- which makes it the
    // one place elapsed time can accumulate without being counted twice or not at all.
    time_ += dt;
    maintAccum_ += dt;
    while (maintAccum_ >= MAINT_STEP)
    {
        // Recount the real populations; accumulate losses since the last step as "pressure".
        for (auto& kv : systems_)
        {
            SystemAggregate& a = kv.second.agg;
            float prevTr = a.traders, prevMi = a.miners, prevPo = a.police, prevPi = a.pirates;
            RecountAgg(kv.second);
            AccumSuppress(a.supTraders, prevTr - a.traders);
            AccumSuppress(a.supMiners, prevMi - a.miners);
            AccumSuppress(a.supPolice, prevPo - a.police);
            AccumSuppress(a.supPirates, prevPi - a.pirates);
        }
        StepWorldMacro();
        for (auto& kv : systems_)
        {
            // Recovery: the pressure slowly subsides.
            SystemAggregate& a = kv.second.agg;
            a.supTraders *= SUPPRESS_DECAY;
            a.supMiners *= SUPPRESS_DECAY;
            a.supPolice *= SUPPRESS_DECAY;
            a.supPirates *= SUPPRESS_DECAY;
            // Player-avoidance, per system: everyone standing in this one.
            std::vector<Vector2> avoid;
            for (const auto& sv : sessions_)
                if (sv.second.systemId == kv.first && sv.second.ship &&
                    sv.second.dockedStationId == 0)
                    avoid.push_back(sv.second.ship->GetPosition());
            TopUpSystem(kv.second, avoid);
        }
        maintAccum_ -= MAINT_STEP;
    }
}
