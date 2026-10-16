#pragma once

#include "entities/Entity.h"
#include "economy/Resource.h"

// Asteroid field: holds ore of a single type that depletes as it's mined -- by players and by
// the system's miners alike -- and grows back over time (#348).
class AsteroidField : public Entity
{
public:
    AsteroidField(Vector2 pos, float size, std::string name, ResourceType resource, int ore);

    std::unique_ptr<Entity> Clone() const override
    {
        return std::make_unique<AsteroidField>(*this);
    }
    Render::Item Describe() const override;
    std::string  GetName() const override { return name_; }

    ResourceType GetResource() const { return resource_; }
    bool         HasOre() const { return oreRemaining_ >= 1.0; }
    int          GetOreMax() const { return oreMax_; }  // what it was made with, and holds full
    double       GetOre() const { return oreRemaining_; }
    void         SetOre(double ore);  // clamped to 0..GetOreMax()

    // Takes up to amount whole units of ore, returns the amount actually extracted.
    int Extract(int amount);
    // Takes up to `amount` (a fraction is fine: the coarse pass's miners), returns what it got.
    double Take(double amount);
    // `hours` of growing back at `share` of the full belt an hour, up to full.
    void Regrow(double share, double hours);

private:
    std::string  name_;
    ResourceType resource_;
    double       oreRemaining_;
    int          oreMax_;
};
