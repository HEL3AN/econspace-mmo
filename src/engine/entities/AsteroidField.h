#pragma once

#include "entities/Entity.h"
#include "economy/Resource.h"

// Asteroid field: holds ore of a single type that depletes as it's mined.
class AsteroidField : public Entity
{
public:
    AsteroidField(Vector2 pos, float size, std::string name, ResourceType resource, int ore);

    void                    Draw() const override;
    std::unique_ptr<Entity> Clone() const override
    {
        return std::make_unique<AsteroidField>(*this);
    }
    std::string GetName() const override { return name_; }

    ResourceType GetResource() const { return resource_; }
    bool         HasOre() const { return oreRemaining_ > 0; }

    // Takes up to amount units of ore, returns the amount actually extracted.
    int Extract(int amount);

private:
    std::string  name_;
    ResourceType resource_;
    int          oreRemaining_;
    int          oreMax_;
};
