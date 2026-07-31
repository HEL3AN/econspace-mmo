#pragma once

#include "entities/Entity.h"
#include "economy/Resource.h"

// Planet type — affects appearance (color/sprite) and serves as a reference point.
enum class PlanetType { Rocky, Gas, Ice, Lava, Oceanic };

std::string PlanetTypeName(PlanetType type);
Color       PlanetTypeColor(PlanetType type);  // default color for the type

class Planet : public Entity
{
public:
    Planet(float orbitRadius, float orbitSpeed, float angle, float size, Color color,
           ResourceType deposit, PlanetType type);

    void        Update(float dt) override;
    void        Draw() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Planet>(*this); }
    std::string GetName() const override { return "Planet"; }

    ResourceType GetDeposit() const { return deposit_; }
    PlanetType   GetPlanetType() const { return type_; }
    float        GetOrbitRadius() const { return orbitRadius_; }  // for network layout (M4d-3c)

private:
    float        orbitRadius_;
    float        orbitSpeed_;
    float        angle_;
    ResourceType deposit_;  // the planet's subsurface resource (data from system.json)
    PlanetType   type_;
};
