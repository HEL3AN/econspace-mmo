#include "sim/StructureSchedule.h"

#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/Structure.h"

void StructureSchedule::Add(const std::string& systemId, const Entity& e)
{
    if (e.GetKind() == EntityKind::Derelict)
    {
        const double end = static_cast<const Derelict&>(e).GetExpiresAt();
        if (end > 0.0)
            due_.insert({ end, e.GetId(), systemId });
        return;
    }
    if (e.GetKind() == EntityKind::Field)
    {
        // A field with a life ends when its time is up (#617).
        const AsteroidField& f = static_cast<const AsteroidField&>(e);
        if (f.HasLife())
            due_.insert({ f.EndsAt(), e.GetId(), systemId });
        return;
    }
    if (e.GetKind() != EntityKind::Structure)
        return;
    const Structure& t = static_cast<const Structure&>(e);
    if (t.IsBuilding())
        due_.insert({ t.GetCompletesAt(), t.GetId(), systemId });
    if (t.GetExpiresAt() > 0.0)
        due_.insert({ t.GetExpiresAt(), t.GetId(), systemId });
}

std::optional<StructureSchedule::Due> StructureSchedule::PopDue(double now)
{
    if (due_.empty() || std::get<0>(*due_.begin()) > now)
        return std::nullopt;
    Due d{ std::get<1>(*due_.begin()), std::get<2>(*due_.begin()) };
    due_.erase(due_.begin());
    return d;
}
