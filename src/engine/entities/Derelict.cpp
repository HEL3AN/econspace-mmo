#include "entities/Derelict.h"
#include "render/Textures.h"

Derelict::Derelict(Vector2 pos, float size, std::string name, double reward)
    : Entity(pos, size, Color{ 130, 130, 120, 255 }, EntityKind::Derelict), name_(std::move(name)),
      reward_(reward)
{
    SetArchetype("derelict.wreck");
}

Render::Item Derelict::Describe() const
{
    Render::Item it = Entity::Describe();
    it.intensity = looted_ ? 0.4f : 1.0f;  // a searched wreck is dimmer, in every backend
    return it;
}

std::string Derelict::GetName() const
{
    return looted_ ? (name_ + " (searched)") : name_;
}
