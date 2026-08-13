#include "entities/JumpGate.h"
#include "render/Textures.h"

JumpGate::JumpGate(Vector2 pos, float size, std::string name, std::string destination)
    : Entity(pos, size, Color{ 90, 200, 210, 255 }), name_(std::move(name)),
      destination_(std::move(destination))
{
}

void JumpGate::Draw() const
{
    if (Tex::DrawSprite("gate", pos_, size_, 0.0f, WHITE))
        return;

    // Gate ring with an inner glow.
    DrawCircleV(pos_, size_ * 0.6f, Fade(color_, 0.15f));
    DrawCircleLines(pos_.x, pos_.y, size_, color_);
    DrawCircleLines(pos_.x, pos_.y, size_ * 0.85f, Fade(color_, 0.6f));
}
