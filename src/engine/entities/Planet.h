#pragma once

#include "entities/Entity.h"
#include "economy/Resource.h"

// Planet type — affects appearance (color/sprite) and serves as a reference point.
enum class PlanetType
{
    Rocky,
    Gas,
    Ice,
    Lava,
    Oceanic
};

std::string PlanetTypeName(PlanetType type);
// Parsing lives beside naming: both are the mapping between the enum and the world file,
// and having only one of them public is how a second copy of the other gets written.
// Falls back to Rocky; ParsePlanetType says when it had to (#191).
PlanetType PlanetTypeFromString(const std::string& s);
bool       ParsePlanetType(const std::string& s, PlanetType& out);
Color      PlanetTypeColor(PlanetType type);  // default color for the type

class Planet : public Entity
{
public:
    Planet(float orbitRadius, float orbitSpeed, float angle, float size, Color color,
           ResourceType deposit, PlanetType type);

    // Where the planet is at a world time (#210). A function of the clock, not a sum of
    // ticks: a planet in a system nobody was in for a week is where it would have got to,
    // and a satellite asks the same function and so cannot fall behind its planet (#136).
    Vector2 PositionAt(double time) const;
    void    SetClock(double time) { pos_ = PositionAt(time); }

    Render::Item            Describe() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Planet>(*this); }
    std::string             GetName() const override { return "Planet"; }

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
