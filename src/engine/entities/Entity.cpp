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
    Render::Item it;
    it.id = id_;
    it.kind = kind_;
    it.pos = pos_;
    it.size = size_;
    it.color = color_;
    it.label = GetName();
    if (archetype_ != nullptr)
    {
        it.glyph = archetype_->visual.glyph;
        it.sprite = archetype_->visual.sprite;
        it.layer = archetype_->visual.layer;
    }
    return it;
}
