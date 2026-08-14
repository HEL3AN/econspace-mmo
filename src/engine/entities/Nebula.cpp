#include "entities/Nebula.h"
#include "render/Textures.h"
#include <cmath>

Nebula::Nebula(Vector2 pos, float radius, std::string name)
    : Entity(pos, radius, Color{ 150, 90, 200, 255 }, EntityKind::Nebula), name_(std::move(name))
{
}

bool Nebula::Contains(Vector2 p) const
{
    float dx = p.x - pos_.x, dy = p.y - pos_.y;
    return (dx * dx + dy * dy) <= size_ * size_;
}

void Nebula::Draw() const
{
    // A few nested translucent circles — a soft cloud.
    DrawCircleV(pos_, size_, Fade(color_, 0.08f));
    DrawCircleV(pos_, size_ * 0.7f, Fade(color_, 0.08f));
    DrawCircleV(pos_, size_ * 0.4f, Fade(color_, 0.10f));
    DrawCircleLines(pos_.x, pos_.y, size_, Fade(color_, 0.35f));
}
