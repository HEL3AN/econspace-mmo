#include "render/Built3DDetail.h"

#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Render
{
namespace Built3D
{

namespace
{

// A small integer hash and a generator on it: the same on every compiler, no <random>.
uint32_t Mix(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

struct Rng
{
    uint32_t s;
    Rng(uint32_t a, uint32_t b) : s(Mix(a, b) | 1u) {}
    uint32_t Next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float F() { return (float)(Next() >> 8) / 16777216.0f; }
    float Range(float lo, float hi) { return lo + (hi - lo) * F(); }
    int   Int(int lo, int hi) { return lo + (int)(F() * (float)(hi - lo + 1)) % (hi - lo + 1); }
};

struct V
{
    float x, y;
};

float Deg(float r)
{
    return r * RAD2DEG;
}

// A piece the generator adds: lies with its parent's z and spin, in the object's own colour
// unless it says otherwise.
struct Maker
{
    const Piece& parent;
    Rng&         rng;
    float        R;

    Piece Make(Form f, Role r, V pos, float angle, float radius, float width, float length, int dz,
               float riseInR) const
    {
        Piece q;
        q.form = f;
        q.role = r;
        q.pos = { pos.x, pos.y };
        q.angle = angle;
        q.radius = radius;
        q.width = width;
        q.length = length;
        q.z = parent.z + dz;
        q.depth = parent.depth;
        q.rise = riseInR * R;
        q.spin = parent.spin;
        q.seed = rng.F();
        q.sides = 6;
        q.filled = true;
        return q;
    }
    Piece Lamp(V pos, float radius, Color tint) const
    {
        Piece q = Make(Form::Disc, Role::Light, pos, 0.0f, radius, 0.0f, 0.0f, 1, 0.012f);
        q.blink = rng.Range(1.8f, 3.6f);
        q.blinkPhase = rng.F();
        q.tint = tint;
        q.rise = 0.0f;
        return q;
    }
};

constexpr Color AMBER{ 255, 186, 92, 255 };
constexpr Color COOL{ 190, 225, 255, 255 };

struct Placed
{
    V     at;
    float r;
};

bool Clear(const std::vector<Placed>& placed, V at, float r)
{
    for (const Placed& p : placed)
        if (std::hypot(p.at.x - at.x, p.at.y - at.y) < p.r + r)
            return false;
    return true;
}

// Long members: a truss beneath, collars along, small modules on the flanks.
void Member(const Piece& p, float R, int level, Rng& rng, std::vector<Piece>& out)
{
    const float len = p.length, w = p.width;
    const float a = p.angle * DEG2RAD;
    const V     dir{ std::cos(a), std::sin(a) }, perp{ -std::sin(a), std::cos(a) };
    const Maker mk{ p, rng, R };

    Piece truss = mk.Make(Form::Lattice, Role::Antenna, { p.pos.x, p.pos.y }, p.angle, 0.0f,
                          w * 1.9f, len * 0.96f, -1, 0.018f);
    truss.count = std::clamp((int)(len / (w * 1.3f)), 3, 16);
    out.push_back(truss);

    const int collars = std::clamp((int)(len / (0.16f * R)), 1, 5);
    for (int k = 0; k < collars; k++)
    {
        const float t = ((float)(k + 1) / (float)(collars + 1) - 0.5f) * len;
        out.push_back(mk.Make(Form::Bar, Role::Trim, { p.pos.x + dir.x * t, p.pos.y + dir.y * t },
                              p.angle + 90.0f, 0.0f, w * 0.24f, w * 1.35f, 1, 0.016f));
    }
    if (level < 2)
        return;
    // Modules on the flanks, of three sizes.
    const int n = std::clamp((int)(len / (0.1f * R)), 1, 6);
    for (int k = 0; k < n; k++)
    {
        if (rng.F() < 0.4f)
            continue;
        const float side = rng.F() < 0.5f ? -1.0f : 1.0f;
        const float cls = rng.F();
        const float ml = (cls < 0.2f ? 0.1f : cls < 0.6f ? 0.06f : 0.035f) * R;
        const float mw = ml * rng.Range(0.45f, 0.8f);
        const float t = (rng.Range(0.1f, 0.9f) - 0.5f) * len;
        const float off = side * (w * 0.5f + mw * 0.35f);
        const V     at{ p.pos.x + dir.x * t + perp.x * off, p.pos.y + dir.y * t + perp.y * off };
        out.push_back(mk.Make(Form::Bar, cls < 0.2f ? Role::Hull : Role::Panel, at, p.angle, 0.0f,
                              mw, ml, 0, ml * 0.35f / R));
    }
}

// A large polygon: stepped plating, buttresses at its corners, and on the roof masts and
// modules of several sizes.
void Plate(const Piece& p, float R, int level, Rng& rng, std::vector<Piece>& out)
{
    const Maker mk{ p, rng, R };
    const float r = p.radius;
    const int   n = std::max(3, p.sides);
    const V     c{ p.pos.x, p.pos.y };
    if (r >= 0.3f * R)
    {
        Piece step =
            mk.Make(Form::Polygon, Role::Panel, c, p.angle, r * 0.8f, 0.0f, 0.0f, 1, 0.018f);
        step.sides = n;
        out.push_back(step);
        Piece core =
            mk.Make(Form::Polygon, Role::Hull, c, p.angle, r * 0.52f, 0.0f, 0.0f, 2, 0.045f);
        core.sides = n;
        out.push_back(core);
        for (int k = 0; k < n; k++)
        {
            const float a = p.angle + 360.0f * (float)k / (float)n;
            const float ar = a * DEG2RAD;
            const V     at{ c.x + std::cos(ar) * r * 0.86f, c.y + std::sin(ar) * r * 0.86f };
            out.push_back(
                mk.Make(Form::Bar, Role::Trim, at, a, 0.0f, 0.04f * R, r * 0.34f, 1, 0.024f));
        }
    }
    if (level < 2)
        return;
    std::vector<Placed> placed;
    placed.push_back({ c, 0.0f });
    // Masts, each with a dish or a plain tip.
    const int masts = 1 + (int)(rng.F() * 2.0f);
    for (int k = 0; k < masts; k++)
    {
        const float d = rng.Range(0.12f, 0.45f) * r, t = rng.Range(0.0f, 6.2831853f);
        const V     at{ c.x + std::cos(t) * d, c.y + std::sin(t) * d };
        if (!Clear(placed, at, 0.03f * R))
            continue;
        placed.push_back({ at, 0.03f * R });
        const float height = rng.Range(0.1f, 0.2f);
        out.push_back(
            mk.Make(Form::Disc, Role::Antenna, at, 0.0f, 0.009f * R, 0.0f, 0.0f, 2, height));
        if (rng.F() < 0.6f)
            out.push_back(mk.Make(Form::Disc, Role::Trim, at, 0.0f, rng.Range(0.025f, 0.045f) * R,
                                  0.0f, 0.0f, 3, 0.006f));
    }
    // Modules: tanks and containers.
    const int mods = std::clamp((int)(r / (0.045f * R)), 3, 9);
    for (int k = 0; k < mods; k++)
    {
        const float cls = rng.F();
        const float s = (cls < 0.2f ? 0.075f : cls < 0.6f ? 0.045f : 0.026f) * R;
        const float d = rng.Range(0.3f, 0.82f) * r, t = rng.Range(0.0f, 6.2831853f);
        const V     at{ c.x + std::cos(t) * d, c.y + std::sin(t) * d };
        if (!Clear(placed, at, s * 1.15f))
            continue;
        placed.push_back({ at, s * 1.15f });
        const float rise = s * rng.Range(0.5f, 0.9f) / R;
        if (rng.F() < 0.45f)
            out.push_back(mk.Make(Form::Disc, Role::Hull, at, 0.0f, s * 0.7f, 0.0f, 0.0f, 2, rise));
        else
            out.push_back(mk.Make(Form::Bar, cls < 0.2f ? Role::Hull : Role::Panel, at,
                                  Deg(t) + 90.0f * (float)rng.Int(0, 1), 0.0f, s * 0.8f, s * 1.5f,
                                  2, rise));
    }
}

// A pad or tank: a rim, a platform on top, small satellites and lamps.
void Pad(const Piece& p, float R, int level, Rng& rng, std::vector<Piece>& out)
{
    const Maker mk{ p, rng, R };
    const float r = p.radius;
    const V     c{ p.pos.x, p.pos.y };
    out.push_back(mk.Make(Form::Ring, Role::Trim, c, 0.0f, r * 0.94f, r * 0.1f, 0.0f, 1, 0.012f));
    Piece plat = mk.Make(Form::Polygon, Role::Panel, c, rng.Range(0.0f, 60.0f), r * 0.5f, 0.0f,
                         0.0f, 1, 0.03f);
    plat.sides = 8;
    out.push_back(plat);
    if (level < 2)
        return;
    const int   tanks = rng.Int(3, 5);
    const float a0 = rng.Range(0.0f, 6.2831853f);
    for (int k = 0; k < tanks; k++)
    {
        const float t = a0 + 6.2831853f * (float)k / (float)tanks;
        const V     at{ c.x + std::cos(t) * r * 0.66f, c.y + std::sin(t) * r * 0.66f };
        out.push_back(
            mk.Make(Form::Disc, Role::Hull, at, 0.0f, r * 0.15f, 0.0f, 0.0f, 2, r * 0.13f / R));
    }
    const int lamps = 6;
    for (int k = 0; k < lamps; k++)
    {
        const float t = 6.2831853f * (float)k / (float)lamps;
        out.push_back(mk.Lamp({ c.x + std::cos(t) * r * 0.97f, c.y + std::sin(t) * r * 0.97f },
                              0.010f * R, k % 2 == 0 ? AMBER : COOL));
    }
}

// A ring: ribs across it, habitat blocks of several sizes, lamps on its inner edge, and spokes
// to the hub so it is not a loop hanging in space.
void RingRule(const Piece& p, float R, int level, Rng& rng, float hubR,
              const std::vector<float>& arms, std::vector<Piece>& out)
{
    const Maker mk{ p, rng, R };
    const float w = p.width, rOut = p.radius, rMid = rOut - w * 0.5f;
    const V     c{ p.pos.x, p.pos.y };

    const int ribs = std::clamp((int)(6.2831853f * rMid / (w * 3.4f)), 12, 72);
    for (int k = 0; k < ribs; k++)
    {
        const float t = 6.2831853f * (float)k / (float)ribs;
        out.push_back(mk.Make(Form::Bar, Role::Trim,
                              { c.x + std::cos(t) * rMid, c.y + std::sin(t) * rMid }, Deg(t), 0.0f,
                              w * 0.3f, w * 1.3f, 1, 0.01f));
    }
    const int   blocks = std::clamp((int)(6.2831853f * rMid / (0.45f * R)), 4, 10);
    const float b0 = rng.Range(0.0f, 6.2831853f);
    for (int k = 0; k < blocks; k++)
    {
        if (rng.F() < 0.25f)
            continue;
        const float t = b0 + 6.2831853f * ((float)k + rng.Range(-0.25f, 0.25f)) / (float)blocks;
        const float cls = rng.F();
        const float len = rMid * (cls < 0.25f ? 0.2f : cls < 0.65f ? 0.11f : 0.055f);
        const float wid = w * (cls < 0.25f ? 2.0f : cls < 0.65f ? 1.5f : 1.1f);
        const float rise = (cls < 0.25f ? 0.06f : cls < 0.65f ? 0.04f : 0.026f);
        out.push_back(mk.Make(Form::Capsule, Role::Hull,
                              { c.x + std::cos(t) * rMid, c.y + std::sin(t) * rMid },
                              Deg(t) + 90.0f, 0.0f, wid, len, 1, rise));
    }

    // Spokes: between the arms where there are some, evenly where there are not.
    std::vector<float> spokes;
    if (arms.size() >= 2)
    {
        std::vector<float> a = arms;
        std::sort(a.begin(), a.end());
        for (size_t i = 0; i < a.size(); i++)
        {
            float next = a[(i + 1) % a.size()];
            if (next <= a[i])
                next += 360.0f;
            spokes.push_back(0.5f * (a[i] + next));
        }
    }
    else
    {
        const float s0 = arms.empty() ? rng.Range(0.0f, 90.0f) : arms[0] + 180.0f;
        const int   n = arms.empty() ? 4 : 1;
        for (int k = 0; k < n; k++)
            spokes.push_back(s0 + 360.0f * (float)k / (float)n);
    }
    const float r0 = hubR * 0.8f, r1 = rOut - w * 0.8f;
    if (r1 > r0 + 0.1f * R)
        for (const float sa : spokes)
        {
            const float a = sa * DEG2RAD;
            const V     dir{ std::cos(a), std::sin(a) };
            const float len = r1 - r0, mid = 0.5f * (r0 + r1);
            Piece       sp =
                mk.Make(Form::Lattice, Role::Antenna, { c.x + dir.x * mid, c.y + dir.y * mid }, sa,
                        0.0f, 0.055f * R, len, -1, 0.022f);
            sp.count = std::clamp((int)(len / (0.09f * R)), 4, 24);
            out.push_back(sp);
            out.push_back(mk.Make(Form::Disc, Role::Trim, { c.x + dir.x * r1, c.y + dir.y * r1 },
                                  0.0f, 0.04f * R, 0.0f, 0.0f, 0, 0.03f));
            if (level >= 2 && rng.F() < 0.75f)
            {
                // Radiator fins across the spoke.
                const int fins = rng.Int(3, 4);
                for (int k = 0; k < fins; k++)
                {
                    const float t = r0 + len * (0.5f + 0.1f * (float)k);
                    out.push_back(mk.Make(Form::Bar, Role::Panel,
                                          { c.x + dir.x * t, c.y + dir.y * t }, sa + 90.0f, 0.0f,
                                          0.034f * R, 0.2f * R, 0, 0.007f));
                }
            }
        }
    if (level < 2)
        return;
    const int lamps = std::clamp((int)(6.2831853f * rMid / (0.14f * R)), 8, 40);
    for (int k = 0; k < lamps; k++)
    {
        const float t = 6.2831853f * (float)k / (float)lamps;
        const float rr = rOut - w * 0.92f;
        out.push_back(mk.Lamp({ c.x + std::cos(t) * rr, c.y + std::sin(t) * rr }, 0.009f * R,
                              k % 3 == 0 ? AMBER : COOL));
    }
}

}  // namespace

int DetailLevel(float pixelRadius)
{
    return pixelRadius < 40.0f ? 0 : pixelRadius < 110.0f ? 1 : 2;
}

std::vector<Piece> Embellish(const std::vector<Piece>& composed, float R, int seed, int level)
{
    if (level <= 0)
        return composed;
    // What the composition has at its centre and out along its radii, for what needs to
    // reach between them.
    float              hubR = 0.0f;
    std::vector<float> arms;
    for (const Piece& p : composed)
    {
        const float d = std::hypot(p.pos.x, p.pos.y);
        if ((p.form == Form::Polygon || p.form == Form::Disc) && p.filled && d < 0.15f * R &&
            p.role != Role::Light && !p.soft)
            hubR = std::max(hubR, p.radius);
        if ((p.form == Form::Capsule || p.form == Form::Bar) && p.length >= 0.3f * R &&
            d > 0.2f * R && p.role != Role::Light)
            arms.push_back(std::fmod(Deg(std::atan2(p.pos.y, p.pos.x)) + 360.0f, 360.0f));
    }
    if (hubR <= 0.0f)
        hubR = 0.25f * R;

    std::vector<Piece> out = composed;
    for (size_t i = 0; i < composed.size(); i++)
    {
        const Piece& p = composed[i];
        if (p.soft || p.surface || p.orbits || p.form == Form::Band || p.alpha < 0.9f ||
            p.role == Role::Light || p.role == Role::Antenna || (int)out.size() > 900)
            continue;
        Rng rng((uint32_t)seed * 2654435761u + 17u, (uint32_t)i);
        switch (p.form)
        {
            case Form::Capsule:
            case Form::Bar:
                if (p.length >= 2.2f * p.width && p.length > 0.18f * R && p.width > 0.02f * R &&
                    p.role != Role::Panel)
                    Member(p, R, level, rng, out);
                break;
            case Form::Polygon:
                if (p.filled && p.radius >= 0.18f * R)
                    Plate(p, R, level, rng, out);
                break;
            case Form::Disc:
                if (p.radius >= 0.15f * R && p.radius < 0.45f * R)
                    Pad(p, R, level, rng, out);
                break;
            case Form::Ring:
                if (p.radius >= 0.5f * R && p.width > 0.01f * R)
                    RingRule(p, R, level, rng, hubR, arms, out);
                break;
            default: break;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Piece& a, const Piece& b)
                     { return a.depth != b.depth ? a.depth < b.depth : a.z < b.z; });
    return out;
}

}  // namespace Built3D
}  // namespace Render
