#include "entities/Star.h"
#include "render/Textures.h"

static Color ColorForStarType(StarType type)
{
    switch (type)
    {
        case StarType::Yellow: return YELLOW;
        case StarType::Red:    return RED;
        case StarType::Blue:   return SKYBLUE;
    }
    return WHITE;
}

Star::Star(Vector2 pos, float size, StarType type)
    : Entity(pos, size, ColorForStarType(type)), type_(type)
{
}

void Star::Draw() const
{
    // Glow — a large translucent circle beneath the star's body.
    DrawCircleV(pos_, size_ * 1.8f, Fade(color_, 0.15f));
    if (!Tex::DrawSprite("star", pos_, size_, 0.0f, color_))
        Entity::Draw();
}
