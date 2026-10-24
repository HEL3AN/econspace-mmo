#include "core/Interpolation.h"

#include "raymath.h"

#include <algorithm>
#include <utility>

namespace
{
const Proto::EntitySnapshot* FindEnt(const std::vector<Proto::EntitySnapshot>* v, int id)
{
    if (v == nullptr)
        return nullptr;
    for (const Proto::EntitySnapshot& e : *v)
        if (e.id == id)
            return &e;
    return nullptr;
}

// Angle interpolation along the shortest arc.
float LerpAngleShort(float a, float b, float t)
{
    float d = b - a;
    while (d > PI)
        d -= 2.0f * PI;
    while (d < -PI)
        d += 2.0f * PI;
    return a + d * t;
}
}  // namespace

void Interpolation::Add(double serverTime, std::vector<Proto::EntitySnapshot> entities)
{
    snaps_.push_back({ serverTime, std::move(entities) });
}

void Interpolation::Settle()
{
    std::stable_sort(snaps_.begin(), snaps_.end(),
                     [](const Snap& x, const Snap& y) { return x.t < y.t; });
    // A layout after the snapshot in the same frame empties the buffer, and the next
    // snapshot starts it again: a back() on nothing used to end the client there.
    const double cutoff = snaps_.empty() ? 0.0 : snaps_.back().t - KEEP_SECONDS;
    while (snaps_.size() > 2 && snaps_.front().t < cutoff)
        snaps_.pop_front();
    while (snaps_.size() > MAX_SNAPSHOTS)
        snaps_.pop_front();
}

Interpolation::Bracket Interpolation::Around(double renderTime) const
{
    Bracket b;
    b.t = renderTime;
    for (const Snap& s : snaps_)
    {
        if (s.t <= renderTime)
        {
            b.before = &s.entities;
            b.beforeT = s.t;
        }
        else
        {
            b.after = &s.entities;
            b.afterT = s.t;
            break;
        }
    }
    return b;
}

bool Interpolation::PoseOf(const Bracket& b, int id, Pose& out)
{
    const Proto::EntitySnapshot* ea = FindEnt(b.before, id);
    const Proto::EntitySnapshot* eb = FindEnt(b.after, id);
    if (ea != nullptr && eb != nullptr && b.afterT > b.beforeT)
    {
        const float al = (float)((b.t - b.beforeT) / (b.afterT - b.beforeT));
        out.pos = Vector2Lerp(ea->pos, eb->pos, al);
        out.heading = LerpAngleShort(ea->heading, eb->heading, al);
        return true;
    }
    const Proto::EntitySnapshot* one = eb != nullptr ? eb : ea;
    if (one == nullptr)
        return false;
    out.pos = one->pos;
    out.heading = one->heading;
    return true;
}

Vector2 Interpolation::VelocityOf(int id) const
{
    const Proto::EntitySnapshot* newer = nullptr;
    double                       newerT = 0.0;
    for (auto it = snaps_.rbegin(); it != snaps_.rend(); ++it)
    {
        const Proto::EntitySnapshot* es = FindEnt(&it->entities, id);
        if (es == nullptr)
            continue;
        if (newer == nullptr)
        {
            newer = es;
            newerT = it->t;
            continue;
        }
        const float span = (float)(newerT - it->t);
        if (span > 0.0f)
            return { (newer->pos.x - es->pos.x) / span, (newer->pos.y - es->pos.y) / span };
        break;
    }
    return { 0.0f, 0.0f };
}
