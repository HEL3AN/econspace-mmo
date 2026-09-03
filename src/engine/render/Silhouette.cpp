#include "render/Silhouette.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

using nlohmann::json;

namespace Render
{
namespace
{
struct NamedForm
{
    Form        form;
    const char* name;
};

const NamedForm FORMS[] = {
    { Form::Disc, "disc" },       { Form::Ring, "ring" },       { Form::Polygon, "polygon" },
    { Form::Capsule, "capsule" }, { Form::Chevron, "chevron" }, { Form::Bar, "bar" },
    { Form::Lattice, "lattice" },
};

struct NamedRole
{
    Role        role;
    const char* name;
};

const NamedRole ROLES[] = {
    { Role::Hull, "hull" },   { Role::Panel, "panel" },     { Role::Trim, "trim" },
    { Role::Light, "light" }, { Role::Antenna, "antenna" },
};

// A small deterministic hash. It has to give the same answer every frame for the same
// object -- jitter that changes between frames is not variation, it is a shimmer -- and it
// has to cost nothing.
float Hash01(int seed, int salt)
{
    unsigned int h = (unsigned int)seed * 374761393u + (unsigned int)salt * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h % 10000u) / 10000.0f;
}

// -1..1
float Signed(int seed, int salt)
{
    return Hash01(seed, salt) * 2.0f - 1.0f;
}
}  // namespace

const char* FormName(Form f)
{
    for (const NamedForm& n : FORMS)
        if (n.form == f)
            return n.name;
    return "disc";
}

bool FormFromName(const std::string& s, Form& out)
{
    for (const NamedForm& n : FORMS)
        if (s == n.name)
        {
            out = n.form;
            return true;
        }
    return false;
}

const char* RoleName(Role r)
{
    for (const NamedRole& n : ROLES)
        if (n.role == r)
            return n.name;
    return "hull";
}

bool RoleFromName(const std::string& s, Role& out)
{
    for (const NamedRole& n : ROLES)
        if (s == n.name)
        {
            out = n.role;
            return true;
        }
    return false;
}

float ShadeRadius(const Piece& p)
{
    switch (p.form)
    {
        case Form::Disc:
        case Form::Ring:
        case Form::Polygon: return p.radius;
        case Form::Capsule:
        case Form::Chevron:
        case Form::Bar:
        case Form::Lattice: return p.width * 0.5f;
    }
    return p.radius;
}

Vector2 Axis(const Piece& p)
{
    switch (p.form)
    {
        case Form::Disc:
        case Form::Ring:
        case Form::Polygon: return { 0.0f, 0.0f };
        case Form::Capsule:
        case Form::Chevron:
        case Form::Bar:
        case Form::Lattice:
        {
            const float a = p.angle * DEG2RAD;
            return { std::cos(a), std::sin(a) };
        }
    }
    return { 0.0f, 0.0f };
}

bool ParseShape(const json& j, Shape& out, std::string& error)
{
    error.clear();
    if (!j.is_array())
    {
        error = "a shape is an array of parts";
        return false;
    }

    Shape s;
    for (const json& e : j)
    {
        if (!e.is_object())
        {
            error = "a part is an object";
            return false;
        }
        Part p;
        if (!FormFromName(e.value("form", std::string("disc")), p.form))
        {
            error = "unknown form '" + e.value("form", std::string()) + "'";
            return false;
        }
        if (!RoleFromName(e.value("role", std::string("hull")), p.role))
        {
            error = "unknown role '" + e.value("role", std::string()) + "'";
            return false;
        }

        if (e.contains("at") && e["at"].is_array() && e["at"].size() == 2)
            p.at = { e["at"][0].get<float>(), e["at"][1].get<float>() };

        p.sides = e.value("sides", p.sides);
        p.angle = e.value("angle", p.angle);
        p.radius = e.value("radius", p.radius);
        p.width = e.value("width", p.width);
        p.length = e.value("length", p.length);
        p.count = e.value("count", p.count);
        p.filled = e.value("filled", p.filled);
        p.repeat = e.value("repeat", p.repeat);
        p.mirror = e.value("mirror", p.mirror);
        p.minPixels = e.value("minPixels", p.minPixels);
        p.jitterAngle = e.value("jitterAngle", p.jitterAngle);
        p.jitterScale = e.value("jitterScale", p.jitterScale);
        p.alpha = e.value("alpha", p.alpha);
        p.orbitRadius = e.value("orbitRadius", p.orbitRadius);
        p.orbitPeriod = e.value("orbitPeriod", p.orbitPeriod);
        p.orbitPhase = e.value("orbitPhase", p.orbitPhase);
        p.orbitTilt = e.value("orbitTilt", p.orbitTilt);
        p.spin = e.value("spin", p.spin);
        p.blink = e.value("blink", p.blink);
        p.onlyThrusting = e.value("onlyThrusting", p.onlyThrusting);
        s.parts.push_back(p);
    }

    out = s;
    return true;
}

float Extent(const Shape& s)
{
    float reach = 1.0f;
    for (const Part& p : s.parts)
    {
        const float from = std::sqrt(p.at.x * p.at.x + p.at.y * p.at.y);

        // Only the measurements this form actually uses. Every part carries a default for
        // all of them, so taking the largest would have an arm reaching a full radius past
        // its own end on the strength of a `radius` it never reads.
        float own = 0.0f;
        switch (p.form)
        {
            case Form::Disc:
            case Form::Ring:
            case Form::Polygon: own = p.radius; break;
            case Form::Capsule:
            case Form::Chevron:
            case Form::Bar:
            case Form::Lattice: own = std::fmax(p.length, p.width) * 0.5f; break;
        }
        reach = std::fmax(reach, (from + own) * (1.0f + p.jitterScale));
    }
    return reach;
}

std::vector<Piece> Compose(const Shape& s, const Pose& pose)
{
    std::vector<Piece> out;
    if (s.Empty() || pose.size <= 0.0f)
        return out;

    const Vector2 pos = pose.pos;
    const float   size = pose.size;
    const int     seed = pose.seed;

    // How large the object actually is on screen, which is what decides how much of it is
    // worth assembling.
    const float pixels = size * 2.0f * pose.pixelsPerUnit;

    for (size_t i = 0; i < s.parts.size(); i++)
    {
        const Part& p = s.parts[i];
        if (p.minPixels > 0.0f && pixels < p.minPixels)
            continue;
        if (p.onlyThrusting && !pose.thrusting)
            continue;

        const int repeat = p.repeat < 1 ? 1 : p.repeat;
        for (int r = 0; r < repeat; r++)
        {
            // The seed is per part *and* per repeat, so three arms are jittered
            // differently rather than all three the same way -- which would only rotate
            // the object.
            const int salt = (int)i * 977 + r * 31;

            // Where this repeat sits, plus however far the clock has turned the part.
            // Wrapped so the number stays small however long the server has been up:
            // a float that has been counting degrees for a week has no precision left.
            const float step = (360.0f / (float)repeat) * (float)r;
            const float turned = std::fmod(p.spin * pose.time, 360.0f);
            const float spin = step + turned;
            const float wobble = p.jitterAngle * Signed(seed, salt);
            const float scale = 1.0f + p.jitterScale * Signed(seed, salt + 7);

            // A light's place in its cycle. The phase comes from the part's own seed, so
            // six lamps on a ring are a sequence rather than a pulse -- lights in step read
            // as a screensaver.
            float brightness = p.alpha;
            if (p.blink > 0.0f)
            {
                const float phase = Hash01(seed, salt + 13);
                const float t = std::fmod(pose.time / p.blink + phase, 1.0f);
                brightness = p.alpha * (0.25f + 0.75f * (0.5f + 0.5f * std::cos(t * 2.0f * PI)));
            }

            // The part's offset is turned by its repeat step and then by the object's own
            // heading, so a ship's parts follow its nose.
            const float turn = (spin + wobble) * DEG2RAD + pose.heading;
            const float cs = std::cos(turn), sn = std::sin(turn);

            // Once, or twice reflected across the object's own axis.
            const int sides = p.mirror ? 2 : 1;
            for (int m = 0; m < sides; m++)
            {
                const float flip = (m == 0) ? 1.0f : -1.0f;
                const float ax = p.at.x, ay = p.at.y * flip;

                Piece piece;
                piece.form = p.form;
                piece.role = p.role;
                piece.sides = p.sides < 3 ? 3 : p.sides;
                piece.filled = p.filled;
                piece.count = p.count < 1 ? 1 : p.count;
                piece.pos = { pos.x + (ax * cs - ay * sn) * size,
                              pos.y + (ax * sn + ay * cs) * size };
                piece.angle = p.angle * flip + spin + wobble + pose.heading * RAD2DEG;
                piece.radius = p.radius * size * scale;
                piece.width = p.width * size * scale;
                piece.length = p.length * size * scale;
                piece.brightness = brightness;

                // An orbiting part ignores `at` entirely: where it is comes from where it
                // has got to in its lap, which is the point of it.
                if (p.orbitRadius > 0.0f)
                {
                    const float period = p.orbitPeriod > 0.01f ? p.orbitPeriod : 60.0f;
                    // Every repeat and every mirror is spread evenly around the lap, so
                    // three moons are three moons rather than three moons on top of one
                    // another.
                    const float spread = (float)(r * sides + m) / (float)(repeat * sides);
                    const float lap = std::fmod(
                        pose.time / period + p.orbitPhase + spread + Hash01(seed, salt + 29), 1.0f);
                    const float a = lap * 2.0f * PI;

                    // The ellipse an inclined circular orbit traces. `front` is +1 at the
                    // nearest point and -1 at the furthest, and it is both the draw order
                    // and how much nearer the part is.
                    const float across = std::cos(a);
                    const float front = std::sin(a);
                    const float ox = across * p.orbitRadius;
                    const float oy = -front * p.orbitRadius * (1.0f - p.orbitTilt);

                    const float ch = std::cos(pose.heading), sh = std::sin(pose.heading);
                    piece.pos = { pos.x + (ox * ch - oy * sh) * size,
                                  pos.y + (ox * sh + oy * ch) * size };
                    piece.angle = p.angle + pose.heading * RAD2DEG;
                    piece.depth = front;

                    // Nearer is bigger and further is dimmer. Without the first the
                    // illusion reads as a sprite sliding under a circle; without the
                    // second the far side looks as lit as the near one, which it is not.
                    const float near = 1.0f + 0.18f * front;
                    piece.radius *= near;
                    piece.width *= near;
                    piece.length *= near;
                    piece.brightness *= 0.72f + 0.28f * (0.5f + 0.5f * front);
                }

                out.push_back(piece);
            }
        }
    }
    // Back to front, so the body hides what is behind it and covers nothing in front.
    // Stable, so parts that share a depth -- everything that is simply part of the object
    // -- keep the order the shape was written in.
    std::stable_sort(out.begin(), out.end(),
                     [](const Piece& a, const Piece& b) { return a.depth < b.depth; });
    return out;
}

}  // namespace Render
