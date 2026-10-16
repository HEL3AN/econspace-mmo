// Ships that leave (#295, the owner's decision of 2026-10-11). One translation unit of
// Simulation (#17).
//
// The spawn director tops a system up to what it keeps and never takes a ship away, so a
// system that holds more than it keeps -- its security fell, its garrison was lent to a siege,
// its holder changed -- used to keep the difference until something destroyed it. Now the
// difference leaves: at most one ship of a role per system every few minutes, through the gate
// to the neighbour that lacks the most of that role. A system that lacks ships a neighbour has
// to spare does not have them made; it waits for them to come.
//
// The same rule, read off the same numbers in the same pass, cold or hot. Where nobody is it
// is a number taken off one aggregate and, `travel` seconds later, added to another. Where
// somebody is, a real ship turns for the gate and goes; where somebody is at the other end,
// it comes in through the gate there. When a ship leaves is drawn from what is decided --
// that system, that role, that stretch of the world's clock -- never from what came before.
#include "sim/Simulation.h"

#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
// The coarse pass migration is stepped by: MaintainWorld's two seconds.
constexpr double PASS_SECONDS = 2.0;
// What keys when a ship leaves, besides where, which role and when.
constexpr uint64_t MIGRATE_KEY = 0x316E7u;
// A system has a ship to spare when it holds a whole one more than it keeps -- a cold
// system's numbers are fractions, a hot one's whole ships -- and lacks one when it is short
// by any of one: the director, which would have made it up, is waiting for it to come.
constexpr float WHOLE = 0.999f;
constexpr float SHORT = 0.01f;
// The roles that come and go: NpcRole's first four. A siege's warships come by their plan.
constexpr int ROLES = 4;

float Interval(int role)
{
    const Migration& m = Factions::MigrationRules();
    switch ((NpcRole)role)
    {
        case NpcRole::Trader: return m.trader;
        case NpcRole::Miner: return m.miner;
        case NpcRole::Police: return m.patrol;
        case NpcRole::Pirate: return m.pirate;
        case NpcRole::Warship: break;
    }
    return 0.0f;
}

float& CountOf(SystemAggregate& a, int role)
{
    switch ((NpcRole)role)
    {
        case NpcRole::Miner: return a.miners;
        case NpcRole::Police: return a.police;
        case NpcRole::Pirate: return a.pirates;
        default: break;
    }
    return a.traders;
}
}  // namespace

int Simulation::KeepOf(const std::string& systemId, NpcRole role) const
{
    const SystemState* st = SystemById(systemId);
    if (st == nullptr || role == NpcRole::Warship)
        return 0;
    return PopulationOf(*st, RoomOf(*st)).keep[(int)role];
}

void Simulation::StepMigration()
{
    // What each system has to spare and lacks, by role, as of this pass.
    passTargets_.clear();
    passTargets_.reserve(systems_.size());
    for (auto& kv : systems_)
    {
        SystemState& st = kv.second;
        passTargets_.push_back(PopulationOf(st, RoomOf(st)));
        const Population& p = passTargets_.back();
        const int         sent[ROLES] = { p.traders, p.miners, p.police, p.pirates };
        for (int r = 0; r < ROLES; r++)
        {
            const float have = CountOf(st.agg, r);
            st.spare[r] = have - (float)p.keep[r];
            st.lack[r] = (float)sent[r] - have;
        }
    }
    // Those on their way in count where they are going: a system they are bound for lacks
    // that many fewer.
    for (const auto& kv : migrants_)
        if (SystemState* to = SystemById(kv.second.to))
            to->lack[(int)kv.second.role] -= 1.0f;
    // A system next to one with a ship to spare of a role waits for it rather than have the
    // director make one.
    for (auto& kv : systems_)
    {
        kv.second.fromNextDoor.fill(false);
        for (const std::string& n : Neighbors(kv.first))
            if (const SystemState* next = SystemById(n))
                for (int r = 0; r < ROLES; r++)
                    kv.second.fromNextDoor[r] = kv.second.fromNextDoor[r] ||
                                                (next->spare[r] >= WHOLE && Interval(r) > 0.0f);
    }

    // A ship on its way to the gate goes through it once it is there, or once its time is up
    // wherever it is -- unless somebody shot it first, and then it is a ship lost, not one on
    // its way.
    for (auto it = migrants_.begin(); it != migrants_.end();)
    {
        Migrant& m = it->second;
        if (m.shipId == 0)
        {
            ++it;
            continue;
        }
        SystemState*   st = SystemById(m.from);
        const NpcShip* n = nullptr;
        if (st != nullptr)
            for (const auto& e : st->entities)
                if (e->GetId() == m.shipId && e->GetKind() == EntityKind::Npc)
                    n = static_cast<const NpcShip*>(e.get());
        const bool alive = n != nullptr && n->IsAlive();
        if (alive && !n->AtGate() && time_ < m.jumpAt)
        {
            ++it;
            continue;
        }
        // Shot down on its way out: what it carried is in its wreck (#343).
        if (n != nullptr && !alive)
            LeaveCargoWreck(*st, *n);
        if (n != nullptr)
        {
            const int id = m.shipId;
            st->entities.erase(std::remove_if(st->entities.begin(), st->entities.end(),
                                              [&](const std::unique_ptr<Entity>& e)
                                              { return e->GetId() == id; }),
                               st->entities.end());
        }
        m.shipId = 0;
        if (alive)
        {
            ++it;
            continue;
        }
        if (st != nullptr && m.role == NpcRole::Pirate)
            st->agg.lostPirates += 1.0f;
        if (st != nullptr && m.role == NpcRole::Police)
            st->agg.lostPolice += 1.0f;
        arrivals_.erase({ m.arrivesAt, m.id });
        it = migrants_.erase(it);
    }

    // Who leaves this pass: in each system, for each role, one moment in each stretch of the
    // clock as long as that role's interval -- if the system has a whole ship to spare then
    // and a neighbour lacks one.
    size_t nth = 0;
    for (auto& kv : systems_)
    {
        const Population&         flags = passTargets_[nth++];
        SystemState&              st = kv.second;
        std::array<float, ROLES>& spare = st.spare;
        for (int r = 0; r < ROLES; r++)
        {
            const double every = Interval(r);
            if (every <= 0.0 || spare[r] < WHOLE)
                continue;
            const double slot = std::floor(time_ / every);
            Gen::Rng     rng(
                Gen::Key(MIGRATE_KEY, Intelligence::KeyOf(kv.first), (uint64_t)r, (uint64_t)slot));
            const double at = slot * every + rng.Unit() * every;
            if (at > time_ || at <= time_ - PASS_SECONDS)
                continue;
            // Where it goes: the neighbour that lacks the most, the first of them on a tie.
            std::string to;
            float       most = 0.0f;
            for (const std::string& n : Neighbors(kv.first))
            {
                const SystemState* next = SystemById(n);
                if (next != nullptr && next->lack[r] > SHORT && next->lack[r] > most)
                {
                    most = next->lack[r];
                    to = n;
                }
            }
            if (to.empty())
                continue;

            // A trader carries what pays on the way (#343): it is going anyway.
            Haul haul;
            if ((NpcRole)r == NpcRole::Trader)
                haul = BestHaul(st, to);
            const FactionId under = (NpcRole)r == NpcRole::Pirate   ? Factions::Pirates()
                                    : (NpcRole)r == NpcRole::Miner  ? Factions::Unaffiliated()
                                    : (NpcRole)r == NpcRole::Police ? flags.policeFaction
                                                                    : flags.tradeFaction;
            SetOut(st, to, (NpcRole)r, under, haul);
        }
    }

    // Haulers (#343): a trader sets out with goods for next door where the price there pays.
    StepHauls();

    // Who has arrived.
    while (!arrivals_.empty() && arrivals_.begin()->first <= time_)
    {
        const long long id = arrivals_.begin()->second;
        arrivals_.erase(arrivals_.begin());
        const auto it = migrants_.find(id);
        if (it == migrants_.end())
            continue;
        Migrant m = it->second;
        migrants_.erase(it);
        if (m.shipId != 0)
        {
            // Still turning for the gate when it should be through: it is through now.
            if (SystemState* from = SystemById(m.from))
                from->entities.erase(std::remove_if(from->entities.begin(), from->entities.end(),
                                                    [&](const std::unique_ptr<Entity>& e)
                                                    { return e->GetId() == m.shipId; }),
                                     from->entities.end());
        }
        ArriveMigrant(m);
    }
}

bool Simulation::SetOut(SystemState& st, const std::string& to, NpcRole role, FactionId faction,
                        const Haul& haul)
{
    const Migration& rules = Factions::MigrationRules();
    Migrant          m;
    m.from = st.id;
    m.to = to;
    m.role = role;
    m.faction = faction;  // under the flag the director would have sent it under
    m.leftAt = time_;
    m.arrivesAt = time_ + rules.travel;
    NpcShip* ship = nullptr;
    if (IsHot(st))
    {
        // A real one turns for the gate: the one of its role nearest to it -- not a hauler that
        // has yet to unload here.
        Vector2 gate{ 0.0f, 0.0f };
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Gate &&
                static_cast<const JumpGate&>(*e).GetDestination() == to)
                gate = e->GetPosition();
        float nearest = 0.0f;
        for (const auto& e : st.entities)
        {
            if (e->GetKind() != EntityKind::Npc)
                continue;
            NpcShip& n = static_cast<NpcShip&>(*e);
            if (n.GetRole() != role || n.IsLeaving() || !n.IsAlive() || n.GetTask() != 0 ||
                n.GetCargoAmount() > 0)
                continue;
            const float dx = n.GetPosition().x - gate.x, dy = n.GetPosition().y - gate.y;
            const float d = dx * dx + dy * dy;
            if (ship == nullptr || d < nearest)
            {
                ship = &n;
                nearest = d;
            }
        }
        if (ship == nullptr)
            return false;
        ship->Leave(gate);
        m.faction = ship->GetFaction();
        m.shipId = ship->GetId();
        m.jumpAt = time_ + rules.align;
    }
    // What it carries leaves the market now and is counted nowhere until it arrives, as the
    // ship is not (#295).
    if (haul.amount > 0)
    {
        int bought = 0;
        st.market.Buy(haul.good, haul.amount, bought);
        m.good = haul.good;
        m.cargo = bought;
        if (ship != nullptr)
            ship->SetCargo(haul.good, bought);  // shot down on the way out, it leaves it there
    }
    m.id = nextMigrant_++;
    float& count = CountOf(st.agg, (int)role);
    count = std::max(0.0f, count - 1.0f);
    st.spare[(int)role] -= 1.0f;
    if (SystemState* dest = SystemById(to))
        dest->lack[(int)role] -= 1.0f;
    migrants_[m.id] = m;
    arrivals_.insert({ m.arrivesAt, m.id });
    return true;
}

void Simulation::ArriveMigrant(const Migrant& m)
{
    SystemState* st = SystemById(m.to);
    if (st == nullptr)
        return;
    if (!IsHot(*st))
    {
        CountOf(st->agg, (int)m.role) += 1.0f;
        // Nobody to see it fly to a station: it has sold what it carried.
        if (m.cargo > 0)
            Unload(*st, m.good, m.cargo);
        return;
    }
    // Somebody is there: it comes in through the gate from where it set out -- with its hold
    // full, if it is a hauler, flying to a market here to unload.
    SpawnRoleAt(*st, m.role, m.faction, ArrivalFrom(st->id, m.from));
    if (m.cargo > 0 && !st->entities.empty() && st->entities.back()->GetKind() == EntityKind::Npc)
        LoadArrival(*st, static_cast<NpcShip&>(*st->entities.back()), m.good, m.cargo);
}

void Simulation::SpawnRoleAt(SystemState& st, NpcRole role, FactionId faction, Vector2 at)
{
    // The work it is given is the work the director gives a ship of its role here.
    const SpawnNodes     nd = GatherNodes(st);
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    std::vector<Vector2> route;
    switch (role)
    {
        case NpcRole::Trader: route = lanes; break;
        case NpcRole::Miner:
            if (!nd.fields.empty())
                route = { nd.fields[(size_t)RandRange(0, (int)nd.fields.size() - 1)] };
            break;
        case NpcRole::Police:
            route = lanes;
            route.insert(route.end(), nd.fields.begin(), nd.fields.end());
            break;
        case NpcRole::Pirate:
        {
            const std::vector<Vector2> dark = PirateSpots(nd);
            if (!dark.empty())
                route = { dark[(size_t)RandRange(0, (int)dark.size() - 1)] };
            break;
        }
        case NpcRole::Warship: break;
    }
    if (route.empty())
        route = { at };
    SpawnNpcInto(st, at, faction, role, route);
}
