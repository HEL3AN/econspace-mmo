// Construction (#39): laying down a site, finishing it, and its end. One translation unit of
// Simulation (#17).
//
// Everything a player builds goes through AddStatic / RemoveStatic, so it reaches everyone
// in the system as a LayoutDelta and outlives a restart (#38) without anything here knowing
// how. What this file decides is only the rules: what it costs, where it may stand, how many
// one player may have, and when it is done.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "sim/Names.h"

#include "core/Blueprint.h"
#include "core/World.h"
#include "entities/Planet.h"
#include "entities/Ship.h"
#include "entities/Structure.h"

#include <algorithm>
#include <cmath>

namespace
{
float Distance(Vector2 a, Vector2 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

std::string Whole(float v)
{
    return std::to_string((long long)std::lround(v));
}
}  // namespace

int Simulation::StructuresOwnedBy(const std::string& account) const
{
    int n = 0;
    for (const auto& kv : systems_)
        for (const auto& e : kv.second.entities)
            if (e->GetKind() == EntityKind::Structure && e->GetOwner() == account)
                n++;
    return n;
}

std::string Simulation::PlacementProblem(const ClientSession& s, const Blueprint& bp,
                                         Vector2 at) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.ship)
        return "there is no ship to build from";
    if (s.accountName.empty())
        return "only an account can own a structure";
    if (s.IsDocked())
        return "undock first: a site is laid down from space";
    if (s.ship->GetWarpPhase() != WarpPhase::None)
        return "not while warping";

    const float fromShip = Distance(at, s.ship->GetPosition());
    if (fromShip > bp.reach)
        return "too far from the ship (" + Whole(fromShip) + " of at most " + Whole(bp.reach) + ")";
    if (Distance(at, Vector2{ 0.0f, 0.0f }) > World::SYSTEM_RADIUS)
        return "outside the system";

    const Archetype* a = Archetypes::Find(bp.archetype);
    const float      own = a != nullptr ? a->defaultSize : 0.0f;
    for (const auto& e : st->entities)
    {
        const float d = Distance(at, e->GetPosition());
        switch (e->GetKind())
        {
            // Not inside a body, nor where one will be: a planet comes round its orbit and a
            // satellite round its planet, and a structure standing in the path would be
            // swept through on the next pass (#210). A rule, not a check of where it is now.
            case EntityKind::Star:
                if (d < e->GetSize() + bp.bodyClearance)
                    return "too close to the star";
                break;
            case EntityKind::Planet:
            {
                const float path = static_cast<const Planet&>(*e).GetOrbitRadius();
                const float fromCentre = Distance(at, Vector2{ 0.0f, 0.0f });
                if (std::fabs(fromCentre - path) < e->GetSize() + bp.bodyClearance)
                    return "in the path of " + e->GetName();
                break;
            }
            // Room around what others use: a site on a dock or across a gate is in the way
            // of everyone who flies there (#41 names it griefing; this is its first rule).
            case EntityKind::Station:
            case EntityKind::Gate:
            case EntityKind::Derelict:
            case EntityKind::Structure:
                if (!e->GetOrbit() && d < e->GetSize() + own + bp.clearance)
                    return "too close to " + e->GetName() + " (keep " + Whole(bp.clearance) +
                           " clear)";
                break;
            // A belt or a cloud is a place, and marking one is what a buoy is for.
            case EntityKind::Field:
            case EntityKind::Nebula:
            case EntityKind::Npc:
            case EntityKind::PlayerShip:
            case EntityKind::Unknown: break;
        }

        // A satellite's path: the ring its planet's path is widened into by its own orbit.
        if (const std::optional<Orbit>& o = e->GetOrbit())
        {
            float planetPath = 0.0f;
            int   index = 0;
            for (const auto& p : st->entities)
                if (p->GetKind() == EntityKind::Planet && index++ == o->planet)
                    planetPath = static_cast<const Planet&>(*p).GetOrbitRadius();
            const float fromCentre = Distance(at, Vector2{ 0.0f, 0.0f });
            if (std::fabs(fromCentre - planetPath) <
                o->radius + e->GetSize() + own + std::max(bp.clearance, bp.bodyClearance))
                return "in the path of " + e->GetName();
        }
    }

    if (bp.perSystem > 0)
    {
        int here = 0;
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Structure &&
                static_cast<const Structure&>(*e).GetBuilds() == bp.archetype)
                here++;
        if (here >= bp.perSystem)
            return "this system already has " + std::to_string(here) + " of them";
    }
    const int cap = Blueprints::PerAccount();
    if (cap > 0 && StructuresOwnedBy(s.accountName) >= cap)
        return "you already have " + std::to_string(cap) + " structures standing";

    for (const auto& c : bp.cost)
        if (s.ship->GetCargoAmount(c.first) < c.second)
            return "the hold has " + std::to_string(s.ship->GetCargoAmount(c.first)) + " " +
                   ResourceName(c.first) + " of the " + std::to_string(c.second) + " it takes";
    return std::string();
}

int Simulation::Deploy(ClientSession& s, const std::string& blueprint, Vector2 at,
                       const std::string& name)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot build: " + why);
        return 0;
    };
    const Blueprint* bp = Blueprints::Find(blueprint);
    if (bp == nullptr)
        return refuse("there is no blueprint '" + blueprint + "'");
    std::string       why;
    const std::string called = name.empty() ? bp->name : name;
    if (!name.empty() && !Names::ValidSystemName(name, why))
        return refuse(why);
    why = PlacementProblem(s, *bp, at);
    if (!why.empty())
        return refuse(why);

    // Committed in full when the site goes down. Interrupting a site (the next slice) decides
    // what comes back; nothing does yet.
    for (const auto& c : bp->cost)
        s.ship->RemoveCargo(c.first, c.second);

    auto site = std::make_unique<Structure>(at, 0.0f, called, bp->archetype);
    site->StartBuilding(time_, time_ + bp->buildSeconds);
    if (bp->lifetime > 0.0f)
        site->SetExpiresAt(time_ + bp->buildSeconds + bp->lifetime);
    const int id = AddStatic(s.systemId, std::move(site), s.accountName);
    if (id == 0)
        return refuse("the system would not take it");
    s.RecordEvent(Ev::Kind::Notice,
                  "Building " + called + ": ready in " + Whole(bp->buildSeconds) + " s");
    return id;
}

void Simulation::StepStructures()
{
    auto tell = [&](const std::string& owner, Ev::Kind kind, const std::string& text)
    {
        for (auto& kv : sessions_)
            if (kv.second.accountName == owner)
                kv.second.RecordEvent(kind, text);
    };

    for (auto& kv : systems_)
    {
        SystemState&     st = kv.second;
        std::vector<int> expired;
        for (const auto& e : st.entities)
        {
            if (e->GetKind() != EntityKind::Structure)
                continue;
            Structure& t = static_cast<Structure&>(*e);
            // The instants were fixed when the site went down; this only notices them pass.
            // A server that was down at the moment finishes it on its first tick back.
            if (t.IsBuilding() && time_ >= t.GetCompletesAt())
            {
                t.Complete();
                MarkStaticChanged(st, t.GetId());
                tell(t.GetOwner(), Ev::Kind::Built,
                     t.GetName() + " is built in " + SystemName(kv.first));
            }
            if (t.GetExpiresAt() > 0.0 && time_ >= t.GetExpiresAt())
                expired.push_back(t.GetId());
        }
        // Outside the pass: removing one moves the others.
        for (int id : expired)
        {
            const Entity* e = nullptr;
            for (const auto& x : st.entities)
                if (x->GetId() == id)
                    e = x.get();
            const std::string owner = e != nullptr ? e->GetOwner() : std::string();
            const std::string what = e != nullptr ? e->GetName() : std::string("A structure");
            if (RemoveStatic(kv.first, id))
                tell(owner, Ev::Kind::Notice,
                     what + " in " + SystemName(kv.first) + " has run its course and is gone");
        }
    }
}
