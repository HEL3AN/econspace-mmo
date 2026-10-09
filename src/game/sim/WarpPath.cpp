#include "sim/WarpPath.h"

#include <cmath>

namespace WarpPath
{

float SegmentDistance(Vector2 a, Vector2 b, Vector2 p)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float       t = len2 > 0.0f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float cx = a.x + dx * t - p.x, cy = a.y + dy * t - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

namespace
{
float Dist(Vector2 a, Vector2 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}
}  // namespace

bool Via(Vector2 from, Vector2 to, const std::vector<Body>& bodies, Vector2& via)
{
    // The largest body in the way decides the detour: it is the one a player would see
    // the line cross, and a detour around it usually clears anything smaller beside it.
    const Body* block = nullptr;
    for (const Body& b : bodies)
    {
        const float keep = b.radius * (1.0f + CLEARANCE);
        if (Dist(from, b.pos) < keep || Dist(to, b.pos) < keep)
            continue;  // starting or ending at a body: no detour helps
        if (SegmentDistance(from, to, b.pos) >= keep)
            continue;
        if (block == nullptr || b.radius > block->radius)
            block = &b;
    }
    if (block == nullptr)
        return false;

    // Out from the body's centre, on the side the line already passes -- the shorter way
    // round. A line through the very centre has no side, so it turns left of its heading,
    // the same way every time.
    const float keep = block->radius * (1.0f + CLEARANCE);
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const float len2 = len * len;
    const float t = ((block->pos.x - from.x) * dx + (block->pos.y - from.y) * dy) / len2;
    Vector2     out = { from.x + dx * t - block->pos.x, from.y + dy * t - block->pos.y };
    float       outLen = std::sqrt(out.x * out.x + out.y * out.y);
    if (outLen < keep * 0.001f)
    {
        out = { -dy / len, dx / len };
        outLen = 1.0f;
    }
    out = { out.x / outLen, out.y / outLen };

    // Far enough out that both legs clear the body. Two legs to a point just outside the
    // surface would still cut its edge; each step out straightens them.
    float r = keep;
    for (int i = 0; i < 24; i++)
    {
        via = { block->pos.x + out.x * r, block->pos.y + out.y * r };
        if (SegmentDistance(from, via, block->pos) >= block->radius &&
            SegmentDistance(via, to, block->pos) >= block->radius)
            return true;
        r *= 1.15f;
    }
    return true;  // as far out as it got; still better than through the middle
}

}  // namespace WarpPath
