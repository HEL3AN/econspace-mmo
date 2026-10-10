// Where a faction's outpost stands (#318). One translation unit of Simulation (#17).
//
// The place follows the purpose, and the purpose is what the faction came for: a mining
// outpost goes up beside the richest belt, a watch post on the way traffic takes between the
// gates -- touching none of them -- an orbital outpost round the planet worth holding, a
// salvage post by the thickest cluster of wrecks, and a lawless faction's base where nobody
// passes: in a cloud, or out at the edge, away from the gate lines. With no purpose in
// particular it is an open point, never the gate.
//
// Every spot is one SpotProblem allows, the rule a player's site is held to (#39). A spot
// beside something that moves is a satellite of what it moves with (#210): its place is then
// the clock's, like every other moon's. Randomness comes from Gen::Rng keyed by who, where
// and what for -- never by when or by order -- so the same world settles the same places.

#include "sim/Simulation.h"

#include "core/Blueprint.h"
#include "core/Orbits.h"
#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/JumpGate.h"
#include "entities/Planet.h"
#include "entities/Structure.h"
#include "gen/Region.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
// What keys an outpost's randomness, besides who, where and what for.
constexpr uint64_t OUTPOST_KEY = 0x0F7905u;
// This many places are tried for a purpose before it is given up for an open point.
constexpr int TRIES = 24;
// Beside a belt or a wreck: this far beyond its edge, and a little further on every try.
constexpr float BESIDE = 4000.0f;
// A watch post watches the way, not the gate: at least this far from any gate's edge. An
// outpost with no purpose keeps further still, so nobody mistakes it for the gate's.
constexpr float WATCH_FROM_GATE = 40000.0f;
constexpr float OPEN_FROM_GATE = 60000.0f;
// An open point: anywhere between these shares of the system's radius.
constexpr float OPEN_INNER = 0.15f, OPEN_OUTER = 0.7f;
// Out at the edge, where a hidden base goes when there is no cloud to hide in.
constexpr float EDGE_INNER = 0.8f, EDGE_OUTER = 0.95f;
// Wrecks within this of each other are one field of them; a salvage post goes by the
// thickest.
constexpr float SALVAGE_FIELD = 60000.0f;
// A moon's orbit, as the generator makes them (gen/Region.cpp): the first this far off the
// planet's surface, each next one this far past the last, at this speed along its circle.
constexpr float MOON_FIRST = 4000.0f, MOON_GAP = 2000.0f;
constexpr int   MOON_SPEED_MIN = 20, MOON_SPEED_MAX = 50;

float Distance(Vector2 a, Vector2 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// How far `p` is from the segment a-b.
float ToSegment(Vector2 p, Vector2 a, Vector2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float       t = len2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return Distance(p, Vector2{ a.x + dx * t, a.y + dy * t });
}

Vector2 Toward(Vector2 from, double turn, float r)
{
    return Vector2{ from.x + r * (float)std::cos(turn), from.y + r * (float)std::sin(turn) };
}

double Turn(Gen::Rng& rng)
{
    return rng.Unit() * 2.0 * PI;
}
}  // namespace

Simulation::OutpostSpot Simulation::FindOutpostSpot(FactionId f, Outposts::Purpose purpose,
                                                    const std::string& from,
                                                    const std::string& systemId) const
{
    using Outposts::Purpose;
    OutpostSpot        spot;
    const SystemState* st = SystemById(systemId);
    const Blueprint*   bp = Blueprints::Find(Outposts::BlueprintOf(purpose));
    if (st == nullptr || bp == nullptr)
        return spot;
    const Archetype* arch = Archetypes::Find(bp->archetype);
    const float      own = arch != nullptr ? arch->defaultSize : 0.0f;
    Gen::Rng         rng(
        Gen::Key(OUTPOST_KEY, (uint64_t)f, Intelligence::KeyOf(systemId), (uint64_t)purpose));

    std::vector<const Entity*>        gates, wrecks, clouds;
    std::vector<const AsteroidField*> belts;
    std::vector<const Planet*>        planets;  // in file order: what an orbit's index names
    const Entity*                     arrival = nullptr;
    for (const auto& e : st->entities)
        switch (e->GetKind())
        {
            case EntityKind::Gate:
                gates.push_back(e.get());
                if (static_cast<const JumpGate&>(*e).GetDestination() == from)
                    arrival = e.get();
                break;
            case EntityKind::Field:
                belts.push_back(static_cast<const AsteroidField*>(e.get()));
                break;
            case EntityKind::Derelict: wrecks.push_back(e.get()); break;
            case EntityKind::Nebula:
                if (!e->GetOrbit())  // a cloud that moves would leave the base behind
                    clouds.push_back(e.get());
                break;
            case EntityKind::Planet: planets.push_back(static_cast<const Planet*>(e.get())); break;
            default: break;
        }

    auto take = [&](Vector2 at)
    {
        if (!SpotProblem(*st, *bp, at).empty())
            return false;
        spot.found = true;
        spot.at = at;
        return true;
    };
    auto clearOfGates = [&](Vector2 at, float gap)
    {
        for (const Entity* g : gates)
            if (Distance(at, g->GetPosition()) < g->GetSize() + gap)
                return false;
        return true;
    };
    // Round something that stands still, from `reach` out, further on every try.
    auto beside = [&](Vector2 centre, float reach)
    {
        for (int i = 0; i < TRIES; i++)
            if (take(Toward(centre, Turn(rng), reach + BESIDE * 0.5f * (float)i)))
                return true;
        return false;
    };
    // The next free orbit round planet `index`, as the generator would give it a moon: past
    // every satellite it has, within the reach of its path, and with nothing that stands
    // still in the band the satellite sweeps along with its planet.
    auto moon = [&](int index)
    {
        if (index < 0 || index >= (int)planets.size())
            return false;
        const Planet& p = *planets[(size_t)index];
        float         next = p.GetSize() + MOON_FIRST;
        int           here = 0;
        for (const auto& e : st->entities)
        {
            if (const std::optional<Orbit>& o = e->GetOrbit(); o && o->planet == index)
                next = std::max(next, o->radius + e->GetSize() + MOON_GAP);
            if (e->GetKind() == EntityKind::Structure &&
                static_cast<const Structure&>(*e).GetBuilds() == bp->archetype)
                here++;
        }
        const float radius = next + own;
        if (radius + own > (float)Gen::MoonZone(p.GetSize()) ||
            (bp->perSystem > 0 && here >= bp->perSystem))
            return false;
        for (const auto& e : st->entities)
        {
            if (e->GetOrbit())
                continue;
            switch (e->GetKind())
            {
                case EntityKind::Star:
                case EntityKind::Station:
                case EntityKind::Gate:
                case EntityKind::Derelict:
                case EntityKind::Structure:
                {
                    const float fromCentre = Distance(e->GetPosition(), Vector2{ 0.0f, 0.0f });
                    if (std::fabs(fromCentre - p.GetOrbitRadius()) <
                        radius + own + e->GetSize() + bp->clearance)
                        return false;
                    break;
                }
                default: break;
            }
        }
        Orbit o;
        o.planet = index;
        o.radius = radius;
        o.speed = (float)rng.Range(MOON_SPEED_MIN, MOON_SPEED_MAX);
        o.phase = (float)Turn(rng);
        spot.found = true;
        spot.orbit = o;
        spot.at = Orbits::SatellitePosition(o, p.PositionAt(time_), time_);
        return true;
    };

    switch (purpose)
    {
        case Purpose::Mining:
        {
            // The richest by what it was made with, which mining does not change.
            const AsteroidField* best = nullptr;
            for (const AsteroidField* b : belts)
                if (best == nullptr || b->GetOreMax() > best->GetOreMax())
                    best = b;
            if (best == nullptr)
                return spot;
            if (best->GetOrbit())
                moon(best->GetOrbit()->planet);
            else
                beside(best->GetPosition(), best->GetSize() + own + BESIDE);
            return spot;
        }
        case Purpose::Salvage:
        {
            // By the wreck with the most others near it: the field, not a stray.
            const Entity* best = nullptr;
            int           most = -1;
            for (const Entity* w : wrecks)
            {
                int near = 0;
                for (const Entity* o : wrecks)
                    near += o != w && Distance(o->GetPosition(), w->GetPosition()) < SALVAGE_FIELD;
                if (near > most)
                {
                    most = near;
                    best = w;
                }
            }
            if (best == nullptr)
                return spot;
            if (best->GetOrbit())
                moon(best->GetOrbit()->planet);
            else
                beside(best->GetPosition(), best->GetSize() + own + bp->clearance + BESIDE);
            return spot;
        }
        case Purpose::Orbital:
        {
            // The largest planet is the one worth holding; failing room there, the next.
            std::vector<int> order;
            for (int i = 0; i < (int)planets.size(); i++)
                order.push_back(i);
            std::stable_sort(
                order.begin(), order.end(), [&](int a, int b)
                { return planets[(size_t)a]->GetSize() > planets[(size_t)b]->GetSize(); });
            for (int i : order)
                if (moon(i))
                    break;
            return spot;
        }
        case Purpose::Watch:
        {
            if (gates.empty())
                return spot;
            Vector2 middle{ 0.0f, 0.0f };
            for (const Entity* g : gates)
                middle = Vector2{ middle.x + g->GetPosition().x / (float)gates.size(),
                                  middle.y + g->GetPosition().y / (float)gates.size() };
            const Entity* a = arrival != nullptr ? arrival : gates.front();
            for (int i = 0; i < TRIES * 2; i++)
            {
                Vector2 at;
                if (gates.size() >= 3 && i < TRIES / 2)
                    at = Toward(middle, Turn(rng), 6000.0f * (float)i);  // the junction
                else if (gates.size() >= 2)
                {
                    // On the way from the gate it came by to one of the others.
                    std::vector<const Entity*> others;
                    for (const Entity* g : gates)
                        if (g != a)
                            others.push_back(g);
                    const Entity* b = others[(size_t)rng.Range(0, (int)others.size() - 1)];
                    const Vector2 pa = a->GetPosition(), pb = b->GetPosition();
                    const float   t = (float)rng.Between(0.3, 0.7);
                    const float   dx = pb.x - pa.x, dy = pb.y - pa.y;
                    const float   len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
                    const float   side = (float)rng.Between(-1.0, 1.0) * 20000.0f;
                    at =
                        Vector2{ pa.x + dx * t - dy / len * side, pa.y + dy * t + dx / len * side };
                }
                else
                {
                    // A dead end: on the way in from its one gate.
                    const Vector2 g = a->GetPosition();
                    const float   t = (float)rng.Between(0.2, 0.5);
                    at = Toward(Vector2{ g.x * (1.0f - t), g.y * (1.0f - t) }, Turn(rng),
                                (float)rng.Between(0.0, 20000.0));
                }
                if (clearOfGates(at, WATCH_FROM_GATE) && take(at))
                    return spot;
            }
            return spot;
        }
        case Purpose::Hidden:
        {
            // In the largest cloud, if there is one to hide in.
            const Entity* cloud = nullptr;
            for (const Entity* c : clouds)
                if (cloud == nullptr || c->GetSize() > cloud->GetSize())
                    cloud = c;
            if (cloud != nullptr)
                for (int i = 0; i < TRIES; i++)
                    if (take(Toward(cloud->GetPosition(), Turn(rng),
                                    (float)rng.Unit() * 0.6f * cloud->GetSize())))
                        return spot;
            // Otherwise out at the edge, as far from the lines traffic flies -- gate to gate,
            // and gate to the star -- as any of a few tries.
            std::vector<std::pair<float, Vector2>> tries;
            for (int i = 0; i < TRIES; i++)
            {
                const Vector2 at =
                    Toward(Vector2{ 0.0f, 0.0f }, Turn(rng),
                           World::SYSTEM_RADIUS * (float)rng.Between(EDGE_INNER, EDGE_OUTER));
                float off = World::SYSTEM_RADIUS * 2.0f;
                for (size_t g = 0; g < gates.size(); g++)
                {
                    off = std::min(off, ToSegment(at, gates[g]->GetPosition(), Vector2{ 0, 0 }));
                    for (size_t h = g + 1; h < gates.size(); h++)
                        off = std::min(
                            off, ToSegment(at, gates[g]->GetPosition(), gates[h]->GetPosition()));
                }
                tries.push_back({ off, at });
            }
            std::stable_sort(tries.begin(), tries.end(),
                             [](const auto& x, const auto& y) { return x.first > y.first; });
            for (const auto& t : tries)
                if (take(t.second))
                    return spot;
            return spot;
        }
        case Purpose::Open:
            for (int i = 0; i < TRIES * 2; i++)
            {
                const Vector2 at =
                    Toward(Vector2{ 0.0f, 0.0f }, Turn(rng),
                           World::SYSTEM_RADIUS * (float)rng.Between(OPEN_INNER, OPEN_OUTER));
                if (clearOfGates(at, OPEN_FROM_GATE) && take(at))
                    return spot;
            }
            return spot;
    }
    return spot;
}
