#include "entities/Entity.h"

Entity::Entity(Vector2 pos, float size, Color color) : pos_(pos), size_(size), color_(color)
{
}

void Entity::Update(float dt)
{
    (void)dt;  // the base object updates nothing
}

void Entity::Draw() const
{
    DrawCircleV(pos_, size_, color_);
}
