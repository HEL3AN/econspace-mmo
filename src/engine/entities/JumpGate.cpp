#include "entities/JumpGate.h"
#include "render/Textures.h"

JumpGate::JumpGate(Vector2 pos, float size, std::string name, std::string destination)
    : Entity(pos, size, Color{ 90, 200, 210, 255 }, EntityKind::Gate), name_(std::move(name)),
      destination_(std::move(destination))
{
    SetArchetype("gate.jump");
}

Render::Item JumpGate::Describe() const
{
    // What is left of a mouth or a hole is its look (#618); `Present` reads it on the clock.
    Render::Item it = Entity::Describe();
    it.endsAt = closesAt_;
    return it;
}
