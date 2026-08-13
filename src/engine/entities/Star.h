#pragma once

#include "entities/Entity.h"

// Star type affects color and (eventually) the resource that can be mined.
enum class StarType
{
    Yellow,
    Red,
    Blue
};

class Star : public Entity
{
public:
    Star(Vector2 pos, float size, StarType type);

    void                    Draw() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Star>(*this); }
    std::string             GetName() const override { return "Star"; }

    StarType GetStarType() const { return type_; }  // for network layout (M4d-3c)

private:
    StarType type_;
};
