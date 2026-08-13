#pragma once

#include "entities/Entity.h"
#include <string>

// Nebula: a large cloud region. Inside it the player ship is hidden from pirates
// (they don't fire on it) — a hiding spot. size_ is used as the radius.
class Nebula : public Entity
{
public:
    Nebula(Vector2 pos, float radius, std::string name);

    void                    Draw() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Nebula>(*this); }
    std::string             GetName() const override { return name_; }

    bool Contains(Vector2 p) const;  // whether the point is inside the cloud

private:
    std::string name_;
};
