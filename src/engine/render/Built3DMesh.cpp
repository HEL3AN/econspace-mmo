#include "render/Built3DMesh.h"

#include "raylib.h"
#include <algorithm>
#include <cmath>

namespace Render
{
namespace Built3D
{

namespace
{

struct V2
{
    float x = 0.0f, y = 0.0f;
};
using Loop = std::vector<V2>;

constexpr float TINY = 1e-4f;

// "Inside the outline" for a wall, which has no outline to be anti-aliased by.
constexpr float WALL_EDGE = 1.0e6f;

// How far the top of a floating slab rises over its rim, in radii.
constexpr float LIFT_CHAMFER = 0.008f;

V2 Add(V2 a, V2 b)
{
    return { a.x + b.x, a.y + b.y };
}
V2 Mul(V2 a, float k)
{
    return { a.x * k, a.y * k };
}
V2 Sub(V2 a, V2 b)
{
    return { a.x - b.x, a.y - b.y };
}

// --- The height rule ------------------------------------------------------------------------

// How tall a piece stands on what it lies on, in the object's radii. Bigger things are
// thicker, up to a cap; a mast is thin and tall. The numbers are art: tuned by looking at
// stations beside planets, and the same for every object, so nothing is tuned per archetype.
constexpr float HEIGHT_SCALE = 3.0f;

float Rise(Role role, float sr)
{
    float k = 0.55f, lo = 0.008f, hi = 0.07f;
    switch (role)
    {
        case Role::Hull: break;
        case Role::Panel:
            k = 0.30f;
            lo = 0.004f;
            hi = 0.03f;
            break;
        case Role::Trim:
            k = 0.60f;
            lo = 0.006f;
            hi = 0.05f;
            break;
        case Role::Light:
            k = 0.40f;
            lo = 0.004f;
            hi = 0.02f;
            break;
        case Role::Antenna:
            k = 1.6f;
            lo = 0.01f;
            hi = 0.10f;
            break;
    }
    return std::clamp(k * sr * HEIGHT_SCALE, lo, hi);
}

// The level a z-order stands on, in radii: below the hull, on it, over it.
float Level(int z)
{
    return 0.045f * (float)(std::clamp(z, -3, 3) + 3);
}

// --- Outlines -------------------------------------------------------------------------------

Loop Circle(V2 c, float r, int n, float a0 = 0.0f)
{
    r = std::max(r, TINY);
    Loop l((size_t)n);
    for (int k = 0; k < n; k++)
    {
        const float t = a0 + 2.0f * PI * (float)k / (float)n;
        l[(size_t)k] = { c.x + std::cos(t) * r, c.y + std::sin(t) * r };
    }
    return l;
}

// Round about the segment a-b at radius r, counter-clockwise.
Loop Stadium(V2 a, V2 b, float r, int caps)
{
    r = std::max(r, TINY);
    const float axis = std::atan2(b.y - a.y, b.x - a.x);
    Loop        l;
    for (int k = 0; k <= caps; k++)  // round the tip, from the right of the axis to its left
    {
        const float t = axis - 0.5f * PI + PI * (float)k / (float)caps;
        l.push_back({ b.x + std::cos(t) * r, b.y + std::sin(t) * r });
    }
    for (int k = 0; k <= caps; k++)  // and round the tail
    {
        const float t = axis + 0.5f * PI + PI * (float)k / (float)caps;
        l.push_back({ a.x + std::cos(t) * r, a.y + std::sin(t) * r });
    }
    return l;
}

Loop Rect(V2 c, V2 along, V2 across, float hl, float hw)
{
    hl = std::max(hl, TINY);
    hw = std::max(hw, TINY);
    auto at = [&](float u, float v)
    { return V2{ c.x + along.x * u + across.x * v, c.y + along.y * u + across.y * v }; };
    return { at(hl, -hw), at(hl, hw), at(-hl, hw), at(-hl, -hw) };
}

// A convex counter-clockwise polygon, every edge moved in by `d` (out when negative).
Loop OffsetConvex(const Loop& p, float d)
{
    const size_t n = p.size();
    Loop         out(n);
    for (size_t i = 0; i < n; i++)
    {
        const V2    a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        V2          e0 = Sub(b, a), e1 = Sub(c, b);
        const float l0 = std::max(std::hypot(e0.x, e0.y), TINY);
        const float l1 = std::max(std::hypot(e1.x, e1.y), TINY);
        e0 = Mul(e0, 1.0f / l0);
        e1 = Mul(e1, 1.0f / l1);
        // Inward normals of a counter-clockwise polygon (y up in the maths, whatever the
        // screen does with it): the left of each edge.
        const V2    n0{ -e0.y, e0.x }, n1{ -e1.y, e1.x };
        const float k = 1.0f + n0.x * n1.x + n0.y * n1.y;
        const V2    s = Add(n0, n1);
        out[i] = Add(b, Mul(s, d / std::max(k, 0.2f)));
    }
    return out;
}

float Area(const Loop& p)
{
    float a = 0.0f;
    for (size_t i = 0; i < p.size(); i++)
    {
        const V2 u = p[i], v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return 0.5f * a;
}

float Perimeter(const Loop& p)
{
    float l = 0.0f;
    for (size_t i = 0; i < p.size(); i++)
    {
        const V2 d = Sub(p[(i + 1) % p.size()], p[i]);
        l += std::hypot(d.x, d.y);
    }
    return l;
}

// --- One piece as an outline family ---------------------------------------------------------

// What a piece is to the mesher: loops at every inset, an inner family if it has a hole, and
// how far in the bevel may go.
struct Section
{
    bool  closed = true;
    bool  hole = false;
    float inradius = 0.0f;  // the most any inset may be
    // `Outer(d)`: the outline moved in by `d`; `Inner(d)`: the hole's, moved out by `d`.
    Loop base, inner;  // for the polygon forms, offset on demand
    int  form = 0;     // 0 generic convex, 1 circle, 2 regular polygon, 3 stadium, 4 rect,
                       // 5 annulus circle, 6 arc, 7 annulus polygon
    V2    c;
    float r = 0.0f, r2 = 0.0f, a0 = 0.0f, a1 = 0.0f;
    int   n = 12;
    V2    p0, p1, along, across;
    float hl = 0.0f, hw = 0.0f;
    int   caps = 4;

    Loop Outer(float d) const
    {
        switch (form)
        {
            case 1: return Circle(c, r - d, n);
            case 2: return Circle(c, r - d / std::cos(PI / (float)n), n, a0);
            case 3: return Stadium(p0, p1, r - d, caps);
            case 4: return Rect(c, along, across, hl - d, hw - d);
            case 5: return Circle(c, r - d, n);
            case 6:
            {
                Loop l;
                for (int k = 0; k <= n; k++)
                {
                    const float t = a0 + (a1 - a0) * (float)k / (float)n;
                    l.push_back({ c.x + std::cos(t) * (r - d), c.y + std::sin(t) * (r - d) });
                }
                return l;
            }
            case 7: return Circle(c, r - d / std::cos(PI / (float)n), n, a0);
            default: return OffsetConvex(base, d);
        }
    }
    Loop Inner(float d) const
    {
        switch (form)
        {
            case 5: return Circle(c, r2 + d, n);
            case 6:
            {
                Loop l;
                for (int k = 0; k <= n; k++)
                {
                    const float t = a0 + (a1 - a0) * (float)k / (float)n;
                    l.push_back({ c.x + std::cos(t) * (r2 + d), c.y + std::sin(t) * (r2 + d) });
                }
                return l;
            }
            case 7: return Circle(c, r2 + d / std::cos(PI / (float)n), n, a0);
            default: return {};
        }
    }
};

// The solid a piece makes, in the maths' orientation. False for forms that make none.
bool SectionOf(const Piece& p, int n, Section& s)
{
    const float a = p.angle * DEG2RAD;
    const V2    along{ std::cos(a), std::sin(a) }, across{ -std::sin(a), std::cos(a) };
    s.n = n;
    s.c = { p.pos.x, p.pos.y };
    switch (p.form)
    {
        case Form::Disc:
            s.form = 1;
            s.r = p.radius;
            s.inradius = p.radius;
            return p.radius > TINY;
        case Form::Polygon:
        {
            const int sides = std::max(3, p.sides);
            s.n = sides;
            s.a0 = p.angle * DEG2RAD;
            if (p.filled)
            {
                s.form = 2;
                s.r = p.radius;
                s.inradius = p.radius * std::cos(PI / (float)sides);
            }
            else
            {
                s.form = 7;
                s.hole = true;
                s.r = p.radius;
                s.r2 = std::max(0.0f, p.radius - p.width / std::cos(PI / (float)sides));
                s.inradius = 0.5f * p.width;
            }
            return p.radius > TINY;
        }
        case Form::Ring:
            s.form = 5;
            s.hole = true;
            s.r = p.radius;
            s.r2 = std::max(0.0f, p.radius - p.width);
            s.inradius = 0.5f * std::min(p.width, p.radius);
            return p.radius > TINY && p.width > TINY;
        case Form::Arc:
        {
            s.form = 6;
            s.closed = false;
            s.hole = true;
            s.r = p.radius;
            s.r2 = std::max(0.0f, p.radius - p.width);
            s.a0 = p.arcFrom * DEG2RAD;
            s.a1 = p.arcTo * DEG2RAD;
            const float span = std::fabs(p.arcTo - p.arcFrom);
            s.n = std::max(3, (int)((float)n * span / 360.0f));
            s.inradius = 0.5f * std::min(p.width, p.radius);
            return p.radius > TINY && p.width > TINY && span > 0.5f;
        }
        case Form::Capsule:
        {
            const V2 tip{ p.pos.x + along.x * p.length * 0.5f,
                          p.pos.y + along.y * p.length * 0.5f };
            const V2 tail{ p.pos.x - along.x * p.length * 0.5f,
                           p.pos.y - along.y * p.length * 0.5f };
            s.form = 3;
            s.p0 = tail;
            s.p1 = tip;
            s.r = p.width * 0.5f;
            s.caps = std::max(2, n / 4);
            s.inradius = s.r;
            return p.width > TINY;
        }
        case Form::Bar:
            s.form = 4;
            s.along = along;
            s.across = across;
            s.hl = p.length * 0.5f;
            s.hw = p.width * 0.5f;
            s.inradius = std::min(s.hl, s.hw);
            return p.length > TINY && p.width > TINY;
        case Form::Chevron:
        {
            const V2    tip{ p.pos.x + along.x * p.length * 0.5f,
                             p.pos.y + along.y * p.length * 0.5f };
            const V2    tail{ p.pos.x - along.x * p.length * 0.5f,
                              p.pos.y - along.y * p.length * 0.5f };
            const float hb = p.width * 0.5f;
            const float ht = hb * p.tip;
            const V2    l = across;
            // Counter-clockwise in the maths' orientation.
            if (p.tip > 0.0f)
                s.base = { Add(tip, Mul(l, -ht)), Add(tip, Mul(l, ht)), Add(tail, Mul(l, hb)),
                           Add(tail, Mul(l, -hb)) };
            else
                s.base = { tip, Add(tail, Mul(l, hb)), Add(tail, Mul(l, -hb)) };
            if (Area(s.base) < 0.0f)
                std::reverse(s.base.begin(), s.base.end());
            s.form = 0;
            const float area = std::fabs(Area(s.base));
            s.inradius = 2.0f * area / std::max(Perimeter(s.base), TINY);
            return p.length > TINY && p.width > TINY;
        }
        default: return false;  // Lattice is expanded before this; Band is a planet's
    }
}

// A point inside a piece, near enough to say "this one is lying on that": the test that raises
// a part over what is under it.
bool Inside(const Piece& p, V2 q)
{
    const float a = p.angle * DEG2RAD;
    const V2    d = Sub(q, { p.pos.x, p.pos.y });
    const float u = d.x * std::cos(a) + d.y * std::sin(a);
    const float v = -d.x * std::sin(a) + d.y * std::cos(a);
    const float rr = std::hypot(d.x, d.y);
    switch (p.form)
    {
        case Form::Disc: return rr <= p.radius;
        case Form::Polygon:
            if (!p.filled)
                return rr <= p.radius && rr >= p.radius - p.width * 1.5f;
            return rr <= p.radius * 0.97f;
        case Form::Ring: return rr <= p.radius && rr >= p.radius - p.width;
        case Form::Arc:
        {
            if (rr > p.radius || rr < p.radius - p.width)
                return false;
            float       t = std::atan2(d.y, d.x) * RAD2DEG;
            const float lo = std::min(p.arcFrom, p.arcTo), hi = std::max(p.arcFrom, p.arcTo);
            while (t < lo)
                t += 360.0f;
            while (t > lo + 360.0f)
                t -= 360.0f;
            return t <= hi;
        }
        case Form::Capsule:
        {
            const float cu = std::clamp(u, -p.length * 0.5f, p.length * 0.5f);
            return std::hypot(u - cu, v) <= p.width * 0.5f;
        }
        case Form::Bar:
        case Form::Lattice:
            return std::fabs(u) <= p.length * 0.5f && std::fabs(v) <= p.width * 0.5f;
        case Form::Chevron:
        {
            const float k = (u + p.length * 0.5f) / std::max(p.length, TINY);  // 0 tail, 1 tip
            if (k < 0.0f || k > 1.0f)
                return false;
            const float w = p.width * 0.5f * (1.0f - k * (1.0f - p.tip));
            return std::fabs(v) <= w;
        }
        default: return false;
    }
}

// A lattice is rails and struts: the same pieces the 2D path draws as lines, as solids.
void Expand(const Piece& p, std::vector<Piece>& out)
{
    if (p.form != Form::Lattice)
    {
        out.push_back(p);
        return;
    }
    const float a = p.angle * DEG2RAD;
    const V2    along{ std::cos(a), std::sin(a) }, across{ -std::sin(a), std::cos(a) };
    const V2    tip{ p.pos.x + along.x * p.length * 0.5f, p.pos.y + along.y * p.length * 0.5f };
    const V2    tail{ p.pos.x - along.x * p.length * 0.5f, p.pos.y - along.y * p.length * 0.5f };
    const V2    offs = Mul(across, p.width * 0.5f);
    auto        capsule = [&](V2 from, V2 to, float width)
    {
        Piece q = p;
        q.form = Form::Capsule;
        q.pos = { (from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f };
        q.angle = std::atan2(to.y - from.y, to.x - from.x) * RAD2DEG;
        q.length = std::hypot(to.x - from.x, to.y - from.y);
        q.width = width;
        out.push_back(q);
    };
    const float rail = std::max(p.width * 0.10f, TINY * 10.0f);
    const float strut = std::max(p.width * 0.08f, TINY * 10.0f);
    capsule(Add(tail, offs), Add(tip, offs), rail);
    capsule(Sub(tail, offs), Sub(tip, offs), rail);
    for (int i = 0; i < p.count; i++)
    {
        const float t0 = (float)i / (float)p.count, t1 = (float)(i + 1) / (float)p.count;
        const V2    a0 = Add(Add(tail, Mul(Sub(tip, tail), t0)), offs);
        const V2    b0 = Sub(Add(tail, Mul(Sub(tip, tail), t1)), offs);
        capsule(a0, b0, strut);
    }
}

// --- The mesh -------------------------------------------------------------------------------

struct Attr
{
    float seed, alpha, spin, blink, phase, role;
    float tint[4];
    float cosA, sinA, cx, cy;  // the piece's frame, for uv
    float radius;              // the object's, for uv in radii
};

struct Builder
{
    MeshData& m;

    unsigned short Vertex(V2 p, float h, float nx, float ny, float nz, const Attr& a, float edge)
    {
        const float dx = p.x - a.cx, dy = p.y - a.cy;
        const float u = (dx * a.cosA + dy * a.sinA) / a.radius;
        const float v = (-dx * a.sinA + dy * a.cosA) / a.radius;
        const float f[VERTEX_FLOATS] = { p.x,       p.y,       h,         nx,     ny,
                                         nz,        u,         v,         a.seed, a.alpha,
                                         a.spin,    a.blink,   a.phase,   a.role, a.tint[0],
                                         a.tint[1], a.tint[2], a.tint[3], edge };
        m.vertex.insert(m.vertex.end(), f, f + VERTEX_FLOATS);
        m.maxHeight = std::max(m.maxHeight, h);
        m.reach = std::max(m.reach, std::hypot(p.x, p.y));
        return (unsigned short)(m.VertexCount() - 1);
    }

    void Tri(unsigned short a, unsigned short b, unsigned short c)
    {
        m.index.push_back(a);
        m.index.push_back(b);
        m.index.push_back(c);
    }
};

// Outward normal of a loop's vertex, counter-clockwise loop, pointing away from the material.
V2 OutOf(const Loop& l, size_t i, bool closed)
{
    const size_t n = l.size();
    const V2     a = l[closed ? (i + n - 1) % n : (i == 0 ? 0 : i - 1)];
    const V2     c = l[closed ? (i + 1) % n : (i + 1 < n ? i + 1 : i)];
    V2           e = Sub(c, a);
    const float  len = std::max(std::hypot(e.x, e.y), TINY);
    return { e.y / len, -e.x / len };  // the right of a counter-clockwise run
}

}  // namespace

bool Meshable(const Piece& p)
{
    if (p.soft || p.surface || p.orbits || p.form == Form::Band)
        return false;
    return true;
}

float RiseOf(Role role, float sr)
{
    return Rise(role, sr);
}

int Corners(float pixelRadius)
{
    // As many as keep the chord within a third of a pixel of the true circle, as BodySides
    // does for a planet: a ring two hundred pixels across is not a polygon.
    const float r = std::max(pixelRadius, 1.0f);
    const float a = std::acos(std::max(0.0f, 1.0f - 0.3f / r));
    return std::clamp((int)std::ceil(PI / std::max(a, 0.01f)), 8, 160);
}

MeshData BuildMesh(const std::vector<Piece>& composed, float R, float pixelRadius, bool chase)
{
    MeshData m;
    Builder  b{ m };
    // Bevel steps: one for a speck, three for something that fills the screen.
    const int steps = pixelRadius < 12.0f ? 1 : pixelRadius < 80.0f ? 2 : 3;

    std::vector<Piece> pieces;
    pieces.reserve(composed.size());
    for (const Piece& p : composed)
        if (Meshable(p))
            Expand(p, pieces);

    // How high each stands: on the level its z says, or on whatever earlier pieces under its
    // centre have made, whichever is higher, and then its own thickness. `foot` is where its
    // wall starts: the top of what it stands on, or the ground.
    std::vector<float> top(pieces.size(), 0.0f), edge(pieces.size(), 0.0f), rise(pieces.size()),
        foot(pieces.size(), 0.0f);
    for (size_t j = 0; j < pieces.size(); j++)
    {
        const Piece& p = pieces[j];
        rise[j] = p.rise > 0.0f ? p.rise : Rise(p.role, ShadeRadius(p) / R) * R;
        float under = 0.0f;
        for (size_t i = 0; i < j; i++)
            if (Inside(pieces[i], { p.pos.x, p.pos.y }))
                under = std::max(under, top[i]);
        foot[j] = under;
        edge[j] = std::max(Level(p.z) * R, under);
        top[j] = edge[j] + rise[j];
        if (p.lift > 0.0f)
        {
            // A slab over empty ground: its wall hangs from `edge` down to the underside.
            rise[j] = p.rise > 0.0f ? p.rise : 0.02f * R;
            foot[j] = p.lift;
            edge[j] = p.lift + rise[j];
            top[j] = edge[j] + LIFT_CHAMFER * R;
        }
    }

    for (size_t j = 0; j < pieces.size(); j++)
    {
        const Piece& p = pieces[j];
        Section      s;
        const float  reachPx =
            (std::max(p.radius, std::max(p.length, p.width)) + 0.0f) * pixelRadius / R;
        if (!SectionOf(p, Corners(reachPx), s))
            continue;
        if (m.VertexCount() > 60000)
            break;  // an object this dense is a smudge at any size a 16-bit index can hold

        const bool bigDisc = p.form == Form::Disc && p.radius > 0.3f * R;  // a floor, not a dome
        // A part the data gave a height is a prism with a small chamfer, a pylon or a mast; the
        // rule's own parts are domes and plates.
        const bool lifted = p.lift > 0.0f;
        const bool given = p.rise > 0.0f || lifted;
        const bool round =
            !given && ((p.form == Form::Disc && !bigDisc) || p.form == Form::Capsule ||
                       p.form == Form::Ring || p.form == Form::Arc || p.role == Role::Light ||
                       p.role == Role::Antenna);
        // The bevel's width: a round form is round all the way in, a plate has a chamfer that
        // is steep, so a wall reads as one when the object is tilted.
        float width = round ? s.inradius * 0.999f
                            : std::min(s.inradius * 0.5f, given ? 0.012f * R : rise[j] * 0.45f);
        width = std::max(width, TINY);
        // A dome is not taller than it is wide: it would be a spike.
        float h = lifted ? LIFT_CHAMFER * R : rise[j];
        if (round && p.form == Form::Disc)
            h = std::min(std::max(h, s.inradius * 0.5f), 0.12f * R);
        if (!given)
            h = std::min(h, 1.2f * s.inradius + 0.004f * R);
        const int K = round ? steps + 1 : std::max(1, steps - 1);

        Attr at{};
        at.seed = p.seed;
        at.alpha = p.alpha;
        at.spin = p.spin;
        at.blink = p.blink;
        // A row of lamps along the object's axis blinks as a sequence, so the light runs
        // toward where the object leads.
        at.phase =
            chase ? std::fmod(std::fmod(-p.pos.x / R * 1.7f, 1.0f) + 1.0f, 1.0f) : p.blinkPhase;
        at.role = (float)(int)p.role + (p.onlyDark ? ROLE_NIGHT_ONLY : 0.0f);
        at.tint[0] = (float)p.tint.r / 255.0f;
        at.tint[1] = (float)p.tint.g / 255.0f;
        at.tint[2] = (float)p.tint.b / 255.0f;
        at.tint[3] = (float)p.tint.a / 255.0f;
        const float ang = p.angle * DEG2RAD;
        at.cosA = std::cos(ang);
        at.sinA = std::sin(ang);
        at.cx = p.pos.x;
        at.cy = p.pos.y;
        at.radius = R;
        if (p.spin != 0.0f || p.blink > 0.0f)
            m.animated = true;

        // Profile: height and slope at step k of K, k = 0 on the outline.
        auto f = [&](int k)
        {
            const float t = (float)k / (float)K;
            return round ? std::sin(t * 0.5f * PI) : t;  // a plate's chamfer is a slope
        };
        auto g = [&](int k)  // dh/dd
        {
            const float t = (float)k / (float)K;
            return round ? h * 0.5f * PI * std::cos(t * 0.5f * PI) / width : h / width;
        };
        // A chamfer is a ramp that ends in the flat top, which is its own ring.
        const int rings = K + 1;

        struct Ring
        {
            Loop            p;
            std::vector<V2> out;  // away from the material
            float           h, slope, edge;
        };
        std::vector<Ring> outer((size_t)rings), inner;
        if (s.hole)
            inner.resize((size_t)rings);
        for (int k = 0; k < rings; k++)
        {
            const float d = width * (float)k / (float)K;
            Ring&       r = outer[(size_t)k];
            r.p = s.Outer(d);
            r.h = edge[j] + h * f(k);
            r.slope = (k == K) ? 0.0f : g(k);
            r.edge = d;
            r.out.resize(r.p.size());
            for (size_t i = 0; i < r.p.size(); i++)
                r.out[i] = OutOf(r.p, i, s.closed);
            if (s.hole)
            {
                Ring& q = inner[(size_t)k];
                q.p = s.Inner(d);
                q.h = r.h;
                q.slope = r.slope;
                q.edge = d;
                q.out.resize(q.p.size());
                for (size_t i = 0; i < q.p.size(); i++)
                {
                    const V2 o = OutOf(q.p, i, s.closed);
                    q.out[i] = { -o.x, -o.y };  // the hole's own wall faces the hole
                }
            }
        }
        // Where the rim of an arc is open, its ends are walls; nothing to add from above.

        auto emit = [&](const Ring& r)
        {
            std::vector<unsigned short> ids(r.p.size());
            for (size_t i = 0; i < r.p.size(); i++)
            {
                V2          o = r.out[i];
                float       nx = o.x * r.slope, ny = o.y * r.slope;
                const float len = std::sqrt(nx * nx + ny * ny + 1.0f);
                ids[i] = b.Vertex(r.p[i], r.h, nx / len, ny / len, 1.0f / len, at, r.edge);
            }
            return ids;
        };
        auto strip = [&](const std::vector<unsigned short>& a, const std::vector<unsigned short>& c,
                         bool closed)
        {
            const size_t n = std::min(a.size(), c.size());
            const size_t last = closed ? n : n - 1;
            for (size_t i = 0; i < last; i++)
            {
                const size_t k = (i + 1) % n;
                b.Tri(a[i], a[k], c[i]);
                b.Tri(a[k], c[k], c[i]);
            }
        };

        std::vector<std::vector<unsigned short>> oi, ii;
        for (int k = 0; k < rings; k++)
            oi.push_back(emit(outer[(size_t)k]));
        for (int k = 0; s.hole && k < rings; k++)
            ii.push_back(emit(inner[(size_t)k]));
        for (int k = 0; k + 1 < rings; k++)
            strip(oi[(size_t)k], oi[(size_t)k + 1], s.closed);
        if (s.hole)
        {
            for (int k = 0; k + 1 < rings; k++)
                strip(ii[(size_t)k], ii[(size_t)k + 1], s.closed);
            // The crest between the two walls, flat.
            strip(oi.back(), ii.back(), s.closed);
        }
        else
        {
            // The flat top: a fan from the middle of the last ring.
            const Ring& last = outer.back();
            V2          mid{ 0.0f, 0.0f };
            for (const V2& q : last.p)
                mid = Add(mid, q);
            mid = Mul(mid, 1.0f / (float)last.p.size());
            const unsigned short c = b.Vertex(mid, last.h, 0.0f, 0.0f, 1.0f, at, s.inradius);
            const std::vector<unsigned short>& ids = oi.back();
            for (size_t i = 0; i < ids.size(); i++)
                b.Tri(ids[i], ids[(i + 1) % ids.size()], c);
        }

        // The walls: from the outline down to what the piece stands on. Straight down from
        // above they are edge-on and nothing; tilted, they are the object's sides.
        const float wallTop = edge[j], wallFoot = foot[j];
        if (wallTop - wallFoot > 1e-4f * R)
        {
            auto wall = [&](const Ring& r)
            {
                const size_t                n = r.p.size();
                std::vector<unsigned short> up(n), down(n);
                for (size_t i = 0; i < n; i++)
                {
                    const V2 o = r.out[i];
                    up[i] = b.Vertex(r.p[i], wallTop, o.x, o.y, 0.0f, at, WALL_EDGE);
                    down[i] = b.Vertex(r.p[i], wallFoot, o.x, o.y, 0.0f, at, WALL_EDGE);
                }
                const size_t last = s.closed ? n : n - 1;
                for (size_t i = 0; i < last; i++)
                {
                    const size_t k = (i + 1) % n;
                    b.Tri(up[i], up[k], down[i]);
                    b.Tri(up[k], down[k], down[i]);
                }
            };
            wall(outer[0]);
            if (s.hole)
            {
                wall(inner[0]);
                if (!s.closed)  // the ends of an arc
                    for (const size_t i : { (size_t)0, outer[0].p.size() - 1 })
                    {
                        const V2 a = outer[0].p[i], c = inner[0].p[i];
                        // Along the arc and away from its body.
                        const V2             tang = Sub(outer[0].p[i == 0 ? 1 : i - 1], a);
                        const float          tl = std::max(std::hypot(tang.x, tang.y), TINY);
                        const V2             nn{ -tang.x / tl, -tang.y / tl };
                        const unsigned short a0 =
                            b.Vertex(a, wallTop, nn.x, nn.y, 0.0f, at, WALL_EDGE);
                        const unsigned short a1 =
                            b.Vertex(a, wallFoot, nn.x, nn.y, 0.0f, at, WALL_EDGE);
                        const unsigned short c0 =
                            b.Vertex(c, wallTop, nn.x, nn.y, 0.0f, at, WALL_EDGE);
                        const unsigned short c1 =
                            b.Vertex(c, wallFoot, nn.x, nn.y, 0.0f, at, WALL_EDGE);
                        b.Tri(a0, c0, a1);
                        b.Tri(c0, c1, a1);
                    }
            }
        }
        m.pieces++;
    }
    return m;
}

}  // namespace Built3D
}  // namespace Render
