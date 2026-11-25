#include "sim/Simulation.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/Archetype.h"
#include "entities/Ship.h"

// Targeting (#619, documents/combat_and_flight.md 3.1): select, lock, active. A lock is the
// commitment a gun fires at; it takes time by the target's signature, holds one of the ship's
// slots, and is shown to the one locked. The timer is Ships::Lock, shared with the NPCs.

namespace
{
float Dist(Vector2 a, Vector2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}
}  // namespace

bool Simulation::LockAimOf(ClientSession& s, SystemState& st, int id, LockAim& out)
{
    for (const auto& e : st.entities)
    {
        if (e->GetId() != id)
            continue;
        if (e->GetKind() == EntityKind::Npc)
        {
            if (!static_cast<const NpcShip&>(*e).IsAlive())
                return false;
        }
        else if (e->GetKind() != EntityKind::Structure)
            return false;
        else if (e->GetOwner() == s.accountName)
            return false;
        if (HiddenFrom(s, st, *e))
            return false;
        out = { e->GetPosition(), e->GetSize(), e->GetKind() == EntityKind::Structure };
        return true;
    }
    if (const ClientSession* p = PilotTarget(s, st, id))
    {
        out = { p->ship->GetPosition(), p->ship->GetSize(), false };
        return true;
    }
    return false;
}

bool Simulation::LockTarget(ClientSession& s, int id, bool quiet)
{
    auto refuse = [&](const std::string& why)
    {
        if (!quiet)
            s.RecordEvent(Ev::Kind::Notice, "Cannot lock: " + why);
        return false;
    };
    if (!s.ship || s.IsDocked() || !s.ship->IsAlive())
        return false;
    if (id == 0 || id == s.ship->GetId())
        return refuse("pick something else");
    if (s.FindLock(id) != nullptr)
        return true;  // already on its way, or held
    SystemState* st = SystemOf(s);
    LockAim      aim;
    if (st == nullptr || !LockAimOf(s, *st, id, aim))
        return refuse("nothing there to lock");
    const ShipStats& stats = s.ship->GetStats();
    if (Dist(aim.pos, s.ship->GetPosition()) - (aim.structure ? aim.size : 0.0f) > stats.lockRange)
        return refuse("out of lock range");
    if ((int)s.locks.size() >= stats.locks)
        return refuse("every lock is in use (" + std::to_string(stats.locks) + ")");
    const Ships::Rules& rules = Archetypes::ShipCatalogue().rules;
    Ships::Lock         lock;
    lock.Begin(id, Ships::LockTime(rules, aim.size, stats.scanRes));
    s.locks.push_back(lock);
    if (s.activeLock == 0 || s.FindLock(s.activeLock) == nullptr)
        s.activeLock = id;
    return true;
}

void Simulation::Unlock(ClientSession& s, int id)
{
    if (id == 0)
        s.locks.clear();
    else
        s.locks.erase(std::remove_if(s.locks.begin(), s.locks.end(),
                                     [&](const Ships::Lock& l) { return l.target == id; }),
                      s.locks.end());
    if (s.FindLock(s.activeLock) == nullptr)
    {
        // Another held lock takes over, else any; the guns never point at a gone one.
        s.activeLock = 0;
        for (const Ships::Lock& l : s.locks)
            if (l.held || s.activeLock == 0)
                s.activeLock = l.target;
    }
}

bool Simulation::SetActiveLock(ClientSession& s, int id)
{
    if (s.FindLock(id) == nullptr)
        return false;
    s.activeLock = id;
    return true;
}

void Simulation::StepPlayerLocks(ClientSession& s, SystemState& st, float dt)
{
    if (s.locks.empty())
    {
        s.activeLock = 0;
        return;
    }
    if (!s.ship || s.IsDocked() || !s.ship->IsAlive())
    {
        Unlock(s, 0);
        return;
    }
    const float grace = Archetypes::ShipCatalogue().rules.lockGrace;
    const float range = s.ship->GetStats().lockRange;
    bool        lost = false;
    for (Ships::Lock& l : s.locks)
    {
        LockAim aim;
        bool    here = LockAimOf(s, st, l.target, aim);
        // A target that warps off is gone from the system's view as a pilot's would be.
        if (here)
            if (const ClientSession* p = PilotOf(l.target))
                here = !p->ship->IsWarping();
        if (!here)
        {
            l.Clear();
            lost = true;
            continue;
        }
        const bool reachable =
            Dist(aim.pos, s.ship->GetPosition()) - (aim.structure ? aim.size : 0.0f) <= range;
        if (!l.Step(dt, reachable, grace))
            lost = true;
    }
    if (lost)
    {
        s.locks.erase(std::remove_if(s.locks.begin(), s.locks.end(),
                                     [](const Ships::Lock& l) { return l.Empty(); }),
                      s.locks.end());
        Unlock(s, -1);  // no id matches: only the active lock is looked after
    }
}

std::vector<Proto::LockedByView> Simulation::LockedByOf(const ClientSession& s,
                                                        const SystemState&   st) const
{
    std::vector<Proto::LockedByView> out;
    if (!s.ship)
        return out;
    const int me = s.ship->GetId();
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc)
        {
            const NpcShip& n = static_cast<const NpcShip&>(*e);
            if (n.IsAlive() && n.TargetLock().target == me)
                out.push_back({ n.GetId(), n.TargetLock().held });
        }
    for (const auto& kv : sessions_)
    {
        const ClientSession& o = kv.second;
        if (&o == &s || !o.ship || o.systemId != s.systemId || o.IsDocked())
            continue;
        if (const Ships::Lock* l = o.FindLock(me))
            out.push_back({ o.ship->GetId(), l->held });
    }
    return out;
}
