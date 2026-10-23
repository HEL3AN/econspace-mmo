#include "render/Scene.h"

#include "rlgl.h"
#include <algorithm>
#include <cmath>

namespace Render
{

Item FromArchetype(const Archetype& a, Vector2 pos, float size)
{
    Item it;
    it.kind = a.kind;
    it.pos = pos;
    it.size = size > 0.0f ? size : a.defaultSize;
    it.color = a.visual.color;
    it.glyph = a.visual.glyph;
    it.sprite = a.visual.sprite;
    it.layer = a.visual.layer;
    it.style = a.visual.style;
    it.lightRadius = a.visual.lightRadius;
    it.lightIntensity = a.visual.lightIntensity;
    it.lightSelf = a.visual.lightSelf;
    it.material = a.visual.material;
    it.shape = &a.visual.shape;
    it.label = a.name;
    return it;
}

// How much larger than itself a ship may be drawn. The floor is for the zoom a station is
// flown at; zoomed out to a whole system a ship is a speck under its bracket again rather
// than a silhouette ten thousand units long.
static constexpr float MAX_SHIP_BOOST = 3.0f;

float ShownSize(EntityKind kind, float size, float zoom)
{
    if ((kind != EntityKind::PlayerShip && kind != EntityKind::Npc) || zoom <= 0.0f || size <= 0.0f)
        return size;
    return std::min(std::max(size, MIN_SHIP_PIXELS / zoom), size * MAX_SHIP_BOOST);
}

// How far a guide's chord may fall inside the true circle, in pixels.
static constexpr float GUIDE_SAG_PIXELS = 0.3f;

void DrawGuideCircle(Vector2 centre, float radius, const Camera2D& view, Color c)
{
    if (radius <= 0.0f || view.zoom <= 0.0f || c.a == 0)
        return;
    const float   w = (float)rlGetFramebufferWidth(), h = (float)rlGetFramebufferHeight();
    const Vector2 sc = GetWorldToScreen2D(centre, view);
    const float   r = radius * view.zoom;

    // The circle misses the target if it passes inside its nearest point or outside its
    // furthest corner.
    const float nx = std::clamp(sc.x, 0.0f, w) - sc.x, ny = std::clamp(sc.y, 0.0f, h) - sc.y;
    const float fx = std::max(std::fabs(sc.x), std::fabs(sc.x - w));
    const float fy = std::max(std::fabs(sc.y), std::fabs(sc.y - h));
    if (r < std::sqrt(nx * nx + ny * ny) - 1.0f || r > std::sqrt(fx * fx + fy * fy) + 1.0f)
        return;

    // A chord of angle t falls r * t^2 / 8 short of the arc.
    const float step = std::sqrt(8.0f * GUIDE_SAG_PIXELS / std::max(r, 1.0f));

    // The arc on the target: around the direction of the target's middle, as far round as
    // the circle stays within the target's half-diagonal of it.
    float       from = 0.0f, span = 2.0f * PI;
    const float mx = w * 0.5f - sc.x, my = h * 0.5f - sc.y;
    const float d = std::sqrt(mx * mx + my * my), half = 0.5f * std::sqrt(w * w + h * h);
    if (d > 1e-3f)
    {
        const float cosA = (r * r + d * d - half * half) / (2.0f * r * d);
        if (cosA > -1.0f)
        {
            const float a = std::acos(std::min(1.0f, cosA)) + step;
            from = std::atan2(my, mx) - a;
            span = std::min(2.0f * PI, 2.0f * a);
        }
    }
    const int n = std::clamp((int)std::ceil(span / step), 8, 4096);

    rlBegin(RL_LINES);
    rlColor4ub(c.r, c.g, c.b, c.a);
    for (int i = 0; i < n; i++)
    {
        const float t0 = from + span * (float)i / (float)n;
        const float t1 = from + span * (float)(i + 1) / (float)n;
        rlVertex2f(centre.x + std::cos(t0) * radius, centre.y + std::sin(t0) * radius);
        rlVertex2f(centre.x + std::cos(t1) * radius, centre.y + std::sin(t1) * radius);
    }
    rlEnd();
}

// No camera given means no camera applied: world units are screen pixels. It is the right
// default for the callers that have no view at all -- the tests, an agent -- and it is
// stated rather than left over from whoever drew last, for the same reason the lighting is.
static Camera2D NoView()
{
    Camera2D c{};
    c.offset = { 0.0f, 0.0f };
    c.target = { 0.0f, 0.0f };
    c.rotation = 0.0f;
    c.zoom = 1.0f;
    return c;
}

void Present(std::vector<Item> items, const Lighting& lighting, const Camera2D& view,
             IBackend& backend, double clock)
{
    // Stable, so that two items on the same layer keep the order the world gave them.
    // Without that a backend would reshuffle overlapping objects between frames.
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.layer < b.layer; });

    // Both stated every time, including when there is nothing to state. A backend outlives
    // the scene it drew, so leaving the previous frame's lights or camera in place would
    // light one view by the star of another -- which is what a preview drawn beside a
    // system would get.
    backend.SetView(view);
    backend.SetLighting(lighting);
    backend.SetClock(clock >= 0.0 ? clock : LocalClock());

    backend.Begin();
    for (const Item& it : items)
        backend.Draw(it);
    backend.End();
}

static double g_frozen = -1.0;

double LocalClock()
{
    return g_frozen >= 0.0 ? g_frozen : GetTime();
}

void FreezeLocalClock(double seconds)
{
    g_frozen = seconds;
}

void Present(std::vector<Item> items, const Lighting& lighting, IBackend& backend)
{
    Present(std::move(items), lighting, NoView(), backend);
}

void Present(std::vector<Item> items, IBackend& backend)
{
    Present(std::move(items), Lighting{}, NoView(), backend);
}

}  // namespace Render
