#include "entities/JumpGate.h"
#include "render/Textures.h"

JumpGate::JumpGate(Vector2 pos, float size, std::string name, std::string destination)
    : Entity(pos, size, Color{ 90, 200, 210, 255 }, EntityKind::Gate), name_(std::move(name)),
      destination_(std::move(destination))
{
    SetArchetype(Archetypes::Find("gate.jump"));
}
