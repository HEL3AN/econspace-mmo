#include "entities/Nebula.h"
#include "render/Textures.h"
#include <cmath>

Nebula::Nebula(Vector2 pos, float radius, std::string name)
    : Entity(pos, radius, Color{ 150, 90, 200, 255 }, EntityKind::Nebula), name_(std::move(name))
{
    SetArchetype("nebula.cloud");
}

bool Nebula::Contains(Vector2 p) const
{
    float dx = p.x - pos_.x, dy = p.y - pos_.y;
    return (dx * dx + dy * dy) <= size_ * size_;
}
