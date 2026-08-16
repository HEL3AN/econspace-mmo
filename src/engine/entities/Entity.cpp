#include "entities/Entity.h"

Entity::Entity(Vector2 pos, float size, Color color, EntityKind kind)
    : pos_(pos), size_(size), color_(color), kind_(kind)
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
