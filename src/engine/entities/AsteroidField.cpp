#include "entities/AsteroidField.h"
#include "render/Textures.h"
#include <cmath>

AsteroidField::AsteroidField(Vector2 pos, float size, std::string name, ResourceType resource,
                             int ore)
    : Entity(pos, size, GRAY, EntityKind::Field), name_(std::move(name)), resource_(resource),
      oreRemaining_(ore), oreMax_(ore)
{
    SetArchetype(Archetypes::Find("field.asteroid"));
}

Render::Item AsteroidField::Describe() const
{
    Render::Item it = Entity::Describe();
    // A worked-out belt draws paler. Only the field knows how much is left, so it says
    // so here rather than the backend guessing from anything.
    it.intensity = (oreMax_ > 0) ? (float)oreRemaining_ / (float)oreMax_ : 0.0f;
    return it;
}

int AsteroidField::Extract(int amount)
{
    int got = (amount < oreRemaining_) ? amount : oreRemaining_;
    oreRemaining_ -= got;
    return got;
}
