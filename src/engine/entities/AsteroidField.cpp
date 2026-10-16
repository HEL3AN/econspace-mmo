#include "entities/AsteroidField.h"
#include "render/Textures.h"
#include <algorithm>
#include <cmath>

AsteroidField::AsteroidField(Vector2 pos, float size, std::string name, ResourceType resource,
                             int ore)
    : Entity(pos, size, GRAY, EntityKind::Field), name_(std::move(name)), resource_(resource),
      oreRemaining_(ore), oreMax_(ore)
{
    SetArchetype("field.asteroid");
}

Render::Item AsteroidField::Describe() const
{
    Render::Item it = Entity::Describe();
    // A worked-out belt draws paler. Only the field knows how much is left, so it says
    // so here rather than the backend guessing from anything.
    it.intensity = (oreMax_ > 0) ? (float)(oreRemaining_ / oreMax_) : 0.0f;
    return it;
}

void AsteroidField::SetOre(double ore)
{
    oreRemaining_ = std::clamp(ore, 0.0, (double)oreMax_);
}

int AsteroidField::Extract(int amount)
{
    const int whole = (int)std::floor(oreRemaining_);
    const int got = amount < whole ? amount : whole;
    if (got <= 0)
        return 0;
    oreRemaining_ -= got;
    return got;
}

double AsteroidField::Take(double amount)
{
    const double got = std::clamp(amount, 0.0, oreRemaining_);
    oreRemaining_ -= got;
    return got;
}

void AsteroidField::Regrow(double share, double hours)
{
    SetOre(oreRemaining_ + share * oreMax_ * hours);
}
