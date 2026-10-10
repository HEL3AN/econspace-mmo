#include "entities/Structure.h"

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
