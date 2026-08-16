#include "entities/Derelict.h"
#include "render/Textures.h"

Derelict::Derelict(Vector2 pos, float size, std::string name, double reward)
    : Entity(pos, size, Color{ 130, 130, 120, 255 }, EntityKind::Derelict), name_(std::move(name)),
      reward_(reward)
{
    SetArchetype(Archetypes::Find("derelict.wreck"));
}

std::string Derelict::GetName() const
{
    return looted_ ? (name_ + " (searched)") : name_;
}

void Derelict::Draw() const
{
    Color c = looted_ ? Fade(color_, 0.4f) : color_;
    if (Tex::DrawSprite("derelict", pos_, size_, 0.0f, c))
        return;

    // Rough wireframe of the wreck: a diamond-shaped hull with a break in it.
    Vector2 top{ pos_.x, pos_.y - size_ };
    Vector2 bot{ pos_.x, pos_.y + size_ };
    Vector2 left{ pos_.x - size_ * 0.7f, pos_.y };
    Vector2 right{ pos_.x + size_ * 0.7f, pos_.y };
    DrawLineEx(top, right, 2.0f, c);
    DrawLineEx(right, bot, 2.0f, c);
    DrawLineEx(bot, left, 2.0f, Fade(c, 0.4f));  // the broken-off side is dimmer
    DrawLineEx(left, top, 2.0f, c);
}
