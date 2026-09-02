#include "render/Lighting.h"

#include "render/Scene.h"
#include <algorithm>
#include <cmath>

namespace Render
{
namespace
{
struct Contribution
{
    float   weight;
    Vector2 dir;
    Color   color;
};
}  // namespace

Lighting::Sample Lighting::At(Vector2 p) const
{
    Sample out;
    if (lights.empty())
        return out;

    std::vector<Contribution> got;
    got.reserve(lights.size());
    for (const Light& l : lights)
    {
        const float dx = l.pos.x - p.x, dy = l.pos.y - p.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (l.radius <= 0.0f || dist >= l.radius)
            continue;

        // Quadratic falloff to zero at the radius rather than inverse-square: a real
        // inverse square never reaches zero, so a star on the far side of the system
        // would still contribute a direction, and the arithmetic would decide the look of
        // an object nowhere near it.
        const float t = 1.0f - dist / l.radius;
        const float weight = t * t * l.intensity;
        if (weight <= 0.0f)
            continue;

        // A point standing exactly on a light has no direction to be lit from. It is lit
        // from everywhere -- which is what a star looks like from inside it.
        Vector2 dir{ 0.0f, 0.0f };
        if (dist > 0.0001f)
            dir = { dx / dist, dy / dist };
        got.push_back({ weight, dir, l.color });
    }
    if (got.empty())
        return out;

    const size_t keep = std::min<size_t>(got.size(), (size_t)MAX_CONTRIBUTORS);
    std::partial_sort(got.begin(), got.begin() + (long)keep, got.end(),
                      [](const Contribution& a, const Contribution& b)
                      { return a.weight > b.weight; });

    float total = 0.0f, dx = 0.0f, dy = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
    for (size_t i = 0; i < keep; i++)
    {
        const Contribution& c = got[i];
        total += c.weight;
        dx += c.dir.x * c.weight;
        dy += c.dir.y * c.weight;
        r += c.color.r * c.weight;
        g += c.color.g * c.weight;
        b += c.color.b * c.weight;
    }

    out.strength = std::min(1.0f, total);
    // Two stars on opposite sides cancel to no direction and full brightness, which is
    // exactly right: the object between them has no dark side.
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.0001f)
        out.dir = { dx / len, dy / len };
    if (total > 0.0f)
        out.tint = { (unsigned char)std::min(255.0f, r / total),
                     (unsigned char)std::min(255.0f, g / total),
                     (unsigned char)std::min(255.0f, b / total), 255 };
    return out;
}

Lighting LightsFrom(const std::vector<Item>& items, float ambient)
{
    Lighting out;
    out.ambient = ambient;
    for (const Item& it : items)
        if (it.lightRadius > 0.0f && it.lightIntensity > 0.0f)
            out.lights.push_back({ it.pos, it.color, it.lightIntensity, it.lightRadius });
    return out;
}

}  // namespace Render
