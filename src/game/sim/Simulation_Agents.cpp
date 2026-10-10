// The living galaxy: NPC behaviour, NPC combat, the spawn director and the macro step.
//
// One translation unit of Simulation (#17), not a separate class: these are the rules
// that run in every system whether or not anyone is watching, and they share the
// population aggregate the spawn director reads and writes.

#include "sim/Simulation.h"
#include "core/Archetype.h"
#include "core/Orbits.h"
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
#include <chrono>
#include <cmath>

namespace
{
// NPC combat parameters (server combat/AI simulation).
constexpr float PIRATE_WEAPON_RANGE = 230.0f;  // NPC fire range
// An NPC's guns are its design's (data/ships.json); against a player they hit this much
// harder -- 7 to the player for 6 to a ship before designs did the arithmetic.
constexpr float PLAYER_DAMAGE_SCALE = 7.0f / 6.0f;
constexpr float NPC_AGGRO_RANGE = 3500.0f;   // AI target detection radius
constexpr float NPC_THREAT_RANGE = 2000.0f;  // distance at which peaceful ones flee
constexpr float DEFENCE_PERIOD = 1.0f;       // seconds between a station's shots

// Spawn director. "Pressure": losses in a role raise the suppression of its spawn,
// which then slowly recovers — the player/battles really change the population.
constexpr float SUPPRESS_PER_KILL = 0.30f;        // suppression gain per 1 loss
constexpr float SUPPRESS_DECAY = 0.97f;           // suppression falloff per coarse step (~2 s)
constexpr float DANGER_SEC = 0.35f;               // below this the system is "unsafe" (ambushes)
constexpr float SPAWN_MIN_PLAYER_DIST = 2600.0f;  // do not spawn closer to the player
constexpr float MAINT_STEP = 2.0f;                // coarse world maintenance period
constexpr int   DIRECTOR_BUDGET = 2;              // ships the director adds to a system a pass

// A cold system's losses per pass (#295), per pirate and per ship of the other side: what
// the hot world showed when measured -- seeds 3 and 7, two hours each, every system hot,
// 75 000 system-passes, every death counted by role against the populations that met. Ships
// in a system a million units across seldom meet: 27 traders, 4 miners, 2 pirates and one
// policeman died in all that time.
constexpr float COLD_TRADER_LOSS = 6e-5f;    // traders lost, per pirate per trader
constexpr float COLD_MINER_LOSS = 6e-6f;     // miners lost, per pirate per miner
constexpr float COLD_PIRATE_LOSS = 1.5e-5f;  // pirates lost, per pirate per policeman
constexpr float COLD_POLICE_LOSS = 7e-6f;    // police lost, per pirate per policeman

// A system a gate into is an ambush spot: the pirates hold it, or its security is low.
bool Dangerous(const SystemAggregate& a)
{
    return a.controller == FactionId::Pirates || a.security < DANGER_SEC;
}

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

// Adds the wall time since `from` to `into` and restarts the stopwatch, when there is
// somewhere to add it (#295). Nothing is read from the clock when nobody asked.
using BenchClock = std::chrono::steady_clock;
void Lap(double* into, BenchClock::time_point& from)
{
    if (into == nullptr)
        return;
    const BenchClock::time_point now = BenchClock::now();
    *into += std::chrono::duration<double>(now - from).count();
    from = now;
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

void Simulation::StepNpcAi(SystemState& st, const std::vector<PlayerPresence>& players)
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
            for (const PlayerPresence& p : players)
            {
                if (p.ship == nullptr || p.session == nullptr ||
                    !AccountHostileToFaction(*p.session, n->GetFaction()))
                    continue;
                float d = Dist(p.ship->GetPosition(), npos);
                if (d < bestD)
                {
                    bestD = d;
                    bestPos = p.ship->GetPosition();
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

void Simulation::StepNpcCombat(SystemState& st, const std::vector<PlayerPresence>& players,
                               std::vector<FireEvent>* fires)
{
    std::vector<NpcShip*> ships = AliveShips(st);
    for (NpcShip* npc : ships)
    {
        if (!npc->IsAlive() || !npc->IsCombatant() || !npc->ReadyToFire())
            continue;

        Vector2    npos = npc->GetPosition();
        float      bestD = PIRATE_WEAPON_RANGE;
        Combatant* target = nullptr;
        int        targetSession = 0;  // 0 means the target is an NPC

        auto consider = [&](Combatant* c, int sessionId)
        {
            float d = Dist(c->GetPosition(), npos);
            if (d <= bestD)
            {
                bestD = d;
                target = c;
                targetSession = sessionId;
            }
        };

        for (const PlayerPresence& p : players)
            if (p.ship != nullptr && p.session != nullptr && !p.hidden &&
                AccountHostileToFaction(*p.session, npc->GetFaction()))
                consider(p.ship, p.session->id);
        for (NpcShip* other : ships)
            if (other != npc && other->IsAlive() && NpcHostileToNpc(npc, other))
                consider(other, 0);

        if (target == nullptr)
            continue;

        // What a volley takes off is the design's guns (#279 step 4); a player is hit a sixth
        // harder than a ship of its own kind, as before, so one raider is a threat on its own.
        target->TakeDamage(npc->GetDamage() * (targetSession != 0 ? PLAYER_DAMAGE_SCALE : 1.0f));
        npc->ResetFireTimer();
        if (fires != nullptr)
        {
            FireEvent fe;
            fe.from = npos;
            fe.to = target->GetPosition();
            fe.shooterFaction = npc->GetFaction();
            fe.targetSessionId = targetSession;
            fires->push_back(fe);
        }
    }
}

void Simulation::StepStationDefence(SystemState& st, const std::vector<PlayerPresence>& players,
                                    std::vector<FireEvent>* fires, float dt)
{
    std::vector<NpcShip*> ships;  // gathered once, and only if something can shoot
    bool                  gathered = false;
    for (auto& e : st.entities)
    {
        // Who a battery shoots at depends on whose it is, and ownership is still
        // Station-specific state (#41) -- so a defensive object with no owner is inert.
        if (!e->Has(Component::Defensive) || e->GetKind() != EntityKind::Station)
            continue;
        const Archetype* a = e->GetArchetype();
        float&           cooldown = st.defenceCooldown[e->GetId()];
        cooldown -= dt;
        if (cooldown > 0.0f || a->weaponDamage <= 0.0f)
            continue;

        if (!gathered)
        {
            ships = AliveShips(st);
            gathered = true;
        }
        const FactionId owner = static_cast<const Station*>(e.get())->GetFaction();
        const Vector2   spos = e->GetPosition();
        float           bestD = e->GetSize() + a->weaponRange;
        Combatant*      target = nullptr;
        int             targetSession = 0;

        auto consider = [&](Combatant* c, int sessionId)
        {
            float d = Dist(c->GetPosition(), spos);
            if (d <= bestD)
            {
                bestD = d;
                target = c;
                targetSession = sessionId;
            }
        };
        for (const PlayerPresence& p : players)
            if (p.ship != nullptr && p.session != nullptr && !p.hidden && p.ship->IsAlive() &&
                AccountHostileToFaction(*p.session, owner))
                consider(p.ship, p.session->id);
        for (NpcShip* n : ships)
        {
            const Stance rel = Factions::Relation(owner, n->GetFaction());
            if (n->IsAlive() && (rel == Stance::Hostile || rel == Stance::War))
                consider(n, 0);
        }

        if (target == nullptr)
        {
            cooldown = 0.0f;  // ready the moment something comes into range
            continue;
        }
        target->TakeDamage(a->weaponDamage * DEFENCE_PERIOD);
        cooldown = DEFENCE_PERIOD;
        if (fires != nullptr)
        {
            FireEvent fe;
            fe.from = spos;
            fe.to = target->GetPosition();
            fe.shooterFaction = owner;
            fe.targetSessionId = targetSession;
            fires->push_back(fe);
        }
    }
}

void Simulation::StepSystemAgents(SystemState& st, const std::vector<PlayerPresence>& players,
                                  std::vector<FireEvent>* fires, float dt)
{
    if (!IsHot(st))
        return;  // nobody to step: the coarse pass keeps a cold system (#295)
    StepNpcAi(st, players);

    // Planets and what orbits them are where the clock says (#210), before anything this
    // tick asks where a station or a belt is.
    Orbits::Place(st.entities, time_);

    for (auto& e : st.entities)  // movement
        e->Update(dt);

    StepNpcCombat(st, players, fires);
    StepStationDefence(st, players, fires, dt);

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
    // The first passes after a start are the world settling into its numbers, not events
    // anybody should read about (#143).
    const bool settling = macroSteps_ < SETTLE_STEPS;
    macroSteps_++;

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

    // Who reaches where, and who holds what: the same rule for every faction (#231).
    StepFactions();
    StepControl(settling);
}

void Simulation::RecountAgg(SystemState& st)
{
    if (!IsHot(st))
        return;
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
                if (Dangerous(dest->second.agg))
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
        pos = { base.x + RandRange(-3000, 3000), base.y + RandRange(-3000, 3000) };
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
    // Which of its role's designs it flies is decided once, by its id (#279 step 4), so a
    // role with several in its faction's doctrine flies all of them.
    const int id = NextAgentId();
    auto      npc = std::make_unique<NpcShip>(
        pos, faction, role, std::move(waypoints),
        Archetypes::ShipCatalogue().Pick(Factions::Id(faction), NpcRoleId(role), (unsigned)id));
    npc->SetId(id);
    st.entities.push_back(std::move(npc));
}

Simulation::Room Simulation::RoomOf(const SpawnNodes& nd)
{
    Room r;
    r.lanes = nd.stations.size() + nd.gates.size();
    r.fields = nd.fields.size();
    // PirateSpots: the periphery and the gates into danger, else the belts.
    r.dark = !nd.fields.empty() || !nd.dangerGates.empty();
    return r;
}

Simulation::Room Simulation::RoomOf(const SystemState& st) const
{
    Room r;
    r.lanes = (size_t)(st.profile.stations + st.profile.gates);
    r.fields = (size_t)st.profile.belts;
    r.dark = r.fields > 0;
    // A gate leads to each neighbour; one into danger is where an ambush belongs.
    for (const std::string& n : Neighbors(st.id))
    {
        if (r.dark)
            break;
        auto it = systems_.find(n);
        r.dark = it != systems_.end() && Dangerous(it->second.agg);
    }
    return r;
}

Simulation::Population Simulation::PopulationOf(const SystemState& st, const Room& room) const
{
    const SystemAggregate& agg = st.agg;
    const float            sec = agg.security;
    const FactionId        ctrl = agg.controller;
    const bool             lawful = Factions::IsLawful(ctrl);
    Population             p;

    // Armed ships come from what each faction has committed here (#231), not from a formula
    // of security: the lawful as police under whichever of them is strongest here, the
    // pirates as pirates.
    p.policeFaction = lawful ? ctrl : FactionId::TradersGuild;
    float lawPresence = 0.0f, strongestLaw = 0.0f;
    for (int f = 0; f < FACTION_COUNT; f++)
        if (Factions::IsLawful((FactionId)f))
        {
            lawPresence += agg.presence[f];
            if (agg.presence[f] > strongestLaw)
            {
                strongestLaw = agg.presence[f];
                p.policeFaction = (FactionId)f;
            }
        }
    p.tradeFaction = lawful ? ctrl : FactionId::Independent;
    const int traders = (ctrl == FactionId::Pirates) ? 1 : (int)roundf(2.0f + sec * 4.0f);

    // "Pressure": recent losses temporarily cut the target (recovers in MaintainWorld).
    p.traders = (int)roundf(traders * (1.0f - agg.supTraders));
    p.miners = (int)roundf(2.0f * (1.0f - agg.supMiners));
    p.police = (int)roundf(roundf(lawPresence) * (1.0f - agg.supPolice));
    p.pirates =
        (int)roundf(roundf(agg.presence[(int)FactionId::Pirates]) * (1.0f - agg.supPirates));

    // Nowhere to put them, none of them: traders need two ends of a lane, miners a belt,
    // police a beat, pirates somewhere dark.
    if (room.lanes < 2)
        p.traders = 0;
    if (room.fields == 0)
        p.miners = 0;
    if (room.lanes + room.fields < 2)
        p.police = 0;
    if (!room.dark)
        p.pirates = 0;
    return p;
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

    const Population target = PopulationOf(st, RoomOf(nd));
    st.agg.policeFaction = target.policeFaction;

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
    int  budget = DIRECTOR_BUDGET;
    auto canSpawn = [&]() { return budget > 0; };

    while (tr < target.traders && canSpawn())
    {
        SpawnNpcInto(st, pick(lanes), target.tradeFaction, NpcRole::Trader, lanes);
        tr++;
        budget--;
    }
    while (mi < target.miners && canSpawn())
    {
        Vector2              spot = pick(nd.fields);
        std::vector<Vector2> near = { spot };
        SpawnNpcInto(st, spot, FactionId::Independent, NpcRole::Miner, near);
        mi++;
        budget--;
    }
    while (po < target.police && canSpawn())
    {
        SpawnNpcInto(st, pick(patrolRoute), target.policeFaction, NpcRole::Police, patrolRoute);
        po++;
        budget--;
    }
    while (pi < target.pirates && canSpawn())
    {
        Vector2              spot = PirateSpawnPos(hot, avoid);
        std::vector<Vector2> patrol = { spot };
        SpawnNpcInto(st, spot, FactionId::Pirates, NpcRole::Pirate, patrol);
        pi++;
        budget--;
    }
}

void Simulation::ColdLosses(SystemState& st)
{
    // Expected losses over one pass, from the rates the hot world shows: pirates raid
    // traders and miners and trade shots with police, each pair at its own rate. They are
    // small -- a system is a million units across and ships seldom meet -- and they are the
    // same losses a hot system counts, so the faction step and the pressure see no seam.
    SystemAggregate& a = st.agg;
    const float      pirates = a.pirates, police = a.police;
    if (pirates <= 0.0f)
        return;
    const float lostPirates = std::min(pirates, COLD_PIRATE_LOSS * pirates * police);
    const float lostPolice = std::min(police, COLD_POLICE_LOSS * pirates * police);
    const float lostTraders = std::min(a.traders, COLD_TRADER_LOSS * pirates * a.traders);
    const float lostMiners = std::min(a.miners, COLD_MINER_LOSS * pirates * a.miners);
    a.pirates -= lostPirates;
    a.police -= lostPolice;
    a.traders -= lostTraders;
    a.miners -= lostMiners;
    AccumSuppress(a.supTraders, lostTraders);
    AccumSuppress(a.supMiners, lostMiners);
    AccumSuppress(a.supPolice, lostPolice);
    AccumSuppress(a.supPirates, lostPirates);
    a.lostPirates += lostPirates;
    a.lostPolice += lostPolice;
}

void Simulation::ColdTopUp(SystemState& st)
{
    // The spawn director, as numbers: up towards what it keeps here by the couple of ships a
    // pass it would send, and never down. In a hot system a ship stays until somebody
    // destroys it, whatever the director would send now; a cold one keeps the same count,
    // so that a system does not change its character by being looked at.
    const Population target = PopulationOf(st, RoomOf(st));
    SystemAggregate& a = st.agg;
    a.policeFaction = target.policeFaction;
    float budget = (float)DIRECTOR_BUDGET;
    auto  topUp = [&](float& count, int want)
    {
        const float step = std::min(std::max(0.0f, (float)want - count), budget);
        count += step;
        budget -= step;
    };
    topUp(a.traders, target.traders);
    topUp(a.miners, target.miners);
    topUp(a.police, target.police);
    topUp(a.pirates, target.pirates);

    // What every tick would have done to the rest of it: prices recover, bodies move on.
    st.market.Recover(MAINT_STEP);
    Orbits::Place(st.entities, time_);
}

void Simulation::MaintainWorld(float dt, MaintainCost* cost)
{
    BenchClock::time_point lap = cost != nullptr ? BenchClock::now() : BenchClock::time_point();
    // The simulation clock lives here because MaintainWorld is called once per tick by
    // every driver of the world -- the host loop and the batch mode -- which makes it the
    // one place elapsed time can accumulate without being counted twice or not at all.
    time_ += dt;
    maintAccum_ += dt;
    // Wherever a player is, the system is hot, and stays so for a while after (#295). Most
    // ways into a system warm it on arrival; this catches the rest.
    for (const auto& sv : sessions_)
        if (SystemState* st = SystemById(sv.second.systemId))
            Warm(*st);
    StepStructures();  // every tick: a site finishes when the clock says, not two seconds on
    Lap(cost ? &cost->structures : nullptr, lap);
    while (maintAccum_ >= MAINT_STEP)
    {
        if (cost != nullptr)
            cost->passes++;
        // Recount the real populations; accumulate losses since the last step as "pressure".
        // A cold system has no ships to count, and its losses are arithmetic.
        for (auto& kv : systems_)
        {
            if (!IsHot(kv.second))
            {
                ColdLosses(kv.second);
                continue;
            }
            SystemAggregate& a = kv.second.agg;
            float prevTr = a.traders, prevMi = a.miners, prevPo = a.police, prevPi = a.pirates;
            RecountAgg(kv.second);
            AccumSuppress(a.supTraders, prevTr - a.traders);
            AccumSuppress(a.supMiners, prevMi - a.miners);
            AccumSuppress(a.supPolice, prevPo - a.police);
            AccumSuppress(a.supPirates, prevPi - a.pirates);
            // What was destroyed, for the faction step: strength a faction committed and
            // lost (#231). A fall in the count is a loss -- ships do not leave a system.
            a.lostPirates += std::max(0.0f, prevPi - a.pirates);
            a.lostPolice += std::max(0.0f, prevPo - a.police);
        }
        Lap(cost ? &cost->recount : nullptr, lap);
        StepWorldMacro();
        Lap(cost ? &cost->macro : nullptr, lap);
        for (auto& kv : systems_)
        {
            // Recovery: the pressure slowly subsides.
            SystemAggregate& a = kv.second.agg;
            a.supTraders *= SUPPRESS_DECAY;
            a.supMiners *= SUPPRESS_DECAY;
            a.supPolice *= SUPPRESS_DECAY;
            a.supPirates *= SUPPRESS_DECAY;
            // Nobody here for long enough: the ships are counted and let go.
            if (IsHot(kv.second) && !allHot_ && time_ >= kv.second.warmUntil)
                Cool(kv.second);
            if (!IsHot(kv.second))
            {
                ColdTopUp(kv.second);
                continue;
            }
            // Player-avoidance, per system: everyone standing in this one.
            std::vector<Vector2> avoid;
            for (const auto& sv : sessions_)
                if (sv.second.systemId == kv.first && sv.second.ship &&
                    sv.second.dockedStationId == 0)
                    avoid.push_back(sv.second.ship->GetPosition());
            TopUpSystem(kv.second, avoid);
        }
        Lap(cost ? &cost->topUp : nullptr, lap);
        maintAccum_ -= MAINT_STEP;
    }
}
