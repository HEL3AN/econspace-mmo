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

double FieldCurve::Cap(double share) const
{
    if (share < 0.0 || share >= 1.0)
        return 0.0;
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    if (share < bloomEnd)
        return lerp(bloomFrom, 1.0, bloomEnd > 0.0 ? share / bloomEnd : 1.0);
    if (share < richEnd)
        return 1.0;
    if (share < thinEnd)
        return lerp(1.0, thinTo, (share - richEnd) / (thinEnd - richEnd));
    return lerp(thinTo, 0.0, (share - thinEnd) / (1.0 - thinEnd));
}

void AsteroidField::SetLife(double bornAt, double life, const FieldCurve& curve)
{
    bornAt_ = bornAt;
    life_ = life > 0.0 ? life : 0.0;
    curve_ = curve;
    if (life_ > 0.0)
        oreRemaining_ = 0.0;
}

double AsteroidField::CapAt(double now) const
{
    if (life_ <= 0.0)
        return (double)oreMax_;
    return (double)oreMax_ * curve_.Cap((now - bornAt_) / life_);
}

double AsteroidField::OreAt(double now) const
{
    if (life_ <= 0.0)
        return oreRemaining_;
    return std::max(0.0, CapAt(now) - taken_);
}

double AsteroidField::Age(double now)
{
    if (life_ <= 0.0)
        return 0.0;
    const double ore = OreAt(now);
    const double lost = std::max(0.0, oreRemaining_ - ore);
    oreRemaining_ = ore;
    return lost;
}

double AsteroidField::Density(double now, double floor) const
{
    const double cap = CapAt(now);
    if (life_ <= 0.0 || cap <= 0.0)
        return 1.0;
    return floor + (1.0 - floor) * std::clamp(oreRemaining_ / cap, 0.0, 1.0);
}

int AsteroidField::Extract(int amount)
{
    const int whole = (int)std::floor(oreRemaining_);
    const int got = amount < whole ? amount : whole;
    if (got <= 0)
        return 0;
    oreRemaining_ -= got;
    taken_ += got;
    return got;
}

double AsteroidField::Take(double amount)
{
    const double got = std::clamp(amount, 0.0, oreRemaining_);
    oreRemaining_ -= got;
    taken_ += got;
    return got;
}

void AsteroidField::Regrow(double share, double hours)
{
    if (life_ > 0.0)
        return;  // a field with a life only thins
    SetOre(oreRemaining_ + share * oreMax_ * hours);
}
