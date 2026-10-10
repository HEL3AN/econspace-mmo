#include "entities/Structure.h"

#include "core/Blueprint.h"

Structure::Structure(Vector2 pos, float size, std::string name, std::string builds)
    : Entity(pos, size, Color{ 200, 200, 210, 255 }, EntityKind::Structure), name_(std::move(name)),
      builds_(std::move(builds))
{
    SetArchetype(builds_);
    // The size the instance gives, else the archetype's: a beacon is as large as a beacon.
    if (size_ <= 0.0f && archetype_ != nullptr)
        size_ = archetype_->defaultSize;
}

void Structure::StartBuilding(double startedAt, double completesAt)
{
    startedAt_ = startedAt;
    completesAt_ = completesAt;
    if (IsBuilding())
        SetArchetype(SITE_ARCHETYPE);
}

void Structure::Complete()
{
    startedAt_ = 0.0;
    completesAt_ = 0.0;
    SetArchetype(builds_);
}

float Structure::Progress(double now) const
{
    if (!IsBuilding())
        return 1.0f;
    const double span = completesAt_ - startedAt_;
    if (span <= 0.0)
        return 1.0f;
    const double f = (now - startedAt_) / span;
    return (float)(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
}

const Blueprint* Structure::GetBlueprint() const
{
    if (!blueprint_.empty())
        return Blueprints::Find(blueprint_);
    return Blueprints::Building(builds_);
}

float Structure::MaxHull(double now) const
{
    const Blueprint* bp = GetBlueprint();
    if (bp == nullptr)
        return 0.0f;
    if (!IsBuilding())
        return bp->hull;
    const float start = Blueprints::Rules().siteHull;
    return bp->hull * (start + (1.0f - start) * Progress(now));
}

float Structure::HullFraction(double now) const
{
    const float max = MaxHull(now);
    if (max <= 0.0f)
        return 1.0f;  // nothing says what it takes: nothing has been taken
    const float left = (max - damage_) / max;
    return left < 0.0f ? 0.0f : left;
}
