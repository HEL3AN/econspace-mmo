#include "core/Orbits.h"

#include "entities/Planet.h"
#include <cmath>

namespace Orbits
{
Vector2 SatellitePosition(const Orbit& o, Vector2 planet, double time)
{
    double a = o.phase;
    if (o.radius > 0.0f)
        a += (double)o.speed / o.radius * time;
    a = std::fmod(a, 2.0 * PI);  // in double, for the reason Planet::PositionAt gives
    return { planet.x + cosf((float)a) * o.radius, planet.y + sinf((float)a) * o.radius };
}

void Place(std::vector<std::unique_ptr<Entity>>& entities, double time)
{
    std::vector<const Planet*> planets;
    for (auto& e : entities)
        if (e->GetKind() == EntityKind::Planet)
        {
            Planet* p = static_cast<Planet*>(e.get());
            p->SetClock(time);
            planets.push_back(p);
        }
    for (auto& e : entities)
    {
        const std::optional<Orbit>& o = e->GetOrbit();
        if (o && o->planet >= 0 && o->planet < (int)planets.size())
            e->SetPosition(SatellitePosition(*o, planets[o->planet]->GetPosition(), time));
    }
}
}  // namespace Orbits
