#include "entities/Entity.h"

Entity::Entity(Vector2 pos, float size, Color color, EntityKind kind)
    : pos_(pos), size_(size), color_(color), kind_(kind)
{
}

void Entity::Update(float dt)
{
    (void)dt;  // the base object updates nothing
}

Render::Item Entity::Describe() const
{
    // The archetype supplies the look; the instance overrides what only it knows. The
    // colour is the instance's on purpose -- faction paint and planet type are chosen per
    // object, and the archetype's colour is the default they start from.
    Render::Item it;
    if (archetype_ != nullptr)
        it = Render::FromArchetype(*archetype_, pos_, size_);
    it.id = id_;
    it.kind = kind_;
    it.pos = pos_;
    it.size = size_;
    it.color = color_;
    it.label = GetName();
    return it;
}
