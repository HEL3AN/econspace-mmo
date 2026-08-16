#include "entities/Planet.h"
#include "render/Textures.h"
#include <cmath>

std::string PlanetTypeName(PlanetType type)
{
    switch (type)
    {
        case PlanetType::Rocky: return "Rocky";
        case PlanetType::Gas: return "Gas Giant";
        case PlanetType::Ice: return "Ice";
        case PlanetType::Lava: return "Lava";
        case PlanetType::Oceanic: return "Oceanic";
    }
    return "Planet";
}

Color PlanetTypeColor(PlanetType type)
{
    switch (type)
    {
        case PlanetType::Rocky: return Color{ 150, 130, 110, 255 };   // brown rock
        case PlanetType::Gas: return Color{ 210, 170, 110, 255 };     // sandy orange
        case PlanetType::Ice: return Color{ 180, 210, 230, 255 };     // pale blue
        case PlanetType::Lava: return Color{ 200, 80, 50, 255 };      // molten
        case PlanetType::Oceanic: return Color{ 70, 130, 200, 255 };  // ocean blue
    }
    return Color{ 150, 150, 150, 255 };
}

// The sprite name used to live here, one switch per type. It is now a field on the
// archetype, which is what lets a planet a player invents have a sprite at all.
static const char* ArchetypeIdForPlanetType(PlanetType type)
{
    switch (type)
    {
        case PlanetType::Rocky: return "planet.rocky";
        case PlanetType::Gas: return "planet.gas";
        case PlanetType::Ice: return "planet.ice";
        case PlanetType::Lava: return "planet.lava";
        case PlanetType::Oceanic: return "planet.oceanic";
    }
    return "planet.rocky";
}

Planet::Planet(float orbitRadius, float orbitSpeed, float angle, float size, Color color,
               ResourceType deposit, PlanetType type)
    : Entity({ cosf(angle) * orbitRadius, sinf(angle) * orbitRadius }, size, color,
             EntityKind::Planet),
      orbitRadius_(orbitRadius), orbitSpeed_(orbitSpeed), angle_(angle), deposit_(deposit),
      type_(type)
{
    SetArchetype(Archetypes::Find(ArchetypeIdForPlanetType(type)));
}

Render::Item Planet::Describe() const
{
    Render::Item it = Entity::Describe();
    it.ring = orbitRadius_;  // the orbit guide; only a planet knows it has one
    return it;
}

void Planet::Update(float dt)
{
    float angularSpeed = orbitSpeed_ / orbitRadius_;
    angle_ += angularSpeed * dt;
    pos_ = { cosf(angle_) * orbitRadius_, sinf(angle_) * orbitRadius_ };
}
