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

int AsteroidField::Extract(int amount)
{
    int got = (amount < oreRemaining_) ? amount : oreRemaining_;
    oreRemaining_ -= got;
    return got;
}

void AsteroidField::Draw() const
{
    DrawCircleLines(pos_.x, pos_.y, size_, Fade(GRAY, 0.35f));

    // Brightness reflects remaining ore: a depleted field is noticeably paler.
    float fill = (oreMax_ > 0) ? (float)oreRemaining_ / oreMax_ : 0.0f;

    if (Tex::DrawSprite("asteroids", pos_, size_, 0.0f, Fade(WHITE, 0.35f + 0.65f * fill)))
        return;

    Color rock = Fade(GRAY, 0.3f + 0.7f * fill);
    for (int i = 0; i < 16; i++)
    {
        float   angle = i * 2.39996f;
        float   radius = size_ * (0.2f + 0.65f * ((i * 7 % 11) / 11.0f));
        Vector2 p = { pos_.x + cosf(angle) * radius, pos_.y + sinf(angle) * radius };
        DrawCircleV(p, 3.0f + (i % 3), rock);
    }
}
